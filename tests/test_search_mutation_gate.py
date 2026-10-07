#!/usr/bin/env python3
"""Synthetic controls for the search mutation gate (no compiler needed).

The gate's verdict is only trustworthy if its classifiers are. This exercises
the pure decode and classification that the real mutation run depends on,
reusing the shared checks in tests/run_yanfs_mutation.py rather than copying
them:

  * classify_run/parse_unity via literal Unity records: a complete PASS, an
    owner FAIL, a marker seen only in a PASS record (never credited), a
    wrong-owner, a same-owner wrong assertion, a duplicated record, a
    duplicated/malformed footer, and every shape that must be a harness error
    (signal, timeout, sanitizer, fixture abort, rc/summary disagreement);
  * the shared two-suite precedence self-test (self_test_run_suites).

It then runs the real mutation CLI on a temporary copy of the repository: a
missing in-repo compiled source, header, test or the shared helper script is a
hard failure (1) before any external tool; an intact repository with an absent
external compiler is 77, and an intact repository with a present placeholder
compiler but no Unity is 77 as well.

Run it directly or through CTest; it returns 0 when every control agrees, 1 if a
control disagrees, 2 on an unexpected exception.
"""
import importlib.util
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
FAILURES = []


def load(name, path):
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


try:
    RUN = load("search_mutation_gate", HERE / "run_search_mutation.py")
except Exception as error:  # a broken gate script is a hard failure, not a skip
    print("FAIL search-mutation-gate: cannot import the mutation gate: %r"
          % error, file=sys.stderr)
    sys.exit(1)

YANFS = RUN._YANFS


def check(label, got, expected):
    if got != expected:
        FAILURES.append("%s: got %r expected %r" % (label, got, expected))


# ------------------------------------------------------------- classifier

MARKER = "OWNER-ASSERTION-MARKER"


def classifier_controls():
    if YANFS is None:
        FAILURES.append("classifier: the shared Unity classifier did not import")
        return
    if RUN._IMPORT_ERROR is not None:
        FAILURES.append("classifier: import error was %r" % RUN._IMPORT_ERROR)
    suite = YANFS.unity_suite
    classify = YANFS.classify_run
    pass_out = suite([("a", "PASS"), ("b", "PASS")])
    owner_out = suite([("owner", "FAIL", MARKER)])
    # unity_suite omits PASS messages, so add the marker to an actual PASS
    # record explicitly and assert the fixture contains that record.
    marker_only_in_pass = suite([("owner", "PASS"), ("other", "FAIL", "other")])
    marker_only_in_pass = marker_only_in_pass.replace(
        b":owner:PASS\n", b":owner:PASS: " + MARKER.encode() + b"\n")
    check("fixture-pass-has-marker",
          b":owner:PASS: " + MARKER.encode() + b"\n" in marker_only_in_pass,
          True)
    marker_in_helper_pass = suite([
        ("owner", "FAIL", "some other assertion"), ("helper", "PASS")])
    marker_in_helper_pass = marker_in_helper_pass.replace(
        b":helper:PASS\n", b":helper:PASS: " + MARKER.encode() + b"\n")
    check("fixture-helper-pass-has-marker",
          b":helper:PASS: " + MARKER.encode() + b"\n" in marker_in_helper_pass,
          True)
    same_owner_other = suite([("owner", "FAIL", "some other assertion")])
    other_owner = suite([("other", "FAIL", MARKER)])
    dup_record = owner_out.replace(
        b"-----------------------",
        b"tests/test_search.c:9:owner:PASS\n-----------------------")
    dup_footer = owner_out + owner_out
    no_footer = b"tests/test_search.c:1:owner:FAIL: " + MARKER.encode() + b"\n"
    no_breaker = owner_out.replace(b"-----------------------\n", b"")

    check("classifier-pass", classify(0, pass_out, b"", "owner", MARKER), "pass")
    check("classifier-owner",
          classify(1, owner_out, b"", "owner", MARKER), "owner-fail")
    check("classifier-marker-only-pass",
          classify(1, marker_only_in_pass, b"", "owner", MARKER), "wrong-owner")
    check("classifier-same-owner-other",
          classify(1, same_owner_other, b"", "owner", MARKER), "wrong-assertion")
    check("classifier-other-pass-cannot-credit-owner",
          classify(1, marker_in_helper_pass, b"", "owner", MARKER),
          "wrong-assertion")
    check("classifier-wrong-owner",
          classify(1, other_owner, b"", "owner", MARKER), "wrong-owner")
    check("classifier-no-marker-metadata",
          classify(1, owner_out, b"", "owner", None), "harness")
    check("classifier-duplicate-record",
          classify(1, dup_record, b"", "owner", MARKER), "harness")
    check("classifier-duplicate-footer",
          classify(1, dup_footer, b"", "owner", MARKER), "harness")
    check("classifier-no-footer", classify(1, no_footer, b"", "owner", MARKER),
          "harness")
    check("classifier-no-breaker", classify(1, no_breaker, b"", "owner", MARKER),
          "harness")
    check("classifier-rc-mismatch",
          classify(2, owner_out, b"", "owner", MARKER), "harness")
    check("classifier-exit0-with-failure",
          classify(0, owner_out, b"", "owner", MARKER), "harness")
    check("classifier-signal-139", classify(139, owner_out, b"", "owner", MARKER),
          "harness")
    check("classifier-signal-negative",
          classify(-11, owner_out, b"", "owner", MARKER), "harness")
    check("classifier-timeout", classify(None, owner_out, b"", "owner", MARKER),
          "harness")
    check("classifier-sanitizer",
          classify(1, owner_out, b"runtime error: x", "owner", MARKER), "harness")
    check("classifier-fixture-abort",
          classify(1, owner_out, b"fixture abort", "owner", MARKER), "harness")
    check("classifier-empty-stdout", classify(0, b"", b"", "owner", MARKER),
          "harness")

    try:
        YANFS.self_test_run_suites()
    except YANFS.HarnessError as error:
        FAILURES.append("two-suite: %s" % error)


# ------------------------------------------------------- real CLI missing inputs

def run_cli(tree, work, unity, cc):
    command = [sys.executable, "-B",
               str(tree / "tests" / "run_search_mutation.py"),
               "--source", str(tree), "--work", str(work), "--cc", cc,
               "--unity", str(unity)]
    try:
        proc = subprocess.run(command, capture_output=True, timeout=120)
    except (subprocess.TimeoutExpired, OSError) as error:
        return None, repr(error).encode()
    return proc.returncode, proc.stdout + proc.stderr


def cli_missing_controls():
    source = HERE.parent
    base = tempfile.mkdtemp(prefix="search-gate-cli-")
    try:
        tree = Path(base) / "source"
        shutil.copytree(source, tree,
                        ignore=shutil.ignore_patterns(".git", "build",
                                                      "__pycache__", ".venv"))
        unity = Path(base) / "unity"
        (unity / "src").mkdir(parents=True)
        (unity / "src" / "unity.c").write_text("/* fake unity for the gate */\n")
        (unity / "src" / "unity.h").write_text("/* fake unity for the gate */\n")
        work = Path(base) / "work"
        placeholder_cc = sys.executable
        absent_cc = str(Path(base) / "no-such-compiler")

        for rel in ("os/search.c", "os/search.h", "os/search_linear.c",
                    "os/search_linear.h", "tests/test_search.c",
                    "tests/test_shell.c", "tests/run_yanfs_mutation.py"):
            victim = tree / rel
            saved = victim.read_bytes()
            victim.unlink()
            rc, blob = run_cli(tree, work, unity, placeholder_cc)
            victim.write_bytes(saved)
            check("cli-missing-" + rel, rc, 1)
            if rel != "tests/run_yanfs_mutation.py":
                check("cli-missing-diagnostic-" + rel,
                      b"missing" in blob.lower(), True)

        rc, _blob = run_cli(tree, work, unity, absent_cc)
        check("cli-intact-absent-cc-77", rc, 77)
        empty_unity = Path(base) / "empty-unity"
        empty_unity.mkdir()
        rc, _blob = run_cli(tree, work, empty_unity, placeholder_cc)
        check("cli-intact-no-unity-77", rc, 77)
    except Exception as error:  # noqa: BLE001 - the control harness itself
        FAILURES.append("cli: control harness failed: %r" % error)
    finally:
        shutil.rmtree(base, ignore_errors=True)


def main():
    classifier_controls()
    cli_missing_controls()
    if FAILURES:
        for line in FAILURES:
            print("FAIL search-mutation-gate: %s" % line, file=sys.stderr)
        print("search-mutation-gate: %d control(s) disagreed" % len(FAILURES),
              file=sys.stderr)
        return 1
    print("PASS search-mutation-gate: all synthetic controls agree")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:  # noqa: BLE001 - a control harness failure
        print("HARNESS-ERROR: search-mutation-gate: %r" % error, file=sys.stderr)
        sys.exit(2)
