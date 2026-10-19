"""Console mutation gate: shared marked Bash verdict, actual compiled controls.

G1: the only status/code/contract/owner/assertion/capture classifier is the
marked console_mutation_verdict in run_console.sh. Python extracts and executes
that current function; it has no independent classifier.
G2: baseline 77 is a dependency SKIP before other controls.
"""
import argparse
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / 'tests/guest/run_console.sh'
GCC = '/usr/bin/riscv64-linux-gnu-gcc'
BEGIN = '# BEGIN CONSOLE_MUTATION_VERDICT'
END = '# END CONSOLE_MUTATION_VERDICT'


class Skip(Exception):
    pass


def extract_verdict(script=SCRIPT):
    text = Path(script).read_text()
    assert text.count(BEGIN) == 1 and text.count(END) == 1, 'verdict markers'
    body = text.split(BEGIN, 1)[1].split(END, 1)[0]
    assert 'console_mutation_verdict()' in body
    return body


def verdict(args, script=SCRIPT):
    body = extract_verdict(script)
    path = Path(tempfile.mkstemp(prefix='verdict-', suffix='.sh')[1])
    try:
        path.write_text(body)
        cmd = ['bash', '-c', 'source "$1"; shift; console_mutation_verdict "$@"',
               '_', str(path)] + [str(x) for x in args]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
        assert result.returncode == 0, ('verdict-run', result.returncode, result.stderr)
        return result.stdout.strip()
    finally:
        path.unlink(missing_ok=True)


def check_pure():
    assert verdict((4, 4, '', 0, 1, 'code', 4, 'ok')) == 'harness'
    assert verdict((5, 4, '', 0, 1, 'code', 4, 'ok')) == 'harness'
    assert verdict((129, 4, '', 0, 1, 'code', 4, 'ok')) == 'harness'
    assert verdict((6, '0xbad1', '', 0, 1, 'code', 4, 'ok')) == 'harness'
    assert verdict((6, 47824, '', 0, 1, 'code', 47824, 'ok')) == 'harness'
    assert verdict((6, 4, '', 0, 1, 'code', 4, 'ok')) == 'detect'
    assert verdict((6, 5, '', 0, 1, 'code', 4, 'ok')) == 'harness'
    assert verdict((6, 5, '', 0, 0, 'code', 4, 'ok')) == 'ignore'
    assert verdict((7, 0, 'uart-read-limit', 0, 1, 'contract', 'uart-read-limit', 'ok')) == 'detect'
    assert verdict((7, 0, 'wrong', 0, 1, 'contract', 'uart-read-limit', 'ok')) == 'harness'
    assert verdict((7, 0, 'wrong', 0, 0, 'contract', 'uart-read-limit', 'ok')) == 'ignore'
    assert verdict((0, 0, '', 0, 1, 'code', 4, 'ok')) == 'survivor'
    assert verdict((0, 0, '', 0, 1, 'capture', '', 'mismatch')) == 'detect'
    assert verdict((0, 0, '', 0, 1, 'code', 4, 'mismatch')) == 'harness'
    assert verdict((0, 0, '', 0, 1, 'capture', '', 'error')) == 'harness'
    assert verdict((0, 0, '', 0, 0, 'code', 4, 'ok')) == 'ignore'
    assert verdict((0, 0, '', 1, 1, 'code', 4, 'ok')) == 'harness'


def check_sensitivity():
    text = SCRIPT.read_text()
    body = extract_verdict()
    assert 'if [ "$status" -eq 4 ] || [ "$status" -eq 5 ]' in body
    corrupt_timeout = re.sub(
        r'if \[ "\$status" -eq 4 \].*?\n    fi\n', '', text,
        count=1, flags=re.S)
    path = Path(tempfile.mkstemp(prefix='verdict-timeout-', suffix='.sh')[1])
    try:
        path.write_text(corrupt_timeout)
        assert verdict((4, 4, '', 0, 1, 'code', 4, 'ok')) == 'harness'
        assert verdict((4, 4, '', 0, 1, 'code', 4, 'ok'), path) != 'harness'
    finally:
        path.unlink(missing_ok=True)
    trap_anchor = 'if [ "$code" -ge $((0xbad0)) ]; then'
    assert trap_anchor in body
    corrupt_trap = text.replace(trap_anchor,
                                'if false; then', 1)
    path = Path(tempfile.mkstemp(prefix='verdict-trap-', suffix='.sh')[1])
    try:
        path.write_text(corrupt_trap)
        assert verdict((6, 47824, '', 0, 1, 'code', 4, 'ok')) == 'harness'
        assert verdict((6, 47824, '', 0, 1, 'code', 47824, 'ok'), path) == 'detect'
    finally:
        path.unlink(missing_ok=True)


def run(args, timeout=360):
    return subprocess.run(args, cwd=ROOT, capture_output=True, text=True,
                          timeout=timeout)


def run_console(work, console=None, mutation=False, no_effect=False):
    args = ['bash', str(SCRIPT), '--source', str(ROOT), '--gcc', GCC,
            '--work', str(work), '--drive', 'scripted']
    if console:
        args += ['--console', str(console)]
    if mutation:
        args += ['--mutation']
    if no_effect:
        args += ['--no-effect']
    return run(args)


def drive(work, image, extra, steps=40000):
    args = [str(work/'console_drive'), '--image', str(image),
            '--capture', str(work/'control.capture'), '--max-steps', str(steps)] + extra
    return run(args, timeout=120)


def counts(text):
    m = re.search(r'mutation: (\d+) detected, (\d+) survived, (\d+) harness errors', text)
    return tuple(map(int, m.groups())) if m else None


def test_actual(work):
    base = work/'base'
    result = run_console(base)
    if result.returncode == 77:
        message = (result.stderr.strip() or result.stdout.strip())
        raise Skip(message.removeprefix('SKIP: '))
    assert result.returncode == 0, ('baseline health', result.stdout, result.stderr)

    headless = base/'images/headless.elf'
    r = drive(base, headless, ['--no-terminal', '--uart-read-limit', '0'])
    assert r.returncode == 7 and 'CONTRACT uart-read-limit' in r.stderr, ('observer-read', r.returncode, r.stderr)
    r = drive(base, headless, ['--no-terminal', '--uart-read-limit', '37'])
    assert r.returncode == 0, ('observer-read-ok', r.returncode, r.stderr)
    refused = base/'images/refused.elf'
    r = drive(base, refused, ['--refuse-tx-after', '2', '--txdata-limit', '1'])
    assert r.returncode == 7 and 'CONTRACT txdata-write-limit' in r.stderr, ('observer-tx', r.returncode, r.stderr)

    original = (ROOT/'os/console.c').read_text()
    puts = 'YanOsResult yan_os_console_puts(const char *text)\n{'
    assert puts in original

    # Actual compiled Host failure paths: nonzero and signal through the real drive.
    r = drive(base, work/'missing.elf', ['--no-terminal'])
    assert r.returncode == 5, ('host-nonzero', r.returncode, r.stderr)
    assert verdict((5, 0, '', 0, 1, 'code', 4, 'ok')) == 'harness'
    p = subprocess.Popen([str(base/'console_drive'), '--image', str(headless),
                          '--capture', str(base/'signal.capture'),
                          '--max-steps', '40000', '--no-terminal'],
                         cwd=ROOT, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL)
    p.send_signal(signal.SIGTERM)
    signal_status = p.wait(timeout=10)
    assert signal_status < 0, ('host-signal', signal_status)
    assert verdict((signal_status, 0, '', 0, 1, 'code', 4, 'ok')) == 'harness'

    # Actual Guest trap source through the real shell mutation path: all
    # scenarios trap before their first check; no clean PASS.
    trap = work/'trap.c'
    trap.write_text(original.replace(puts, puts + '\n    __builtin_trap();\n', 1))
    r = run_console(work/'trap', console=trap, mutation=True)
    assert r.returncode == 1, ('trap-exit', r.returncode, r.stdout[-1000:])
    assert 'PASS every console check that could run passed' not in r.stdout
    assert 'PASS mutation' not in r.stdout, ('trap-no-pass', r.stdout[-1000:])
    assert 'HARNESS ERROR mutation' in r.stdout

    # Actual endless source through the real shell mutation path: all
    # designated runs hit no-termination, so ten harness errors.
    hang = work/'hang.c'
    hang.write_text(original.replace(puts, puts + '\n    for (;;) { }\n', 1))
    r = run_console(work/'hang', console=hang, mutation=True)
    assert r.returncode == 1, ('hang-exit', r.returncode, r.stdout[-1000:])
    assert 'PASS every console check that could run passed' not in r.stdout
    assert 'PASS mutation' not in r.stdout, ('hang-no-pass', r.stdout[-1000:])
    assert counts(r.stdout) == (0, 0, 10), r.stdout[-1000:]

    # Build failure on one scenario, while another designated scenario detects:
    # PASS must be suppressed by harness priority.
    broken = work/'broken.c'
    broken.write_text('#define YAN_CONSOLE_SCENARIO_BOUNDARY 5\n'
                      '#if YAN_CONSOLE_SCENARIO == 5\n'
                      '#error control build failure\n'
                      '#endif\n' + original)
    r = run_console(work/'broken', console=broken, mutation=True)
    assert r.returncode == 1, ('build-failure-exit', r.returncode, r.stdout[-1000:])
    assert 'HARNESS ERROR mutation' in r.stdout
    assert 'PASS mutation' not in r.stdout, ('build-failure-no-pass', r.stdout[-1000:])

    # Actual anchor mismatch is harness, while other mutants can still detect.
    anchor = work/'anchor.c'
    anchor.write_text(original.replace('            if (stored > 0) {',
                                       '            if (stored > 0u) {', 1))
    r = run_console(work/'anchor', console=anchor, mutation=True)
    assert r.returncode == 1, ('anchor-exit', r.returncode, r.stdout[-1000:])
    assert 'HARNESS ERROR mutation backspace-without-empty-check: anchor mismatch' in r.stdout
    assert 'PASS mutation headless-polls' in r.stdout

    # Actual no-effect source copies: textual-only mutations survive.
    no_effect = work/'no-effect.c'
    no_effect.write_text(original + '\n/* no-effect source copy */\n')
    r = run_console(work/'no-effect', console=no_effect, mutation=True, no_effect=True)
    assert r.returncode == 1, ('no-effect-exit', r.returncode, r.stdout[-1000:])
    assert counts(r.stdout) == (0, 10, 0), r.stdout[-1000:]

    # Actual ten mutants: exact 10/0/0.
    r = run_console(work/'ten', mutation=True)
    assert r.returncode == 0, ('ten-exit', r.returncode, r.stdout[-1000:])
    assert counts(r.stdout) == (10, 0, 0), r.stdout[-1000:]


def test_missing(work):
    r = run_console(work/'missing', console=work/'absent-console.c')
    assert r.returncode == 1, ('missing-source', r.returncode, r.stdout)
    r = subprocess.run(['bash', str(SCRIPT), '--source', str(ROOT),
                        '--gcc', str(work/'absent-gcc'), '--work', str(work/'missing-both'),
                        '--console', str(work/'absent-console.c'), '--drive', 'scripted'],
                       cwd=ROOT, capture_output=True, text=True, timeout=60)
    assert r.returncode == 1, ('missing-source-and-gcc', r.returncode, r.stdout, r.stderr)
    r = subprocess.run(['bash', str(SCRIPT), '--source', str(ROOT),
                        '--gcc', str(work/'absent-gcc'), '--work', str(work/'nogcc'),
                        '--drive', 'scripted'], cwd=ROOT, capture_output=True,
                       text=True, timeout=60)
    assert r.returncode == 77, ('all-local-no-gcc', r.returncode, r.stdout, r.stderr)


def main():
    global GCC
    parser = argparse.ArgumentParser()
    parser.add_argument('--gcc', default=GCC,
                        help='cross compiler used by run_console.sh and controls')
    args = parser.parse_args()
    GCC = args.gcc
    check_pure()
    check_sensitivity()
    work = Path(tempfile.mkdtemp(prefix='console-gate-'))
    try:
        test_actual(work)
        test_missing(work)
    except Skip as error:
        print('SKIP:', error)
        return 77
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print('PASS console mutation gate: shared marked verdict + actual compiled controls')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (AssertionError, subprocess.TimeoutExpired, OSError) as error:
        print(':FAIL:', error)
        sys.exit(1)
