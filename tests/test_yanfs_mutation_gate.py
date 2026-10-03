#!/usr/bin/env python3
"""Boundary controls for the YanFS mutation-gate verdict logic.

tests/run_yanfs_mutation.py decides "detected" from a test run's exit code and a
*complete, self-consistent* Unity footer. That decision is the thing the gate's
credibility rests on, so the pure function classify_run() is exercised here on
synthetic runs: a real detection must be an owner FAIL inside a run whose
record lines and footer counters agree and whose exit code equals the footer's
failure count. Every other shape -- empty stdout, a bare FAIL without a footer,
a missing or duplicated footer, a counter mismatch, an ignored test swallowed by
the footer, exit 0 with a FAIL, a signal (including the shell's 128+ form), a
timeout, a sanitizer report, a fixture abort, a step-limit stop, or a record
after the footer -- must be a survivor/wrong-owner or a harness error, never a
detection. The mocked run_suites controls (one suite harness while the other has
an owner failure, and friends) run here too, so the default-suite
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


def record(index, name, status):
    suffix = ": message" if status == "FAIL" else ""
    return "tests/test_yanfs.c:%d:%s:%s%s" % (index, name, status, suffix)


def footer(tests, failures, ignored):
    return "%d Tests %d Failures %d Ignored " % (tests, failures, ignored)


def consistent(records, terminal=None):
    """A complete Unity run for the given (name, status) records."""
    failures = sum(1 for _name, status in records if status == "FAIL")
    ignored = sum(1 for _name, status in records if status == "IGNORE")
    lines = [record(index, name, status)
             for index, (name, status) in enumerate(records, start=1)]
    lines.append("-----------------------")
    lines.append(footer(len(records), failures, ignored))
    if terminal is not None:
        lines.append(terminal)
    else:
        lines.append("OK" if failures == 0 else "FAIL")
    return ("\n".join(lines) + "\n").encode()


# (label, returncode, stdout, stderr, expected verdict)
CASES = [
    ("consistent single pass", 0, consistent([("owner", "PASS")]), b"", "pass"),
    ("no-effect copy stays green", 0,
     consistent([("a", "PASS"), ("b", "PASS")]), b"", "pass"),
    ("consistent ignore is not a failure", 0,
     consistent([("a", "PASS"), ("b", "IGNORE")]), b"", "pass"),
    ("owner FAIL is detected", 1, consistent([("owner", "FAIL")]), b"",
     "owner-fail"),
    ("other test FAIL is wrong owner", 1, consistent([("other", "FAIL")]), b"",
     "wrong-owner"),
    ("owner plus another FAIL stays owner", 2,
     consistent([("owner", "FAIL"), ("other", "FAIL")]), b"", "owner-fail"),
    ("empty stdout with exit 0", 0, b"", b"", "harness"),
    ("probe: FAIL line, footer but no breaker", 2,
     b"x:1:owner:FAIL: x\n2 Tests 2 Failures 0 Ignored\n", b"", "harness"),
    ("bare FAIL without a footer", 1,
     b"tests/test_yanfs.c:1:owner:FAIL: message\n", b"", "harness"),
    ("missing footer", 1, (record(1, "owner", "FAIL") + "\n").encode(), b"",
     "harness"),
    ("duplicate footer", 0,
     consistent([("a", "PASS")]) + consistent([("a", "PASS")]), b"", "harness"),
    ("test count mismatch", 0,
     (record(1, "a", "PASS") + "\n-----------------------\n"
      "3 Tests 0 Failures 0 Ignored \nOK\n").encode(), b"", "harness"),
    ("FAIL under-counted by the footer", 0,
     (record(1, "owner", "FAIL") + "\n-----------------------\n"
      "1 Tests 0 Failures 0 Ignored \nOK\n").encode(), b"", "harness"),
    ("FAIL over-counted by the footer", 1,
     (record(1, "a", "PASS") + "\n-----------------------\n"
      "1 Tests 1 Failures 0 Ignored \nFAIL\n").encode(), b"", "harness"),
    ("ignored swallowed by the footer", 0,
     (record(1, "a", "PASS") + record(2, "b", "IGNORE")
      + "\n-----------------------\n2 Tests 0 Failures 0 Ignored \nOK\n"
      ).encode(), b"", "harness"),
    ("missing terminal line", 1,
     (record(1, "owner", "FAIL") + "\n-----------------------\n"
      "1 Tests 1 Failures 0 Ignored \n").encode(), b"", "harness"),
    ("terminal disagrees with the footer", 1,
     (record(1, "owner", "FAIL") + "\n-----------------------\n"
      "1 Tests 1 Failures 0 Ignored \nOK\n").encode(), b"", "harness"),
    ("record after the footer", 0,
     consistent([("a", "PASS")]) + (record(2, "late", "PASS") + "\n").encode(),
     b"", "harness"),
    ("owner FAIL with exit 0", 0, consistent([("owner", "FAIL")]), b"",
     "harness"),
    ("exit code above the footer", 2, consistent([("owner", "FAIL")]), b"",
     "harness"),
    ("signal (negative) with owner FAIL", -11,
     consistent([("owner", "FAIL")]), b"", "harness"),
    ("signal 139 with owner FAIL", 139, consistent([("owner", "FAIL")]), b"",
     "harness"),
    ("timeout", None, consistent([("a", "PASS")]), b"", "harness"),
    ("sanitizer report with owner FAIL", 1,
     consistent([("owner", "FAIL")]),
     b"ERROR: AddressSanitizer: heap-use-after-free\n", "harness"),
    ("fixture abort", 1, consistent([("owner", "FAIL")]),
     b"HARNESS-ERROR: fixture abort\n", "harness"),
    ("step-limit stop", 4, b"", b"yan_run: stopped after N steps\n", "harness"),
]


def main():
    failures = 0
    for label, returncode, stdout, stderr, expected in CASES:
        got = classify_run(returncode, stdout, stderr, "owner")
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
