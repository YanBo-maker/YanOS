#!/usr/bin/env python3
"""Synthetic controls for the terminal mutation gate (no compiler needed).

The gate's verdict is only trustworthy if its classifiers are. This exercises
the pure functions the real mutation run depends on:

  * the permanent runtime controls inside run_terminal_mutation.py
    (self_test_classifiers): complete PASS / owner-FAIL / wrong-owner verdicts
    and every malformed shape that must be a harness error, built from literal
    records rather than a helper that maps an unknown record to a verdict;
  * the Unity decode and native classification reused from
    tests/run_yanfs_mutation.py (including its own two-suite precedence
    controls);
  * the runtime decode (parse_runtime / classify_runtime) over synthetic
    run_terminal_runtime.py output: a full footer, unique per-case records, an
    owner FAIL with no harness line, late/duplicate/unknown records, a footer on
    stderr, and every condition that must win over an owner FAIL;
  * the real CLI's missing-input behaviour on a temporary copy of the
    repository: a missing source/header/test/helper is a hard failure (1) and a
    missing external host compiler is 77 -- not the other way round.

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
    TERM = load("term_mutation_gate", HERE / "run_terminal_mutation.py")
except Exception as error:  # a broken gate script is a hard failure, not a skip
    print("FAIL terminal-mutation-gate: cannot import the mutation gate: %r"
          % error, file=sys.stderr)
    sys.exit(1)

YANFS = TERM._YANFS


def check(group, label, got, expected):
    if got != expected:
        FAILURES.append("%s/%s: got %r expected %r" % (group, label, got, expected))


# ---------------------------------------------------------- permanent controls

def permanent_controls():
    try:
        TERM.self_test_classifiers()
    except Exception as error:  # noqa: BLE001 - a broken selftest is a failure
        FAILURES.append("self-test: %s" % error)


# ------------------------------------------------------------- native decode

MARKER = "OWNER-MARKER"


def native_controls():
    if YANFS is None:
        FAILURES.append("native: the shared Unity classifier did not import")
        return
    suite = YANFS.unity_suite
    pass_out = suite([("a", "PASS"), ("b", "PASS")])
    owner_out = suite([("owner", "FAIL", MARKER)])
    wrong_assertion_out = suite([("owner", "FAIL", "some other assertion")])
    other_out = suite([("other", "FAIL", MARKER)])
    ignored_out = suite([("a", "IGNORE")])
    after_footer = pass_out + b"tests/test_shell.c:3:c:PASS\n"

    check("native", "pass", YANFS.classify_run(0, pass_out, b"", "owner", MARKER),
          "pass")
    check("native", "owner", YANFS.classify_run(1, owner_out, b"", "owner", MARKER),
          "owner-fail")
    check("native", "wrong-assertion", YANFS.classify_run(
        1, wrong_assertion_out, b"", "owner", MARKER), "wrong-assertion")
    check("native", "missing-marker", YANFS.classify_run(
        1, owner_out, b"", "owner", None), "harness")
    check("native", "wrong", YANFS.classify_run(1, other_out, b"", "owner", MARKER),
          "wrong-owner")
    check("native", "empty", YANFS.classify_run(0, b"", b"", "owner", MARKER),
          "harness")
    check("native", "no-footer", YANFS.classify_run(
        1, b"tests/test_shell.c:1:a:FAIL:\n", b"", "owner", MARKER), "harness")
    check("native", "double-footer", YANFS.classify_run(
        1, owner_out + owner_out, b"", "owner", MARKER), "harness")
    check("native", "count-wrong", YANFS.classify_run(
        1, b"tests/test_shell.c:1:a:FAIL:\n-----------------------\n"
           b"9 Tests 1 Failures 0 Ignored \nFAIL\n", b"", "owner", MARKER),
        "harness")
    check("native", "record-after-footer", YANFS.classify_run(
        1, after_footer, b"", "owner", MARKER), "harness")
    check("native", "exit0-with-fail", YANFS.classify_run(0, owner_out, b"",
                                                          "owner", MARKER),
          "harness")
    check("native", "ignored-swallowed", YANFS.classify_run(
        1, ignored_out, b"", "owner", MARKER), "harness")
    check("native", "stderr-noop", YANFS.classify_run(0, pass_out, b"a note\n",
                                                      "owner", MARKER), "pass")
    check("native", "signal", YANFS.classify_run(139, owner_out, b"", "owner",
                                                 MARKER), "harness")
    check("native", "128-form", YANFS.classify_run(137, owner_out, b"", "owner",
                                                   MARKER), "harness")
    check("native", "timeout", YANFS.classify_run(None, owner_out, b"", "owner",
                                                  MARKER), "harness")
    check("native", "sanitizer-overrides-owner", YANFS.classify_run(
        1, owner_out, b"runtime error: boom", "owner", MARKER), "harness")
    check("native", "fixture-abort", YANFS.classify_run(1, owner_out,
                                                        b"fixture abort", "owner",
                                                        MARKER), "harness")
    duplicate_out = (b"tests/test_shell.c:1:owner:FAIL: " + MARKER.encode()
                     + b"\ntests/test_shell.c:2:owner:FAIL: " + MARKER.encode()
                     + b"\n-----------------------\n2 Tests 2 Failures 0 Ignored"
                       b" \nFAIL\n")
    check("native", "duplicate-owner-records", YANFS.classify_run(
        2, duplicate_out, b"", "owner", MARKER), "harness")
    check("native", "marker-only-stderr", YANFS.classify_run(
        1, suite([("owner", "FAIL", "unrelated")]), MARKER.encode(), "owner",
        MARKER), "wrong-assertion")
    check("native", "marker-only-other-owner", YANFS.classify_run(
        1, suite([("other", "FAIL", MARKER)]), b"", "owner", MARKER),
        "wrong-owner")
    check("native", "same-owner-same-numbers-wrong-assertion",
          YANFS.classify_run(1, suite([("owner", "FAIL", "Expected 10 Was 16")]),
                             b"", "owner", MARKER), "wrong-assertion")


# ------------------------------------------------------------ runtime decode

def runtime_controls():
    out = TERM._runtime_output
    ok = out([("a", "PASS")])
    owner = out([("owner", "FAIL")])
    other = out([("other", "FAIL")])
    two = out([("a", "PASS"), ("b", "PASS")])

    check("runtime", "pass", TERM.classify_runtime(0, ok, b"", "owner"), "pass")
    check("runtime", "owner", TERM.classify_runtime(1, owner, b"", "owner"),
          "owner-fail")
    check("runtime", "owner-assertion", TERM.classify_runtime(
        1, owner, b"", "owner", "bad"), "owner-fail")
    check("runtime", "same-case-wrong-assertion", TERM.classify_runtime(
        1, owner, b"", "owner", "different invariant"), "wrong-owner")
    for exit_code in (4, 5):
        check("runtime", "host-exit-%d-beats-owner" % exit_code,
              TERM.classify_runtime(exit_code, owner, b"", "owner"), "harness")
    check("runtime", "wrong", TERM.classify_runtime(1, other, b"", "owner"),
          "wrong-owner")
    check("runtime", "exit1-no-fail", TERM.classify_runtime(1, ok, b"", "owner"),
          "harness")
    check("runtime", "empty", TERM.classify_runtime(0, b"", b"", "owner"), "harness")
    check("runtime", "no-footer", TERM.classify_runtime(1, b"PASS a\n", b"", "owner"),
          "harness")
    check("runtime", "double-footer", TERM.classify_runtime(1, owner + owner, b"",
                                                            "owner"), "harness")
    check("runtime", "count-wrong", TERM.classify_runtime(
        1, out([("owner", "FAIL")], passes=9), b"", "owner"), "harness")
    check("runtime", "late-pass", TERM.classify_runtime(1, owner + b"PASS late\n",
                                                        b"", "owner"), "harness")
    check("runtime", "late-fail", TERM.classify_runtime(
        1, ok + b":FAIL: late: x\n", b"", "owner"), "harness")
    check("runtime", "duplicate-case", TERM.classify_runtime(
        1, out([("owner", "FAIL"), ("owner", "FAIL")]), b"", "owner"), "harness")
    check("runtime", "ignore-record", TERM.classify_runtime(
        1, b"a:IGNORE\nterminal-runtime: 1 passed, 0 failed, 0 harness errors"
           b" (baseline ok)\n", b"", "owner"), "harness")
    check("runtime", "unknown-record", TERM.classify_runtime(
        1, b"WHATEVER x\n" + owner, b"", "owner"), "harness")
    check("runtime", "malformed-footer", TERM.classify_runtime(
        1, b"PASS a\nterminal-runtime: two passed (baseline ok)\n", b"", "owner"),
        "harness")
    check("runtime", "stderr-record", TERM.classify_runtime(1, owner, b"PASS sneaky\n",
                                                            "owner"), "harness")
    check("runtime", "stderr-footer", TERM.classify_runtime(
        1, owner, owner, "owner"), "harness")
    check("runtime", "exit0-with-fail", TERM.classify_runtime(0, owner, b"", "owner"),
          "harness")
    check("runtime", "stderr-noop", TERM.classify_runtime(0, two, b"a note\n", "owner"),
          "pass")
    check("runtime", "harness-beats-owner", TERM.classify_runtime(
        1, out([("owner", "FAIL")], harness=1), b"", "owner"), "harness")
    check("runtime", "baseline-failed", TERM.classify_runtime(
        1, out([("owner", "FAIL")], baseline="failed"), b"", "owner"), "harness")
    check("runtime", "signal", TERM.classify_runtime(139, owner, b"", "owner"),
          "harness")
    check("runtime", "128-form", TERM.classify_runtime(137, owner, b"", "owner"),
          "harness")
    check("runtime", "timeout", TERM.classify_runtime(None, owner, b"", "owner"),
          "harness")
    check("runtime", "sanitizer", TERM.classify_runtime(1, owner, b"runtime error: x",
                                                        "owner"), "harness")


# --------------------------------------------------------------- two suites

def two_suite_controls():
    if YANFS is None:
        return
    try:
        YANFS.self_test_run_suites()
    except YANFS.HarnessError as error:
        FAILURES.append("two-suite: %s" % error)


# ------------------------------------------------------- real CLI missing inputs

def run_cli(tree, work, unity, cc, gcc, mode_args=("--native-only",)):
    command = [sys.executable, "-B", str(tree / "tests" / "run_terminal_mutation.py"),
               "--source", str(tree), "--work", str(work), "--cc", cc,
               "--unity", str(unity), "--gcc", gcc] + list(mode_args)
    try:
        proc = subprocess.run(command, capture_output=True, timeout=120)
    except (subprocess.TimeoutExpired, OSError) as error:
        return None, repr(error).encode()
    return proc.returncode, proc.stdout + proc.stderr


def run_runtime_cli(tree, work, cc, gcc, guest, extra):
    command = [sys.executable, "-B",
               str(tree / "tests" / "guest" / "run_terminal_runtime.py"),
               "--source", str(tree), "--work", str(work), "--cc", cc,
               "--gcc", gcc, "--guest", guest] + list(extra)
    try:
        proc = subprocess.run(command, capture_output=True, timeout=120)
    except (subprocess.TimeoutExpired, OSError) as error:
        return None, repr(error).encode()
    return proc.returncode, proc.stdout + proc.stderr


def cli_missing_controls():
    """Run the real CLI on a temporary copy with one repository input removed:
    every removal must be a hard 1, and a missing external compiler must be 77.
    The original repository is never touched."""
    source = HERE.parent
    base = tempfile.mkdtemp(prefix="gate-cli-")
    try:
        tree = Path(base) / "source"
        shutil.copytree(source, tree,
                        ignore=shutil.ignore_patterns(".git", "build", "__pycache__",
                                                      ".venv"))
        unity = Path(base) / "unity"
        (unity / "src").mkdir(parents=True)
        (unity / "src" / "unity.c").write_text("/* fake unity for the gate */\n")
        (unity / "src" / "unity.h").write_text("/* fake unity for the gate */\n")
        work = Path(base) / "work"
        # A discoverable external "cc" placeholder: sys.executable is an
        # absolute, executable file on every platform, and the repository-missing
        # controls return 1 long before anything is compiled. The fake Unity is
        # only used for input classification, never built.
        placeholder_cc = sys.executable
        absent_cc = str(Path(base) / "no-such-compiler")
        for rel in ("os/shell.c", "os/shell.h", "os/search.c", "os/search.h",
                    "os/search_linear.c", "os/search_linear.h",
                    "tests/test_line.c",
                    "tests/guest/guest.h", "tests/terminal_runtime_driver.c",
                    "tests/guest/run_terminal_runtime.py",
                    "tests/run_yanfs_mutation.py"):
            victim = tree / rel
            saved = victim.read_bytes()
            victim.unlink()
            rc, _blob = run_cli(tree, work, unity, placeholder_cc, "gcc")
            victim.write_bytes(saved)
            check("cli", "missing-" + rel, rc, 1)
        rc, _blob = run_cli(tree, work, unity, absent_cc, "gcc")
        check("cli", "missing-cc-77", rc, 77)
        # The external cross compiler is checked before repository inputs and is
        # only required by --runtime-only: an absent gcc is 77, not 1.
        rc, _blob = run_cli(tree, work, unity, placeholder_cc, absent_cc,
                            mode_args=("--runtime-only",))
        check("cli", "runtime-only-missing-gcc-77", rc, 77)
        # The selected fixture scenario is only legal with --only-fixtures.
        guest = str(Path(base) / "guest.elf")
        rc, _blob = run_runtime_cli(tree, work, placeholder_cc, "gcc", guest,
                                    ["--fixture-scenario", "blocked"])
        check("runtime-cli", "fixture-scenario-requires-only-fixtures", rc, 2)
        # An existing production ELF must not hide a removed compiled input.
        # Missing external tools make the required order visible: repository
        # failure is 1, while an intact repository may legitimately return 77.
        Path(guest).write_bytes(b"guard-only ELF placeholder")
        for rel in ("os/search.c", "os/search_linear.c", "os/shell.c",
                    "os/editor.c", "os/yanfs.c", "apps/yanfs_terminal/main.c"):
            victim = tree / rel
            saved = victim.read_bytes()
            victim.unlink()
            rc, blob = run_runtime_cli(tree, work, absent_cc, absent_cc,
                                       guest, [])
            victim.write_bytes(saved)
            check("runtime-cli", "stale-elf-missing-" + rel, rc, 1)
            check("runtime-cli", "missing-diagnostic-" + rel,
                  b"missing required input" in blob, True)
        rc, _blob = run_runtime_cli(tree, work, absent_cc, absent_cc, guest, [])
        check("runtime-cli", "intact-inputs-missing-tools-77", rc, 77)
    except Exception as error:  # noqa: BLE001 - the control harness itself
        FAILURES.append("cli: control harness failed: %r" % error)
    finally:
        shutil.rmtree(base, ignore_errors=True)


def main():
    permanent_controls()
    native_controls()
    runtime_controls()
    two_suite_controls()
    cli_missing_controls()
    if FAILURES:
        for line in FAILURES:
            print("FAIL terminal-mutation-gate: %s" % line, file=sys.stderr)
        print("terminal-mutation-gate: %d control(s) disagreed" % len(FAILURES),
              file=sys.stderr)
        return 1
    print("PASS terminal-mutation-gate: all synthetic controls agree")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:  # noqa: BLE001 - a control harness failure
        print("HARNESS-ERROR: terminal-mutation-gate: %r" % error, file=sys.stderr)
        sys.exit(2)
