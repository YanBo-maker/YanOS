"""0031 wire/process oracle: real stdio server and real C retrieval with a local fixture."""
import hashlib
import fcntl
import json
import os
from pathlib import Path
import select
import signal
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from semantic_fixture import image, note
from semantic_json_checks import unique_object, logical_hash

META = {'io.modelcontextprotocol/protocolVersion': '2026-07-28',
        'io.modelcontextprotocol/clientCapabilities': {}}
# SHA-256 of the exact 0033 six-tool JSON array, captured from the pre-0035
# tools_json_parts join. R1-AC1 compares the wire bytes against this value.
TOOLS_0033_SHA256 = '3bbd05c5c7e5adfef17122dbf46a10b2704ee960c8cddf6a163a176c72bab4a2'


def json_member(raw, key):
    marker = b'"' + key.encode() + b'":'
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
    raise AssertionError(('unterminated_json_member', key))


class Wire:
    def __init__(self, args, env, **spawn_options):
        self.p = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, env=env, **spawn_options)
        self.buffer = b''

    def raw(self, data):
        self.p.stdin.write(data)
        self.p.stdin.flush()

    def send(self, method, ident=1, params=None, modern=True):
        obj = {'jsonrpc': '2.0', 'method': method}
        if ident is not None:
            obj['id'] = ident
        if params is not None or modern:
            obj['params'] = dict(params or {})
            if modern:
                obj['params']['_meta'] = dict(META)
        self.raw(json.dumps(obj, ensure_ascii=False, separators=(',', ':')).encode() + b'\n')

    def read_line(self, timeout=15):
        deadline = time.monotonic() + timeout
        while b'\n' not in self.buffer:
            left = deadline - time.monotonic()
            if left <= 0 or not select.select([self.p.stdout], [], [], max(0, left))[0]:
                raise RuntimeError('harness timeout waiting for MCP frame')
            data = os.read(self.p.stdout.fileno(), 65536)
            if not data:
                raise RuntimeError(('server exited before frame', self.p.poll(), self.p.stderr.read()))
            self.buffer += data
        line, self.buffer = self.buffer.split(b'\n', 1)
        self.last_length = len(line)
        assert len(line) <= 1024 * 1024, 'output_bound'
        return line

    def get(self, timeout=15):
        line = self.read_line(timeout)
        obj = json.loads(line, object_pairs_hook=unique_object,
                         parse_constant=lambda x: (_ for _ in ()).throw(AssertionError(x)))
        assert obj['jsonrpc'] == '2.0', 'jsonrpc_response'
        return obj

    def close(self, rc=0, allow_extra=False):
        if not self.p.stdin.closed:
            self.p.stdin.close()
        try:
            self.p.wait(timeout=12)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait()
            raise RuntimeError('harness timeout closing MCP server')
        stdout, stderr = self.p.stdout.read(), self.p.stderr.read()
        self.p.stdout.close()
        self.p.stderr.close()
        if b'sanitizer' in stderr.lower() or b'runtime error:' in stderr.lower():
            raise RuntimeError(('harness sanitizer diagnostic', stderr))
        if rc is None:
            if self.p.returncode not in (0, 5):
                raise RuntimeError(('harness unexpected server exit', self.p.returncode, stderr))
        else:
            assert self.p.returncode == rc, ('exit_code', rc, self.p.returncode, stderr)
        if not allow_extra:
            assert self.buffer + stdout == b'', ('unexpected_response', self.buffer + stdout)


def tool_value(reply, ident=1, code=None):
    assert reply['id'] == ident and type(reply['id']) is type(ident), 'typed_reply_id'
    result = reply['result']
    assert result['resultType'] == 'complete', 'modern_complete'
    assert 'io.modelcontextprotocol/serverInfo' in result['_meta'], 'server_identity'
    assert result['isError'] is (code is not None), 'execution_error_boundary'
    assert len(result['content']) == 1 and result['content'][0]['type'] == 'text', 'text_content'
    text = json.loads(result['content'][0]['text'], object_pairs_hook=unique_object)
    if code:
        assert text == {'code': code} and 'structuredContent' not in result, ('error_no_success', result)
    else:
        assert text == result['structuredContent'], 'same_text_and_structured'
    return text


def gone(pid):
    return not Path('/proc', str(pid)).exists()


def await_file(path):
    deadline = time.monotonic() + 8
    while not path.exists():
        if time.monotonic() >= deadline:
            raise RuntimeError('harness fixture did not start: ' + str(path))
        time.sleep(.02)
    return int(path.read_text())


def await_worker_reaped(wire, pidfile):
    await_file(pidfile)
    children = Path('/proc', str(wire.p.pid), 'task', str(wire.p.pid), 'children')
    deadline = time.monotonic() + 10
    while children.read_text().strip():
        if time.monotonic() >= deadline:
            raise RuntimeError('harness timeout waiting for worker reap')
        time.sleep(.01)


def main():
    server, semantic, runner = map(lambda p: Path(p).resolve(), sys.argv[1:])
    for path in (server, semantic, runner):
        if not path.is_file():
            raise RuntimeError('FAIL implementation/fixture missing: ' + str(path))
    cases = 0
    with tempfile.TemporaryDirectory(prefix='yan-mcp-') as tmp:
        root = Path(tmp)
        disk, index, model, childlog, pidfile = [root/n for n in ('source', 'index', 'model', 'children', 'runner.pid')]
        env = dict(os.environ, YAN_TEST_CHILD_LOG=str(childlog), YAN_TEST_SERVER_PID_FILE=str(pidfile))
        options = ['--disk-image', str(disk), '--index', str(index), '--model', str(model), '--runner', str(runner),
                   '--startup-timeout', '3', '--request-timeout', '3']
        args = [str(server), *options]
        for extra in (['--bad','x'], ['--model',str(model)], ['--startup-timeout','0'], ['--request-timeout','3601']):
            invalid=subprocess.run([*args,*extra],env=env,capture_output=True,timeout=3)
            assert (invalid.returncode,invalid.stdout,invalid.stderr)==(2,b'',b'ERROR USAGE\n'), 'startup_usage_no_protocol'
        assert not childlog.exists(), 'startup_usage_no_child'

        def rebuild(files):
            disk.write_bytes(image(files))
            model.write_text('universal')
            p = subprocess.run([str(semantic), 'rebuild', *options], env=env, capture_output=True, timeout=15)
            if p.returncode != 0:
                raise RuntimeError(('fixture rebuild failed', p.returncode, p.stderr))

        def snapshot():
            return tuple(hashlib.sha256(p.read_bytes()).digest() if p.exists() else None
                         for p in (disk, index, model, runner))

        # Discovery and argument rejection work without touching missing dependencies.
        wire = Wire(args, env)
        try:
            batch=[{'jsonrpc':'2.0','id':66,'method':'tools/list','params':{'_meta':META}},
                   {'jsonrpc':'2.0','id':66,'method':'initialize','params':{'protocolVersion':'2025-11-25',
                    'capabilities':{},'clientInfo':{'name':'oracle','version':'1'}}},
                   {'jsonrpc':'2.0','method':'notifications/initialized'},
                   {'jsonrpc':'2.0','id':67,'method':'tools/list'}]
            wire.raw(b''.join(json.dumps(item,separators=(',',':')).encode()+b'\n' for item in batch))
            assert wire.get()['id']==66, 'queued_control_first_response'
            duplicate=wire.get()
            assert duplicate['id']==66 and duplicate['error']['code']==-32600, 'queued_control_duplicate_id'
            assert wire.get()['error']['code']==-32602, 'duplicate_initialize_cannot_change_session'
            cases+=1
            wire.send('server/discover')
            r = wire.get()['result']
            assert r['supportedVersions'] == ['2026-07-28', '2025-11-25'], 'discover_versions'
            assert r['ttlMs'] == 0 and r['cacheScope'] == 'private', 'discovery_cache_fields'
            wire.send('tools/list')
            raw = wire.read_line()
            reply = json.loads(raw, object_pairs_hook=unique_object)
            assert reply['result']['_meta']['io.modelcontextprotocol/serverInfo']['version'] == '0035', 'server_version_0035'
            tools = reply['result']['tools']
            assert [t['name'] for t in tools] == ['yanos_search', 'yanos_index_status', 'yanos_block_list', 'yanos_block_read', 'yanos_block_refs', 'yanos_block_backlinks'], 'six_read_tools'
            assert hashlib.sha256(json_member(raw, 'tools')).hexdigest() == TOOLS_0033_SHA256, '0035_r1_ac1_exact_0033_tools'
            for t in tools:
                assert 'outputSchema' in t and t['inputSchema']['additionalProperties'] is False, 'schemas'
                assert t['annotations'] == dict(readOnlyHint=True, destructiveHint=False,
                                                idempotentHint=True, openWorldHint=False), 'read_annotations'
            wire.send('tools/call', params={'name': 'yanos_search', 'arguments': {'query': 'cpu', 'path': '/'}})
            tool_value(wire.get(), code='INVALID')
            wire.send('tools/call', params={'name': 'yanos_index_status', 'arguments': None})
            assert wire.get()['error']['code'] == -32602, 'null_arguments_structure'
            for badmeta in (dict(META, **{'io.modelcontextprotocol/clientCapabilities': {'sampling': False}}),
                            dict(META, **{'io.modelcontextprotocol/clientInfo': {'name':'x', 'version':'1', 'title':7}})):
                obj={'jsonrpc':'2.0','id':9,'method':'server/discover','params':{'_meta':badmeta}}
                wire.raw(json.dumps(obj).encode()+b'\n')
                assert wire.get()['error']['code']==-32602, 'known_metadata_types'
            wire.send('initialize', modern=False, params={'protocolVersion':'2025-11-25', 'capabilities':{},
                'clientInfo':{'name':'x','version':'1'}, '_meta':7})
            assert wire.get()['error']['code']==-32602, 'legacy_metadata_object'
            wire.send('tools/call', ident=None, params={'name': 'yanos_index_status'})
            wire.send('server/discover', ident=8)
            assert wire.get()['id'] == 8, 'notification_no_reply'
            assert not childlog.exists(), 'discovery_invalid_zero_child'
            cases += 1

            # Complete-line recovery; the tail of a rejected frame cannot execute.
            wire.raw(b'X'*16385 + b'\n')
            r = wire.get()
            assert r['error']['code'] == -32700 and 'id' not in r, 'oversize_no_id'
            wire.raw(b'{"jsonrpc":"2.0","id":2,"id":3,"method":"ping"}\n')
            assert wire.get()['error']['code'] == -32700, 'duplicate_wire_keys'
            wire.raw(b'{"query":"\xff"}\n')
            assert wire.get()['error']['code'] == -32700, 'strict_utf8'
            wire.send('tools/list', ident='fragment', params={'cursor': ''})
            assert wire.get()['id'] == 'fragment', 'recovery_after_bad_frame'
            frame = {'jsonrpc': '2.0', 'id': 'split', 'method': 'server/discover',
                     'params': {'_meta': dict(META, vendor='中')}}
            encoded = json.dumps(frame, ensure_ascii=False).encode() + b'\r\n'
            split = encoded.index('中'.encode()) + 1
            wire.raw(encoded[:split]); wire.raw(encoded[split:])
            assert wire.get()['id'] == 'split', 'fragmented_utf8_crlf'
            frame['id'] = 'exact'
            frame['params']['_meta']['vendor'] = ''
            encoded = json.dumps(frame, separators=(',', ':')).encode()
            frame['params']['_meta']['vendor'] = 'x' * (16384 - len(encoded))
            encoded = json.dumps(frame, separators=(',', ':')).encode()
            assert len(encoded) == 16384, 'exact_boundary_fixture'
            wire.raw(encoded + b'\r\n')
            assert wire.get()['id'] == 'exact', 'exact_16k_crlf'
            wire.raw(encoded + b' \n')
            assert wire.get()['error']['code'] == -32700, 'over_16k_rejected'
            cases += 1
        finally:
            wire.close()

        files = [('z.txt', note('中', ('原文😀\t"\\\r\n'*90))), None,
                 ('a.txt', b'cpu interrupt\r\nsecond\tline'), ('binary', b'\0\xff'),
                 ('blocks.txt', b'@block first\r\nhello\r\n@block empty\n@block last\nend\n'),
                 ('refs.txt', b'@block src\n[[blocks.txt#first]] [[gone#x]]\n'),
                 ('long.txt', b'@block long\n' + b'x' * 70000),
                 ('utf8.txt', b'@block u\n\xe4\xb8\xad\xf0\x9f\x98\x80\r\nend\n'),
                 ('escape.txt', b'@block esc\n' + (b'"\\\t' * 21846)[:65536])]
        rebuild(files)
        before = snapshot()
        wire = Wire(args, dict(env, YAN_MCP_FD_CHECK='1'))
        try:
            # R1-AC2: without --allow-write the write tool name is unknown.
            replies = []
            for tool in ('yanos_note_create', 'yanos_not_a_tool'):
                request = {'jsonrpc': '2.0', 'id': 90, 'method': 'tools/call',
                           'params': {'_meta': META, 'name': tool,
                                      'arguments': {'name': 'a.txt', 'text': 'x',
                                                    'expected_source': '0' * 64}}}
                wire.raw(json.dumps(request, separators=(',', ':')).encode() + b'\n')
                replies.append(wire.read_line())
            assert replies[0] == replies[1], '0035_r1_ac2_write_tool_unknown'
            assert snapshot() == before, '0035_r1_ac2_image_unchanged'
            digest = logical_hash(files)
            wire.send('tools/call', params={'name': 'yanos_block_list', 'arguments': {'name': 'blocks.txt'}})
            value = tool_value(wire.get())
            assert value['schema_version'] == 1 and value['operation'] == 'block_list', 'block_list_shape'
            assert value['source_sha256'] == digest and value['name'] == 'blocks.txt', 'block_list_identity'
            assert value['total'] == 3 and value['shown'] == 3, 'block_list_counts'
            assert [b['id'] for b in value['blocks']] == ['first', 'empty', 'last'], 'block_list_order'
            assert value['blocks'][1]['first_line'] == 0 and value['blocks'][1]['last_line'] == 0, 'block_empty_lines'
            assert value['blocks'][0]['start'] < value['blocks'][0]['end'], 'block_list_range'

            wire.send('tools/call', params={'name': 'yanos_block_read',
                                            'arguments': {'name': 'blocks.txt', 'block_id': 'first'}})
            value = tool_value(wire.get())
            assert value['operation'] == 'block_read' and value['text'] == 'hello\r\n', 'block_read_text'
            assert value['complete'] is True and value['next_offset'] is None, 'block_read_complete'
            assert value['block'] == {'start': value['page']['start'], 'end': value['page']['end'],
                                      'first_line': value['page']['first_line'],
                                      'last_line': value['page']['last_line']}, 'block_read_full_range'

            wire.send('tools/call', params={'name': 'yanos_block_read',
                                            'arguments': {'name': 'blocks.txt', 'block_id': 'empty'}})
            value = tool_value(wire.get())
            assert value['text'] == '' and value['complete'] is True and value['next_offset'] is None, 'block_read_empty'

            wire.send('tools/call', params={'name': 'yanos_block_read',
                                            'arguments': {'name': 'utf8.txt', 'block_id': 'u'}})
            value = tool_value(wire.get())
            assert value['text'] == '中😀\r\nend\n', 'block_read_utf8_crlf'
            assert value['page']['first_line'] == 2 and value['page']['last_line'] == 3, 'block_read_utf8_lines'

            wire.send('tools/call', params={'name': 'yanos_block_read',
                                            'arguments': {'name': 'escape.txt', 'block_id': 'esc'}})
            value = tool_value(wire.get())
            assert value['text'] == (b'"\\\t' * 21846)[:65536].decode(), 'block_read_escaped_text'
            assert value['complete'] is True and value['next_offset'] is None, 'block_read_escaped_complete'
            assert wire.last_length <= 1024 * 1024, 'spec6_escaped_64k_within_1mib'
            print('PASS spec6 escaped 64KiB block_read response bytes=' + str(wire.last_length))

            for tool, arguments in (
                    ('yanos_block_list', {'name': 'blocks.txt', '_meta': {}}),
                    ('yanos_block_read', {'name': 'blocks.txt', 'block_id': 'first', '_meta': {}}),
                    ('yanos_block_refs', {'name': 'refs.txt', '_meta': {}}),
                    ('yanos_block_backlinks', {'name': 'blocks.txt', 'block_id': 'first', '_meta': {}})):
                wire.send('tools/call', params={'name': tool, 'arguments': arguments})
                tool_value(wire.get(), code='INVALID')

            wire.send('tools/call', params={'name': 'yanos_block_refs', 'arguments': {'name': 'refs.txt'}})
            value = tool_value(wire.get())
            assert value['total'] == 2 and value['shown'] == 2, 'block_refs_counts'
            assert value['refs'][0]['target'] == {'name': 'blocks.txt', 'id': 'first'}, 'block_refs_target'
            assert value['refs'][0]['state'] == 'OK' and value['refs'][1]['state'] == 'DANGLING_FILE', 'block_refs_states'

            wire.send('tools/call', params={'name': 'yanos_block_backlinks',
                                            'arguments': {'name': 'blocks.txt', 'block_id': 'first'}})
            value = tool_value(wire.get())
            assert value['total'] == 1 and value['shown'] == 1 and value['skipped'] == 1, 'block_backlinks_counts'
            assert value['backlinks'][0]['source'] == 'refs.txt', 'block_backlinks_source'
            assert value['target_state'] == 'OK', 'block_backlinks_target'

            wire.send('tools/call', params={'name': 'yanos_block_list',
                                            'arguments': {'name': 'blocks.txt', 'expected_source': digest}})
            assert tool_value(wire.get())['source_sha256'] == digest, 'block_expected_success'
            wire.send('tools/call', params={'name': 'yanos_block_list',
                                            'arguments': {'name': 'blocks.txt', 'expected_source': '0' * 64}})
            tool_value(wire.get(), code='STALE')

            offset = 0
            joined = ''
            while True:
                arguments = {'name': 'long.txt', 'block_id': 'long', 'offset': offset}
                if offset > 0:
                    arguments['expected_source'] = digest
                wire.send('tools/call', params={'name': 'yanos_block_read', 'arguments': arguments})
                value = tool_value(wire.get())
                joined += value['text']
                if value['complete']:
                    assert value['next_offset'] is None, 'block_pages_terminal'
                    break
                assert value['next_offset'] > offset, 'block_pages_advance'
                offset = value['next_offset']
            assert joined == 'x' * 70000, 'block_pages_join'

            wire.send('tools/call', params={'name': 'yanos_block_read',
                                            'arguments': {'name': 'blocks.txt', 'block_id': 'missing'}})
            tool_value(wire.get(), code='NOT_FOUND')
            wire.send('tools/call', params={'name': 'yanos_block_list', 'arguments': {'name': 'missing'}})
            tool_value(wire.get(), code='NOT_FOUND')
            wire.send('tools/call', params={'name': 'yanos_block_read',
                                            'arguments': {'name': 'blocks.txt', 'block_id': 'first', 'offset': 1}})
            tool_value(wire.get(), code='INVALID')
            wire.send('tools/call', params={'name': 'yanos_block_read',
                                            'arguments': {'name': 'blocks.txt', 'block_id': 'first', 'offset': 4294967296}})
            tool_value(wire.get(), code='INVALID')
            wire.send('tools/call', params={'name': 'yanos_block_list',
                                            'arguments': {'name': 'blocks.txt', 'expected_source': 'A' * 64}})
            tool_value(wire.get(), code='INVALID')
            wire.send('tools/call', params={'name': 'yanos_block_list',
                                            'arguments': {'name': 'blocks.txt', 'extra': 1}})
            tool_value(wire.get(), code='INVALID')
            cases += 1

            wire.send('tools/call', params={'name': 'yanos_search', 'arguments': {'query': 'cpu'}})
            value = tool_value(wire.get())
            p = subprocess.run([str(semantic), 'search', *options, '--query', 'cpu', '--format', 'json'],
                               env=env, capture_output=True, timeout=15)
            assert p.returncode == 0, ('cli_oracle', p.stderr)
            assert value == json.loads(p.stdout), 'complete_0030_context'
            assert value['source_sha256'] == logical_hash(files), 'logical_source_identity'
            originals = dict(item for item in files if item)
            for row in value['results']:
                raw = originals[row['file']]
                assert row['text'].encode() == raw[row['start']:row['end']], 'raw_physical_evidence'
                assert row['first_line'] == raw[:row['start']].count(b'\n')+1, 'physical_first_line'
            assert value['shown'] > 0, 'search_not_fake_empty'
            old_log=childlog.read_text().splitlines()
            wire.send('tools/call', params={'name': 'yanos_index_status'})
            assert tool_value(wire.get()) == dict(state='READY', segments=value['segments'], skipped=1), 'ready_status'
            assert 'server' not in childlog.read_text().splitlines()[len(old_log):], 'status_no_embedding_start'
            assert before == snapshot(), 'read_only_tool_calls'
            model.write_text('universal\n')
            wire.send('tools/call', params={'name':'yanos_index_status'})
            assert tool_value(wire.get())=={'state':'STALE'}, 'same_process_model_identity_rechecked'
            model.write_text('universal')
            cases += 1

            # Same process sees replacements; no cached startup identity or implicit rebuild.
            disk.write_bytes(image([('a.txt', b'changed')]))
            wire.send('tools/call', params={'name': 'yanos_index_status', 'arguments': {}})
            assert tool_value(wire.get()) == {'state': 'STALE'}, 'status_observes_stale'
            wire.send('tools/call', params={'name': 'yanos_search', 'arguments': {'query': 'cpu'}})
            tool_value(wire.get(), code='STALE')
            rebuild([('a.txt', b'changed')])
            wire.send('tools/call', params={'name': 'yanos_search', 'arguments': {'query': 'new'}})
            assert tool_value(wire.get())['results'][0]['text'] == 'changed', 'rebind_after_external_rebuild'
            good_index = index.read_bytes()
            index.write_bytes(b'invalid')
            wire.send('tools/call', params={'name': 'yanos_index_status'})
            assert tool_value(wire.get()) == {'state': 'CORRUPT'}, 'corrupt_success_observation'
            index.unlink()
            wire.send('tools/call', params={'name': 'yanos_index_status'})
            assert tool_value(wire.get()) == {'state': 'MISSING'}, 'missing_success_observation'
            index.write_bytes(good_index)
            rebuild([])
            wire.send('tools/call', params={'name':'yanos_search','arguments':{'query':'cpu'}})
            empty=tool_value(wire.get())
            assert empty['shown']==0 and empty['results']==[] and empty['segments']==0, 'genuine_empty_success'
            rebuild([('a.txt',b'changed')])
            cases += 1
        finally:
            wire.close()

        # Block tools need only the image: all other startup dependencies may be absent.
        nodep_disk = root/'nodep-disk'
        nodep_disk.write_bytes(image([('blocks.txt', b'@block first\r\nhello\r\n@block empty\n@block last\nend\n')]))
        block_args = [str(server), '--disk-image', str(nodep_disk), '--index', str(root/'missing-index'),
                      '--model', str(root/'missing-model'), '--runner', str(root/'missing-runner'),
                      '--startup-timeout', '3', '--request-timeout', '3']
        wire = Wire(block_args, env)
        try:
            wire.send('tools/call', params={'name': 'yanos_block_list', 'arguments': {'name': 'blocks.txt'}})
            assert tool_value(wire.get())['total'] == 3, 'block_without_dependencies'
            wire.send('tools/call', params={'name': 'yanos_search', 'arguments': {'query': 'cpu'}})
            tool_value(wire.get(), code='DEPENDENCY')
            cases += 1
        finally:
            wire.close()

        bad_disk = root/'bad-disk'
        bad_disk.write_bytes(image([('bad', b'@yan-note 9\n@end\n'),
                                    ('binary', b'@block a\n\0')]))
        bad_args = [str(server), '--disk-image', str(bad_disk), '--index', str(root/'missing-index'),
                    '--model', str(root/'missing-model'), '--runner', str(root/'missing-runner'),
                    '--startup-timeout', '3', '--request-timeout', '3']
        wire = Wire(bad_args, env)
        try:
            wire.send('tools/call', params={'name': 'yanos_block_list', 'arguments': {'name': 'bad'}})
            tool_value(wire.get(), code='METADATA')
            wire.send('tools/call', params={'name': 'yanos_block_list', 'arguments': {'name': 'binary'}})
            tool_value(wire.get(), code='INVALID_TEXT')
            wire.send('tools/call', params={'name': 'yanos_block_list', 'arguments': {'name': 'missing'}})
            tool_value(wire.get(), code='NOT_FOUND')
            cases += 1
        finally:
            wire.close()

        unsupported = bytearray(image([('note', b'@block a\nx')]))
        struct.pack_into('<I', unsupported, 8, 2)
        struct.pack_into('<I', unsupported, 60, zlib.crc32(unsupported[:4096]))
        unsupported_disk = root/'unsupported.img'
        unsupported_disk.write_bytes(unsupported)
        unsupported_args = [str(server), '--disk-image', str(unsupported_disk),
                            '--index', str(root/'missing-index'), '--model', str(root/'missing-model'),
                            '--runner', str(root/'missing-runner'), '--startup-timeout', '3',
                            '--request-timeout', '3']
        wire = Wire(unsupported_args, env)
        try:
            wire.send('tools/call', params={'name': 'yanos_block_list', 'arguments': {'name': 'note'}})
            tool_value(wire.get(), code='UNSUPPORTED')
            cases += 1
        finally:
            wire.close()

        # A legacy client has its own handshake; modern requests never borrow it.
        wire = Wire(args, env)
        try:
            wire.send('initialize', modern=False, params={'protocolVersion': '2025-11-25',
                      'capabilities': {}, 'clientInfo': {'name': 'oracle', 'version': '1'}})
            r = wire.get()['result']
            assert r['protocolVersion'] == '2025-11-25' and 'resultType' not in r, 'legacy_handshake'
            wire.send('notifications/initialized', ident=None, modern=False)
            wire.send('ping', modern=False)
            assert wire.get()['result'] == {}, 'legacy_ping'
            wire.send('tools/call', modern=False, params={'name': 'yanos_index_status'})
            r = wire.get()['result']
            assert 'resultType' not in r and r['structuredContent']['state'] == 'READY', 'legacy_tool'
            wire.send('ping')
            assert wire.get()['error']['code'] == -32601, 'modern_no_ping'
            cases += 1
        finally:
            wire.close()

        # Cancellation responsiveness, typed IDs, BUSY, and owned descendant reaping.
        pidfile.unlink(missing_ok=True)
        wire = Wire(args, dict(env, YAN_TEST_EMBED_DELAY='2'))
        sentinel = subprocess.Popen(['sleep', '30'])
        try:
            wire.send('tools/call', ident=7, params={'name': 'yanos_search', 'arguments': {'query': 'cpu'}})
            runner_pid = await_file(pidfile)
            wire.send('tools/list', ident=7)
            duplicate=wire.get(timeout=2)
            assert duplicate['error']['code']==-32600 and duplicate['id']==7, 'duplicate_active_id_rejected'
            wire.send('notifications/cancelled', ident=None, modern=False, params={'requestId': '7'})
            wire.send('tools/call', ident=8, params={'name': 'yanos_index_status'})
            tool_value(wire.get(timeout=2), ident=8, code='BUSY')
            wire.send('server/discover', ident=9)
            assert wire.get(timeout=2)['id'] == 9, 'responsive_during_embedding'
            wire.send('notifications/cancelled', ident=None, modern=False, params={'requestId': 7})
            deadline = time.monotonic()+10
            while not gone(runner_pid):
                if time.monotonic() >= deadline:
                    raise RuntimeError('harness timeout reaping cancelled runner')
                time.sleep(.02)
            assert sentinel.poll() is None, 'unrelated_process_untouched'
            # Poll the task slot through normal discovery before another status.
            time.sleep(.15)
            wire.send('tools/call', ident=10, params={'name': 'yanos_index_status'})
            assert tool_value(wire.get(), ident=10)['state'] == 'READY', 'reuse_after_cancel_cleanup'
            cases += 1
        finally:
            wire.close()
            sentinel.terminate(); sentinel.wait()

        for close_mode in ('eof', 'sigterm', 'sigint', 'worker_crash'):
            pidfile.unlink(missing_ok=True)
            wire = Wire(args, dict(env, YAN_TEST_EMBED_DELAY='2'))
            wire.send('tools/call', params={'name': 'yanos_search', 'arguments': {'query': 'cpu'}})
            runner_pid = await_file(pidfile)
            if close_mode == 'sigterm':
                wire.p.send_signal(signal.SIGTERM)
            elif close_mode == 'sigint':
                wire.p.send_signal(signal.SIGINT)
            elif close_mode == 'worker_crash':
                children = Path('/proc', str(wire.p.pid), 'task', str(wire.p.pid), 'children').read_text().split()
                assert len(children) == 1, 'one_owned_worker'
                os.kill(int(children[0]), signal.SIGKILL)
                tool_value(wire.get(), code='IO')
            wire.close(rc=143 if close_mode == 'sigterm' else 130 if close_mode=='sigint' else 0)
            assert gone(runner_pid), ('owned_descendant_reaped', close_mode, runner_pid)
            cases += 1

        # Normal cancellation's TERM grace must escalate for a stubborn runner.
        pidfile.unlink(missing_ok=True)
        wire = Wire(args, dict(env, YAN_TEST_EMBED_DELAY='2', YAN_TEST_IGNORE_TERM='1'))
        wire.send('tools/call', params={'name': 'yanos_search', 'arguments': {'query': 'cpu'}})
        runner_pid = await_file(pidfile)
        wire.send('notifications/cancelled', ident=None, modern=False, params={'requestId': 1})
        wire.close()
        assert gone(runner_pid), 'term_escalation_reaps_runner'
        cases += 1

        # A queued, still-unsent tool response may be dropped without corrupting
        # unrelated list responses already in the writer.
        wire = Wire(args, env)
        fcntl.fcntl(wire.p.stdout.fileno(), fcntl.F_SETPIPE_SZ, 4096)
        try:
            wire.send('tools/list', ident=2)
            wire.send('tools/list', ident=3)
            pidfile.unlink(missing_ok=True)
            wire.send('tools/call', ident=7, params={'name': 'yanos_search', 'arguments': {'query': 'cpu'}})
            await_worker_reaped(wire, pidfile)
            wire.send('tools/call', ident=8, params={'name':'yanos_index_status'})
            wire.send('notifications/cancelled', ident=None, modern=False, params={'requestId': 7})
            assert wire.get()['id'] == 2 and wire.get()['id'] == 3, 'other_queued_frames_preserved'
            tool_value(wire.get(),ident=8,code='BUSY')
            wire.send('tools/call', ident=10, params={'name': 'yanos_index_status'})
            assert tool_value(wire.get(), ident=10)['state'] == 'READY', 'unsent_cancel_releases_slot'
            cases += 1
        finally:
            wire.close()

        # After any part of the tool frame was sent, accepting cancel closes
        # transport. It cannot append a replacement response after a prefix.
        rebuild([(f'n{i:02d}', ('😀'*512).encode()) for i in range(25)])
        wire = Wire(args, env)
        fcntl.fcntl(wire.p.stdout.fileno(), fcntl.F_SETPIPE_SZ, 4096)
        pidfile.unlink(missing_ok=True)
        wire.send('tools/call', ident=7, params={'name': 'yanos_search', 'arguments': {'query': 'cpu'}})
        await_worker_reaped(wire, pidfile)
        assert select.select([wire.p.stdout], [], [], 2)[0], 'partial_frame_started'
        wire.send('notifications/cancelled', ident=None, modern=False, params={'requestId': 7})
        wire.p.wait(timeout=12)
        prefix = wire.p.stdout.read()
        assert wire.p.returncode == 5 and prefix.startswith(b'{') and b'\n' not in prefix, 'partial_cancel_transport_fatal'
        wire.p.stdin.close(); wire.p.stdout.close(); wire.p.stderr.close()
        cases += 1

        # Output backpressure has one monotonic deadline, independent of input.
        wire = Wire(args, env)
        fcntl.fcntl(wire.p.stdout.fileno(), fcntl.F_SETPIPE_SZ, 4096)
        for i in range(20):
            wire.send('tools/list', ident=i)
        started = time.monotonic()
        wire.p.wait(timeout=9)
        assert wire.p.returncode == 5 and time.monotonic()-started < 8, 'output_deadline'
        wire.p.stdin.close(); wire.p.stdout.close(); wire.p.stderr.close()
        cases += 1

        wire=Wire(args,env)
        fcntl.fcntl(wire.p.stdout.fileno(),fcntl.F_SETPIPE_SZ,4096)
        os.set_blocking(wire.p.stdin.fileno(),False)
        packet=b''.join(json.dumps({'jsonrpc':'2.0','id':i,'method':'tools/list',
            'params':{'_meta':META}},separators=(',',':')).encode()+b'\n' for i in range(2000))
        offset=0; deadline=time.monotonic()+4
        while offset<len(packet) and wire.p.poll() is None:
            if time.monotonic()>=deadline:
                raise RuntimeError('harness output queue did not reach capacity')
            if select.select([],[wire.p.stdin],[],.05)[1]:
                try:
                    offset+=os.write(wire.p.stdin.fileno(),packet[offset:offset+8192])
                except BrokenPipeError:
                    break
        wire.p.wait(timeout=5)
        assert wire.p.returncode==5 and time.monotonic()<deadline, 'bounded_output_queue_fatal'
        wire.p.stdin.close(); wire.p.stdout.close(); wire.p.stderr.close()
        cases+=1

        pidfile.unlink(missing_ok=True)
        wire = Wire(args, dict(env, YAN_TEST_EMBED_DELAY='2'))
        wire.send('tools/call', params={'name': 'yanos_search', 'arguments': {'query': 'cpu'}})
        runner_pid = await_file(pidfile)
        wire.p.stdout.close()
        wire.send('server/discover', ident=9)
        wire.p.wait(timeout=12)
        assert wire.p.returncode == 5 and gone(runner_pid), 'broken_stdout_cleanup'
        wire.p.stdin.close(); wire.p.stderr.close()
        cases += 1
        wire=Wire(args,dict(env,YAN_TEST_EMBED_FAILURE='1'))
        try:
            wire.send('tools/call',params={'name':'yanos_search','arguments':{'query':'cpu'}})
            tool_value(wire.get(),code='PROVIDER')
            wire.send('tools/call',params={'name':'yanos_index_status'})
            assert tool_value(wire.get())['state']=='READY', 'ordinary_failure_allows_next_task'
            cases+=1
        finally:
            wire.close()

        # Cancel during runner --version, before any embedding process exists.
        version_pid=root/'version.pid'
        wire=Wire(args,dict(env,YAN_TEST_VERSION_MODE='timeout',YAN_TEST_PID_FILE=str(version_pid)))
        wire.send('tools/call',ident=7,params={'name':'yanos_search','arguments':{'query':'cpu'}})
        pid=await_file(version_pid)
        wire.send('notifications/cancelled',ident=None,modern=False,params={'requestId':7})
        wire.close()
        assert gone(pid), 'version_process_cancelled_and_reaped'
        cases+=1
        print(f'PASS mcp wire/process oracle {cases} scenarios')


if __name__ == '__main__':
    main()
