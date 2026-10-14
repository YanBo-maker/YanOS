"""1024-dimensional exact cosine reference, independent of C facade/index."""
import math
from pathlib import Path
import random
import struct
import subprocess
import sys

def f32(x): return struct.unpack('<f',struct.pack('<f',x))[0]
def cosine(a,b):
    dot=sum(x*y for x,y in zip(a,b))
    return max(-1.0,min(1.0,dot/(math.sqrt(sum(x*x for x in a))*math.sqrt(sum(y*y for y in b)))))
def main():
    probe=Path(sys.argv[1])
    if not probe.is_file(): raise RuntimeError('FAIL implementation under test missing: '+str(probe))
    rng=random.Random(290015); count=0
    def check(vectors,query,expected='OK'):
        nonlocal count
        vectors=[[f32(x) for x in v] for v in vectors]; query=list(map(f32,query))
        data=struct.pack('<I',len(vectors))+b''.join(struct.pack('<1024f',*v) for v in vectors+[query])
        run=subprocess.run([str(probe)],input=data,capture_output=True,timeout=15)
        assert run.returncode==0 and not run.stderr,(run.returncode,run.stderr)
        lines=run.stdout.decode().splitlines()
        assert lines[-1]=='RESULT '+expected,lines
        if expected=='OK':
            wanted=[('doc%02d'%i,cosine(query,v)) for i,v in enumerate(vectors)]
            wanted.sort(key=lambda item:(-item[1],item[0]));wanted=wanted[:20]
            assert len(lines)-1==len(wanted),(len(lines),len(wanted))
            for line,(name,score) in zip(lines,wanted):
                fields=line.split()
                assert fields[0:2]==['ROW',name] and fields[3:]==['0','5','1','1'],line
                assert math.isclose(float(fields[2]),score,rel_tol=1e-12,abs_tol=1e-12),(line,score)
        else: assert lines==['RESULT '+expected],lines
        count+=1
    unit=[1.0]+[0.0]*1023
    check([],unit);check([unit]*63,unit)
    # The last dimensions must contribute, and negative scores retain order.
    last=[0.0]*1023+[1.0]
    check([unit,last,[-x for x in last]],last)
    check([[1.0,0.0004]+[0.0]*1022,[1.0,0.0003]+[0.0]*1022],unit)
    for scale in (1e-38,1e30):
        check([[scale]*1024,[-scale]*1024],unit)
    for _ in range(40):
        vectors=[[rng.uniform(-3,3) for _ in range(1024)] for _ in range(rng.randrange(1,64))]
        query=[rng.uniform(-3,3) for _ in range(1024)]
        check(vectors,query)
    check([[0.0]*1024],unit,'PROVIDER')
    check([unit],[0.0]*1024,'PROVIDER')
    check([[float('inf')]+[0.0]*1023],unit,'PROVIDER')
    check([[float('nan')]+[0.0]*1023],unit,'PROVIDER')
    print('PASS semantic cosine/ranking independent oracle: %d cases'%count)
if __name__=='__main__':main()
