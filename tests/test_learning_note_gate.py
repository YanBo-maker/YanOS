"""0035-R8 learning-note negative controls; pure Python, no C toolchain.

The gate imports ``run_learning_note`` without importing its lazy MCP/identity
helpers, so it stays importable where ``fcntl`` is unavailable.  It proves that
wrong expected values and mutated preview responses are rejected, that every
missing local input exits 1 with the required message before any external work,
and that a named fixture copy neither scans nor copies an unrelated directory.
This is not mutation testing.
"""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE / "guest"))

import gate_fixture
import run_learning_note as runner


class LearningNoteGate(unittest.TestCase):
    def test_assert_exact_rejects_wrong_expected_value(self):
        with self.assertRaises(AssertionError):
            runner.assert_exact(b"preview text", b"mutated text", "preview")
        self.assertEqual(b"same", runner.assert_exact(b"same", b"same"))

    def test_expected_diff_matches_spec_line_rule(self):
        cases = (
            (b"x\ny\nz\n", b"x\nY\nz\n",
             {"start_line": 2, "removed": ["y\n"], "added": ["Y\n"]}),
            (b"a", b"a\n",
             {"start_line": 1, "removed": ["a"], "added": ["a\n"]}),
            (b"", b"q\n",
             {"start_line": 1, "removed": [], "added": ["q\n"]}),
            (b"same\n", b"same\n",
             {"start_line": 0, "removed": [], "added": []}),
        )
        for old, new, expected in cases:
            with self.subTest(old=old, new=new):
                self.assertEqual(expected, runner.expected_diff(old, new))
        # The checked-in fixtures are expected to exercise the same rule.
        initial = (HERE / "learning-note" / "learning-note.md").read_bytes()
        updated = (HERE / "learning-note" /
                   "learning-note-updated.md").read_bytes()
        self.assertEqual(9, runner.expected_diff(initial, updated)["start_line"])

    def test_check_preview_rejects_mutated_mcp_response(self):
        text = "@block a\nhello\n"
        diff = runner.expected_diff(b"", text.encode("utf-8"))
        source = "0" * 64
        good = {
            "schema_version": 1,
            "operation": "note_create_preview",
            "name": "a.txt",
            "source_sha256": source,
            "text": text,
            "diff": diff,
        }
        runner.check_preview(good, "note_create_preview", "a.txt", text,
                             source, diff)
        mutations = (
            ("text", "@block a\nbye\n"),
            ("source_sha256", "1" * 64),
            ("diff", {"start_line": 1, "removed": [], "added": []}),
            ("operation", "note_replace_preview"),
            ("name", "other.txt"),
            ("schema_version", 2),
            ("unexpected", True),
        )
        for key, value in mutations:
            with self.subTest(field=key):
                bad = dict(good)
                bad[key] = value
                with self.assertRaises(AssertionError):
                    runner.check_preview(bad, "note_create_preview", "a.txt",
                                         text, source, diff)

        commit = {
            "schema_version": 1,
            "operation": "note_create",
            "name": "a.txt",
            "bytes": len(text.encode("utf-8")),
            "source_sha256": source,
            "previous_source_sha256": None,
        }
        runner.check_commit(commit, "note_create", "a.txt",
                            len(text.encode("utf-8")), source, None)
        bad = dict(commit)
        bad["previous_source_sha256"] = "2" * 64
        with self.assertRaises(AssertionError):
            runner.check_commit(bad, "note_create", "a.txt",
                                len(text.encode("utf-8")), source, None)

    def _source_tree(self, root):
        source = root / "source"
        gate_fixture.copy_named_inputs(REPO, source, runner.INPUTS)
        products = {}
        for name in ("run", "guest", "server"):
            path = root / "products" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"")
            products[name] = path
        return source, products

    @staticmethod
    def _argv(source, products, work, include_server=True):
        argv = [sys.executable, "-B", str(runner.__file__),
                "--source", str(source), "--work", str(work),
                "--run", str(products["run"]),
                "--guest", str(products["guest"])]
        if include_server:
            argv += ["--server", str(products["server"])]
        return argv

    def test_missing_inputs_exit_one_before_work_or_toolchain(self):
        with tempfile.TemporaryDirectory(prefix="learning-gate-") as temporary:
            root = Path(temporary)
            source, products = self._source_tree(root)
            unrelated = source / "unrelated-dynamic-task" / "not-an-input"
            unrelated.parent.mkdir()
            unrelated.write_bytes(b"unrelated data")
            work = root / "work"

            # Every declared source-relative input, including the runner itself.
            for relative in runner.INPUTS:
                path = source / relative
                original = path.read_bytes()
                path.unlink()
                try:
                    result = subprocess.run(
                        self._argv(source, products, work),
                        capture_output=True, timeout=30)
                finally:
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(original)
                self.assertEqual(1, result.returncode,
                                 (relative, result.stdout, result.stderr))
                self.assertNotEqual(77, result.returncode)
                self.assertIn(b"missing required input", result.stdout)
                self.assertIn(str(path).encode(), result.stdout)
                self.assertFalse(work.exists(), relative)
                self.assertEqual(b"unrelated data", unrelated.read_bytes())

            # A declared product missing, including the explicit Guest ELF.
            for name in ("run", "guest", "server"):
                path = products[name]
                original = path.read_bytes()
                path.unlink()
                try:
                    result = subprocess.run(
                        self._argv(source, products, work),
                        capture_output=True, timeout=30)
                finally:
                    path.write_bytes(original)
                self.assertEqual(1, result.returncode,
                                 (name, result.stdout, result.stderr))
                self.assertNotEqual(77, result.returncode)
                self.assertIn(b"missing required input", result.stdout)
                self.assertIn(str(path).encode(), result.stdout)
                self.assertFalse(work.exists(), name)
                self.assertEqual(b"unrelated data", unrelated.read_bytes())

            # Omitting --server is the same hard failure, not an argparse exit 2.
            result = subprocess.run(
                self._argv(source, products, work, include_server=False),
                capture_output=True, timeout=30)
            self.assertEqual(1, result.returncode,
                             (result.stdout, result.stderr))
            self.assertIn(b"missing required input", result.stdout)
            self.assertIn(b"--server", result.stdout)
            self.assertFalse(work.exists())
            self.assertEqual(b"unrelated data", unrelated.read_bytes())

    def test_require_inputs_does_not_scan_unrelated_directory(self):
        with tempfile.TemporaryDirectory(prefix="learning-gate-") as temporary:
            root = Path(temporary)
            source, products = self._source_tree(root)
            unrelated = source / "unrelated-dynamic-task" / "not-an-input"
            unrelated.parent.mkdir()
            unrelated.write_bytes(b"unrelated data")
            with patch.object(os, "scandir",
                              side_effect=AssertionError(
                                  "required-input validation scanned a directory")):
                missing = runner.require_inputs(
                    source, products["run"], products["guest"],
                    products["server"])
            self.assertEqual([], missing)
            self.assertEqual(b"unrelated data", unrelated.read_bytes())

    def test_named_fixture_copy_skips_unrelated_directory(self):
        with tempfile.TemporaryDirectory(prefix="learning-gate-") as temporary:
            root = Path(temporary)
            source = root / "source"
            (source / "needed").mkdir(parents=True)
            (source / "needed" / "input").write_bytes(b"needed")
            unrelated = source / "unrelated-dynamic-task" / "not-an-input"
            unrelated.parent.mkdir()
            unrelated.write_bytes(b"unrelated data")
            target = root / "target"
            with patch.object(os, "scandir",
                              side_effect=AssertionError(
                                  "named copy scanned a directory")):
                gate_fixture.copy_named_inputs(
                    source, target, ["needed/input"])
            self.assertEqual(b"needed",
                             (target / "needed" / "input").read_bytes())
            self.assertFalse((target / "unrelated-dynamic-task").exists())
            self.assertEqual(b"unrelated data", unrelated.read_bytes())


if __name__ == "__main__":
    unittest.main()
