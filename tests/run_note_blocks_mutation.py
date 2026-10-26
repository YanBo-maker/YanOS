"""Actual 0032 defects: exact suite, Unity owner and assertion are required.

Repository inputs are explicitly declared and copied individually. Missing
tested inputs fail before compiler/Unity skips. Default execution covers all
three suites; --suites is only for a named, partial development check.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

from run_note_metadata_mutation import classify_suites
from run_yanfs_mutation import HarnessError, classify_run, parse_unity

FORMAT_SOURCES = ('note_blocks_format.c', 'note_metadata_format.c', 'search_text.c')
SERVICE_SOURCES = FORMAT_SOURCES + ('note_blocks.c', 'yanfs.c')
SHELL_SOURCES = SERVICE_SOURCES + ('shell.c', 'note_relations.c', 'note_metadata.c',
    'search.c', 'search_linear.c', 'search_terms.c', 'search_terms_linear.c',
    'search_terms_index.c', 'search_terms_core.c')
GROUPS = {
    'format': ('tests/test_note_blocks_format.c', FORMAT_SOURCES),
    'service': ('tests/test_note_blocks.c', SERVICE_SOURCES),
    'shell': ('tests/test_shell_blocks.c', SHELL_SOURCES),
}
UNITY_FILES = ('src/unity.c', 'src/unity.h', 'src/unity_internals.h')
STRICT_FLAGS = ('-std=c17', '-Wall', '-Wextra', '-Wpedantic', '-Werror')


def required_inputs(suites):
    """Return only declared dependencies, preserving their deterministic order."""
    inputs = ['tests/run_note_blocks_mutation.py',
              'tests/run_note_metadata_mutation.py', 'tests/run_yanfs_mutation.py',
              'tests/fs_test_support.h', 'os/memory.h',
              'os/note_metadata_format.h', 'os/note_metadata.h',
              'os/note_metadata_format_internal.h',
              'os/note_relations.h', 'os/note_blocks_format.h',
              'os/note_blocks.h', 'os/search_terms.h',
              'os/search_terms_core.h', 'os/search_terms_linear.h',
              'os/search_terms_index.h']
    for suite in suites:
        test, sources = GROUPS[suite]
        inputs.append(test)
        for source in sources:
            inputs.extend(('os/' + source, 'os/' + source.replace('.c', '.h')))
            if source in ('yanfs.c', 'note_blocks.c', 'note_metadata.c', 'note_relations.c'):
                inputs.append('os/sort.h')
    return tuple(dict.fromkeys(inputs))


REQUIRED = required_inputs(GROUPS)


def copy_inputs(repo, destination, required):
    """Copy an explicit manifest; never enumerate repository directories."""
    for name in required:
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(repo / name, target)


# label, suite, production file, unique old/new source, exact owner/assertion.
FORMAT_MUTATIONS = (
    ('text-c1-accepted', 'format', 'note_blocks_format.c',
     '(scalar >= 0x7fu && scalar <= 0x9fu)',
     '(scalar >= 0x7fu && scalar <= 0x7fu)',
     'format_text_controls_and_bare_cr', 'format_text_c1_rejected'),
    ('text-final-bare-cr-accepted', 'format', 'note_blocks_format.c',
     'validator->utf8_length != 0u || validator->pending_cr',
     'validator->utf8_length != 0u',
     'format_text_controls_and_bare_cr', 'format_text_c0_del_rejected'),
    ('trailing-lf-phantom-line', 'format', 'note_blocks_format.c',
     'has_final_line ? parser->line : parser->line - 1u', 'parser->line',
     'format_trailing_lf_does_not_invent_line', 'format_trailing_lf_last_actual_line'),
    ('empty-block-line-not-zero', 'format', 'note_blocks_format.c',
     'block->first_line = block->start == end ? 0u : block->marker_line + 1u;',
     'block->first_line = block->marker_line + 1u;',
     'format_empty_blocks_and_unterminated_marker', 'format_empty_block_first_zero'),
    ('capacity-hides-duplicate', 'format', 'note_blocks_format.c',
     "parser->blocks[i].id[id_length] == '\\0' &&",
     "parser->block_count < YAN_BLOCK_MAX_COUNT &&\n            parser->blocks[i].id[id_length] == '\\0' &&",
     'format_duplicate_precedes_capacity', 'format_duplicate_before_capacity'),
    ('tab-marker-separator-accepted', 'format', 'note_blocks_format.c',
     "parser->line_prefix[6] != (uint8_t)' ' ||",
     "(parser->line_prefix[6] != (uint8_t)' ' &&\n         parser->line_prefix[6] != (uint8_t)'\\t') ||",
     'format_reserved_marker_errors', 'format_reserved_marker_bad_syntax'),
    ('named-reference-loses-source', 'format', 'note_blocks_format.c',
     'parser->reference.has_source_id = parser->block_count != 0u;',
     'parser->reference.has_source_id = false;',
     'format_references_without_blocks_and_source_identity',
     'format_named_reference_has_source_block'),
    ('nested-bracket-start-dropped', 'format', 'note_blocks_format.c',
     "if (parser->last_byte == (uint8_t)'[') {",
     "if (false && parser->last_byte == (uint8_t)'[') {",
     'format_nested_refs_follow_left_to_right', 'format_nested_refs_complete_substrings'),
)

SERVICE_MUTATIONS = (
    ('reference-total-doubled', 'service', 'note_blocks.c',
     '++c->total;', 'c->total += 2u;',
     'blocks_refs_no_marker_and_unlimited_count', 'blocks_refs_full_total'),
    ('reference-top19', 'service', 'note_blocks.c',
     'if (c->row_count < YAN_BLOCK_REFS_SHOWN) {',
     'if (c->row_count < YAN_BLOCK_REFS_SHOWN - 1u) {',
     'blocks_refs_no_marker_and_unlimited_count', 'blocks_refs_top20'),
    ('backlink-ascii-reversed', 'service', 'note_blocks.c',
     'name_compare(blocks->names[left], blocks->names[right]) < 0',
     'name_compare(blocks->names[left], blocks->names[right]) > 0',
     'blocks_backlinks_sort_occurrences_and_skips', 'blocks_backlinks_ascii_first'),
    ('binary-skip-doubled', 'service', 'note_blocks.c',
     '++c->skipped; r = YAN_BLOCK_OK;', 'c->skipped += 2u; r = YAN_BLOCK_OK;',
     'blocks_backlinks_sort_occurrences_and_skips', 'blocks_backlinks_binary_skip'),
    ('read-io-swallowed', 'service', 'note_blocks.c',
     'case YAN_FS_IO: return YAN_BLOCK_IO;', 'case YAN_FS_IO: return YAN_BLOCK_OK;',
     'blocks_preflight_io_propagates', 'blocks_io_error'),
    ('bad-header-ignored', 'service', 'note_blocks.c',
     'if (note != YAN_NOTE_OK) return note == YAN_NOTE_METADATA',
     'if (false && note != YAN_NOTE_OK) return note == YAN_NOTE_METADATA',
     'blocks_missing_source_and_bad_header', 'blocks_bad_header_error'),
    ('invalid-text-ignored', 'service', 'note_blocks.c',
     'if (r == YAN_BLOCK_OK) r = yan_block_text_finish(&c->text);',
     'if (r == YAN_BLOCK_OK) r = yan_block_text_finish(&c->text);\n    if (r == YAN_BLOCK_INVALID_TEXT) r = YAN_BLOCK_OK;',
     'blocks_text_precedes_head_and_structure', 'blocks_source_text_priority'),
    ('shown-target-ambiguity-ignored', 'service', 'note_blocks.c',
     'r = target(c, &c->rows[i].reference.target, &c->rows[i].state);',
     'r = target(c, &c->rows[i].reference.target, &c->rows[i].state);\n    if (r == YAN_BLOCK_AMBIGUOUS) r = YAN_BLOCK_OK;',
     'blocks_refs_bad_shown_target_no_rows', 'blocks_refs_shown_bad_error'),
    ('premature-reference-output', 'service', 'note_blocks.c',
     'r = file_scan(c, key.name, collect);\n    for (uint32_t i = 0;',
     'r = file_scan(c, key.name, collect);\n    if (r == YAN_BLOCK_OK) (void)emit(c, sink, opaque);\n    for (uint32_t i = 0;',
     'blocks_refs_bad_shown_target_no_rows', 'blocks_refs_no_partial_rows'),
    ('callback-stop-success', 'service', 'note_blocks.c',
     'if (!sink(opaque, &c->rows[i])) return YAN_BLOCK_STOPPED;',
     'if (!sink(opaque, &c->rows[i])) return YAN_BLOCK_OK;',
     'blocks_stop_and_reentry_preserve_summary', 'blocks_stop_result'),
    ('protocol-error-relabelled-io', 'service', 'note_blocks.c',
     'case YAN_FS_PROTOCOL: return YAN_BLOCK_PROTOCOL;',
     'case YAN_FS_PROTOCOL: return YAN_BLOCK_IO;',
     'blocks_protocol_error_remains_fatal_class', 'blocks_protocol_error'),
    ('show-callback-after-read-error', 'service', 'note_blocks.c',
     'if (r == YAN_BLOCK_OK && !chunk(opaque, offset, c->buffer, amount)) r = YAN_BLOCK_STOPPED;',
     'if ((r == YAN_BLOCK_OK || r == YAN_BLOCK_IO) && !chunk(opaque, offset, c->buffer, amount)) r = YAN_BLOCK_STOPPED;',
     'blocks_show_late_io_preserves_prefix_without_summary', 'blocks_show_late_io_prefix'),
    ('show-chunk-stop-ignored', 'service', 'note_blocks.c',
     'if (r == YAN_BLOCK_OK && !chunk(opaque, offset, c->buffer, amount)) r = YAN_BLOCK_STOPPED;',
     'if (r == YAN_BLOCK_OK && !chunk(opaque, offset, c->buffer, amount)) r = YAN_BLOCK_OK;',
     'blocks_show_chunk_stop_prevents_more_io', 'blocks_show_chunk_stop'),
)

SHELL_MUTATIONS = (
    ('shell-reference-kind-reversed', 'shell', 'shell.c',
     'out->command == BLOCK_REFS ? "REF source=" : "BACKLINK source="',
     'out->command == BLOCK_REFS ? "BACKLINK source=" : "REF source="',
     'shell_blocks_refs_and_backlinks_golden', 'blocks_refs_golden'),
    ('shell-total-truncated-to-shown', 'shell', 'shell.c',
     '!shell_put_decimal64(shell, summary.total)',
     '!shell_put_decimal64(shell, summary.shown)',
     'shell_blocks_refs_top20_total', 'blocks_shell_full_total'),
    ('shell-stopped-is-success', 'shell', 'shell.c',
     'if (result == YAN_BLOCK_STOPPED) return YAN_SHELL_FATAL;\n    if (out.line_open',
     'if (result == YAN_BLOCK_STOPPED) return YAN_SHELL_OK;\n    if (out.line_open',
     'shell_blocks_output_failures_stop_without_io', 'blocks_output_fatal'),
    ('shell-range-end-is-start', 'shell', 'shell.c',
     'shell_put_decimal(s, b->end)', 'shell_put_decimal(s, b->start)',
     'shell_blocks_list_and_show_golden', 'blocks_list_golden'),
    ('shell-line-alias-guard-disabled', 'shell', 'shell.c',
     'if (shell->blocks != NULL &&\n            ranges_overlap((uintptr_t)shell->blocks, sizeof *shell->blocks,\n                           begin, length)) return YAN_SHELL_INVALID;',
     'if (false && shell->blocks != NULL &&\n            ranges_overlap((uintptr_t)shell->blocks, sizeof *shell->blocks,\n                           begin, length)) return YAN_SHELL_INVALID;',
     'shell_blocks_binding_and_line_aliases', 'blocks_line_alias_rejected'),
)
MUTATIONS = FORMAT_MUTATIONS + SERVICE_MUTATIONS + SHELL_MUTATIONS


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--repo', type=Path, required=True)
    parser.add_argument('--unity', type=Path, required=True)
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--suites', nargs='+', choices=tuple(GROUPS),
                        default=tuple(GROUPS))
    args = parser.parse_args()
    suites = tuple(dict.fromkeys(args.suites))
    required = required_inputs(suites)
    missing = [name for name in required if not (args.repo / name).is_file()]
    if missing:
        print('harness: missing repository inputs:', missing)
        return 1
    if not shutil.which(args.cc) or any(not (args.unity / name).is_file()
                                       for name in UNITY_FILES):
        print('SKIP: compiler or Unity unavailable')
        return 77
    started = time.monotonic()
    selected = [entry for entry in MUTATIONS if entry[1] in suites]
    if any(not any(entry[1] == suite for entry in selected) for suite in suites):
        raise HarnessError('selected suite has no declared actual mutants')
    args.work.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='blocks-', dir=args.work))
    fixture = root / 'fixture'
    copy_inputs(args.repo, fixture, required)
    unity = root / 'unity.o'
    built = subprocess.run([args.cc, '-std=c17', '-I', str(args.unity / 'src'),
        '-c', str(args.unity / 'src/unity.c'), '-o', str(unity)],
        capture_output=True, timeout=90)
    (root / 'unity-build.log').write_bytes(built.stdout + built.stderr)
    if built.returncode:
        raise HarnessError('Unity compilation failed')

    def variant(label, suite, changed=None, old=None, new=None):
        test, names = GROUPS[suite]
        sources = []
        if changed is not None and changed not in names:
            raise HarnessError('mutation source is absent from suite: ' + label)
        for name in names:
            source = fixture / 'os' / name
            if name == changed:
                text = source.read_text(encoding='utf-8')
                if text.count(old) != 1:
                    raise HarnessError('replacement is not unique: ' + label)
                source = root / (label + '-' + name)
                source.write_text(text.replace(old, new, 1), encoding='utf-8')
            sources.append(str(source))
        binary = root / label
        built = subprocess.run([args.cc, *STRICT_FLAGS,
            '-I', str(fixture / 'os'), '-I', str(args.unity / 'src'),
            *sources, str(fixture / test), str(unity), '-o', str(binary)],
            capture_output=True, timeout=90)
        (root / (label + '-build.log')).write_bytes(built.stdout + built.stderr)
        if built.returncode:
            raise HarnessError('compilation failed: ' + label)
        try:
            ran = subprocess.run([str(binary)], capture_output=True, timeout=30)
            result = ran.returncode, ran.stdout, ran.stderr
        except subprocess.TimeoutExpired as error:
            result = None, error.stdout or b'', error.stderr or b''
        (root / (label + '.log')).write_bytes(result[1] + result[2])
        return result

    for suite in suites:
        baseline = variant('baseline-' + suite, suite)
        parsed = parse_unity(baseline[1])
        if classify_run(*baseline, None) != 'pass' or parsed is None or parsed[2]:
            raise HarnessError('baseline must be complete, green, unignored: ' + suite)
        print('BASELINE', suite, 'PASS', flush=True)
    for suite in suites:
        no_effect = variant('no-effect-' + suite, suite, 'note_blocks_format.c',
            'length > YAN_BLOCK_ID_MAX', '(length) > YAN_BLOCK_ID_MAX')
        if classify_run(*no_effect, 'format_id_rules', 'format_id_32_rejected') != 'pass':
            raise HarnessError('no-effect control credited: ' + suite)
    for label, suite, changed, old, new, owner, assertion in selected:
        result = variant(label, suite, changed, old, new)
        if classify_suites({suite: result}, owner, assertion, suite) != 'owner-fail':
            raise HarnessError('designated assertion did not detect: ' + label)
        if classify_run(*result, 'absent_owner', assertion) != 'wrong-owner':
            raise HarnessError('wrong-owner control credited: ' + label)
        if classify_run(*result, owner, 'absent_assertion') != 'wrong-assertion':
            raise HarnessError('wrong-assertion control credited: ' + label)
        print('DETECTED', label, suite, owner, assertion, flush=True)
    print('PASS:', len(selected), 'actual block mutations; suites', ','.join(suites),
          '; designated assertions and real negative controls;',
          round(time.monotonic() - started, 3), 'seconds; logs', root)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (RuntimeError, subprocess.TimeoutExpired, OSError) as error:
        print('harness:', error)
        sys.exit(1)
