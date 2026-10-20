#!/usr/bin/env python3
"""0036-R3-AC1: the Guest arena usage follows the image's directory capacity.

The same production terminal ELF runs against every supported capacity class
(63, 1023, 2047 and 4095 files). The driver reads `yan_arena_used_bytes` by
symbol after the Guest halts and the oracle below recomputes the sum from the
allocation order in docs/specs/0036-yanfs-multi-block-directory.md. #4 appears
exactly once (the index_eligible copy the terminal actually uses) and #8 is
absent because PR1 removed `allocate_extent`'s stack arrays (approved 实施澄清).

A header that fails probe (unknown version, D out of range, N != 64*D-1) must
stop before the first allocation, so arena_used stays 0.
"""
import argparse
import subprocess
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_terminal_runtime as runtime  # noqa: E402

BLOCK = 4096
FAILURES = []


def check(condition, message):
    if not condition:
        FAILURES.append(message)
        print(":FAIL: " + message, flush=True)


def mkfs_image(mkfs, path, files, blocks):
    if path.exists():
        path.unlink()
    proc = subprocess.run([str(mkfs), "--image", str(path), "--blocks",
                           str(blocks), "--files", str(files)],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if proc.returncode != 0:
        raise RuntimeError("yan_mkfs failed: %s"
                           % proc.stderr.decode(errors="replace"))
    return path.read_bytes()


def expected_arena(files):
    """Σ(align16 padding + item bytes) in the documented allocation order."""
    sizes = [
        (files + 1) * 64,  # directory (N+1) * 64, = D * 4096
        files * 36,        # #1 YanNoteEntry
        files * 32,        # #2 sort_name
        files * 36,        # #3 YanRelationRow
        files,             # #4 index_eligible, shared by both linear contexts
        files * 36,        # #5 YanFsInfo
        files * 32,        # #6 block names
    ]
    current = 0
    used = 0
    for size in sizes:
        padding = (-current) % 16
        current += padding + size
        used += padding + size
    return used


def run_case(driver, args, work, label, image, max_steps):
    result, status, disk, capture = runtime.run_production(
        driver, args, work, image, b"exit\n", label, "none", 0, 0,
        expect_tohost=1, max_steps=max_steps)
    values = runtime.classify_production(result, status, label, {1})
    check(values["_tohost"] == 1, "%s: tohost is not 1" % label)
    check(values.get("arena_known") == "1",
          "%s: the ELF has no yan_arena_used_bytes symbol" % label)
    return int(values.get("arena_used", "-1")), values, capture.read_bytes()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--work", required=True, type=Path)
    parser.add_argument("--mkfs", required=True, type=Path)
    parser.add_argument("--guest", required=True, type=Path)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--max-steps", type=int, default=60000000)
    args = parser.parse_args()
    if not runtime.tool_available(args.cc):
        print("SKIP host compiler")
        return 77
    args.work.mkdir(parents=True, exist_ok=True)
    try:
        driver = runtime.build_host(args.source, args.work, args.cc)
    except runtime.HarnessError as error:
        print(":HARNESS-ERROR: %s" % error)
        return 2

    class Args:
        pass
    run_args = Args()
    run_args.source = args.source
    run_args.work = args.work
    run_args.guest = args.guest

    oracle = {63: 15008, 1023: 242528, 2047: 485216, 4095: 970592}
    for files in (63, 1023, 2047, 4095):
        blocks = 65 if files == 4095 else 64
        image = mkfs_image(args.mkfs, args.work / ("ac1-%d.img" % files),
                           files, blocks)
        used, values, _ = run_case(driver, run_args, args.work,
                                   "ac1-%d" % files, image, args.max_steps)
        expected = expected_arena(files)
        print("0036-R3-AC1 N=%d arena_used=%d expected=%d oracle=%d"
              % (files, used, expected, oracle[files]), flush=True)
        check(expected == oracle[files],
              "N=%d: computed expectation %d != approved oracle %d"
              % (files, expected, oracle[files]))
        check(used == expected,
              "N=%d: arena_used %d != expected %d" % (files, used, expected))

    base = bytearray(mkfs_image(args.mkfs, args.work / "ac1-base.img", 1023, 64))
    bad_cases = {}
    bad_cases["version"] = bytearray(base)
    bad_cases["version"][8] = 3
    bad_cases["d-65"] = bytearray(base)
    bad_cases["d-65"][8] = 2
    bad_cases["d-65"][24] = 65
    bad_cases["n-mismatch"] = bytearray(base)
    bad_cases["n-mismatch"][8] = 2
    bad_cases["n-mismatch"][24] = 2
    bad_cases["n-mismatch"][20] = 64
    for label, bad_image in bad_cases.items():
        path = args.work / ("ac1-bad-%s.img" % label)
        path.write_bytes(bad_image)
        result, status, disk, capture = runtime.run_production(
            driver, run_args, args.work, bad_image, b"exit\n",
            "ac1-bad-" + label, "none", 0, 0,
            expect_tohost=runtime.APP_FAIL_TOP | 6, max_steps=200000)
        values = runtime.classify_production(
            result, status, "ac1-bad-" + label,
            {runtime.APP_FAIL_TOP | 6})
        used = int(values.get("arena_used", "-1"))
        output = capture.read_bytes()
        print("0036-R3-AC1 invalid=%s tohost=0x%08x arena_used=%d"
              % (label, values["_tohost"], used), flush=True)
        check(values["_tohost"] == runtime.APP_FAIL_TOP | 6,
              "%s: startup did not report the mount path" % label)
        check(used == 0, "%s: an allocation happened before header validation"
              % label)
        check(b"yanfs> " not in output,
              "%s: the shell prompt was reached" % label)
        check(b"ERROR " in output, "%s: no error token was printed" % label)
        check(disk.read_bytes() == bad_image,
              "%s: the rejected image changed" % label)

    for failure in FAILURES:
        print(":FAIL: " + failure, flush=True)
    print("arena-accounting: %d failures" % len(FAILURES), flush=True)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (runtime.HarnessError, OSError, RuntimeError) as error:
        print(":HARNESS-ERROR: %s" % error)
        sys.exit(2)
