"""0040-R4 negative controls; no compiler, Guest or model dependency."""
import contextlib
import copy
import hashlib
import io
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib

import yanfs_4095_performance as oracle
sys.path.insert(0,str(Path(__file__).parent/'guest'))
import run_yanfs_4095_performance as entry


def observation(n=4095,case='ls'):
    count=1 if case=='mount' else 3 if case=='create' else 2
    values=dict(verdict='pass',tohost='0x00000001',protocol_fired='0',tx_fired='0',
                arena_known='1',arena_used=str(oracle.arena_bytes(n)),task0_observed_stack_bytes='2096')
    for i in range(count):
        values['perf_prompt_%d'%i]=str(1000+i*2000)
        values['perf_input_%d'%i]=str(2000+i*2000)
    values['perf_total']=str(3000+count*2000)
    capture=oracle.expected(n,case)[1]
    values.update(delivered=str(len(capture)),out_length=str(len(capture)))
    return dict(n=n,case=case,values=values,capture=capture,disk=oracle.fixture(n),image=oracle.fixture(n))


class PerformanceControls(unittest.TestCase):
    def source_fixture(self,base):
        source=base/'source'
        required=set(entry.runtime.HOST_SOURCES+entry.runtime.HOST_HEADERS+
            entry.runtime.PRODUCTION_SOURCES+entry.runtime.GUEST_HEADERS+['os/sort.h','os/guest.ld',
            'tests/yanfs_fill_probe.c','tests/yanfs_4095_performance.py','tests/guest/run_arena_budget.py',
            'tests/guest/run_terminal_runtime.py','tests/guest/run_yanfs_4095_performance.py'])
        real=Path(__file__).resolve().parents[1]
        for name in required:
            path=source/name;path.parent.mkdir(parents=True,exist_ok=True)
            path.write_bytes((real/name).read_bytes())
        for name in ('guest','mkfs'): (base/name).write_bytes(b'controlled artifact')
        return source

    def invoke(self,base,source):
        argv=['entry','--source',str(source),'--work',str(base/'work'),
              '--guest',str(base/'guest'),'--mkfs',str(base/'mkfs'),'--cc','missing-controlled-cc']
        with patch.object(sys,'argv',argv): return entry.main()

    def reject(self,code,**changes):
        data=observation();data.update(changes)
        with self.assertRaises(oracle.Failure) as caught: oracle.verify(**data)
        self.assertEqual(code,caught.exception.code)

    def test_all_complete_shapes(self):
        self.assertEqual(0xcbf43926,zlib.crc32(b'123456789'))
        self.assertEqual(65,len(oracle.NOTE))
        for n in oracle.SIZES:
            oracle.decode_check(oracle.fixture(n),n)
            for case in oracle.CASES: oracle.verify(**observation(n,case))

    def test_image_decoder_rejects_damage(self):
        image=bytearray(oracle.fixture(4095));image[-1]=1
        with self.assertRaises(oracle.Failure): oracle.decode_check(bytes(image),4095)
        image=bytearray(oracle.fixture(4095));image[60]^=1
        with self.assertRaises(oracle.Failure): oracle.decode_check(bytes(image),4095)

    def test_decoder_rejects_resealed_reserved_and_version_damage(self):
        for offset,value in ((8,3),(28,1),(64+10,1)):
            image=bytearray(oracle.fixture(4095));image[offset]=value
            image[60:64]=b'\0'*4
            struct.pack_into('<I',image,60,zlib.crc32(image[:64*4096])&0xffffffff)
            with self.assertRaises(oracle.Failure) as caught: oracle.decode_check(bytes(image),4095)
            self.assertEqual('FIXTURE_STRUCTURE',caught.exception.code)

    def test_matrix_exact_complete_unique(self):
        records=[dict(files=n,case=case) for n in oracle.SIZES for case in oracle.CASES]
        oracle.verify_matrix(records)
        for invalid in ([],records[:-1],records+[records[0]],records[:-1]+[records[0]]):
            with self.assertRaises(oracle.Failure) as caught: oracle.verify_matrix(invalid)
            self.assertEqual('MATRIX',caught.exception.code)

    def test_rows_missing_wrong_duplicated_and_order(self):
        good=observation()['capture']
        for bad in (good.replace(b'65 f0000.txt\r\n',b'',1),
                    good.replace(b'65 f0000.txt',b'64 f0000.txt',1),
                    good.replace(b'65 f0000.txt\r\n',b'65 f0000.txt\r\n'*2,1),
                    good.replace(b'65 f0000.txt\r\n65 f0001.txt',b'65 f0001.txt\r\n65 f0000.txt',1),
                    good[:-1],good+b'junk'):
            self.reject('CAPTURE_EXACT',capture=bad)

    def test_summary_and_reference_positions(self):
        for case in ('backlinks','related','block-backlinks','search','meta-filter'):
            data=observation(case=case)
            if case=='block-backlinks': data['capture']=data['capture'].replace(b'bytes=49:64',b'bytes=48:64',1)
            else: data['capture']=data['capture'].replace(b'4094',b'4093').replace(b'4093',b'4092').replace(b'8188',b'8187')
            with self.assertRaises(oracle.Failure) as caught: oracle.verify(**data)
            self.assertEqual('CAPTURE_EXACT',caught.exception.code)

    def test_exit_disk_lengths_and_faults(self):
        self.reject('EXIT',returncode=4)
        self.reject('IMAGE_EXACT',disk=observation()['disk'][:-1])
        for key,value,code in (('tohost','0','TOHOST'),('delivered','1','DELIVERED'),
                               ('out_length','65535','OUTPUT_LENGTH'),('tx_fired','1','FAULT'),
                               ('arena_used','970592','ARENA')):
            values=copy.copy(observation()['values']);values[key]=value
            self.reject(code,values=values)
        self.reject('DIAGNOSTIC',stderr=b'runtime error: controlled diagnostic')
        self.reject('DIAGNOSTIC',stdout=b'AddressSanitizer: controlled diagnostic')

    def test_counter_missing_extra_order_duplicate(self):
        for mode in ('missing','extra','order','zero'):
            values=copy.copy(observation()['values'])
            if mode=='missing': del values['perf_input_0']
            elif mode=='extra': values['perf_input_3']='10'
            elif mode=='order': values['perf_prompt_1']='1'
            else: values['perf_prompt_1']=values['perf_input_0']
            self.reject('TIMING_MARKERS' if mode in ('missing','extra') else 'TIMING_ORDER',values=values)
        with self.assertRaises(oracle.Failure) as caught: oracle.parse_status('verdict=pass\nverdict=pass\n')
        self.assertEqual('STATUS_DUPLICATE',caught.exception.code)

    def test_exact_budget_and_one_more(self):
        data=observation();values=data['values'];values['perf_prompt_1']=str(2000+oracle.STEP_LIMIT)
        values['perf_input_1']=str(3000+oracle.STEP_LIMIT);values['perf_total']=str(4000+oracle.STEP_LIMIT)
        self.assertEqual(oracle.STEP_LIMIT,oracle.verify(**data)['steps'])
        values['perf_prompt_1']=str(2001+oracle.STEP_LIMIT)
        with self.assertRaises(oracle.Failure) as caught: oracle.verify(**data)
        self.assertEqual('COMMAND_BUDGET',caught.exception.code)

    def test_stack_missing_negative_zero_overflow(self):
        for value,code in ((None,'STATUS_MISSING_task0_observed_stack_bytes'),
                            ('-1','STATUS_NEGATIVE_task0_observed_stack_bytes'),('0','STACK'),('4097','STACK')):
            values=copy.copy(observation()['values'])
            if value is None: del values['task0_observed_stack_bytes']
            else: values['task0_observed_stack_bytes']=value
            self.reject(code,values=values)

    def test_current_arena_budget_and_distinguishing_elf(self):
        path=Path(__file__).parent/'guest/run_arena_budget.py'
        spec=importlib.util.spec_from_file_location('budget',path);budget=importlib.util.module_from_spec(spec);spec.loader.exec_module(budget)
        self.assertEqual(1003358,budget.expected_arena(4095,0))
        # BSS need not occupy file bytes. Old seven-table accounting accepts
        # this PT_LOAD footprint, current eleven-table accounting must reject.
        data=bytearray(84);data[:7]=b'\x7fELF\x01\x01\x01'
        struct.pack_into('<I',data,28,52);struct.pack_into('<HH',data,42,32,1)
        struct.pack_into('<I',data,52,1);struct.pack_into('<I',data,60,0x80000000)
        struct.pack_into('<I',data,72,32*1024*1024-990000)
        failures=budget.boundary_failures(bytes(data),1003358)
        self.assertTrue(any('exceed' in failure for failure in failures),failures)
        # The historical seven-table 970,592-byte value would still fit this
        # boundary, so the test really separates the current eleven tables.
        self.assertEqual([],budget.boundary_failures(bytes(data),970592))

    def test_main_missing_local_before_external_skip_and_fresh_work(self):
        with tempfile.TemporaryDirectory(prefix='yanfs-entry-control-') as temp:
            base=Path(temp);source=base/'missing'
            with patch.object(entry.runtime,'tool_available',return_value=False):
                self.assertEqual(1,self.invoke(base,source))
                source=self.source_fixture(base)
                sentinel=base/'work/sentinel';sentinel.write_bytes(b'keep')
                self.assertEqual(77,self.invoke(base,source))
                self.assertEqual(77,self.invoke(base,source))
            self.assertEqual(b'keep',sentinel.read_bytes())
            manifests=list((base/'work').glob('run-*/manifest.json'))
            self.assertEqual(3,len(manifests))
            states=sorted(json.loads(p.read_text())['status'] for p in manifests)
            self.assertEqual(['FAIL','NOT_VERIFIED','NOT_VERIFIED'],states)

    def test_main_rejects_another_harness_and_build_error(self):
        with tempfile.TemporaryDirectory(prefix='yanfs-entry-control-') as temp:
            base=Path(temp);source=self.source_fixture(base)
            path=source/'tests/yanfs_4095_performance.py';original=path.read_bytes()
            path.write_bytes(original+b'\n# controlled different harness\n')
            with patch.object(entry.runtime,'tool_available',return_value=False):
                self.assertEqual(1,self.invoke(base,source))
            path.write_bytes(original)
            def failed_build(work,name,argv,timeout=600):
                return subprocess.CompletedProcess(argv,1 if name=='build-driver' else 0,b'controlled',b'')
            with patch.object(entry.runtime,'tool_available',return_value=True),patch.object(entry,'execute',side_effect=failed_build):
                self.assertEqual(2,self.invoke(base,source))
            states=[json.loads(p.read_text())['status'] for p in (base/'work').glob('run-*/manifest.json')]
            self.assertEqual(['FAIL','HARNESS_ERROR'],sorted(states))

    def test_execute_timeout_and_zero_exit_sanitizer_are_harness_errors(self):
        with tempfile.TemporaryDirectory(prefix='yanfs-entry-control-') as temp:
            work=Path(temp)
            with patch.object(entry.subprocess,'run',side_effect=subprocess.TimeoutExpired(['fake'],1,output=b'prefix',stderr=b'controlled timeout')):
                with self.assertRaises(entry.runtime.HarnessError): entry.execute(work,'timeout',['fake'],1)
            self.assertEqual(b'prefix',(work/'timeout.stdout').read_bytes())
            with patch.object(entry.subprocess,'run',return_value=subprocess.CompletedProcess(['fake'],0,b'',b'AddressSanitizer: controlled')):
                with self.assertRaises(entry.runtime.HarnessError): entry.execute(work,'sanitizer',['fake'])

    def test_existing_stack_and_persistent_local_inputs_precede_tool_skip(self):
        real=Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory(prefix='yanfs-preflight-control-') as temp:
            base=Path(temp);source=base/'source';source.mkdir()
            stack=real/'tests/run_stack_usage.py'
            argv=[sys.executable,'-B',str(stack),'--source',str(source),'--work',str(base/'stack'),
                  '--gcc','missing-controlled-gcc']
            self.assertEqual(1,subprocess.run(argv,capture_output=True).returncode)
            for relative in ('os/yanfs.c','os/yanfs.h','os/memory.h','os/sort.h'):
                path=source/relative;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes((real/relative).read_bytes())
            self.assertEqual(77,subprocess.run(argv,capture_output=True).returncode)
            required=('tests/guest/persistent_combined.c','tests/guest/guest_lib.c','tests/guest/link.ld',
                'tests/guest/test_persistent_combined_gate.py','os/trap_entry.S','os/task_switch.S','os/task.c',
                'os/task.h','os/block.c','os/block.h','os/block_peek.h','os/platform.h','os/console.c',
                'tools/host_disk.c','tools/host_disk.h','tools/host_block.c','tools/yan_run.c',
                'host/note_write.c','host/note_write.h','os/yanfs.c','os/yanfs.h','os/sort.h','os/memory.h',
                'os/note_metadata_format.c','os/note_metadata_format.h','os/note_metadata_format_internal.h',
                'os/note_blocks_format.c','os/note_blocks_format.h','os/search_text.c','os/search_text.h')
            for relative in required:
                if relative=='os/block_peek.h': continue
                path=source/relative;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes((real/relative).read_bytes())
            executor=base/'executor';executor.write_bytes(b'controlled local artifact')
            argv=[sys.executable,'-B',str(real/'tests/guest/run_persistent_combined.py'),
                  '--source',str(source),'--work',str(base/'persistent'),'--run',str(executor),
                  '--gcc','missing-controlled-gcc']
            result=subprocess.run(argv,capture_output=True)
            self.assertEqual(1,result.returncode);self.assertIn(b'block_peek.h',result.stdout+result.stderr)
            (source/'os/block_peek.h').write_bytes((real/'os/block_peek.h').read_bytes())
            self.assertEqual(77,subprocess.run(argv,capture_output=True).returncode)
            argv[argv.index('--run')+1]=str(base/'missing-executor')
            self.assertEqual(1,subprocess.run(argv,capture_output=True).returncode)

    def test_main_case_harness_priority_and_fixture_failure_finalize(self):
        with tempfile.TemporaryDirectory(prefix='yanfs-entry-control-') as temp:
            base=Path(temp);source=self.source_fixture(base)
            def controlled(work,name,argv,timeout=600):
                if name=='build-driver': (work/'terminal_runtime_driver').write_bytes(b'driver')
                if name=='build-filler': (work/'fill').write_bytes(b'filler')
                if name[:4] in ('mkfs','fill'): (work/('production-%s.img'%name[4:])).write_bytes(oracle.fixture(int(name[4:])))
                return subprocess.CompletedProcess(argv,4 if name=='driver' else 0,b'controlled',b'')
            with patch.object(entry.runtime,'tool_available',return_value=True),patch.object(entry,'execute',side_effect=controlled),\
                 patch.object(oracle,'SIZES',(1023,)),patch.object(oracle,'CASES',('mount',)):
                self.assertEqual(2,self.invoke(base,source))
            manifest=json.loads(next((base/'work').glob('run-*/manifest.json')).read_text())
            self.assertEqual('HARNESS_ERROR',manifest['status'])
            self.assertEqual(1,manifest['harness_errors']);self.assertEqual(0,manifest['failures'])
            with patch.object(entry.runtime,'tool_available',return_value=True),patch.object(entry,'execute',side_effect=controlled),\
                 patch.object(oracle,'fixture',return_value=b'too short'):
                self.assertEqual(1,self.invoke(base,source))
            errors=[json.loads(p.read_text()) for p in (base/'work').glob('run-*/manifest.json')]
            self.assertTrue(any(e['status']=='FAIL' and 'FIXTURE_STRUCTURE' in e.get('error','') for e in errors))



def verified_record(n,case):
    """A complete first-round evidence record, built from a good observation."""
    data=observation(n,case);summary=oracle.verify(**data)
    summary.update(status='PASS',work='controlled/%d-%s'%(n,case),raw_status=data['values'],returncode=0,
                   input_sha256=hashlib.sha256(data['image']).hexdigest(),
                   disk_sha256=hashlib.sha256(data['disk']).hexdigest())
    return summary


def complete_plan():
    first=[verified_record(n,case) for n in oracle.SIZES for case in oracle.CASES]
    by_key={(row['files'],row['case']):row for row in first}
    repeats=[dict(by_key[(4095,case)],work='controlled/4095-%s.repeat'%case) for case in oracle.CASES]
    return first,repeats


class Repeat4095Controls(unittest.TestCase):
    """0043-R2/R4: the new option, the repeat plan and the 36-launch contract."""

    source_fixture=PerformanceControls.source_fixture
    invoke=PerformanceControls.invoke

    def argv(self,base,source,*extra):
        return ['entry','--source',str(source),'--work',str(base/'work'),
                '--guest',str(base/'guest'),'--mkfs',str(base/'mkfs'),'--cc','missing-controlled-cc',*extra]

    def cli_call(self,base,source,*extra):
        """Run the real entry to its CLI boundary; returns (exit code, text).

        argparse reports usage errors on stderr, while the legacy paths raise
        SystemExit with the message on the exception, so both are collected.
        """
        buffer=io.StringIO()
        with patch.object(sys,'argv',self.argv(base,source,*extra)),\
             contextlib.redirect_stderr(buffer):
            with self.assertRaises(SystemExit) as caught: entry.main()
        code=caught.exception.code
        # SystemExit("message") leaves .code as the string; the interpreter then
        # exits 1, which is the behaviour the legacy CLI promises.
        if not isinstance(code,int): code=1 if code else 0
        return code,buffer.getvalue()+' '+str(caught.exception)

    def test_cli_usage_errors_create_no_evidence(self):
        with tempfile.TemporaryDirectory(prefix='yanfs-repeat-control-') as temp:
            base=Path(temp);source=self.source_fixture(base)
            # 0043-R2: every invalid combination of the new flag is a usage error
            # (exit 2) reported before an evidence directory exists. That includes
            # values the legacy paths reject: repeat 0/negative, malformed, unknown
            # and empty size tuples.
            invalid=(['--repeat-4095','--sizes','1023'],
                     ['--repeat-4095','--sizes','1023,2047'],
                     ['--repeat-4095','--repeat','2'],
                     ['--repeat-4095','--sizes','1023','--repeat','3'],
                     ['--repeat-4095','--repeat','0'],
                     ['--repeat-4095','--repeat','-1'],
                     ['--repeat-4095','--sizes','bad'],
                     ['--repeat-4095','--sizes','1023,bad'],
                     ['--repeat-4095','--sizes','999'],
                     ['--repeat-4095','--sizes',','],
                     ['--repeat-4095','--sizes','4095,999'],
                     ['--repeat-4095','--sizes','4095','--repeat','0'])
            for extra in invalid:
                self.assertEqual(2,self.cli_call(base,source,*extra)[0],extra)
            self.assertFalse((base/'work').exists(),'usage error created an evidence directory')
            # Legacy calls without the new flag keep their original diagnostics and
            # exit behaviour (message on the exception, exit 1, still no evidence).
            for extra,message in ((['--repeat','0'],'--repeat must be >= 1'),
                                  (['--repeat','-2'],'--repeat must be >= 1'),
                                  (['--sizes','bad'],'--sizes must be comma-separated integers'),
                                  (['--sizes','999'],'--sizes must be a non-empty subset'),
                                  (['--sizes',','],'--sizes must be a non-empty subset'),
                                  (['--sizes','4095','--repeat','0'],'--repeat must be >= 1')):
                code,text=self.cli_call(base,source,*extra)
                self.assertEqual(1,code,extra)
                self.assertIn(message,text,extra)
                self.assertNotIn('usage:',text,extra)
            self.assertFalse((base/'work').exists(),'legacy usage error created an evidence directory')
            # A subset that includes 4095 is a valid --repeat-4095 plan.
            with patch.object(sys,'argv',self.argv(base,source,'--sizes','4095','--repeat-4095')),\
                 patch.object(entry.runtime,'tool_available',return_value=False):
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(77,entry.main())
            self.assertTrue(list((base/'work').glob('run-*/manifest.json')))

    def test_production_image_mismatch_blocks_matrix(self):
        def bad_fill(work,name,argv,timeout=600):
            if name=='build-driver': (work/'terminal_runtime_driver').write_bytes(b'driver')
            if name=='build-filler': (work/'fill').write_bytes(b'filler')
            if name[:4] in ('mkfs','fill'):
                n=int(name[4:]);image=bytearray(oracle.fixture(n))
                if name.startswith('fill') and n==1023: image[-1]=1
                (work/('production-%d.img'%n)).write_bytes(bytes(image))
            return subprocess.CompletedProcess(argv,0,b'controlled',b'')
        with tempfile.TemporaryDirectory(prefix='yanfs-repeat-control-') as temp:
            base=Path(temp);source=self.source_fixture(base)
            with patch.object(entry.runtime,'tool_available',return_value=True),\
                 patch.object(entry,'execute',side_effect=bad_fill):
                self.assertEqual(1,self.invoke(base,source))
            manifest=json.loads(next((base/'work').glob('run-*/manifest.json')).read_text())
            self.assertEqual('FAIL',manifest['status'])
            self.assertEqual('HOST_FIXTURE_CROSSCHECK',manifest.get('error'))
            self.assertEqual([],manifest['cases'],'a mismatched production image reached the matrix')

    def test_missing_and_duplicate_repeats_rejected(self):
        first,repeats=complete_plan()
        self.assertEqual(27,len(first));self.assertEqual(9,len(repeats))
        self.assertTrue(oracle.verify_plan(first,repeats)['repeats']==9)
        for bad in (repeats[:-1],repeats[1:],repeats+[repeats[0]],repeats[:-1]+[repeats[0]],
                    first[:]):
            with self.assertRaises(oracle.Failure) as caught: oracle.verify_plan(first,bad)
            self.assertEqual('REPEAT_MATRIX',caught.exception.code)

    def test_raw_status_and_exit_drift_rejected(self):
        first,repeats=complete_plan()
        for index,mutate,code in ((0,lambda r:r['raw_status'].__setitem__('task0_observed_stack_bytes','2097'),'REPEAT_STATUS_DRIFT'),
                                  (1,lambda r:r['raw_status'].__setitem__('perf_total','1'),'REPEAT_STATUS_DRIFT'),
                                  (2,lambda r:r.__setitem__('returncode',6),'REPEAT_EXIT_DRIFT'),
                                  (3,lambda r:r['raw_status'].pop('verdict'),'REPEAT_STATUS_DRIFT'),
                                  (4,lambda r:r.__setitem__('capture_sha256','0'*64),'REPEAT_CAPTURE'),
                                  (5,lambda r:r.__setitem__('status','FAIL'),'REPEAT_STATUS'),
                                  (6,lambda r:r.__setitem__('input_sha256','1'*64),'REPEAT_INPUT')):
            broken=copy.deepcopy(repeats);mutate(broken[index])
            with self.assertRaises(oracle.Failure) as caught: oracle.verify_plan(first,broken)
            self.assertEqual(code,caught.exception.code)

    def controlled_matrix(self,corrupt=None):
        images={n:oracle.fixture(n) for n in (63,1023,2047,4095)}
        launched=[]
        def run(work,name,argv,timeout=600):
            launched.append(name)
            if name=='build-driver': (work/'terminal_runtime_driver').write_bytes(b'driver')
            elif name=='build-filler': (work/'fill').write_bytes(b'filler')
            elif name[:4] in ('mkfs','fill'):
                n=int(name[4:]);(work/('production-%d.img'%n)).write_bytes(images[n])
            elif name=='driver':
                label=work.name.split('.')[0];n=int(label.split('-')[0]);case=label.split('-',1)[1]
                capture=oracle.expected(n,case)[1]
                if corrupt is not None and corrupt(work.name,case): capture=capture+b'controlled damaged output'
                (work/'status').write_text(''.join('%s=%s\n'%(key,value) for key,value in observation(n,case)['values'].items()))
                (work/'capture').write_bytes(capture)
            return subprocess.CompletedProcess(argv,0,b'controlled',b'')
        return run,launched

    def run_matrix(self,base,source,corrupt=None,extra=('--repeat-4095',)):
        run,launched=self.controlled_matrix(corrupt)
        with patch.object(sys,'argv',self.argv(base,source,*extra)),\
             patch.object(entry.runtime,'tool_available',return_value=True),\
             patch.object(entry,'execute',side_effect=run):
            # The synthetic matrix would otherwise print its whole PASS JSON; the
            # assertions below are the signal. The real runner is untouched.
            with contextlib.redirect_stdout(io.StringIO()): code=entry.main()
        manifest=json.loads(next((base/'work').glob('run-*/manifest.json')).read_text())
        return code,manifest,[name for name in launched if name=='driver']

    def test_legacy_sizes_without_flag_still_runs_its_own_matrix(self):
        with tempfile.TemporaryDirectory(prefix='yanfs-repeat-control-') as temp:
            base=Path(temp);source=self.source_fixture(base)
            code,manifest,drivers=self.run_matrix(base,source,extra=('--sizes','1023'))
            self.assertEqual(0,code);self.assertEqual('PASS',manifest['status'])
            self.assertEqual(9,len(drivers),'--sizes 1023 --repeat 1 must launch 9 Guest runs')
            self.assertEqual([],manifest['repeats'])
            self.assertEqual({1023},{row['files'] for row in manifest['cases']})
            self.assertTrue(all(row['status']=='PASS' for row in manifest['cases']))

    def test_legacy_repeat_two_launches_eighteen_with_full_validation(self):
        with tempfile.TemporaryDirectory(prefix='yanfs-repeat-control-') as temp:
            base=Path(temp);source=self.source_fixture(base)
            code,manifest,drivers=self.run_matrix(base,source,extra=('--sizes','4095','--repeat','2'))
            self.assertEqual(0,code);self.assertEqual('PASS',manifest['status'])
            self.assertEqual(18,len(drivers),'--sizes 4095 --repeat 2 must launch 18 Guest runs')
            self.assertEqual([],manifest['repeats'])
            fixture=oracle.fixture(4095)
            for row in manifest['cases']:
                self.assertEqual('PASS',row['status'])
                attempts=row['attempts'];self.assertEqual(2,len(attempts))
                self.assertNotEqual(attempts[0]['work'],attempts[1]['work'])
                for attempt in attempts:
                    # A fresh copy of the original fixture in every attempt, and a
                    # complete oracle verdict (capture, image, budgets, counters),
                    # not only a step-count comparison.
                    self.assertEqual('PASS',attempt['status'])
                    self.assertEqual((Path(attempt['work'])/'disk.img').read_bytes(),fixture)
                    for key in ('capture_sha256','disk_sha256','input_sha256','raw_status',
                                'returncode','mount','steps','stack','arena','capture_bytes'):
                        self.assertIn(key,attempt)
                self.assertEqual(attempts[0]['raw_status'],attempts[1]['raw_status'])

    def test_bad_legacy_repeat_capture_is_caught_by_the_oracle(self):
        corrupt=lambda work,case: work.endswith('.repeat1') and case=='ls'
        with tempfile.TemporaryDirectory(prefix='yanfs-repeat-control-') as temp:
            base=Path(temp);source=self.source_fixture(base)
            code,manifest,drivers=self.run_matrix(base,source,corrupt,
                                                  extra=('--sizes','4095','--repeat','2'))
            self.assertEqual(18,len(drivers))
            self.assertEqual(1,code);self.assertEqual('FAIL',manifest['status'])
            row=[row for row in manifest['cases'] if row['case']=='ls'][0]
            self.assertEqual('CAPTURE_EXACT',row['attempts'][1]['assertion'],
                             'a repeat must run the full oracle, not just compare counters')
            self.assertEqual(row['attempts'][0]['raw_status'],row['attempts'][1]['raw_status'])

    def harness_fault(self,mode,corrupt=None,extra=()):
        with tempfile.TemporaryDirectory(prefix='yanfs-repeat-control-') as temp:
            base=Path(temp);source=self.source_fixture(base)
            run,launched=self.controlled_matrix(corrupt)
            def wrapped(work,name,argv,timeout=600):
                if name=='driver' and mode(work.name):
                    if os_fault: raise OSError('controlled local I/O failure')
                    raise entry.runtime.HarnessError('controlled transport failure')
                return run(work,name,argv,timeout)
            os_fault=getattr(mode,'os_fault',False)
            with patch.object(sys,'argv',self.argv(base,source,*extra)),\
                 patch.object(entry.runtime,'tool_available',return_value=True),\
                 patch.object(entry,'execute',side_effect=wrapped):
                with contextlib.redirect_stdout(io.StringIO()): code=entry.main()
            manifest=json.loads(next((base/'work').glob('run-*/manifest.json')).read_text())
            return code,manifest

    def test_repeat_harness_error_keeps_harness_classification(self):
        # A tool fault in the 4095 repeat (raised as HarnessError and as OSError) and
        # in a first-round attempt (raised as HarnessError) must all stay
        # HARNESS_ERROR rather than becoming a behavioural FAIL.
        for marker,os_fault,label in ((lambda name:'.repeat-4095' in name,False,'repeat-4095 HarnessError'),
                                      (lambda name:'.repeat-4095' in name,True,'repeat-4095 OSError'),
                                      (lambda name:name=='4095-search',False,'first round HarnessError')):
            mode=marker;mode.os_fault=os_fault
            code,manifest=self.harness_fault(marker,extra=('--repeat-4095',))
            self.assertEqual(2,code,'%s: harness fault must exit 2, not a behavioural 1' % label)
            self.assertEqual('HARNESS_ERROR',manifest['status'],label)
            self.assertEqual(0,manifest['failures'],'%s: no behavioural defect may be filed' % label)
            failed=[row for row in manifest['cases']+manifest['repeats'] if row['status']=='HARNESS_ERROR']
            self.assertGreaterEqual(len(failed),1,label)
            self.assertEqual(len(failed),manifest['harness_errors'],label)
            for row in failed: self.assertNotIn('assertion',row)

    def test_legacy_repeat_harness_error_keeps_harness_classification(self):
        mode=lambda name: name=='4095-ls.repeat1';mode.os_fault=False
        code,manifest=self.harness_fault(mode,extra=('--sizes','4095','--repeat','2'))
        self.assertEqual(2,code);self.assertEqual('HARNESS_ERROR',manifest['status'])
        self.assertEqual(1,manifest['harness_errors']);self.assertEqual(0,manifest['failures'])
        row=[row for row in manifest['cases'] if row['case']=='ls'][0]
        self.assertEqual('HARNESS_ERROR',row['attempts'][1]['status'])
        self.assertNotIn('assertion',row['attempts'][1])

    def test_valid_plan_launches_36_and_passes(self):
        with tempfile.TemporaryDirectory(prefix='yanfs-repeat-control-') as temp:
            base=Path(temp);source=self.source_fixture(base)
            code,manifest,drivers=self.run_matrix(base,source)
            self.assertEqual(0,code)
            self.assertEqual('PASS',manifest['status'])
            self.assertEqual(27,len(manifest['cases']));self.assertEqual(9,len(manifest['repeats']))
            self.assertEqual(36,len(drivers))
            self.assertEqual({4095}, {row['files'] for row in manifest['repeats']})
            for row in manifest['repeats']:
                for key in ('work','raw_status','returncode','input_sha256','capture_sha256','disk_sha256','status'):
                    self.assertIn(key,row)
                self.assertEqual('PASS',row['status'])
            first={(row['files'],row['case']):row for row in manifest['cases']}
            for row in manifest['repeats']:
                other=first[(4095,row['case'])]
                self.assertEqual(other['raw_status'],row['raw_status'])
                self.assertEqual(other['returncode'],row['returncode'])
                self.assertNotEqual(other['work'],row['work'])

    def test_bad_repeat_capture_with_unchanged_status_cannot_pass(self):
        corrupt=lambda work,case: '.repeat-4095' in work and case=='search'
        with tempfile.TemporaryDirectory(prefix='yanfs-repeat-control-') as temp:
            base=Path(temp);source=self.source_fixture(base)
            code,manifest,drivers=self.run_matrix(base,source,corrupt)
            self.assertEqual(36,len(drivers))
            self.assertEqual(1,code)
            self.assertEqual('FAIL',manifest['status'])
            # The repeat's own oracle recorded the damaged capture; the plan
            # check then rejects the record rather than trusting its status.
            self.assertIn(manifest.get('error'),('REPEAT_STATUS','REPEAT_CAPTURE'))
            failed=[row for row in manifest['cases']+manifest['repeats'] if row['status']=='FAIL']
            self.assertEqual(1,len(failed))
            repeat=[row for row in manifest['repeats'] if row['case']=='search'][0]
            self.assertEqual('FAIL',repeat['status'])
            self.assertEqual('CAPTURE_EXACT',repeat['assertion'])
            first=[row for row in manifest['cases'] if row['files']==4095 and row['case']=='search'][0]
            self.assertEqual(first['raw_status'],repeat['raw_status'])


if __name__=='__main__': unittest.main()
