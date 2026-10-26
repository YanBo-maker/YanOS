"""0040 independent full-image and complete-output performance oracle."""
import hashlib
import re
import struct
import zlib

BLOCK=4096
SIZES=(1023,2047,4095)
CASES=('mount','create','ls','backlinks','related','block-backlinks','meta','meta-filter','search')
STEP_LIMIT=50_000_000
NOTE=b'@yan-note 1\nlink: f0000.txt\ntag: t\n@end\n@block x\n[[f0000.txt#x]]\n'
PLAIN=b'a'*60

class Failure(Exception):
    def __init__(self, code, detail=''):
        self.code=code
        super().__init__(code+(': '+detail if detail else ''))

def require(condition, code):
    if not condition: raise Failure(code)

def fixture(n):
    d = (n+1)//64
    blocks = n+d+4
    image = bytearray(blocks*BLOCK)
    image[:8] = b'YANFS01\0'
    struct.pack_into('<5I', image, 8, 1 if d==1 else 2, BLOCK, blocks, n, 0 if d==1 else d)
    for slot in range(n):
        payload = PLAIN if slot==n//2 else NOTE
        at = 64+slot*64
        name = ('f%04d.txt'%slot).encode()
        image[at:at+len(name)] = name
        struct.pack_into('<3I', image, at+32, len(payload), d+slot, 1)
        image[(d+slot)*BLOCK:(d+slot)*BLOCK+len(payload)] = payload
    struct.pack_into('<I', image, 60, zlib.crc32(image[:d*BLOCK]) & 0xffffffff)
    return bytes(image)


def _decode_check(image, n):
    magic=image[:8]
    version,blocksize,blocks,capacity=struct.unpack_from('<4I',image,8)
    d=1 if version==1 else struct.unpack_from('<I',image,24)[0]
    assert magic==b'YANFS01\0' and blocksize==BLOCK and capacity==n
    assert version==(1 if d==1 else 2) and not any(image[28:60])
    assert version!=1 or struct.unpack_from('<I',image,24)[0]==0
    assert blocks==n+d+4 and len(image)==blocks*BLOCK and d==(n+1)//64
    directory=bytearray(image[:d*BLOCK]); stored=struct.unpack_from('<I',directory,60)[0]
    directory[60:64]=b'\0'*4
    assert zlib.crc32(directory)&0xffffffff==stored
    names=[]; extents=[]
    for slot in range(capacity):
        entry=image[64+slot*64:128+slot*64]
        name=entry[:32].split(b'\0')[0]
        size,start,count=struct.unpack_from('<3I',entry,32)
        want=PLAIN if slot==n//2 else NOTE
        assert name==('f%04d.txt'%slot).encode() and size==len(want)
        assert not any(entry[len(name):32])
        assert start==d+slot and count==1 and not any(entry[44:])
        data=image[start*BLOCK:(start+count)*BLOCK]
        assert data[:size]==want and not any(data[size:])
        names.append(name);extents.append((start,start+count))
    assert len(set(names))==n and all(a[1]==b[0] for a,b in zip(extents,extents[1:]))
    assert not any(image[(d+n)*BLOCK:])
    return dict(files=n, blocks=blocks, directory_blocks=d, crc=stored,
                sha256=hashlib.sha256(image).hexdigest(), contiguous=True,
                all_content_tail_zero=True, slot_name_order='ascending')


def decode_check(image,n):
    try: return _decode_check(image,n)
    except (AssertionError,struct.error,IndexError) as error:
        raise Failure('FIXTURE_STRUCTURE',str(error)) from error


def expected(n, case):
    names=[('f%04d.txt'%i).encode() for i in range(n)]
    plain=n//2; notes=[s for i,s in enumerate(names) if i!=plain]
    prefix=b'yanfs terminal\r\nyanfs> '
    tail=b'yanfs> exit\r\nOK exit\r\nyanfs: exit\r\n'
    if case=='mount': return b'exit\n',prefix+b'exit\r\nOK exit\r\nyanfs: exit\r\n'
    if case=='create':
        rm=b'rm '+names[plain]
        create=b'create '+names[plain]+b' '+PLAIN
        return rm+b'\n'+create+b'\nexit\n',prefix+rm+b'\r\nOK rm\r\nyanfs> '+create+b'\r\nOK create\r\n'+tail
    if case=='ls':
        cmd=b'ls';body=b''.join(str(60 if i==plain else len(NOTE)).encode()+b' '+s+b'\r\n' for i,s in enumerate(names))+b'OK ls\r\n'
    elif case in ('meta','meta-filter'):
        cmd=b'meta list'+(b' tag:t' if case=='meta-filter' else b'')
        items=notes if case=='meta-filter' else names
        body=b''.join(s+b'\r\n' for s in items)+('OK meta list shown=%d\r\n'%len(items)).encode()
    elif case=='backlinks':
        cmd=b'backlinks f0000.txt'
        body=b''.join(b'BACKLINK '+s+b'\r\n' for s in notes[:20])+('OK backlinks total=%d shown=20 target=OK\r\n'%(n-1)).encode()
    elif case=='related':
        cmd=b'related f0000.txt'
        body=b''.join(b'RELATED '+s+b' out=0 in=1 tags=1\r\n' for s in notes[1:21])+('OK related total=%d shown=20\r\n'%(n-2)).encode()
    elif case=='block-backlinks':
        cmd=b'block backlinks f0000.txt#x'
        body=b''.join(b'BACKLINK source='+s+b' block=#x line=6 bytes=49:64 target=f0000.txt#x\r\n' for s in notes[:20])+('OK block backlinks total=%d shown=20 skipped=0 target=OK\r\n'%(n-1)).encode()
    elif case=='search':
        cmd=b'search x'
        body=b''.join(s+b':5:@block x\r\n'+s+b':6:[[f0000.txt#x]]\r\n' for s in names[:10])+('OK search total=%d shown=20 skipped=0 mode=index\r\n'%(2*(n-1))).encode()
    else: raise ValueError(case)
    return cmd+b'\nexit\n',prefix+cmd+b'\r\n'+body+tail


def arena_bytes(n):
    total=0
    for size in ((n+1)*64,n*36,n*32,n*36,n,n*36,n*32,n*2,n*2,n*2,n*2):
        total=(total+15)//16*16+size
    return total


def parse_status(text):
    values={}
    for line in text.splitlines():
        require('=' in line,'STATUS_LINE')
        key,value=line.split('=',1)
        require(key not in values,'STATUS_DUPLICATE')
        values[key]=value
    return values


def verify_matrix(records):
    keys=[(record.get('files'),record.get('case')) for record in records]
    require(len(keys)==len(SIZES)*len(CASES) and len(set(keys))==len(keys) and
            set(keys)=={(n,case) for n in SIZES for case in CASES},'MATRIX')


def integer(values,key,base=10):
    require(key in values,'STATUS_MISSING_'+key)
    try: value=int(values[key],base)
    except (TypeError,ValueError): raise Failure('STATUS_NUMBER_'+key) from None
    require(value>=0,'STATUS_NEGATIVE_'+key)
    return value


def verify(n,case,values,capture,disk,image,returncode=0,stdout=b'',stderr=b''):
    require(case in CASES and n in SIZES,'CASE')
    require(not re.search(rb'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:|Sanitizer:|HARNESS.ERROR',stdout+stderr,re.I),'DIAGNOSTIC')
    require(returncode==0 and values.get('verdict')=='pass','EXIT')
    require(integer(values,'tohost',16)==1,'TOHOST')
    _,want=expected(n,case)
    require(capture==want,'CAPTURE_EXACT')
    require(disk==image==fixture(n),'IMAGE_EXACT')
    require(integer(values,'delivered')==len(capture),'DELIVERED')
    require(integer(values,'out_length')==len(capture)<65535,'OUTPUT_LENGTH')
    require(values.get('protocol_fired')=='0' and values.get('tx_fired')=='0','FAULT')
    require(values.get('arena_known')=='1' and integer(values,'arena_used')==arena_bytes(n),'ARENA')
    count=1 if case=='mount' else 3 if case=='create' else 2
    for kind in ('input','prompt'):
        found={key for key in values if key.startswith('perf_'+kind+'_')}
        require(found=={'perf_%s_%d'%(kind,i) for i in range(count)},'TIMING_MARKERS')
    prompts=[integer(values,'perf_prompt_%d'%i) for i in range(count)]
    inputs=[integer(values,'perf_input_%d'%i) for i in range(count)]
    total=integer(values,'perf_total')
    require(0<prompts[0]<inputs[0],'TIMING_ORDER')
    for i in range(count-1):
        require(inputs[i]<prompts[i+1]<inputs[i+1],'TIMING_ORDER')
    require(inputs[-1]<total,'TIMING_ORDER')
    index=1 if case=='create' else 0
    steps=prompts[0] if case=='mount' else prompts[index+1]-inputs[index]
    stack=integer(values,'task0_observed_stack_bytes')
    require(0<stack<=4096,'STACK')
    require(0<prompts[0]<=STEP_LIMIT,'MOUNT_BUDGET')
    require(0<steps<=STEP_LIMIT,'COMMAND_BUDGET')
    return dict(files=n,case=case,mount=prompts[0],steps=steps,stack=stack,
                arena=arena_bytes(n),capture_bytes=len(capture),
                capture_sha256=hashlib.sha256(capture).hexdigest())
