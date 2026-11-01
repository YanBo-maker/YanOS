#!/usr/bin/env python3
"""Controls for layering_gate.py (0017: os/ must not include tests/ headers).

Each case runs the real CLI against a throw-away tree, so no fixture ever lands
in os/. A case asserts three things: the exact exit code, the expected
diagnostic category, and that no PASS line is printed when the run failed. The
child renders stdout as strict cp1252, so non-ASCII output raises instead of
passing on a tolerant terminal.

The checker is resolved as a sibling file, so this suite runs both beside the
gate and after it is installed in tests/.

The real-tree case is skipped when the sibling layout has no os/ (the artifact
folder); it runs after transfer, and the artifact run is covered separately with
an explicit --source.
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

GATE = Path(__file__).with_name("layering_gate.py")
HIT = ":FAIL: 0017_os_must_not_include_tests:"
INPUT = ":FAIL: 0017_layering_scan_input:"
PASS = "0017 PASS:"


def run(root):
    env = dict(os.environ, PYTHONIOENCODING="cp1252:strict")
    return subprocess.run([sys.executable, "-B", str(GATE),
                           "--source", str(root)], capture_output=True, env=env)


def judge(label, rc, want, out, extra=""):
    ok = rc == want and out.isascii() and not extra
    print("%s %s: exit=%d expected=%d" % ("PASS" if ok else "FAIL", label,
                                          rc, want), flush=True)
    if not ok:
        print("  ascii=%s %s" % (out.isascii(), extra), flush=True)
        for line in out.splitlines():
            print("  | " + line, flush=True)
    return ok


def expect(label, files, want_rc, markers=(), absent=()):
    with tempfile.TemporaryDirectory(prefix="layering-ctl-") as tmp:
        root = Path(tmp)
        for name, body in files.items():
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(body, encoding="utf-8")
        result = run(root)
    out = (result.stdout + result.stderr).decode("ascii", "replace")
    bad = ["missing:" + m for m in markers if m not in out]
    bad += ["present:" + a for a in absent if a in out]
    return judge(label, result.returncode, want_rc, out, " ".join(bad))


def read_fault():
    """A supported source that cannot be read fails the input category.

    The fault is installed at the filesystem boundary (Path.read_text) inside a
    child process; no classifier logic or checker result is replaced.
    """
    script = (
        "import runpy, sys, pathlib\n"
        "root = sys.argv[1]\n"
        "real = pathlib.Path.read_text\n"
        "def guarded(self, *a, **k):\n"
        "    if self.name == 'broken.c':\n"
        "        raise OSError(13, 'simulated read failure')\n"
        "    return real(self, *a, **k)\n"
        "pathlib.Path.read_text = guarded\n"
        "sys.argv = [%r, '--source', root]\n"
        "runpy.run_path(%r, run_name='__main__')\n" % (str(GATE), str(GATE)))
    with tempfile.TemporaryDirectory(prefix="layering-ctl-") as tmp:
        root = Path(tmp)
        (root / "os").mkdir()
        (root / "os" / "broken.c").write_text('#include "own.h"\n', encoding="utf-8")
        result = subprocess.run([sys.executable, "-B", "-c", script, str(root)],
                                capture_output=True,
                                env=dict(os.environ, PYTHONIOENCODING="cp1252:strict"))
    out = (result.stdout + result.stderr).decode("ascii", "replace")
    bad = ""
    if INPUT not in out:
        bad = "missing:" + INPUT
    elif PASS in out:
        bad = "printed PASS despite an unreadable source"
    return judge("unreadable-source-is-input-failure", result.returncode, 2, out, bad)


CASES = [
    # Allowed: os/ own headers, and the reverse tests -> os direction.
    ("allowed-own-and-reverse", {
        "os/console.c": '#include "console.h"\n#include <os/task.h>\n',
        "tests/test_console.c": '#include "../os/console.h"\n',
    }, 0, (PASS,), (HIT,)),
    # Forbidden spellings, including a middle relative segment and a backslash
    # path that a prefix-only or POSIX-only test would miss.
    ("forbidden-quoted", {"os/a.c": '#include "tests/x.h"\n'}, 1, (HIT, "os/a.c:1")),
    ("forbidden-angle", {"os/b.c": '#include <tests/x.h>\n'}, 1, (HIT, "os/b.c:1")),
    ("forbidden-parent", {"os/c.c": '#include "../tests/x.h"\n'}, 1, (HIT, "os/c.c:1")),
    ("forbidden-dotslash", {"os/d.c": '#include "./tests/x.h"\n'}, 1, (HIT, "os/d.c:1")),
    ("forbidden-middle-segment", {"os/e.c": '#include "sub/../../tests/x.h"\n'},
     1, (HIT, "os/e.c:1")),
    ("forbidden-backslash", {"os/f.c": '#include "..\\\\tests\\\\x.h"\n'},
     1, (HIT, "os/f.c:1")),
    # Forbidden with a comment inside the directive: all three are valid C.
    ("comment-between-hash-and-include", {"os/g.c": '#/**/include "tests/x.h"\n'},
     1, (HIT, "os/g.c:1")),
    ("comment-before-operand", {"os/h.c": '#include/**/<tests/x.h>\n'},
     1, (HIT, "os/h.c:1")),
    ("leading-comment", {"os/i.c": '/* lead */ #include "../tests/x.h"\n'},
     1, (HIT, "os/i.c:1")),
    # Comment and string look-alikes are text, not directives.
    ("line-comment-lookalike", {"os/j.c": '// #include "tests/x.h"\n#include "own.h"\n'},
     0, (PASS,), (HIT,)),
    ("block-comment-lookalike", {
        "os/k.c": '/*\n#include "tests/x.h"\n*/\n#include "own.h"\n',
    }, 0, (PASS,), (HIT,)),
    ("string-contains-comment-open", {
        "os/l.c": 'static const char *s = "/* no";\n#include "own.h"\n',
    }, 0, (PASS,), (HIT,)),
    ("string-with-line-comment-marker", {
        "os/m.c": 'static const char *s = "// #include \\"tests/x.h\\"";\n#include "own.h"\n',
    }, 0, (PASS,), (HIT,)),
    # A string holding an unterminated-looking comment must not swallow the
    # following real directive.
    ("string-then-forbidden", {
        "os/n.c": 'static const char *s = "/*";\n#include "tests/x.h"\n',
    }, 1, (HIT, "os/n.c:2")),
    # Recursion into os/ subdirectories, both directions.
    ("recursive-subdirectory", {"os/ui/deep/p.c": '#include "../../tests/x.h"\n'},
     1, (HIT, "os/ui/deep/p.c:1")),
    ("recursive-allowed", {"os/ui/q.c": '#include "../console.h"\n'}, 0, (PASS,), (HIT,)),
    # Suffix coverage.
    ("lowercase-assembly", {"os/r.s": '#include "tests/x.h"\n'}, 1, (HIT, "os/r.s:1")),
    ("uppercase-assembly", {"os/t.S": '#include "tests/x.h"\n'}, 1, (HIT, "os/t.S:1")),
    # A non-ASCII operand is reported, but escaped, so cp1252 stdout survives.
    ("unicode-operand-ascii-output", {"os/s.c": '#include "../tests/\u77e5\u8bc6.h"\n'},
     1, (HIT, "os/s.c:1", "\\u77e5")),
    ("unicode-filename-ascii-output", {"os/\u77e5.c": '#include "../tests/x.h"\n'},
     1, (HIT, "os/\\u77e5.c:1")),
    # Missing and empty scan input fail in their own category, never as PASS.
    ("missing-os-directory", {"tests/test_console.c": '#include "../os/x.h"\n'},
     2, (INPUT,), (PASS, HIT)),
    ("empty-os-directory", {"os/notes.txt": "no sources\n"}, 2, (INPUT,), (PASS, HIT)),
]


def real_tree():
    root = GATE.parent.parent
    if not (root / "os").is_dir():
        print("SKIP real-tree-0017: no os/ beside %s" % GATE.parent, flush=True)
        return True
    result = run(root)
    out = (result.stdout + result.stderr).decode("ascii", "replace")
    bad = "" if (PASS in out and HIT not in out) else "real tree did not pass cleanly"
    return judge("real-tree-0017", result.returncode, 0, out, bad)


def main():
    results = [expect(*case) for case in CASES]
    results.append(read_fault())
    results.append(real_tree())
    print("%s layering gate controls: %d/%d"
          % ("PASS" if all(results) else "FAIL", sum(results), len(results)),
          flush=True)
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
