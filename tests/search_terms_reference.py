"""Independent Python reference for 0026; Unicode text+regex, no C tokenizer code."""
import re

ASCII_WORD = r'[A-Za-z0-9_]'
FOLD = str.maketrans({chr(i): chr(i+32) for i in range(65,91)})

def query_patterns(raw):
    if not 1 <= len(raw) <= 1023:
        raise ValueError('query length')
    text = raw.decode('utf-8', errors='strict')
    if any((ord(c)<32 and c!='\t') or ord(c)==127 for c in text):
        raise ValueError('control byte')
    groups = re.findall(r'[A-Za-z0-9_\u0080-\U0010ffff]+', text)
    groups = list(dict.fromkeys(group.translate(FOLD) for group in groups))
    if not groups or len(groups)>16:
        raise ValueError('group count')
    patterns = []
    for group in groups:
        pieces = []
        for atom in re.findall(r'[a-z0-9_]+|[^\x00-\x7f]', group):
            if ord(atom[0])<128:
                if len(atom.encode('ascii'))>255:
                    raise ValueError('ASCII word length')
                # Exact ASCII word boundaries, not Python Unicode \w boundaries.
                word = ''.join('['+c+c.upper()+']' if 'a'<=c<='z' else re.escape(c) for c in atom)
                pieces.append('(?<!'+ASCII_WORD+')'+word+'(?!'+ASCII_WORD+')')
            else:
                pieces.append(re.escape(atom))
        patterns.append(re.compile('(?=('+''.join(pieces)+'))'))
    return patterns

def index_fits_reference(files):
    """Independent regex census used only to verify index vs scan mode."""
    keys = set()
    postings = 0
    for _slot, _name, data in files:
        if b'\0' in data:
            continue
        try:
            text = data.decode('utf-8', errors='strict')
        except UnicodeError:
            continue
        for atom in re.findall(r'[A-Za-z0-9_]+|[^\x00-\x7f]', text):
            key = atom.translate(FOLD).encode('utf-8')
            if ord(atom[0]) < 128 and len(key) > 255:
                return False
            keys.add(key)
            postings += 1
    return len(keys) <= 8192 and sum(map(len, keys)) <= 262144 and postings <= 65536

def search_reference(query, files):
    patterns = query_patterns(query)
    rows, skipped = [], 0
    for slot, name, data in files:
        try:
            if b'\0' in data:
                raise ValueError('NUL')
            data.decode('utf-8', errors='strict')
        except (UnicodeError, ValueError):
            skipped += 1
            continue
        parts = data.split(b'\n')
        split_count = len(parts)
        if not data or data.endswith(b'\n'):
            parts.pop()
        byte_start = 0
        for number, rawline in enumerate(parts,1):
            terminated = number < split_count
            content = rawline[:-1] if terminated and rawline.endswith(b'\r') else rawline
            line = content.decode('utf-8')
            matches = [list(p.finditer(line)) for p in patterns]
            if all(matches):
                anchor = min(m.start() for ms in matches for m in ms)
                left = max(0, anchor-40)
                right = min(len(line),left+160)
                snippet = line[left:right].encode('utf-8')
                rows.append(dict(slot=slot,name=name,line=number,
                    score=sum(min(len(ms),255) for ms in matches),
                    anchor=byte_start+len(line[:anchor].encode('utf-8')),
                    snippet_start=byte_start+len(line[:left].encode('utf-8')),
                    snippet=snippet,left_clipped=left>0,right_clipped=right<len(line)))
            byte_start += len(rawline)+1
    rows.sort(key=lambda r:(-r['score'],r['slot'],r['line']))
    return dict(total=len(rows),shown=min(len(rows),20),skipped=skipped,rows=rows[:20])

if __name__=='__main__':
    fs=[(0,'irq.md','Interrupt controller handles 中断\nPLIC handles interrupt interrupt\n'.encode()),
        (2,'trap.md',b'interruption pending'),(3,'broken',b'interrupt\xff'),(4,'nul',b'interrupt\0')]
    r=search_reference(b'interrupt',fs)
    assert (r['total'],r['skipped'],[(x['line'],x['score']) for x in r['rows']])==(2,2,[(2,2),(1,1)])
    assert search_reference(b'CPU cpu',[(0,'a',b'CPU')])['rows'][0]['score']==1
    assert search_reference(b' '.join([b'CPU',b'cpu']*9),[(0,'a',b'CPU')])['rows'][0]['score']==1
    assert search_reference('中中中'.encode(),[(0,'a','中中中中'.encode())])['rows'][0]['score']==2
    assert search_reference(b'aa',[(0,'a',b'aaaa')])['total']==0
    assert search_reference('中断'.encode(),[(0,'a','中 x 断'.encode())])['total']==0
    assert search_reference('CPU处理中断'.encode(),[(0,'a','cpu 正在处理中断'.encode())])['total']==0
    assert search_reference('CPU 中断'.encode(),[(0,'a','cpu 正在处理中断'.encode())])['total']==1
    assert search_reference(b'caf',[(0,'a','café'.encode())])['total']==1
    assert search_reference('中'.encode(),[(0,'a',('x'*100000+'中'+'🙂'*200).encode())])['rows'][0]['snippet']==('x'*40+'中'+'🙂'*119).encode()
    print('Independent reference fixed examples: PASS (design/verification helper, no C implementation tested)')
