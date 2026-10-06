#!/usr/bin/env python3
"""Production Guest file-management fault acceptance over the real executor (0024).

This drives the production ELF built from apps/yanfs_terminal/ through the real
``terminal_runtime_driver`` in production mode, reusing the 0022 runtime
acceptance module rather than copying it: ``build_host``, ``run_production``,
``classify_production``, ``seeded_image`` and the display-independent image
oracle all come from ``tests/guest/run_terminal_runtime.py``. Nothing here
reimplements the Host backend, the driver format or the result classifier.

Fault surface covered, all with an independent whole-image oracle:

  * two healthy baselines, ``mv`` and ``cp``;
  * an output refusal at the boundaries the real threshold can distinguish.
    ``--tx-fault-at N`` delivers exactly N bytes and refuses the next. The
    physical first receipt byte is *not* separately injectable: the terminal
    re-checks TX readiness after the last echo byte, so ``N = start`` (the
    receipt offset) delivers the last echo byte, then its post-write readiness
    check fails and the session ends with
    0x71000009 (LINE_UNAVAILABLE) before the command runs, leaving the ORIGINAL
    image. The middle is ``start + len//2`` and the last ``start + len - 1``
    ("OK mv\\r\\n" / "OK cp\\r\\n", 7 bytes); those refuse the shell's own success
    receipt and end with 0x7100000c (SHELL_FATAL) while the committed rename or
    copy survives. Native tests/test_shell.c already pin the physical
    first/middle/last receipt bytes after commit;
  * the first IO request after the prompt: the rename metadata write and the
    copy source read. Both stop the session with ERROR IO, leave the old image
    and skip every later command;
  * the scheduled corrupt-response protocol faults. For a 5000-byte copy the
    request order after mount (responses 1-2) is R1/W3/R2/W4/W0 = responses
    3-7, and a rename's metadata write is response 3. The Host performs each
    successful write before the tag is corrupted, so the whole image is exact:
    response 3 leaves the source only, responses 4 and 5 leave one orphan target
    block, response 6 leaves two, and response 7 has already written the new
    metadata. No general IO rollback is claimed.

Every accepted run's exact tohost is checked by a small wrapper around the
shared ``classify_production`` (no classifier copy), so a healthy 1 and the
fault codes 0x71000009/0x7100000c are pinned even where the shared helper would
accept a return-code-0 branch.

Interface:
  --source ROOT   repository root (required)
  --work DIR      build and case directory (required)
  --cc CC         host C compiler (required)
  --guest ELF     the production terminal ELF (required)
  --sanitizers    build the Host driver with ASan/UBSan

A missing in-repo file, the driver, the ELF or the script itself is a hard
failure (exit 1). The host compiler is the only external dependency, so only
its absence is a skip (exit 77). A crash, timeout, sanitizer report or an
unexpected driver exit is a harness error (exit 2), never a pass.
"""

import argparse
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_terminal_runtime as base  # noqa: E402
import run_editor as editor_base  # noqa: E402  (for the production input list)

BLOCK = base.BLOCK
BLOCKS = base.BLOCKS


# ------------------------------------------------------------- image oracle

def pattern(length, seed):
    """A repeatable, non-constant byte pattern so copied bytes are identifiable."""
    return bytes((seed + i * 7 + (i >> 8)) & 0xFF for i in range(length))


def oracle(files, extra=None):
    """The whole image this file expects: the reused independent metadata/data
    seed, plus any orphan blocks the deterministic driver leaves behind."""
    image = bytearray(base.seeded_image(files))
    if extra:
        for lba, payload in extra.items():
            image[lba * BLOCK:lba * BLOCK + len(payload)] = payload
    return bytes(image)


# ------------------------------------------------------------------ helpers

def require_tohost(values, expected, owner):
    """Exact tohost check around the shared classifier. It also runs on an
    accepted return-code-0 branch, where classify_production does not compare
    the tohost itself, so a silent wrong exit cannot pass."""
    got = values["_tohost"]
    if got != expected:
        raise base.OwnerFailure("%s: tohost is 0x%08x not the expected 0x%08x"
                                % (owner, got, expected))


def check_tx(values, at, owner):
    if values["tx_fired"] != "1":
        raise base.OwnerFailure("%s: the tx fault never fired" % owner)
    if values["delivered"] != at:
        raise base.OwnerFailure("%s: delivered %d, expected %d"
                                % (owner, values["delivered"], at))
    if values["out_length"] != at:
        raise base.OwnerFailure("%s: output length %d, expected %d"
                                % (owner, values["out_length"], at))


def healthy_baseline(driver, args, work, owner, files, stdin_bytes, receipt,
                     committed, results):
    """Run one healthy ``mv`` or ``cp``, pin its image and keep its output as
    the exact prefix every tx-fault run is compared against."""
    try:
        result, status, disk, capture = base.run_production(
            driver, args, work, base.seeded_image(files), stdin_bytes,
            owner, "none", 0, 0)
        values = base.classify_production(result, status, owner, {0})
        require_tohost(values, 1, owner)
        out = capture.read_bytes()
        if receipt not in out or b"yanfs: exit\r\n" not in out:
            raise base.OwnerFailure("%s: expected output is missing" % owner)
        if disk.read_bytes() != committed:
            raise base.OwnerFailure("%s: the final image oracle differs" % owner)
        start = out.find(receipt)
        if start < 0:
            raise base.HarnessError("%s: cannot locate the receipt" % owner)
        path = work / (owner + ".bin")
        path.write_bytes(out)
        results.append((owner, None))
        return True, out, start, path
    except base.OwnerFailure as error:
        results.append((owner, str(error)))
    except base.HarnessError as error:
        results.append((owner, "HARNESS: " + str(error)))
    return False, b"", 0, None


def receipt_faults(driver, args, work, prefix, out, start, receipt, files,
                   stdin_bytes, original, committed, baseline_path, results):
    """The echo boundary and the middle/last shell receipt byte.

    ``start`` is the receipt offset in the healthy output; the byte at
    ``start - 1`` is the last echo byte, whose post-write readiness check owns
    the threshold, so that case is the LINE_UNAVAILABLE echo boundary and the
    command never runs. The middle and last bytes are the shell's own receipt
    and leave the committed change."""
    edges = (("echo-boundary", start, base.TOHOST_LINE_UNAVAILABLE, original),
             ("mid", start + len(receipt) // 2, base.TOHOST_SHELL_FATAL,
              committed),
             ("last", start + len(receipt) - 1, base.TOHOST_SHELL_FATAL,
              committed))
    for edge, at, expected_tohost, expected_image in edges:
        owner = "%s-%s" % (prefix, edge)
        try:
            result, status, disk, capture = base.run_production(
                driver, args, work, base.seeded_image(files), stdin_bytes,
                owner, "none", 0, at, baseline=baseline_path,
                expect_tohost=expected_tohost)
            values = base.classify_production(result, status, owner,
                                              {expected_tohost})
            require_tohost(values, expected_tohost, owner)
            check_tx(values, at, owner)
            if capture.read_bytes() != out[:at]:
                raise base.OwnerFailure(owner + ": the delivered prefix differs"
                                        " from the healthy baseline")
            if disk.read_bytes() != expected_image:
                raise base.OwnerFailure(owner + ": the image after the refusal"
                                        " differs from the oracle")
            results.append((owner, None))
        except base.OwnerFailure as error:
            results.append((owner, str(error)))
        except base.HarnessError as error:
            results.append((owner, "HARNESS: " + str(error)))


def protocol_case(driver, args, work, owner, corrupt_response, files, stdin_bytes,
                  expected, results):
    try:
        result, status, disk, capture = base.run_production(
            driver, args, work, base.seeded_image(files), stdin_bytes, owner,
            "protocol", corrupt_response, 0,
            expect_tohost=base.TOHOST_SHELL_FATAL)
        values = base.classify_production(result, status, owner,
                                          {base.TOHOST_SHELL_FATAL})
        require_tohost(values, base.TOHOST_SHELL_FATAL, owner)
        if values.get("protocol_fired") != "1":
            raise base.OwnerFailure(owner + ": the response tag was never corrupted")
        out = capture.read_bytes()
        if b"ERROR PROTOCOL\r\n" not in out:
            raise base.OwnerFailure(owner + ": ERROR PROTOCOL is missing")
        if b"OK " in out:
            raise base.OwnerFailure(owner + ": a command printed an OK line")
        if b"yanfs: exit\r\n" in out:
            raise base.OwnerFailure(owner + ": a later command still ran")
        if disk.read_bytes() != expected:
            raise base.OwnerFailure(owner + ": the image after the protocol fault"
                                    " differs from the oracle")
        results.append((owner, None))
    except base.OwnerFailure as error:
        results.append((owner, str(error)))
    except base.HarnessError as error:
        results.append((owner, "HARNESS: " + str(error)))


def io_case(driver, args, work, owner, files, stdin_bytes, expected, results):
    try:
        result, status, disk, capture = base.run_production(
            driver, args, work, base.seeded_image(files), stdin_bytes, owner,
            "io", 0, 0, expect_tohost=base.TOHOST_SHELL_FATAL)
        values = base.classify_production(result, status, owner,
                                          {base.TOHOST_SHELL_FATAL})
        require_tohost(values, base.TOHOST_SHELL_FATAL, owner)
        out = capture.read_bytes()
        if b"ERROR IO\r\n" not in out:
            raise base.OwnerFailure(owner + ": ERROR IO is missing")
        if b"OK " in out:
            raise base.OwnerFailure(owner + ": a command printed an OK line")
        if b"yanfs: exit\r\n" in out:
            raise base.OwnerFailure(owner + ": a later command still ran")
        if disk.read_bytes() != expected:
            raise base.OwnerFailure(owner + ": the failed operation changed the image")
        results.append((owner, None))
    except base.OwnerFailure as error:
        results.append((owner, str(error)))
    except base.HarnessError as error:
        results.append((owner, "HARNESS: " + str(error)))


# ------------------------------------------------------------------- driver

def run_all(driver, args, work, results):
    data = pattern(5000, 3)

    # Healthy mv baseline, then the echo boundary and the mid/last receipt.
    mv_files = [("note.txt", data, 1)]
    mv_stdin = b"mv note.txt diary.txt\nexit\n"
    mv_original = oracle([("note.txt", data, 1)])
    mv_committed = oracle([("diary.txt", data, 1)])
    ok, out, start, path = healthy_baseline(
        driver, args, work, "fm-baseline-mv", mv_files, mv_stdin,
        b"OK mv\r\n", mv_committed, results)
    if ok:
        receipt_faults(driver, args, work, "fm-tx-mv-receipt", out, start,
                       b"OK mv\r\n", mv_files, mv_stdin, mv_original,
                       mv_committed, path, results)

    # Healthy cp baseline, then the echo boundary and the mid/last receipt.
    cp_files = [("note.txt", data, 1)]
    cp_stdin = b"cp note.txt backup.txt\nexit\n"
    cp_original = oracle([("note.txt", data, 1)])
    cp_committed = oracle([("note.txt", data, 1), ("backup.txt", data, 3)])
    ok, out, start, path = healthy_baseline(
        driver, args, work, "fm-baseline-cp", cp_files, cp_stdin,
        b"OK cp\r\n", cp_committed, results)
    if ok:
        receipt_faults(driver, args, work, "fm-tx-cp-receipt", out, start,
                       b"OK cp\r\n", cp_files, cp_stdin, cp_original,
                       cp_committed, path, results)

    # A rename's only metadata write is response 3.
    rename_files = [("old.txt", data, 1)]
    rename_stdin = b"mv old.txt new.txt\nls\nexit\n"
    protocol_case(driver, args, work, "fm-protocol-rename-w0", 3, rename_files,
                  rename_stdin, oracle([("new.txt", data, 1)]), results)

    # A 5000-byte copy: R1/W3/R2/W4/W0 are responses 3/4/5/6/7.
    copy_files = [("src", data, 1)]
    copy_stdin = b"cp src dst\nls\nexit\n"
    protocol_case(driver, args, work, "fm-protocol-copy-r1", 3, copy_files,
                  copy_stdin, oracle([("src", data, 1)]), results)
    protocol_case(driver, args, work, "fm-protocol-copy-w3", 4, copy_files,
                  copy_stdin, oracle([("src", data, 1)], {3: data[:BLOCK]}),
                  results)
    protocol_case(driver, args, work, "fm-protocol-copy-r2", 5, copy_files,
                  copy_stdin, oracle([("src", data, 1)], {3: data[:BLOCK]}),
                  results)
    protocol_case(driver, args, work, "fm-protocol-copy-w4", 6, copy_files,
                  copy_stdin,
                  oracle([("src", data, 1)],
                         {3: data[:BLOCK], 4: data[BLOCK:]}), results)
    protocol_case(driver, args, work, "fm-protocol-copy-w0", 7, copy_files,
                  copy_stdin, oracle([("src", data, 1), ("dst", data, 3)]),
                  results)

    # The first IO request after the prompt: rename metadata, copy source read.
    io_case(driver, args, work, "fm-io-rename-metadata", rename_files,
            rename_stdin, oracle([("old.txt", data, 1)]), results)
    io_case(driver, args, work, "fm-io-copy-source-read", copy_files,
            copy_stdin, oracle([("src", data, 1)]), results)


def require_inputs(source, args):
    """Every repository input this case needs: the shared Host-driver sources
    and headers, the full production source/header list the ELF is built from
    (run_editor.PRODUCTION_INPUTS), the imported scripts and this script itself,
    so a deleted os/yanfs.c or apps/yanfs_terminal/main.c cannot pass against a
    stale ELF. ``args.guest`` is the built artifact."""
    missing = list(base.require_inputs(source, args))
    for rel in editor_base.PRODUCTION_INPUTS:
        path = source / rel
        if not path.is_file():
            missing.append(str(path))
    for rel in ("tests/guest/run_terminal_runtime.py",
                "tests/guest/run_editor.py",
                "tests/guest/run_file_management_faults.py"):
        path = source / rel
        if not path.is_file():
            missing.append(str(path))
    if not os.path.isfile(args.guest):
        missing.append("--guest %s" % args.guest)
    return missing


def main():
    parser = argparse.ArgumentParser(
        description="production Guest file-management fault acceptance")
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--work", required=True, type=Path)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--guest", required=True)
    parser.add_argument("--sanitizers", action="store_true")
    args = parser.parse_args()

    source = args.source.resolve()
    missing = require_inputs(source, args)
    if missing:
        for line in missing:
            print(":FAIL: missing required input: %s" % line, file=sys.stderr)
        return 1
    if not base.tool_available(args.cc):
        print("SKIP: no host C compiler: %s" % args.cc, file=sys.stderr)
        return 77

    args.work.mkdir(parents=True, exist_ok=True)
    try:
        driver = base.build_host(source, args.work, args.cc,
                                 sanitizers=args.sanitizers)
    except base.HarnessError as error:
        print(":HARNESS-ERROR: build: %s" % error, file=sys.stderr)
        return 2

    results = []
    try:
        run_all(driver, args, args.work, results)
    except base.HarnessError as error:
        results.append(("fm-harness", "HARNESS: " + str(error)))
    except base.OwnerFailure as error:
        results.append(("fm-harness", str(error)))

    for name, failure in results:
        if failure is None:
            print("PASS %s" % name)
        elif failure.startswith("HARNESS:"):
            print(":HARNESS-ERROR: %s: %s"
                  % (name, failure[len("HARNESS:"):].strip()))
        else:
            print(":FAIL: %s" % failure)

    passed = sum(1 for _, failure in results if failure is None)
    failed = sum(1 for _, failure in results
                 if failure is not None and not failure.startswith("HARNESS:"))
    harness = sum(1 for _, failure in results
                  if failure is not None and failure.startswith("HARNESS:"))
    print("file-management-faults: %d passed, %d failed, %d harness errors"
          % (passed, failed, harness))
    if harness:
        return 2
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
