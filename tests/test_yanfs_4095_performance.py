"""0040-R4 negative controls; no compiler, Guest or model dependency."""
import copy
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
                if name=='fill63': (work/'reference63.img').write_bytes(oracle.fixture(63))
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


if __name__=='__main__': unittest.main()
