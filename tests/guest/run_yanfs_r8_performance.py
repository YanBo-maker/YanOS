#!/usr/bin/env python3
"""0036-R8-AC4: measure real Guest command steps on full N=1023/2047 images."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

import run_terminal_runtime as runtime


def main():
    parser = argparse.ArgumentParser()
    for name in ('source', 'work', 'guest', 'mkfs'):
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('--cc', required=True)
    args = parser.parse_args()
    required = [*runtime.HOST_SOURCES, *runtime.HOST_HEADERS,
                *runtime.PRODUCTION_SOURCES, *runtime.GUEST_HEADERS,
                'os/sort.h', 'tests/yanfs_fill_probe.c']
    for name in required:
        if not (args.source / name).is_file():
            raise RuntimeError('missing repository input: ' + name)
    if not args.guest.is_file() or not args.mkfs.is_file():
        raise RuntimeError('missing built Guest or mkfs')
    if not runtime.tool_available(args.cc):
        print('SKIP host compiler unavailable')
        return 77
    args.work.mkdir(parents=True, exist_ok=True)
    driver = runtime.build_host(args.source, args.work, args.cc)
    filler = args.work / 'fill'
    command = [args.cc, *runtime.HOST_CC_FLAGS, '-I', str(args.source / 'os'),
               str(args.source / 'tests/yanfs_fill_probe.c'),
               str(args.source / 'os/yanfs.c'), '-o', str(filler)]
    subprocess.run(command, check=True, capture_output=True)
    records = []
    for files in (1023, 2047):
        path = args.work / ('full-%d.img' % files)
        if path.exists():
            path.unlink()  # exactly this test's own disposable fixture
        subprocess.run([str(args.mkfs), '--image', str(path), '--files', str(files),
                        '--blocks', str(files + (files + 1) // 64 + 4)],
                       check=True, capture_output=True)
        subprocess.run([str(filler), '--image', str(path), '--files', str(files)],
                       check=True, capture_output=True)
        image = path.read_bytes()
        names = [image[64 + i * 64:96 + i * 64].split(b'\0')[0]
                 for i in range(files)]
        if not all(names) or len(set(names)) != files:
            raise RuntimeError('fixture is not full with distinct files')
        commands = {'ls': b'ls\n', 'meta': b'meta list\n',
            'meta-filter': b'meta list tag:t\n',
            'backlinks': b'backlinks f0000.txt\n', 'related': b'related f0000.txt\n',
            'block-backlinks': b'block backlinks f0000.txt#x\n',
            'search': b'search x\n',
            'create': (('rm f%04d.txt\ncreate f%04d.txt ' %
                       (files // 2, files // 2)).encode() + b'a' * 60 + b'\n')}
        for case, line in commands.items():
            label = '%d-%s' % (files, case)
            result, status, disk, capture = runtime.run_production(
                driver, args, args.work, image, line + b'exit\n', label,
                'none', 0, 0, expect_tohost=1, max_steps=200000000)
            values = runtime.classify_production(result, status, label, {1})
            output = capture.read_bytes()
            receipt = {'meta': b'OK meta list', 'meta-filter': b'OK meta list',
                       'block-backlinks': b'OK block backlinks'}.get(
                           case, ('OK ' + case).encode())
            if receipt not in output or b'ERROR ' in output:
                raise RuntimeError(label + ': expected command did not succeed')
            if case == 'meta-filter' and (
                    'OK meta list shown=%d' % (files - 1)).encode() not in output:
                raise RuntimeError(label + ': filter did not exclude the untagged file')
            index = 1 if case == 'create' else 0
            mount = int(values['perf_prompt_0'])
            steps = int(values['perf_prompt_%d' % (index + 1)]) - int(
                values['perf_input_%d' % index])
            stack = int(values['task0_observed_stack_bytes'])
            if values['_tohost'] != 1 or disk.read_bytes() != image:
                raise RuntimeError(label + ': Guest failed or image bytes changed')
            record = dict(files=files, case=case, mount=mount, steps=steps,
                          stack=stack, output_sha256=hashlib.sha256(
                              capture.read_bytes()).hexdigest())
            records.append(record)
            print(json.dumps(record), flush=True)
            (args.work / 'measurements.json').write_text(
                json.dumps(records, indent=2) + '\n', encoding='utf-8')
            if mount > 50000000 or not 0 < steps <= 50000000 or not 0 < stack <= 4096:
                raise RuntimeError(label + ': Guest steps/stack exceed R8 budget')
    print('PASS R8: 16 commands, mount, exact images, task0 <=4KiB')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (RuntimeError, OSError, ValueError, KeyError,
            subprocess.SubprocessError, runtime.HarnessError,
            runtime.OwnerFailure) as error:
        print('FAIL R8:', error)
        sys.exit(1)
