"""0028 through the production RV32 terminal and persistent YanFS images."""
import argparse
from pathlib import Path
import subprocess
import sys
sys.dont_write_bytecode=True
import run_note_metadata as meta
terms=meta.terms
base=terms.base
INPUTS=tuple(meta.INPUTS)+('os/note_relations.h','os/note_relations.c','tests/guest/run_note_relations.py')


def seed(path, files):
    capacity=128
    image=bytearray(capacity*4096)
    directory=bytearray(base.metadata_block(capacity))
    lba=1
    for slot,(name,data) in enumerate(files):
        count=(len(data)+4095)//4096
        base.put_entry(directory,slot,name,len(data),lba if count else 0,count)
        if data:image[lba*4096:lba*4096+len(data)]=data
        lba+=count
    assert lba<=capacity
    base.reseal(directory);image[:4096]=directory;path.write_bytes(image)


def run(args, work, label, files, expected, readonly=True, disk=None):
    if disk is None:
        disk=work/(label+'.img');seed(disk,files)
    return terms.run_commands(args,work,label,[],expected,readonly=readonly,disk=disk)


def related(rows, total=None):
    result=b''.join(('RELATED %s out=%d in=%d tags=%d\r\n'%r).encode() for r in rows)
    return result+('OK related total=%d shown=%d\r\n'%(len(rows) if total is None else total,len(rows))).encode()


def navigation(args, work):
    run(args,work,'navigation',[
        ('plic',meta.head(b'tag: os',b'tag: riscv',b'link: uart')),
        ('interrupt',meta.head(b'tag: os',b'tag: riscv',b'link: plic')),
        ('uart',meta.head(b'tag: os')),('memory',meta.head(b'tag: os',b'tag: riscv'))],[
        (b'links plic',b'LINK uart OK\r\nOK links shown=1\r\n'),
        (b'backlinks plic',b'BACKLINK interrupt\r\nOK backlinks total=1 shown=1 target=OK\r\n'),
        (b'related plic',related([('interrupt',0,1,2),('uart',1,0,1),('memory',0,0,2)])),
        (b'links uart',b'OK links shown=0\r\n'),
        (b'backlinks absent',b'OK backlinks total=0 shown=0 target=DANGLING\r\n')])


def missing_and_bad_target(args, work):
    run(args,work,'missing-bad-target',[
        ('n',meta.head(b'link: n',b'link: gone',b'link: bad')),
        ('bad',b'@yan-note 9\n@end\n')],[
        (b'links n',b'LINK n OK\r\nLINK gone DANGLING\r\nLINK bad OK\r\nOK links shown=3\r\n'),
        (b'backlinks gone',b'ERROR METADATA\r\n'),
        (b'related missing',b'ERROR NOT_FOUND\r\n'),
        (b'links bad',b'ERROR METADATA\r\n')])


def missing_target_backlinks(args, work):
    run(args,work,'missing-backlinks',[
        ('z',meta.head(b'link: gone')),('A',meta.head(b'link: gone')),
        ('a',meta.head(b'link: Gone')),('self',meta.head(b'link: self'))],[
        (b'backlinks gone',b'BACKLINK A\r\nBACKLINK z\r\nOK backlinks total=2 shown=2 target=DANGLING\r\n'),
        (b'backlinks self',b'BACKLINK self\r\nOK backlinks total=1 shown=1 target=OK\r\n'),
        (b'related self',related([]))])


def late_bad_header(args, work):
    files=[('n',meta.head(b'tag: os'))]+[('empty%02d'%i,b'') for i in range(61)]
    files += [('zzbad',b'@yan-note 9\n@end\n')]
    run(args,work,'late-bad',files,[
        (b'links n',b'OK links shown=0\r\n'),
        (b'backlinks n',b'ERROR METADATA\r\n'),
        (b'related n',b'ERROR METADATA\r\n'),
        (b'related missing',b'ERROR NOT_FOUND\r\n')])


def top20_full_directory(args, work):
    files=[('n',meta.head(b'tag: os'))]+[('f%02d'%i,meta.head(b'tag: os')) for i in reversed(range(62))]
    run(args,work,'top20',files,[(b'related n',related([('f%02d'%i,0,0,1) for i in range(20)],62))])


def opaque_and_exact_tags(args, work):
    run(args,work,'opaque',[
        ('n',meta.head(b'tag: os',b'tag: OS',body=b'\x00\xff'*10000)),
        ('a',meta.head(b'tag: OS',body=b'\xff'*10000)),('binary',b'\x00\xff'*5000),('old',b'')],[
        (b'links n',b'OK links shown=0\r\n'),
        (b'related n',related([('a',0,0,1)])),
        (b'related binary',related([])),
        (b'backlinks n',b'OK backlinks total=0 shown=0 target=OK\r\n')])


def mutations_and_restart(args, work):
    disk,_=run(args,work,'changes',[
        ('n',meta.head(b'tag: os',b'link: a')),
        ('a',meta.head(b'tag: os',b'link: n'))],[
        (b'related n',related([('a',1,1,1)])),
        (b'mv a b',b'OK mv\r\n'),
        (b'links n',b'LINK a DANGLING\r\nOK links shown=1\r\n'),
        (b'backlinks a',b'BACKLINK n\r\nOK backlinks total=1 shown=1 target=DANGLING\r\n'),
        (b'related n',related([('b',0,1,1)])),
        (b'cp b c',b'OK cp\r\n'),
        (b'related n',related([('b',0,1,1),('c',0,1,1)])),
        (b'rm b',b'OK rm\r\n'),
        (b'meta link n del a',b'OK meta link\r\n'),
        (b'meta link n add c',b'OK meta link\r\n'),
        (b'meta tag c del os',b'OK meta tag\r\n'),
        (b'related n',related([('c',1,1,0)])),
        (b'write c plain',b'OK write\r\n'),
        (b'related n',related([('c',1,0,0)])),
        (b'rm c',b'OK rm\r\n'),
        (b'related n',related([]))],readonly=False)
    run(args,work,'restart',[],[
        (b'links n',b'LINK c DANGLING\r\nOK links shown=1\r\n'),
        (b'backlinks c',b'BACKLINK n\r\nOK backlinks total=1 shown=1 target=DANGLING\r\n'),
        (b'related n',related([]))],disk=disk)


def grammar(args, work):
    expected=[(cmd,b'ERROR USAGE\r\n') for cmd in (
        b'links',b'links n extra',b'backlinks',b'backlinks ../x',b'related',b'related n extra')]
    expected += [(b'links absent',b'ERROR NOT_FOUND\r\n'),(b'backlinks absent',b'OK backlinks total=0 shown=0 target=DANGLING\r\n')]
    run(args,work,'grammar',[('n',b'plain')],expected)


CASES=[navigation,missing_and_bad_target,missing_target_backlinks,late_bad_header,
       top20_full_directory,opaque_and_exact_tags,mutations_and_restart,grammar]


def main():
    parser=argparse.ArgumentParser()
    for name in ('source','work','run','mkfs','guest'):parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args()
    missing=[args.source/p for p in INPUTS if not (args.source/p).is_file()]
    missing += [p for p in (args.run,args.mkfs,args.guest) if not p.is_file()]
    if missing:
        print(':FAIL: missing required input:',', '.join(map(str,missing)));return 1
    failed=harness=0
    for case in CASES:
        work=args.work/case.__name__;work.mkdir(parents=True,exist_ok=True)
        try:case(args,work);print('PASS',case.__name__,flush=True)
        except AssertionError as error:failed+=1;print(':FAIL:',case.__name__,error,flush=True)
        except (base.HarnessError,OSError,subprocess.TimeoutExpired) as error:
            harness+=1;print(':HARNESS-ERROR:',case.__name__,error,flush=True)
    print('note relations Guest: %d passed, %d failed, %d harness errors'%(len(CASES)-failed-harness,failed,harness))
    return 2 if harness else 1 if failed else 0

if __name__=='__main__':sys.exit(main())
