"""0027 real Guest metadata faults via the shared production Host driver.

Corrupted response tags land at prefix/body/data/directory phases; classification
requires both driver status and known application tohost, never just nonzero rc.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import run_terminal_runtime as runtime
import run_editor as editor
INPUTS=tuple(editor.PRODUCTION_INPUTS)+tuple(runtime.HOST_SOURCES)+tuple(runtime.HOST_HEADERS)+(
    'os/note_metadata.c','os/note_metadata.h','os/note_metadata_format.c','os/note_metadata_format.h',
    'tests/guest/run_note_metadata_faults.py','tests/guest/run_terminal_runtime.py','tests/guest/run_editor.py')
def main():
    p=argparse.ArgumentParser()
    for name in ('source','work','guest'): p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--cc',required=True);p.add_argument('--sanitizers',action='store_true');args=p.parse_args()
    missing=[args.source/rel for rel in INPUTS if not (args.source/rel).is_file()]
    if not args.guest.is_file(): missing.append(args.guest)
    if missing: print(':FAIL: missing required input:', ', '.join(map(str, missing))); return 1
    if not runtime.tool_available(args.cc): print('SKIP host compiler');return 77
    args.work.mkdir(parents=True,exist_ok=True)
    driver=runtime.build_host(args.source,args.work,args.cc,sanitizers=args.sanitizers)
    body=b'x'*5000
    image=runtime.seeded_image([('n',body,1)])
    passed=failed=harness=0
    def exercise(label,command,*,response=0,io=False,tx=0,baseline=None,expected=1,readonly=True):
        result,status,disk,capture=runtime.run_production(driver,args,args.work,image,command+b'\nexit\n',label,
            'io' if io else 'protocol' if response else 'none',response,tx,
            baseline=baseline,expect_tohost=expected)
        values=runtime.classify_production(result,status,label,{expected})
        if values['_tohost']!=expected: raise runtime.OwnerFailure(label+': wrong tohost')
        output=capture.read_bytes(); raw=disk.read_bytes()
        if readonly and raw!=image: raise runtime.OwnerFailure(label+': read-only image changed')
        if response:
            if values['protocol_fired']!='1' or int(values.get('block_responses','-1'))!=response or b'ERROR PROTOCOL\r\n' not in output:
                raise runtime.OwnerFailure(label+': protocol injection/diagnostic absent')
        if io and (int(values.get('block_responses','-1'))!=4 or b'ERROR IO\r\n' not in output): raise runtime.OwnerFailure(label+': IO diagnostic absent')
        if expected!=1 and not tx and (b'OK meta' in output or b'yanfs: exit\r\n' in output):
            raise runtime.OwnerFailure(label+': success/later command after failure')
        return output,raw,values
    def case(label,fn):
        nonlocal passed,failed,harness
        try: fn(); passed+=1;print('PASS',label,flush=True)
        except runtime.OwnerFailure as error: failed+=1;print(':FAIL:',label,error,flush=True)
        except (runtime.HarnessError,OSError,subprocess.TimeoutExpired) as error: harness+=1;print(':HARNESS-ERROR:',label,error,flush=True)
    # Show requires one prefix request regardless of the large binary-compatible body.
    healthy,_,_=exercise('show-baseline',b'meta show n')
    passed+=1;print('PASS show-baseline',flush=True)
    for command,label in ((b'meta show n','show'),(b'meta list','list'),(b'search tag:os x','filter')):
        case(label+'-prefix-protocol',lambda command=command,label=label:exercise(label+'-prefix-protocol',command,response=4,expected=runtime.TOHOST_SHELL_FATAL))
    case('show-prefix-io',lambda:exercise('show-prefix-io',b'meta show n',io=True,expected=runtime.TOHOST_SHELL_FATAL))
    # Update: probe1, capacity2, mount3, head4, whole body5/6, new data7/8,
    # directory9.
    for response in range(4,10):
        def check_update(response=response):
            _,raw,_=exercise('update-protocol-%d'%response,b'meta tag n add os',response=response,
                expected=runtime.TOHOST_SHELL_FATAL,readonly=response<=6)
            if response<=8 and raw[:4096]!=image[:4096]:
                raise runtime.OwnerFailure('failed update published directory before data complete')
            if response==9 and raw[:4096]==image[:4096]:
                raise runtime.OwnerFailure('directory response fixture failed to exercise committed nonrollback write')
        case('update-protocol-%d'%response,check_update)
    baseline=args.work/'show-baseline.bin';baseline.write_bytes(healthy)
    start=healthy.index(b'META n\r\n'); tail=healthy.index(b'OK meta show\r\n')
    for label,at in (('first',start+1),('middle',start+12),('last',tail+len(b'OK meta show\r\n'))):
        def check_tx(label=label,at=at):
            out,_,v=exercise('show-tx-'+label,b'meta show n',tx=at,baseline=baseline,expected=runtime.TOHOST_SHELL_FATAL)
            if v['tx_fired']!='1' or v['delivered']!=at or int(v.get('block_responses','-1'))!=4 or out!=healthy[:at]:
                raise runtime.OwnerFailure('output failure did not stop at exact healthy prefix')
        case('show-tx-'+label,check_tx)
    listed,_,_=exercise('list-baseline',b'meta list')
    passed+=1;print('PASS list-baseline',flush=True)
    baseline_list=args.work/'list-baseline.bin';baseline_list.write_bytes(listed)
    start_list=listed.index(b'n\r\nOK meta list shown=1\r\n')
    for label,at in (('name-first',start_list+1),('receipt-middle',start_list+7),('receipt-last',start_list+len(b'n\r\nOK meta list shown=1\r\n'))):
        def check_list_tx(label=label,at=at):
            out,_,v=exercise('list-tx-'+label,b'meta list',tx=at,baseline=baseline_list,expected=runtime.TOHOST_SHELL_FATAL)
            if v['tx_fired']!='1' or int(v.get('block_responses','-1'))!=4 or out!=listed[:at]:
                raise runtime.OwnerFailure('list output failure continued IO/output')
        case('list-tx-'+label,check_list_tx)
    update,committed,_=exercise('update-baseline',b'meta tag n add os',readonly=False)
    passed+=1;print('PASS update-baseline',flush=True)
    baseline2=args.work/'update-baseline.bin';baseline2.write_bytes(update)
    receipt=update.index(b'OK meta tag\r\n')
    for label,at in (('first',receipt+1),('middle',receipt+3),('last',receipt+len(b'OK meta tag\r\n'))):
        def committed_tx(label=label,at=at):
            out,raw,v=exercise('update-receipt-tx-'+label,b'meta tag n add os',tx=at,baseline=baseline2,expected=runtime.TOHOST_SHELL_FATAL,readonly=False)
            if raw!=committed or out!=update[:at] or v['tx_fired']!='1' or int(v.get('block_responses','-1'))!=9:
                raise runtime.OwnerFailure('receipt failure continued IO/output or rolled back committed file')
        case('update-receipt-tx-'+label,committed_tx)
    print('metadata-faults: %d passed, %d failed, %d harness errors'%(passed,failed,harness))
    return 2 if harness else 1 if failed else 0
if __name__=='__main__':
    try:sys.exit(main())
    except (runtime.HarnessError,OSError,subprocess.TimeoutExpired) as error:print(':HARNESS-ERROR:',error);sys.exit(2)
