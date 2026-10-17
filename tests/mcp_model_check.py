"""Explicit production MCP smoke using existing real embedding assets, never CI."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from mcp_checks import Wire, tool_value
from semantic_fixture import image, QUALITY_FILES, QUALITY_QUERIES
from semantic_json_checks import logical_hash


def main():
    server, semantic, model, runner = map(lambda p: Path(p).resolve(), sys.argv[1:])
    for path in (server,semantic):
        if not path.is_file():
            raise RuntimeError('FAIL implementation missing: '+str(path))
    for path in (model,runner):
        if not path.is_file():
            print('SKIP external real-model asset missing: '+str(path)); return 77
    with tempfile.TemporaryDirectory(prefix='yan-mcp-model-') as raw:
        root=Path(raw); disk=root/'notes.img'; index=root/'notes.idx'
        disk.write_bytes(image(QUALITY_FILES))
        options=['--disk-image',str(disk),'--index',str(index),'--model',str(model),'--runner',str(runner)]
        p=subprocess.run([str(semantic),'rebuild',*options],capture_output=True,timeout=600)
        if p.returncode:
            raise RuntimeError(('real embedding rebuild failed',p.returncode,p.stderr))
        before=(disk.read_bytes(),index.read_bytes())
        expected_hash=logical_hash(QUALITY_FILES)
        originals=dict(QUALITY_FILES)
        rows=0
        wire=Wire([str(server),*options],{})
        try:
            wire.send('tools/call',params={'name':'yanos_index_status'})
            assert tool_value(wire.get(timeout=120))['state']=='READY', 'real_index_status'
            for query, relevant in (QUALITY_QUERIES[0],QUALITY_QUERIES[5]):
                wire.send('tools/call',params={'name':'yanos_search','arguments':{'query':query}})
                value=tool_value(wire.get(timeout=120))
                assert value['source_sha256']==expected_hash, 'real_source_identity'
                header=index.read_bytes()[:256]
                for key,off in [('model',64),('runner',96),('config',128)]:
                    assert value[key+'_sha256']==header[off:off+32].hex(), 'real_current_identity'
                assert value['shown']==20, 'real_top20_complete'
                assert set(relevant)&{r['file'] for r in value['results'][:5]}, 'fixed_query_top5_relevance'
                for row in value['results']:
                    original=originals[row['file']]
                    assert row['text'].encode()==original[row['start']:row['end']], 'real_original_bytes'
                    assert row['first_line']==original[:row['start']].count(b'\n')+1, 'real_physical_line'
                rows+=value['shown']
                print('PASS real MCP query',json.dumps(query,ensure_ascii=False),'shown='+str(value['shown']))
        finally:
            wire.close()
        assert before==(disk.read_bytes(),index.read_bytes()), 'real_read_only'
        print('PASS real production MCP 2 fixed queries/'+str(rows)+' original rows; no answer generation')
        print('server_sha256='+hashlib.sha256(server.read_bytes()).hexdigest())
        print('model_sha256='+header[64:96].hex())
        print('runner_sha256='+header[96:128].hex())
    return 0


if __name__=='__main__':
    try:
        raise SystemExit(main())
    except (RuntimeError,subprocess.TimeoutExpired,OSError) as exc:
        print('harness:',exc); raise SystemExit(1)
