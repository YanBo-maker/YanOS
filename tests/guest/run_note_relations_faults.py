"""0028 real Guest response and output faults with exact firing and read-only images."""
import argparse
from pathlib import Path
import subprocess
import sys
import run_terminal_runtime as runtime
import run_editor as editor

INPUTS = tuple(editor.PRODUCTION_INPUTS) + tuple(runtime.HOST_SOURCES) + tuple(runtime.HOST_HEADERS) + (
    'os/note_relations.c', 'os/note_relations.h',
    'tests/guest/run_note_relations_faults.py', 'tests/guest/run_terminal_runtime.py',
    'tests/guest/run_editor.py', 'tests/guest/note_relations_init_fail.c')


def main():
    parser = argparse.ArgumentParser()
    for name in ('source', 'work', 'guest'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--cc', required=True)
    parser.add_argument('--gcc', default='riscv64-linux-gnu-gcc')
    parser.add_argument('--sanitizers', action='store_true')
    args = parser.parse_args()
    missing = [args.source / path for path in INPUTS if not (args.source / path).is_file()]
    if not args.guest.is_file():
        missing.append(args.guest)
    if missing:
        print(':FAIL: missing required input:', ', '.join(map(str, missing)))
        return 1
    if not runtime.tool_available(args.cc) or not runtime.tool_available(args.gcc):
        print('SKIP host/cross compiler')
        return 77
    args.work.mkdir(parents=True, exist_ok=True)
    driver = runtime.build_host(args.source, args.work, args.cc, sanitizers=args.sanitizers)
    image = runtime.seeded_image([
        ('n', b'@yan-note 1\nlink: z\nlink: a\ntag: os\n@end\n\x00body', 1),
        ('z', b'@yan-note 1\nlink: n\n@end\n', 2),
        ('a', b'@yan-note 1\nlink: n\ntag: os\n@end\n', 3),
    ])
    passed = failed = harness = 0

    def exercise(label, command, *, response=0, io=False, tx=0, baseline=None):
        expected = runtime.TOHOST_SHELL_FATAL if response or io or tx else 1
        result, status, disk, capture = runtime.run_production(
            driver, args, args.work, image, command + b'\nexit\n', label,
            'io' if io else 'protocol' if response else 'none', response, tx,
            baseline=baseline, expect_tohost=expected)
        values = runtime.classify_production(result, status, label, {expected})
        if values['_tohost'] != expected:
            raise runtime.OwnerFailure(label + ': wrong tohost')
        output = capture.read_bytes()
        if disk.read_bytes() != image:
            raise runtime.OwnerFailure(label + ': relation query changed image')
        if response:
            if (values['protocol_fired'] != '1' or
                    int(values.get('block_responses', '-1')) != response or
                    b'ERROR PROTOCOL\r\n' not in output):
                raise runtime.OwnerFailure(label + ': protocol injection/stop/diagnostic absent')
        if io and (int(values.get('block_responses', '-1')) != 4 or b'ERROR IO\r\n' not in output):
            raise runtime.OwnerFailure(label + ': IO injection/stop/diagnostic absent')
        if (response or io) and any(token in output for token in
                (b'LINK ', b'BACKLINK ', b'RELATED ', b'OK links', b'OK backlinks', b'OK related', b'yanfs: exit\r\n')):
            raise runtime.OwnerFailure(label + ': partial result/success after read failure')
        return output, values

    def case(label, action):
        nonlocal passed, failed, harness
        try:
            action()
            passed += 1
            print('PASS', label, flush=True)
        except runtime.OwnerFailure as error:
            failed += 1
            print(':FAIL:', label, error, flush=True)
        except (runtime.HarnessError, OSError, subprocess.TimeoutExpired) as error:
            harness += 1
            print(':HARNESS-ERROR:', label, error, flush=True)

    def startup_failure():
        # A test-only link wrapper changes init's return value; production app
        # failure handling, Host driver and Python classifier remain real.
        elf = args.work / 'relation-init-failure.elf'
        argv = [args.gcc, *runtime.CROSS_FLAGS, '-I', str(args.source / 'os'),
                '-T', str(args.source / runtime.GUEST_LD), '-Wl,--wrap=yan_relation_init',
                '-o', str(elf)]
        argv += [str(args.source / name) for name in runtime.PRODUCTION_SOURCES]
        argv.append(str(args.source / 'tests/guest/note_relations_init_fail.c'))
        built = runtime.run(argv)
        (args.work / 'startup-build.log').write_bytes(built.stdout + built.stderr)
        if built.returncode:
            raise runtime.HarnessError('startup fixture build failed')
        startup_args = argparse.Namespace(guest=elf)
        result, status, disk, capture = runtime.run_production(
            driver, startup_args, args.work, image, b'exit\n', 'startup-relation-init',
            'none', 0, 0, expect_tohost=0x71000016)
        values = runtime.classify_production(result, status, 'startup-relation-init', {0x71000016})
        if (values['_tohost'] != 0x71000016 or b'ERROR RELATION_INIT\r\n' not in capture.read_bytes()
                or disk.read_bytes() != image):
            raise runtime.OwnerFailure('startup relation failure was not identified/read-only')
        try:
            runtime.classify_production(result, status, 'startup-unexpected-failure', {1})
        except runtime.OwnerFailure:
            return
        raise runtime.OwnerFailure('known unexpected startup failure was not an owner failure')

    case('startup-relation-init', startup_failure)

    expected_rows = {
        'links': b'LINK z OK\r\nLINK a OK\r\nOK links shown=2\r\n',
        'backlinks': b'BACKLINK a\r\nBACKLINK z\r\nOK backlinks total=2 shown=2 target=OK\r\n',
        'related': b'RELATED a out=1 in=1 tags=1\r\nRELATED z out=1 in=1 tags=0\r\nOK related total=2 shown=2\r\n',
    }
    for name, rows in expected_rows.items():
        command = name.encode() + b' n'
        count = 4 if name == 'links' else 6  # probe 1, mount 2/3, then bounded headers
        baseline_output, values = exercise(name + '-baseline', command)
        if rows not in baseline_output or int(values.get('block_responses', '-1')) != count:
            raise runtime.OwnerFailure(name + ': healthy rows/read count mismatch')
        passed += 1
        print('PASS', name + '-baseline', flush=True)
        for response in range(4, count + 1):
            label = name + '-protocol-' + str(response)
            case(label, lambda label=label, response=response, command=command:
                 exercise(label, command, response=response))
        label = name + '-io'
        case(label, lambda label=label, command=command: exercise(label, command, io=True))
        baseline = args.work / (name + '-baseline.bin')
        baseline.write_bytes(baseline_output)
        start = baseline_output.index(rows)
        positions = [('first', start + 1), ('second-row', start + rows.index(b'\r\n') + 3),
                     ('receipt-last', start + len(rows))]
        for suffix, at in positions:
            label = name + '-tx-' + suffix

            def check_tx(label=label, at=at, command=command, baseline=baseline,
                         healthy=baseline_output, count=count):
                output, status = exercise(label, command, tx=at, baseline=baseline)
                if (status['tx_fired'] != '1' or int(status.get('delivered', '-1')) != at or
                        int(status.get('block_responses', '-1')) != count or output != healthy[:at]):
                    raise runtime.OwnerFailure(label + ': exact output prefix/IO stop mismatch')

            case(label, check_tx)
    print('relations-faults: %d passed, %d failed, %d harness errors' % (passed, failed, harness))
    return 2 if harness else 1 if failed else 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except runtime.OwnerFailure as error:
        print(':FAIL:', error)
        sys.exit(1)
    except (runtime.HarnessError, OSError, subprocess.TimeoutExpired) as error:
        print(':HARNESS-ERROR:', error)
        sys.exit(2)
