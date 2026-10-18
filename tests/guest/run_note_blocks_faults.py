"""0032 production Guest read/output failures, exact firing and zero media writes."""
import argparse
from pathlib import Path
import subprocess
import sys
import time
sys.dont_write_bytecode = True
import run_terminal_runtime as runtime
import run_editor as editor

INPUTS = tuple(dict.fromkeys((*editor.PRODUCTION_INPUTS, *runtime.HOST_SOURCES,
    *runtime.HOST_HEADERS, 'tests/guest/run_note_blocks_faults.py',
    'tests/guest/run_terminal_runtime.py', 'tests/guest/run_editor.py',
    'tests/guest/note_blocks_init_fail.c')))


def build_driver(args):
    # Only the private fixture gains request-number selection for the existing
    # real fail_next path. No production or shared driver source is edited.
    source = args.source / 'tests/terminal_runtime_driver.c'
    old = 'strstr(drive->backend.out, "yanfs> ") != NULL) {'
    new = ('strstr(drive->backend.out, "yanfs> ") != NULL &&\n'
           '        drive->block.served + 1u >= drive->corrupt_response) {')
    text = source.read_text()
    if text.count(old) != 1:
        raise runtime.HarnessError('IO request seam is absent or not unique')
    private = args.work / 'block_fault_driver.c'
    private.write_text(text.replace(old, new, 1))
    (args.work / 'driver-seam.txt').write_text('before:\n' + old + '\nafter:\n' + new + '\n')
    driver = args.work / 'block_fault_driver'
    flags = runtime.HOST_CC_FLAGS + (runtime.SANITIZER_FLAGS if args.sanitizers else [])
    argv = [args.cc, *flags, '-I', str(args.source / 'include'), '-I', str(args.source / 'tools')]
    argv += [str(private if name == 'tests/terminal_runtime_driver.c' else args.source / name)
             for name in runtime.HOST_SOURCES]
    argv += ['-o', str(driver)]
    result = runtime.run(argv)
    (args.work / 'driver-build.log').write_bytes(result.stdout + result.stderr)
    if result.returncode:
        raise runtime.HarnessError('private driver build failed: ' + result.stderr.decode(errors='replace'))
    return driver


def main():
    parser = argparse.ArgumentParser()
    for name in ('source', 'work', 'guest'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--cc', required=True)
    parser.add_argument('--gcc', default='riscv64-linux-gnu-gcc')
    parser.add_argument('--sanitizers', action='store_true')
    args = parser.parse_args()
    missing = [args.source / name for name in INPUTS if not (args.source / name).is_file()]
    if not args.guest.is_file():
        missing.append(args.guest)
    if missing:
        print(':FAIL: missing required input:', ', '.join(map(str, missing)))
        return 1
    if not runtime.tool_available(args.cc) or not runtime.tool_available(args.gcc):
        print('SKIP host/cross compiler')
        return 77
    args.work.mkdir(parents=True, exist_ok=True)
    driver = build_driver(args)
    image = runtime.seeded_image([('n', b'@block a\n' + b'x' * 10000 + b'\n[[n#a]]\n@block b\nlast\n', 1)])
    passed = failed = harness = 0
    started = time.monotonic()

    def exercise(label, command, *, fault='none', response=0, tx=0, baseline=None):
        expected = runtime.TOHOST_SHELL_FATAL if fault != 'none' or tx else 1
        result, status, disk, capture = runtime.run_production(driver, args, args.work,
            image, command + b'\nexit\n', label, fault, response, tx,
            baseline=baseline, expect_tohost=expected)
        values = runtime.classify_production(result, status, label, {expected})
        output = capture.read_bytes()
        if values['_tohost'] != expected or disk.read_bytes() != image:
            raise runtime.OwnerFailure(label + ': wrong finishing code or changed image')
        if fault in ('io', 'protocol'):
            if (int(values.get('block_responses', '-1')) != response
                    or ('ERROR %s\r\n' % fault.upper()).encode() not in output
                    or (fault == 'protocol' and values['protocol_fired'] != '1')):
                raise runtime.OwnerFailure(label + ': requested read fault did not fire/stop exactly')
            if b'OK block ' in output or b'yanfs: exit\r\n' in output:
                raise runtime.OwnerFailure(label + ': success/next command after read failure')
        return output, values

    def case(label, action):
        nonlocal passed, failed, harness
        try:
            action(); passed += 1; print('PASS', label, flush=True)
        except runtime.OwnerFailure as error:
            failed += 1; print(':FAIL:', label, error, flush=True)
        except (runtime.HarnessError, OSError, subprocess.TimeoutExpired) as error:
            harness += 1; print(':HARNESS-ERROR:', label, error, flush=True)

    def startup():
        elf = args.work / 'block-init-failure.elf'
        argv = [args.gcc, *runtime.CROSS_FLAGS, '-I', str(args.source / 'os'),
                '-T', str(args.source / runtime.GUEST_LD), '-Wl,--wrap=yan_blocks_init',
                '-o', str(elf), *(str(args.source / name) for name in runtime.PRODUCTION_SOURCES),
                str(args.source / 'tests/guest/note_blocks_init_fail.c')]
        built = runtime.run(argv)
        (args.work / 'startup-build.log').write_bytes(built.stdout + built.stderr)
        if built.returncode:
            raise runtime.HarnessError('startup wrapper build failed: ' + built.stderr.decode(errors='replace'))
        result, status, disk, capture = runtime.run_production(driver, argparse.Namespace(guest=elf),
            args.work, image, b'exit\n', 'startup', 'none', 0, 0, expect_tohost=0x71000017)
        values = runtime.classify_production(result, status, 'startup', {0x71000017})
        if (values['_tohost'] != 0x71000017 or b'ERROR BLOCK_INIT\r\n' not in capture.read_bytes()
                or disk.read_bytes() != image):
            raise runtime.OwnerFailure('BLOCK_INIT handling/code/zero write mismatch')
        try:
            runtime.classify_production(result, status, 'unexpected-startup', {1})
        except runtime.OwnerFailure:
            return
        raise runtime.OwnerFailure('unexpected known startup code must be owner failure')

    case('startup-block-init', startup)
    for verb in ('list', 'refs', 'backlinks', 'show'):
        command = ('block %s n%s' % (verb, '#a' if verb in ('backlinks', 'show') else '')).encode()
        label = verb + '-baseline'
        healthy, status = exercise(label, command)
        receipt = ('OK block %s ' % verb).encode()
        if receipt not in healthy:
            raise runtime.OwnerFailure(label + ': healthy receipt absent')
        count = int(status.get('block_responses', '-1'))
        if count < 9:
            raise runtime.OwnerFailure(label + ': expected complete three-block preflight')
        passed += 1; print('PASS', label, 'responses=' + str(count), flush=True)
        # mount consumes1/2; n eligibility3..5, head6, structure7..9.
        preflight_last = 9 if verb == 'show' else count
        for fault in ('io', 'protocol'):
            for response in sorted({3, (3 + preflight_last) // 2, preflight_last}):
                label = '%s-%s-%d' % (verb, fault, response)

                def check_read(label=label, fault=fault, response=response, command=command):
                    output, _ = exercise(label, command, fault=fault, response=response)
                    if any(token in output for token in (b'BLOCK n#', b'REF source=', b'BACKLINK source=', b'2 xxxx')):
                        raise runtime.OwnerFailure(label + ': result published before preflight finished')

                case(label, check_read)
        if verb == 'show':
            # The unaligned first range chunk consumes responses10/11;12 is
            # the next chunk's first physical block. A visible prefix is legal.
            for fault in ('io', 'protocol'):
                label = 'show-late-' + fault

                def check_late(label=label, fault=fault):
                    output, _ = exercise(label, command, fault=fault, response=12)
                    if b'2 ' + b'x' * 4096 not in output:
                        raise runtime.OwnerFailure(label + ': preceding raw chunk was not displayed')

                case(label, check_late)
        baseline = args.work / (verb + '-healthy.bin'); baseline.write_bytes(healthy)
        start = healthy.index(b'\r\n', healthy.index(command)) + 2
        end = healthy.index(b'\r\n', healthy.index(receipt)) + 2
        for suffix, at in (('first', start + 1), ('middle', (start + end) // 2), ('receipt-last', end)):
            label = verb + '-tx-' + suffix

            def check_tx(label=label, at=at, command=command, baseline=baseline,
                         healthy=healthy, count=count):
                output, values = exercise(label, command, tx=at, baseline=baseline)
                actual_reads = int(values.get('block_responses', '-1'))
                if (values['tx_fired'] != '1' or int(values['delivered']) != at
                        or output != healthy[:at] or actual_reads > count):
                    raise runtime.OwnerFailure(label + ': exact post-accept TX prefix/stop mismatch')
                if command.startswith(b'block show'):
                    expected_reads = 11 if at <= start + 2 + 4096 else 13 if at <= start + 2 + 8192 else count
                    if actual_reads != expected_reads:
                        raise runtime.OwnerFailure(label + ': continued reading after output rejection')
                elif actual_reads != count:
                    raise runtime.OwnerFailure(label + ': table preflight was not complete')

            case(label, check_tx)
    print('blocks faults: %d passed, %d failed, %d harness errors; %.3fs' %
          (passed, failed, harness, time.monotonic() - started))
    return 2 if harness else 1 if failed else 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except runtime.OwnerFailure as error:
        print(':FAIL:', error); sys.exit(1)
    except (runtime.HarnessError, OSError, subprocess.TimeoutExpired) as error:
        print(':HARNESS-ERROR:', error); sys.exit(2)
