#!/usr/bin/env python3
"""Boundary controls for the editor mutation gate (no compiler needed).

tests/run_editor_mutation.py decides "detected" from one Unity run's exit code, a
complete self-consistent footer, and the specific assertion marker the mutation
is meant to trip. Its classifier is the hardened classifier of
tests/run_yanfs_mutation.py, reused by import; this gate exercises that shared
classifier on synthetic runs and, separately, checks that *every* marker the
editor table declares is specific:

  * the exact owner + marker is a detection;
  * the same owner failing on a different assertion is wrong-assertion;
  * the marker on stderr, on a PASS line, or on another owner's FAIL is never a
    detection;
  * missing marker metadata is a harness error, never a detection;
  * a malformed footer, a counter mismatch, exit 0 with a FAIL, a signal
    (including the shell's 128+ form), a timeout, a sanitizer report and
    duplicate/ambiguous records are all harness errors;
  * a complete pass is a pass, and a no-effect copy stays green.

It also runs the shared two-suite precedence controls and the real CLI's
missing-input behaviour on a temporary copy of the repository: a missing
source/header/test/helper is a hard failure (1), while a missing external host
compiler or Unity is 77 -- the external check comes first. The original
repository is never touched.

Exit codes: 0 all synthetic cases matched, 1 a mismatch, 2 import failure.
"""
import importlib.util
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
FAILURES = []

sys.path.insert(0, str(HERE))
try:
    import run_editor_mutation as EDITOR
    import run_yanfs_mutation as SHARED
except Exception as error:  # a broken gate script is a hard failure, not a skip
    print("FAIL editor-mutation-gate: cannot import the mutation gate: %r"
          % error, file=sys.stderr)
    sys.exit(1)

classify_run = SHARED.classify_run
unity_suite = SHARED.unity_suite


def check(group, label, got, expected):
    if got != expected:
        FAILURES.append("%s/%s: got %r expected %r" % (group, label, got, expected))


# ---------------------------------------------------------- Unity synthesis

def record(index, name, status, message="message"):
    suffix = (": " + message) if status in ("FAIL", "IGNORE") else ""
    return "tests/test_editor.c:%d:%s:%s%s" % (index, name, status, suffix)


def footer(tests, failures, ignored):
    return "%d Tests %d Failures %d Ignored " % (tests, failures, ignored)


def consistent(records, terminal=None):
    failures = sum(1 for r in records if r[1] == "FAIL")
    ignored = sum(1 for r in records if r[1] == "IGNORE")
    lines = [record(index, r[0], r[1], r[2] if len(r) > 2 else "message")
             for index, r in enumerate(records, start=1)]
    lines.append("-----------------------")
    lines.append(footer(len(records), failures, ignored))
    lines.append(terminal if terminal is not None
                 else ("OK" if failures == 0 else "FAIL"))
    return ("\n".join(lines) + "\n").encode()


# ------------------------------------------------------ shared classifier

MARKER = "EDITOR-MARKER"

CASES = [
    ("exact detection", 1, consistent([("owner", "FAIL", MARKER)]), b"",
     "owner", MARKER, "owner-fail"),
    ("no-effect copy stays green", 0,
     consistent([("a", "PASS"), ("b", "PASS")]), b"", "owner", MARKER, "pass"),
    ("same-owner wrong assertion", 1,
     consistent([("owner", "FAIL", "an unrelated assertion")]), b"", "owner",
     MARKER, "wrong-assertion"),
    ("marker only on stderr does not count", 1,
     consistent([("owner", "FAIL", "an unrelated assertion")]),
     ("stderr says " + MARKER).encode(), "owner", MARKER, "wrong-assertion"),
    ("marker only on a PASS line does not count", 1,
     (record(1, "owner", "FAIL", "an unrelated assertion") + "\n"
      + "tests/test_editor.c:2:helper:PASS: " + MARKER + "\n"
      + "-----------------------\n2 Tests 1 Failures 0 Ignored \nFAIL\n"
      ).encode(), b"", "owner", MARKER, "wrong-assertion"),
    ("marker only on another owner is wrong owner", 1,
     consistent([("other", "FAIL", MARKER)]), b"", "owner", MARKER,
     "wrong-owner"),
    ("missing marker metadata is never a detection", 1,
     consistent([("owner", "FAIL", MARKER)]), b"", "owner", None, "harness"),
    ("empty stdout with exit 0", 0, b"", b"", "owner", MARKER, "harness"),
    ("bare FAIL without a footer", 1,
     (record(1, "owner", "FAIL", MARKER) + "\n").encode(), b"", "owner",
     MARKER, "harness"),
    ("duplicate footer", 0, consistent([("a", "PASS")]) + consistent([("a", "PASS")]),
     b"", "owner", MARKER, "harness"),
    ("test count mismatch", 0,
     (record(1, "a", "PASS") + "\n-----------------------\n"
      "3 Tests 0 Failures 0 Ignored \nOK\n").encode(), b"", "owner", MARKER,
     "harness"),
    ("exit 0 with an owner FAIL", 0,
     consistent([("owner", "FAIL", MARKER)]), b"", "owner", MARKER, "harness"),
    ("signal 139 with an owner FAIL", 139,
     consistent([("owner", "FAIL", MARKER)]), b"", "owner", MARKER, "harness"),
    ("signal 137 with an owner FAIL", 137,
     consistent([("owner", "FAIL", MARKER)]), b"", "owner", MARKER, "harness"),
    ("timeout", None, consistent([("owner", "FAIL", MARKER)]), b"", "owner",
     MARKER, "harness"),
    ("sanitizer overrides a correct owner", 1,
     consistent([("owner", "FAIL", MARKER)]), b"runtime error: boom", "owner",
     MARKER, "harness"),
    ("duplicate owner records are harness", 2,
     (record(1, "owner", "FAIL", MARKER) + "\n"
      + record(2, "owner", "FAIL", MARKER) + "\n"
      + "-----------------------\n2 Tests 2 Failures 0 Ignored \nFAIL\n"
      ).encode(), b"", "owner", MARKER, "harness"),
]


def pure_controls():
    for label, returncode, stdout, stderr, owner, assertion, expected in CASES:
        if label == "marker only on a PASS line does not count":
            check("fixture", "PASS record really contains its marker",
                  ("helper:PASS: " + MARKER).encode() in stdout, True)
        check("pure", label,
              classify_run(returncode, stdout, stderr, owner, assertion),
              expected)
    try:
        SHARED.self_test_run_suites()
    except Exception as error:  # noqa: BLE001 - a broken selftest is a failure
        FAILURES.append("shared-selftest: %s" % error)


# --------------------------------------------------- per-marker specificity

def marker_controls():
    for name, (owner, marker) in EDITOR.OWNING_ASSERTIONS.items():
        check("marker/%s" % name, "exact",
              classify_run(1, unity_suite([(owner, "FAIL", marker)]), b"",
                           owner, marker), "owner-fail")
        check("marker/%s" % name, "wrong-assertion",
              classify_run(1, unity_suite([(owner, "FAIL", "unrelated")]), b"",
                           owner, marker), "wrong-assertion")
        check("marker/%s" % name, "wrong-owner",
              classify_run(1, unity_suite([("another_owner", "FAIL", marker)]),
                           b"", owner, marker), "wrong-owner")
        check("marker/%s" % name, "missing-metadata",
              classify_run(1, unity_suite([(owner, "FAIL", marker)]), b"",
                           owner, None), "harness")


def marker_uniqueness():
    markers = [marker for _owner, marker in EDITOR.OWNING_ASSERTIONS.values()]
    if len(set(markers)) != len(markers):
        FAILURES.append("markers: duplicate marker strings")
    for first in markers:
        for second in markers:
            if first != second and first in second:
                FAILURES.append("markers: %r is a substring of %r"
                                % (first, second))


# ------------------------------------------------------- real CLI controls

def run_cli(tree, work, unity, cc):
    command = [sys.executable, "-B",
               str(tree / "tests" / "run_editor_mutation.py"),
               "--source", str(tree), "--work", str(work),
               "--cc", cc, "--unity", str(unity)]
    try:
        proc = subprocess.run(command, capture_output=True, timeout=180)
    except (subprocess.TimeoutExpired, OSError) as error:
        return None, repr(error).encode()
    return proc.returncode, proc.stdout + proc.stderr


def cli_missing_controls():
    """Run the real CLI on a temporary copy with one repository input removed:
    every removal must be a hard 1, and a missing external compiler or Unity
    must be 77. The original repository is never touched."""
    source = HERE.parent
    base = tempfile.mkdtemp(prefix="editor-gate-cli-")
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
        for rel in ("os/editor.c", "os/editor.h", "os/yanfs.c", "os/yanfs.h",
                    "os/shell.h", "tests/test_editor.c",
                    "tests/run_yanfs_mutation.py"):
            victim = tree / rel
            saved = victim.read_bytes()
            victim.unlink()
            rc, _blob = run_cli(tree, work, unity, placeholder_cc)
            victim.write_bytes(saved)
            check("cli", "missing-" + rel, rc, 1)
        rc, _blob = run_cli(tree, work, unity, absent_cc)
        check("cli", "missing-cc", rc, 77)
        empty_unity = Path(base) / "no-unity"
        empty_unity.mkdir()
        rc, _blob = run_cli(tree, work, empty_unity, placeholder_cc)
        check("cli", "missing-unity", rc, 77)
    except Exception as error:  # noqa: BLE001 - the control harness itself
        FAILURES.append("cli: control harness failed: %r" % error)
    finally:
        shutil.rmtree(base, ignore_errors=True)


def main():
    pure_controls()
    marker_controls()
    marker_uniqueness()
    cli_missing_controls()
    if FAILURES:
        for line in FAILURES:
            print("FAIL editor-mutation-gate: %s" % line, file=sys.stderr)
        print("editor-mutation-gate: %d control(s) disagreed" % len(FAILURES),
              file=sys.stderr)
        return 1
    print("PASS editor-mutation-gate: all synthetic controls agree")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:  # noqa: BLE001 - a control harness failure
        print("HARNESS-ERROR: editor-mutation-gate: %r" % error, file=sys.stderr)
        sys.exit(2)
