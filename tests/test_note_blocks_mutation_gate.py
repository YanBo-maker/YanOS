"""Pure classifier/input controls, never an actual mutation-detection claim."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from run_note_blocks_mutation import REQUIRED, UNITY_FILES, copy_inputs
from run_note_metadata_mutation import classify_suites
from run_yanfs_mutation import unity_suite


class BlocksGate(unittest.TestCase):
    @staticmethod
    def run_case(records, rc=None, err=b''):
        failures = sum(row[1] == 'FAIL' for row in records)
        return failures if rc is None else rc, unity_suite(records), err

    def test_exact_suite_owner_assertion(self):
        run = self.run_case([('owner', 'FAIL', 'exact')])
        for suite in ('format', 'service', 'shell'):
            with self.subTest(suite=suite):
                self.assertEqual('owner-fail',
                    classify_suites({suite: run}, 'owner', 'exact', suite))

    def test_wrong_owner_and_same_owner_wrong_assertion(self):
        other = self.run_case([('other', 'FAIL', 'exact')])
        wrong = self.run_case([('owner', 'FAIL', 'different')])
        self.assertEqual('wrong-owner', classify_suites({'format': other}, 'owner', 'exact'))
        self.assertEqual('wrong-assertion', classify_suites({'format': wrong}, 'owner', 'exact'))

    def test_wrong_suite_and_missing_designated_suite(self):
        good = self.run_case([('owner', 'PASS')])
        bad = self.run_case([('owner', 'FAIL', 'exact')])
        self.assertEqual('wrong-suite', classify_suites(
            {'format': good, 'service': bad}, 'owner', 'exact'))
        self.assertEqual('harness', classify_suites({'service': bad}, 'owner', 'exact'))

    def test_harness_in_other_suite_wins(self):
        bad = self.run_case([('owner', 'FAIL', 'exact')])
        self.assertEqual('harness', classify_suites(
            {'format': bad, 'service': (1, b'', b'')}, 'owner', 'exact'))

    def test_signal_timeout_san_ignore_footer_and_fixture_abort(self):
        raw = unity_suite([('owner', 'FAIL', 'exact')])
        runs = [(None, b'', b''), (-11, raw, b''), (139, raw, b''),
                (1, raw, b'AddressSanitizer:'), (1, raw, b'runtime error:'),
                (0, raw, b''), (1, b'FAIL', b''),
                self.run_case([('owner', 'FAIL', 'exact'), ('other', 'IGNORE')]),
                (1, raw, b'fixture abort'),
                (1, raw.replace(b'1 Tests', b'2 Tests'), b'')]
        for run in runs:
            with self.subTest(run=run):
                self.assertEqual('harness', classify_suites({'format': run}, 'owner', 'exact'))

    def test_green_no_effect_is_not_detection(self):
        run = self.run_case([('owner', 'PASS')])
        self.assertEqual('pass', classify_suites({'format': run}, 'owner', 'exact'))

    def test_required_inputs_hard_fail_before_external_skip(self):
        self.assertIn('os/sort.h', REQUIRED)
        script = Path(__file__).with_name('run_note_blocks_mutation.py')
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            for name in REQUIRED:
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('placeholder')

            def invoke():
                return subprocess.run([sys.executable, '-B', str(script),
                    '--repo', str(root), '--unity', str(root / 'absent-unity'),
                    '--cc', 'absent-compiler-yanos', '--work', str(root / 'work')],
                    capture_output=True, timeout=20)

            self.assertEqual(77, invoke().returncode)
            for name in REQUIRED:
                with self.subTest(missing=name):
                    path = root / name
                    saved = path.read_bytes()
                    path.unlink()
                    result = invoke()
                    path.write_bytes(saved)
                    self.assertEqual(1, result.returncode)
                    self.assertIn(b'missing repository inputs', result.stdout)

    def test_fixture_copy_uses_only_manifest_without_directory_scans(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            repo = root / 'source'
            destination = root / 'copied'
            for name in REQUIRED:
                path = repo / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('declared:' + name)
            unrelated = repo / 'unrelated-dynamic-task' / 'do-not-read'
            unrelated.parent.mkdir(parents=True)
            unrelated.write_text('must stay outside fixture')
            with (patch.object(Path, 'glob', side_effect=AssertionError('no glob')),
                  patch.object(Path, 'rglob', side_effect=AssertionError('no rglob')),
                  patch.object(Path, 'iterdir', side_effect=AssertionError('no enumeration')),
                  patch('shutil.copytree', side_effect=AssertionError('no recursive copy'))):
                copy_inputs(repo, destination, REQUIRED)
            actual = {str(path.relative_to(destination)).replace('\\', '/')
                      for path in destination.rglob('*') if path.is_file()}
            self.assertEqual(set(REQUIRED), actual)
            self.assertFalse((destination / 'unrelated-dynamic-task').exists())

    def test_each_external_unity_input_or_compiler_absence_is_77(self):
        script = Path(__file__).with_name('run_note_blocks_mutation.py')
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            for name in REQUIRED:
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('placeholder')
            unity = root / 'external-unity'
            for name in UNITY_FILES:
                path = unity / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('placeholder')

            def invoke(compiler):
                return subprocess.run([sys.executable, '-B', str(script),
                    '--repo', str(root), '--unity', str(unity), '--cc', compiler,
                    '--work', str(root / 'work')], capture_output=True, timeout=20)

            self.assertEqual(77, invoke('absent-compiler-yanos').returncode)
            for name in UNITY_FILES:
                with self.subTest(external=name):
                    path = unity / name
                    saved = path.read_bytes()
                    path.unlink()
                    # A present executable makes only the absent Unity input
                    # decide this check; no compilation is attempted.
                    result = invoke(sys.executable)
                    path.write_bytes(saved)
                    self.assertEqual(77, result.returncode)
                    self.assertIn(b'compiler or Unity unavailable', result.stdout)


if __name__ == '__main__':
    unittest.main()
