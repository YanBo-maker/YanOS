#!/usr/bin/env python3
"""Production multiline-editor fault acceptance over the real Host driver (0023).

This is the editor half of the fault surface the terminal runtime suite already
drives. It builds only the Host driver (tests/terminal_runtime_driver.c plus
the real machine library and tools/host_block.c, tools/host_file.c) with a
strict C17 host compiler and -Werror; no cross compiler is involved, and the
production ELF is passed in already built (--guest).

Reused by import, not copied:

  * run_terminal_runtime.build_host / run_production / classify_production /
    parse_status / check_not_harness / check_status_exit / record / last_line
    give the Host build and the driver's status-file parsing and classification;
  * run_terminal_files.empty_image / expected_image / assert_image /
    display_self_check give the independent whole-image oracle: block 0 is
    rebuilt entry by entry and resealed here, data blocks are written from the
    bytes this file defines, and nothing is decoded by the production encoder.

Fault model actually available (and its limits, so no coverage is invented):

  * --tx-fault-at N: the UART backend delivers exactly N bytes and then refuses
    every later byte. N counts accepted bytes, so the byte at N is delivered and
    the refusal starts at N+1. The driver already asserts the delivered count,
    the output length, that the captured bytes are the healthy baseline prefix,
    that tohost never reached 1, and that no later yan_terminal_putc /
    yan_console_putc entry happened.
  * --fault io: yan_host_block.fail_next is armed once the first "yanfs> "
    prompt has been emitted and fails the next single request that touches
    storage, whatever it is (read or write). There is no separate "fail the
    second write / the metadata write" switch: the editor's whole-file save is
    one create/replace whose data write is the next request, so the metadata
    second-write path is NOT separately injectable here and is not claimed.
  * --fault protocol --corrupt-response N: flips the tag byte of the N-th
    served response. The mount consumes responses 1 (capacity) and 2 (block 0),
    so response 3 is the editor's first data-block read and response 4 the
    second. Unlike the shell's chunked cat (16 reads per block), the editor
    loads a file with one yan_fs_read whose block reads are responses 3 and 4
    for a 5000-byte file.

Application failure codes checked here by exact tohost values:

  * 0x71000013 (reason 19, EDITOR_FATAL)  an editor output byte, an editor
    load save IO, or a load protocol failure;
  * 0x71000011 (reason 17, OUTPUT)        an application prompt byte;
  * 0x71000009 (reason 9, LINE_UNAVAILABLE) a line-echo byte.

Interface:
  --source ROOT   repository root (required)
  --work DIR      build and case directory (required)
  --cc CC         host C compiler (required)
  --guest ELF     the production terminal ELF (required)
  --sanitizers    build the Host driver with ASan/UBSan (optional)

A missing in-repo source/header or guest ELF is a hard failure (exit 1).
A build/timeout/signal/sanitizer/trap/invalid-status condition is a harness
error (exit 2). The
only skip (77) is a missing host compiler, and it is checked after the in-repo
and ELF checks. A named owner failure prints ":FAIL: <owner>: ..." and makes the
script return 1. A failing baseline stops only the fault runs that depend on it.
"""

import argparse
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_terminal_files as images  # noqa: E402  (path installed just above)
import run_terminal_runtime as runtime  # noqa: E402
from run_editor import PRODUCTION_INPUTS  # noqa: E402

APP_FAIL_TOP = 0x71000000
TOHOST_LINE_ECHO = APP_FAIL_TOP | 9
TOHOST_APP_PROMPT = APP_FAIL_TOP | 17
TOHOST_EDITOR = APP_FAIL_TOP | 19

# The main baseline: a new file, append, print, save, then the shell exit.
MAIN_STDIN = b"edit note.txt\na X\np\nw\nexit\n"
MAIN_ENTRIES = [(0, "note.txt", 2, 1, 1)]
MAIN_BLOCKS = {1: b"X\n"}
MAIN_FRAGMENTS = (
    b"OK edit\r\n", b"OK a\r\n", b"1 X\r\nOK p\r\n", b"OK w\r\n",
    b"yanfs: exit\r\n",
)

# A distinct loaded-existing baseline: CRLF plus a missing final EOL, a
# replacement, an append, print, and a whole-file replace.
LOADED_SEED_ENTRIES = [(0, "note", 4, 1, 1)]
LOADED_SEED_BLOCKS = {1: b"A\r\nB"}
LOADED_STDIN = b"edit note\nr 1 C\na D\np\nw\nexit\n"
LOADED_ENTRIES = [(0, "note", 7, 2, 1)]
LOADED_BLOCKS = {1: b"A\r\nB", 2: b"C\r\nB\nD\n"}
LOADED_FRAGMENTS = (
    b"OK edit\r\n", b"OK r\r\n", b"OK a\r\n",
    b"1 C\r\n2 B\r\n3 D\r\nOK p\r\n", b"OK w\r\n", b"yanfs: exit\r\n",
)

# (name, marker, marker offset, phase length, expected tohost). The editor
# success receipts and the p display are editor output (19); the editing prompt
# is an application prompt (17); a command echo is the line reader (9).
MAIN_TX_PHASES = (
    ("ok-edit", b"OK edit\r\n", 0, len(b"OK edit\r\n"), TOHOST_EDITOR),
    ("ok-a", b"OK a\r\n", 0, len(b"OK a\r\n"), TOHOST_EDITOR),
    ("display", b"1 X\r\n", 0, len(b"1 X\r\n"), TOHOST_EDITOR),
    ("ok-p", b"OK p\r\n", 0, len(b"OK p\r\n"), TOHOST_EDITOR),
    ("ok-w", b"OK w\r\n", 0, len(b"OK w\r\n"), TOHOST_EDITOR),
    ("prompt-edit", b"edit> ", 0, len(b"edit> "), TOHOST_APP_PROMPT),
    ("echo-w", b"edit> w\r\n", len(b"edit> "), len(b"w\r\n"), TOHOST_LINE_ECHO),
)
LOADED_TX_PHASES = (
    ("ok-w", b"OK w\r\n", 0, len(b"OK w\r\n"), TOHOST_EDITOR),
    ("echo-r", b"edit> r 1 C\r\n", len(b"edit> "), len(b"r 1 C"),
     TOHOST_LINE_ECHO),
)


# --------------------------------------------------------------- helpers

def classify_editor(result, status, owner, expected):
    """Keep the shared harness rules and require the exact application verdict."""
    expected_codes = {expected} if expected != 1 else set()
    values = runtime.classify_production(result, status, owner, expected_codes)
    if values["_tohost"] != expected:
        raise runtime.OwnerFailure(
            "%s: tohost 0x%08x is not the expected 0x%08x"
            % (owner, values["_tohost"], expected))
    return values


def check_image(disk, entries, blocks, owner):
    try:
        images.assert_image(disk, entries, blocks, owner)
    except AssertionError as error:
        raise runtime.OwnerFailure(str(error)) from error


def check_tx(values, captured, baseline_bytes, at, owner):
    """The exact bytes the UART delivered before the refusal."""
    if values["tx_fired"] != "1":
        raise runtime.OwnerFailure("%s: the tx fault never fired" % owner)
    if values["delivered"] != at:
        raise runtime.OwnerFailure("%s: delivered %d, expected %d"
                                   % (owner, values["delivered"], at))
    if values["out_length"] != at:
        raise runtime.OwnerFailure("%s: out_length %d, expected %d"
                                   % (owner, values["out_length"], at))
    if captured != baseline_bytes[:at]:
        raise runtime.OwnerFailure(
            "%s: the captured output is not the healthy baseline prefix" % owner)
    if values["_tohost"] == 1:
        raise runtime.OwnerFailure("%s: a faulted run reached tohost 1" % owner)


def record_failure(results, owner, error):
    if isinstance(error, runtime.HarnessError):
        results.append((owner, "HARNESS: " + str(error)))
    else:
        results.append((owner, str(error)))


# ---------------------------------------------------------------- cases

def run_baseline(driver, args, owner, image, stdin_bytes, entries, blocks,
                 fragments):
    result, status, disk, capture = runtime.run_production(
        driver, args, args.work, image, stdin_bytes, owner, "none", 0, 0)
    classify_editor(result, status, owner, 1)
    output = capture.read_bytes()
    for text in fragments:
        if text not in output:
            raise runtime.OwnerFailure("%s: missing output %r" % (owner, text))
    check_image(disk, entries, blocks, owner)
    return output


def run_tx_faults(driver, args, results, prefix, stdin_bytes, image,
                  baseline_bytes, unchanged_entries, unchanged_blocks,
                  committed_entries, committed_blocks, phases):
    """For each phase, refuse the first, middle and last delivered byte and
    require the exact application reason, the baseline prefix, and the image
    the independent oracle expects at that point of the save."""
    baseline_path = args.work / (prefix + "-baseline.bin")
    baseline_path.write_bytes(baseline_bytes)
    ok_w_at = baseline_bytes.find(b"OK w\r\n")
    if ok_w_at < 0:
        results.append((prefix, "HARNESS: the baseline has no OK w receipt"))
        return
    for name, marker, offset, length, code in phases:
        start = baseline_bytes.find(marker)
        if start < 0:
            results.append(("%s-%s" % (prefix, name),
                            "HARNESS: cannot locate the %s phase" % name))
            continue
        start += offset
        for edge, delta in (("first", 1), ("mid", (length + 1) // 2),
                            ("last", length)):
            at = start + delta
            owner = "%s-%s-%s" % (prefix, name, edge)
            # `at` counts delivered bytes; `ok_w_at` is a zero-based offset.
            # Equality still ends on the preceding echo byte, before save.
            committed = at > ok_w_at
            try:
                result, status, disk, capture = runtime.run_production(
                    driver, args, args.work, image, stdin_bytes, owner,
                    "none", 0, at, baseline=baseline_path,
                    expect_tohost=code)
                values = classify_editor(result, status, owner, code)
                captured = capture.read_bytes()
                check_tx(values, captured, baseline_bytes, at, owner)
                # An incomplete success receipt must not carry its full OK tail.
                if marker.startswith(b"OK ") and at < start + length:
                    if marker in captured:
                        raise runtime.OwnerFailure(
                            "%s: an incomplete %r printed its full tail"
                            % (owner, marker))
                if committed:
                    check_image(disk, committed_entries, committed_blocks,
                                owner)
                else:
                    check_image(disk, unchanged_entries, unchanged_blocks,
                                owner)
                results.append((owner, None))
            except (runtime.OwnerFailure, runtime.HarnessError) as error:
                record_failure(results, owner, error)


def run_fs_faults(driver, args, results):
    # A new draft whose save is the next storage request: the create's data
    # write fails, so the editor reports IO and the application stops with
    # reason 19 before any later q/w/exit runs. Nothing was committed.
    owner = "editor-w-io"
    try:
        result, status, disk, capture = runtime.run_production(
            driver, args, args.work, images.empty_image(),
            b"edit note.txt\na X\nw\nq\nexit\n", "w-io", "io", 0, 0,
            expect_tohost=TOHOST_EDITOR)
        classify_editor(result, status, owner, TOHOST_EDITOR)
        output = capture.read_bytes()
        if b"ERROR IO\r\n" not in output:
            raise runtime.OwnerFailure("%s: ERROR IO is missing" % owner)
        for later in (b"OK w\r\n", b"OK q\r\n", b"yanfs: exit\r\n"):
            if later in output:
                raise runtime.OwnerFailure("%s: a later command still ran"
                                           % owner)
        check_image(disk, [], {}, owner)
        results.append((owner, None))
    except (runtime.OwnerFailure, runtime.HarnessError) as error:
        record_failure(results, owner, error)

    # Loading an existing file whose first data-block read fails: no OK edit,
    # no editing prompt, and the image is untouched.
    owner = "editor-load-io"
    seed = images.expected_image(LOADED_SEED_ENTRIES, LOADED_SEED_BLOCKS)
    try:
        result, status, disk, capture = runtime.run_production(
            driver, args, args.work, seed, b"edit note\nexit\n", "load-io",
            "io", 0, 0, expect_tohost=TOHOST_EDITOR)
        classify_editor(result, status, owner, TOHOST_EDITOR)
        output = capture.read_bytes()
        if b"ERROR IO\r\n" not in output:
            raise runtime.OwnerFailure("%s: ERROR IO is missing" % owner)
        for forbidden in (b"OK edit\r\n", b"edit> ", b"yanfs: exit\r\n"):
            if forbidden in output:
                raise runtime.OwnerFailure("%s: %r appeared after the failed load"
                                           % (owner, forbidden))
        check_image(disk, LOADED_SEED_ENTRIES, LOADED_SEED_BLOCKS, owner)
        results.append((owner, None))
    except (runtime.OwnerFailure, runtime.HarnessError) as error:
        record_failure(results, owner, error)

    # A corrupted response tag on the editor's first and second data-block read
    # of a 5000-byte file. The mount consumes responses 1 and 2, so response 3
    # is the first read and response 4 the second; the editor's one yan_fs_read
    # reads each block once, unlike the shell's chunked cat.
    big = bytes((index % 95) + 32 for index in range(5000))
    seed = images.expected_image([(0, "big", 5000, 1, 2)], {1: big})
    for owner, response in (("editor-protocol-first", 3),
                            ("editor-protocol-second", 4)):
        try:
            result, status, disk, capture = runtime.run_production(
                driver, args, args.work, seed, b"edit big\nexit\n",
                "protocol-%d" % response, "protocol", response, 0,
                expect_tohost=TOHOST_EDITOR)
            values = classify_editor(result, status, owner, TOHOST_EDITOR)
            if values["protocol_fired"] != "1":
                raise runtime.OwnerFailure("%s: the response tag was never"
                                           " corrupted" % owner)
            output = capture.read_bytes()
            if b"ERROR PROTOCOL\r\n" not in output:
                raise runtime.OwnerFailure("%s: ERROR PROTOCOL is missing"
                                           % owner)
            for forbidden in (b"OK edit\r\n", b"edit> ", b"yanfs: exit\r\n"):
                if forbidden in output:
                    raise runtime.OwnerFailure(
                        "%s: %r appeared after the protocol failure"
                        % (owner, forbidden))
            check_image(disk, [(0, "big", 5000, 1, 2)], {1: big}, owner)
            results.append((owner, None))
        except (runtime.OwnerFailure, runtime.HarnessError) as error:
            record_failure(results, owner, error)


# ---------------------------------------------------------------- driver

def require_inputs(source, args):
    missing = []
    seen = set()
    for rel in (runtime.HOST_SOURCES + runtime.HOST_HEADERS
                + runtime.GUEST_HEADERS + list(PRODUCTION_INPUTS)
                + ["tests/guest/run_editor.py",
                   "tests/guest/run_terminal_runtime.py"]):
        if rel in seen:
            continue
        seen.add(rel)
        if not (source / rel).is_file():
            missing.append(str(source / rel))
    if not Path(args.guest).is_file():
        missing.append("guest ELF: %s" % args.guest)
    return missing


def main():
    parser = argparse.ArgumentParser(
        description="production multiline-editor fault acceptance over the "
                    "real Host driver")
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--work", required=True, type=Path)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--guest", required=True)
    parser.add_argument("--sanitizers", action="store_true",
                        help="build the Host driver with ASan/UBSan")
    args = parser.parse_args()

    source = args.source.resolve()
    missing = require_inputs(source, args)
    if missing:
        for line in missing:
            print(":FAIL: missing required input: %s" % line, flush=True)
        return 1
    # The only external dependency is the host compiler, so it alone can skip.
    if not runtime.tool_available(args.cc):
        print("SKIP: no host C compiler (%s)" % args.cc, flush=True)
        return 77

    args.work.mkdir(parents=True, exist_ok=True)
    try:
        driver = runtime.build_host(source, args.work, args.cc,
                                    sanitizers=args.sanitizers)
    except runtime.HarnessError as error:
        print(":HARNESS-ERROR: build: %s" % error, flush=True)
        return 2

    try:
        images.display_self_check()
    except AssertionError as error:
        print(":HARNESS-ERROR: display oracle: %s" % error, flush=True)
        return 2

    results = []
    main_ok = True
    loaded_ok = True
    try:
        main_output = run_baseline(driver, args, "editor-baseline",
                                   images.empty_image(), MAIN_STDIN,
                                   MAIN_ENTRIES, MAIN_BLOCKS, MAIN_FRAGMENTS)
        results.append(("editor-baseline", None))
    except (runtime.OwnerFailure, runtime.HarnessError) as error:
        record_failure(results, "editor-baseline", error)
        main_ok = False
        main_output = b""

    try:
        loaded_output = run_baseline(
            driver, args, "editor-loaded-baseline",
            images.expected_image(LOADED_SEED_ENTRIES, LOADED_SEED_BLOCKS),
            LOADED_STDIN, LOADED_ENTRIES, LOADED_BLOCKS, LOADED_FRAGMENTS)
        results.append(("editor-loaded-baseline", None))
    except (runtime.OwnerFailure, runtime.HarnessError) as error:
        record_failure(results, "editor-loaded-baseline", error)
        loaded_ok = False
        loaded_output = b""

    if main_ok:
        run_tx_faults(driver, args, results, "editor-tx", MAIN_STDIN,
                      images.empty_image(), main_output, [], {},
                      MAIN_ENTRIES, MAIN_BLOCKS, MAIN_TX_PHASES)
    if loaded_ok:
        run_tx_faults(driver, args, results, "editor-loaded-tx", LOADED_STDIN,
                      images.expected_image(LOADED_SEED_ENTRIES,
                                            LOADED_SEED_BLOCKS),
                      loaded_output, LOADED_SEED_ENTRIES, LOADED_SEED_BLOCKS,
                      LOADED_ENTRIES, LOADED_BLOCKS, LOADED_TX_PHASES)
    run_fs_faults(driver, args, results)

    for name, failure in results:
        if failure is None:
            print("PASS %s" % name, flush=True)
        elif failure.startswith("HARNESS:"):
            print(":HARNESS-ERROR: %s: %s"
                  % (name, failure[len("HARNESS:"):].strip()), flush=True)
        else:
            print(":FAIL: %s" % failure, flush=True)

    passed = sum(1 for _, failure in results if failure is None)
    failed = sum(1 for _, failure in results
                 if failure is not None and not failure.startswith("HARNESS:"))
    harness = sum(1 for _, failure in results
                  if failure is not None and failure.startswith("HARNESS:"))
    print("editor-faults: %d passed, %d failed, %d harness errors"
          " (%d cases including baselines; baseline %s)"
          % (passed, failed, harness, len(results),
             "ok" if (main_ok and loaded_ok) else "failed"), flush=True)
    if harness:
        return 2
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
