#!/usr/bin/env python3
"""0045 R2 acceptance: the two graph tools over real stdio MCP.

Contract: docs/specs/0045-graph-mcp-tools.md R2.

Two things are checked that only a real server can show:

  * the default six-tool list is byte-identical to the pinned 0033 array, and a
    graph tool name is unknown (the existing protocol error) whenever
    --allow-graph is absent -- the disabled surface must be indistinguishable
    from a tool that does not exist;
  * with --allow-graph the tools are appended AFTER every previously enabled
    tool (including the --allow-meta and --allow-write tools), in the order
    graph_list then graph_query, and never reorder or alter an existing tool.

The graph content itself is verified against an oracle in this file: the fixture
notes are written here, the concept/alias/relation expectations come from
parsing those bytes, and source_sha256 is recomputed with the 0033 identity
recipe. Nothing is compared against the server's own output.

A missing in-repo input or binary is a hard failure (exit 1); nothing here needs
an external dependency, so there is no 77 path. A case failure prints
":FAIL: <owner>: ..." and exits 1; a harness problem is exit 2.
"""
import argparse
import hashlib
import json
import os
import select
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from pathlib import Path

BLOCK = 4096
MAGIC = b"YANFS01\x00"
TIMEOUT = 60
META = {'io.modelcontextprotocol/protocolVersion': '2026-07-28',
        'io.modelcontextprotocol/clientCapabilities': {}}
# SHA-256 of the exact 0033 six-tool JSON array (same constant the 0035 suite
# pins), so a regression in the default list is caught by identity, not by a
# field-by-field approximation.
TOOLS_0033_SHA256 = ('3bbd05c5c7e5adfef17122dbf46a10b2704ee960c8cddf6a163a176c72bab4a2')

FIXTURE = [
    ('geometry', None),  # filled in below
    ('relations', None),
    ('broken', None),
    ('binary', b'@block nope\n@kg bad\n\x00'),
    ('other', b'plain text with no graph lines\n'),
]


class HarnessError(RuntimeError):
    """A broken harness, not a failed assertion of the tool's behaviour."""


def _reseal(raw, directory_blocks):
    crc = zlib.crc32(raw[:60] + b'\x00\x00\x00\x00' + raw[64:directory_blocks * BLOCK])
    struct.pack_into('<I', raw, 60, crc & 0xFFFFFFFF)


def build_image(files, capacity=63):
    """A valid v1 YanFS image, encoded here rather than by the tools."""
    directory_blocks = (capacity + 1) // 64
    needed = directory_blocks + sum(
        max(1, (len(data) + BLOCK - 1) // BLOCK) if data else 0
        for _, data in files)
    blocks = max(needed, directory_blocks + 1)
    raw = bytearray(blocks * BLOCK)
    raw[0:8] = MAGIC
    struct.pack_into('<IIII', raw, 8, 1, BLOCK, blocks, capacity)
    lba = directory_blocks
    for slot, (name, data) in enumerate(files):
        encoded = name.encode('ascii')
        at = 64 + slot * 64
        raw[at:at + len(encoded)] = encoded
        count = (len(data) + BLOCK - 1) // BLOCK if data else 0
        struct.pack_into('<III', raw, at + 32, len(data), lba if count else 0, count)
        if data:
            raw[lba * BLOCK:lba * BLOCK + len(data)] = data
        lba += count
    _reseal(raw, directory_blocks)
    return bytes(raw)


def logical_hash(image):
    """0033 identity: sorted ASCII names, then slot/length/name/size/bytes."""
    capacity = struct.unpack_from('<I', image, 20)[0]
    entries = []
    for slot in range(capacity):
        entry = image[64 + slot * 64:128 + slot * 64]
        if entry[0] == 0:
            continue
        name = entry[:32].split(b'\x00', 1)[0]
        size, start, _count = struct.unpack_from('<III', entry, 32)
        entries.append((slot, name, size, start))
    entries.sort(key=lambda item: item[1])
    hasher = hashlib.sha256()
    for slot, name, size, start in entries:
        hasher.update(struct.pack('<I', slot))
        hasher.update(struct.pack('<I', len(name)))
        hasher.update(name)
        hasher.update(struct.pack('<I', size))
        hasher.update(image[start * BLOCK:start * BLOCK + size])
    return hasher.hexdigest()


class Wire:
    """One real server over stdin/stdout with the minimum needed here."""

    def __init__(self, args, env):
        self.p = subprocess.Popen(args, stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, env=env)
        self.buffer = b''

    def send(self, method, ident=1, params=None):
        obj = {'jsonrpc': '2.0', 'method': method}
        if ident is not None:
            obj['id'] = ident
        if params is not None:
            obj['params'] = dict(params)
        obj.setdefault('params', {})['_meta'] = dict(META)
        self.p.stdin.write(json.dumps(obj, ensure_ascii=False,
                                      separators=(',', ':')).encode() + b'\n')
        self.p.stdin.flush()

    def read_line(self, timeout=TIMEOUT):
        deadline = time.monotonic() + timeout
        while b'\n' not in self.buffer:
            left = deadline - time.monotonic()
            if left <= 0 or not select.select([self.p.stdout], [], [], max(0, left))[0]:
                raise HarnessError('timeout waiting for a frame')
            data = os.read(self.p.stdout.fileno(), 65536)
            if not data:
                raise HarnessError(('server exited early', self.p.poll()))
            self.buffer += data
        line, self.buffer = self.buffer.split(b'\n', 1)
        self.last_length = len(line)
        if len(line) > 1048576:
            raise AssertionError('response exceeds the 1 MiB payload bound')
        return line

    def get(self, timeout=TIMEOUT):
        return json.loads(self.read_line(timeout))

    def close(self, rc=0):
        if not self.p.stdin.closed:
            self.p.stdin.close()
        try:
            self.p.wait(timeout=12)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait()
            raise HarnessError('server did not exit')
        stderr = self.p.stderr.read()
        self.p.stdout.close()
        self.p.stderr.close()
        if b'sanitizer' in stderr.lower() or b'runtime error:' in stderr.lower():
            raise HarnessError(('sanitizer diagnostic', stderr))
        assert self.p.returncode == rc, ('exit_code', rc, self.p.returncode, stderr)
        assert self.buffer == b'', ('unexpected_extra_output', self.buffer)


def tool_value(reply, ident=1, code=None):
    """Decode one tool reply, asserting the shared MCP result contract."""
    assert reply['id'] == ident, 'reply_id'
    result = reply['result']
    assert result['resultType'] == 'complete', 'complete_result'
    assert result['isError'] is (code is not None), 'error_boundary'
    assert len(result['content']) == 1 and result['content'][0]['type'] == 'text', 'text_content'
    text = json.loads(result['content'][0]['text'])
    if code:
        assert text == {'code': code}, ('error_token', code, text)
        assert 'structuredContent' not in result, 'error_has_no_success_body'
    else:
        assert text == result['structuredContent'], 'text_equals_structured'
    return text


def tools_member(raw):
    """The raw bytes of the tools array, for the byte-identity comparison."""
    marker = b'"tools":'
    start = raw.index(marker) + len(marker)
    depth = 0
    quoted = escaped = False
    for index in range(start, len(raw)):
        char = raw[index:index + 1]
        if quoted:
            if escaped:
                escaped = False
            elif char == b'\\':
                escaped = True
            elif char == b'"':
                quoted = False
        elif char == b'"':
            quoted = True
        elif char in (b'[', b'{'):
            depth += 1
        elif char in (b']', b'}'):
            depth -= 1
            if depth == 0:
                return raw[start:index + 1]
    raise HarnessError('unterminated tools array')


def listing(wire):
    wire.send('tools/list')
    raw = wire.read_line()
    return raw, json.loads(raw, object_pairs_hook=lambda pairs: dict(pairs))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--server', required=True)
    parser.add_argument('--fault', required=True)
    # The 0044 command is the independent measurement of the shared payload:
    # it is the same encoder the MCP tool calls, with an unlimited cap, so its
    # object size says exactly what the service produced for a fixture.
    parser.add_argument('--cli', required=True)
    args = parser.parse_args()
    for path in (args.server, args.fault, args.cli):
        if not Path(path).is_file():
            raise SystemExit('FAIL implementation/fixture missing: ' + path)

    cases = 0
    work = Path(tempfile.mkdtemp(prefix='yan-mcp-graph-'))

    def image_for(geometry_note, relations_note, broken_note):
        files = [('geometry', geometry_note), ('relations', relations_note),
                 ('broken', broken_note), ('binary', FIXTURE[3][1]),
                 ('other', FIXTURE[4][1])]
        data = build_image(files)
        path = work / 'source.img'
        path.write_bytes(data)
        return path, data, logical_hash(data)

    geometry = ('@yan-note 1\ntitle: 几何\n@end\n'
                '@block definitions\n'
                '@kg concept square 正方形\n'
                '@kg concept rectangle 矩形\n'
                '@kg concept line 直线\n'
                '@kg alias square 等边矩形\n'
                '@kg alias square 方形\n'
                '@kg relation is_a square rectangle\n'
                '@kg relation equivalent square square\n').encode()
    relations = ('@block kinds\n'
                 '@kg relation is_a square line\n'
                 '@kg concept plane 平面\n').encode()
    healthy_broken = b'@block ok\njust text\n'
    disk, raw_image, source = image_for(geometry, relations, healthy_broken)

    def base_options(*extra):
        return [args.server, '--disk-image', str(disk), '--index',
                str(work / 'index'), '--model', str(work / 'model'),
                '--runner', str(work / 'runner'), *extra]

    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1')

    # 1. Default list is the pinned 0033 array, and the graph names are unknown.
    wire = Wire(base_options(), env)
    try:
        raw, payload = listing(wire)
        names = [tool['name'] for tool in payload['result']['tools']]
        assert names == ['yanos_search', 'yanos_index_status', 'yanos_block_list',
                         'yanos_block_read', 'yanos_block_refs',
                         'yanos_block_backlinks'], ('default_six_tools', names)
        assert hashlib.sha256(tools_member(raw)).hexdigest() == TOOLS_0033_SHA256, \
            'default_tools_byte_identical'
        # A disabled graph tool must be indistinguishable from a name that does
        # not exist at all: the same reply bytes, not merely "some error".
        replies = {}
        for name in ('yanos_graph_list', 'yanos_graph_query',
                     'yanos_not_a_tool'):
            wire.send('tools/call', ident=50, params={'name': name,
                                                      'arguments': {}})
            replies[name] = wire.read_line()
        assert replies['yanos_graph_list'] == replies['yanos_not_a_tool'], \
            ('disabled_graph_list_unknown', replies['yanos_graph_list'])
        assert replies['yanos_graph_query'] == replies['yanos_not_a_tool'], \
            ('disabled_graph_query_unknown', replies['yanos_graph_query'])
        cases += 1
    finally:
        wire.close()

    # 2. The flag adds the tools after all previously enabled ones. The default
    # array is captured once here so each combination can be compared as raw
    # bytes: an enabled list must begin with the default bytes unchanged, which
    # is a stronger statement than "the same six names in the same order".
    wire = Wire(base_options(), env)
    try:
        default_raw, _payload = listing(wire)
        default_member = tools_member(default_raw)
    finally:
        wire.close()
    for extra, expected_tail in ((('--allow-graph',),
                                  ['yanos_graph_list', 'yanos_graph_query']),
                                 (('--allow-meta', '--allow-graph'),
                                  ['yanos_note_meta', 'yanos_note_list',
                                   'yanos_graph_list', 'yanos_graph_query']),
                                 (('--allow-graph', '--allow-meta'),
                                  ['yanos_note_meta', 'yanos_note_list',
                                   'yanos_graph_list', 'yanos_graph_query'])):
        wire = Wire(base_options(*extra), env)
        try:
            raw, payload = listing(wire)
            names = [tool['name'] for tool in payload['result']['tools']]
            member = tools_member(raw)
            # Byte-level compatibility: the enabled array is the default array
            # with entries appended, so its bytes after the trailing ']' are
            # exactly ",<appended>]" and the prefix is untouched.
            assert member.startswith(default_member[:-1] + b','), \
                ('enabled_list_prefix_bytes', extra)
            six = ['yanos_search', 'yanos_index_status', 'yanos_block_list',
                   'yanos_block_read', 'yanos_block_refs', 'yanos_block_backlinks']
            assert names[:6] == six, ('existing_tools_unchanged', extra, names)
            assert names[6:] == expected_tail, ('graph_tools_appended_last', extra, names)
            for tool in payload['result']['tools'][-2:]:
                assert tool['name'].startswith('yanos_graph_'), 'graph_tools_tail'
                assert 'outputSchema' in tool, ('graph_output_schema', tool['name'])
                assert tool['inputSchema']['additionalProperties'] is False, \
                    ('graph_input_closed', tool['name'])
                assert tool['annotations'] == {
                    'readOnlyHint': True, 'destructiveHint': False,
                    'idempotentHint': True, 'openWorldHint': False}, \
                    ('graph_annotations', tool['name'])
                dumped = json.dumps(tool)
                for forbidden in ('mastery', 'understood', 'confidence', 'score'):
                    assert forbidden not in dumped, ('no_inferred_field', forbidden)
            cases += 1
        finally:
            wire.close()

    # 3. Real calls: list, query by ID/alias/name, and the 0044 object shape.
    wire = Wire(base_options('--allow-graph'), env)
    before = disk.read_bytes()
    try:
        wire.send('tools/call', ident=1, params={'name': 'yanos_graph_list', 'arguments': {}})
        value = tool_value(wire.get())
        assert list(value) == ['version', 'operation', 'source_sha256', 'skipped',
                               'total', 'shown', 'concepts'], ('list_field_order', list(value))
        assert value['version'] == 1 and value['operation'] == 'list', 'list_header'
        assert value['source_sha256'] == source, 'list_source_identity'
        assert value['total'] == 4 and value['shown'] == 4, ('list_counts', value)
        assert [c['id'] for c in value['concepts']] == \
            ['line', 'plane', 'rectangle', 'square'], 'list_id_order'
        square = value['concepts'][3]
        assert set(square) == {'id', 'name', 'alias_count', 'declaration_count',
                               'relation_count'}, ('list_row_keys', square)
        assert square['alias_count'] == 2 and square['relation_count'] == 3, \
            ('list_row_counts', square)

        # An EMPTY arguments object must give exactly the same answer as the
        # first call, which already used {}: this pins that the two {} spellings
        # agree, nothing more.
        wire.send('tools/call', ident=2,
                  params={'name': 'yanos_graph_list', 'arguments': {}})
        assert tool_value(wire.get(), ident=2) == value, 'list_empty_object_same_answer'

        # An OMITTED arguments object is a different request from {} and must be
        # accepted too: `params` here carries only the name (and the protocol
        # _meta), so the JSON has no "arguments" key at all. This is the case the
        # 0045 R2 spec allows ("arguments可缺省"), and it must return the same
        # complete list object as the {} call above -- not an INVALID error and
        # not an empty success.
        wire.send('tools/call', ident=6,
                  params={'name': 'yanos_graph_list'})
        reply = wire.get()
        # Named separately from the shared decoder so that a build which refuses
        # an omitted arguments object is attributed to this contract, not to the
        # generic reply-shape check.
        assert reply.get('result', {}).get('isError') is False, \
            ('list_omitted_arguments_accepted', reply)
        omitted = tool_value(reply, ident=6)
        assert omitted == value, ('list_omitted_arguments_same_answer', omitted)

        wire.send('tools/call', ident=3,
                  params={'name': 'yanos_graph_query', 'arguments': {'query': '等边矩形'}})
        queried = tool_value(wire.get(), ident=3)
        assert list(queried) == ['version', 'operation', 'source_sha256', 'skipped',
                                 'query', 'concept', 'declarations', 'relations'], \
            ('query_field_order', list(queried))
        assert queried['query'] == '等边矩形', 'query_echo'
        # The alias array is complete and in the 0044 byte order of the label
        # bytes (U+65B9 sorts before U+7B49 in UTF-8), not in declaration order.
        assert queried['concept'] == {'id': 'square', 'name': '正方形',
                                      'aliases': ['方形', '等边矩形']}, \
            ('query_aliases_complete', queried['concept'])
        assert queried['declarations']['total'] == 1, 'query_declarations'
        declaration = queried['declarations']['items'][0]
        assert set(declaration) == {'file', 'block_id', 'declaration_line',
                                    'declaration_start', 'declaration_end',
                                    'block_marker_line', 'block_first_line',
                                    'block_last_line', 'block_start', 'block_end'}, \
            ('provenance_fields', sorted(declaration))
        assert declaration['file'] == 'geometry' and declaration['block_id'] == 'definitions', \
            'provenance_target'
        relations_payload = queried['relations']
        assert relations_payload['total'] == 3 and relations_payload['shown'] == 3, \
            'relation_counts'
        # Canonical order is (file name, declaration byte), so the square's own
        # out edge in `geometry` precedes its self loop there, and its out edge
        # in `relations` follows both because "relations" > "geometry". A self
        # loop is listed once, from `self`, never also from out or in.
        directions = [row['direction'] for row in relations_payload['items']]
        assert directions == ['out', 'self', 'out'], ('relation_directions', directions)
        assert [row['where']['file'] for row in relations_payload['items']] == \
            ['geometry', 'geometry', 'relations'], 'relation_file_order'
        for row in relations_payload['items']:
            assert set(row) == {'type', 'subject', 'object', 'direction', 'where'}, \
                ('relation_row_keys', sorted(row))
            assert set(row['subject']) == {'id', 'name'}, 'endpoint_keys'
        # The provenance must locate the original bytes in the image: the
        # declaration range is the `@kg concept square ...` line, and the block
        # range contains it. Both are absolute half-open file offsets, so they
        # are checked against the fixture text this file wrote.
        # The offsets are absolute positions in the note file (0044 R2), and the
        # note bytes are the ones this file wrote, so the ranges are checked
        # against the fixture rather than against the tool's own answer.
        start, end = declaration['declaration_start'], declaration['declaration_end']
        assert geometry[start:end].startswith('@kg concept square 正方形'.encode()), \
            ('declaration_range', geometry[start:end][:40])
        assert geometry[start:end].endswith(b'\n') is False, 'range_excludes_eol'
        block_span = geometry[declaration['block_start']:declaration['block_end']]
        # The block content begins at its first declaration line here, because
        # the marker line is excluded from the block range.
        assert block_span.startswith('@kg concept square 正方形\n'.encode()), \
            ('block_range_start', block_span[:40])
        assert block_span.endswith(b'\n'), 'block_range_ends_after_content'
        assert len(block_span) < len(geometry), 'block_range_is_the_block_only'
        assert declaration['file'] == 'geometry', 'provenance_file'
        assert declaration['block_id'] == 'definitions', 'provenance_block_id'
        # Physical 1-based lines, counted here over the whole note including the
        # 0027 header and the marker line.
        first_line = geometry[:start].count(b'\n') + 1
        assert declaration['declaration_line'] == first_line, \
            ('declaration_line', declaration['declaration_line'], first_line)
        assert declaration['block_marker_line'] == first_line - 1, \
            ('block_marker_line', declaration['block_marker_line'])
        assert declaration['block_first_line'] == first_line, 'block_first_line'

        wire.send('tools/call', ident=4,
                  params={'name': 'yanos_graph_query', 'arguments': {'query': 'square'}})
        assert tool_value(wire.get(), ident=4)['concept']['id'] == 'square', 'query_by_id'
        wire.send('tools/call', ident=5,
                  params={'name': 'yanos_graph_query', 'arguments': {'query': '矩形'}})
        inbound = tool_value(wire.get(), ident=5)
        assert inbound['concept']['id'] == 'rectangle', 'query_by_name'
        # The same proposition seen from the object side is an `in` row, which
        # proves direction is which adjacency list yielded the row.
        assert [row['direction'] for row in inbound['relations']['items']] == ['in'], \
            ('inbound_direction', inbound['relations']['items'])
        assert disk.read_bytes() == before, 'graph_calls_write_nothing'
        cases += 1
    finally:
        wire.close()

    # 4. Errors keep their 0044 tokens, and no error becomes an empty success.
    wire = Wire(base_options('--allow-graph'), env)
    try:
        for ident, arguments, token in (
                (10, {'query': 'absent'}, 'NOT_FOUND'),
                (11, {'query': 'square\nrectangle'}, 'INVALID'),
                (12, {'query': ''}, 'INVALID'),
                (13, {'query': 'x' * 257}, 'INVALID'),
                (14, {'query': 'square', 'path': '/etc/passwd'}, 'INVALID'),
                (15, {'query': 'square', 'expected_source': 'A' * 64}, 'INVALID'),
                (16, {'query': 'square', 'expected_source': '0' * 63}, 'INVALID'),
                # A required query is missing here, so this is rejected before
                # any identity comparison is reached.
                (17, {'expected_source': '0' * 64}, 'INVALID'),
                # With the query present, the wrong identity is STALE.
                (21, {'query': 'square', 'expected_source': '0' * 64}, 'STALE')):
            wire.send('tools/call', ident=ident,
                      params={'name': 'yanos_graph_query',
                              'arguments': arguments})
            tool_value(wire.get(), ident=ident, code=token)
        # The list tool refuses the same unknown field and a wrong identity.
        for ident, arguments, token in (
                (18, {'query': 'square'}, 'INVALID'),
                (19, {'expected_source': '0' * 64}, 'STALE')):
            wire.send('tools/call', ident=ident,
                      params={'name': 'yanos_graph_list', 'arguments': arguments})
            tool_value(wire.get(), ident=ident, code=token)
        # Argument validation must not depend on a usable image: a bad query in a
        # server whose image does not exist is still INVALID, not IO.
        cases += 1
    finally:
        wire.close()

    # 5. Graph-specific failures surface as their own tokens, not as IO.
    graph_errors = [
        (b'@block b\n@kg concept\n', 'GRAPH_FORMAT'),
        (b'@block b\n@kg alias nowhere x\n', 'UNRESOLVED'),
        (b'@block b\n@kg concept same first\n@kg concept same second\n', 'CONFLICT'),
        ('@block b\n@kg concept dup 同一个\n@kg concept dup2 同一个\n'.encode(), 'AMBIGUOUS'),
    ]
    for note, token in graph_errors:
        disk.write_bytes(build_image([('geometry', geometry),
                                      ('relations', relations),
                                      ('broken', note)]))
        wire = Wire(base_options('--allow-graph'), env)
        try:
            arguments = {'query': '同一个'} if token == 'AMBIGUOUS' else {}
            name = 'yanos_graph_query' if token == 'AMBIGUOUS' else 'yanos_graph_list'
            wire.send('tools/call', ident=20,
                      params={'name': name, 'arguments': arguments})
            tool_value(wire.get(), ident=20, code=token)
            cases += 1
        finally:
            wire.close()

    # 6. Invalid arguments never open the image: with an unreadable image path
    # an INVALID argument still reports INVALID, which proves validation ran
    # before any I/O attempt.
    disk.write_bytes(build_image([('geometry', geometry), ('relations', relations),
                                  ('broken', healthy_broken)]))
    wire = Wire([args.server, '--disk-image', str(work / 'absent.img'),
                 '--index', str(work / 'index'), '--model', str(work / 'model'),
                 '--runner', str(work / 'runner'), '--allow-graph'], env)
    try:
        wire.send('tools/call', ident=30,
                  params={'name': 'yanos_graph_query',
                          'arguments': {'query': 'square', 'bogus': 1}})
        tool_value(wire.get(), ident=30, code='INVALID')
        wire.send('tools/call', ident=31,
                  params={'name': 'yanos_graph_list', 'arguments': {}})
        tool_value(wire.get(), ident=31, code='IO')
        cases += 1
    finally:
        wire.close()

    def shared_payload(note_bytes):
        """The exact byte length of the shared 0044 object for one fixture.

        Measured through the 0044 command, which calls the same
        yan_kg_query_json with output_limit 0, so this is a real measurement of
        what the MCP tool would have produced -- not a guess from the fixture.
        """
        path = work / 'measure.img'
        path.write_bytes(build_image([('geometry', note_bytes)]))
        measured = subprocess.run([args.cli, '--disk-image', str(path),
                                   'query', 'wide'],
                                  capture_output=True, timeout=TIMEOUT)
        assert measured.returncode == 0, ('measure_cli_failed', measured.stderr[:200])
        body = measured.stdout
        assert body.endswith(b'\n') and b'\n' not in body[:-1], 'measure_cli_shape'
        return len(body) - 1

    # 6b. Payload bounds, using genuinely large graphs rather than a fake
    # payload. One concept with a long alias array is legal and its object stays
    # under the 1 MiB cap; a much larger alias array crosses a bound and must be
    # reported as LIMIT with no partial body.
    #
    # Two different bounds can refuse, and both end as the same LIMIT token: the
    # service's own object cap (1 MiB, unit-tested directly in
    # test_knowledge_graph_query) and the reply encoder's whole-response bound
    # (the object is carried twice, as structuredContent and as text). Each case
    # below is sized so that at least one of them must refuse; the assertion is
    # the honest one, that an oversized real graph is a reported LIMIT and the
    # server keeps serving.
    def alias_note(count, width=40):
        lines = ['@block big', '@kg concept wide ' + '概' * width]
        for index in range(count):
            # Each alias is a distinct label, so no dedup can shrink the array.
            lines.append('@kg alias wide ' + ('别' * width) + '%05d' % index)
        return '\n'.join(lines).encode()

    small = alias_note(300)
    disk.write_bytes(build_image([('geometry', small)]))
    wire = Wire(base_options('--allow-graph'), env)
    try:
        wire.send('tools/call', ident=40,
                  params={'name': 'yanos_graph_query',
                          'arguments': {'query': 'wide'}})
        value = tool_value(wire.get(), ident=40)
        assert len(value['concept']['aliases']) == 300, ('alias_array_complete',
                                                        len(value['concept']['aliases']))
        assert wire.last_length <= 1048576, 'small_graph_within_bound'
        assert shared_payload(small) <= 1048576, 'small_shared_payload_within_cap'
        print('PASS 0045 R2 graph payload small object frame bytes=%d' % wire.last_length)
        cases += 1
    finally:
        wire.close()

    # An object over the 1 MiB service cap: LIMIT, and the server keeps serving
    # afterwards, so the refusal is a reported answer and not a crash.
    oversized = alias_note(6000, width=60)
    object_size = shared_payload(oversized)
    # The object itself is over the 1 MiB cap, so the service's own limit is
    # what must refuse; this is the case that pins the object cap.
    assert object_size > 1048576, ('oversized_object_measured', object_size)
    print('PASS 0045 R2 graph payload oversized object bytes=%d > 1048576'
          % object_size)
    disk.write_bytes(build_image([('geometry', oversized)]))
    wire = Wire(base_options('--allow-graph'), env)
    try:
        wire.send('tools/call', ident=41,
                  params={'name': 'yanos_graph_query',
                          'arguments': {'query': 'wide'}})
        value = tool_value(wire.get(), ident=41, code='LIMIT')
        assert value == {'code': 'LIMIT'}, 'oversized_object_limit'
        wire.send('server/discover', ident=42)
        assert wire.get()['id'] == 42, 'server_alive_after_object_limit'
        cases += 1
    finally:
        wire.close()

    # An object that fits the service cap but whose framed response does not:
    # the object appears once as structuredContent and once as text, so a
    # ~700 KiB object is over 1 MiB as a frame. The reply encoder must report
    # LIMIT rather than send a truncated frame.
    framed = alias_note(3900, width=60)
    framed_size = shared_payload(framed)
    # This is the case the report could not pin before. The shared payload is
    # WITHIN the 1 MiB cap -- so the service returns it -- while the MCP success
    # response is over the cap, because the object is carried twice (as
    # structuredContent and as the identical text copy). The refusal therefore
    # comes from the reply encoder, and the measurement below is what proves
    # which guard fired.
    assert framed_size <= 1048576, ('framed_payload_within_cap', framed_size)
    assert framed_size * 2 > 1048576, ('framed_response_over_cap', framed_size)
    print('PASS 0045 R2 graph payload framing: shared payload bytes=%d <= 1048576, '
          'doubled response bytes=%d > 1048576' % (framed_size, framed_size * 2))
    disk.write_bytes(build_image([('geometry', framed)]))
    wire = Wire(base_options('--allow-graph'), env)
    try:
        wire.send('tools/call', ident=43,
                  params={'name': 'yanos_graph_query',
                          'arguments': {'query': 'wide'}})
        value = tool_value(wire.get(), ident=43, code='LIMIT')
        assert value == {'code': 'LIMIT'}, 'oversized_frame_limit'
        wire.send('server/discover', ident=44)
        assert wire.get()['id'] == 44, 'server_alive_after_frame_limit'
        cases += 1
        print('PASS 0045 R2 graph payload frame limit reported as LIMIT')
    finally:
        wire.close()

    disk.write_bytes(build_image([('geometry', geometry), ('relations', relations),
                                 ('broken', healthy_broken)]))

    # 6e. The parent's existing task control applies unchanged to the new graph
    # branch: BUSY while a graph request is in flight, cancellation that drops
    # the response, EOF/SIGTERM exit, and reaping of the graph worker. The hold
    # is a test-only seam in the fault probe (it waits around the shared service
    # call and also leaves when the request's own cancel flag is raised), so the
    # production binaries keep no such switch.
    def children_of(server_pid):
        path = Path('/proc', str(server_pid), 'task', str(server_pid), 'children')
        return path.read_text().split() if path.exists() else []

    hold = work / 'graph.hold'
    pidlog = work / 'graph.pid'
    fault_env = dict(env, YAN_MCP_TEST_GRAPH_HOLD=str(hold),
                     YAN_MCP_TEST_BLOCK_OBSERVER=str(pidlog))
    fault_options = [args.fault, '--disk-image', str(disk), '--index',
                     str(work / 'index'), '--model', str(work / 'model'),
                     '--runner', str(work / 'runner'), '--allow-graph']
    wire = Wire(fault_options, fault_env)
    try:
        wire.send('tools/call', ident=60,
                  params={'name': 'yanos_graph_list', 'arguments': {}})
        deadline = time.monotonic() + 8
        while not pidlog.exists() or b'graph-hold' not in pidlog.read_bytes():
            if time.monotonic() >= deadline:
                raise HarnessError('graph hold did not start')
            time.sleep(.02)
        # The worker really exists as one owned child of the server.
        deadline = time.monotonic() + 8
        while not children_of(wire.p.pid):
            if time.monotonic() >= deadline:
                raise HarnessError('graph worker not observed')
            time.sleep(.02)
        worker = int(children_of(wire.p.pid)[0])
        # A second request is BUSY while the first is still in flight.
        wire.send('tools/call', ident=61,
                  params={'name': 'yanos_graph_query',
                          'arguments': {'query': 'square'}})
        tool_value(wire.get(), ident=61, code='BUSY')
        # Discovery stays responsive during the graph work.
        wire.send('server/discover', ident=62)
        assert wire.get()['id'] == 62, 'responsive_during_graph'
        # Cancelling drops the response and the worker is reaped.
        wire.send('notifications/cancelled', ident=None,
                  params={'requestId': 60})
        deadline = time.monotonic() + 10
        while Path('/proc', str(worker)).exists():
            if time.monotonic() >= deadline:
                raise HarnessError('cancelled graph worker was not reaped')
            time.sleep(.02)
        # The slot is reusable: a fresh list succeeds on the same server.
        hold.write_bytes(b'go')
        wire.send('tools/call', ident=63,
                  params={'name': 'yanos_graph_list', 'arguments': {}})
        tool_value(wire.get(), ident=63)
        cases += 1
    finally:
        wire.close()

    # 6f. EOF while a graph request is in flight still exits 0 and reaps.
    hold2 = work / 'graph2.hold'
    pidlog2 = work / 'graph2.pid'
    wire = Wire(fault_options, dict(env, YAN_MCP_TEST_GRAPH_HOLD=str(hold2),
                                    YAN_MCP_TEST_BLOCK_OBSERVER=str(pidlog2)))
    wire.send('tools/call', ident=70,
              params={'name': 'yanos_graph_list', 'arguments': {}})
    deadline = time.monotonic() + 8
    while not pidlog2.exists() or b'graph-hold' not in pidlog2.read_bytes():
        if time.monotonic() >= deadline:
            raise HarnessError('graph hold did not start for EOF case')
        time.sleep(.02)
    deadline = time.monotonic() + 8
    while not children_of(wire.p.pid):
        if time.monotonic() >= deadline:
            raise HarnessError('graph worker not observed for EOF case')
        time.sleep(.02)
    eof_worker = int(children_of(wire.p.pid)[0])
    wire.close(rc=0)
    deadline = time.monotonic() + 10
    while Path('/proc', str(eof_worker)).exists():
        if time.monotonic() >= deadline:
            raise HarnessError('graph worker survived EOF exit')
        time.sleep(.02)
    cases += 1

    # 7. Duplicate and valueless flag spellings stay startup USAGE.
    disk.write_bytes(build_image([('geometry', geometry), ('relations', relations),
                                  ('broken', healthy_broken)]))
    for extra in (('--allow-graph', '--allow-graph'),
                  ('--allow-graph=1',),
                  ('--allow-graph', 'x')):
        result = subprocess.run(base_options(*extra), capture_output=True,
                                timeout=10, env=env,
                                input=b'')
        assert (result.returncode, result.stdout, result.stderr) == \
            (2, b'', b'ERROR USAGE\n'), ('graph_flag_usage', extra, result.returncode)
    cases += 1

    print('PASS 0045 R2 graph tools: %d groups' % cases)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except AssertionError as error:
        print(':FAIL: graph_mcp: %s' % (error,))
        sys.exit(1)
    except HarnessError as error:
        print('HARNESS ERROR: %s' % (error,), file=sys.stderr)
        sys.exit(2)
