"""Compare full term-search results to an independent Unicode/regex reference."""
import importlib.util, random, struct, subprocess, sys
from pathlib import Path
spec=importlib.util.spec_from_file_location('oracle',Path(__file__).with_name('search_terms_reference.py'))
ref=importlib.util.module_from_spec(spec);spec.loader.exec_module(ref)
exe=sys.argv[1]
backend=sys.argv[2] if len(sys.argv)>2 else 'scan'
rng=random.Random(260007)

def payload(query, files):
    b=bytearray(struct.pack('<I',len(query))+query+struct.pack('<I',len(files)))
    for slot,name,data in files:
        rawname=name.encode('ascii');b+=struct.pack('<III',slot,len(rawname),len(data))+rawname+data
    return bytes(b)

def check(query,files,label):
    try:
        expected=ref.search_reference(query,files)
    except (ValueError,UnicodeError):
        expected=None
    p=subprocess.run([exe,backend],input=payload(query,files),stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=20)
    if p.returncode:
        raise AssertionError((label,'fixture/driver failure',p.returncode,p.stderr.decode(errors='replace')))
    rows=[];summary=None
    for line in p.stdout.decode('ascii').splitlines():
        fields=line.split('\t')
        if fields[0]=='M':
            rows.append((fields[1],int(fields[2]),int(fields[3]),bool(int(fields[4])),bool(int(fields[5])),bytes.fromhex(fields[6])))
        elif fields[0]=='Q':
            summary=tuple(map(int,fields[1:]))
        else:
            raise AssertionError((label,'unknown driver output',line))
    if summary is None:
        raise AssertionError((label,'missing footer'))
    status,mode,total,shown,skipped,reads,writes=summary
    if expected is None:
        assert status==1 and not rows and reads==0 and writes==0,(label,'invalid query IO/result',summary)
    else:
        want=[(r['name'],r['line'],r['score'],r['left_clipped'],r['right_clipped'],r['snippet']) for r in expected['rows']]
        assert status==0,(label,'unexpected API error',summary)
        assert (total,shown,skipped)==(expected['total'],expected['shown'],expected['skipped']),(label,'summary',summary,expected)
        assert rows==want,(label,'rows',query,rows,want)
        assert writes==0,(label,'search wrote medium',summary)
        fits = ref.index_fits_reference(files)
        expected_mode = 0 if backend.startswith('index') and fits else 1
        assert mode == expected_mode, (label,'wrong backend mode',summary,expected_mode)
        if backend == 'index-repeat' and fits:
            assert reads <= 2 * shown, (label,'ready index rescanned source',summary)
    return mode,reads

fixed=[
    (b'interrupt',[(0,'irq.md','Interrupt 中断\nPLIC interrupt interrupt\r\n'.encode()),(2,'trap.md',b'interruption')]),
    ('中中中'.encode(),[(0,'a','中中中中'.encode())]),
    ('CPU处理中断'.encode(),[(0,'a','CPU处理中断\nCPU 正在处理中断'.encode())]),
    ('CPU 中断'.encode(),[(0,'a','cpu 中断\n处理中断 cpu'.encode())]),
    (b'CPU cpu '*30,[(0,'a',b'cpu cpu\r\nCPU')]),
    (b'aa',[(0,'a',b'aaaa aa aaaa')]),
    (b'caf',[(0,'a','café CAFE café'.encode())]),
    ('中断'.encode(),[(0,'a',('x'*4095+'中断\r\n'+'中'*100).encode())]),
    ('中'.encode(),[(0,'a',('x'*100000+'中'+'🙂'*200).encode())]),
    (b'hit',[(0,'a',b'hit\n'*21+b'hit hit hit\r\n')]),
    (b'hit',[(0,'a',b'\0hit'),(2,'b',b'hit\xff'),(3,'c',b'hit')]),
    (b'hit',[(0,'a',b'hit\r'),(1,'b',b'hit\r\n'),(2,'c',b'')]),
    (b'hit',[(0,'a',b'\n'+b'x'*40+b' hit'+b'y'*180+b'\n')]),
    (b' '.join(bytes([c]) for c in range(ord('a'),ord('q'))),[(0,'a',b' '.join(bytes([c]) for c in range(ord('a'),ord('q')) for _ in range(260)))]),
    (b' '.join(bytes([c]) for c in range(ord('a'),ord('r'))),[(0,'a',b'a')]),
    (b'\xff',[(0,'a',b'a')]),(b'\0',[(0,'a',b'a')]),(b'a\tA',[(0,'a',b'A')]),
    (b'hit',[(0,'a',b'hit\x1ahit')]),
    ('中'.encode()*341,[(0,'a','中'.encode()*350)]),
]
for i,(q,files) in enumerate(fixed):check(q,files,'fixed-'+str(i))
terms=['cpu','CPU','plic','interrupt','interruption','中断','中','断','CPU处理中断','café','中，断','Σ','σ','🙂','aa','aaaa','_x2','é']
for number in range(300):
    query=(' '.join(rng.choice(terms) for _ in range(rng.randrange(1,7)))).encode()
    slots=sorted(rng.sample(range(9),rng.randrange(1,5)))
    files=[]
    for slot in slots:
        data=''.join(rng.choice(terms)+rng.choice([' ','\t','-','/','\r','\n','\r\n','']) for _ in range(rng.randrange(0,100))).encode()
        if rng.randrange(12)==0:data+=rng.choice([b'\xff',b'\0',b'\xe4'])
        files.append((slot,'n'+str(slot),data))
    check(query,files,'random-'+str(number))
print('PASS: independent full summary/rows/score/snippet/flags/no-write oracle',len(fixed)+300,'cases; not a performance or full regression claim')
