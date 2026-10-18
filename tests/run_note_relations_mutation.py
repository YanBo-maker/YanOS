"""Actual relation defects must fail their designated suite, owner and assertion."""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from run_note_metadata_mutation import classify_suites
from run_yanfs_mutation import classify_run, parse_unity, HarnessError

CORE_SOURCES = ('note_relations.c', 'note_metadata_format.c', 'yanfs.c')
SHELL_SOURCES = CORE_SOURCES + ('note_blocks.c', 'note_blocks_format.c', 'shell.c', 'note_metadata.c', 'search.c', 'search_linear.c',
    'search_terms.c', 'search_terms_linear.c', 'search_terms_index.c', 'search_terms_core.c', 'search_text.c')
REQUIRED = tuple('os/' + name for name in SHELL_SOURCES) + tuple(
    'os/' + name.replace('.c', '.h') for name in SHELL_SOURCES) + (
    'os/note_relations.h', 'os/note_metadata_format.h', 'os/yanfs.h', 'os/memory.h',
    'tests/test_note_relations.c', 'tests/run_note_relations_mutation.py',
    'tests/run_note_metadata_mutation.py', 'tests/run_yanfs_mutation.py', 'tests/test_shell_relations.c')

# label, unique old/new production text, exact Unity owner and assertion.
MUTATIONS = (
    ('edge-order', 'return left_score < right_score;', 'return left_score > right_score;',
     'relation_related_bidirectional_outranks_shared_tags', 'relation_related_bidirectional_first'),
    ('tag-order', 'return left->shared_tags < right->shared_tags;', 'return left->shared_tags > right->shared_tags;',
     'relation_related_uses_shared_tags_before_ascii_for_equal_edges', 'relation_secondary_more_tags_first'),
    ('ascii-order', 'return name_compare(left->name, right->name) > 0;', 'return name_compare(left->name, right->name) < 0;',
     'relation_related_breaks_ties_by_ascii_filename', 'relation_related_tiebreak_ascii_first'),
    ('dangling-exists', 'context->source_info.link_exists[i] = false; /* DANGLING */',
     'context->source_info.link_exists[i] = true; /* defective existence */',
     'relation_links_declaration_order_dangling_and_self', 'relation_links_dangling_exists_false'),
    ('missing-source-success', 'case YAN_FS_NOT_FOUND: return YAN_NOTE_NOT_FOUND;',
     'case YAN_FS_NOT_FOUND: return YAN_NOTE_OK;',
     'relation_links_missing_source_is_not_created', 'relation_links_missing_source_not_found'),
    ('io-swallowed', 'case YAN_FS_IO: return YAN_NOTE_IO;', 'case YAN_FS_IO: return YAN_NOTE_OK;',
     'relation_prefix_io_error_propagates_and_sticks', 'relation_prefix_io_propagates'),
    ('protocol-swallowed', 'case YAN_FS_PROTOCOL: return YAN_NOTE_PROTOCOL;',
     'case YAN_FS_PROTOCOL: return YAN_NOTE_OK;',
     'relation_prefix_protocol_error_propagates', 'relation_prefix_protocol_propagates'),
    ('shared-under-count', 'shared = (uint8_t)(shared + 1u);', 'shared = (uint8_t)(shared + 0u);',
     'relation_related_counts_sixteen_shared_tags', 'relation_related_sixteen_tags_value'),
    ('self-included', 'if (name_compare(info.name, name_copy) == 0) {\n                continue;',
     'if (false && name_compare(info.name, name_copy) == 0) {\n                continue;',
     'relation_related_excludes_self_and_dangling', 'relation_related_self_dangling_no_rows'),
    ('cap-nineteen', ': YAN_RELATION_RELATED_SHOWN;', ': YAN_RELATION_RELATED_SHOWN - 1u;',
     'relation_related_shows_twenty_with_the_full_total', 'relation_related_top20_rows'),
    ('truncated-total', 'summary->total = context->row_count;\n        summary->shown = shown;\n        summary->target_exists = true;',
     'summary->total = shown;\n        summary->shown = shown;\n        summary->target_exists = true;',
     'relation_related_shows_twenty_with_the_full_total', 'relation_related_top20_full_total'),
    ('stop-success', 'if (!sink(sink_context, &row)) {\n                result = YAN_NOTE_STOPPED;',
     'if (!sink(sink_context, &row)) {\n                result = YAN_NOTE_OK;',
     'relation_sink_stop_reports_stopped_without_summary', 'relation_links_stop_reports_stopped'),
    ('target-head-parsed', 'context->source_info.link_exists[i] = true;',
     'context->source_info.link_exists[i] = true;\n                result = read_head(context, (const char *)context->source_info.link[i], &context->candidate_info);\n                if (result != YAN_NOTE_OK) break;',
     'relation_links_ignores_a_broken_target_head', 'relation_links_bad_target_head_ok'),
    ('skip-late-bad-head', 'result = read_head(context, info.name, &context->candidate_info);\n        if (result != YAN_NOTE_OK) {\n            break;\n        }',
     'result = read_head(context, info.name, &context->candidate_info);\n        if (result == YAN_NOTE_METADATA) { result = YAN_NOTE_OK; continue; }\n        if (result != YAN_NOTE_OK) {\n            break;\n        }',
     'relation_backlinks_late_bad_header_reserves_everything', 'relation_backlinks_late_bad_header_metadata'),
)

SHELL_MUTATIONS = (
    ('shell-link-status', 'row->exists ? " OK\\r\\n" : " DANGLING\\r\\n"',
     'row->exists ? " DANGLING\\r\\n" : " OK\\r\\n"',
     'shell_links_exact_golden', 'links_golden_bytes'),
    ('shell-backlink-target', 'summary.target_exists ? "OK\\r\\n"',
     'summary.target_exists ? "DANGLING\\r\\n"',
     'shell_backlinks_exact_golden', 'backlinks_golden_bytes'),
    ('shell-top20-total', '!shell_put_decimal(shell, summary.total) ||\n        !shell_puts(shell, " shown=")',
     '!shell_put_decimal(shell, summary.shown) ||\n        !shell_puts(shell, " shown=")',
     'shell_related_top20_full_total', 'related_top20_bytes'),
    ('shell-stop-nonfatal', '/* The callback refused a byte: stop now, with no receipt and no retry. */\n        return YAN_SHELL_FATAL;',
     '/* defective ignored output failure */\n        return YAN_SHELL_OK;',
     'shell_relations_output_failure_is_fatal', 'output_failure_first'),
)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--repo', type=Path, required=True)
    parser.add_argument('--unity', type=Path, required=True)
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--work', type=Path, required=True)
    args = parser.parse_args()
    missing = [name for name in REQUIRED if not (args.repo / name).is_file()]
    if missing:
        print('harness: missing repository inputs:', missing)
        return 1
    if not shutil.which(args.cc) or not (args.unity / 'src/unity.c').is_file():
        print('SKIP: compiler or Unity unavailable')
        return 77
    args.work.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='relations-', dir=args.work))
    unity = root / 'unity.o'
    built = subprocess.run([args.cc, '-std=c17', '-I', str(args.unity / 'src'), '-c',
                            str(args.unity / 'src/unity.c'), '-o', str(unity)],
                           capture_output=True, timeout=90)
    if built.returncode:
        raise HarnessError('Unity compilation failed')

    def variant(label, old=None, new=None, suite='relation'):
        sources = []
        changed = 'note_relations.c' if suite == 'relation' else 'shell.c'
        for filename in CORE_SOURCES if suite == 'relation' else SHELL_SOURCES:
            source = args.repo / 'os' / filename
            if filename == changed and old is not None:
                text = source.read_text(encoding='utf-8')
                if text.count(old) != 1:
                    raise HarnessError('replacement is not unique: ' + label)
                source = root / (label + '.c')
                source.write_text(text.replace(old, new, 1), encoding='utf-8')
            sources.append(str(source))
        binary = root / label
        built = subprocess.run([args.cc, '-std=c17', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                                '-I', str(args.repo / 'os'), '-I', str(args.unity / 'src'),
                                *sources, str(args.repo / ('tests/test_note_relations.c' if suite == 'relation' else 'tests/test_shell_relations.c')),
                                str(unity), '-o', str(binary)], capture_output=True, timeout=90)
        (root / (label + '-build.log')).write_bytes(built.stdout + built.stderr)
        if built.returncode:
            raise HarnessError('compilation failed: ' + label)
        try:
            ran = subprocess.run([str(binary)], capture_output=True, timeout=30)
            result = ran.returncode, ran.stdout, ran.stderr
        except subprocess.TimeoutExpired:
            result = None, b'', b''
        (root / (label + '.log')).write_bytes(result[1] + result[2])
        return result

    baseline = variant('baseline')
    if classify_run(*baseline, None) != 'pass' or parse_unity(baseline[1])[2]:
        raise HarnessError('baseline must be complete, green, unignored')
    shell_baseline = variant('baseline-shell', suite='shell')
    if classify_run(*shell_baseline, None) != 'pass' or parse_unity(shell_baseline[1])[2]:
        raise HarnessError('shell baseline must be complete, green, unignored')
    no_effect = variant('no-effect', 'return left_score < right_score;', 'return (left_score) < right_score;')
    if classify_run(*no_effect, MUTATIONS[0][3], MUTATIONS[0][4]) != 'pass':
        raise HarnessError('no-effect control credited')
    for suite, mutation in [('relation', item) for item in MUTATIONS] + [('shell', item) for item in SHELL_MUTATIONS]:
        label, old, new, owner, assertion = mutation
        result = variant(label, old, new, suite)
        if classify_suites({suite: result}, owner, assertion, suite) != 'owner-fail':
            raise HarnessError('designated assertion did not detect: ' + label)
        if classify_run(*result, 'absent_owner', assertion) != 'wrong-owner':
            raise HarnessError('wrong owner credited: ' + label)
        if classify_run(*result, owner, 'absent_assertion') != 'wrong-assertion':
            raise HarnessError('wrong assertion credited: ' + label)
        print('DETECTED', label, suite, owner, assertion, flush=True)
    print('PASS:', len(MUTATIONS) + len(SHELL_MUTATIONS), 'actual relation mutations; designated assertions; logs', root)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (RuntimeError, subprocess.TimeoutExpired, OSError) as error:
        print('harness:', error)
        sys.exit(1)
