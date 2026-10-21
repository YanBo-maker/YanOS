"""Local inputs must fail before an unavailable external compiler can skip."""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

SCRIPT = Path(__file__).with_name('run_large_capacity.py')
SOURCE = SCRIPT.parent.parent
spec = importlib.util.spec_from_file_location('large_capacity_gate_sut', SCRIPT)
driver = importlib.util.module_from_spec(spec)
spec.loader.exec_module(driver)

class InputGate(unittest.TestCase):
    def invoke(self, *, missing_source=False, missing_artifact=False):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / 'empty-source' if missing_source else SOURCE
            source.mkdir(exist_ok=True)
            values = {'source': source, 'work': root / 'work', 'cc': root / 'missing-compiler',
                      **{name: Path(__file__) for name in
                         ('mkfs', 'run', 'guest', 'semantic', 'mcp', 'semantic-runner', 'terms-probe')}}
            if missing_artifact:
                values['mkfs'] = root / 'missing-mkfs'
            args = [sys.executable, '-B', str(SCRIPT)]
            for key, value in values.items():
                args.extend(('--' + key, str(value)))
            return subprocess.run(args, capture_output=True, timeout=10)

    def test_missing_repository_input_is_failure(self):
        result = self.invoke(missing_source=True)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn(b'missing repository input', result.stdout)

    def test_missing_built_artifact_is_failure(self):
        result = self.invoke(missing_artifact=True)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn(b'missing explicit artifact', result.stdout)

    def test_only_external_compiler_absence_skips(self):
        result = self.invoke()
        self.assertEqual(result.returncode, 77, result.stderr)
        self.assertIn(b'host compiler unavailable', result.stdout)

    def test_validation_does_not_enumerate_unrelated_directories(self):
        with mock.patch.object(os, 'scandir', side_effect=AssertionError('unexpected scan')), \
             mock.patch.object(os, 'listdir', side_effect=AssertionError('unexpected listing')), \
             mock.patch.object(Path, 'rglob', side_effect=AssertionError('unexpected glob')):
            driver.require_inputs(SOURCE)

if __name__ == '__main__':
    unittest.main()
