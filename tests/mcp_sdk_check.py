"""Explicit official SDK 2.3.0 acceptance; no SDK dependency in production/default CTest."""
import asyncio
import hashlib
from importlib.metadata import version, PackageNotFoundError
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import block_references_reference as reference
from block_page_oracle import page_expected
from semantic_fixture import image, note
from semantic_json_checks import logical_hash

TOOLS = ['yanos_search', 'yanos_index_status', 'yanos_block_list',
         'yanos_block_read', 'yanos_block_refs', 'yanos_block_backlinks']
WRITE_TOOLS = ['yanos_note_create', 'yanos_note_replace']

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
    'long.txt': 65536,
    'edge2.txt': 65535,   # U+00E9 (2-byte UTF-8) starts at 65535
    'edge3.txt': 65535,   # U+4E2D (3-byte CJK) starts at 65535
    'edge4.txt': 65534,   # U+1F600 (4-byte) starts at 65534, rollback 2 bytes
    'edgecrlf.txt': 65535,  # CR at 65535, LF at 65536, page ends before CR
    'exact65536.txt': 65536,  # no rollback needed at the boundary
}


async def await_pid(path):
    deadline = time.monotonic() + 8
    while not path.exists():
        if time.monotonic() >= deadline:
            raise RuntimeError('SDK fixture runner failed to start')
        await asyncio.sleep(.02)
    return int(path.read_text())


async def await_gone(pid):
    deadline = time.monotonic() + 10
    while Path('/proc', str(pid)).exists():
        if time.monotonic() >= deadline:
            raise RuntimeError('SDK cancellation/EOF failed to reap runner')
        await asyncio.sleep(.02)


def call_value(result):
    result = result.model_dump(by_alias=True, exclude_none=True)
    assert result['isError'] is False, ('sdk_block_error', result)
    value = result['structuredContent']
    assert json.loads(result['content'][0]['text']) == value, 'sdk_block_text_structured'
    return value


def call_error(result, token):
    result = result.model_dump(by_alias=True, exclude_none=True)
    assert result['isError'] is True, ('sdk_block_error_expected', token, result)
    assert 'structuredContent' not in result, 'sdk_error_no_success_context'
    assert json.loads(result['content'][0]['text']) == {'code': token}, ('sdk_error_token', token)


def is_int(value):
    return type(value) is int


def check_common(value, operation, name, source, keys):
    assert set(value) == keys, ('sdk_block_keys', operation, sorted(value))
    assert is_int(value['schema_version']) and value['schema_version'] == 1, 'sdk_block_schema_version'
    assert value['operation'] == operation, ('sdk_block_operation', operation)
    assert value['name'] == name, ('sdk_block_name', name)
    assert value['source_sha256'] == source, ('sdk_block_source', operation)


def check_range(value, expected, label):
    assert set(value) == RANGE_KEYS, (label, sorted(value))
    for key in RANGE_KEYS:
        assert is_int(value[key]) and value[key] >= 0, (label, key, value[key])
    assert [value['start'], value['end'], value['first_line'], value['last_line']] == [
        expected['start'], expected['end'], expected['first_line'], expected['last_line']], label


def check_listing(value, data, name, source):
    expected = reference.scan(data)[0]
    check_common(value, 'block_list', name, source, LIST_KEYS)
    assert is_int(value['total']) and is_int(value['shown'])
    assert value['total'] == value['shown'] == len(expected) <= 64, 'sdk_block_list_counts'
    assert type(value['blocks']) is list and len(value['blocks']) == value['shown'], 'sdk_block_list_shown'
    for got, block in zip(value['blocks'], expected):
        assert set(got) == BLOCK_KEYS, ('sdk_block_row_keys', sorted(got))
        assert got['id'] == block['id'], 'sdk_block_list_id'
        for key in ('marker_line', 'first_line', 'last_line', 'start', 'end'):
            assert is_int(got[key]) and got[key] >= 0, ('sdk_block_row_int', key)
        assert [got['marker_line'], got['first_line'], got['last_line'],
                got['start'], got['end']] == [
            block['marker'], block['first'], block['last'], block['start'], block['end']], \
            ('sdk_block_list_fields', block['id'])
    return expected


def check_page(value, data, block, offset, source, name, block_id):
    expected = page_expected(data, block, offset)
    assert not isinstance(expected, tuple), ('sdk_page_fixture_invalid', name, offset)
    check_common(value, 'block_read', name, source, READ_KEYS)
    assert value['block_id'] == block_id, ('sdk_block_read_id', block_id)
    encoded = value['text'].encode('utf-8')
    span = expected['page_end'] - expected['page_start']
    assert encoded == expected['text'], ('sdk_block_read_bytes', name, offset)
    assert len(encoded) == span <= 65536, ('sdk_block_page_size', name, offset, len(encoded))
    check_range(value['page'], dict(start=expected['page_start'], end=expected['page_end'],
                                    first_line=expected['page_first'],
                                    last_line=expected['page_last']), 'sdk_page_range')
    check_range(value['block'], dict(start=expected['block_start'], end=expected['block_end'],
                                     first_line=expected['block_first'],
                                     last_line=expected['block_last']), 'sdk_block_range')
    assert type(value['complete']) is bool and value['complete'] is expected['complete'], \
        ('sdk_block_complete', name, offset)
    if expected['complete']:
        assert value['next_offset'] is None, ('sdk_block_terminal', name, offset)
    else:
        assert is_int(value['next_offset']), ('sdk_block_next_type', name, offset)
        assert value['next_offset'] == expected['next_offset'], ('sdk_block_next', name, offset)
    return span


async def read_pages(client, data, block, source, name):
    offset = 0
    joined = b''
    spans, texts = [], []
    while True:
        arguments = {'name': name, 'block_id': block['id'], 'offset': offset,
                     'expected_source': source}
        value = call_value(await client.call_tool('yanos_block_read', arguments))
        span = check_page(value, data, block, offset, source, name, block['id'])
        spans.append(span)
        texts.append(value['text'])
        joined += value['text'].encode('utf-8')
        if value['complete']:
            assert value['next_offset'] is None, 'sdk_block_multipage_terminal'
            assert offset + span == block['end'] - block['start'], 'sdk_block_multipage_eof'
            break
        assert value['next_offset'] == offset + span, 'sdk_block_multipage_next'
        assert value['next_offset'] > offset, 'sdk_block_multipage_advance'
        offset = value['next_offset']
    assert joined == data[block['start']:block['end']], ('sdk_block_multipage_bytes', name)
    return spans, texts


def check_refs_value(value, files, name, source):
    by_name = dict(item for item in files if item)
    expected = reference.scan(by_name[name])[1]
    check_common(value, 'block_refs', name, source, REFS_KEYS)
    assert is_int(value['total']) and is_int(value['shown'])
    assert value['total'] == len(expected), ('sdk_block_refs_total', name)
    assert value['shown'] == min(20, value['total']), ('sdk_block_refs_shown', name)
    assert type(value['refs']) is list and len(value['refs']) == value['shown'], 'sdk_block_refs_array'
    for got, ref in zip(value['refs'], expected[:20]):
        assert set(got) == REF_ROW_KEYS, ('sdk_block_ref_row_keys', sorted(got))
        assert got['source'] == name, 'sdk_block_ref_source'
        if ref['source_id'] is None:
            assert got['block_id'] is None, 'sdk_block_ref_null_block_id'
        else:
            assert got['block_id'] == ref['source_id'], 'sdk_block_ref_block_id'
        for key in ('line', 'start', 'end'):
            assert is_int(got[key]) and got[key] >= 0, ('sdk_block_ref_int', key)
        assert [got['line'], got['start'], got['end']] == [
            ref['line'], ref['start'], ref['end']], 'sdk_block_ref_position'
        assert set(got['target']) == TARGET_KEYS, 'sdk_block_ref_target_keys'
        assert got['target'] == {'name': ref['name'], 'id': ref['id']}, 'sdk_block_ref_target'
        assert got['state'] == reference.target(by_name, ref['name'], ref['id']), \
            ('sdk_block_ref_state', ref['name'], ref['id'])
    return expected


def backlinks_expected(by_name, name, identity):
    matches, skipped = [], 0
    for source in sorted(by_name):
        try:
            _, refs = reference.scan(by_name[source])
        except reference.BlockError as error:
            if error.token == 'INVALID_TEXT':
                skipped += 1
                continue
            raise
        matches.extend((source, ref) for ref in refs
                       if (ref['name'], ref['id']) == (name, identity))
    return matches, skipped


def check_backlinks_value(value, files, name, identity, source):
    by_name = dict(item for item in files if item)
    matches, skipped = backlinks_expected(by_name, name, identity)
    check_common(value, 'block_backlinks', name, source, BACK_KEYS)
    assert value['block_id'] == identity, ('sdk_block_backlinks_id', identity)
    for key in ('total', 'shown', 'skipped'):
        assert is_int(value[key]) and value[key] >= 0, ('sdk_block_backlinks_int', key)
    assert value['total'] == len(matches), 'sdk_block_backlinks_total'
    assert value['shown'] == min(20, value['total']), 'sdk_block_backlinks_shown'
    assert value['skipped'] == skipped, 'sdk_block_backlinks_skipped'
    assert type(value['backlinks']) is list and len(value['backlinks']) == value['shown'], \
        'sdk_block_backlinks_array'
    assert value['target_state'] == reference.target(by_name, name, identity), \
        'sdk_block_backlinks_state'
    for got, (row_source, ref) in zip(value['backlinks'], matches[:20]):
        assert set(got) == BACK_ROW_KEYS, ('sdk_block_backlink_row_keys', sorted(got))
        assert got['source'] == row_source, 'sdk_block_backlink_source'
        if ref['source_id'] is None:
            assert got['block_id'] is None, 'sdk_block_backlink_null_block_id'
        else:
            assert got['block_id'] == ref['source_id'], 'sdk_block_backlink_block_id'
        for key in ('line', 'start', 'end'):
            assert is_int(got[key]) and got[key] >= 0, ('sdk_block_backlink_int', key)
        assert [got['line'], got['start'], got['end']] == [
            ref['line'], ref['start'], ref['end']], 'sdk_block_backlink_position'
    return matches, skipped


async def block_calls(client, files, source, source_hash, disk):
    assert source == source_hash == logical_hash(files), 'sdk_block_search_source'
    by_name = dict(item for item in files if item)

    blocks = call_value(await client.call_tool('yanos_block_list',
                                               {'name': 'blocks.txt',
                                                'expected_source': source}))
    expected_blocks = check_listing(blocks, by_name['blocks.txt'], 'blocks.txt', source)
    assert blocks['schema_version'] == 1 and blocks['operation'] == 'block_list', 'sdk_block_list_shape'
    assert blocks['source_sha256'] == source, 'sdk_block_source'
    assert [b['id'] for b in blocks['blocks']] == ['first', 'empty', 'last'], 'sdk_block_list_order'
    assert blocks['total'] == 3 and blocks['shown'] == 3, 'sdk_block_list_counts'

    first = call_value(await client.call_tool('yanos_block_read',
                                              {'name': 'blocks.txt', 'block_id': 'first',
                                               'expected_source': source}))
    first_span = check_page(first, by_name['blocks.txt'], expected_blocks[0], 0,
                            source, 'blocks.txt', 'first')
    assert first_span == len('hello\r\n'.encode()), 'sdk_block_read_first_size'
    assert first['text'] == 'hello\r\n' and first['complete'] is True, 'sdk_block_read_first'
    assert first['next_offset'] is None, 'sdk_block_read_terminal'
    assert first['block'] == first['page'], 'sdk_block_read_full_page'

    empty = call_value(await client.call_tool('yanos_block_read',
                                              {'name': 'blocks.txt', 'block_id': 'empty',
                                               'expected_source': source}))
    blocks_expected = {block['id']: block for block in expected_blocks}
    check_page(empty, by_name['blocks.txt'], blocks_expected['empty'], 0,
               source, 'blocks.txt', 'empty')
    assert empty['text'] == '' and empty['next_offset'] is None, 'sdk_block_read_empty'
    assert empty['block'] == empty['page'], 'sdk_block_read_empty_range'
    assert empty['page']['first_line'] == 0 and empty['page']['last_line'] == 0, \
        'sdk_block_read_empty_lines'

    refs = call_value(await client.call_tool('yanos_block_refs',
                                             {'name': 'refs.txt',
                                              'expected_source': source}))
    check_refs_value(refs, files, 'refs.txt', source)
    assert refs['total'] == 2 and refs['shown'] == 2, 'sdk_block_refs_counts'
    assert refs['refs'][0]['target'] == {'name': 'blocks.txt', 'id': 'first'}, 'sdk_block_refs_target'
    assert refs['refs'][0]['state'] == 'OK' and refs['refs'][1]['state'] == 'DANGLING_FILE', 'sdk_block_refs_state'

    back = call_value(await client.call_tool('yanos_block_backlinks',
                                             {'name': 'blocks.txt', 'block_id': 'first',
                                              'expected_source': source}))
    check_backlinks_value(back, files, 'blocks.txt', 'first', source)
    assert back['total'] == 1 and back['shown'] == 1 and back['skipped'] == 1, 'sdk_block_backlinks_counts'
    assert back['backlinks'][0]['source'] == 'refs.txt' and back['target_state'] == 'OK', 'sdk_block_backlinks_row'

    expected = call_value(await client.call_tool('yanos_block_list',
        {'name': 'blocks.txt', 'expected_source': source}))
    assert expected['source_sha256'] == source, 'sdk_block_expected_success'

    long_blocks = reference.scan(by_name['long.txt'])[0]
    long_spans, _ = await read_pages(client, by_name['long.txt'], long_blocks[0], source, 'long.txt')
    assert long_spans[0] == EXPECTED_FIRST['long.txt'], ('sdk_block_long_first', long_spans[0])
    assert sum(long_spans) == len(by_name['long.txt']) - long_blocks[0]['start'], 'sdk_block_long_total'

    for name in ('edge2.txt', 'edge3.txt', 'edge4.txt', 'edgecrlf.txt', 'exact65536.txt'):
        edge_blocks = reference.scan(by_name[name])[0]
        spans, texts = await read_pages(client, by_name[name], edge_blocks[0], source, name)
        assert spans[0] == EXPECTED_FIRST[name], ('sdk_block_edge_first', name, spans[0])
        assert spans[0] <= 65536 and sum(spans) == len(by_name[name]) - edge_blocks[0]['start'], \
            ('sdk_block_edge_total', name)
        assert len(spans) == 2, ('sdk_block_edge_pages', name)
        if name == 'edge2.txt':
            assert texts[1].startswith('\u00e9'), 'sdk_block_edge_u2_second'
        elif name == 'edge3.txt':
            assert texts[1].startswith('\u4e2d'), 'sdk_block_edge_u3_second'
        elif name == 'edge4.txt':
            assert texts[1].startswith('\U0001f600'), 'sdk_block_edge_u4_second'
        elif name == 'edgecrlf.txt':
            assert texts[1].startswith('\r\n'), 'sdk_block_edge_crlf_second'
        else:
            assert texts[1] == 'end', 'sdk_block_edge_exact_second'

    many = call_value(await client.call_tool('yanos_block_refs',
                                             {'name': 'manyrefs',
                                              'expected_source': source}))
    check_refs_value(many, files, 'manyrefs', source)
    assert many['total'] == 25 and many['shown'] == 20 and len(many['refs']) == 20, \
        'sdk_block_refs_20'
    assert [row['state'] for row in many['refs']] == (
        ['OK'] * 9 + ['DANGLING_FILE'] * 8 + ['DANGLING_BLOCK'] * 3), 'sdk_block_refs_20_states'

    tail = call_value(await client.call_tool('yanos_block_backlinks',
                                             {'name': 'blocks.txt', 'block_id': 'last',
                                              'expected_source': source}))
    tail_matches, _ = check_backlinks_value(tail, files, 'blocks.txt', 'last', source)
    assert tail['total'] == 27 and tail['shown'] == 20 and len(tail['backlinks']) == 20, \
        'sdk_block_backlinks_20'
    assert [row['source'] for row in tail['backlinks']] == (
        ['b'] * 9 + ['c'] * 9 + ['d'] * 2), 'sdk_block_backlinks_order'
    assert tail['target_state'] == 'OK', 'sdk_block_backlinks_target_state'
    assert tail_matches[0][0] == 'b' and tail_matches[-1][0] == 'd', 'sdk_block_backlinks_oracle_order'

    missing = call_value(await client.call_tool('yanos_block_backlinks',
                                                {'name': 'blocks.txt',
                                                 'block_id': 'missing_target',
                                                 'expected_source': source}))
    check_backlinks_value(missing, files, 'blocks.txt', 'missing_target', source)
    assert missing['total'] == 0 and missing['shown'] == 0 and missing['backlinks'] == [] and \
        missing['target_state'] == 'DANGLING_BLOCK', 'sdk_block_backlinks_missing_target'

    call_error(await client.call_tool('yanos_block_refs',
        {'name': 'badtarget', 'expected_source': source}), 'INVALID_TEXT')
    call_error(await client.call_tool('yanos_block_read',
        {'name': 'binary', 'block_id': 'a', 'expected_source': source}), 'INVALID_TEXT')
    call_error(await client.call_tool('yanos_block_backlinks',
        {'name': 'binary', 'block_id': 'a', 'expected_source': source}), 'INVALID_TEXT')

    await negative_calls(client, files, source, disk)

    # A real image change invalidates the previously verified source identity.
    changed = [(name, data if name != 'blocks.txt' else b'@block changed\nnew\n')
               for name, data in files]
    disk.write_bytes(image(changed))
    call_error(await client.call_tool('yanos_block_list',
        {'name': 'blocks.txt', 'expected_source': source}), 'STALE')
    disk.write_bytes(image(files))


async def negative_calls(client, files, source, disk):
    """Error-token matrix on a probe image; the production image is restored."""
    # The semantic rebuild scans the whole base image, so metadata/binary/limit
    # probes are added only after the search-backed identity is established.
    augmented = files + [
        ('many65.txt', b''.join(b'@block b%02d\n' % i for i in range(65))),
        ('dup.txt', b'@block same\nx\n@block same\n'),
        ('badhead.txt', b'@yan-note 9\n@end\n'),
        ('badblock.txt', b'@block bad id\n'),
    ]
    disk.write_bytes(image(augmented))
    try:
        probe_source = logical_hash(augmented)
        cases = [
            ('yanos_block_list', {'name': 'missing', 'expected_source': probe_source}, 'NOT_FOUND'),
            ('yanos_block_read', {'name': 'blocks.txt', 'block_id': 'missing',
                                  'expected_source': probe_source}, 'NOT_FOUND'),
            ('yanos_block_refs', {'name': 'missing', 'expected_source': probe_source}, 'NOT_FOUND'),
            ('yanos_block_list', {'name': 'binary', 'expected_source': probe_source}, 'INVALID_TEXT'),
            ('yanos_block_read', {'name': 'binary', 'block_id': 'a',
                                  'expected_source': probe_source}, 'INVALID_TEXT'),
            ('yanos_block_list', {'name': 'badhead.txt', 'expected_source': probe_source}, 'METADATA'),
            ('yanos_block_list', {'name': 'badblock.txt', 'expected_source': probe_source}, 'BLOCK_FORMAT'),
            ('yanos_block_list', {'name': 'many65.txt', 'expected_source': probe_source}, 'LIMIT'),
            ('yanos_block_list', {'name': 'dup.txt', 'expected_source': probe_source}, 'AMBIGUOUS'),
            ('yanos_block_list', {'name': 'blocks.txt', 'extra': 1,
                                  'expected_source': probe_source}, 'INVALID'),
            ('yanos_block_list', {'name': 'blocks.txt',
                                  'expected_source': 'A' * 64}, 'INVALID'),
            ('yanos_block_read', {'name': 'blocks.txt', 'block_id': 'first',
                                  'offset': 1}, 'INVALID'),
            ('yanos_block_read', {'name': 'blocks.txt', 'block_id': 'first',
                                  'offset': True, 'expected_source': probe_source}, 'INVALID'),
            ('yanos_block_read', {'name': 'blocks.txt', 'block_id': 'first',
                                  'offset': 1.0, 'expected_source': probe_source}, 'INVALID'),
            ('yanos_block_read', {'name': 'blocks.txt', 'block_id': 'first',
                                  'offset': -1, 'expected_source': probe_source}, 'INVALID'),
            ('yanos_block_read', {'name': 'blocks.txt', 'block_id': 'first',
                                  'offset': 4294967296,
                                  'expected_source': probe_source}, 'INVALID'),
            ('yanos_block_read', {'name': 'blocks.txt', 'block_id': 'first',
                                  'offset': 1000, 'expected_source': probe_source}, 'INVALID'),
            ('yanos_block_read', {'name': 'blocks.txt'}, 'INVALID'),
            ('yanos_block_read', {'name': 'edge3.txt', 'block_id': 'u3',
                                  'offset': 65536,
                                  'expected_source': probe_source}, 'INVALID'),
            ('yanos_block_read', {'name': 'edgecrlf.txt', 'block_id': 'c',
                                  'offset': 65536,
                                  'expected_source': probe_source}, 'INVALID'),
            ('yanos_block_backlinks', {'name': 'blocks.txt', 'block_id': 'first',
                                       'offset': 0,
                                       'expected_source': probe_source}, 'INVALID'),
        ]
        for name, arguments, token in cases:
            before = hashlib.sha256(disk.read_bytes()).digest()
            call_error(await client.call_tool(name, arguments), token)
            assert hashlib.sha256(disk.read_bytes()).digest() == before, \
                ('sdk_block_error_changed_image', name, token)

        # A valid nonzero offset still returns the independently expected page.
        first_block = reference.scan(dict(augmented)['blocks.txt'])[0][0]
        value = call_value(await client.call_tool('yanos_block_read',
            {'name': 'blocks.txt', 'block_id': 'first', 'offset': 1,
             'expected_source': probe_source}))
        check_page(value, dict(augmented)['blocks.txt'], first_block, 1,
                   probe_source, 'blocks.txt', 'first')

        # Block BUSY/cancel/reap/reuse is not asserted through the SDK here:
        # the deterministic YAN_MCP_TEST_BLOCK_HOLD/DELAY hooks live only in
        # tests/mcp_fault_hooks.c, which is linked into mcp_fault_probe rather
        # than the production yan_mcp used by this acceptance, and a 70KiB read
        # finishes before a second call can observe BUSY. The fault suite
        # (tests/mcp_fault_checks.py test_block_busy_cancel_and_reuse) and
        # native tests/test_block_query.c own that lifecycle; see
        # build/0033-taskD-correction1-sdk-guest-report.md.
    finally:
        disk.write_bytes(image(files))


def with_paths(options, model=None, index=None, runner=None):
    result = list(options)
    for flag, value in (('--model', model), ('--index', index), ('--runner', runner)):
        if value is not None:
            result[result.index(flag) + 1] = str(value)
    return result


async def no_dependency_calls(server, options, root, mode):
    fifo = [root / ('sdk-fifo-' + mode + '-' + n) for n in ('index', 'model', 'runner')]
    for path in fifo:
        if path.exists():
            path.unlink()
        os.mkfifo(path)
    pidfile = root / ('sdk-no-runner-' + mode)
    if pidfile.exists():
        pidfile.unlink()
    args = with_paths(options, index=fifo[0], model=fifo[1], runner=fifo[2])
    env = dict(os.environ, YAN_TEST_SERVER_PID_FILE=str(pidfile))
    from mcp import Client
    from mcp.client.stdio import StdioServerParameters
    async with Client(StdioServerParameters(command=str(server), args=args, env=env),
                      mode=mode, read_timeout_seconds=10) as client:
        value = call_value(await client.call_tool('yanos_block_list',
                                                  {'name': 'blocks.txt'}))
        assert value['total'] == 3, 'sdk_block_without_dependencies'
        # All three other options are FIFOs with no writer; opening any would
        # block/time out, so a fast successful block call proves they are unused.
        assert not pidfile.exists(), 'sdk_block_no_runner_process'
    absent = with_paths(options, index=root / 'absent-index',
                        model=root / 'absent-model', runner=root / 'absent-runner')
    async with Client(StdioServerParameters(command=str(server), args=absent),
                      mode=mode, read_timeout_seconds=10) as client:
        call_error(await client.call_tool('yanos_search', {'query': 'cpu'}), 'DEPENDENCY')


async def write_tools(server, options):
    """0035-R2-AC1: official SDK sees six tools plus the two write tools."""
    from mcp import Client
    from mcp.client.stdio import StdioServerParameters
    target = StdioServerParameters(command=str(server), args=[*options, '--allow-write'])
    expected_annotations = {
        'yanos_note_create': {'readOnlyHint': False, 'destructiveHint': False,
                              'openWorldHint': False},
        'yanos_note_replace': {'readOnlyHint': False, 'destructiveHint': True,
                               'idempotentHint': True, 'openWorldHint': False},
    }
    expected_operations = {
        'yanos_note_create': {'note_create', 'note_create_preview'},
        'yanos_note_replace': {'note_replace', 'note_replace_preview'},
    }
    required_text = ('criteria', 'known', 'unknown', 'untouched', 'log',
                     '写 `log` 和 `unknown`',
                     '把条目移入 `known` 必须在同一会话里得到闫博明确确认',
                     '把预览的 `text` 和 `diff` 展示给闫博')
    async with Client(target, mode='legacy', read_timeout_seconds=15) as client:
        assert client.server_info is not None, 'sdk_server_identity_missing'
        assert client.server_info.version == '0035', 'sdk_server_version_write'
        listing = await client.list_tools()
        assert [t.name for t in listing.tools] == TOOLS + WRITE_TOOLS, 'sdk_write_discovery'
        for tool in listing.tools[-2:]:
            annotations = (tool.annotations.model_dump(by_alias=True, exclude_none=True)
                           if tool.annotations else {})
            assert annotations == expected_annotations[tool.name], \
                ('sdk_write_annotations', tool.name, annotations)
            schema = tool.input_schema
            assert schema['type'] == 'object' and schema['additionalProperties'] is False, \
                ('sdk_write_input_closed', tool.name)
            assert schema['required'] == ['name', 'text'], ('sdk_write_required', tool.name)
            assert set(schema['properties']) == {'name', 'text', 'expected_source', 'dry_run'}, \
                ('sdk_write_input_properties', tool.name)
            assert schema['properties']['name'] == {
                'type': 'string', 'minLength': 1, 'maxLength': 31}, 'sdk_write_name_bounds'
            assert schema['properties']['text']['type'] == 'string', 'sdk_write_text'
            assert schema['properties']['expected_source']['pattern'] == '^[0-9a-f]{64}$', \
                'sdk_write_expected_source'
            assert schema['properties']['dry_run'] == {'type': 'boolean'}, 'sdk_write_dry_run'
            output = tool.output_schema
            assert output['type'] == 'object' and len(output['oneOf']) == 2, \
                ('sdk_write_output_oneof', tool.name)
            operations = set()
            for branch in output['oneOf']:
                operation = branch['properties']['operation']
                operations.update(operation.get('enum', [operation.get('const')]))
            assert operations == expected_operations[tool.name], \
                ('sdk_write_output_operations', tool.name, operations)
            assert all(phrase in tool.description for phrase in required_text), \
                ('sdk_write_description', tool.name)
        print('PASS official mcp==2.3.0 write-tools schema+annotations version=0035')


async def run(server, options, source_hash, root, files):
    from mcp import Client
    from mcp.client.stdio import StdioServerParameters
    disk = Path(options[options.index('--disk-image') + 1])
    for mode in ('2026-07-28', 'legacy', 'auto'):
        target = StdioServerParameters(command=str(server), args=options)
        async with Client(target, mode=mode, read_timeout_seconds=15) as client:
            expected = '2025-11-25' if mode == 'legacy' else '2026-07-28'
            assert client.protocol_version == expected, ('actual_protocol_version', mode, client.protocol_version)
            if client.server_info is not None:
                assert client.server_info.version == '0035', ('sdk_server_version', mode)
            listing = await client.list_tools()
            assert [t.name for t in listing.tools] == TOOLS, 'sdk_discovery'
            status = (await client.call_tool('yanos_index_status')).model_dump(by_alias=True, exclude_none=True)
            assert status['isError'] is False and status['structuredContent']['state'] == 'READY', 'sdk_status'
            result = (await client.call_tool('yanos_search', {'query': '设备如何叫醒处理器'})).model_dump(by_alias=True, exclude_none=True)
            assert result['isError'] is False, ('sdk_search_failed', result)
            value = result['structuredContent']
            assert value['source_sha256'] == source_hash and value['shown'] > 0, 'sdk_actual_evidence'
            assert json.loads(result['content'][0]['text']) == value, 'sdk_compatible_content'
            failure = (await client.call_tool('yanos_search', {'query': ''})).model_dump(by_alias=True, exclude_none=True)
            assert failure['isError'] is True and 'structuredContent' not in failure, 'sdk_error_not_empty_success'
            assert json.loads(failure['content'][0]['text']) == {'code': 'INVALID'}, 'sdk_error_token'
            # The legacy search identity is the only expected source for every
            # block call in this mode, not a value re-derived per tool.
            await block_calls(client, files, value['source_sha256'], source_hash, disk)
            print('PASS official mcp==2.3.0 stdio', mode, 'actual=' + client.protocol_version,
                  'rows=' + str(value['shown']), 'block_tools=4')
        await no_dependency_calls(server, options, root, mode)
        print('PASS official mcp==2.3.0 block-no-dependency', mode, 'fifo-paths+absent-pidfile')
        pidfile = root / ('sdk-pid-' + mode)
        target = StdioServerParameters(command=str(server), args=options,
            env=dict(os.environ, YAN_TEST_EMBED_DELAY='2', YAN_TEST_SERVER_PID_FILE=str(pidfile)))
        async with Client(target, mode=mode, read_timeout_seconds=15) as client:
            task = asyncio.create_task(client.call_tool('yanos_search', {'query': 'cpu'}))
            pid = await await_pid(pidfile)
            task.cancel()
            try:
                await task
                raise AssertionError('SDK cancellation unexpectedly returned a result')
            except asyncio.CancelledError:
                pass
            await await_gone(pid)
            deadline = time.monotonic() + 10
            while True:
                r = (await client.call_tool('yanos_index_status')).model_dump(by_alias=True, exclude_none=True)
                if not r['isError']:
                    assert r['structuredContent']['state'] == 'READY', 'sdk_after_cancel_status'
                    break
                assert json.loads(r['content'][0]['text']) == {'code': 'BUSY'}, 'sdk_cancel_failure_boundary'
                if time.monotonic() >= deadline:
                    raise RuntimeError('SDK cancelled task slot not reclaimed')
                await asyncio.sleep(.02)
            pidfile.unlink()
            eof_task = asyncio.create_task(client.call_tool('yanos_search', {'query': 'cpu'}))
            eof_pid = await await_pid(pidfile)
        abandoned = await asyncio.gather(eof_task, return_exceptions=True)
        assert isinstance(abandoned[0], BaseException), 'sdk_close_did_not_publish_success'
        await await_gone(eof_pid)
        print('PASS official mcp==2.3.0 stdio', mode, 'cancel+reuse+active-close/reap')
    await write_tools(server, options)


def main():
    server, semantic, runner = map(lambda p: Path(p).resolve(), sys.argv[1:])
    for p in (server, semantic, runner):
        if not p.is_file():
            raise RuntimeError('FAIL implementation/fixture missing: ' + str(p))
    try:
        sdk = version('mcp')
    except PackageNotFoundError:
        print('SKIP external official MCP SDK missing', file=sys.stderr)
        return 77
    if sdk != '2.3.0':
        raise RuntimeError('expected pinned mcp==2.3.0, found ' + sdk)
    with tempfile.TemporaryDirectory(prefix='yan-mcp-sdk-') as tmp:
        root = Path(tmp)
        disk, index, model = [root / n for n in ('source', 'index', 'model')]
        manyrefs = b'@block src\n' + (b'[[long.txt#long]]\n' * 9 +
                                       b'[[gone#x]]\n' * 8 +
                                       b'[[blocks.txt#nope]]\n' * 8)
        cross = lambda identity: b'@block b\n' + (('[[blocks.txt#%s]]\n' % identity).encode() * 9)
        files = [
            ('irq.txt', note('外设中断', '外设经PLIC通知CPU处理中断。UART收到字符后唤醒等待任务。\r\n')),
            ('uart.txt', b'CPU wakes a waiting task when a device interrupt arrives.\n'),
            ('blocks.txt', b'@block first\r\nhello\r\n@block empty\n@block last\nend\n'),
            ('refs.txt', b'@block src\n[[blocks.txt#first]] [[gone#x]]\n'),
            ('long.txt', b'@block long\n' + '中😀\r\n'.encode() + b'x' * 70000),
            ('binary', b'@block a\n\x00'),
            ('badtarget', b'@block t\n[[binary#a]]\n'),
            ('manyrefs', manyrefs),
            ('b', cross('last')),
            ('c', cross('last')),
            ('d', cross('last')),
            # Q=65536 for the first page lands on/inside the final character.
            ('edge2.txt', b'@block u2\n' + b'a' * 65535 + '\u00e9'.encode() + b'tail'),
            ('edge3.txt', b'@block u3\n' + b'a' * 65535 + '\u4e2d'.encode() + b'tail'),
            ('edge4.txt', b'@block u4\n' + b'a' * 65534 + '\U0001f600'.encode() + b'tail'),
            ('edgecrlf.txt', b'@block c\n' + b'b' * 65535 + b'\r\nend'),
            ('exact65536.txt', b'@block e\n' + b'z' * 65536 + b'end'),
        ]
        disk.write_bytes(image(files)); model.write_text('universal')
        options = ['--disk-image', str(disk), '--index', str(index), '--model', str(model),
                   '--runner', str(runner), '--startup-timeout', '3', '--request-timeout', '3']
        p = subprocess.run([str(semantic), 'rebuild', *options], capture_output=True, timeout=15)
        if p.returncode:
            raise RuntimeError(('SDK fixture rebuild failed', p.stderr))
        before = [hashlib.sha256(p.read_bytes()).digest() for p in (disk, index, model, runner)]
        asyncio.run(run(server, options, logical_hash(files), root, files))
        assert before == [hashlib.sha256(p.read_bytes()).digest() for p in (disk, index, model, runner)], 'sdk_read_only'
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
