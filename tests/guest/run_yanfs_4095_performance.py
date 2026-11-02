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
    # 0040-R5-AC4: the spec states "两次计步完全一致" (two runs count identically).
    # Guest step counts are the simulator's instruction counter, not a clock, so
    # they must be bit-identical between runs. --repeat 2 turns that claim into a
    # check; the default of 1 leaves the matrix exactly as it was.
    parser.add_argument('--repeat',type=int,default=1)
    # --sizes narrows the matrix so the determinism check can run 4095 only,
    # instead of paying for all three sizes twice.
    parser.add_argument('--sizes',default='')
    # 0043-R2: one flag turns the acceptance matrix into the single default entry:
    # 27 first-round cases plus nine independent 4095 repeats, 36 Guest launches in
    # all. Any selected subset that includes 4095 is a valid plan for it (the repeat
    # rows follow the selection); a plan without 4095, or one combined with another
    # --repeat, is a CLI usage error that reports before any evidence directory
    # exists. Legacy --sizes/--repeat calls without this flag keep their own
    # diagnostics and exit behaviour.
    parser.add_argument('--repeat-4095',action='store_true')
    args=parser.parse_args()
    def reject(message):
        """Usage error for the new flag (exit 2); legacy message and exit 1."""
        if args.repeat_4095: parser.error(message)
        raise SystemExit(message)
    if args.repeat<1: reject('--repeat must be >= 1')
    sizes=oracle.SIZES
    if args.sizes:
        try: sizes=tuple(int(part) for part in args.sizes.split(',') if part.strip())
        except ValueError: reject('--sizes must be comma-separated integers')
        if not sizes or any(n not in oracle.SIZES for n in sizes):
            reject('--sizes must be a non-empty subset of %s' % (oracle.SIZES,))
    if args.repeat_4095:
        if oracle.REPEAT_FILES not in sizes:
            parser.error('--repeat-4095 requires --sizes to include %d' % oracle.REPEAT_FILES)
        if args.repeat!=1:
            parser.error('--repeat-4095 requires --repeat 1; the repeat is built in')
    args.work.mkdir(parents=True,exist_ok=True)
    work=Path(tempfile.mkdtemp(prefix='run-',dir=args.work))
    manifest=dict(status='INCOMPLETE',cases=[],repeats=[],source=str(args.source),guest=str(args.guest),work=str(work))
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
        # 0043-R1: build the full images with the production tools, then compare
        # every byte with the independent encoder. The 1023/2047 products are
        # what the matrix mounts, so a damaged product cannot be replaced by the
        # encoder result.
        production={}
        manifest['fixture_crosscheck_sha256']={}
        for files in (63,1023,2047):
            path=work/('production-%d.img'%files)
            blocks=files+(files+1)//64+4
            for name,argv in [('mkfs%d'%files,[str(args.mkfs),'--image',str(path),'--files',str(files),'--blocks',str(blocks)]),
                              ('fill%d'%files,[str(filler),'--image',str(path),'--files',str(files)])]:
                if execute(work,name,argv).returncode: raise runtime.HarnessError(name+' failed')
            oracle.require(path.is_file(),'MISSING_PRODUCTION_IMAGE')
            production[files]=path.read_bytes()
            oracle.require(production[files]==oracle.fixture(files),'HOST_FIXTURE_CROSSCHECK')
            oracle.decode_check(production[files],files)
            manifest['fixture_crosscheck_sha256'][str(files)]=digest(path)
        failures=0;harness_errors=0;inconsistent=0
        def run_case(n,case,image,label):
            """Every Guest launch: fresh disk and input, the complete oracle, one
            classified evidence row. First rounds, legacy --repeat attempts and the
            0043 repeats all go through here, so no attempt can skip verification
            and no tool fault can be filed as a behavioural defect."""
            nonlocal failures,harness_errors
            casework=work/label
            casework.mkdir()
            record=dict(files=n,case=case,status='INCOMPLETE',work=str(casework),
                        input_sha256=hashlib.sha256(image).hexdigest())
            disk=casework/'disk.img';disk.write_bytes(image)
            stdin=casework/'stdin';stdin.write_bytes(oracle.expected(n,case)[0])
            (casework/'expected').write_bytes(oracle.expected(n,case)[1])
            capture=casework/'capture';status=casework/'status'
            argv=[str(driver),'--image',str(args.guest),'--mode','production','--disk-image',str(disk),'--stdin',str(stdin),
                  '--capture',str(capture),'--status',str(status),'--max-steps','500000000','--fault','none',
                  '--corrupt-response','0','--tx-fault-at','0','--expect-tohost','0x00000001']
            try:
                result=execute(casework,'driver',argv)
                runtime.check_not_harness(result,label)
                if result.returncode not in (0,6,7): raise runtime.HarnessError(label+': driver exit '+str(result.returncode))
                values=oracle.parse_status(status.read_text())
                record.update(raw_status=values,returncode=result.returncode,
                              capture_sha256=digest(capture),disk_sha256=digest(disk))
                record.update(oracle.verify(n,case,values,capture.read_bytes(),disk.read_bytes(),image,
                                            result.returncode,result.stdout,result.stderr))
                record['status']='PASS'
            except oracle.Failure as error:
                failures+=1;record.update(status='FAIL',error=str(error),assertion=error.code)
            except (runtime.HarnessError,OSError,ValueError) as error:
                harness_errors+=1;record.update(status='HARNESS_ERROR',error=str(error))
            return record
        for n in sizes:
            # 0043-R1: the large sizes are mounted from the production image that
            # was just compared byte-for-byte; 4095 keeps the independent encoder
            # product because it has no filler-generated counterpart.
            image=production.get(n) or oracle.fixture(n)
            oracle.decode_check(image,n)
            for case in oracle.CASES:
                label='%d-%s'%(n,case)
                record=dict(files=n,case=case,status='INCOMPLETE')
                manifest['cases'].append(record);save()
                first=run_case(n,case,image,label)
                record.update(first)
                # 0043-R2: every legacy --repeat attempt also runs the complete
                # oracle, in its own fresh directory, and is compared with the
                # first round as a whole status table plus exit code.
                attempts=[dict(first)]
                for attempt in range(1,args.repeat):
                    attempts.append(run_case(n,case,image,label+'.repeat%d'%attempt))
                if len(attempts)>1:
                    record['attempts']=attempts
                    first_status,first_rc=record.get('raw_status'),record.get('returncode')
                    for index,other in enumerate(attempts[1:],start=2):
                        if other.get('raw_status') is None or first_status is None: continue
                        if other['raw_status']!=first_status or other.get('returncode')!=first_rc:
                            inconsistent+=1
                            only_here={k:other['raw_status'].get(k) for k in first_status
                                       if other['raw_status'].get(k)!=first_status.get(k)}
                            print(':FAIL: %s: steps not reproducible (run 1 vs run %d: %s)'
                                  %(label,index,json.dumps(only_here,sort_keys=True)),flush=True)
                            record.setdefault('reproducibility',[]).append(
                                dict(run=index,differences=only_here))
                # 0043-R2: the independent 4095 repeat. It starts from a fresh copy
                # of the original image and records its own directory, input,
                # output, image, raw status, exit code and verdict, so a missing,
                # duplicated or drifting repeat cannot pass.
                if args.repeat_4095 and n==oracle.REPEAT_FILES:
                    manifest['repeats'].append(run_case(n,case,image,label+'.repeat-4095'))
                save();print(json.dumps(record),flush=True)
        # 0043-R2: with --repeat-4095 the acceptance plan is the 27-case matrix
        # plus nine independent records; missing, duplicate or drifting repeats
        # fail here. Without it the matrix check only applies to a full three-size
        # run; a narrowed run is a determinism probe, not the acceptance matrix.
        # The plan verdict never overrides the harness classification: a tool fault
        # is reported as HARNESS_ERROR even though the plan is therefore unprovable.
        plan_error=None
        try:
            if args.repeat_4095: oracle.verify_plan(manifest['cases'],manifest['repeats'],sizes)
            elif tuple(sizes)==tuple(oracle.SIZES): oracle.verify_matrix(manifest['cases'])
        except oracle.Failure as error:
            plan_error=str(error)
        unchanged=manifest['source_sha256']=={name:digest(args.source/name) for name in required}
        oracle.require(unchanged,'SOURCE_CHANGED')
        oracle.require(manifest['artifact_sha256']=={path:digest(Path(path)) for path in manifest['artifact_sha256']},'ARTIFACT_CHANGED')
        status='HARNESS_ERROR' if harness_errors else 'FAIL' if (failures or inconsistent or plan_error) else 'PASS'
        if plan_error is not None:
            manifest['plan_error']=plan_error
            if not harness_errors: manifest['error']=plan_error
        manifest.update(status=status,failures=failures,harness_errors=harness_errors,
                        repeat=args.repeat,sizes=list(sizes),repeat_4095=args.repeat_4095,
                        repeat_records=len(manifest['repeats']),
                        step_inconsistencies=inconsistent,source_unchanged=unchanged)
        save();return 2 if harness_errors else 1 if status=='FAIL' else 0
    except oracle.Failure as error:
        manifest.update(status='FAIL',error=str(error));save();print('FAIL '+str(error));return 1
    except (RuntimeError,OSError,ValueError,subprocess.SubprocessError) as error:
        manifest.update(status='HARNESS_ERROR',error=str(error));save();print('HARNESS_ERROR '+str(error));return 2


if __name__=='__main__': sys.exit(main())
