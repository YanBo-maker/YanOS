"""Actual MCP defects: exact suite/owner/assertion, with harness-first controls.
Only named repository inputs and existing named CMake artifacts are used;
no workspace copy, production binary replacement, or repository mutation.
"""
import argparse
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
from run_note_metadata_mutation import classify_suites
from run_yanfs_mutation import classify_run, parse_unity, HarnessError

REQUIRED = ('tools/mcp_protocol.c', 'tools/mcp_protocol.h', 'tools/yan_mcp.c',
            'tools/mcp_worker.c', 'tools/mcp_worker.h', 'host/semantic_text.c',
            'host/semantic_text.h', 'host/semantic_result.h', 'host/semantic_posix.h',
            'host/semantic_llama.h', 'host/semantic.h', 'os/yanfs.h',
            'host/semantic_corpus.h', 'host/semantic_sha256.h', 'os/note_metadata_format.h',
            'tools/semantic_json.h',
            'tests/test_mcp_protocol.c', 'tests/mcp_fault_checks.py', 'tests/mcp_checks.py',
            'tests/semantic_fake_runner.py', 'tests/semantic_fixture.py',
            'tests/semantic_json_checks.py', 'tests/mcp_fault_hooks.c',
            'tests/run_mcp_mutation.py', 'tests/run_note_metadata_mutation.py',
            'tests/run_yanfs_mutation.py')
BUILD_REQUIRED = ('yan_mcp', 'mcp_fault_probe', 'yan_semantic',
                  'CMakeFiles/yan_mcp.dir/link.txt', 'CMakeFiles/mcp_fault_probe.dir/link.txt')
# label, source, exact anchor, replacement, suite, owner, assertion
MUTATIONS = (
    ('typed-id', 'tools/mcp_protocol.c', 'a->string==b->string &&\n'
     '        (a->string ? a->length==b->length && !memcmp(a->text,b->text,a->length) : a->number==b->number)', 'true',
     'mcp_protocol', 'test_ids_are_typed_and_current_id_cannot_be_reused', 'mcp_typed_id_cancel'),
    ('duplicate-key', 'tools/mcp_protocol.c', 'if (!unique(root)) goto done;',
     'if (false && !unique(root)) goto done;', 'mcp_protocol',
     'test_duplicate_keys_are_compared_after_unescaping', 'mcp_duplicate_decoded_key'),
    ('depth-seventeen', 'tools/mcp_protocol.c', '++depth>16', '++depth>17',
     'mcp_protocol', 'test_depth_is_bounded_before_json_decode', 'mcp_depth_limit'),
    ('missing-caps', 'tools/mcp_protocol.c', '!caps_valid(caps,false)', '(false && !caps_valid(caps,false))',
     'mcp_protocol', 'test_modern_required_metadata_and_version_errors', 'mcp_modern_required_caps'),
    ('extra-query-field', 'tools/mcp_protocol.c', 'yyjson_obj_size(args)!=1', 'yyjson_obj_size(args)<1',
     'mcp_protocol', 'test_business_arguments_fail_without_dispatch', 'mcp_extra_business_args'),
    ('utf8-byte-limit', 'host/semantic_text.c', 'length > YAN_SEMANTIC_QUERY_MAX',
     'length > YAN_SEMANTIC_QUERY_MAX + 1u', 'mcp_protocol',
     'test_utf8_query_limit_is_bytes_and_nul_cannot_bypass_it', 'mcp_query_utf8_byte_limit'),
    ('busy-as-io', 'tools/yan_mcp.c', 'enqueue(s,&request,NULL,0,"BUSY",false)',
     'enqueue(s,&request,NULL,0,"IO",false)', 'mcp_faults',
     'test_busy_does_not_queue_second_task', 'mcp_busy_not_queued'),
    ('cancel-response', 'tools/yan_mcp.c', 'if (cancelled) t->cancelled=true;', '(void)cancelled;',
     'mcp_faults', 'test_cancel_suppresses_response_and_reaps', 'mcp_cancel_no_response'),
    ('inherited-stdin', 'tools/yan_mcp.c', 'dup2(null_fd,STDIN_FILENO)<0', 'false',
     'mcp_faults', 'test_runner_stdio_and_private_pipe_isolation', 'mcp_runner_fd_isolation'),
    ('ignore-worker-exit', 'tools/yan_mcp.c', 'WIFEXITED(t->status) && WEXITSTATUS(t->status)==0 &&',
     '', 'mcp_faults', 'test_worker_exit_checked', 'mcp_worker_exit_checked'),
    ('expired-write', 'tools/yan_mcp.c', 'int64_t now=now_ms();\n    if (now<0 || now>=frame->deadline) return false;',
     'int64_t now=now_ms();\n    if (now<0) return false;', 'mcp_faults',
     'test_expired_writable_frame_cannot_escape_deadline', 'mcp_expired_output_not_written'),
    ('ignore-cleanup', 'tools/mcp_worker.c', 'if (result==YAN_SEMANTIC_OK) result=closed;',
     '(void)closed;', 'mcp_faults', 'test_adapter_cleanup_failure', 'mcp_cleanup_checked'),
    ('ignore-stop', 'tools/mcp_worker.c', 'if (result==YAN_SEMANTIC_OK) result=stopped;',
     '(void)stopped;', 'mcp_faults', 'test_adapter_stop_failure', 'mcp_stop_checked'),
    ('inherit-child-exit-ignore', 'tools/yan_mcp.c', 'ready=ready && sigaction(SIGCHLD,&action,NULL)==0;',
     '(void)action;', 'mcp_faults', 'test_inherited_signal_state_is_owned_by_server',
     'mcp_child_exit_stays_waitable'),
    ('inherit-blocked-control-signals', 'tools/yan_mcp.c', 'ready=ready && sigprocmask(SIG_UNBLOCK,&controlled,NULL)==0;',
     '(void)controlled;', 'mcp_faults', 'test_inherited_signal_state_is_owned_by_server',
     'mcp_control_signals_unblocked'),
)


def main():
    p = argparse.ArgumentParser()
    for name in ('repo', 'build', 'unity', 'work', 'yyjson-lib'):
        p.add_argument('--'+name, type=Path, required=True)
    p.add_argument('--yyjson-include', required=True)
    p.add_argument('--cc', default='cc')
    args = p.parse_args()
    args.repo = args.repo.resolve(); args.build = args.build.resolve()
    missing = [n for n in REQUIRED if not (args.repo/n).is_file()]
    missing += [n for n in BUILD_REQUIRED if not (args.build/n).is_file()]
    if missing:
        print('harness: missing repository/implementation inputs:', missing); return 1
    includes = [Path(n) for n in args.yyjson_include.split('|')]
    if (not shutil.which(args.cc) or not (args.unity/'src/unity.c').is_file() or
            not args.yyjson_lib.is_file() or not any((n/'yyjson.h').is_file() for n in includes)):
        print('SKIP external compiler/Unity/yyjson dependency missing'); return 77
    args.work.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='mcp-', dir=args.work)).resolve()
    flags = [args.cc, '-std=c17', '-Wall', '-Wextra', '-Wpedantic', '-Werror']
    for directory in (args.repo/'host', args.repo/'os', args.repo/'tools', args.unity/'src', *includes):
        flags += ['-I', str(directory)]

    def compile_command(command, label, cwd=None):
        built = subprocess.run(command, capture_output=True, timeout=90, cwd=cwd)
        (root/(label+'-build.log')).write_bytes(built.stdout+built.stderr)
        if built.returncode:
            raise HarnessError('compilation failed: '+label)

    unity = root/'unity.o'
    compile_command([*flags, '-c', str(args.unity/'src/unity.c'), '-o', str(unity)], 'unity')

    def run(command, label):
        try:
            result = subprocess.run(command, capture_output=True, timeout=60)
            value = result.returncode, result.stdout, result.stderr
        except subprocess.TimeoutExpired:
            value = None, b'', b''
        (root/(label+'.log')).write_bytes(value[1]+value[2])
        return value

    def variant(label, changed=None, old=None, new=None):
        source = None
        if changed:
            text = (args.repo/changed).read_text(encoding='utf-8')
            if text.count(old) != 1:
                raise HarnessError('replacement is not unique: '+label)
            source = root/(label+'-'+Path(changed).name)
            source.write_text(text.replace(old, new, 1), encoding='utf-8')
        native = root/(label+'-native')
        native_sources = [source if n == changed else args.repo/n
                          for n in ('tools/mcp_protocol.c', 'host/semantic_text.c')]
        compile_command([*flags, *map(str, native_sources), str(args.repo/'tests/test_mcp_protocol.c'),
                         str(unity), str(args.yyjson_lib), '-lm', '-o', str(native)], label+'-native')
        binaries = []
        for target in ('yan_mcp', 'mcp_fault_probe'):
            if changed not in ('tools/yan_mcp.c', 'tools/mcp_worker.c'):
                binaries.append(args.build/target); continue
            obj = root/(label+'-'+target+'.o')
            defines = ['-D_exit=yan_mcp_test_exit', '-Dclock_gettime=yan_mcp_test_clock_gettime'] if target=='mcp_fault_probe' else []
            compile_command([*flags, *defines, '-c', str(source), '-o', str(obj)], label+'-'+target+'-object')
            link = shlex.split((args.build/'CMakeFiles'/f'{target}.dir/link.txt').read_text())
            anchor = f'CMakeFiles/{target}.dir/{changed}.o'
            if link.count(anchor)!=1 or link.count('-o')!=1:
                raise HarnessError('unexpected CMake link inputs: '+target)
            link[link.index(anchor)] = str(obj)
            binary = root/(label+'-'+target)
            link[link.index('-o')+1] = str(binary)
            compile_command(link, label+'-'+target+'-link', args.build)
            binaries.append(binary)
        return {'mcp_protocol': run([str(native)], label+'-native'),
                'mcp_faults': run([sys.executable, '-B', str(args.repo/'tests/mcp_fault_checks.py'),
                                  *map(str,binaries), str(args.build/'yan_semantic'),
                                  str(args.repo/'tests/semantic_fake_runner.py')], label+'-faults')}

    baseline = variant('baseline')
    for suite, result in baseline.items():
        if classify_run(*result,None)!='pass' or parse_unity(result[1])[2]:
            raise HarnessError('baseline must be green/complete/unignored: '+suite)
    no_effect = variant('no-effect', 'tools/yan_mcp.c', 'if (!t->active) return;', 'if (!(t->active)) return;')
    if classify_suites(no_effect,'absent','absent','mcp_faults')!='pass':
        raise HarnessError('no-effect control credited or harness failed')
    for label, changed, old, new, suite, owner, assertion in MUTATIONS:
        result = variant(label,changed,old,new)
        if classify_suites(result,owner,assertion,suite)!='owner-fail':
            raise HarnessError('designated assertion did not detect: '+label)
        if classify_suites(result,'absent_owner',assertion,suite)!='wrong-owner':
            raise HarnessError('wrong-owner credited: '+label)
        if classify_suites(result,owner,'absent_assertion',suite)!='wrong-assertion':
            raise HarnessError('wrong-assertion credited: '+label)
        print('DETECTED',label,suite,owner,assertion,flush=True)
    print('PASS',len(MUTATIONS),'actual MCP defects; baseline/no-effect/wrong-owner/wrong-assertion controls; logs',root)
    return 0


if __name__=='__main__':
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.TimeoutExpired, OSError) as exc:
        print('harness:',exc); raise SystemExit(1)
