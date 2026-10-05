#!/usr/bin/env python3
"""Behavioural tests for tools/yan_shell.py (0022's Host TTY launcher).

The launcher is observed through a real executable child, not a mock: this file
writes a small Python program to a temporary directory, makes it executable and
lets the launcher exec it (no shell anywhere). The child records its argv, its
PID and the termios attributes it sees on fd 0 into files named by environment
variables, optionally echoes standard input, and exits with a chosen status.

The TTY cases open a real PTY with pty.openpty(), set ECHONL before the run and
require the child to observe ICANON/ECHO/ECHONL cleared and ISIG preserved with
VMIN=1/VTIME=0, then require the complete original attributes - including the
ECHONL this test set - to be back afterwards. That is what distinguishes a real
termios change from a fake one. The pipe case is kept separate and never claims
to be a TTY.

Three further cases import the launcher as a module inside a test subprogram and
monkeypatch termios.tcsetattr / subprocess.Popen there to send the signal to the
subprogram's own PID in three exact windows: right after the raw attributes are
set, inside the Popen call (the child exists but the launcher has not seen it
yet), and just before the restore. The injection lives only in this test; the
production launcher reads no environment variable for it. Each must return
128 + signal, leave no child behind and give the complete original attributes
back.

Usage: test_shell_launcher.py --launcher PATH --work DIR
Exit: 0 all cases passed, 1 a case failed, 2 a missing platform facility.
Every subprocess call has a finite timeout; a timeout is a failure, never a
pass. A missing pty/termios facility is a hard error on this Linux target.
"""

import argparse
import json
import os
import pty
import shutil
import signal
import subprocess
import sys
import tempfile
import termios
import time

FAKE_BODY = """\
import json
import os
import sys
import termios
import time


def log(path, text):
    if not path:
        return
    with open(path, "a") as handle:
        handle.write(text + "\\n")


log(os.environ.get("FAKE_ARGV_LOG"), repr(sys.argv[1:]))
log(os.environ.get("FAKE_PID_LOG"), str(os.getpid()))

attr_log = os.environ.get("FAKE_ATTR_LOG")
if attr_log:
    try:
        attrs = termios.tcgetattr(0)
        cc = attrs[6]
        vmin = cc[termios.VMIN]
        vtime = cc[termios.VTIME]
        if isinstance(vmin, (bytes, bytearray)):
            vmin = vmin[0]
        if isinstance(vtime, (bytes, bytearray)):
            vtime = vtime[0]
        record = {"lflag": attrs[3], "vmin": vmin, "vtime": vtime}
    except Exception as error:  # a pipe is not a terminal: an answer, not a bug
        record = {"error": repr(error)}
    log(attr_log, json.dumps(record))

if os.environ.get("FAKE_ECHO"):
    data = sys.stdin.buffer.read()
    sys.stdout.buffer.write(data)
    sys.stdout.buffer.flush()

if os.environ.get("FAKE_IGNORE_SIGNALS"):
    import signal as _signal
    _signal.signal(_signal.SIGINT, _signal.SIG_IGN)
    _signal.signal(_signal.SIGTERM, _signal.SIG_IGN)
    log(os.environ.get("FAKE_READY_LOG"), "ignoring-signals")

sleep = float(os.environ.get("FAKE_SLEEP", "0"))
if sleep > 0:
    time.sleep(sleep)

sys.exit(int(os.environ.get("FAKE_EXIT_CODE", "0")))
"""

INJECTED_BODY = """\
import importlib.util
import os
import signal
import subprocess
import sys
import termios
import time

launcher_path = sys.argv[1]
window = sys.argv[2]
signum = int(sys.argv[3])
launcher_argv = sys.argv[4:]

spec = importlib.util.spec_from_file_location("yan_shell_under_test", launcher_path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def wait_for_file(path, timeout=20.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path and os.path.exists(path) and os.path.getsize(path) > 0:
            return True
        time.sleep(0.05)
    return False


_real_tcsetattr = termios.tcsetattr
_calls = {"tcsetattr": 0}


def injecting_tcsetattr(fd, when, attrs, *args, **kwargs):
    _calls["tcsetattr"] += 1
    if window == "raw-set" and _calls["tcsetattr"] == 1:
        result = _real_tcsetattr(fd, when, attrs, *args, **kwargs)
        os.kill(os.getpid(), signum)  # right after the raw switch
        return result
    if window == "before-restore" and _calls["tcsetattr"] == 2:
        os.kill(os.getpid(), signum)  # just before the restore
        return _real_tcsetattr(fd, when, attrs, *args, **kwargs)
    if window == "setup-partial" and _calls["tcsetattr"] == 1:
        # Apply the raw attributes for real, then report a termios failure, so
        # the launcher's restore must run over a terminal it half changed.
        _real_tcsetattr(fd, when, attrs, *args, **kwargs)
        raise termios.error("injected: failed after applying raw attributes")
    if window == "restore-error" and _calls["tcsetattr"] == 2:
        # Restore for real, then report a failure: the TTY is healthy but the
        # launcher must still treat the failed restore as a startup error.
        _real_tcsetattr(fd, when, attrs, *args, **kwargs)
        raise termios.error("injected: restore failed")
    return _real_tcsetattr(fd, when, attrs, *args, **kwargs)


termios.tcsetattr = injecting_tcsetattr

_real_popen = subprocess.Popen


def injecting_popen(*args, **kwargs):
    proc = _real_popen(*args, **kwargs)
    wait_log = os.environ.get("TEST_CHILD_WAIT_LOG")
    if wait_log:
        real_wait = proc.wait
        def recording_wait(*wait_args, **wait_kwargs):
            code = real_wait(*wait_args, **wait_kwargs)
            with open(wait_log, "a") as handle:
                handle.write(str(code) + "\\n")
            return code
        proc.wait = recording_wait
    if window == "popen":
        # The child exists but the launcher has not returned from Popen yet, so
        # its own reference is still None: this is the window the fix covers.
        wait_for_file(os.environ.get("FAKE_PID_LOG"))
        os.kill(os.getpid(), signum)
    return proc


subprocess.Popen = injecting_popen

# The escalation test shortens the grace period here, in the test subprogram;
# the production launcher reads no environment variable for its timing.
module.GRACE_SECONDS = float(os.environ.get("TEST_GRACE_SECONDS",
                                            module.GRACE_SECONDS))

sys.exit(module.main(launcher_argv))
"""

CASES = []
LAUNCHER = None
WORK = None
TMP = None
FAKE = None
GUEST = None
DISK = None


class Failure(Exception):
    """A failed behavioural expectation (not a harness error)."""


def case(function):
    CASES.append(function)
    return function


def check(condition, message):
    if not condition:
        raise Failure(message)


def launcher_command(*extra):
    return [sys.executable, "-B", LAUNCHER, "--run", FAKE,
            "--guest", GUEST, "--disk-image", DISK] + list(extra)


def fake_env(name, **values):
    env = dict(os.environ)
    env["FAKE_ARGV_LOG"] = os.path.join(TMP, name + ".argv")
    env["FAKE_PID_LOG"] = os.path.join(TMP, name + ".pid")
    env["FAKE_ATTR_LOG"] = os.path.join(TMP, name + ".attr")
    for key, value in values.items():
        env[key] = str(value)
    return env


def read_one(path):
    with open(path, "r") as handle:
        for line in handle:
            line = line.strip()
            if line:
                return line
    raise Failure("no record in %s" % path)


def wait_for_record(path, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if os.path.exists(path) and os.path.getsize(path) > 0:
            return
        time.sleep(0.05)
    raise Failure("timed out waiting for %s" % path)


def process_gone(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return True
    except PermissionError:
        return False
    return False


def close_pty(master, slave):
    for descriptor in (master, slave):
        try:
            os.close(descriptor)
        except OSError:
            pass


def make_pty_with_echonl():
    master, slave = pty.openpty()
    attrs = termios.tcgetattr(slave)
    attrs[3] |= termios.ECHONL
    termios.tcsetattr(slave, termios.TCSANOW, attrs)
    return master, slave


def require_raw_mode(record):
    check("lflag" in record, "the child did not report termios attributes: %r" % record)
    lflag = record["lflag"]
    check((lflag & termios.ICANON) == 0, "ICANON was not cleared")
    check((lflag & termios.ECHO) == 0, "ECHO was not cleared")
    check((lflag & termios.ECHONL) == 0, "ECHONL was not cleared")
    check((lflag & termios.ISIG) != 0, "ISIG was not preserved")
    check(record["vmin"] == 1, "VMIN is %r, expected 1" % record["vmin"])
    check(record["vtime"] == 0, "VTIME is %r, expected 0" % record["vtime"])


@case
def pty_child_sees_raw_mode_and_status_6_is_passed_through():
    name = "pty6"
    env = fake_env(name, FAKE_EXIT_CODE=6)
    master, slave = make_pty_with_echonl()
    try:
        before = termios.tcgetattr(slave)
        result = subprocess.run(launcher_command(), stdin=slave, stdout=slave,
                                stderr=subprocess.PIPE, env=env, timeout=60)
        check(result.returncode == 6,
              "launcher returned %d, expected the child's 6" % result.returncode)
        require_raw_mode(json.loads(read_one(os.path.join(TMP, name + ".attr"))))
        after = termios.tcgetattr(slave)
        check(after == before, "terminal attributes were not restored exactly")
        pid = int(read_one(os.path.join(TMP, name + ".pid")))
        check(process_gone(pid), "child process %d still exists" % pid)
    finally:
        close_pty(master, slave)


@case
def pty_restores_the_original_echonl_bit():
    # The previous case already compares the whole attribute list; this one
    # states the property the test is built around in the failure message.
    name = "echonl"
    env = fake_env(name, FAKE_EXIT_CODE=0)
    master, slave = make_pty_with_echonl()
    try:
        before = termios.tcgetattr(slave)
        check((before[3] & termios.ECHONL) != 0, "the fixture could not set ECHONL")
        result = subprocess.run(launcher_command(), stdin=slave, stdout=slave,
                                stderr=subprocess.PIPE, env=env, timeout=60)
        check(result.returncode == 0, "launcher returned %d" % result.returncode)
        after = termios.tcgetattr(slave)
        check((after[3] & termios.ECHONL) != 0,
              "the ECHONL this test set was not restored")
        check(after == before, "the full attribute list was not restored")
    finally:
        close_pty(master, slave)


def run_signal_case(name, signum):
    env = fake_env(name, FAKE_SLEEP=30, FAKE_EXIT_CODE=0)
    master, slave = make_pty_with_echonl()
    proc = None
    try:
        before = termios.tcgetattr(slave)
        proc = subprocess.Popen(launcher_command(), stdin=slave, stdout=slave,
                                stderr=subprocess.PIPE, env=env)
        pid_path = os.path.join(TMP, name + ".pid")
        wait_for_record(pid_path, 20)
        child_pid = int(read_one(pid_path))
        proc.send_signal(signum)
        code = proc.wait(timeout=30)
        check(code == 128 + signum,
              "launcher returned %d, expected %d" % (code, 128 + signum))
        check(process_gone(child_pid), "child process %d survived" % child_pid)
        after = termios.tcgetattr(slave)
        check(after == before, "terminal attributes were not restored after a signal")
    finally:
        if proc is not None and proc.poll() is None:
            proc.kill()
            proc.wait()
        close_pty(master, slave)


@case
def sigint_is_forwarded_and_reported_as_128_plus_2():
    run_signal_case("sigint", signal.SIGINT)


@case
def sigterm_is_forwarded_and_reported_as_128_plus_15():
    run_signal_case("sigterm", signal.SIGTERM)


def write_injected(path):
    with open(path, "w") as handle:
        handle.write("#!" + sys.executable + "\n")
        handle.write(INJECTED_BODY)
    os.chmod(path, 0o755)


def run_injected_window(name, window, signum, expect_code, expect_child):
    injected = os.path.join(TMP, "injected_" + name + ".py")
    write_injected(injected)
    env = fake_env(name, FAKE_EXIT_CODE=0,
                   FAKE_SLEEP=(30 if window == "popen" else 0))
    master, slave = make_pty_with_echonl()
    try:
        before = termios.tcgetattr(slave)
        result = subprocess.run(
            [sys.executable, "-B", injected, LAUNCHER, window, str(int(signum)),
             "--run", FAKE, "--guest", GUEST, "--disk-image", DISK],
            stdin=slave, stdout=slave, stderr=subprocess.PIPE, env=env,
            timeout=60)
        check(result.returncode == expect_code,
              "%s window returned %d, expected %d; stderr=%r"
              % (window, result.returncode, expect_code, result.stderr))
        check(termios.tcgetattr(slave) == before,
              "terminal attributes were not restored in the %s window" % window)
        pid_path = os.path.join(TMP, name + ".pid")
        if expect_child:
            child_pid = int(read_one(pid_path))
            check(process_gone(child_pid),
                  "child %d survived the %s window" % (child_pid, window))
        else:
            check(not os.path.exists(pid_path),
                  "a child was started although the signal arrived before Popen")
    finally:
        close_pty(master, slave)


@case
def signal_after_raw_set_does_not_start_a_child():
    run_injected_window("raw_window", "raw-set", signal.SIGTERM, 128 + 15, False)


@case
def signal_during_popen_is_forwarded_to_the_new_child():
    run_injected_window("popen_window", "popen", signal.SIGTERM, 128 + 15, True)


@case
def signal_before_restore_returns_128_plus_signal():
    run_injected_window("restore_window", "before-restore", signal.SIGINT, 128 + 2,
                        True)


def run_injected_plain(name, window, expect_code, expect_child, **env_values):
    """Run the launcher through a test subprogram with no signal, on a real PTY.

    The subprogram imports the launcher and injects a termios failure at the
    named window; only the tests carry that injection."""
    injected = os.path.join(TMP, "injected_" + name + ".py")
    write_injected(injected)
    env = fake_env(name, FAKE_EXIT_CODE=0, **env_values)
    master, slave = make_pty_with_echonl()
    try:
        before = termios.tcgetattr(slave)
        result = subprocess.run(
            [sys.executable, "-B", injected, LAUNCHER, window, "0",
             "--run", FAKE, "--guest", GUEST, "--disk-image", DISK],
            stdin=slave, stdout=slave, stderr=subprocess.PIPE, env=env,
            timeout=60)
        check(result.returncode == expect_code,
              "%s window returned %d, expected %d; stderr=%r"
              % (window, result.returncode, expect_code, result.stderr))
        check(termios.tcgetattr(slave) == before,
              "terminal attributes were not restored in the %s window" % window)
        pid_path = os.path.join(TMP, name + ".pid")
        if expect_child:
            child_pid = int(read_one(pid_path))
            check(process_gone(child_pid),
                  "child %d survived the %s window" % (child_pid, window))
        else:
            check(not os.path.exists(pid_path),
                  "a child was started although the %s window failed early"
                  % window)
        return result
    finally:
        close_pty(master, slave)


@case
def actual_popen_oserror_restores_the_terminal():
    # The executor exists and is executable, so the preflight passes, but its
    # shebang names an interpreter that does not exist: only the real Popen
    # fails. The launcher must report it as a startup error and still restore
    # the terminal, and no child may be left behind.
    name = "bad_exec"
    broken = os.path.join(TMP, "bad_executable")
    with open(broken, "w") as handle:
        handle.write("#!/nonexistent/interpreter\nnot a real program\n")
    os.chmod(broken, 0o755)
    check(os.path.isfile(broken) and os.access(broken, os.X_OK),
          "the broken executor did not pass the preflight checks")
    env = fake_env(name, FAKE_EXIT_CODE=0)
    master, slave = make_pty_with_echonl()
    try:
        before = termios.tcgetattr(slave)
        result = subprocess.run(
            [sys.executable, "-B", LAUNCHER, "--run", broken,
             "--guest", GUEST, "--disk-image", DISK],
            stdin=slave, stdout=slave, stderr=subprocess.PIPE, env=env,
            timeout=60)
        check(result.returncode == 5,
              "launcher returned %d, expected the startup error 5; stderr=%r"
              % (result.returncode, result.stderr))
        check(b"cannot start" in result.stderr,
              "the real Popen failure was not named on stderr: %r"
              % result.stderr)
        check(termios.tcgetattr(slave) == before,
              "terminal attributes were not restored after the Popen failure")
        check(not os.path.exists(os.path.join(TMP, name + ".pid")),
              "a child record exists although the real Popen failed")
    finally:
        close_pty(master, slave)


@case
def setup_tcsetattr_error_after_partial_apply_restores():
    result = run_injected_plain("setup_partial", "setup-partial", 5, False)
    check(b"terminal setup failed" in result.stderr,
          "the setup failure was not reported: %r" % result.stderr)


@case
def restore_error_reports_and_returns_5():
    result = run_injected_plain("restore_error", "restore-error", 5, True)
    check(b"cannot restore terminal attributes" in result.stderr,
          "the restore failure was not reported: %r" % result.stderr)


@case
def own_child_that_ignores_signals_is_escalated_and_reaped():
    name = "escalate"
    injected = os.path.join(TMP, "injected_" + name + ".py")
    write_injected(injected)
    # The child ignores SIGINT and SIGTERM, so the launcher has to escalate to
    # kill after the (shortened) grace period and still reap it.
    env = fake_env(name, FAKE_SLEEP=30, FAKE_EXIT_CODE=0, FAKE_IGNORE_SIGNALS=1,
                   TEST_GRACE_SECONDS=0.1)
    env["FAKE_READY_LOG"] = os.path.join(TMP, name + ".ready")
    env["TEST_CHILD_WAIT_LOG"] = os.path.join(TMP, name + ".wait")
    master, slave = make_pty_with_echonl()
    proc = None
    try:
        before = termios.tcgetattr(slave)
        proc = subprocess.Popen(
            [sys.executable, "-B", injected, LAUNCHER, "none", "0",
             "--run", FAKE, "--guest", GUEST, "--disk-image", DISK],
            stdin=slave, stdout=slave, stderr=subprocess.PIPE, env=env)
        pid_path = os.path.join(TMP, name + ".pid")
        wait_for_record(pid_path, 20)
        child_pid = int(read_one(pid_path))
        # PID creation precedes the fake's signal handlers. Wait until the
        # child has really installed SIG_IGN before testing escalation.
        wait_for_record(env["FAKE_READY_LOG"], 20)
        proc.send_signal(signal.SIGTERM)
        code = proc.wait(timeout=30)
        check(code == 128 + signal.SIGTERM,
              "launcher returned %d, expected 143" % code)
        check(process_gone(child_pid),
              "the signal-ignoring child %d survived the escalation" % child_pid)
        check(int(read_one(env["TEST_CHILD_WAIT_LOG"])) == -signal.SIGKILL,
              "the child did not actually terminate through SIGKILL")
        check(termios.tcgetattr(slave) == before,
              "terminal attributes were not restored after the escalation")
    finally:
        if proc is not None and proc.poll() is None:
            proc.kill()
            proc.wait()
        close_pty(master, slave)


@case
def pipe_preserves_data_and_carries_a_small_step_limit():
    name = "pipe"
    env = fake_env(name, FAKE_ECHO=1, FAKE_EXIT_CODE=0)
    result = subprocess.run(launcher_command("--max-steps", "123"),
                            input=b"hello from the pipe\n",
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            env=env, timeout=60)
    check(result.returncode == 0, "launcher returned %d" % result.returncode)
    check(result.stdout == b"hello from the pipe\n",
          "data through the pipe changed: %r" % result.stdout)
    argv = read_one(os.path.join(TMP, name + ".argv"))
    check("'--max-steps', '123'" in argv,
          "the executor argv did not carry --max-steps 123: %s" % argv)
    record = json.loads(read_one(os.path.join(TMP, name + ".attr")))
    check("error" in record,
          "the child on a pipe could read terminal attributes: %r" % record)


@case
def default_step_limit_is_uint64_max():
    name = "default"
    env = fake_env(name, FAKE_ECHO=1, FAKE_EXIT_CODE=0)
    result = subprocess.run(launcher_command(), input=b"",
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            env=env, timeout=60)
    check(result.returncode == 0, "launcher returned %d" % result.returncode)
    argv = read_one(os.path.join(TMP, name + ".argv"))
    check("18446744073709551615" in argv,
          "the default step limit was not UINT64_MAX: %s" % argv)


@case
def nonexistent_executor_is_a_clear_startup_error():
    missing = os.path.join(TMP, "no-such-executor")
    result = subprocess.run(
        [sys.executable, "-B", LAUNCHER, "--run", missing,
         "--guest", GUEST, "--disk-image", DISK],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
    check(result.returncode == 5,
          "launcher returned %d, expected the startup error 5" % result.returncode)
    check(b"no-such-executor" in result.stderr,
          "the missing executor was not named on stderr: %r" % result.stderr)


@case
def bad_max_steps_is_a_usage_error():
    for value in ("0", "18446744073709551616", "not-a-number"):
        result = subprocess.run(launcher_command("--max-steps", value),
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                timeout=30)
        check(result.returncode == 2,
              "--max-steps %s returned %d, expected usage 2"
              % (value, result.returncode))


def write_fake(path):
    with open(path, "w") as handle:
        handle.write("#!" + sys.executable + "\n")
        handle.write(FAKE_BODY)
    os.chmod(path, 0o755)


def main():
    parser = argparse.ArgumentParser(
        description="behavioural tests for the YanOS terminal launcher")
    parser.add_argument("--launcher", required=True)
    parser.add_argument("--work", required=True)
    args = parser.parse_args()

    global LAUNCHER, WORK, TMP, FAKE, GUEST, DISK
    LAUNCHER = os.path.abspath(args.launcher)
    WORK = os.path.abspath(args.work)
    if not os.path.isfile(LAUNCHER):
        print("HARNESS-ERROR: launcher not found: %s" % LAUNCHER, file=sys.stderr)
        return 2
    os.makedirs(WORK, exist_ok=True)
    TMP = tempfile.mkdtemp(prefix="launcher-case-", dir=WORK)
    try:
        FAKE = os.path.join(TMP, "fake_runner.py")
        write_fake(FAKE)
        GUEST = os.path.join(TMP, "guest.elf")
        with open(GUEST, "wb") as handle:
            handle.write(b"\x7fELF")
        DISK = os.path.join(TMP, "disk.img")
        with open(DISK, "wb") as handle:
            handle.write(b"\x00" * 16)

        passed = 0
        failed = 0
        for function in CASES:
            name = function.__name__
            try:
                function()
            except Failure as error:
                failed += 1
                print("FAIL %s: %s" % (name, error), flush=True)
            except subprocess.TimeoutExpired as error:
                failed += 1
                print("FAIL %s: timeout (%s)" % (name, error), flush=True)
            except Exception as error:  # harness problem in this case
                failed += 1
                print("FAIL %s: harness exception %r" % (name, error), flush=True)
            else:
                passed += 1
                print("PASS %s" % name, flush=True)
        total = passed + failed
        print("%d/%d cases passed" % (passed, total), flush=True)
        return 0 if failed == 0 else 1
    finally:
        shutil.rmtree(TMP, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
