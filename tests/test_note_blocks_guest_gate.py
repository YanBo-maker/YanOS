"""0032 verdict, oracle and missing-input controls without a cross compiler."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent / 'guest'))
import run_note_blocks as healthy
import run_note_blocks_api as api
import run_note_blocks_faults as faults
reference = healthy.reference


class BlockGuestGate(unittest.TestCase):
    def test_startup_code_is_known_owner_and_next_code_harness(self):
        runtime = faults.runtime
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'status'
            values = {name: '0' for name in runtime.STATUS_FIELDS}
            values.update(verdict='guest', tohost='0x71000017')
            path.write_text(''.join('%s=%s\n' % item for item in values.items()))
            result = subprocess.CompletedProcess([], 6, b'', b'')
            with self.assertRaises(runtime.OwnerFailure):
                runtime.classify_production(result, path, 'block-init', {1})
            self.assertEqual(0x71000017, runtime.classify_production(
                result, path, 'block-init', {0x71000017})['_tohost'])
            values['tohost'] = '0x71000018'
            path.write_text(''.join('%s=%s\n' % item for item in values.items()))
            with self.assertRaises(runtime.HarnessError):
                runtime.classify_production(result, path, 'unknown', {0x71000018})

    def test_rv32_marker_signature_and_harness_priority(self):
        for rc, out, err, code, expected in (
            (0, api.SIGNATURE, b'', 1, 0), (0, b'', b'', 1, 2),
            (6, api.SIGNATURE, b'', 19, 1), (-11, api.SIGNATURE, b'', 19, 2),
            (6, api.SIGNATURE, b'AddressSanitizer', 19, 2),
            (0, api.SIGNATURE, b'runtime error:', 1, 2),
            (6, api.SIGNATURE, b'', 1, 2), (4, api.SIGNATURE, b'', 19, 2),
            (None, api.SIGNATURE, b'', 19, 2), (0, api.SIGNATURE, b'', None, 2)):
            with self.subTest(rc=rc, code=code, err=err):
                signature = b'' if code is None else struct.pack('<I', code)
                result = subprocess.CompletedProcess([], rc, out, err)
                self.assertEqual(expected, api.classify_probe(result, signature)[0])

    def test_oracle_literal_ranges_empty_nested_and_text_priority(self):
        data = b'[[n#a]]\r\n@block a\r\n\xe4\xb8\xad\t\\\n@block empty'
        blocks, refs = reference.scan(data)
        self.assertEqual([dict(id='a', marker=2, first=3, last=3, start=19, end=25),
                          dict(id='empty', marker=4, first=0, last=0, start=37, end=37)], blocks)
        self.assertEqual([dict(name='n', id='a', source_id=None, line=1, start=0, end=7)], refs)
        self.assertEqual(b'3 \xe4\xb8\xad\\x09\\\\\r\nOK block show total=1 shown=1\r\n',
                         reference.query({'n': data}, b'block show n#a'))
        _, nested = reference.scan(b'[[[n#a]]] [[.#a]] [[n#a|alias]]')
        self.assertEqual([1], [ref['start'] for ref in nested])
        self.assertEqual(b'ERROR INVALID_TEXT\r\n', reference.query(
            {'n': b'@block a\n@block a\n\x00'}, b'block list n'))
        sixtyfour = b''.join(b'@block a%d\n' % i for i in range(64))
        self.assertEqual(b'ERROR AMBIGUOUS\r\n', reference.query(
            {'n': sixtyfour + b'@block a0\n'}, b'block list n'))
        self.assertEqual(b'ERROR LIMIT\r\n', reference.query(
            {'n': sixtyfour + b'@block a64\n@block a0\n'}, b'block list n'))

    def test_each_missing_declared_input_precedes_external_skip(self):
        for module, extra, complete in (
            (healthy, ['--run', 'run', '--mkfs', 'mkfs', '--guest', 'guest'], 2),
            (api, ['--run', 'run', '--gcc', 'absent-yanos-cross', '--nm', 'absent-yanos-nm'], 77),
            (faults, ['--guest', 'guest', '--cc', 'absent-yanos-host', '--gcc', 'absent-yanos-cross'], 77)):
            products = {'run', 'mkfs', 'guest'} if module is healthy else {'run'} if module is api else {'guest'}
            with tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                required = set(module.INPUTS) | products
                for name in required:
                    path = root / name
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(b'')
                # An irrelevant subtree is deliberately not a manifest input.
                unrelated = root / 'unrelated-dynamic-task' / 'not-an-input'
                unrelated.parent.mkdir(); unrelated.write_bytes(b'irrelevant')
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
                self.assertEqual(b'irrelevant', unrelated.read_bytes())


if __name__ == '__main__':
    unittest.main()
