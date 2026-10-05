#!/usr/bin/env python3
"""Boundary controls for the YanFS mutation-gate verdict logic.

tests/run_yanfs_mutation.py decides "detected" from a test run's exit code, a
*complete, self-consistent* Unity footer, AND the specific assertion the mutant
is meant to trip. That decision is the thing the gate's credibility rests on, so
the pure function classify_run() is exercised here on synthetic runs. A real
detection must be an owner FAIL on the required assertion marker inside a run
whose record lines and footer counters agree and whose exit code equals the
footer's failure count. Every other shape -- empty stdout, a bare FAIL without a
footer, a missing or duplicated footer, a counter mismatch, an ignored test
swallowed by the footer, exit 0 with a FAIL, a signal (including the shell's
128+ form), a timeout, a sanitizer report, a fixture abort, a step-limit stop, a
record after the footer, a same-owner failure on a different assertion, or a
missing marker -- must be a survivor/wrong-owner/wrong-assertion or a harness
error, never a detection. The mocked run_suites controls (one suite harness while
the other has an owner failure, and friends) run here too, so the default-suite
yanfs_mutation_gate covers them without a compiler.

The real compile-and-run negative controls (a no-effect mutant that must survive
and a wrong-owner table entry that must not be credited) live in
run_yanfs_mutation.py and run before its mutants.

Exit codes: 0 all synthetic cases matched, 1 a mismatch, 2 import failure.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

try:
    from run_yanfs_mutation import classify_run, self_test_run_suites
except ImportError as error:  # pragma: no cover - defensive
    print("HARNESS-ERROR: cannot import the gate: %s" % error, file=sys.stderr)
    sys.exit(2)

MARKER = "OWNER-MARKER"


def record(index, name, status, message="message"):
    suffix = (": " + message) if status in ("FAIL", "IGNORE") else ""
    return "tests/test_yanfs.c:%d:%s:%s%s" % (index, name, status, suffix)


def footer(tests, failures, ignored):
    return "%d Tests %d Failures %d Ignored " % (tests, failures, ignored)


def consistent(records, terminal=None):
    """A complete Unity run for the given (name, status[, message]) records."""
    failures = sum(1 for r in records if r[1] == "FAIL")
    ignored = sum(1 for r in records if r[1] == "IGNORE")
    lines = [record(index, r[0], r[1], r[2] if len(r) > 2 else "message")
             for index, r in enumerate(records, start=1)]
    lines.append("-----------------------")
    lines.append(footer(len(records), failures, ignored))
    if terminal is not None:
        lines.append(terminal)
    else:
        lines.append("OK" if failures == 0 else "FAIL")
    return ("\n".join(lines) + "\n").encode()


# (label, returncode, stdout, stderr, assertion, expected verdict)
CASES = [
    ("consistent single pass", 0, consistent([("owner", "PASS")]), b"", MARKER,
     "pass"),
    ("no-effect copy stays green", 0,
     consistent([("a", "PASS"), ("b", "PASS")]), b"", MARKER, "pass"),
    ("consistent ignore is not a failure", 0,
     consistent([("a", "PASS"), ("b", "IGNORE")]), b"", MARKER, "pass"),
    ("owner FAIL on the required assertion is detected", 1,
     consistent([("owner", "FAIL", MARKER)]), b"", MARKER, "owner-fail"),
    ("same owner on a different assertion is rejected", 1,
     consistent([("owner", "FAIL", "some unrelated assertion")]), b"", MARKER,
     "wrong-assertion"),
    ("missing marker metadata is never a detection", 1,
     consistent([("owner", "FAIL", MARKER)]), b"", None, "harness"),
    ("other test FAIL is wrong owner", 1,
     consistent([("other", "FAIL", MARKER)]), b"", MARKER, "wrong-owner"),
    ("owner plus another FAIL stays owner", 2,
     consistent([("owner", "FAIL", MARKER), ("other", "FAIL", MARKER)]), b"",
     MARKER, "owner-fail"),
    ("harness overrides a correct owner assertion", 1,
     consistent([("owner", "FAIL", MARKER)]),
     b"ERROR: AddressSanitizer: heap-use-after-free\n", MARKER, "harness"),
    ("empty stdout with exit 0", 0, b"", b"", MARKER, "harness"),
    ("probe: FAIL line, footer but no breaker", 2,
     b"x:1:owner:FAIL: x\n2 Tests 2 Failures 0 Ignored\n", b"", MARKER,
     "harness"),
    ("bare FAIL without a footer", 1,
     b"tests/test_yanfs.c:1:owner:FAIL: message\n", b"", MARKER, "harness"),
    ("missing footer", 1, (record(1, "owner", "FAIL", MARKER) + "\n").encode(),
     b"", MARKER, "harness"),
    ("duplicate footer", 0,
     consistent([("a", "PASS")]) + consistent([("a", "PASS")]), b"", MARKER,
     "harness"),
    ("test count mismatch", 0,
     (record(1, "a", "PASS") + "\n-----------------------\n"
      "3 Tests 0 Failures 0 Ignored \nOK\n").encode(), b"", MARKER, "harness"),
    ("FAIL under-counted by the footer", 0,
     (record(1, "owner", "FAIL", MARKER) + "\n-----------------------\n"
      "1 Tests 0 Failures 0 Ignored \nOK\n").encode(), b"", MARKER, "harness"),
    ("FAIL over-counted by the footer", 1,
     (record(1, "a", "PASS") + "\n-----------------------\n"
      "1 Tests 1 Failures 0 Ignored \nFAIL\n").encode(), b"", MARKER, "harness"),
    ("ignored swallowed by the footer", 0,
     (record(1, "a", "PASS") + record(2, "b", "IGNORE")
      + "\n-----------------------\n2 Tests 0 Failures 0 Ignored \nOK\n"
      ).encode(), b"", MARKER, "harness"),
    ("missing terminal line", 1,
     (record(1, "owner", "FAIL", MARKER) + "\n-----------------------\n"
      "1 Tests 1 Failures 0 Ignored \n").encode(), b"", MARKER, "harness"),
    ("terminal disagrees with the footer", 1,
     (record(1, "owner", "FAIL", MARKER) + "\n-----------------------\n"
      "1 Tests 1 Failures 0 Ignored \nOK\n").encode(), b"", MARKER, "harness"),
    ("record after the footer", 0,
     consistent([("a", "PASS")]) + (record(2, "late", "PASS") + "\n").encode(),
     b"", MARKER, "harness"),
    ("owner FAIL with exit 0", 0,
     consistent([("owner", "FAIL", MARKER)]), b"", MARKER, "harness"),
    ("exit code above the footer", 2,
     consistent([("owner", "FAIL", MARKER)]), b"", MARKER, "harness"),
    ("signal (negative) with owner FAIL", -11,
     consistent([("owner", "FAIL", MARKER)]), b"", MARKER, "harness"),
    ("signal 139 with owner FAIL", 139,
     consistent([("owner", "FAIL", MARKER)]), b"", MARKER, "harness"),
    ("timeout", None, consistent([("a", "PASS")]), b"", MARKER, "harness"),
    ("sanitizer report with owner FAIL", 1,
     consistent([("owner", "FAIL", MARKER)]),
     b"ERROR: AddressSanitizer: heap-use-after-free\n", MARKER, "harness"),
    ("fixture abort", 1, consistent([("owner", "FAIL", MARKER)]),
     b"HARNESS-ERROR: fixture abort\n", MARKER, "harness"),
    ("step-limit stop", 4, b"", b"yan_run: stopped after N steps\n", MARKER,
     "harness"),
    ("duplicate owner records are harness", 2,
     (record(1, "owner", "FAIL", MARKER) + "\n" + record(2, "owner", "FAIL", MARKER)
      + "\n-----------------------\n2 Tests 2 Failures 0 Ignored \nFAIL\n"
      ).encode(), b"", MARKER, "harness"),
    ("conflicting PASS and FAIL for one owner are harness", 1,
     (record(1, "owner", "PASS") + "\n" + record(2, "owner", "FAIL", MARKER)
      + "\n-----------------------\n2 Tests 1 Failures 0 Ignored \nFAIL\n"
      ).encode(), b"", MARKER, "harness"),
    ("marker only on stderr does not count", 1,
     consistent([("owner", "FAIL", "an unrelated assertion")]),
     ("stderr says " + MARKER).encode(), MARKER, "wrong-assertion"),
    ("marker only on a PASS line does not count", 1,
     (record(1, "owner", "FAIL", "an unrelated assertion") + "\n"
      + "tests/test_yanfs.c:9:helper:PASS: " + MARKER + "\n"
      + "\n-----------------------\n2 Tests 1 Failures 0 Ignored \nFAIL\n"
      ).encode(), b"", MARKER, "wrong-assertion"),
    ("marker only in another owner's message is wrong owner", 1,
     consistent([("other", "FAIL", MARKER)]), b"", MARKER, "wrong-owner"),
    ("same owner, same numbers, different assertion is rejected", 1,
     consistent([("owner", "FAIL", "Expected 10 Was 0")]), b"",
     "YFS bad_crc accepted", "wrong-assertion"),
]


def main():
    failures = 0
    for label, returncode, stdout, stderr, assertion, expected in CASES:
        if label == "marker only on a PASS line does not count":
            if ("helper:PASS: " + MARKER).encode() not in stdout:
                print(":FAIL: PASS control fixture lost its marker")
                failures += 1
        got = classify_run(returncode, stdout, stderr, "owner", assertion)
        if got != expected:
            print(":FAIL: %s: got %s expected %s" % (label, got, expected))
            failures += 1
    try:
        self_test_run_suites()
    except Exception as error:  # HarnessError and anything else is a failure
        print(":FAIL: mocked run_suites controls: %s" % error)
        failures += 1
    if failures:
        return 1
    print("PASS yanfs mutation classifier: %d synthetic cases and the mocked "
          "run_suites controls" % len(CASES))
    return 0


if __name__ == "__main__":
    sys.exit(main())
