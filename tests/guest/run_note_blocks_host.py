"""0033 Guest/Host same-image acceptance: Guest edits, Host MCP raw-byte oracle."""
import argparse
import hashlib
from pathlib import Path
import struct
import subprocess
import sys
import time

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve()
TESTS = HERE.parents[1]
sys.path.insert(0, str(TESTS))
sys.path.insert(0, str(HERE.parent))
import run_note_blocks as blocks
import block_references_reference as reference
from block_page_oracle import page_expected
from mcp_checks import Wire, tool_value
from semantic_json_checks import logical_hash
from semantic_fixture import image

BASE = blocks.base
REL = blocks.relation

BLOCK_KEYS = {'id', 'marker_line', 'first_line', 'last_line', 'start', 'end'}
RANGE_KEYS = {'start', 'end', 'first_line', 'last_line'}
LIST_KEYS = {'schema_version', 'operation', 'source_sha256', 'name',
             'total', 'shown', 'blocks'}
READ_KEYS = {'schema_version', 'operation', 'source_sha256', 'name', 'block_id',
             'block', 'page', 'text', 'complete', 'next_offset'}
REFS_KEYS = {'schema_version', 'operation', 'source_sha256', 'name',
             'total', 'shown', 'refs'}
REF_ROW_KEYS = {'source', 'block_id', 'line', 'start', 'end', 'target', 'state'}
BACK_KEYS = {'schema_version', 'operation', 'source_sha256', 'name', 'block_id',
             'total', 'shown', 'skipped', 'target_state', 'backlinks'}
BACK_ROW_KEYS = {'source', 'block_id', 'line', 'start', 'end'}
TARGET_KEYS = {'name', 'id'}

# Hand-listed first-page spans from the fixture geometry, not from a response.
EXPECTED_FIRST = {
    'long': 65536,
    'edge2': 65535,   # U+00E9 (2-byte UTF-8) starts at 65535
    'edge3': 65535,   # U+4E2D (3-byte CJK) starts at 65535
    'edge4': 65534,   # U+1F600 (4-byte) starts at 65534, rollback 2 bytes
    'edgecrlf': 65535,  # CR at 65535, LF at 65536, page ends before CR
    'exact': 65536,   # no rollback needed at the boundary
}


def files_from_disk(disk):
    raw = disk.read_bytes()
    capacity = struct.unpack_from('<I', raw, 16)[0]
    if capacity * 4096 != len(raw):
        raise AssertionError('image capacity mismatch')
    files = []
    for slot in range(63):
        entry = raw[64 + slot * 64:128 + slot * 64]
        name = entry[:32].split(b'\0', 1)[0]
        if not name:
            files.append(None)
            continue
        size, start = struct.unpack_from('<II', entry, 32)
        files.append((name.decode('ascii'), raw[start * 4096:start * 4096 + size]))
    while files and files[-1] is None:
        files.pop()
    return files


def disk_hash(disk):
    return hashlib.sha256(disk.read_bytes()).digest()


def run_guest(args, disk, commands, work, label):
    result = BASE.run_guest(args.run, args.guest, disk, commands, work, label)
    BASE.require_rc(result, 0, label)
    return result


def require_int(value, key, label):
    if type(value[key]) is not int or value[key] < 0:
        raise AssertionError((label, key, value[key]))


def check_keys(value, keys, label):
    if set(value) != keys:
        raise AssertionError((label, 'keys', sorted(value), sorted(keys)))


def check_common(value, operation, name, source, keys):
    check_keys(value, keys, operation)
    if type(value['schema_version']) is not int or value['schema_version'] != 1:
        raise AssertionError((operation, 'schema_version', value['schema_version']))
    if value['operation'] != operation:
        raise AssertionError((operation, 'operation', value['operation']))
    if name is not None and value['name'] != name:
        raise AssertionError((operation, 'name', value['name']))
    if value['source_sha256'] != source:
        raise AssertionError((operation, 'source_sha256', value['source_sha256'], source))


def check_range(value, expected, label):
    check_keys(value, RANGE_KEYS, label)
    for key in RANGE_KEYS:
        require_int(value, key, label)
    if [value['start'], value['end'], value['first_line'], value['last_line']] != [
            expected['start'], expected['end'], expected['first_line'], expected['last_line']]:
        raise AssertionError((label, [value[k] for k in ('start', 'end', 'first_line', 'last_line')]))


class Host:
    def __init__(self, args, disk, work):
        self.pidfile = work / 'host-runner.pid'
        self.disk = disk
        options = ['--disk-image', str(disk), '--index', str(work/'absent-index'),
                   '--model', str(work/'absent-model'), '--runner', str(work/'absent-runner'),
                   '--startup-timeout', '3', '--request-timeout', '3']
        import os
        env = dict(os.environ, YAN_TEST_SERVER_PID_FILE=str(self.pidfile))
        self.wire = Wire([str(args.server), *options], env)
        self.absent = [work/'absent-index', work/'absent-model', work/'absent-runner']

    def quiet(self, label):
        for path in self.absent:
            if path.exists():
                raise AssertionError((label, 'Host read created a dependency path', str(path)))
        if self.pidfile.exists():
            raise AssertionError((label, 'Host block call started a runner process'))
        children = Path('/proc', str(self.wire.p.pid), 'task', str(self.wire.p.pid), 'children')
        try:
            active = children.read_text().strip()
        except OSError:
            active = ''
        if active:
            raise AssertionError((label, 'Host block call left a child process', active))

    def call(self, name, arguments, label='host-call'):
        before = disk_hash(self.disk)
        self.wire.send('tools/call', params={'name': name, 'arguments': arguments})
        value = tool_value(self.wire.get())
        if disk_hash(self.disk) != before:
            raise AssertionError((label, 'Host block call changed the image'))
        self.quiet(label)
        return value

    def fail(self, name, arguments, token):
        before = disk_hash(self.disk)
        self.wire.send('tools/call', params={'name': name, 'arguments': arguments})
        tool_value(self.wire.get(), code=token)
        if disk_hash(self.disk) != before:
            raise AssertionError(('Host block error changed the image', token))
        self.quiet('host-error-' + token)

    def close(self):
        self.wire.close()


def check_list(value, files, name):
    by_name = dict(item for item in files if item)
    source = logical_hash(files)
    expected = reference.scan(by_name[name])[0]
    check_common(value, 'block_list', name, source, LIST_KEYS)
    require_int(value, 'total', 'list')
    require_int(value, 'shown', 'list')
    if value['total'] != len(expected) or value['shown'] != len(expected) or \
            len(value['blocks']) != value['shown'] or value['shown'] > 64:
        raise AssertionError(('Host list counts', value['total'], value['shown'], len(expected)))
    if type(value['blocks']) is not list:
        raise AssertionError('Host list blocks type')
    for got, block in zip(value['blocks'], expected):
        check_keys(got, BLOCK_KEYS, 'list-row')
        if got['id'] != block['id']:
            raise AssertionError(('Host list id', got['id'], block['id']))
        for key in ('marker_line', 'first_line', 'last_line', 'start', 'end'):
            require_int(got, key, 'list-row')
        if [got['marker_line'], got['first_line'], got['last_line'], got['start'], got['end']] != [
                block['marker'], block['first'], block['last'], block['start'], block['end']]:
            raise AssertionError(('Host list range/lines', got, block))
    return expected


def check_read(host, files, name, block):
    by_name = dict(item for item in files if item)
    data = by_name[name]
    source = logical_hash(files)
    oracle = reference.scan(data)[0]
    block_expected = next((item for item in oracle if item['id'] == block['id']), None)
    if block_expected is None:
        raise AssertionError(('Host read unknown block', block['id']))
    if [block['start'], block['end'], block['first'], block['last']] != [
            block_expected['start'], block_expected['end'],
            block_expected['first'], block_expected['last']]:
        raise AssertionError(('Host read caller block is not the oracle', block, block_expected))
    offset = 0
    joined = b''
    spans, texts = [], []
    while True:
        value = host.call('yanos_block_read', {
            'name': name, 'block_id': block_expected['id'], 'offset': offset,
            'expected_source': source})
        check_common(value, 'block_read', name, source, READ_KEYS)
        if value['block_id'] != block_expected['id']:
            raise AssertionError(('Host read block id', value['block_id']))
        expected = page_expected(data, block_expected, offset)
        if isinstance(expected, tuple):
            raise AssertionError(('Host read unexpected error page', name, offset, expected))
        text = value['text'].encode('utf-8')
        span = expected['page_end'] - expected['page_start']
        if span > 65536 or len(text) != span or text != expected['text']:
            raise AssertionError(('Host read raw page', name, offset, len(text), span))
        check_range(value['block'], dict(start=expected['block_start'], end=expected['block_end'],
                                         first_line=expected['block_first'],
                                         last_line=expected['block_last']), 'read-block')
        check_range(value['page'], dict(start=expected['page_start'], end=expected['page_end'],
                                        first_line=expected['page_first'],
                                        last_line=expected['page_last']), 'read-page')
        if type(value['complete']) is not bool or value['complete'] is not expected['complete']:
            raise AssertionError(('Host read complete', name, offset, value['complete']))
        if expected['complete']:
            if value['next_offset'] is not None:
                raise AssertionError(('Host read terminal next_offset', name, offset))
            if offset + span != block_expected['end'] - block_expected['start']:
                raise AssertionError(('Host read EOF range', name, offset))
            joined += text
            spans.append(span)
            texts.append(value['text'])
            break
        if type(value['next_offset']) is not int:
            raise AssertionError(('Host read next_offset type', name, offset))
        if value['next_offset'] != expected['next_offset'] or value['next_offset'] <= offset:
            raise AssertionError(('Host read next_offset', name, offset, value['next_offset']))
        joined += text
        spans.append(span)
        texts.append(value['text'])
        offset = value['next_offset']
    if joined != data[block_expected['start']:block_expected['end']]:
        raise AssertionError(('Host multipage join', name, block_expected['id']))
    return spans[0], texts


def check_refs(value, files, name):
    by_name = dict(item for item in files if item)
    source = logical_hash(files)
    expected = reference.scan(by_name[name])[1]
    check_common(value, 'block_refs', name, source, REFS_KEYS)
    require_int(value, 'total', 'refs')
    require_int(value, 'shown', 'refs')
    if value['total'] != len(expected) or value['shown'] != min(20, len(expected)) or \
            len(value['refs']) != value['shown']:
        raise AssertionError(('Host refs counts', value['total'], value['shown'], len(expected)))
    if type(value['refs']) is not list:
        raise AssertionError('Host refs array type')
    for got, ref in zip(value['refs'], expected[:20]):
        check_keys(got, REF_ROW_KEYS, 'refs-row')
        if got['source'] != name:
            raise AssertionError(('Host refs source', got['source'], name))
        if ref['source_id'] is None:
            if got['block_id'] is not None:
                raise AssertionError(('Host refs null block_id', got['block_id']))
        elif got['block_id'] != ref['source_id']:
            raise AssertionError(('Host refs block_id', got['block_id'], ref['source_id']))
        for key in ('line', 'start', 'end'):
            require_int(got, key, 'refs-row')
        if [got['line'], got['start'], got['end']] != [ref['line'], ref['start'], ref['end']]:
            raise AssertionError(('Host refs position', got, ref))
        check_keys(got['target'], TARGET_KEYS, 'refs-target')
        if got['target'] != {'name': ref['name'], 'id': ref['id']}:
            raise AssertionError(('Host refs target', got['target'], ref))
        state = reference.target(by_name, ref['name'], ref['id'])
        if got['state'] != state:
            raise AssertionError(('Host refs state', got['state'], state))
    return expected


def check_backlinks(value, files, name, identity):
    by_name = dict(item for item in files if item)
    source = logical_hash(files)
    matches, skipped = [], 0
    for row_source in sorted(by_name):
        try:
            _, refs = reference.scan(by_name[row_source])
        except reference.BlockError as error:
            if error.token == 'INVALID_TEXT':
                skipped += 1
                continue
            raise
        for ref in refs:
            if (ref['name'], ref['id']) == (name, identity):
                matches.append((row_source, ref))
    state = reference.target(by_name, name, identity)
    check_common(value, 'block_backlinks', name, source, BACK_KEYS)
    if value['block_id'] != identity:
        raise AssertionError(('Host backlinks block_id', value['block_id'], identity))
    for key in ('total', 'shown', 'skipped'):
        require_int(value, key, 'backlinks')
    if [value['total'], value['shown'], value['skipped'], value['target_state']] != [
            len(matches), min(20, len(matches)), skipped, state]:
        raise AssertionError(('Host backlinks summary', value['total'], value['shown'],
                              value['skipped'], value['target_state'], len(matches), skipped, state))
    if len(value['backlinks']) != value['shown'] or type(value['backlinks']) is not list:
        raise AssertionError(('Host backlinks array', len(value['backlinks']), value['shown']))
    for got, (row_source, ref) in zip(value['backlinks'], matches[:20]):
        check_keys(got, BACK_ROW_KEYS, 'backlinks-row')
        if got['source'] != row_source:
            raise AssertionError(('Host backlinks source', got['source'], row_source))
        if ref['source_id'] is None:
            if got['block_id'] is not None:
                raise AssertionError(('Host backlinks null block_id', got['block_id']))
        elif got['block_id'] != ref['source_id']:
            raise AssertionError(('Host backlinks block_id', got['block_id'], ref['source_id']))
        for key in ('line', 'start', 'end'):
            require_int(got, key, 'backlinks-row')
        if [got['line'], got['start'], got['end']] != [ref['line'], ref['start'], ref['end']]:
            raise AssertionError(('Host backlinks position', got, ref))
    return matches, skipped


def check_collections(host, files):
    source = logical_hash(files)
    many = host.call('yanos_block_refs', {'name': 'manyrefs', 'expected_source': source})
    check_refs(many, files, 'manyrefs')
    if [many['total'], many['shown'], len(many['refs'])] != [25, 20, 20]:
        raise AssertionError(('Host manyrefs counts', many['total'], many['shown']))
    if [row['state'] for row in many['refs']] != (
            ['OK'] * 9 + ['DANGLING_FILE'] * 8 + ['DANGLING_BLOCK'] * 3):
        raise AssertionError('Host manyrefs order/states')

    back = host.call('yanos_block_backlinks', {'name': 'n', 'block_id': 'a',
                                               'expected_source': source})
    check_backlinks(back, files, 'n', 'a')
    if [back['total'], back['shown'], len(back['backlinks'])] != [28, 20, 20]:
        raise AssertionError(('Host backlinks count', back['total'], back['shown']))
    if [row['source'] for row in back['backlinks']] != ['b'] * 9 + ['c'] * 9 + ['d'] * 2:
        raise AssertionError('Host backlinks order')

    missing = host.call('yanos_block_backlinks', {'name': 'n', 'block_id': 'no_such_block',
                                                  'expected_source': source})
    check_backlinks(missing, files, 'n', 'no_such_block')
    if [missing['total'], missing['shown'], missing['backlinks'], missing['target_state']] != [
            0, 0, [], 'DANGLING_BLOCK']:
        raise AssertionError('Host missing target backlinks')

    gone = host.call('yanos_block_backlinks', {'name': 'gone', 'block_id': 'x',
                                               'expected_source': source})
    check_backlinks(gone, files, 'gone', 'x')
    if [gone['total'], gone['shown'], gone['target_state']] != [8, 8, 'DANGLING_FILE']:
        raise AssertionError('Host dangling-file backlinks')


def host_suite(host, files, name, extra_reads=()):
    source = logical_hash(files)
    listed = host.call('yanos_block_list', {'name': name, 'expected_source': source})
    blocks_expected = check_list(listed, files, name)
    if not blocks_expected:
        raise AssertionError('Host fixture has no blocks')
    reads = {}
    for block in blocks_expected:
        reads[(name, block['id'])] = check_read(host, files, name, block)
    check_refs(host.call('yanos_block_refs', {'name': name, 'expected_source': source}),
               files, name)
    check_backlinks(host.call('yanos_block_backlinks', {
        'name': name, 'block_id': blocks_expected[0]['id'], 'expected_source': source}),
        files, name, blocks_expected[0]['id'])
    by_name = dict(item for item in files if item)
    for extra in extra_reads:
        for block in reference.scan(by_name[extra])[0]:
            reads[(extra, block['id'])] = check_read(host, files, extra, block)
    return reads


def main():
    parser = argparse.ArgumentParser()
    for name in ('source', 'work', 'run', 'mkfs', 'guest', 'server'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    for path in (args.run, args.mkfs, args.guest, args.server):
        if not path.is_file():
            print(':FAIL: missing required input:', path)
            return 1
    args.work.mkdir(parents=True, exist_ok=True)
    disk = args.work / 'same-image.img'
    refs_9 = ('[[n#a]]\n' * 9).encode()
    base_files = [
        ('n', b'@block a\nold\n@block b\nlast\n'),
        ('s', b'[[n#a]] [[renamed#a]]\n'),
        ('long', b'@block long\n' + '中😀\r\n'.encode() + b'x' * 70000),
        ('edge2', b'@block u2\n' + b'a' * 65535 + '\u00e9'.encode() + b'tail'),
        ('edge3', b'@block u3\n' + b'a' * 65535 + '\u4e2d'.encode() + b'tail'),
        ('edge4', b'@block u4\n' + b'a' * 65534 + '\U0001f600'.encode() + b'tail'),
        ('edgecrlf', b'@block c\n' + b'b' * 65535 + b'\r\nend'),
        ('exact', b'@block e\n' + b'z' * 65536 + b'end'),
        ('manyrefs', b'@block src\n' + (b'[[long#long]]\n' * 9 +
                                        b'[[gone#x]]\n' * 8 +
                                        b'[[n#missing]]\n' * 8)),
        ('b', b'@block b\n' + refs_9),
        ('c', b'@block c\n' + refs_9),
        ('d', b'@block d\n' + refs_9),
        ('badtarget', b'@block t\n[[binary#a]]\n'),
        ('binary', b'@block a\n\x00'),
    ]
    REL.seed(disk, base_files)
    before_guest = disk_hash(disk)
    run_guest(args, disk,
              b'edit n\n' + b'd 1\n' * 4 +
              b'a @block a\na new\na @block b\na last\nw\nexit\n',
              args.work, 'guest-edit')
    files = files_from_disk(disk)
    raw_n = dict(item for item in files if item)['n']
    if raw_n != b'@block a\nnew\n@block b\nlast\n':
        raise AssertionError(('Guest edit raw bytes', raw_n))
    guest_view = run_guest(args, disk, b'block list n\nblock show n#a\nexit\n',
                           args.work, 'guest-reopen')
    if b'@block a' in guest_view.stdout or b'new' not in guest_view.stdout:
        # Guest display is checked only as a renderer; raw bytes above are the oracle.
        raise AssertionError('Guest reopen display')
    if disk_hash(disk) == before_guest:
        raise AssertionError('Guest edit did not change the image')

    host = Host(args, disk, args.work)
    edges = ('long', 'edge2', 'edge3', 'edge4', 'edgecrlf', 'exact')
    started = time.monotonic()
    try:
        before = disk_hash(disk)
        reads = host_suite(host, files, 'n', extra_reads=edges)
        if disk_hash(disk) != before:
            raise AssertionError('Host suite changed the image')
        expected_ids = {'long': 'long', 'edge2': 'u2', 'edge3': 'u3',
                        'edge4': 'u4', 'edgecrlf': 'c', 'exact': 'e'}
        for edge in edges:
            span, texts = reads[(edge, expected_ids[edge])]
            if span != EXPECTED_FIRST[edge] or span > 65536:
                raise AssertionError(('Guest edge first page', edge, span))
            if edge == 'long':
                if len(texts) != 2:
                    raise AssertionError(('Guest long pages', len(texts)))
            elif edge == 'edge2':
                if not texts[1].startswith('\u00e9'):
                    raise AssertionError('Guest edge2 second page')
            elif edge == 'edge3':
                if not texts[1].startswith('\u4e2d'):
                    raise AssertionError('Guest edge3 second page')
            elif edge == 'edge4':
                if not texts[1].startswith('\U0001f600'):
                    raise AssertionError('Guest edge4 second page')
            elif edge == 'edgecrlf':
                if not texts[1].startswith('\r\n'):
                    raise AssertionError('Guest edgecrlf second page')
            else:
                if texts[1] != 'end':
                    raise AssertionError('Guest exact second page')

        check_collections(host, files)
        source = logical_hash(files)
        host.fail('yanos_block_list', {'name': 'missing', 'expected_source': source}, 'NOT_FOUND')
        host.fail('yanos_block_read', {'name': 'n', 'block_id': 'missing',
                                       'expected_source': source}, 'NOT_FOUND')
        host.fail('yanos_block_list', {'name': 'binary', 'expected_source': source}, 'INVALID_TEXT')
        host.fail('yanos_block_read', {'name': 'binary', 'block_id': 'a',
                                       'expected_source': source}, 'INVALID_TEXT')
        host.fail('yanos_block_refs', {'name': 'badtarget', 'expected_source': source}, 'INVALID_TEXT')
        host.fail('yanos_block_backlinks', {'name': 'binary', 'block_id': 'a',
                                            'expected_source': source}, 'INVALID_TEXT')

        # Real Guest edit changes the source; an expected_source from before is STALE.
        old_source = host.call('yanos_block_list', {'name': 'n',
                                                    'expected_source': source})['source_sha256']
        run_guest(args, disk, b'edit n\nr 2 changed\nw\nexit\n',
                  args.work, 'guest-second-edit')
        files = files_from_disk(disk)
        raw_n = dict(item for item in files if item)['n']
        if raw_n != b'@block a\nchanged\n@block b\nlast\n':
            raise AssertionError(('Guest second edit raw bytes', raw_n))
        if logical_hash(files) == old_source:
            raise AssertionError('Guest second edit did not change the source')
        host.fail('yanos_block_list', {'name': 'n', 'expected_source': old_source}, 'STALE')
        host_suite(host, files, 'n')

        old_n = dict(item for item in files if item)['n']
        before = disk_hash(disk)
        run_guest(args, disk, b'mv n renamed\nexit\n', args.work, 'guest-mv')
        files = files_from_disk(disk)
        by_name = dict(item for item in files if item)
        if 'n' in by_name or by_name['renamed'] != old_n:
            raise AssertionError('Guest mv persistence')
        if disk_hash(disk) == before:
            raise AssertionError('Guest mv did not change the image')
        refs = host.call('yanos_block_refs', {'name': 's',
                                              'expected_source': logical_hash(files)})
        check_refs(refs, files, 's')
        if [row['state'] for row in refs['refs']] != ['DANGLING_FILE', 'OK']:
            raise AssertionError('Guest refs after rename')
        before = disk_hash(disk)
        host_suite(host, files, 'renamed')
        if disk_hash(disk) != before:
            raise AssertionError('Host suite after mv changed the image')

        run_guest(args, disk, b'cp renamed copy\nexit\n', args.work, 'guest-cp')
        files = files_from_disk(disk)
        by_name = dict(item for item in files if item)
        if by_name['copy'] != by_name['renamed'] or by_name['copy'] != old_n:
            raise AssertionError('Guest cp persistence')
        refs = host.call('yanos_block_refs', {'name': 's',
                                              'expected_source': logical_hash(files)})
        check_refs(refs, files, 's')
        if [row['state'] for row in refs['refs']] != ['DANGLING_FILE', 'OK']:
            raise AssertionError('Guest refs after cp')
        before = disk_hash(disk)
        host_suite(host, files, 'copy')
        if disk_hash(disk) != before:
            raise AssertionError('Host suite after cp changed the image')

        run_guest(args, disk, b'rm renamed\nexit\n', args.work, 'guest-rm')
        files = files_from_disk(disk)
        by_name = dict(item for item in files if item)
        if 'renamed' in by_name or by_name['copy'] != old_n:
            raise AssertionError('Guest rm persistence')
        refs = host.call('yanos_block_refs', {'name': 's',
                                              'expected_source': logical_hash(files)})
        check_refs(refs, files, 's')
        if [row['state'] for row in refs['refs']] != ['DANGLING_FILE', 'DANGLING_FILE']:
            raise AssertionError('Guest refs after rm')
        before = disk_hash(disk)
        host_suite(host, files, 'copy')
        if disk_hash(disk) != before:
            raise AssertionError('Host suite after rm changed the image')
        # Guest display and Host raw bytes are deliberately separate oracles.
        print('PASS Guest/Host same image; host latency %.3fs' % (time.monotonic() - started))
    finally:
        host.close()
    print('block_host: 1 passed, 0 failed, 0 harness errors')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (AssertionError, BASE.HarnessError, OSError, subprocess.TimeoutExpired) as error:
        print(':FAIL:', error)
        sys.exit(1)
