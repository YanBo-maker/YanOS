"""Actual assertion owners for fault seams and lifecycle defects, with strict footer."""
import json
import os
import signal
import select
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from mcp_checks import Wire, META, tool_value, await_file, gone
from semantic_fixture import image


def direct_owner_assertion(test, err):
    if not err or not issubclass(err[0], test.failureException):
        return False
    tb=err[2]; last_project=None
    unittest_dir=Path(unittest.__file__).resolve().parent
    while tb:
        if not Path(tb.tb_frame.f_code.co_filename).resolve().is_relative_to(unittest_dir):
            last_project=tb.tb_frame.f_code
        tb=tb.tb_next
    return last_project is getattr(type(test),test._testMethodName).__code__


def owner_assertion_failed(test):
    if getattr(test,'_mcp_owner_assertion_failed',False):
        return True
    # Python 3.10 defers TestResult.addFailure until after tearDown. Newer
    # unittest versions dispatch it immediately. Check pending outcome tuples
    # with the same direct-owner rule; never relax helper/signal/SAN errors.
    return any(case is test and direct_owner_assertion(test,err)
               for case,err in getattr(test._outcome,'errors',()))


class Lifecycle(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='yan-mcp-fault-')
        self.root = Path(self.tmp.name)
        self.disk, self.index, self.model, self.pidfile = [self.root/n for n in ('disk', 'index', 'model', 'pid')]
        self.disk.write_bytes(image([('note', b'cpu interrupt')]))
        self.model.write_text('universal')
        self.options = ['--disk-image', str(self.disk), '--index', str(self.index), '--model', str(self.model),
                        '--runner', str(RUNNER), '--startup-timeout', '3', '--request-timeout', '3']
        self.env = dict(os.environ, YAN_TEST_SERVER_PID_FILE=str(self.pidfile))
        result = subprocess.run([str(SEMANTIC), 'rebuild', *self.options], capture_output=True, timeout=15)
        if result.returncode:
            raise RuntimeError(('fixture build failed', result.stderr))
        self.wire = None
        self.expected_exit = 0

    def tearDown(self):
        if self.wire:
            # Records marks only a direct owner assertion. Teardown must not
            # replace that verdict merely because this tested failure closed
            # the server with its defined fatal code. Signals/SAN still win.
            owner_failed=owner_assertion_failed(self)
            self.wire.close(rc=None if owner_failed else self.expected_exit, allow_extra=True)
        self.tmp.cleanup()

    def search(self, env=None, probe=False):
        self.wire = Wire([str(PROBE if probe else SERVER), *self.options], dict(self.env, **(env or {})))
        self.wire.send('tools/call', ident=7, params={'name': 'yanos_search', 'arguments': {'query': 'cpu'}})
        return self.wire.get()

    def test_worker_exit_checked(self):
        r = self.search({'YAN_MCP_TEST_WORKER_EXIT_FAILURE': '1'}, probe=True)
        self.assertTrue(r['result']['isError'], 'mcp_worker_exit_checked')
        self.assertNotIn('structuredContent', r['result'], 'mcp_worker_exit_no_context')

    def test_inherited_signal_state_is_owned_by_server(self):
        def inherited_state():
            signal.signal(signal.SIGCHLD, signal.SIG_IGN)
            signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGINT, signal.SIGTERM})
        self.wire = Wire([str(SERVER), *self.options], self.env, preexec_fn=inherited_state)
        self.wire.send('server/discover')
        self.wire.get()  # Synchronize after main's signal setup, before any worker.
        state = dict(line.split(':', 1) for line in
                     Path('/proc', str(self.wire.p.pid), 'status').read_text().splitlines() if ':' in line)
        self.assertEqual(0, int(state['SigIgn'], 16) & (1 << (signal.SIGCHLD-1)),
                         'mcp_child_exit_stays_waitable')
        controlled = sum(1 << (sig-1) for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGCHLD))
        self.assertEqual(0, int(state['SigBlk'], 16) & controlled, 'mcp_control_signals_unblocked')
        self.wire.send('tools/call', ident=7, params={'name': 'yanos_index_status'})
        self.assertEqual('READY', tool_value(self.wire.get(), ident=7)['state'],
                         'mcp_inherited_state_real_worker')
        self.expected_exit = 143
        self.wire.p.send_signal(signal.SIGTERM)
        self.wire.p.wait(timeout=3)

    def test_adapter_cleanup_failure(self):
        r = self.search({'YAN_MCP_TEST_CLEANUP_FAILURE': '1'}, probe=True)
        self.assertTrue(r['result']['isError'], 'mcp_cleanup_checked')
        self.assertNotIn('structuredContent', r['result'], 'mcp_cleanup_no_context')

    def test_adapter_stop_failure(self):
        r = self.search({'YAN_MCP_TEST_STOP_FAILURE': '1'}, probe=True)
        self.assertTrue(r['result']['isError'], 'mcp_stop_checked')
        self.assertNotIn('structuredContent', r['result'], 'mcp_stop_no_context')

    def test_worker_memory_failure(self):
        r = self.search({'YAN_MCP_TEST_WORKER_ALLOC_FAILURE': '1'}, probe=True)
        self.assertTrue(r['result']['isError'], 'mcp_memory_failure_no_success')
        self.assertNotIn('structuredContent', r['result'], 'mcp_memory_failure_no_context')

    def block_wire(self, env, files=None):
        self.disk.write_bytes(image(files or [('blocks', b'@block a\nhello\n')]))
        self.observer = self.root/'block-observer.log'
        self.wire = Wire([str(PROBE), *self.options],
                         dict(self.env, YAN_MCP_TEST_BLOCK_OBSERVER=str(self.observer), **env))

    def observer_lines(self):
        if not self.observer.exists():
            return []
        return [line.split() for line in self.observer.read_text().splitlines() if line.strip()]

    def first_observer_fork(self):
        deadline = time.monotonic()+8
        while True:
            for line in self.observer_lines():
                if len(line) == 2 and line[0] == 'fork':
                    return int(line[1])
            if time.monotonic() >= deadline:
                return None
            time.sleep(.01)

    def first_observer_hold(self, pid):
        deadline = time.monotonic()+8
        while True:
            for line in self.observer_lines():
                if len(line) == 2 and line[0] == 'hold' and int(line[1]) == pid:
                    return
            if time.monotonic() >= deadline:
                raise RuntimeError('harness timeout waiting for block worker hold')
            time.sleep(.01)

    def reply_code(self, reply):
        return json.loads(reply['result']['content'][0]['text'])

    def test_block_without_dependencies(self):
        self.disk.write_bytes(image([('blocks', b'@block a\nhello\n')]))
        self.wire = Wire([str(PROBE), '--disk-image', str(self.disk),
                          '--index', str(self.root/'absent-index'),
                          '--model', str(self.root/'absent-model'),
                          '--runner', str(self.root/'absent-runner'),
                          '--startup-timeout', '3', '--request-timeout', '3'], self.env)
        self.wire.send('tools/call', ident=7,
                       params={'name': 'yanos_block_list', 'arguments': {'name': 'blocks'}})
        r = self.wire.get()
        self.assertFalse(r['result']['isError'], 'block_no_dependencies_success')
        self.assertEqual(1, r['result']['structuredContent']['total'],
                         'block_without_dependencies')

    def test_block_invalid_has_no_child(self):
        # The observer is the dispatcher/source-I/O owner: the parent records a
        # fork, and the worker records direct source opens/preads before exit.
        # The semantic runner pidfile used before this correction was unrelated.
        self.block_wire({})
        for ident, tool, arguments in (
                (7, 'yanos_block_list', {'name': 'a/b'}),
                (8, 'yanos_block_read', {'name': 'blocks', 'block_id': 'a/b'}),
                (9, 'yanos_block_read',
                 {'name': 'blocks', 'block_id': 'a', 'offset': 1})):
            self.wire.send('tools/call', ident=ident,
                           params={'name': tool, 'arguments': arguments})
            r = self.wire.get()
            self.assertTrue(r['result']['isError'], 'block_invalid_is_error')
            self.assertEqual({'code': 'INVALID'}, self.reply_code(r),
                             'block_invalid_code')
            self.assertNotIn('structuredContent', r['result'],
                             'block_invalid_no_context')
        self.assertFalse(self.observer.exists(), 'block_invalid_no_worker_fork')
        self.assertEqual([], self.observer_lines(),
                         'block_invalid_no_dispatch_or_source_io')

    def test_block_valid_dispatch_and_source_io_observed(self):
        # Positive control for the observer: a valid block call must fork one
        # worker and that worker must open/read the image before its reply.
        self.block_wire({})
        self.wire.send('tools/call', ident=7,
                       params={'name': 'yanos_block_list', 'arguments': {'name': 'blocks'}})
        r = self.wire.get()
        self.assertFalse(r['result']['isError'], 'block_valid_dispatch_success')
        self.assertEqual(1, r['result']['structuredContent']['total'],
                         'block_valid_dispatch_total')
        lines = self.observer_lines()
        forks = [line for line in lines if line and line[0] == 'fork']
        children = [line for line in lines if line and line[0] == 'child']
        self.assertEqual(1, len(forks), 'block_valid_fork_observed')
        self.assertEqual(1, len(children), 'block_valid_worker_exit_observed')
        self.assertEqual([], [line for line in lines
                              if line and line[0] in ('parent-open', 'parent-pread')],
                         'block_worker_owns_source_io')
        fields = dict(item.split('=', 1) for item in children[0][2:])
        self.assertGreaterEqual(int(fields['source_opens']), 1,
                                'block_valid_source_open_observed')
        self.assertGreaterEqual(int(fields['preads']), 1,
                                'block_valid_source_read_observed')
        self.assertGreaterEqual(int(fields['block_inits']), 1,
                                'block_valid_dispatch_observed')

    def test_block_busy_cancel_and_reuse(self):
        # The test-only hold keeps the real worker (owned PID from the observer
        # fork record) alive after the block-query allocation. The slot must
        # stay BUSY until that worker and its process group are reaped.
        self.block_wire({'YAN_MCP_TEST_BLOCK_HOLD': str(self.root/'block-hold')})
        hold = self.root/'block-hold'
        self.wire.send('tools/call', ident=7,
                       params={'name': 'yanos_block_list', 'arguments': {'name': 'blocks'}})
        pid = self.first_observer_fork()
        self.assertIsNotNone(pid, 'block_worker_fork_observed')
        self.first_observer_hold(pid)
        self.wire.send('tools/call', ident=8,
                       params={'name': 'yanos_block_list', 'arguments': {'name': 'blocks'}})
        r = self.wire.get()
        self.assertEqual(8, r['id'], 'block_busy_second_id')
        self.assertEqual({'code': 'BUSY'}, self.reply_code(r), 'block_busy_boundary')
        self.wire.send('notifications/cancelled', ident=None, modern=False,
                       params={'requestId': 7})
        time.sleep(.1)
        self.wire.send('tools/call', ident=9,
                       params={'name': 'yanos_block_list', 'arguments': {'name': 'blocks'}})
        r = self.wire.get(timeout=2)
        self.assertEqual(9, r['id'], 'block_busy_before_reap_id')
        self.assertEqual({'code': 'BUSY'}, self.reply_code(r), 'block_busy_before_reap')
        self.assertFalse(gone(pid), 'block_worker_held_until_released')
        hold.write_text('go')
        deadline = time.monotonic()+10
        while not gone(pid):
            if time.monotonic() >= deadline:
                raise RuntimeError('harness timeout waiting for cancelled block worker reap')
            time.sleep(.01)
        deadline = time.monotonic()+10
        while True:
            try:
                os.killpg(pid, 0)
            except ProcessLookupError:
                break
            if time.monotonic() >= deadline:
                raise RuntimeError('harness timeout waiting for block worker group reap')
            time.sleep(.01)
        self.wire.send('tools/call', ident=10,
                       params={'name': 'yanos_block_list', 'arguments': {'name': 'blocks'}})
        r = self.wire.get()
        self.assertEqual(10, r['id'], 'block_cancel_suppressed_no_stale_reply')
        self.assertFalse(r['result']['isError'], 'block_cancel_reuse_after_reap')
        self.assertEqual(1, r['result']['structuredContent']['total'],
                         'block_cancel_reuse_after_reap')
        try:
            extra = self.wire.get(timeout=.2)
        except RuntimeError:
            extra = None
        self.assertIsNone(extra, 'block_cancel_no_stale_reply')

    def test_block_cleanup_failure(self):
        self.block_wire({'YAN_MCP_TEST_CLEANUP_FAILURE': '1'})
        self.wire.send('tools/call', ident=7,
                       params={'name': 'yanos_block_list', 'arguments': {'name': 'blocks'}})
        r = self.wire.get()
        self.assertTrue(r['result']['isError'], 'block_cleanup_is_error')
        self.assertEqual(json.loads(r['result']['content'][0]['text']), {'code': 'IO'},
                         'block_cleanup_is_error')
        self.assertNotIn('structuredContent', r['result'], 'block_cleanup_no_context')

    def test_block_close_precedence_keeps_service_error(self):
        # Cleanup/close fails in every worker; an existing service error keeps
        # its own token while plain success is the only case promoted to IO.
        self.block_wire({'YAN_MCP_TEST_CLEANUP_FAILURE': '1'},
                        files=[('blocks', b'@block a\nhello\n'),
                               ('bad', b'@yan-note 9\n@end\n'),
                               ('binary', b'@block a\n\0')])
        for ident, (name, code) in enumerate(
                (('missing', 'NOT_FOUND'), ('bad', 'METADATA'),
                 ('binary', 'INVALID_TEXT')), start=7):
            self.wire.send('tools/call', ident=ident,
                           params={'name': 'yanos_block_list', 'arguments': {'name': name}})
            r = self.wire.get()
            self.assertTrue(r['result']['isError'], 'block_close_keeps_service_error')
            self.assertEqual({'code': code}, self.reply_code(r),
                             'block_close_keeps_' + code.lower())
            self.assertNotIn('structuredContent', r['result'],
                             'block_close_keeps_no_context_' + code.lower())
        self.wire.send('tools/call', ident=10,
                       params={'name': 'yanos_block_list', 'arguments': {'name': 'blocks'}})
        r = self.wire.get()
        self.assertTrue(r['result']['isError'], 'block_close_success_becomes_io')
        self.assertEqual({'code': 'IO'}, self.reply_code(r), 'block_close_success_becomes_io')
        self.assertNotIn('structuredContent', r['result'], 'block_close_success_no_context')

    def test_block_worker_memory_failure(self):
        self.block_wire({'YAN_MCP_TEST_WORKER_ALLOC_FAILURE': '1'})
        self.wire.send('tools/call', ident=7,
                       params={'name': 'yanos_block_list', 'arguments': {'name': 'blocks'}})
        r = self.wire.get()
        self.assertTrue(r['result']['isError'], 'block_memory_is_error')
        self.assertEqual({'code': 'IO'}, self.reply_code(r), 'block_memory_is_io')
        self.assertNotIn('structuredContent', r['result'], 'block_memory_no_context')

    def test_block_bad_domain_rejected(self):
        self.block_wire({'YAN_MCP_TEST_WORKER_FRAME': 'bad-domain'})
        self.wire.send('tools/call', ident=7,
                       params={'name': 'yanos_block_list', 'arguments': {'name': 'blocks'}})
        r = self.wire.get()
        self.assertTrue(r['result']['isError'], 'block_bad_domain_is_error')
        self.assertEqual({'code': 'IO'}, self.reply_code(r), 'block_bad_domain_is_io')
        self.assertNotIn('structuredContent', r['result'], 'block_bad_domain_no_context')

    def test_block_result_out_of_range_rejected(self):
        self.block_wire({'YAN_MCP_TEST_WORKER_FRAME': 'bad-result'})
        self.wire.send('tools/call', ident=7,
                       params={'name': 'yanos_block_list', 'arguments': {'name': 'blocks'}})
        r = self.wire.get()
        self.assertTrue(r['result']['isError'], 'block_bad_result_is_error')
        self.assertEqual({'code': 'IO'}, self.reply_code(r), 'block_bad_result_is_io')
        self.assertNotIn('structuredContent', r['result'], 'block_bad_result_no_context')

    def test_block_truncated_header_rejected(self):
        self.block_wire({'YAN_MCP_TEST_WORKER_FRAME': 'truncated-header'})
        self.wire.send('tools/call', ident=7,
                       params={'name': 'yanos_block_list', 'arguments': {'name': 'blocks'}})
        r = self.wire.get()
        self.assertTrue(r['result']['isError'], 'block_truncated_header_is_error')
        self.assertEqual({'code': 'IO'}, self.reply_code(r), 'block_truncated_header_is_io')
        self.assertNotIn('structuredContent', r['result'],
                         'block_truncated_header_no_context')

    def test_block_truncated_body_rejected(self):
        self.block_wire({'YAN_MCP_TEST_WORKER_FRAME': 'truncated-body'})
        self.wire.send('tools/call', ident=7,
                       params={'name': 'yanos_block_list', 'arguments': {'name': 'blocks'}})
        r = self.wire.get()
        self.assertTrue(r['result']['isError'], 'block_truncated_body_is_error')
        self.assertEqual({'code': 'IO'}, self.reply_code(r), 'block_truncated_body_is_io')
        self.assertNotIn('structuredContent', r['result'], 'block_truncated_body_no_context')

    def test_block_extra_bytes_rejected(self):
        self.block_wire({'YAN_MCP_TEST_WORKER_FRAME': 'extra-bytes'})
        self.wire.send('tools/call', ident=7,
                       params={'name': 'yanos_block_list', 'arguments': {'name': 'blocks'}})
        r = self.wire.get()
        self.assertTrue(r['result']['isError'], 'block_extra_bytes_is_error')
        self.assertEqual({'code': 'IO'}, self.reply_code(r), 'block_extra_bytes_is_io')
        self.assertNotIn('structuredContent', r['result'], 'block_extra_bytes_no_context')

    def test_worker_pipe_close_failure(self):
        r = self.search({'YAN_MCP_TEST_WRITER_CLOSE_FAILURE': '1'}, probe=True)
        self.assertTrue(r['result']['isError'], 'mcp_worker_pipe_close_checked')
        self.assertNotIn('structuredContent', r['result'], 'mcp_worker_pipe_close_no_context')

    def test_parent_pipe_close_failure(self):
        self.wire = Wire([str(PROBE), *self.options], dict(self.env, YAN_MCP_TEST_READER_CLOSE_FAILURE='1'))
        self.wire.send('tools/call', params={'name': 'yanos_search', 'arguments': {'query': 'cpu'}})
        self.wire.p.wait(timeout=12)
        self.expected_exit = self.wire.p.returncode
        self.assertEqual(5, self.wire.p.returncode, 'mcp_parent_pipe_close_fatal')
        self.assertEqual(b'', self.wire.p.stdout.read(), 'mcp_parent_close_no_context')

    def test_proc_failure_stops_serving_and_retries_cleanup(self):
        self.wire = Wire([str(PROBE), *self.options], dict(self.env, YAN_MCP_TEST_PROC_FAILURE='1'))
        self.wire.send('tools/call', params={'name': 'yanos_search', 'arguments': {'query': 'cpu'}})
        self.wire.p.wait(timeout=12)
        self.expected_exit = self.wire.p.returncode
        self.assertEqual(5, self.wire.p.returncode, 'mcp_proc_failure_fatal')
        self.assertEqual(b'', self.wire.p.stdout.read(), 'mcp_proc_failure_no_success')
        self.assertTrue(gone(int(self.pidfile.read_text())), 'mcp_proc_failure_reaps_runner')

    def test_expired_writable_frame_cannot_escape_deadline(self):
        self.wire = Wire([str(PROBE), *self.options], dict(self.env, YAN_MCP_TEST_EXPIRED_OUTPUT='1'))
        self.wire.send('server/discover')
        if not select.select([self.wire.p.stdout],[],[],2)[0]:
            raise RuntimeError('harness expired-output fixture produced neither EOF nor a frame')
        self.assertEqual(b'', os.read(self.wire.p.stdout.fileno(),4096), 'mcp_expired_output_not_written')
        self.wire.p.wait(timeout=12)
        self.expected_exit = self.wire.p.returncode
        self.assertEqual(5, self.wire.p.returncode, 'mcp_expired_output_fatal')

    def test_runner_stdio_and_private_pipe_isolation(self):
        r = self.search({'YAN_MCP_FD_CHECK': '1'})
        self.assertFalse(r['result']['isError'], 'mcp_runner_fd_isolation')
        self.assertGreater(r['result']['structuredContent']['shown'], 0, 'mcp_runner_fd_real_search')

    def test_busy_does_not_queue_second_task(self):
        self.wire = Wire([str(SERVER), *self.options], dict(self.env, YAN_TEST_EMBED_DELAY='2'))
        self.wire.send('tools/call', ident=7, params={'name': 'yanos_search', 'arguments': {'query': 'cpu'}})
        await_file(self.pidfile)
        self.wire.send('tools/call', ident=8, params={'name': 'yanos_index_status'})
        r = self.wire.get(timeout=2)
        self.assertEqual(8, r['id'], 'mcp_busy_immediate_id')
        self.assertTrue(r['result']['isError'], 'mcp_busy_execution_error')
        self.assertEqual('{"code":"BUSY"}', r['result']['content'][0]['text'], 'mcp_busy_not_queued')
        self.wire.send('notifications/cancelled', ident=None, modern=False, params={'requestId': 7})

    def test_cancel_suppresses_response_and_reaps(self):
        self.wire = Wire([str(SERVER), *self.options], dict(self.env, YAN_TEST_EMBED_DELAY='2'))
        self.wire.send('tools/call', ident=7, params={'name': 'yanos_search', 'arguments': {'query': 'cpu'}})
        pid = await_file(self.pidfile)
        self.wire.send('notifications/cancelled', ident=None, modern=False, params={'requestId': 7})
        deadline = time.monotonic()+10
        while not gone(pid):
            if time.monotonic() >= deadline:
                raise RuntimeError('harness timeout during cancellation')
            time.sleep(.02)
        time.sleep(.1)
        self.wire.send('tools/call', ident=8, params={'name': 'yanos_index_status'})
        r = self.wire.get()
        self.assertEqual(8, r['id'], 'mcp_cancel_no_response')
        self.assertFalse(r['result']['isError'], 'mcp_cancel_reuse_after_cleanup')

    def test_crashed_worker_reaps_all_generations(self):
        descendants = self.root/'descendants'
        self.wire = Wire([str(SERVER), *self.options], dict(self.env, YAN_TEST_EMBED_DELAY='2',
            YAN_TEST_FORK_TREE='1', YAN_TEST_DESCENDANTS_FILE=str(descendants)))
        self.wire.send('tools/call', params={'name': 'yanos_search', 'arguments': {'query': 'cpu'}})
        runner = await_file(self.pidfile)
        deadline=time.monotonic()+8
        while not descendants.exists() or len(descendants.read_text().split())<2:
            if time.monotonic()>=deadline:
                raise RuntimeError('harness fork tree did not start')
            time.sleep(.01)
        pids = [runner, *map(int, descendants.read_text().split())]
        children=Path('/proc',str(self.wire.p.pid),'task',str(self.wire.p.pid),'children').read_text().split()
        if len(children)!=1:
            raise RuntimeError('harness worker PID is ambiguous')
        os.kill(int(children[0]),signal.SIGKILL)
        reply=self.wire.get()
        self.assertTrue(reply['result']['isError'], 'mcp_crashed_worker_no_success')
        self.assertTrue(all(gone(pid) for pid in pids), 'mcp_multigeneration_reaped_before_reply')


class Records(unittest.TestResult):
    """Only test assertions count as FAIL. Setup/teardown, timeouts, process
    errors, missing output and arbitrary exceptions are harness errors."""
    def __init__(self):
        super().__init__(); self.records = []; self.harness = []

    def addSuccess(self, test):
        super().addSuccess(test); self.records.append((test._testMethodName, 'PASS', ''))

    def addFailure(self, test, err):
        # TestResult formats/cleans traceback links. Capture the originating
        # project frame before calling super; function-name heuristics after
        # formatting reject valid assertions on some Python versions.
        direct_owner=direct_owner_assertion(test,err)
        if direct_owner:
            test._mcp_owner_assertion_failed=True
        super().addFailure(test, err)
        if not direct_owner:
            self.harness.append(test._testMethodName+': '+str(err[1])); return
        self.records.append((test._testMethodName, 'FAIL', str(err[1]).replace('\n', ' ')))

    def addError(self, test, err):
        super().addError(test, err); self.harness.append(test._testMethodName+': '+str(err[1]))


def format_records(result):
    lines = ['mcp_fault_checks.py:1:' + name + ':' + status + (': ' + detail if detail else '')
             for name, status, detail in result.records]
    failures = len(result.failures)
    lines += ['', '-----------------------', f'{result.testsRun} Tests {failures} Failures 0 Ignored',
              'FAIL' if failures else 'OK', '']
    return '\n'.join(lines)


def main():
    global SERVER, PROBE, SEMANTIC, RUNNER
    SERVER, PROBE, SEMANTIC, RUNNER = map(lambda p: Path(p).resolve(), sys.argv[1:])
    for p in (SERVER, PROBE, SEMANTIC, RUNNER):
        if not p.is_file():
            raise RuntimeError('FAIL required implementation/fixture missing: ' + str(p))
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(Lifecycle)
    result = Records(); suite.run(result)
    if result.harness or len(result.records) != result.testsRun:
        print('harness:', result.harness, file=sys.stderr); return 2
    failures = len(result.failures)
    print(format_records(result), end='')
    return failures


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.TimeoutExpired, OSError) as exc:
        print('harness:', exc, file=sys.stderr)
        raise SystemExit(2)
