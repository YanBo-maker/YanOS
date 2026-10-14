"""Default optional-provider contract tests use a real owned fake child."""
from pathlib import Path
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time

def main():
    probe, runner = map(Path, sys.argv[1:])
    for item in (probe, runner):
        if not item.is_file(): raise RuntimeError('FAIL implementation/fixture missing: '+str(item))
    cases = [('valid','OK'), ('dimension','PROVIDER'), ('zero','PROVIDER'),
             ('nan','PROVIDER'), ('huge','PROVIDER'), ('underflow','PROVIDER'), ('index','PROVIDER'),
             ('extra','PROVIDER'), ('malformed','PROVIDER'), ('oversize','PROVIDER'),
             ('redirect','PROVIDER'), ('http_error','PROVIDER'),
             ('timeout','TIMEOUT'), ('startup_timeout','TIMEOUT'),
             ('early_exit','PROVIDER'), ('wrong_auth','PROVIDER'), ('ignore_term','PROVIDER')]
    count = 0
    with tempfile.TemporaryDirectory(prefix='yan-semantic-provider-') as tmp:
        model = Path(tmp)/'model'
        for mode, expected in cases:
            model.write_text(mode)
            run = subprocess.run([str(probe),str(model),str(runner),'0','normal','embed'],
                                 capture_output=True, text=True, timeout=12)
            assert run.returncode == 0, (mode,run.returncode,run.stderr)
            assert run.stdout == 'RESULT %s reaped=1\n'%expected, (mode,run.stdout,run.stderr)
            count += 1
        model.write_text('valid')
        run = subprocess.run([str(probe),str(model),str(runner),'0','cancel','embed'],
                             capture_output=True,text=True,timeout=12)
        assert run.returncode == 0 and run.stdout == 'RESULT CANCELLED reaped=1\n', run
        count += 1
        pidfile=Path(tmp)/'version.pid'
        for mode,expected in [('empty','PROVIDER'),('long','PROVIDER'),('timeout','TIMEOUT')]:
            env=dict(os.environ,YAN_TEST_VERSION_MODE=mode,YAN_TEST_PID_FILE=str(pidfile))
            run=subprocess.run([str(probe),str(model),str(runner),'0','normal','version'],
                               env=env,capture_output=True,text=True,timeout=12)
            assert run.returncode==0 and run.stdout=='RESULT %s reaped=1\n'%expected,(mode,run)
            if mode=='timeout':
                assert Path(str(pidfile)+'.term').read_text()=='TERM'
                try: os.kill(int(pidfile.read_text()),0)
                except ProcessLookupError: pass
                else: raise AssertionError('version child survived cleanup')
            count+=1
        model.write_text('delayed_exit')
        run = subprocess.run([str(probe),str(model),str(runner),'0','normal','exit_stop'],
                             capture_output=True,text=True,timeout=12)
        assert run.returncode == 0 and run.stdout == 'RESULT PROVIDER reaped=1\n', run
        count += 1
        # An unrelated listener stays ours; the provider must never terminate it
        # or accept its unauthenticated response as readiness for another child.
        with socket.socket() as listener:
            listener.bind(('127.0.0.1',0)); listener.listen()
            port = listener.getsockname()[1]
            run = subprocess.run([str(probe),str(model),str(runner),str(port),'normal','embed'],
                                 capture_output=True,text=True,timeout=12)
            assert run.returncode == 0 and run.stdout == 'RESULT PROVIDER reaped=1\n', run
            assert listener.fileno() >= 0 and listener.getsockname()[1] == port
        count += 1
        model.write_text('timeout')
        proc = subprocess.Popen([str(probe),str(model),str(runner),'0','normal','embed'],
                                stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        try:
            time.sleep(0.35); proc.send_signal(signal.SIGINT)
            out, err = proc.communicate(timeout=12)
            assert proc.returncode == 0 and out == 'RESULT CANCELLED reaped=1\n', (out,err)
        finally:
            if proc.poll() is None: proc.kill(); proc.communicate()
        count += 1
    print('PASS semantic provider lifecycle: %d cases'%count)
if __name__ == '__main__': main()
