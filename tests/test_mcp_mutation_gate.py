"""Classifier and missing-input controls; these are not actual detections."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest
from run_mcp_mutation import REQUIRED, BUILD_REQUIRED
from run_note_metadata_mutation import classify_suites
from run_yanfs_mutation import unity_suite
from mcp_fault_checks import Records, format_records, owner_assertion_failed
from mcp_checks import Wire


class Gate(unittest.TestCase):
    def classify(self, runs, owner='owner', assertion='exact'):
        return classify_suites(runs, owner, assertion, 'mcp_faults')

    def test_exact_and_wrong_suite_owner_assertion(self):
        red = (1, unity_suite([('owner', 'FAIL', 'exact')]), b'')
        green = (0, unity_suite([('owner', 'PASS')]), b'')
        self.assertEqual('owner-fail', self.classify({'mcp_faults': red, 'mcp_protocol': green}))
        self.assertEqual('wrong-suite', self.classify({'mcp_faults': green, 'mcp_protocol': red}))
        self.assertEqual('wrong-owner', self.classify({'mcp_faults': red}, owner='other'))
        self.assertEqual('wrong-assertion', self.classify({'mcp_faults': red}, assertion='other'))
        self.assertEqual('pass', self.classify({'mcp_faults': green}))

    def test_harness_wins_even_with_designated_assertion(self):
        raw = unity_suite([('owner', 'FAIL', 'exact')])
        for bad in [(None,b'',b''),(-9,raw,b''),(139,raw,b''),(2,b'',b''),
                    (1,raw,b'AddressSanitizer: error'),(1,raw,b'runtime error:'),
                    (0,raw,b''),(1,b'FAIL',b''),(1,unity_suite([('owner','IGNORE')]),b'')]:
            with self.subTest(bad=bad):
                self.assertEqual('harness', self.classify({'mcp_faults': (1,raw,b''), 'mcp_protocol': bad}))

    def test_python_owner_assertions_and_helper_errors_are_distinct(self):
        class AssertCase(unittest.TestCase):
            def test_owner(self):
                self.assertTrue(False, 'exact')
        result=Records(); unittest.defaultTestLoader.loadTestsFromTestCase(AssertCase).run(result)
        self.assertFalse(result.harness)
        self.assertEqual(('test_owner','FAIL'),result.records[0][:2])
        self.assertEqual('owner-fail',self.classify({'mcp_faults': (1,format_records(result).encode(),b'')},
                                                  owner='test_owner'))
        class ErrorCase(unittest.TestCase):
            def test_owner(self):
                raise RuntimeError('fixture abort')
        result=Records(); unittest.defaultTestLoader.loadTestsFromTestCase(ErrorCase).run(result)
        self.assertTrue(result.harness)
        self.assertFalse(result.records)
        class HelperCase(unittest.TestCase):
            def test_owner(self):
                def helper():
                    assert False, 'exact'
                helper()
        result=Records(); unittest.defaultTestLoader.loadTestsFromTestCase(HelperCase).run(result)
        self.assertTrue(result.harness)

    def test_required_inputs_hard_fail_before_external_skip(self):
        script=Path(__file__).with_name('run_mcp_mutation.py')
        with tempfile.TemporaryDirectory() as raw:
            root=Path(raw); build=root/'build'
            for name in REQUIRED:
                p=root/name; p.parent.mkdir(parents=True,exist_ok=True); p.write_text('placeholder')
            for name in BUILD_REQUIRED:
                p=build/name; p.parent.mkdir(parents=True,exist_ok=True); p.write_text('placeholder')
            def invoke():
                return subprocess.run([sys.executable,'-B',str(script),'--repo',str(root),'--build',str(build),
                    '--unity',str(root/'absent-unity'),'--yyjson-include',str(root/'absent-json'),
                    '--yyjson-lib',str(root/'absent-json.a'),'--cc','absent-yan-compiler','--work',str(root/'work')],
                    capture_output=True,timeout=20)
            self.assertEqual(77,invoke().returncode)
            (root/'tools/yan_mcp.c').unlink()
            self.assertEqual(1,invoke().returncode)
            (root/'tools/yan_mcp.c').write_text('placeholder')
            (build/'yan_mcp').unlink()
            self.assertEqual(1,invoke().returncode)

    def test_real_stderr_bridge_keeps_sanitizer_harness_priority(self):
        for diagnostic in ('AddressSanitizer: deliberate-control\n','runtime error: deliberate-control\n','ERROR IO\n'):
            class Bridge(unittest.TestCase):
                def setUp(self):
                    code='import sys; sys.stderr.write('+repr(diagnostic)+'); sys.exit(5)'
                    self.wire=Wire([sys.executable,'-c',code],dict(os.environ))
                def test_owner(self):
                    self.assertTrue(False,'exact_owner_assertion')
                def tearDown(self):
                    self.wire.close(rc=None,allow_extra=True)
            result=Records(); unittest.defaultTestLoader.loadTestsFromTestCase(Bridge).run(result)
            expected=diagnostic!='ERROR IO\n'
            self.assertEqual(expected,bool(result.harness),'real bridge sanitizer classification')
            ran=(2,b'',str(result.harness).encode()) if result.harness else (len(result.failures),format_records(result).encode(),b'')
            self.assertEqual('harness' if expected else 'owner-fail',
                self.classify({'mcp_faults':ran},owner='test_owner',assertion='exact_owner_assertion'))

    def test_direct_owner_flag_allows_defined_fatal_cleanup_only(self):
        class Bridge(unittest.TestCase):
            def test_owner(self):
                self.assertTrue(False,'exact_owner_assertion')
            def tearDown(self):
                wire=Wire([sys.executable,'-c','import sys; sys.exit(5)'],dict(os.environ))
                wire.close(rc=None if owner_assertion_failed(self) else 0,allow_extra=True)
        result=Records(); unittest.defaultTestLoader.loadTestsFromTestCase(Bridge).run(result)
        self.assertFalse(result.harness)
        self.assertEqual('owner-fail',self.classify({'mcp_faults':(1,format_records(result).encode(),b'')},
            owner='test_owner',assertion='exact_owner_assertion'))


if __name__=='__main__':
    unittest.main()
