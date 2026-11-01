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

REQUIRED = (
    'tools/mcp_protocol.c', 'tools/mcp_protocol.h', 'tools/mcp_note.h', 'tools/yan_mcp.c',
    'tools/mcp_worker.c', 'tools/mcp_worker.h', 'host/semantic_text.c',
    'host/semantic_text.h', 'host/semantic_sha256.c', 'host/semantic_sha256.h',
    'host/semantic_result.h', 'host/semantic_posix.c', 'host/semantic_posix.h',
    'host/semantic_llama.h', 'host/semantic.h', 'os/memory.h', 'os/sort.h', 'os/yanfs.c', 'os/yanfs.h',
    'host/semantic_corpus.c', 'host/semantic_corpus.h',
    'os/note_blocks.c', 'os/note_blocks.h', 'os/note_blocks_format.c',
    'os/note_blocks_format.h', 'os/note_metadata_format.c',
    'os/note_metadata_format.h', 'os/search_text.c', 'os/search_text.h',
    'os/note_metadata_format_internal.h',
    'host/block_query.c', 'host/block_query.h', 'tools/semantic_json.h',
    'tests/test_mcp_protocol.c', 'tests/test_block_query.c',
    'tests/test_mcp_block_worker.c', 'tests/block_query_probe.c',
    'tests/block_page_oracle.py', 'tests/mcp_fault_checks.py', 'tests/mcp_checks.py',
    'tests/semantic_fake_runner.py', 'tests/semantic_fixture.py',
    'tests/semantic_json_checks.py', 'tests/mcp_fault_hooks.c',
    'tests/run_mcp_mutation.py', 'tests/run_note_metadata_mutation.py',
    'tests/run_yanfs_mutation.py')
BUILD_REQUIRED = (
    'yan_mcp', 'mcp_fault_probe', 'yan_semantic',
    'CMakeFiles/yan_mcp.dir/link.txt', 'CMakeFiles/mcp_fault_probe.dir/link.txt',
    'CMakeFiles/yan_mcp.dir/tools/yan_mcp.c.o',
    'CMakeFiles/yan_mcp.dir/tools/mcp_worker.c.o',
    'CMakeFiles/mcp_fault_probe.dir/tools/yan_mcp.c.o',
    'CMakeFiles/mcp_fault_probe.dir/tools/mcp_worker.c.o',
    'CMakeFiles/mcp_fault_probe.dir/tests/mcp_fault_hooks.c.o',
    'libyan_mcp_protocol.a', 'libyan_semantic_text.a',
    'libyan_block_query.a', 'libyan_semantic_llama.a', 'libyan_semantic_json.a',
    'libyan_semantic_posix.a', 'libyan_blocks.a', 'libyan_blocks_format.a',
    'libyan_search_text.a', 'libyan_fs.a', 'libyan_semantic_core.a',
    'libyan_semantic_corpus.a', 'libyan_note_format.a',
    'libyan_mcp_note.a', 'libyan_note_write.a',
    'libyan_mcp_meta.a', 'libyan_note.a')
SUITE_INPUTS = {
    'mcp_protocol': ('tools/mcp_protocol.c', 'host/semantic_text.c',
                     'host/semantic_sha256.c', 'tools/mcp_worker.c'),
    'block_query': ('host/block_query.c', 'host/semantic_posix.c',
                    'host/semantic_corpus.c'),
    'mcp_block_worker': ('tools/mcp_protocol.c', 'host/semantic_text.c',
                         'host/semantic_sha256.c', 'tools/mcp_worker.c'),
    'mcp_faults': ('tools/yan_mcp.c', 'tools/mcp_worker.c'),
}
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
    ('busy-as-io', 'tools/yan_mcp.c',
     'if (s->task.active) return enqueue(s,&request,NULL,0,"BUSY",false);\n'
     '        if (!start_task(s,&request))',
     'if (s->task.active) return enqueue(s,&request,NULL,0,"IO",false);\n'
     '        if (!start_task(s,&request))', 'mcp_faults',
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
    ('block-page-absolute', 'host/block_query.c',
     'state->page_start = page_start; state->page_end = page_end;',
     'state->page_start = page_start + 1u; state->page_end = page_end;',
     'block_query', 'test_read_pages_and_terminal', 'block_absolute_page_offset'),
    ('block-next-relative', 'host/block_query.c',
     'state->next_offset = state->complete ? 0u : page_end - start;',
     'state->next_offset = state->complete ? 0u : page_end - start + 1u;',
     'block_query', 'test_read_pages_and_terminal', 'block_relative_next_offset'),
    ('block-stale-identity', 'host/block_query.c',
     'if (memcmp(before, after, 32) != 0) return YAN_BLOCK_QUERY_STALE;',
     'if (false && memcmp(before, after, 32) != 0) return YAN_BLOCK_QUERY_STALE;',
     'block_query', 'test_stale_expected_and_source_change', 'block_stale_identity'),
    ('block-preflight-cancel', 'host/block_query.c',
     'if (cancelled(io->cancel)) return YAN_FS_IO_ERROR;\n'
     '    YanFsIoResult result = io->base.read_block(io->base.context, lba, out);\n'
     '    if (result != YAN_FS_IO_OK) return result;\n'
     '    if (cancelled(io->cancel)) return YAN_FS_IO_ERROR;\n'
     '    return YAN_FS_IO_OK;',
     'YanFsIoResult result = io->base.read_block(io->base.context, lba, out);\n'
     '    return result;',
     'block_query', 'test_cancel_interrupts_service_preflight', 'block_preflight_cancel'),
    ('block-limit-map', 'host/block_query.c',
     'case YAN_BLOCK_LIMIT: return YAN_BLOCK_QUERY_LIMIT;',
     'case YAN_BLOCK_LIMIT: return YAN_BLOCK_QUERY_IO;',
     'block_query', 'test_list_64_65_and_duplicate', 'block_limit_map'),
    ('block-branch', 'tools/mcp_worker.c',
     'if (request->kind==YAN_MCP_BLOCK_LIST || request->kind==YAN_MCP_BLOCK_READ ||\n'
     '        request->kind==YAN_MCP_BLOCK_REFS || request->kind==YAN_MCP_BLOCK_BACKLINKS) {',
     'if (false) {', 'mcp_block_worker', 'test_block_worker_no_model',
     'block_worker_no_model_success'),
    ('block-cleanup', 'tools/mcp_worker.c',
     'if (result==YAN_BLOCK_QUERY_OK && closed!=YAN_SEMANTIC_OK)',
     'if (false && closed!=YAN_SEMANTIC_OK)',
     'mcp_faults', 'test_block_cleanup_failure', 'block_cleanup_is_error'),
    ('response-limit-equal', 'tools/mcp_protocol.c',
     'if (encoded_length>YAN_MCP_OUTPUT_MAX)',
     'if (encoded_length>=YAN_MCP_OUTPUT_MAX)',
     'mcp_protocol', 'test_reply_exact_wire_boundary', 'block_response_limit'),
)


def main():
    p = argparse.ArgumentParser()
    for name in ('repo', 'build', 'unity', 'work', 'yyjson-lib'):
        p.add_argument('--'+name, type=Path, required=True)
    p.add_argument('--yyjson-include', required=True)
    p.add_argument('--curl-lib', type=Path, default=Path(''))
    p.add_argument('--cc', default='cc')
    p.add_argument('--only', default='',
                   help='comma-separated mutation labels; empty runs the full group')
    args = p.parse_args()
    args.repo = args.repo.resolve(); args.build = args.build.resolve()
    missing = [n for n in REQUIRED if not (args.repo/n).is_file()]
    missing += [n for n in BUILD_REQUIRED if not (args.build/n).is_file()]
    if missing:
        print('harness: missing repository/implementation inputs:', missing); return 1
    link_inputs = []
    for target in ('yan_mcp', 'mcp_fault_probe'):
        link_path = args.build/'CMakeFiles'/f'{target}.dir'/'link.txt'
        for token in shlex.split(link_path.read_text()):
            if token.endswith(('.o', '.a')) and not Path(token).is_absolute():
                if not (args.build/token).is_file():
                    link_inputs.append(token)
    if link_inputs:
        print('harness: missing CMake relink inputs:', sorted(set(link_inputs))); return 1
    includes = [Path(n) for n in args.yyjson_include.split('|')]
    if (not shutil.which(args.cc) or not (args.unity/'src/unity.c').is_file() or
            not args.yyjson_lib.is_file() or not args.curl_lib.is_file() or
            not any((n/'yyjson.h').is_file() for n in includes)):
        print('SKIP external compiler/Unity/yyjson/curl dependency missing'); return 77
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

    def variant(label, changed=None, old=None, new=None, suites=None):
        source = None
        if changed:
            text = (args.repo/changed).read_text(encoding='utf-8')
            if text.count(old) != 1:
                raise HarnessError('replacement is not unique: '+label)
            source = root/(label+'-'+Path(changed).name)
            source.write_text(text.replace(old, new, 1), encoding='utf-8')
        wanted = (set(suites) if suites is not None else
                  {'mcp_protocol', 'block_query', 'mcp_block_worker', 'mcp_faults'})
        results = {}
        native_libs = []
        if {'mcp_protocol', 'mcp_block_worker'} & wanted:
            native_libs = [args.build/n for n in (
                'libyan_block_query.a', 'libyan_semantic_llama.a', 'libyan_semantic_json.a')]
            native_libs += [args.yyjson_lib, unity,
                args.build/'libyan_mcp_note.a', args.build/'libyan_note_write.a',
                args.build/'libyan_mcp_meta.a', args.build/'libyan_note.a',
                *(args.build/n for n in ('libyan_semantic_posix.a', 'libyan_blocks.a',
                    'libyan_blocks_format.a', 'libyan_search_text.a', 'libyan_fs.a')),
                args.curl_lib,
                *(args.build/n for n in ('libyan_semantic_core.a', 'libyan_semantic_corpus.a',
                    'libyan_note_format.a')), '-lm']
        if 'mcp_protocol' in wanted:
            native = root/(label+'-native')
            native_sources = [source if n == changed else args.repo/n for n in (
                'tools/mcp_protocol.c', 'host/semantic_text.c', 'host/semantic_sha256.c',
                'tools/mcp_worker.c')]
            compile_command([*flags, *map(str, native_sources),
                             str(args.repo/'tests/test_mcp_protocol.c'), *map(str, native_libs),
                             '-o', str(native)], label+'-native')
            results['mcp_protocol'] = run([str(native)], label+'-native')
        if 'block_query' in wanted:
            block_native = root/(label+'-block')
            block_set = ('host/block_query.c', 'host/semantic_posix.c', 'host/semantic_corpus.c')
            block_sources = [source if n == changed else args.repo/n for n in (
                'tests/test_block_query.c', 'host/block_query.c', 'host/semantic_posix.c',
                'host/semantic_corpus.c', 'host/semantic_sha256.c', 'host/semantic_text.c',
                'os/yanfs.c', 'os/note_blocks.c', 'os/note_blocks_format.c',
                'os/note_metadata_format.c', 'os/search_text.c')]
            if changed in block_set and source is None:
                raise HarnessError('block source replacement missing: '+label)
            compile_command([*flags, *map(str, block_sources), str(unity), '-lm',
                             '-o', str(block_native)], label+'-block')
            results['block_query'] = run([str(block_native)], label+'-block')
        if 'mcp_block_worker' in wanted:
            block_worker = root/(label+'-block-worker')
            worker_sources = [source if n == changed else args.repo/n for n in (
                'tools/mcp_protocol.c', 'host/semantic_text.c', 'host/semantic_sha256.c',
                'tools/mcp_worker.c')]
            compile_command([*flags, *map(str, worker_sources),
                             str(args.repo/'tests/test_mcp_block_worker.c'),
                             *map(str, native_libs), '-o', str(block_worker)],
                            label+'-block-worker')
            results['mcp_block_worker'] = run([str(block_worker)], label+'-block-worker')
        if 'mcp_faults' in wanted:
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
            results['mcp_faults'] = run([sys.executable, '-B', str(args.repo/'tests/mcp_fault_checks.py'),
                                         *map(str,binaries), str(args.build/'yan_semantic'),
                                         str(args.repo/'tests/semantic_fake_runner.py')], label+'-faults')
        return results

    selected = set(filter(None, args.only.split(',')))
    known = {m[0] for m in MUTATIONS}
    if selected - known:
        raise HarnessError('unknown mutation labels: '+','.join(sorted(selected-known)))
    selected_suites = ({m[4] for m in MUTATIONS if m[0] in selected}
                       if selected else None)
    baseline = variant('baseline', suites=selected_suites)
    for suite, result in baseline.items():
        if classify_run(*result,None)!='pass' or parse_unity(result[1])[2]:
            raise HarnessError('baseline must be green/complete/unignored: '+suite)
    if selected_suites is None or 'mcp_faults' in selected_suites:
        no_effect = variant('no-effect', 'tools/yan_mcp.c', 'if (!t->active) return;',
                            'if (!(t->active)) return;', {'mcp_faults'})
        if classify_suites(no_effect,'absent','absent','mcp_faults')!='pass':
            raise HarnessError('no-effect control credited or harness failed')
    for label, changed, old, new, suite, owner, assertion in MUTATIONS:
        if selected and label not in selected:
            continue
        if changed not in SUITE_INPUTS[suite]:
            raise HarnessError('mutation input is not freshly compiled by %s: %s (%s)' %
                               (suite, label, changed))
        result = variant(label,changed,old,new,{suite})
        if classify_suites(result,owner,assertion,suite)!='owner-fail':
            raise HarnessError('designated assertion did not detect: '+label)
        if classify_suites(result,'absent_owner',assertion,suite)!='wrong-owner':
            raise HarnessError('wrong-owner credited: '+label)
        if classify_suites(result,owner,'absent_assertion',suite)!='wrong-assertion':
            raise HarnessError('wrong-assertion credited: '+label)
        print('DETECTED',label,suite,owner,assertion,flush=True)
    count = len(selected) if selected else len(MUTATIONS)
    controls = ('baseline/no-effect' if (selected_suites is None or 'mcp_faults' in selected_suites)
                else 'baseline')
    print('PASS',count,'actual MCP defects;',
          controls+'/wrong-owner/wrong-assertion controls; logs',root)
    return 0


if __name__=='__main__':
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.TimeoutExpired, OSError) as exc:
        print('harness:',exc); raise SystemExit(1)
