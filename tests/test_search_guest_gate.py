#!/usr/bin/env python3
"""Hard-input CLI guard for the 0025 Guest search acceptance scripts.

This exercises the real scripts through their command line in a minimal
temporary tree, never by calling their private functions. Removing any in-repo
input they declare (the search sources, the headers, the shared scripts, or the
scripts themselves) or the provided real ELF must produce exit 1 with the exact
missing-input message, even when the external host compiler is a placeholder.
With every input present the script must get past that check, which proves the
classifier is not merely mirroring a copied function.

Only Python is needed, so this gate is in the default suite.
"""

import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.dont_write_bytecode = True
sys.path.insert(0, str(HERE / "guest"))
import run_terminal_files as base  # noqa: E402
import run_editor as editor  # noqa: E402
import run_terminal_runtime as runtime  # noqa: E402
import run_search_api as api  # noqa: E402
import run_search_terms_api as terms_api  # noqa: E402
import run_search as healthy  # noqa: E402
import run_search_terms as terms
import run_search_terms_faults as terms_faults

HEALTHY_SCRIPT = HERE / "guest" / "run_search.py"
FAULT_SCRIPT = HERE / "guest" / "run_search_faults.py"

HEALTHY_INPUTS = list(editor.PRODUCTION_INPUTS) + [
    "tests/guest/run_search.py", "tests/guest/run_editor.py",
]
FAULT_INPUTS = list(editor.PRODUCTION_INPUTS) + [
    "tests/guest/run_search_faults.py",
    "tests/guest/run_terminal_runtime.py",
    "tests/guest/run_editor.py",
] + list(runtime.HOST_SOURCES) + list(runtime.HOST_HEADERS)


def check(name, condition, detail=""):
    if condition:
        print("PASS %s" % name, flush=True)
        return True
    print(":FAIL: %s: %s" % (name, detail), flush=True)
    return False


def build_tree(root, inputs):
    for rel in inputs:
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"")


def run_script(script, tree, work, extra):
    argv = [sys.executable, str(script), "--source", str(tree),
            "--work", str(work)] + extra
    return subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          timeout=120)


def main():
    ok = True
    # Use the production acceptance wrapper: Guest failures are semantic,
    # executor failures and sanitizer reports are harness errors.
    for name, rc, out, err, expected in (
        ("healthy", 0, b"", b"", 0),
        ("guest-failure", 6, b"", b"", 1),
        ("usage", 2, b"", b"", 2),
        ("load-error", 3, b"", b"", 2),
        ("step-limit", 4, b"", b"", 2),
        ("close-error", 5, b"", b"", 2),
        ("unexpected", 7, b"", b"", 2),
        ("signal", -11, b"", b"", 2),
        ("sanitizer-wins", 6, b"", b"AddressSanitizer", 2),
        ("undefined-behavior-wins", 0, b"runtime error:", b"", 2),
    ):
        result = subprocess.CompletedProcess([], rc, out, err)
        try:
            healthy.require_rc(result, 0, name)
            got = 0
        except healthy.HarnessError:
            got = 2
        except AssertionError:
            got = 1
        ok = check("healthy-classifier-" + name, got == expected,
                   "classification=%d expected=%d" % (got, expected)) and ok
    # Exercise the same classifier used for the real RV32 probe. A PASS marker
    # cannot hide a wrong tohost, crash, sanitizer or executor/signature mismatch.
    for name, rc, out, err, signature, expected in (
        ("healthy", 0, b"SEARCH_API_PASS", b"", b"\x01\0\0\0", 0),
        ("wrong-tohost-with-marker", 0, b"SEARCH_API_PASS", b"", b"\x02\0\0\0", 1),
        ("owner-code", 6, b"", b"", b"\x1e\0\0\0", 1),
        ("missing-marker", 0, b"", b"", b"\x01\0\0\0", 1),
        ("signal-wins", -11, b"SEARCH_API_PASS", b"", b"\x02\0\0\0", 2),
        ("sanitizer-wins", 6, b"", b"AddressSanitizer", b"\x02\0\0\0", 2),
        ("unexpected-exit", 7, b"SEARCH_API_PASS", b"", b"\x01\0\0\0", 2),
        ("missing-signature", 0, b"SEARCH_API_PASS", b"", b"", 2),
        ("rc-tohost-mismatch", 6, b"SEARCH_API_PASS", b"", b"\x01\0\0\0", 2),
    ):
        result = subprocess.CompletedProcess([], rc, out, err)
        got, detail = api.classify_probe(result, signature)
        ok = check("api-classifier-" + name, got == expected, detail) and ok
    # The 0026 term probe gets its own classifier controls: a PASS marker cannot
    # hide a wrong tohost, a crash, a sanitizer report or an executor/signature
    # mismatch.
    term_marker = terms_api.SIGNATURE
    for name, rc, out, err, signature, expected in (
        ("healthy", 0, term_marker, b"", b"\x01\0\0\0", 0),
        ("wrong-tohost-with-marker", 0, term_marker, b"", b"\x02\0\0\0", 1),
        ("owner-code", 6, b"", b"", b"\x3c\0\0\0", 1),
        ("missing-marker", 0, b"", b"", b"\x01\0\0\0", 1),
        ("signal-wins", -11, term_marker, b"", b"\x02\0\0\0", 2),
        ("sanitizer-wins", 6, b"", b"AddressSanitizer", b"\x02\0\0\0", 2),
        ("unexpected-exit", 7, term_marker, b"", b"\x01\0\0\0", 2),
        ("missing-signature", 0, term_marker, b"", b"", 2),
        ("rc-tohost-mismatch", 6, term_marker, b"", b"\x01\0\0\0", 2),
    ):
        result = subprocess.CompletedProcess([], rc, out, err)
        got, detail = terms_api.classify_probe(result, signature)
        ok = check("terms-api-classifier-" + name, got == expected, detail) and ok
    with tempfile.TemporaryDirectory(prefix="search-guest-gate-") as base_dir:
        base_path = Path(base_dir)
        # Windows reproduction of the CI-only failure: File identifiers are
        # case-insensitive there, so Path equality (and mkdir/read/write) treat
        # an alternative case spelling of the same directory as identical, but
        # the scripts canonicalize --source with Path.resolve() and print the
        # canonical spelling. The fixture is therefore built under an
        # alternative case spelling of the tempfile base, so the raw string the
        # gate would otherwise compare against differs from what the scripts
        # print. Path(name).swapcase() only changes case, never the directory
        # identity, so this cannot pick a different input.
        if sys.platform == "win32":
            base_path = Path(str(base_path).swapcase())
        tree = base_path / "tree"
        work = base_path / "work"
        work.mkdir(parents=True)
        build_tree(tree, sorted(set(HEALTHY_INPUTS + FAULT_INPUTS + api.INPUTS +
                                   list(terms_api.INPUTS) + list(terms.INPUTS) +
                                   list(terms_faults.INPUTS))))
        # The directory now exists: compare the canonical spelling the real
        # scripts resolve to, not the raw tempfile spelling. This is a string
        # comparison of the printed diagnostic, where case matters on Windows
        # even though pathlib path equality does not.
        tree = tree.resolve()
        fake_guest = base_path / "guest.elf"
        # An executable Python placeholder reliably fails the guest/mkfs argv
        # on Windows as well as Unix, exercising the real harness-error path.
        fake_run = Path(sys.executable)
        fake_mkfs = Path(sys.executable)
        fake_guest.write_bytes(b"guard-only ELF placeholder")

        # Positive control: every declared input exists, so the real CLI gets
        # past the hard-input check (a later harness error is acceptable).
        for label, script, extra in (
                ("healthy", HEALTHY_SCRIPT,
                 ["--run", str(fake_run), "--mkfs", str(fake_mkfs),
                  "--guest", str(fake_guest)]),
                ("faults", FAULT_SCRIPT,
                 ["--cc", sys.executable, "--guest", str(fake_guest)])):
            result = run_script(script, tree, work, extra)
            stdout = result.stdout.decode(errors="replace")
            ok = check("cli-positive-" + label,
                       result.returncode == 2 and ":HARNESS-ERROR:" in stdout,
                       stdout.strip()) and ok

        # Missing in-repo source and header controls for both scripts.
        for label, script, extra, victims in (
                ("healthy", HEALTHY_SCRIPT,
                 ["--run", str(fake_run), "--mkfs", str(fake_mkfs),
                  "--guest", str(fake_guest)],
                 ["os/search.c", "os/search_linear.c", "os/search.h",
                  "os/search_linear.h", "tests/guest/run_search.py",
                  "tests/guest/run_editor.py"]),
                ("faults", FAULT_SCRIPT,
                 ["--cc", sys.executable, "--guest", str(fake_guest)],
                 ["os/search.c", "os/search_linear.c", "os/search.h",
                  "os/search_linear.h", "tests/guest/run_search_faults.py",
                  "tests/guest/run_terminal_runtime.py"])):
            for rel in victims:
                victim = tree / rel
                saved = victim.read_bytes()
                victim.unlink()
                try:
                    result = run_script(script, tree, work, extra)
                finally:
                    victim.write_bytes(saved)
                expected = ":FAIL: missing required input: %s" % (tree / rel)
                stdout = result.stdout.decode(errors="replace")
                ok = check("cli-missing-%s-%s" % (label, rel),
                           result.returncode == 1 and expected in stdout,
                           "rc=%d stdout=%r" % (result.returncode,
                                                stdout.strip())) and ok

        # A missing provided ELF is a hard failure too, not a skip.
        result = run_script(HEALTHY_SCRIPT, tree, work,
                            ["--run", str(fake_run), "--mkfs", str(fake_mkfs),
                             "--guest", str(base_path / "absent.elf")])
        ok = check("cli-missing-guest",
                   result.returncode == 1 and b"guest ELF" in result.stdout,
                   "rc=%d stdout=%r" % (result.returncode,
                                        result.stdout[:200])) and ok

        api_script = HERE / "guest" / "run_search_api.py"
        absent_tool = str(base_path / "absent-tool")
        api_extra = ["--gcc", absent_tool, "--nm", absent_tool,
                     "--run", str(fake_run)]
        for rel in api.INPUTS:
            victim = tree / rel
            saved = victim.read_bytes()
            victim.unlink()
            try:
                result = run_script(api_script, tree, work, api_extra)
            finally:
                victim.write_bytes(saved)
            message = (":FAIL: missing required input: %s" % (tree / rel)).encode()
            ok = check("api-cli-missing-" + rel,
                       result.returncode == 1 and message in result.stdout,
                       "rc=%d" % result.returncode) and ok
        result = run_script(api_script, tree, work, api_extra)
        ok = check("api-cli-intact-missing-tool-77",
                   result.returncode == 77 and b"SKIP:" in result.stdout,
                   "rc=%d" % result.returncode) and ok

        # The 0026 term API probe declares its own inventory; every missing
        # in-repo input is a hard 1 even though the tools are absent.
        terms_api_script = HERE / "guest" / "run_search_terms_api.py"
        terms_api_extra = ["--gcc", absent_tool, "--nm", absent_tool,
                           "--run", str(fake_run)]
        for rel in sorted(set(terms_api.INPUTS)):
            victim = tree / rel
            saved = victim.read_bytes()
            victim.unlink()
            try:
                result = run_script(terms_api_script, tree, work, terms_api_extra)
            finally:
                victim.write_bytes(saved)
            message = (":FAIL: missing required input: %s" % (tree / rel)).encode()
            ok = check("terms-api-cli-missing-" + rel,
                       result.returncode == 1 and message in result.stdout,
                       "rc=%d" % result.returncode) and ok
        result = run_script(terms_api_script, tree, work, terms_api_extra)
        ok = check("terms-api-cli-intact-missing-tool-77",
                   result.returncode == 77 and b"SKIP:" in result.stdout,
                   "rc=%d" % result.returncode) and ok

        # Exercise every declared 0026 input via the real CLI. Even when the
        # compiler is missing, a missing repository source must be hard 1.
        term_cases = [
            ("terms", HERE / "guest/run_search_terms.py", terms.INPUTS,
             ["--run", str(fake_run), "--mkfs", str(fake_mkfs),
              "--guest", str(fake_guest)]),
            ("terms-faults", HERE / "guest/run_search_terms_faults.py",
             terms_faults.INPUTS,
             ["--cc", absent_tool, "--guest", str(fake_guest)]),
        ]
        for label, script, inputs, extra in term_cases:
            for rel in sorted(set(inputs)):
                victim = tree / rel
                saved = victim.read_bytes()
                victim.unlink()
                try:
                    result = run_script(script, tree, work, extra)
                finally:
                    victim.write_bytes(saved)
                message = (":FAIL: missing required input: %s" % victim).encode()
                ok = check("cli-missing-%s-%s" % (label, rel),
                           result.returncode == 1 and message in result.stdout,
                           "rc=%d" % result.returncode) and ok
        result = run_script(term_cases[1][1], tree, work, term_cases[1][3])
        ok = check("terms-fault-cli-intact-missing-compiler-77",
                   result.returncode == 77 and b"SKIP:" in result.stdout,
                   "rc=%d" % result.returncode) and ok
        result = run_script(term_cases[0][1], tree, work, term_cases[0][3])
        ok = check("terms-cli-intact-reaches-executor",
                   result.returncode == 2 and b":HARNESS-ERROR:" in result.stdout,
                   "rc=%d" % result.returncode) and ok

    print("search-guest-gate: %s" % ("ok" if ok else "failed"), flush=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
