#!/usr/bin/env python3
"""0036/R7 test: semantic_fake_runner publishes complete PID files atomically.

Runs the actual runner entrypoints. It observes Path.write_text after the file
is created but before bytes are written, so an old direct write to the final
PID path is visible-empty and fails; the atomic publisher writes a temporary
file and replaces it, so the final path stays absent until complete.
"""
import contextlib
import io
import os
from pathlib import Path
import runpy
import signal
import sys
import tempfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_RUNNER = ROOT / 'tests' / 'semantic_fake_runner.py'


def run_entry(runner, entry):
    with tempfile.TemporaryDirectory(prefix='runner-pid-') as tmp:
        root = Path(tmp)
        pids = root / 'pids'
        pids.mkdir()
        final = pids / 'ready.pid'
        model = root / 'model'
        model.write_text('universal')
        observed = []
        real_read_text = Path.read_text

        def write_text(path, text, *args, **kwargs):
            with path.open('w', encoding='utf-8') as stream:
                observed.append({
                    'opened': path.name,
                    'final_visible_before_write': final.exists(),
                })
                return stream.write(text)

        def read_text(path, *args, **kwargs):
            if path == model:
                raise SystemExit(0)
            return real_read_text(path, *args, **kwargs)

        if entry == 'version':
            env = {'YAN_TEST_PID_FILE': str(final),
                   'YAN_TEST_VERSION_MODE': 'timeout'}
            argv = [str(runner), '--version']
        else:
            env = {'YAN_TEST_SERVER_PID_FILE': str(final)}
            argv = [str(runner), '--model', str(model), '--port', '1',
                    '--host', '127.0.0.1', '--api-key', 'fixture']

        old_term = signal.getsignal(signal.SIGTERM)
        try:
            with patch.dict(os.environ, env, clear=True), \
                 patch.object(sys, 'argv', argv), \
                 patch.object(Path, 'write_text', write_text), \
                 patch.object(Path, 'read_text', read_text), \
                 patch('time.sleep', lambda _: None), \
                 contextlib.redirect_stdout(io.StringIO()):
                try:
                    runpy.run_path(str(runner), run_name='__main__')
                except SystemExit as ended:
                    assert ended.code in (0, None), (entry, 'runner exit', ended.code)
        finally:
            signal.signal(signal.SIGTERM, old_term)

        assert len(observed) == 1, (entry, 'PID publication not observed once', observed)
        assert observed[0]['final_visible_before_write'] is False, \
            (entry, 'final PID path visible before complete bytes', observed[0])
        assert final.read_text() == str(os.getpid()), \
            (entry, 'published PID is not this process', final.read_text())
        leftovers = sorted(p.name for p in pids.iterdir() if p.name != final.name)
        assert leftovers == [], (entry, 'temporary PID leftovers', leftovers)
        return observed[0]


def main(argv):
    if len(argv) > 1:
        raise SystemExit('usage: test_semantic_runner_pid.py [runner]')
    runner = Path(argv[0]).resolve() if argv else DEFAULT_RUNNER
    if not runner.is_file():
        raise SystemExit('FAIL semantic_fake_runner fixture missing: ' + str(runner))
    for entry in ('version', 'server'):
        run_entry(runner, entry)
    print('PASS semantic runner PID publication version+server')


if __name__ == '__main__':
    main(sys.argv[1:])
