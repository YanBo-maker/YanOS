#!/usr/bin/env python3
"""0045 R2 acceptance with the official MCP SDK: the two graph tools plus the
block read of the provenance they publish.

Contract: docs/specs/0045-graph-mcp-tool.md R2 AC -- "真实SDK调用两工具再读取出处块，
原字节/行号一致". The chain that matters is end to end and cannot be faked by
calling the C service directly:

    yanos_graph_query -> where{file, block_id, block_start/end, *_line}
    yanos_block_read  -> the same original bytes at the same physical lines

This file computes the expectation from the note bytes it wrote, so the block
text and line numbers are checked against the fixture, not against the graph
answer. Discovery also pins that the graph tools are appended after the six
default tools, and that the object carried in structuredContent equals the text
copy the SDK decodes separately.

A missing in-repo input, binary or the pinned SDK is a hard failure here: this
test exists only to exercise the real SDK path, and 0045 R2 requires that path.
It must never be reported as a pass or a skip when the implementation or the
SDK is absent.
"""
import asyncio
import json
import os
from pathlib import Path
import sys
import tempfile
from importlib.metadata import version, PackageNotFoundError

DEFAULT_TOOLS = ['yanos_search', 'yanos_index_status', 'yanos_block_list',
                 'yanos_block_read', 'yanos_block_refs', 'yanos_block_backlinks']
GRAPH_TOOLS = ['yanos_graph_list', 'yanos_graph_query']


def build_image(files, capacity=63):
    import struct
    import zlib
    block = 4096
    directory_blocks = (capacity + 1) // 64
    needed = directory_blocks + sum(
        max(1, (len(data) + block - 1) // block) if data else 0
        for _, data in files)
    blocks = max(needed, directory_blocks + 1)
    raw = bytearray(blocks * block)
    raw[0:8] = b"YANFS01\x00"
    struct.pack_into('<IIII', raw, 8, 1, block, blocks, capacity)
    lba = directory_blocks
    for slot, (name, data) in enumerate(files):
        encoded = name.encode('ascii')
        at = 64 + slot * 64
        raw[at:at + len(encoded)] = encoded
        count = (data and (len(data) + block - 1) // block) or 0
        struct.pack_into('<III', raw, at + 32, len(data), lba if count else 0, count)
        if data:
            raw[lba * block:lba * block + len(data)] = data
        lba += count
    crc = zlib.crc32(raw[:60] + b'\x00\x00\x00\x00' + raw[64:directory_blocks * block])
    struct.pack_into('<I', raw, 60, crc & 0xFFFFFFFF)
    return bytes(raw)


def value_of(result):
    data = result.model_dump(by_alias=True, exclude_none=True)
    assert data['isError'] is False, ('sdk_graph_error', data)
    payload = data['structuredContent']
    assert json.loads(data['content'][0]['text']) == payload, 'sdk_graph_text_equals_structured'
    return payload


NOTE = ('@yan-note 1\ntitle: 几何\n@end\n'
        '@block definitions\n'
        '@kg concept square 正方形\n'
        '@kg concept rectangle 矩形\n'
        '@kg alias square 等边矩形\n'
        '@kg relation is_a square rectangle\n')


async def run(server, options, root):
    from mcp import Client
    from mcp.client.stdio import StdioServerParameters

    # The chain is exercised in the modern protocol mode, where the graph
    # tools' structuredContent and text agreement is what a real client sees.
    target = StdioServerParameters(command=str(server), args=options)
    async with Client(target, mode='2026-07-28', read_timeout_seconds=20) as client:
        listing = await client.list_tools()
        names = [tool.name for tool in listing.tools]
        assert names == DEFAULT_TOOLS + GRAPH_TOOLS, ('sdk_graph_discovery', names)
        graph = {tool.name: tool for tool in listing.tools[6:]}
        for name in GRAPH_TOOLS:
            tool = graph[name]
            assert tool.input_schema['type'] == 'object', ('sdk_graph_input', name)
            assert tool.input_schema['additionalProperties'] is False, \
                ('sdk_graph_closed', name)
            assert tool.output_schema['type'] == 'object', ('sdk_graph_output', name)
            annotations = (tool.annotations.model_dump(by_alias=True, exclude_none=True)
                           if tool.annotations else {})
            assert annotations == {'readOnlyHint': True, 'destructiveHint': False,
                                   'idempotentHint': True, 'openWorldHint': False}, \
                ('sdk_graph_annotations', name)
        assert graph['yanos_graph_query'].input_schema['required'] == ['query'], \
            'sdk_graph_query_required'
        assert graph['yanos_graph_query'].input_schema['properties']['query'] == {
            'type': 'string', 'minLength': 1, 'maxLength': 256}, 'sdk_graph_query_bounds'
        assert set(graph['yanos_graph_list'].input_schema.get('properties', {})) <= {
            'expected_source'}, 'sdk_graph_list_arguments'

        listed = value_of(await client.call_tool('yanos_graph_list', {}))
        assert listed['operation'] == 'list' and listed['version'] == 1, 'sdk_graph_list_shape'
        concepts = {item['id']: item for item in listed['concepts']}
        assert set(concepts) == {'square', 'rectangle'}, ('sdk_graph_list_ids', list(concepts))
        source = listed['source_sha256']

        queried = value_of(await client.call_tool('yanos_graph_query',
                                                  {'query': '等边矩形',
                                                   'expected_source': source}))
        assert queried['concept']['id'] == 'square', 'sdk_graph_query_alias'
        assert queried['concept']['aliases'] == ['等边矩形'], 'sdk_graph_query_aliases'
        relation = queried['relations']['items'][0]
        assert relation['type'] == 'is_a' and relation['direction'] == 'out', 'sdk_graph_relation'
        where = relation['where']

        # The provenance must address the real text: read it back through the
        # block tool using the SAME source identity the graph just published.
        read = value_of(await client.call_tool('yanos_block_read',
                                               {'name': where['file'],
                                                'block_id': where['block_id'],
                                                'expected_source': source}))
        assert read['block']['start'] == where['block_start'], 'sdk_graph_block_start'
        assert read['block']['end'] == where['block_end'], 'sdk_graph_block_end'
        assert read['block']['first_line'] == where['block_first_line'], 'sdk_graph_first_line'
        assert read['block']['last_line'] == where['block_last_line'], 'sdk_graph_last_line'
        # NOTE is decoded text, so the slice is compared as UTF-8 bytes: the
        # block tool must return the original bytes, not a re-encoding of them.
        expected_text = NOTE.encode()[where['block_start']:where['block_end']]
        assert read['text'].encode() == expected_text, ('sdk_graph_block_text', read['text'])
        # And the declaration range points at the declaring line itself.
        declared = NOTE.encode()[where['declaration_start']:where['declaration_end']]
        assert declared.startswith('@kg relation is_a square rectangle'.encode()), \
            ('sdk_graph_declaration_range', declared)
        lines = NOTE.encode()[:where['declaration_start']].count(b'\n') + 1
        assert where['declaration_line'] == lines, ('sdk_graph_declaration_line', lines)
        print('PASS official mcp==2.3.0 graph tools: list+query+block_read '
              'file=%s block=%s lines=%d..%d bytes=%d' %
              (where['file'], where['block_id'], where['block_first_line'],
               where['block_last_line'], len(expected_text)))

        # An absent concept is a tool error with its own token, never an empty
        # success object.
        missing = (await client.call_tool('yanos_graph_query',
                                         {'query': 'absent'})).model_dump(
            by_alias=True, exclude_none=True)
        assert missing['isError'] is True, 'sdk_graph_missing_is_error'
        assert 'structuredContent' not in missing, 'sdk_graph_missing_no_body'
        assert json.loads(missing['content'][0]['text']) == {'code': 'NOT_FOUND'}, \
            'sdk_graph_missing_token'

        # A wrong identity is STALE, and the same identity the graph published
        # is accepted -- the two directions of the same check.
        stale = (await client.call_tool('yanos_graph_query',
                                       {'query': 'square',
                                        'expected_source': '0' * 64})).model_dump(
            by_alias=True, exclude_none=True)
        assert stale['isError'] is True and json.loads(stale['content'][0]['text']) == {
            'code': 'STALE'}, 'sdk_graph_stale'


def main():
    server, root_image = Path(sys.argv[1]).resolve(), sys.argv[2] if len(sys.argv) > 2 else None
    if not server.is_file():
        raise SystemExit('FAIL implementation missing: ' + str(server))
    try:
        found = version('mcp')
    except PackageNotFoundError:
        raise SystemExit('FAIL pinned official mcp==2.3.0 SDK is required for this test '
                         '(not a skip: 0045 R2 requires the real SDK path)')
    if found != '2.3.0':
        raise SystemExit('FAIL expected pinned mcp==2.3.0, found ' + found)
    with tempfile.TemporaryDirectory(prefix='yan-mcp-graph-sdk-') as tmp:
        root = Path(tmp)
        disk = root / 'source'
        disk.write_bytes(build_image([('geometry', NOTE.encode()),
                                      ('other', b'plain text\n')]))
        options = ['--disk-image', str(disk), '--index', str(root / 'index'),
                   '--model', str(root / 'model'), '--runner', str(root / 'runner'),
                   '--allow-graph']
        asyncio.run(run(server, options, root))
    return 0


if __name__ == '__main__':
    sys.exit(main())
