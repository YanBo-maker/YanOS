"""0032 real production Guest, independent byte oracle and stable-image checks."""
import argparse
from pathlib import Path
import subprocess
import struct
import sys
import time

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import block_references_reference as reference
import run_note_relations as relation

terms, base = relation.terms, relation.base
INPUTS = tuple(dict.fromkeys((*relation.INPUTS, 'tests/block_references_reference.py',
    'tests/note_metadata_reference.py', 'tests/guest/run_note_blocks.py',
    'os/note_blocks.c', 'os/note_blocks.h', 'os/note_blocks_format.c', 'os/note_blocks_format.h')))


def contents(disk, name):
    # The shared terminal parser fixes its image at16 blocks; these healthy
    # fixtures deliberately use128 blocks for large text, so read the known
    # v1 directory layout independently instead of assuming that fixed size.
    raw = disk.read_bytes()
    if struct.unpack_from('<I', raw, 16)[0] * 4096 != len(raw):
        raise AssertionError('readback image capacity differs')
    for slot in range(63):
        entry = raw[64 + slot * 64:128 + slot * 64]
        if entry[:32].split(b'\0', 1)[0] == name.encode('ascii'):
            size, start = struct.unpack_from('<II', entry, 32)
            return raw[start * 4096:start * 4096 + size]
    raise AssertionError('missing readback file: ' + name)


def check(args, work, label, files, commands, *, disk=None):
    expectations = [(command, reference.query(files, command)) for command in commands]
    return relation.run(args, work, label, list(files.items()), expectations, disk=disk)


def basic_and_old_relations(args, work):
    files = {'n': b'@yan-note 1\nlink: s\n@end\n@block a\r\n' + '中断'.encode() +
             b'\t\\\r\n[[s#NONE]]\n@block empty\n@block tail',
             's': b'[[n#a]]\n@block NONE\n[[n#a]][[gone#x]]',
             'old': b'[[n#a]]', 'binary': b'[[n#a]]\x00'}
    check(args, work, 'basic', files, [b'block list n', b'block show n#a',
        b'block show n#empty', b'block show n#tail', b'block refs s',
        b'block refs old', b'block backlinks n#a', b'block backlinks absent#x'])
    relation.run(args, work, 'old-relations', list(files.items()), [
        (b'links n', b'LINK s OK\r\nOK links shown=1\r\n'),
        (b'backlinks s', b'BACKLINK n\r\nOK backlinks total=1 shown=1 target=OK\r\n'),
        (b'related n', b'RELATED s out=1 in=0 tags=0\r\nOK related total=1 shown=1\r\n')])


def boundaries_and_large_text(args, work):
    data = b'x' * 4092 + b'\n@block edge\r\n'
    data += b'a' * (8190 - len(data)) + b'[[target#a]]\n'
    data += b'z' * (12287 - len(data)) + '中'.encode() + b'\r\n@block end\nend'
    files = {'boundary': data, 'target': b'@block a\nx',
             'large': b'@block a\n' + b'L' * 17000 + b'\r\n',
             'long': b'@block a\n' + b'X' * 100000}
    check(args, work, 'boundaries', files, [b'block list boundary',
        b'block refs boundary', b'block show boundary#end', b'block show large#a',
        b'block list long', b'block refs long'])


def structure_and_text_errors(args, work):
    sixtyfour = b''.join(b'@block b%02d\n' % i for i in range(64))
    files = {'ok': sixtyfour, 'duplicate': sixtyfour + b'@block b00\n',
             'limit': sixtyfour + b'@block b64\n@block b00\n',
             'reserved': b'@block\tbad\n', 'lookalike': b'@blockchain\n @block x\n',
             'binary': b'@yan-note 9\n@block\n' + b'x' * 9000 + b'\x00',
             'truncated': b'@block a\n\xf0\x9f', 'barecr': b'@block a\nx\ry',
             'badhead': b'@yan-note 9\n@end\n'}
    commands = [b'block list ' + name.encode() for name in files]
    commands += [b'block list absent', b'block show ok#missing', b'block show ok',
                 b'block refs ok extra', b'block backlinks binary#a']
    check(args, work, 'errors', files, commands)


def top20_and_late_library_error(args, work):
    files = {'target': b'@block a\nx', 'bad': b'@block a\n@block a\n',
             'source': b'[[target#a]]' * 20 + b'[[bad#a]]',
             'many': b'[[target#a]]' * 300,
             'shown': b'[[target#a]]' * 19 + b'[[bad#a]]'}
    check(args, work, 'top20', files, [b'block refs source', b'block refs shown', b'block refs many'])
    late = {'a': b'[[target#a]]', 'target': files['target'], 'zzbad': b'@block\n'}
    check(args, work, 'late-unrelated-structure', late, [b'block backlinks target#a'])


def backlinks_order_full_count_and_binary_skips(args, work):
    files = {'z': b'[[gone#a]]', 'A': b'@block NONE\n' + b'[[gone#a]]' * 300,
             'a': b'[[gone#a]][[gone#other]]',
             'binary': b'@yan-note 9\n[[gone#a]]\x00', 'n': b'@block a\n[[n#a]]'}
    check(args, work, 'backlinks', files, [b'block backlinks gone#a',
        b'block backlinks n#a', b'block refs n'])


def lifecycle_and_restart(args, work):
    files = {'n': b'@block a\nold\n@block b\nlast', 's': b'[[n#a]]'}
    disk, _ = check(args, work, 'before', files, [b'block show n#a', b'block refs s'])
    rewritten = b'prefix\n' * 100 + b'@block b\nmoved\n@block a\nnew [[n#a]]\n'
    commands = (b'edit n\n' + b'd 1\n' * 4 + b'a prefix\n' * 100 +
        b'a @block b\na moved\na @block a\na new [[n#a]]\nw\nexit\n')
    result = base.run_guest(args.run, args.guest, disk, commands, work, 'edit-move')
    terms.literal.require_rc(result, 0, 'edit-move')
    if contents(disk, 'n') != rewritten:
        raise AssertionError('editor move/insert did not preserve the intended original bytes')
    files['n'] = rewritten
    check(args, work, 'after-edit-restart', files, [b'block show n#a', b'block refs s'], disk=disk)
    expectations = [(b'mv n renamed', b'OK mv\r\n')]
    files['renamed'] = files.pop('n')
    expectations.append((b'block refs s', reference.query(files, b'block refs s')))
    expectations.append((b'cp renamed copy', b'OK cp\r\n'))
    files['copy'] = files['renamed']
    expectations += [(command, reference.query(files, command)) for command in
                     (b'block list copy', b'block refs copy')]
    expectations.append((b'rm renamed', b'OK rm\r\n')); files.pop('renamed')
    expectations.append((b'block refs s', reference.query(files, b'block refs s')))
    relation.run(args, work, 'rename-copy', [], expectations, readonly=False, disk=disk)
    if contents(disk, 'copy') != rewritten:
        raise AssertionError('copy rewrote IDs or self-reference bytes')
    created = b'@block a\nreborn\n'
    result = base.run_guest(args.run, args.guest, disk,
        b'edit n\na @block a\na reborn\nw\nexit\n', work, 'recreate')
    terms.literal.require_rc(result, 0, 'recreate')
    files['n'] = created
    if contents(disk, 'n') != created:
        raise AssertionError('recreated note differs')
    check(args, work, 'rebound-restart', files,
          [b'block refs s', b'block backlinks n#a', b'block show copy#a'], disk=disk)


CASES = (basic_and_old_relations, boundaries_and_large_text, structure_and_text_errors,
         top20_and_late_library_error, backlinks_order_full_count_and_binary_skips,
         lifecycle_and_restart)


def main():
    parser = argparse.ArgumentParser()
    for name in ('source', 'work', 'run', 'mkfs', 'guest'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    missing = [args.source / name for name in INPUTS if not (args.source / name).is_file()]
    missing += [path for path in (args.run, args.mkfs, args.guest) if not path.is_file()]
    if missing:
        print(':FAIL: missing required input:', ', '.join(map(str, missing)))
        return 1
    started = time.monotonic()
    failed = harness = 0
    for case in CASES:
        work = args.work / case.__name__; work.mkdir(parents=True, exist_ok=True)
        try:
            case(args, work); print('PASS', case.__name__, flush=True)
        except AssertionError as error:
            failed += 1; print(':FAIL:', case.__name__, error, flush=True)
        except (base.HarnessError, OSError, subprocess.TimeoutExpired) as error:
            harness += 1; print(':HARNESS-ERROR:', case.__name__, error, flush=True)
    print('blocks Guest: %d passed, %d failed, %d harness errors; %.3fs' %
          (len(CASES) - failed - harness, failed, harness, time.monotonic() - started))
    return 2 if harness else 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
