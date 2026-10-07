#!/usr/bin/env python3
"""0026 real Guest build/snippet protocol faults and exact output refusal.

Uses the existing Host driver and its strict verdict classifier. A short-word
two-block fixture checks build and snippet faults; an overlong-word fixture
checks LIMIT followed by scan fallback. Fault numbers count completed block
responses, including the two mount responses. Each injection must actually
fire and stop exactly at its response, without a result, OK, or later exit.
"""
import argparse
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_terminal_runtime as runtime
import run_search_terms as healthy

INPUTS = healthy.INPUTS + ("tests/guest/run_search_terms_faults.py",
                          "tests/guest/run_terminal_runtime.py") + tuple(
    runtime.HOST_SOURCES + runtime.HOST_HEADERS)
COMMAND = b"search hit\nexit\n"
FATAL = runtime.TOHOST_SHELL_FATAL


def body():
    # The hit starts at byte 4094, so its window crosses the block boundary.
    # Both snippet block replies (7 and 8) are therefore actually requested.
    return b"z " * 2047 + b"hit " + b"z " * 450 + b"z\n"


def execute(driver, args, image, label, fault="none", response=0, tx=0,
            baseline=None, expected=1, command=COMMAND):
    result, status, disk, capture = runtime.run_production(
        driver, args, args.work, image, command, label, fault, response, tx,
        baseline=baseline, expect_tohost=expected)
    values = runtime.classify_production(result, status, label, {expected})
    if values["_tohost"] != expected:
        raise runtime.OwnerFailure("%s: unexpected tohost" % label)
    if disk.read_bytes() != image:
        raise runtime.OwnerFailure("%s: search changed disk" % label)
    try:
        stack = int(values["task0_observed_stack_bytes"])
        responses = int(values["block_responses"])
    except (KeyError, ValueError) as error:
        raise runtime.HarnessError("missing stack/block observation") from error
    require(0 < stack < 4096, "%s: task stack observation must fit" % label)
    values["_responses"] = responses
    values["_stack"] = stack
    return values, capture.read_bytes()


def require(condition, message):
    if not condition:
        raise runtime.OwnerFailure(message)


def main():
    parser = argparse.ArgumentParser()
    for flag in ("source", "work", "guest"):
        parser.add_argument("--" + flag, required=True, type=Path)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--sanitizers", action="store_true")
    args = parser.parse_args()
    missing = [args.source / path for path in INPUTS
               if not (args.source / path).is_file()]
    if not args.guest.is_file():
        missing.append(args.guest)
    if missing:
        for path in missing:
            print(":FAIL: missing required input: %s" % path)
        return 1
    if not runtime.tool_available(args.cc):
        print("SKIP: no external host compiler")
        return 77
    args.work.mkdir(parents=True, exist_ok=True)
    try:
        driver = runtime.build_host(args.source.resolve(), args.work, args.cc,
                                    sanitizers=args.sanitizers)
    except (runtime.HarnessError, OSError) as error:
        print(":HARNESS-ERROR: build: %s" % error)
        return 2
    image = runtime.seeded_image([("f", body(), 1)])
    results = []

    def case(name, operation):
        try:
            operation()
            results.append((name, None))
        except runtime.OwnerFailure as error:
            results.append((name, str(error)))
        except (runtime.HarnessError, OSError) as error:
            results.append((name, "HARNESS: " + str(error)))

    baseline_output = None
    try:
        _, baseline_output = execute(driver, args, image, "healthy")
        expected = healthy.row("f", 1, b"z " * 20 + b"hit " + b"z " * 58,
                               left=True, right=True)
        expected += healthy.summary(1)
        require(expected in baseline_output,
                "healthy: exact bounded result and summary must appear")
        results.append(("healthy", None))
    except runtime.OwnerFailure as error:
        results.append(("healthy", str(error)))
    except (runtime.HarnessError, OSError) as error:
        results.append(("healthy", "HARNESS: " + str(error)))

    def protocol(response):
        values, output = execute(driver, args, image, "protocol-%d" % response,
                                 "protocol", response, expected=FATAL)
        require(values["protocol_fired"] == "1", "protocol fault must fire")
        require(values["_responses"] == response,
                "protocol fault must stop all later block requests")
        require(b"ERROR PROTOCOL\r\n" in output, "protocol diagnostic missing")
        require(b"OK search" not in output, "failed query must not print OK")
        require(b"yanfs: exit" not in output, "failed query must stop later exit")
        require(b"f:1:" not in output,
                "failed build/snippet preparation must not publish partial row")

    for response in range(3, 9):
        case("protocol-%d" % response, lambda r=response: protocol(r))

    def io():
        _, output = execute(driver, args, image, "io", "io", expected=FATAL)
        require(b"ERROR IO\r\n" in output, "IO diagnostic missing")
        require(b"OK search" not in output and b"f:1:" not in output,
                "failed preflight must publish neither row nor success")
        require(b"yanfs: exit" not in output, "IO must stop later commands")
    case("io-preflight", io)

    # One overlong ASCII word makes the build LIMIT. Replies 3-4 preflight,
    # 5-6 build, 7-8 fallback preflight and 9-10 fallback scan are requested.
    fallback_image = runtime.seeded_image([
        ("f", b"x" * 256 + b" " + body()[257:], 1)])
    def fallback_protocol(response):
        values, output = execute(driver, args, fallback_image,
                                 "fallback-protocol-%d" % response,
                                 "protocol", response, expected=FATAL)
        require(values["protocol_fired"] == "1", "fallback fault must fire")
        require(values["_responses"] == response,
                "fallback fault must stop later block requests")
        require(b"ERROR PROTOCOL\r\n" in output,
                "fallback protocol diagnostic missing")
        require(b"OK search" not in output and b"f:1:" not in output,
                "fallback failure must publish neither result nor success")
        require(b"yanfs: exit" not in output,
                "fallback failure must stop later commands")
    for response in range(7, 11):
        case("fallback-protocol-%d" % response,
             lambda r=response: fallback_protocol(r))

    if baseline_output is not None:
        row_at = baseline_output.find(b"f:1:")
        ok_at = baseline_output.find(b"OK search total=")
        if row_at < 0 or ok_at < 0:
            results.append(("tx", "HARNESS: missing baseline row/summary"))
        else:
            baseline = args.work / "healthy.bin"
            baseline.write_bytes(baseline_output)
            thresholds = [("row-first", row_at + 1),
                          ("row-middle", (row_at + ok_at) // 2),
                          ("row-last", ok_at),
                          ("summary-first", ok_at + 1),
                          ("summary-last", ok_at + len(healthy.summary(1)))]

            def tx(label, at):
                values, output = execute(driver, args, image, "tx-" + label,
                                         tx=at, baseline=baseline, expected=FATAL)
                require(values["tx_fired"] == "1", "output fault must fire")
                require(values["delivered"] == at and values["out_length"] == at,
                        "output must stop at the injected byte")
                require(output == baseline_output[:at],
                        "output must retain exactly the delivered prefix")
                require(values["_responses"] == 8,
                        "output refusal must not initiate another disk request")
            for label, at in thresholds:
                case("tx-" + label, lambda label=label, at=at: tx(label, at))

    for name, failure in results:
        if failure is None:
            print("PASS " + name, flush=True)
        elif failure.startswith("HARNESS:"):
            print(":HARNESS-ERROR: %s: %s" % (name, failure), flush=True)
        else:
            print(":FAIL: %s: %s" % (name, failure), flush=True)
    harness = sum(failure is not None and failure.startswith("HARNESS:")
                  for _, failure in results)
    failed = sum(failure is not None and not failure.startswith("HARNESS:")
                 for _, failure in results)
    print("search_terms faults: %d passed, %d failed, %d harness errors"
          % (len(results) - failed - harness, failed, harness), flush=True)
    return 2 if harness else (1 if failed else 0)


if __name__ == "__main__":
    sys.exit(main())
