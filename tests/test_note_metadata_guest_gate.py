"""0027 Guest required-input and verdict controls, independent of a toolchain."""
import unittest
import sys
from pathlib import Path
import tempfile
import subprocess
import struct
sys.dont_write_bytecode=True
sys.path.insert(0,str(Path(__file__).resolve().parent/'guest'))
import run_note_metadata as healthy
import run_note_metadata_api as api
import run_note_metadata_faults as faults
class GuestGate(unittest.TestCase):
    def test_rv32_dual_signature_and_harness_priority(self):
        marker=api.SIGNATURE
        cases=[(0,marker,b'',1,0),(0,b'',b'',1,2),(6,marker,b'',7,1),
               (-11,marker,b'',7,2),(6,marker,b'AddressSanitizer',7,2),
               (0,marker,b'runtime error:',1,2),(6,marker,b'',1,2),
               (4,marker,b'',7,2),(0,marker,b'',None,2)]
        for rc,out,err,value,expected in cases:
            with self.subTest(rc=rc,out=out,value=value):
                signature=struct.pack('<I',value) if value is not None else b''
                self.assertEqual(expected,api.classify_probe(subprocess.CompletedProcess([],rc,out,err),signature)[0])
    def test_required_inputs_missing_is_hard_failure(self):
        here=Path(__file__).resolve().parent/'guest'
        for module,extra,expected in (
            (healthy,['--run','run','--mkfs','mkfs','--guest','guest'],2),
            (api,['--run','run','--gcc','absent-cross-yanos','--nm','absent-nm-yanos'],77),
            (faults,['--guest','guest','--cc','absent-host-yanos'],77)):
            with tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp)
                for name in set(module.INPUTS)|{'run','mkfs','guest'}:
                    p=root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(b'')
                flags=list(extra)
                for i,value in enumerate(flags):
                    if value in ('run','mkfs','guest'): flags[i]=str(root/value)
                argv=[sys.executable,'-B',str(here/Path(module.__file__).name),'--source',str(root),'--work',str(root/'work'),*flags]
                result=subprocess.run(argv,capture_output=True,timeout=20)
                self.assertEqual(expected,result.returncode,(module.__name__,result.stdout,result.stderr))
                for name in set(module.INPUTS)|({'run','mkfs','guest'} if module is healthy else {'run'} if module is api else {'guest'}):
                    with self.subTest(script=module.__name__,missing=name):
                        p=root/name;p.unlink()
                        result=subprocess.run(argv,capture_output=True,timeout=20)
                        p.write_bytes(b'')
                        self.assertEqual(1,result.returncode,(name,result.stdout,result.stderr))
                        self.assertIn(b'missing required input',result.stdout)
                        self.assertIn(str(p).encode(),result.stdout)
if __name__=='__main__':unittest.main()
