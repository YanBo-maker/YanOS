"""Guard the relation Guest verdict and mandatory input rules without a cross compiler."""
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent / 'guest'))
import run_note_relations as healthy
import run_note_relations_api as api
import run_note_relations_faults as faults


class RelationGuestGate(unittest.TestCase):
    def test_relation_startup_failure_is_known_owner_not_harness(self):
        runtime = faults.runtime
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'status'
            values = {name: '0' for name in runtime.STATUS_FIELDS}
            values.update(verdict='guest', tohost='0x71000016')
            path.write_text(''.join('%s=%s\n' % item for item in values.items()))
            result = subprocess.CompletedProcess([], 6, b'', b'')
            with self.assertRaises(runtime.OwnerFailure):
                runtime.classify_production(result, path, 'relation-startup', {1})
            accepted = runtime.classify_production(result, path, 'relation-startup', {0x71000016})
            self.assertEqual(0x71000016, accepted['_tohost'])
            values['tohost'] = '0x7100001a'
            path.write_text(''.join('%s=%s\n' % item for item in values.items()))
            with self.assertRaises(runtime.HarnessError):
                runtime.classify_production(result, path, 'unknown-startup', {0x7100001a})

    def test_rv32_requires_both_signatures_and_prioritizes_harness(self):
        for rc, out, err, code, expected in (
            (0, api.SIGNATURE, b'', 1, 0),
            (0, b'', b'', 1, 2),
            (6, api.SIGNATURE, b'', 19, 1),
            (-11, api.SIGNATURE, b'', 19, 2),
            (6, api.SIGNATURE, b'AddressSanitizer', 19, 2),
            (0, api.SIGNATURE, b'runtime error:', 1, 2),
            (6, api.SIGNATURE, b'', 1, 2),
            (4, api.SIGNATURE, b'', 19, 2),
            (0, api.SIGNATURE, b'', None, 2),
        ):
            with self.subTest(rc=rc, code=code, err=err):
                signature = b'' if code is None else struct.pack('<I', code)
                result = subprocess.CompletedProcess([], rc, out, err)
                self.assertEqual(expected, api.classify_probe(result, signature)[0])

    def test_missing_repo_inputs_fail_before_external_tool_skip(self):
        for module, extra, complete in (
            (healthy, ['--run', 'run', '--mkfs', 'mkfs', '--guest', 'guest'], 2),
            (api, ['--run', 'run', '--gcc', 'absent-yanos-cross', '--nm', 'absent-yanos-nm'], 77),
            (faults, ['--guest', 'guest', '--cc', 'absent-yanos-host'], 77),
        ):
            products = {'run', 'mkfs', 'guest'} if module is healthy else {'run'} if module is api else {'guest'}
            with tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                required = set(module.INPUTS) | products
                for name in required:
                    path = root / name
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(b'')
                flags = [str(root / item) if item in products else item for item in extra]
                argv = [sys.executable, '-B', module.__file__, '--source', str(root),
                        '--work', str(root / 'work'), *flags]
                result = subprocess.run(argv, capture_output=True, timeout=20)
                self.assertEqual(complete, result.returncode, (result.stdout, result.stderr))
                for name in sorted(required):
                    with self.subTest(script=module.__name__, missing=name):
                        path = root / name
                        path.unlink()
                        result = subprocess.run(argv, capture_output=True, timeout=20)
                        path.write_bytes(b'')
                        self.assertEqual(1, result.returncode, (name, result.stdout, result.stderr))
                        self.assertIn(b'missing required input', result.stdout)
                        self.assertIn(str(path).encode(), result.stdout)


if __name__ == '__main__':
    unittest.main()
