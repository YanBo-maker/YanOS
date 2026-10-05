#!/usr/bin/env python3
"""Host TTY launcher for the production Guest terminal (0022).

This script does exactly three things: validate that the paths it was given
exist, put the controlling terminal into the mode the line editor expects, and
exec the existing ``yan_run`` executor with the production Guest, the terminal
and the disk image. It never builds a Guest, never formats or creates an image,
and never touches a configuration file. Whether an existing image is a regular
file is the executor's final judgment, not this script's.

Terminal mode
-------------
When standard input is a TTY the complete original attributes are saved and a
deep copy is switched to VMIN=1/VTIME=0 with ICANON, ECHO and ECHONL cleared.
ISIG and every other bit is preserved, so Ctrl-C still reaches the foreground
process group. A pipe is left untouched. The original attributes are restored
on every exit path: normal return, a failed explicit error, Ctrl-C and SIGTERM.

Signals
-------
The SIGINT/SIGTERM handlers are installed *before* the terminal is changed and
stay installed until the terminal has been restored and the child this script
created has been cleaned up; they are restored last. That closes the windows
where a default disposition would have terminated the script with the TTY left
raw. A signal that arrives before the child exists is remembered: the child is
not started at all and the script returns 128 + signal. A signal that arrives
during Popen is forwarded to the child as soon as it exists, before any grace
period. The child is addressed by its own PID only - never its process group or
any other process. After a bounded grace period the child is terminated and
then killed. The script returns 128 + signal for a signalled run, and the
child's own non-negative exit status otherwise.

Exit codes: the child's status, 128 + signal when the run was signalled, 2 for
a usage error (argparse), 5 for a startup or terminal-restore failure.
"""

import argparse
import copy
import os
import signal
import subprocess
import sys
import termios
import time

UINT64_MAX = (1 << 64) - 1
GRACE_SECONDS = 3.0
STARTUP_ERROR = 5
# termios raises its own error class on some builds and OSError on others; a
# terminal problem on any of them is a startup failure, not a silent success.
TERMINAL_ERRORS = (OSError, termios.error)


def positive_max_steps(text):
    """argparse type for --max-steps: a positive integer that fits in u64."""
    try:
        value = int(text, 10)
    except ValueError:
        raise argparse.ArgumentTypeError("must be a base-10 integer")
    if value <= 0 or value > UINT64_MAX:
        raise argparse.ArgumentTypeError(
            "must be in 1..%d (a very large finite step limit is allowed)" % UINT64_MAX)
    return value


def _set_cc(attrs, index, value):
    """Set one termios control character; the cc entries may be bytes or ints."""
    cc = attrs[6]
    if isinstance(cc[index], (bytes, bytearray)):
        cc[index] = bytes([value])
    else:
        cc[index] = value


def _raw_attributes(saved):
    """A deep copy of `saved` with line editing off and one-byte reads."""
    raw = copy.deepcopy(saved)
    raw[3] &= ~(termios.ICANON | termios.ECHO | termios.ECHONL)
    _set_cc(raw, termios.VMIN, 1)
    _set_cc(raw, termios.VTIME, 0)
    return raw


def build_argv(run, guest, disk_image, max_steps):
    """The executor command line; no shell is involved anywhere."""
    return [run, "--image", guest, "--terminal", "--disk-image", disk_image,
            "--max-steps", str(max_steps)]


def _forward(child, signum):
    """Signal the child this script owns, if it is still alive.

    Only the child's own PID is addressed; its process group and any other
    process are never touched."""
    if child is None or child.poll() is not None:
        return
    try:
        child.send_signal(signum)
    except (ProcessLookupError, OSError):
        pass


def _wait_for_child(child, pending):
    """Wait for the child, escalating terminate then kill after a signal.

    A signal that arrived during Popen could not be forwarded then (there was
    no child yet), so it is forwarded here before the grace period rather than
    waiting three seconds to switch to SIGTERM. `pending` is a one-element list
    the signal handler writes. Returns the child's status once it is gone."""
    if pending[0]:
        _forward(child, pending[0])
    terminate_sent = False
    kill_sent = False
    deadline = None
    code = 0
    while True:
        try:
            code = child.wait(timeout=0.2)
            break
        except subprocess.TimeoutExpired:
            pass
        if not pending[0]:
            continue
        now = time.monotonic()
        if deadline is None:
            deadline = now + GRACE_SECONDS
        elif now >= deadline:
            if not terminate_sent:
                child.terminate()
                terminate_sent = True
            elif not kill_sent:
                child.kill()
                kill_sent = True
            else:
                # The last resort: block until it is gone.
                code = child.wait()
                break
            deadline = now + GRACE_SECONDS
    if pending[0]:
        return 128 + pending[0]
    if code < 0:
        return 128 + (-code)
    return code


def main(argv=None):
    parser = argparse.ArgumentParser(
        prog="yan_shell.py",
        description="run the production YanFS terminal Guest on a real TTY")
    parser.add_argument("--run", required=True,
                        help="path to the existing yan_run executor")
    parser.add_argument("--guest", required=True,
                        help="path to the production Guest ELF")
    parser.add_argument("--disk-image", required=True,
                        help="path to an existing raw image (never created here)")
    parser.add_argument("--max-steps", type=positive_max_steps, default=UINT64_MAX,
                        help="positive step limit (default: 2^64-1)")
    args = parser.parse_args(argv)

    missing = []
    if not os.path.isfile(args.run) or not os.access(args.run, os.X_OK):
        missing.append("--run %s is not an executable file" % args.run)
    if not os.path.isfile(args.guest):
        missing.append("--guest %s is not a file" % args.guest)
    if not os.path.exists(args.disk_image):
        missing.append("--disk-image %s does not exist" % args.disk_image)
    if missing:
        for line in missing:
            print("yan_shell.py: %s" % line, file=sys.stderr)
        return STARTUP_ERROR

    saved = None
    old_handlers = {}
    pending = [0]
    child = None
    startup_error = False
    restore_error = False
    result = STARTUP_ERROR

    def forward(signum, _frame):
        pending[0] = signum
        _forward(child, signum)

    def cleanup_child():
        """Terminate then kill the child this script created, never a group."""
        if child is None or child.poll() is not None:
            return
        try:
            child.terminate()
        except OSError:
            pass
        try:
            child.wait(timeout=GRACE_SECONDS)
            return
        except subprocess.TimeoutExpired:
            pass
        try:
            child.kill()
        except OSError:
            pass
        try:
            child.wait(timeout=GRACE_SECONDS)
        except subprocess.TimeoutExpired:
            pass

    try:
        # Handlers go in before the terminal is touched: a signal during the
        # raw switch, during Popen or during the restore must be caught rather
        # than take the default disposition and leave the TTY raw.
        old_handlers[signal.SIGINT] = signal.signal(signal.SIGINT, forward)
        old_handlers[signal.SIGTERM] = signal.signal(signal.SIGTERM, forward)
        if os.isatty(0):
            saved = termios.tcgetattr(0)
            termios.tcsetattr(0, termios.TCSANOW, _raw_attributes(saved))
        if pending[0]:
            # A signal arrived before the child existed. Do not start one; the
            # run ends with the original signal's status.
            result = 128 + pending[0]
        else:
            try:
                child = subprocess.Popen(build_argv(args.run, args.guest,
                                                    args.disk_image, args.max_steps))
            except OSError as error:
                print("yan_shell.py: cannot start %s: %s" % (args.run, error),
                      file=sys.stderr)
                startup_error = True
            else:
                result = _wait_for_child(child, pending)
    except TERMINAL_ERRORS as error:
        print("yan_shell.py: terminal setup failed: %s" % error, file=sys.stderr)
        startup_error = True
    finally:
        # Clean up the child even on an exception, then restore the terminal,
        # and only then restore the handlers: they stay installed through the
        # whole cleanup, so a signal in any window is still caught and the TTY
        # still gets restored.
        cleanup_child()
        # A failed tcsetattr may already have applied part of the requested
        # attributes. Once the original is saved, every setup path restores it.
        if saved is not None:
            try:
                termios.tcsetattr(0, termios.TCSANOW, saved)
            except TERMINAL_ERRORS as error:
                print("yan_shell.py: cannot restore terminal attributes: %s"
                      % error, file=sys.stderr)
                restore_error = True
        for signum, handler in old_handlers.items():
            signal.signal(signum, handler)

    if startup_error or restore_error:
        return STARTUP_ERROR
    if pending[0]:
        return 128 + pending[0]
    return result


if __name__ == "__main__":
    sys.exit(main())
