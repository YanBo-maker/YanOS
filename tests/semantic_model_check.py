"""Explicit real-model acceptance; never substitutes fixture vectors for model quality."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
from semantic_fixture import QUALITY_FILES, QUALITY_QUERIES, image

def digest(path):
    h=hashlib.sha256()
    with path.open('rb') as source:
        while block:=source.read(1024*1024): h.update(block)
    return h.hexdigest()
def main():
    parser=argparse.ArgumentParser()
    for name in ('tool','model','runner','work'): parser.add_argument('--'+name,required=True,type=Path)
    args=parser.parse_args()
    if not args.tool.is_file(): raise RuntimeError('FAIL implementation under test missing: '+str(args.tool))
    for dependency in (args.model,args.runner,Path('/usr/bin/time')):
        if not dependency.is_file():
            print('SKIP external dependency missing: '+str(dependency)); return 77
    args.work.mkdir(parents=True,exist_ok=True)
    disk=args.work/'notes.img'; index=args.work/'notes.index'
    disk.write_bytes(image(QUALITY_FILES))
    before=digest(disk); model_hash=digest(args.model); runner_hash=digest(args.runner)
    fixture_hash=hashlib.sha256(repr((QUALITY_FILES,QUALITY_QUERIES)).encode()).hexdigest()
    common=['--disk-image',str(disk),'--index',str(index),'--model',str(args.model),'--runner',str(args.runner)]
    timings=[]
    def run(label,command,*extra):
        memory=args.work/(label+'.resource')
        start=time.monotonic()
        process=subprocess.run(['/usr/bin/time','-f','%M','-o',str(memory),str(args.tool),command,*common,*extra],
                               capture_output=True,text=True,timeout=300)
        elapsed=time.monotonic()-start
        (args.work/(label+'.stdout')).write_text(process.stdout)
        (args.work/(label+'.stderr')).write_text(process.stderr)
        assert process.returncode==0 and not process.stderr,(label,process.returncode,process.stderr)
        assert digest(disk)==before,'source image changed'
        timings.append({'command':command,'seconds':elapsed,'peak_rss_kib':int(memory.read_text().strip())})
        return process.stdout
    rebuilt=run('rebuild','rebuild')
    assert rebuilt=='OK rebuild segments=21 skipped=0\n',rebuilt
    status=run('status','status')
    assert status=='STATUS READY segments=21 skipped=0\n',status
    results=[]
    for number,(query,relevant) in enumerate(QUALITY_QUERIES):
        output=run('query-%02d'%number,'search','--query',query)
        matches=[]
        for line in output.splitlines():
            if not line.startswith('MATCH '): continue
            parsed=re.fullmatch(r'MATCH (\S+) lines=(\d+)-(\d+) bytes=(\d+)-(\d+) score=(-?\d+\.\d{6}) (.*)',line)
            assert parsed,line
            name,first,last,start,end,score,excerpt=parsed.groups()
            data=dict(QUALITY_FILES)[name]
            start,end,first,last=map(int,(start,end,first,last))
            assert 0<=start<end<=len(data)
            assert first==1+data[:start].count(b'\n') and last==1+data[:end-1].count(b'\n')
            assert start==data.index(b'@end\n')+5
            matches.append({'name':name,'score':float(score),'lines':[first,last],'bytes':[start,end]})
        assert len(matches)==20 and output.endswith('OK search shown=20 segments=21 skipped=0\n'),output
        top5=[r['name'] for r in matches[:5]]
        hit=[i+1 for i,row in enumerate(matches) if row['name'] in relevant]
        literal_files=[name for name,data in QUALITY_FILES if query.encode() in data]
        results.append({'query':query,'relevant':relevant,'top5':top5,
                        'top5_relevant':len(set(top5)&set(relevant)),
                        'first_relevant_rank':hit[0] if hit else None,
                        'literal_files':literal_files,'candidates':matches})
    labeled=[r for r in results if r['relevant']]
    successes=sum(r['top5_relevant']>0 for r in labeled)
    no_literal=sum(r['top5_relevant']>0 and not r['literal_files'] for r in labeled)
    version=subprocess.check_output([str(args.runner),'--version'],stderr=subprocess.STDOUT,text=True).strip()
    report={'scope':'real owned local model via production CLI; fixed small corpus only',
            'tool_sha256':digest(args.tool),'model_sha256':model_hash,'model_bytes':args.model.stat().st_size,
            'runner_sha256':runner_hash,'runner_version':version,'fixture_sha256':fixture_hash,
            'index_sha256':digest(index),'source_sha256':before,'source_unchanged':digest(disk)==before,
            'model_unchanged':digest(args.model)==model_hash,'runner_unchanged':digest(args.runner)==runner_hash,
            'labeled_queries':len(labeled),'top5_queries_with_relevant':successes,
            'semantic_hits_without_literal':no_literal,'unrelated_queries':len(results)-len(labeled),
            'timings':timings,'results':results,
            'limitations':'No global corpus accuracy or confidence guarantee. Unrelated queries still return top20 candidates; inspect their top5 false positives.'}
    (args.work/'real-model-report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
    print('REAL labeled Top5 %d/%d; without literal %d; unrelated questions %d (candidates still returned)'%
          (successes,len(labeled),no_literal,len(results)-len(labeled)))
    for r in results: print(json.dumps({k:r[k] for k in ('query','top5','top5_relevant','literal_files')},ensure_ascii=False))
    print('timings='+json.dumps(timings))
    print('report='+str(args.work/'real-model-report.json'))
    assert successes==len(labeled) and no_literal>=1, 'fixed corpus retrieval acceptance failed'
    assert report['source_unchanged'] and report['model_unchanged'] and report['runner_unchanged']
    return 0
if __name__=='__main__':sys.exit(main())
