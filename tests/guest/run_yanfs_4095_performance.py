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
    parser.add_argument('--cc',required=True)
    # 0040-R5-AC1: the spec states "两次计步完全一致" (two runs count identically).
    # Guest step counts are the simulator's instruction counter, not a clock, so
    # they must be bit-identical between runs. --repeat 2 turns that claim into a
    # check; the default of 1 leaves the matrix exactly as it was.
    parser.add_argument('--repeat',type=int,default=1)
    # --sizes narrows the matrix so the determinism check can run 4095 only,
    # instead of paying for all three sizes twice.
    parser.add_argument('--sizes',default='')
    args=parser.parse_args()
    if args.repeat<1: raise SystemExit('--repeat must be >= 1')
    sizes=oracle.SIZES
    if args.sizes:
        try: sizes=tuple(int(part) for part in args.sizes.split(',') if part.strip())
        except ValueError: raise SystemExit('--sizes must be comma-separated integers') from None
        if not sizes or any(n not in oracle.SIZES for n in sizes):
            raise SystemExit('--sizes must be a non-empty subset of %s' % (oracle.SIZES,))
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
        failures=0;harness_errors=0;inconsistent=0
        for n in sizes:
            image=oracle.fixture(n);oracle.decode_check(image,n)
            for case in oracle.CASES:
                label='%d-%s'%(n,case)
                # Run the same case args.repeat times. Runs after the first go
                # into a sibling directory so the recorded evidence of run 1 is
                # untouched; only the step counts are compared.
                seen=[]
                record=None
                for attempt in range(args.repeat):
                    casework=work/label if attempt==0 else work/(label+'.repeat%d'%attempt)
                    casework.mkdir()
                    disk=casework/'disk.img';disk.write_bytes(image)
                    stdin=casework/'stdin';stdin.write_bytes(oracle.expected(n,case)[0])
                    (casework/'expected').write_bytes(oracle.expected(n,case)[1])
                    capture=casework/'capture';status=casework/'status'
                    argv=[str(driver),'--image',str(args.guest),'--mode','production','--disk-image',str(disk),'--stdin',str(stdin),
                          '--capture',str(capture),'--status',str(status),'--max-steps','500000000','--fault','none',
                          '--corrupt-response','0','--tx-fault-at','0','--expect-tohost','0x00000001']
                    if attempt==0:
                        record=dict(files=n,case=case,status='INCOMPLETE',work=str(casework));manifest['cases'].append(record);save()
                    try:
                        result=execute(casework,'driver',argv)
                        runtime.check_not_harness(result,label)
                        if result.returncode not in (0,6,7): raise runtime.HarnessError(label+': driver exit '+str(result.returncode))
                        values=oracle.parse_status(status.read_text())
                        if attempt==0:
                            record['raw_status']=values
                            record['capture_sha256']=digest(capture)
                            record['disk_sha256']=digest(disk)
                            record.update(oracle.verify(n,case,values,capture.read_bytes(),disk.read_bytes(),image,result.returncode,result.stdout,result.stderr))
                            record['status']='PASS'
                        seen.append((values,result.returncode))
                    except oracle.Failure as error:
                        if attempt==0:
                            failures+=1;record.update(status='FAIL',error=str(error),assertion=error.code)
                        else:
                            raise
                    except (runtime.HarnessError,OSError,ValueError) as error:
                        if attempt==0:
                            harness_errors+=1;record.update(status='HARNESS_ERROR',error=str(error))
                        else:
                            raise
                    if attempt==0: save()
                # 0040-R5-AC1: every repeat must report the same step counters.
                # Comparing the whole status map (not just totals) also catches a
                # drift that happens to preserve the sum.
                if len(seen)>1:
                    first_status,first_rc=seen[0]
                    for index,(other_status,other_rc) in enumerate(seen[1:],start=2):
                        if other_status!=first_status or other_rc!=first_rc:
                            inconsistent+=1
                            only_here={k:other_status.get(k) for k in first_status if other_status.get(k)!=first_status.get(k)}
                            print(':FAIL: %s: steps not reproducible (run 1 vs run %d: %s)'
                                  %(label,index,json.dumps(only_here,sort_keys=True)),flush=True)
                            if record is not None:
                                record.setdefault('reproducibility',[]).append(
                                    dict(run=index,differences=only_here))
                save();print(json.dumps(record),flush=True)
        # The matrix check only applies to a full three-size run; a narrowed run
        # is a determinism probe, not the acceptance matrix.
        if tuple(sizes)==tuple(oracle.SIZES):
            oracle.verify_matrix(manifest['cases'])
        unchanged=manifest['source_sha256']=={name:digest(args.source/name) for name in required}
        oracle.require(unchanged,'SOURCE_CHANGED')
        oracle.require(manifest['artifact_sha256']=={path:digest(Path(path)) for path in manifest['artifact_sha256']},'ARTIFACT_CHANGED')
        manifest.update(status='HARNESS_ERROR' if harness_errors else 'FAIL' if (failures or inconsistent) else 'PASS',
                        failures=failures,harness_errors=harness_errors,
                        repeat=args.repeat,sizes=list(sizes),
                        step_inconsistencies=inconsistent,source_unchanged=unchanged)
        save();return 2 if harness_errors else 1 if (failures or inconsistent) else 0
    except oracle.Failure as error:
        manifest.update(status='FAIL',error=str(error));save();print('FAIL '+str(error));return 1
    except (RuntimeError,OSError,ValueError,subprocess.SubprocessError) as error:
        manifest.update(status='HARNESS_ERROR',error=str(error));save();print('HARNESS_ERROR '+str(error));return 2


if __name__=='__main__': sys.exit(main())
