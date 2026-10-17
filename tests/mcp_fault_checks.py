"""Actual assertion owners for fault seams and lifecycle defects, with strict footer."""
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
