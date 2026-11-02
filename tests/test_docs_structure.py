#!/usr/bin/env python3
"""Controls for tests/docs_structure.py (0034 R6-AC6.1, R9-AC9.6 row count).

Each control builds a tiny throw-away tree, runs the checker CLI on it, and
asserts the exit code, the marker, that output is ASCII, and that no file in the
tree changed. Scope is the two invariants: rows are counted, never resolved.
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

CHECKER = Path(__file__).with_name("docs_structure.py")
TIMEOUT = 60
HEADER = ("# 规格索引\n\n| 编号 | 标题 | 状态 | supersedes | 被谁取代 |\n"
          "| --- | --- | --- | --- | --- |\n")
MULTIBYTE = "中"  # three bytes in UTF-8, one character: why bytes are counted
LEGAL_ROWS = ["0002", "[0001](0001-a.md)", "[0003](0003-c.md)"]
CP1252 = {"PYTHONIOENCODING": "cp1252:strict"}


def build(root, spec_names, row_cells, status_bytes=b"# STATUS\n",
          index_bytes=None):
    specs = root / "docs" / "specs"
    specs.mkdir(parents=True, exist_ok=True)
    if status_bytes is not None:
        (root / "docs" / "STATUS.md").write_bytes(status_bytes)
    for name in spec_names:
        (specs / name).write_text("# %s\n" % name, encoding="utf-8")
    if index_bytes is None:
        cells = "".join("| %s | row | Implemented | none | none |\n" % cell
                        for cell in row_cells)
        index_bytes = (HEADER + cells).encode("utf-8")
    (specs / "README.md").write_bytes(index_bytes)
    return root


def legal(root):
    return build(root, ["0001-a.md", "0003-c.md"], LEGAL_ROWS)


def readme_excluded(root):
    # README.md is the index, and the scan stays one level deep.
    build(root, ["0001-a.md", "0003-c.md"], LEGAL_ROWS)
    nested = root / "docs" / "specs" / "archive"
    nested.mkdir()
    (nested / "9999-nested.md").write_text("# nested\n", encoding="utf-8")
    (root / "docs" / "specs" / "notes.txt").write_text("x\n", encoding="utf-8")
    return root


def boundary_exact(root):
    # 1 + 5461 * 3 == 16384: the limit is inclusive.
    return build(root, ["0001-a.md", "0003-c.md"], LEGAL_ROWS,
                 status_bytes=b"1" + (MULTIBYTE * 5461).encode("utf-8"))


def boundary_over(root):
    # 2 + 5461 * 3 == 16385: one byte past the limit.
    return build(root, ["0001-a.md", "0003-c.md"], LEGAL_ROWS,
                 status_bytes=b"22" + (MULTIBYTE * 5461).encode("utf-8"))


def new_spec_without_row(root):
    return build(root, ["0001-a.md", "0003-c.md", "0004-d.md"], LEGAL_ROWS)


def missing_row(root):
    return build(root, ["0001-a.md", "0003-c.md"],
                 ["0002", "[0001](0001-a.md)"])


def bare_0003_row(root):
    # Only the 0002 placeholder may appear bare; a bare 0003 is not a data row.
    return build(root, ["0001-a.md", "0003-c.md"],
                 ["0002", "[0001](0001-a.md)", "0003"])


def unlinked_0001_row(root):
    # `[0001]` carries no (target), so it is not the decided linked form.
    return build(root, ["0001-a.md", "0003-c.md"],
                 ["0002", "[0001]", "[0003](0003-c.md)"])


def missing_status(root):
    return build(root, ["0001-a.md", "0003-c.md"], LEGAL_ROWS,
                 status_bytes=None)


def missing_index(root):
    build(root, ["0001-a.md", "0003-c.md"], LEGAL_ROWS)
    (root / "docs" / "specs" / "README.md").unlink()
    return root


def missing_specs_dir(root):
    (root / "docs").mkdir(parents=True)
    (root / "docs" / "STATUS.md").write_bytes(b"# STATUS\n")
    return root


def empty_spec_inputs(root):
    return build(root, [], ["0002"])


def illegal_utf8_index(root):
    return build(root, ["0001-a.md", "0003-c.md"], LEGAL_ROWS,
                 index_bytes=b"| \xff\xfe 0001 | x |\n")


def unicode_missing_status(root):
    # Non-ASCII root path under a strict cp1252 console: diagnostics must stay
    # printable, which is why paths go through ascii().
    return build(root / "规格 根", ["0001-a.md"], ["0002"],
                 status_bytes=None)


# (name, builder, expected exit, required substrings, forbidden, env)
CASES = (
    ("legal_counts", legal, 0,
     ("0034 R6 AC6.1 PASS", "0034 R9 AC9.6 PASS", "PASS: 0034_docs_structure"),
     (":FAIL:", ":ERROR:", "SKIP"), None),
    ("readme_excluded_one_level", readme_excluded, 0,
     ("0034 R9 AC9.6 PASS", "PASS: 0034_docs_structure"),
     (":FAIL:", ":ERROR:", "SKIP"), None),
    ("multibyte_16384_exact", boundary_exact, 0,
     ("PASS: 0034_docs_structure", "16384"), (":FAIL:", ":ERROR:", "SKIP"),
     None),
    ("multibyte_16385_over", boundary_over, 1,
     (":FAIL: 0034_R6_AC6_1_status_bytes", "16385"),
     ("PASS: 0034_docs_structure", ":ERROR:", "SKIP"), None),
    ("new_spec_without_row", new_spec_without_row, 1,
     (":FAIL: 0034_R9_AC9_6_spec_index_rows",),
     ("PASS: 0034_docs_structure", ":ERROR:", "SKIP"), None),
    ("missing_row", missing_row, 1,
     (":FAIL: 0034_R9_AC9_6_spec_index_rows",),
     ("PASS: 0034_docs_structure", ":ERROR:", "SKIP"), None),
    ("bare_0003_row_ignored", bare_0003_row, 1,
     (":FAIL: 0034_R9_AC9_6_spec_index_rows", "has 2 data rows"),
     ("PASS: 0034_docs_structure", ":ERROR:", "SKIP"), None),
    ("unlinked_0001_row_ignored", unlinked_0001_row, 1,
     (":FAIL: 0034_R9_AC9_6_spec_index_rows", "has 2 data rows"),
     ("PASS: 0034_docs_structure", ":ERROR:", "SKIP"), None),
    ("missing_status", missing_status, 2, (":ERROR: 0034_docs_structure",),
     ("PASS", ":FAIL:", "SKIP"), None),
    ("missing_index", missing_index, 2, (":ERROR: 0034_docs_structure",),
     ("PASS", ":FAIL:", "SKIP"), None),
    ("missing_specs_dir", missing_specs_dir, 2, (":ERROR: 0034_docs_structure",),
     ("PASS", ":FAIL:", "SKIP"), None),
    ("empty_spec_inputs", empty_spec_inputs, 2,
     (":ERROR: 0034_docs_structure",), ("PASS", ":FAIL:", "SKIP"), None),
    ("illegal_utf8_index", illegal_utf8_index, 2,
     (":ERROR: 0034_docs_structure", "UTF-8"),
     ("PASS", ":FAIL:", "SKIP"), None),
    ("unicode_root_missing_status_cp1252", unicode_missing_status, 2,
     (":ERROR: 0034_docs_structure", "STATUS.md"),
     ("PASS", ":FAIL:", "SKIP"), CP1252),
)


def run_checker(root, env):
    full_env = dict(os.environ)
    full_env.update(env or {})
    proc = subprocess.run(
        [sys.executable, "-B", str(CHECKER), "--source", str(root)],
        capture_output=True, text=True, encoding="utf-8", errors="replace",
        timeout=TIMEOUT, env=full_env)
    return proc.returncode, proc.stdout + proc.stderr


def snapshot(root):
    """Relative paths with type and file bytes: an in-place rewrite shows up."""
    entries = []
    for path in sorted(root.rglob("*")):
        name = str(path.relative_to(root))
        entries.append((name, "dir") if path.is_dir()
                       else (name, "file", path.read_bytes()))
    return entries


def main():
    failures = 0
    for name, builder, expected_exit, required, forbidden, env in CASES:
        with tempfile.TemporaryDirectory() as tmp:
            root = builder(Path(tmp))
            before = snapshot(root)
            try:
                code, output = run_checker(root, env)
            except subprocess.TimeoutExpired:
                print(":CONTROL %s: FAIL timed out after %ds" % (name, TIMEOUT),
                      flush=True)
                failures += 1
                continue
            after = snapshot(root)
        problems = []
        if code != expected_exit:
            problems.append("exit %d, expected %d" % (code, expected_exit))
        for needle in required:
            if needle not in output:
                problems.append("missing marker %r" % needle)
        for needle in forbidden:
            if needle in output:
                problems.append("forbidden marker %r" % needle)
        if not output.isascii():
            problems.append("diagnostics are not ASCII")
        if before != after:
            problems.append("checker wrote into the tree")
        if problems:
            failures += 1
            print(":CONTROL %s: FAIL (%s)" % (name, "; ".join(problems)),
                  flush=True)
            for line in output.splitlines():
                print("    | " + line, flush=True)
        else:
            print(":CONTROL %s: ok exit %d" % (name, code), flush=True)
    if failures:
        print(":FAIL: 0034_docs_structure controls: %d of %d cases failed"
              % (failures, len(CASES)), flush=True)
        return 1
    print("PASS: 0034_docs_structure controls: %d cases hold" % len(CASES),
          flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
