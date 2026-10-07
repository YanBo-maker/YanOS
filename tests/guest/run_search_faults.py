#!/usr/bin/env python3
"""Real production fault acceptance for the 0025 `grep` front end.

This reuses the Host driver, the production runner and the verdict classifier
from run_terminal_runtime.py; it does not clone them. The image is built by the
same whole-image oracle, and the only external dependency is the host C
compiler (exit 77 when it is absent). A missing in-repo source, header, shared
script or the provided real ELF is a hard failure (exit 1).

The fixture is one 5000-byte logical line over two blocks: ``x`` everywhere,
the literal ``needle`` near the start, and the three UTF-8 bytes of U+4E2D at
offsets 4095/4096/4097. The block-request order is mount 1-2, NUL preflight
3-4, match scan 5-6, matched-range reader 7-8, so a corrupted response tag at
3..8 lands in exactly one search phase.
"""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_terminal_runtime as runtime  # noqa: E402  (path installed just above)
import run_editor as editor  # noqa: E402  (path installed just above)

TOHOST_SHELL_FATAL = runtime.TOHOST_SHELL_FATAL
TOHOST_LINE_UNAVAILABLE = runtime.TOHOST_LINE_UNAVAILABLE
TOHOST_OUTPUT = runtime.TOHOST_OUTPUT
TIMEOUT = runtime.TIMEOUT
display = runtime.display

COMMAND = b"grep needle\nexit\n"

# Every repository input this script reads, including the shared scripts and the
# Host driver sources it compiles.
PRODUCTION_INPUTS = tuple(editor.PRODUCTION_INPUTS) + (
    "tests/guest/run_search_faults.py",
    "tests/guest/run_terminal_runtime.py",
    "tests/guest/run_editor.py",
) + tuple(runtime.HOST_SOURCES) + tuple(runtime.HOST_HEADERS)


class HarnessError(RuntimeError):
    """A condition that is not a clean pass/fail of a Guest assertion."""


def search_body():
    """The 5000-byte single line shared by every case."""
    data = bytearray(b"x" * 5000)
    data[2:8] = b"needle"
    data[4095:4098] = "\u4e2d".encode("utf-8")
    return bytes(data)


def image_for(body):
    return runtime.seeded_image([("f", body, 1)])


def run(driver, args, image, label, fault, corrupt_response, tx_fault_at,
        baseline=None, expected_tohost=None):
    return runtime.run_production(
        driver, args, args.work, image, COMMAND, label, fault,
        corrupt_response, tx_fault_at, baseline=baseline,
        expect_tohost=expected_tohost)


def classify(result, status, owner, expected):
    values = runtime.classify_production(result, status, owner, {expected})
    if values["_tohost"] != expected:
        raise runtime.OwnerFailure(
            "%s: tohost 0x%08x, expected 0x%08x"
            % (owner, values["_tohost"], expected))
    return values


# ------------------------------------------------------------------- cases

def case_healthy_baseline(driver, args, results):
    body = search_body()
    image = image_for(body)
    result, status, disk, capture = run(driver, args, image, "healthy", "none",
                                        0, 0)
    owner = "search-healthy"
    try:
        values = classify(result, status, owner, 1)
        if values["_tohost"] != 1:
            raise runtime.OwnerFailure("%s: tohost is not 1" % owner)
        output = capture.read_bytes()
        expected = b"f:1:" + display(body) + b"\r\nOK grep\r\n"
        if expected not in output:
            raise runtime.OwnerFailure(
                "%s: the healthy record differs from the oracle" % owner)
        if b"ERROR " in output:
            raise runtime.OwnerFailure("%s: an error appeared in a healthy run"
                                       % owner)
        if disk.read_bytes() != image:
            raise runtime.OwnerFailure("%s: the image changed" % owner)
        results.append((owner, None))
        return output
    except runtime.OwnerFailure as error:
        results.append((owner, str(error)))
        return None
    except runtime.HarnessError as error:
        results.append((owner, "HARNESS: " + str(error)))
        return None


def case_protocol_tags(driver, args, results):
    body = search_body()
    image = image_for(body)
    for response in range(3, 9):
        owner = "search-protocol-%d" % response
        result, status, disk, capture = run(
            driver, args, image, "protocol-%d" % response, "protocol",
            response, 0, expected_tohost=TOHOST_SHELL_FATAL)
        try:
            values = classify(result, status, owner, TOHOST_SHELL_FATAL)
            if values.get("protocol_fired") != "1":
                raise runtime.OwnerFailure(
                    "%s: the response tag was never corrupted" % owner)
            output = capture.read_bytes()
            if b"OK grep\r\n" in output:
                raise runtime.OwnerFailure("%s: a failed grep printed OK" % owner)
            if b"yanfs: exit\r\n" in output:
                raise runtime.OwnerFailure("%s: the later exit still ran" % owner)
            if b"ERROR PROTOCOL\r\n" not in output:
                raise runtime.OwnerFailure("%s: ERROR PROTOCOL is missing" % owner)
            if response == 7:
                if not output.endswith(b"f:1:\r\nERROR PROTOCOL\r\nERROR SHELL_FATAL\r\n"):
                    raise runtime.OwnerFailure(
                        "%s: the resp7 record prefix differs: %r" % (owner, output))
            elif response == 8:
                prefix = (b"f:1:" + display(body[:4095]) +
                          b"\\xE4\r\nERROR PROTOCOL\r\nERROR SHELL_FATAL\r\n")
                if not output.endswith(prefix):
                    raise runtime.OwnerFailure(
                        "%s: the resp8 record prefix differs: %r" % (owner, output))
            elif b"f:1:" in output:
                raise runtime.OwnerFailure(
                    "%s: a record prefix appeared before the whole line was"
                    " scanned" % owner)
            if disk.read_bytes() != image:
                raise runtime.OwnerFailure("%s: the image changed" % owner)
            results.append((owner, None))
        except runtime.OwnerFailure as error:
            results.append((owner, str(error)))
        except runtime.HarnessError as error:
            results.append((owner, "HARNESS: " + str(error)))


def case_first_io(driver, args, results):
    """The first actual block IO after the prompt is the NUL preflight; an IO
    fault there must end the session with a source IO error and no record."""
    body = search_body()
    image = image_for(body)
    owner = "search-io-preflight"
    result, status, disk, capture = run(
        driver, args, image, "io", "io", 0, 0,
        expected_tohost=TOHOST_SHELL_FATAL)
    try:
        classify(result, status, owner, TOHOST_SHELL_FATAL)
        output = capture.read_bytes()
        if b"ERROR IO\r\n" not in output:
            raise runtime.OwnerFailure("%s: ERROR IO is missing" % owner)
        if b"OK grep\r\n" in output or b"f:1:" in output:
            raise runtime.OwnerFailure(
                "%s: a record or success appeared after a failed preflight" % owner)
        if b"yanfs: exit\r\n" in output:
            raise runtime.OwnerFailure("%s: the later exit still ran" % owner)
        if disk.read_bytes() != image:
            raise runtime.OwnerFailure("%s: the image changed" % owner)
        results.append((owner, None))
    except runtime.OwnerFailure as error:
        results.append((owner, str(error)))
    except runtime.HarnessError as error:
        results.append((owner, "HARNESS: " + str(error)))


def case_output_thresholds(driver, args, results, baseline_output):
    """Output refusal at exact byte positions of the healthy run: the echo
    start is the line reader's LINE_UNAVAILABLE before the query; the middle
    and last content bytes and the final success byte are SHELL_FATAL."""
    if baseline_output is None:
        results.append(("search-tx", "HARNESS: no healthy baseline"))
        return
    body = search_body()
    image = image_for(body)
    prompt_at = baseline_output.find(b"yanfs> ")
    if prompt_at < 0:
        results.append(("search-tx", "HARNESS: no prompt in the baseline"))
        return
    echo_at = prompt_at + len(b"yanfs> ")
    echo_end = baseline_output.find(b"\r\n", echo_at)
    if echo_end < 0:
        results.append(("search-tx", "HARNESS: no echo CRLF in the baseline"))
        return
    record_at = echo_end + 2
    ok_at = baseline_output.find(b"OK grep\r\n", record_at)
    if ok_at < 0:
        results.append(("search-tx", "HARNESS: no OK in the baseline"))
        return
    content_start = record_at + len(b"f:1:")
    content_end = ok_at - 2
    thresholds = [
        ("echo-first", echo_at + 1, TOHOST_LINE_UNAVAILABLE),
        ("echo-last", record_at, TOHOST_LINE_UNAVAILABLE),
        ("content-mid", content_start + (content_end - content_start) // 2,
         TOHOST_SHELL_FATAL),
        ("content-last", content_end, TOHOST_SHELL_FATAL),
        ("ok-last", ok_at + len(b"OK grep\r\n"), TOHOST_SHELL_FATAL),
    ]
    baseline_path = args.work / "search-healthy.bin"
    baseline_path.write_bytes(baseline_output)
    for label, at, expected in thresholds:
        owner = "search-tx-" + label
        result, status, disk, capture = run(
            driver, args, image, "tx-" + label, "none", 0, at,
            baseline=baseline_path, expected_tohost=expected)
        try:
            values = classify(result, status, owner, expected)
            if values["tx_fired"] != "1":
                raise runtime.OwnerFailure("%s: the tx fault never fired" % owner)
            if values["delivered"] != at or values["out_length"] != at:
                raise runtime.OwnerFailure(
                    "%s: delivered=%s out_length=%s, expected %d"
                    % (owner, values["delivered"], values["out_length"], at))
            if capture.read_bytes() != baseline_output[:at]:
                raise runtime.OwnerFailure(
                    "%s: the delivered prefix differs from the healthy run" % owner)
            if disk.read_bytes() != image:
                raise runtime.OwnerFailure("%s: the image changed" % owner)
            results.append((owner, None))
        except runtime.OwnerFailure as error:
            results.append((owner, str(error)))
        except runtime.HarnessError as error:
            results.append((owner, "HARNESS: " + str(error)))


def require_inputs(source):
    missing = []
    for rel in PRODUCTION_INPUTS:
        if not (source / rel).is_file():
            missing.append(str(source / rel))
    return missing


def main():
    parser = argparse.ArgumentParser(
        description="real production fault acceptance for 0025 grep")
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--work", required=True, type=Path)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--guest", required=True, type=Path)
    parser.add_argument("--sanitizers", action="store_true")
    args = parser.parse_args()

    source = args.source.resolve()
    missing = require_inputs(source)
    if missing:
        for line in missing:
            print(":FAIL: missing required input: %s" % line, flush=True)
        return 1
    if not args.guest.is_file():
        print(":FAIL: missing required input: --guest %s" % args.guest,
              flush=True)
        return 1
    if not runtime.tool_available(args.cc):
        print("SKIP: no host compiler (%s)" % args.cc, flush=True)
        return 77

    args.work.mkdir(parents=True, exist_ok=True)
    try:
        driver = runtime.build_host(source, args.work, args.cc,
                                    sanitizers=args.sanitizers)
    except runtime.HarnessError as error:
        print(":HARNESS-ERROR: build: %s" % error, flush=True)
        return 2
    try:
        runtime.display_self_check()
    except AssertionError as error:
        print(":HARNESS-ERROR: display oracle: %s" % error, flush=True)
        return 2

    results = []
    try:
        baseline_output = case_healthy_baseline(driver, args, results)
        case_protocol_tags(driver, args, results)
        case_first_io(driver, args, results)
        case_output_thresholds(driver, args, results, baseline_output)
    except (runtime.HarnessError, OSError) as error:
        results.append(("search-harness", "HARNESS: " + str(error)))

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
    print("search-faults: %d passed, %d failed, %d harness errors"
          % (passed, failed, harness), flush=True)
    if harness:
        return 2
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
