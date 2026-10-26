#!/usr/bin/env python3
"""0040 full current-tree correctness/performance matrix with durable evidence."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import yanfs_4095_performance as oracle
import run_terminal_runtime as runtime


def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def execute(work,name,argv,timeout=600):
    (work/(name+'.argv.json')).write_text(json.dumps(argv,indent=2)+'\n')
    try: result=subprocess.run(argv,capture_output=True,timeout=timeout)
    except subprocess.TimeoutExpired as error:
        (work/(name+'.stdout')).write_bytes(error.stdout or b'')
        (work/(name+'.stderr')).write_bytes(error.stderr or b'')
        raise runtime.HarnessError(name+': timeout') from error
    (work/(name+'.stdout')).write_bytes(result.stdout);(work/(name+'.stderr')).write_bytes(result.stderr)
    (work/(name+'.rc')).write_text(str(result.returncode)+'\n')
    runtime.check_not_harness(result,name)
    return result


def main():
    parser=argparse.ArgumentParser()
    for name in ('source','work','guest','mkfs'): parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--cc',required=True);args=parser.parse_args()
    args.work.mkdir(parents=True,exist_ok=True)
    work=Path(tempfile.mkdtemp(prefix='run-',dir=args.work))
    manifest=dict(status='INCOMPLETE',cases=[],source=str(args.source),guest=str(args.guest),work=str(work))
    def save(): (work/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    save();print('EVIDENCE '+str(work),flush=True)
    required=sorted(set(runtime.HOST_SOURCES+runtime.HOST_HEADERS+runtime.PRODUCTION_SOURCES+runtime.GUEST_HEADERS+
        ['os/sort.h','os/guest.ld','tests/yanfs_fill_probe.c','tests/yanfs_4095_performance.py','tests/guest/run_arena_budget.py',
         'tests/guest/run_terminal_runtime.py','tests/guest/run_yanfs_4095_performance.py']))
    try:
        for name in required:
            if not (args.source/name).is_file(): raise oracle.Failure('MISSING_INPUT',name)
        for path in (args.guest,args.mkfs):
            if not path.is_file(): raise oracle.Failure('MISSING_INPUT',str(path))
        for relative,actual in [('tests/yanfs_4095_performance.py',oracle.__file__),
                                ('tests/guest/run_terminal_runtime.py',runtime.__file__),
                                ('tests/guest/run_yanfs_4095_performance.py',__file__)]:
            oracle.require(digest(args.source/relative)==digest(Path(actual)),'HARNESS_IDENTITY')
        if not runtime.tool_available(args.cc):
            manifest['status']='NOT_VERIFIED';manifest['error']='external Host compiler unavailable';save()
            print('SKIP external Host compiler unavailable');return 77
        manifest['source_sha256']={name:digest(args.source/name) for name in required}
        manifest['artifact_sha256']={str(path):digest(path) for path in (args.guest,args.mkfs)}
        version=execute(work,'compiler-version',[args.cc,'--version'])
        if version.returncode: raise runtime.HarnessError('compiler version failed')
        manifest['compiler_version']=version.stdout.decode(errors='replace')
        driver=work/'terminal_runtime_driver'
        argv=[args.cc,*runtime.HOST_CC_FLAGS,'-I',str(args.source/'include'),'-I',str(args.source/'tools'),*[str(args.source/s) for s in runtime.HOST_SOURCES],'-o',str(driver)]
        if execute(work,'build-driver',argv).returncode: raise runtime.HarnessError('driver build failed')
        manifest['artifact_sha256'][str(driver)]=digest(driver)
        # Cross-check the independent encoder against the actual FS API once.
        filler=work/'fill'
        argv=[args.cc,*runtime.HOST_CC_FLAGS,'-I',str(args.source/'os'),str(args.source/'tests/yanfs_fill_probe.c'),str(args.source/'os/yanfs.c'),'-o',str(filler)]
        if execute(work,'build-filler',argv).returncode: raise runtime.HarnessError('filler build failed')
        manifest['artifact_sha256'][str(filler)]=digest(filler)
        ref=work/'reference63.img'
        for name,argv in [('mkfs63',[str(args.mkfs),'--image',str(ref),'--files','63','--blocks','68']),
                          ('fill63',[str(filler),'--image',str(ref),'--files','63'])]:
            if execute(work,name,argv).returncode: raise runtime.HarnessError(name+' failed')
        oracle.require(ref.read_bytes()==oracle.fixture(63),'HOST_FIXTURE_CROSSCHECK')
        manifest['fixture_crosscheck63_sha256']=digest(ref)
        failures=0;harness_errors=0
        for n in oracle.SIZES:
            image=oracle.fixture(n);oracle.decode_check(image,n)
            for case in oracle.CASES:
                label='%d-%s'%(n,case);casework=work/label;casework.mkdir()
                disk=casework/'disk.img';disk.write_bytes(image)
                stdin=casework/'stdin';stdin.write_bytes(oracle.expected(n,case)[0])
                (casework/'expected').write_bytes(oracle.expected(n,case)[1])
                capture=casework/'capture';status=casework/'status'
                argv=[str(driver),'--image',str(args.guest),'--mode','production','--disk-image',str(disk),'--stdin',str(stdin),
                      '--capture',str(capture),'--status',str(status),'--max-steps','500000000','--fault','none',
                      '--corrupt-response','0','--tx-fault-at','0','--expect-tohost','0x00000001']
                record=dict(files=n,case=case,status='INCOMPLETE',work=str(casework));manifest['cases'].append(record);save()
                try:
                    result=execute(casework,'driver',argv)
                    runtime.check_not_harness(result,label)
                    if result.returncode not in (0,6,7): raise runtime.HarnessError(label+': driver exit '+str(result.returncode))
                    values=oracle.parse_status(status.read_text())
                    record['raw_status']=values
                    record['capture_sha256']=digest(capture)
                    record['disk_sha256']=digest(disk)
                    record.update(oracle.verify(n,case,values,capture.read_bytes(),disk.read_bytes(),image,result.returncode,result.stdout,result.stderr))
                    record['status']='PASS'
                except oracle.Failure as error:
                    failures+=1;record.update(status='FAIL',error=str(error),assertion=error.code)
                except (runtime.HarnessError,OSError,ValueError) as error:
                    harness_errors+=1;record.update(status='HARNESS_ERROR',error=str(error))
                save();print(json.dumps(record),flush=True)
        oracle.verify_matrix(manifest['cases'])
        unchanged=manifest['source_sha256']=={name:digest(args.source/name) for name in required}
        oracle.require(unchanged,'SOURCE_CHANGED')
        oracle.require(manifest['artifact_sha256']=={path:digest(Path(path)) for path in manifest['artifact_sha256']},'ARTIFACT_CHANGED')
        manifest.update(status='HARNESS_ERROR' if harness_errors else 'FAIL' if failures else 'PASS',
                        failures=failures,harness_errors=harness_errors,source_unchanged=unchanged)
        save();return 2 if harness_errors else 1 if failures else 0
    except oracle.Failure as error:
        manifest.update(status='FAIL',error=str(error));save();print('FAIL '+str(error));return 1
    except (RuntimeError,OSError,ValueError,subprocess.SubprocessError) as error:
        manifest.update(status='HARNESS_ERROR',error=str(error));save();print('HARNESS_ERROR '+str(error));return 2


if __name__=='__main__': sys.exit(main())
