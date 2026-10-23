"""Independent metadata-only relation reference and whole-corpus fixtures."""
import random
import struct
import subprocess
import sys
import note_metadata_reference as metadata


def header(tags=(), links=(), body=b'body\n'):
    return (b'@yan-note 1\n' + b''.join(b'tag: '+t.encode('utf-8')+b'\n' for t in tags)
            + b''.join(b'link: '+n.encode('ascii')+b'\n' for n in links) + b'@end\n' + body)


def reference(mode, target, files):
    names = {name for name, _ in files}
    if mode != 'backlinks' and target not in names:
        return 9, [], None
    try:
        if mode == 'links':
            info = metadata.parse_header(dict(files)[target][:4096])
            rows = [(name, int(name in names), 0, 0, 0) for name in info['links']]
        else:
            parsed = {name: metadata.parse_header(data[:4096]) for name, data in files}
            if mode == 'backlinks':
                rows = [(name, 1, 0, 1, 0) for name in sorted(names) if target in parsed[name]['links']]
            else:
                source = parsed[target]
                rows = []
                for name in names - {target}:
                    out = int(name in source['links'])
                    incoming = int(target in parsed[name]['links'])
                    shared = len(set(source['tags']) & set(parsed[name]['tags']))
                    if out or incoming or shared:
                        rows.append((name, 1, out, incoming, shared))
                rows.sort(key=lambda r: (-(r[2]+r[3]), -r[4], r[0]))
        total = len(rows)
        shown = rows[:20] if mode in ('backlinks', 'related') else rows
        return 0, shown, (total, len(shown), int(target in names))
    except metadata.MetadataError:
        return 4, [], None


def fixtures():
    yield [('plic', header(['os','riscv'], ['uart'])),
           ('interrupt', header(['os','riscv'], ['plic'])), ('uart', header(['os'])),
           ('memory', header(['os','riscv']))], 'plic', 'navigation'
    yield [('n',header(['os'],['n','missing','bad'])),('bad',b'@yan-note 9\n@end\n')], 'n', 'bad-target'
    yield [('b',header(links=['gone'])),('A',header(links=['gone'])),('a',header(links=['Gone']))], 'gone', 'missing-target-case'
    yield [('n',header(links=['n'])),('empty',b''),('binary',b'\x00\xff'*10000)], 'n', 'self-empty-large-binary'
    yield [('n',header(['os'],['both','one'])), ('both',header(links=['n'])),
           ('one',header()), ('tags',header(['os']))], 'n', 'directions-before-tags'
    tags = ['t%02d'%i for i in range(16)]
    yield [('n',header(tags, ['linked'])),('linked',header()),('tags',header(tags))], 'n', 'one-link-beats-16-tags'
    yield [('n',header(['OS','é','中断'])),('a',header(['os','e\u0301','中断'])),('b',header(['OS','é']))], 'n', 'exact-tags'
    yield [('n',header(['os'],body=b'\x00\xff'*10000)),('a',header(['os'],body=b'\xff'*10000))], 'n', 'opaque-binary-body'
    yield [('n',header(['os'])),('good',header(['os'])),('bad',b'@yan-note 1\nunknown: x\n@end\n')], 'n', 'unrelated-bad-header'
    yield [('bad',b'@yan-note 9\n@end\n')], 'missing', 'missing-source-before-bad-head'
    yield [('n',b'@yan-note'),('old',b'@yan-not')], 'n', 'complete-prefix-invalid'
    yield [('n',b'\xef\xbb\xbf@yan-note 1\ntag: os\n@end\n'),('a',header(['os']))], 'n', 'bom-before-marker-old'
    yield [('n',header(['x\ufeffy']))], 'n', 'feff-field-invalid'
    full = [('n',header(['os']))] + [('f%02d'%i,header(['os'], ['n'] if i%3==0 else [])) for i in range(62)]
    yield full, 'n', 'full-63-top20'
    yield full[:-1] + [('zzbad',b'@yan-note 9\n@end\n')], 'n', '63-last-bad'
    long_a, long_z = 'x'*30+'a', 'x'*30+'z'
    yield [('n',header(['os'],[long_z,long_a,'x'])),
           (long_z,header(['os'],['n'])),('x',header(['os'],['n'])),
           (long_a,header(['os'],['n']))], 'n', '31-byte-and-prefix-ties'
    yield [(long_a,header(links=[long_z])),(long_z,header(links=[long_a]))], long_a, '31-byte-source'
    yield [('n',header(['os','os']))], 'n', 'duplicate-tag-invalid'
    yield [('n',header(links=['gone','gone']))], 'n', 'duplicate-link-invalid'
    rng = random.Random(280011)
    choices = ['os','OS','riscv','中文','é','e\u0301']
    for case in range(60):
        names = ['n'] + ['f%02d'%i for i in range(rng.randrange(1,40))]
        files=[]
        for name in names:
            tags=rng.sample(choices,rng.randrange(len(choices)+1))
            links=rng.sample(names+['gone'],min(16,rng.randrange(len(names)+2)))
            files.append((name,header(tags,links,body=b'\x00\xff' if case%2 else b'body')))
        rng.shuffle(files)
        yield files,'n','random-%d'%case


def check(executable, mode, target, files, label):
    wanted_status, wanted_rows, wanted_summary = reference(mode,target,files)
    raw = bytearray(struct.pack('<I',len(target))+target.encode('ascii')+struct.pack('<I',len(files)))
    for name,data in files:
        encoded=name.encode('ascii')
        raw += struct.pack('<II',len(encoded),len(data))+encoded+data
    run=subprocess.run([executable,mode],input=raw,capture_output=True,timeout=30)
    assert run.returncode==0, (label,mode,'harness',run.returncode,run.stderr)
    rows=[]; summary=None
    for line in run.stdout.decode('ascii').splitlines():
        fields=line.split('\t')
        if fields[0]=='R' and len(fields)==6:
            rows.append((fields[1],*map(int,fields[2:])))
        elif fields[0]=='Q' and len(fields)==7 and summary is None:
            summary=tuple(map(int,fields[1:]))
        else:
            raise AssertionError((label,mode,'unexpected-output',line))
    assert summary is not None, (label,mode,'missing-footer')
    status,total,shown,exists,reads,writes=summary
    assert status==wanted_status and writes==0, (label,mode,summary,wanted_status)
    assert rows==wanted_rows, (label,mode,rows,wanted_rows)
    if wanted_summary is None:
        assert (total,shown,exists)==(0xdeadbeef,0xa5a5,1), (label,mode,'error-holder-published',summary)
    else:
        assert (total,shown,exists)==wanted_summary, (label,mode,summary,wanted_summary)
    nonempty = sum(bool(data) for _,data in files)
    if mode == 'links':
        expected_reads = int(any(name == target and data for name,data in files))
        assert reads == expected_reads, (label,mode,'source-only-prefix-read',reads,expected_reads)
    elif wanted_status == 0:
        assert reads == nonempty, (label,mode,'one-prefix-per-nonempty-file',reads,nonempty)
    else:
        assert reads <= nonempty, (label,mode,'unbounded-prefix-reads',reads,nonempty)


def main():
    count=0
    for files,target,label in fixtures():
        for mode in ('links','backlinks','related'):
            check(sys.argv[1],mode,target,files,label);count+=1
    print('PASS note relations independent oracle:',count,'queries')

if __name__=='__main__':main()
