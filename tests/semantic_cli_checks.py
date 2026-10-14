"""Actual optional CLI against independent images and an owned fake runner."""
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from semantic_fixture import image, note

def main():
    tool, runner = map(Path, sys.argv[1:])
    for p in (tool,runner):
        if not p.is_file(): raise RuntimeError('FAIL implementation/fixture missing: '+str(p))
    count=0
    with tempfile.TemporaryDirectory(prefix='yan-semantic-cli-') as tmp:
        root=Path(tmp); disk=root/'source.img'; index=root/'index'; model=root/'model'
        model.write_text('universal')
        disk.write_bytes(image([('note',note('中文标题','hello\n第二行\\\t\r\n')),
                                ('binary',b'\0')]))
        source_hash=hashlib.sha256(disk.read_bytes()).digest()
        child_log=root/'children.log'
        env=dict(os.environ,YAN_TEST_CHILD_LOG=str(child_log))
        def run(command,*extra,rc=0,out=None,err=''):
            nonlocal count
            args=[str(tool),command,'--disk-image',str(disk),'--index',str(index),
                  '--model',str(model),'--runner',str(runner),*extra]
            before=child_log.read_text().splitlines() if child_log.exists() else []
            proc=subprocess.run(args,capture_output=True,text=True,timeout=15,env=env)
            assert (proc.returncode,proc.stderr)==(rc,err),(args,proc.returncode,proc.stdout,proc.stderr)
            if out is not None: assert proc.stdout==out,(args,out,proc.stdout)
            assert hashlib.sha256(disk.read_bytes()).digest()==source_hash
            after=child_log.read_text().splitlines() if child_log.exists() else []
            added=after[len(before):]
            if rc==2: assert not added,('invalid parameters started child',added)
            if command=='status' or rc==3: assert 'server' not in added,added
            count+=1
            return proc.stdout
        run('status',out='STATUS MISSING\n')
        run('search','--query','问',rc=3,out='',err='ERROR MISSING\n')
        run('rebuild',out='OK rebuild segments=1 skipped=1\n')
        run('status',out='STATUS READY segments=1 skipped=1\n')
        text=run('search','--query','问')
        assert 'MATCH note lines=4-5 ' in text and 'hello\\x0A第二行\\\\\\x09\\x0D\\x0A\n' in text,text
        assert text.endswith('OK search shown=1 segments=1 skipped=1\n'),text
        # Output errors occur after successful publication and must keep it.
        for command,extra in [('rebuild',[]),('search',['--query','问'])]:
            with open('/dev/full','wb') as full:
                proc=subprocess.run([str(tool),command,'--disk-image',str(disk),'--index',str(index),
                     '--model',str(model),'--runner',str(runner),*extra],stdout=full,stderr=subprocess.PIPE,
                     text=True,timeout=15,env=env)
            assert proc.returncode==5 and proc.stderr=='ERROR OUTPUT\n',proc
            assert hashlib.sha256(disk.read_bytes()).digest()==source_hash
            count+=1
            run('status',out='STATUS READY segments=1 skipped=1\n')
        saved=index.read_bytes(); broken=bytearray(saved);broken[0]^=1;index.write_bytes(broken)
        run('status',out='STATUS CORRUPT\n')
        run('search','--query','问',rc=3,out='',err='ERROR CORRUPT\n')
        index.write_bytes(saved)
        for value in ('0','3601','x','1.5','999999999999999999999999'):
            run('rebuild','--startup-timeout',value,rc=2,out='',err='ERROR USAGE\n')
        for query in ('',' ','\t','x\n','x\r','\x7f','x'*1024):
            run('search','--query',query,rc=2,out='',err='ERROR USAGE\n')
        run('status','--query','x',rc=2,out='',err='ERROR USAGE\n')
        run('status','--model',str(model),rc=2,out='',err='ERROR USAGE\n')
        run('status','--unknown','x',rc=2,out='',err='ERROR USAGE\n')
        model.write_text('changed')
        run('status',out='STATUS STALE\n')
        run('search','--query','问',rc=3,out='',err='ERROR STALE\n')
        model.unlink(); index.unlink()
        run('status',rc=4,out='',err='ERROR DEPENDENCY\n')
        model.write_text('universal')
        disk.write_bytes(image([]));source_hash=hashlib.sha256(disk.read_bytes()).digest()
        run('rebuild',out='OK rebuild segments=0 skipped=0\n')
        run('status',out='STATUS READY segments=0 skipped=0\n')
    print('PASS semantic CLI: %d cases'%count)
if __name__=='__main__':main()
