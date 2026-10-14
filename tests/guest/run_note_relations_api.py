"""Real RV32 service guard probe. Repository inputs hard-fail before tool skips."""
import argparse
from pathlib import Path
import struct
import subprocess
import sys
import run_search_terms_api as common
INPUTS = ['os/note_relations.c','os/note_relations.h','os/note_metadata_format.c',
          'os/note_metadata_format.h','os/yanfs.c','os/yanfs.h','os/memory.c',
          'os/memory.h','os/platform.h','tests/guest/start.S','tests/guest/link.ld',
          'tests/guest/guest.h','tests/guest/note_relations_api_check.c',
          'tests/guest/run_note_relations_api.py','tests/guest/run_search_terms_api.py']
SIGNATURE=b'NOTE_RELATIONS_API_PASS'
def classify_probe(result, signature):
    blob=result.stdout+result.stderr
    if result.returncode is None or result.returncode < 0 or result.returncode >=128 or any(h in blob for h in (b'Sanitizer',b'runtime error:')):
        return 2,'crash/timeout/sanitizer'
    if result.returncode not in (0,6) or len(signature)!=4:
        return 2,'executor exit/signature unavailable'
    code=struct.unpack('<I',signature)[0]
    if code!=1:
        return 1,'service guard check %d'%code
    if result.returncode!=0 or SIGNATURE not in result.stdout:
        return 2,'healthy tohost lacks marker/exit'
    return 0,'NOTE_RELATIONS_API_PASS, tohost 1'
def main():
    parser=argparse.ArgumentParser()
    for name in ('source','work','run'): parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--gcc',default='riscv64-linux-gnu-gcc'); parser.add_argument('--nm',default='riscv64-linux-gnu-nm')
    args=parser.parse_args()
    missing=[args.source/p for p in INPUTS if not (args.source/p).is_file()]
    if not args.run.is_file(): missing.append(args.run)
    if missing: print(':FAIL: missing required input', ', '.join(map(str, missing))); return 1
    if not common.tool_available(args.gcc) or not common.tool_available(args.nm): print('SKIP toolchain'); return 77
    args.work.mkdir(parents=True,exist_ok=True); elf=args.work/'note_relations_api.elf'
    argv=[args.gcc,*common.CROSS_FLAGS,'-I',str(args.source/'os'),'-I',str(args.source/'tests/guest'),'-T',str(args.source/'tests/guest/link.ld'),'-o',str(elf)]
    argv += [str(args.source/p) for p in ('os/note_relations.c','os/note_metadata_format.c','os/yanfs.c','os/memory.c','tests/guest/start.S','tests/guest/note_relations_api_check.c')]
    built=common.run(argv); (args.work/'build.log').write_bytes(built.stdout+built.stderr)
    if built.returncode: print(':HARNESS-ERROR: build failed',built.stderr.decode(errors='replace')); return 2
    symbols=common.run([args.nm,'-n',str(elf)])
    addresses=[int(r.split()[0],16) for r in symbols.stdout.decode().splitlines() if len(r.split())==3 and r.split()[2]=='tohost']
    if symbols.returncode or len(addresses)!=1: print(':HARNESS-ERROR: symbol lookup'); return 2
    signature=args.work/'tohost.bin'
    if signature.exists(): signature.unlink()
    address=addresses[0]
    ran=common.run([str(args.run),'--image',str(elf),'--terminal','--max-steps',common.MAX_STEPS,'--signature',str(signature),hex(address),hex(address+4)])
    (args.work/'run.log').write_bytes(ran.stdout+ran.stderr)
    code,detail=classify_probe(ran,signature.read_bytes() if signature.is_file() else b'')
    print(('PASS' if code==0 else ':FAIL:' if code==1 else ':HARNESS-ERROR:'),detail)
    return code
if __name__=='__main__':
    try: sys.exit(main())
    except (OSError,subprocess.TimeoutExpired) as error: print(':HARNESS-ERROR:',error); sys.exit(2)
