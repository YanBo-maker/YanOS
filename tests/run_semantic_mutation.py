#!/usr/bin/env python3
"""Actual 0029 host-semantic defects must fail their designated suite, owner
and assertion.

This is not regression evidence. It compiles the host semantic core (semantic.c,
semantic_corpus.c, semantic_sha256.c, semantic_text.c), os/note_metadata_format.c
and tests/test_semantic.c with the repository strict C17 bar, then applies one
exact single-occurrence replacement per mutation and classifies the Unity run
with the proven run_yanfs_mutation classifier. A mutant counts only when the
declared owner test fails and its FAIL message contains the declared assertion
marker (a TEST_ASSERT_*_MESSAGE id). Baseline and a real no-effect control run
first; each mutant also checks wrong-owner and wrong-assertion classification.

Exit codes: 0 all detected, 1 any failed baseline/control, survivor or harness
error, 77 the external C compiler or Unity is missing. Required
repository inputs fail hard (1) before the external-dependency skip."""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from run_note_metadata_mutation import classify_suites
from run_yanfs_mutation import classify_run, parse_unity, HarnessError

SOURCES = ('host/semantic.c', 'host/semantic_corpus.c', 'host/semantic_sha256.c',
           'host/semantic_text.c', 'os/note_metadata_format.c')
REQUIRED = tuple(SOURCES) + (
    'host/semantic.h', 'host/semantic_corpus.h', 'host/semantic_sha256.h',
    'host/semantic_text.h', 'host/semantic_result.h',
    'os/note_metadata_format.h', 'os/memory.h',
    'os/note_metadata_format_internal.h',
    'tests/test_semantic.c', 'tests/run_semantic_mutation.py',
    'tests/run_note_metadata_mutation.py', 'tests/run_yanfs_mutation.py')
SUITE = 'semantic'

# label, mutated file, unique old text, new text, owner test, assertion id.
MUTATIONS = (
    ('payload-checksum', 'host/semantic.c',
     'if (memcmp(payload, meta->payload, sizeof payload) != 0) {',
     'if (false && memcmp(payload, meta->payload, sizeof payload) != 0) {',
     'test_index_corruption_variants', 'semantic_index_payload_mismatch'),
    ('model-identity-stale', 'host/semantic.c',
     '        memcmp(meta->model, ctx->identity.model, 32u) != 0 ||\n',
     '',
     'test_index_corruption_variants', 'semantic_index_model_stale'),
    ('source-identity-stale', 'host/semantic.c',
     '    if (memcmp(meta->source, corpus->source_hash, 32u) != 0 ||',
     '    if ((false && memcmp(meta->source, corpus->source_hash, 32u) != 0) ||',
     'test_missing_stale_and_corrupt_no_provider_or_output',
     'semantic_status_stale_identity'),
    ('same-identity-provenance', 'host/semantic.c',
     '        if (!same_provenance(&records[i], &corpus->segments[i])) {',
     '        if (false && !same_provenance(&records[i], &corpus->segments[i])) {',
     'test_index_corruption_variants', 'semantic_index_provenance_mismatch'),
    ('skipped-count', 'host/semantic.c',
     '    if (meta->segments != corpus->segment_count ||\n'
     '        meta->skipped != corpus->skipped) {',
     '    if (meta->segments != corpus->segment_count) {',
     'test_index_corruption_variants', 'semantic_index_skipped_mismatch'),
    ('reverse-score-order', 'host/semantic.c',
     '    if (a->score > b->score) {\n        return -1;\n    }\n'
     '    if (a->score < b->score) {\n        return 1;\n    }',
     '    if (a->score < b->score) {\n        return -1;\n    }\n'
     '    if (a->score > b->score) {\n        return 1;\n    }',
     'test_search_ranking_raw_and_ties', 'semantic_rank_score_desc'),
    ('tie-ascii-order', 'host/semantic.c',
     'const int order = strcmp(a->name, b->name);',
     'const int order = strcmp(b->name, a->name);',
     'test_search_tie_break_is_ascii_name', 'semantic_tie_ascii_name'),
    ('top-nineteen', 'host/semantic.c',
     'selected = count < YAN_SEMANTIC_TOP ? count : YAN_SEMANTIC_TOP;',
     'selected = count < YAN_SEMANTIC_TOP ? count : YAN_SEMANTIC_TOP - 1u;',
     'test_top20_after_global_ranking', 'semantic_top20_shown'),
    ('truncated-segments', 'host/semantic.c',
     '        out->segments = meta.segments;',
     '        out->segments = delivered;',
     'test_top20_after_global_ranking', 'semantic_summary_full_segments'),
    ('late-reread-error', 'host/semantic.c',
     '            result = ctx->source.read(ctx->source.context, provenance->name,\n'
     '                                      provenance->start,\n'
     '                                      raw + (size_t)i * 2048u, raw_length);\n'
     '            if (result != YAN_SEMANTIC_OK) {\n'
     '                goto cleanup;\n'
     '            }',
     '            result = ctx->source.read(ctx->source.context, provenance->name,\n'
     '                                      provenance->start,\n'
     '                                      raw + (size_t)i * 2048u, raw_length);\n'
     '            if (false && result != YAN_SEMANTIC_OK) {\n'
     '                goto cleanup;\n'
     '            }',
     'test_late_reread_failure_zero_callbacks', 'semantic_late_reread_io'),
    ('search-close-error', 'host/semantic.c',
     '        result = ctx->close_source(ctx->source.context);\n'
     '        source_closed = true;\n'
     '        if (result != YAN_SEMANTIC_OK) {\n'
     '            goto cleanup;\n'
     '        }\n'
     '    } else {\n'
     '        source_closed = true;\n'
     '    }\n'
     '\n'
     '    for (uint32_t i = 0u; i < selected; ++i) {',
     '        result = ctx->close_source(ctx->source.context);\n'
     '        source_closed = true;\n'
     '        if (false && result != YAN_SEMANTIC_OK) {\n'
     '            goto cleanup;\n'
     '        }\n'
     '    } else {\n'
     '        source_closed = true;\n'
     '    }\n'
     '\n'
     '    for (uint32_t i = 0u; i < selected; ++i) {',
     'test_search_source_close_error_stops_before_output',
     'semantic_search_source_close_io'),
    ('stopped-as-ok', 'host/semantic.c',
     '            result = YAN_SEMANTIC_STOPPED;\n'
     '            goto cleanup;\n'
     '        }\n'
     '        delivered += 1u;',
     '            result = YAN_SEMANTIC_OK;\n'
     '            goto cleanup;\n'
     '        }\n'
     '        delivered += 1u;',
     'test_stop_prefix_busy_and_invalid_query', 'semantic_stop_is_stopped'),
    ('publish-after-finish-error', 'host/semantic.c',
     '        result = ctx->store.finish_write(ctx->store.context);\n'
     '        if (result != YAN_SEMANTIC_OK) {\n'
     '            goto cleanup;\n'
     '        }\n'
     '        result = ctx->store.publish(ctx->store.context);',
     '        result = ctx->store.finish_write(ctx->store.context);\n'
     '        if (false && result != YAN_SEMANTIC_OK) {\n'
     '            goto cleanup;\n'
     '        }\n'
     '        result = ctx->store.publish(ctx->store.context);',
     'test_finish_error_is_not_published', 'semantic_finish_error_not_published'),
    ('output-alias-guard', 'host/semantic.c',
     '    if (!span_ok(out, sizeof *out) || holder_conflicts(ctx, out, sizeof *out)) {',
     '    if (!span_ok(out, sizeof *out) || (false && holder_conflicts(ctx, out, sizeof *out))) {',
     'test_holder_alias_rejection', 'semantic_alias_rebuild_out'),
    ('scalar-count-zero', 'host/semantic.c',
     '        out->scalar_count < 1u ||\n',
     '',
     'test_index_structural_record_guards', 'semantic_record_scalar_zero'),
    ('scalar-count-max', 'host/semantic.c',
     '        out->scalar_count > YAN_SEMANTIC_CHUNK_MAX_SCALARS) {',
     '        out->scalar_count > 100000u) {',
     'test_index_structural_record_guards', 'semantic_record_scalar_max'),
    ('reserved-nonzero', 'host/semantic.c',
     '    for (uint32_t i = 52u; i < 64u; ++i) {\n'
     '        if (record[i] != 0u) {\n'
     '            return false;\n'
     '        }\n'
     '    }',
     '    for (uint32_t i = 52u; i < 64u; ++i) {\n'
     '        if (false && record[i] != 0u) {\n'
     '            return false;\n'
     '        }\n'
     '    }',
     'test_index_structural_record_guards', 'semantic_record_reserved_zero'),
    ('name-padding', 'host/semantic.c',
     '    for (uint32_t i = name_length + 1u; i < 32u; ++i) {\n'
     '        if (record[i] != 0u) {\n'
     '            return false;\n'
     '        }\n'
     '    }',
     '    for (uint32_t i = name_length + 1u; i < 32u; ++i) {\n'
     '        if (false && record[i] != 0u) {\n'
     '            return false;\n'
     '        }\n'
     '    }',
     'test_index_structural_record_guards', 'semantic_record_name_padding'),
    ('invalid-name', 'host/semantic.c',
     '    if (!yan_note_name_valid(record, name_length)) {',
     '    if (false && !yan_note_name_valid(record, name_length)) {',
     'test_index_structural_record_guards', 'semantic_record_name_invalid'),
    ('evidence-source-zero', 'host/semantic.c',
     '        memcpy(evidence_out->source_sha256, corpus->source_hash, 32u);',
     '        memset(evidence_out->source_sha256, 0, 32u);',
     'test_evidence_full_raw_identity_and_late_publication', 'evidence_source_same_scan'),
    ('evidence-wrong-config', 'host/semantic.c',
     '        evidence_out->identity = verified_identity;',
     '        evidence_out->identity = verified_identity; evidence_out->identity.config[0] ^= 1u;',
     'test_evidence_full_raw_identity_and_late_publication', 'evidence_verified_identity'),
    ('evidence-query-suffix-alias', 'host/semantic.c',
     '        spans_overlap(query, length, evidence_out, sizeof *evidence_out)) {',
     '        (false && spans_overlap(query, length, evidence_out, sizeof *evidence_out))) {',
     'test_evidence_complete_holder_guards_and_busy', 'evidence_query_suffix_alias'),
    ('evidence-binding-suffix-alias', 'host/semantic.c',
     '        holder_conflicts(ctx, evidence_out, sizeof *evidence_out) ||',
     '        holder_conflicts(ctx, evidence_out, sizeof(YanSemanticSummary)) ||',
     'test_evidence_complete_holder_guards_and_busy', 'evidence_binding_suffix_alias'),
    ('evidence-publish-on-failure', 'host/semantic.c',
     '    if (result == YAN_SEMANTIC_OK) {\n        *evidence_out = pending;\n    }',
     '    if (true) {\n        *evidence_out = pending;\n    }',
     'test_evidence_all_failures_preserve_entire_holder', 'evidence_failure_preserves_all'),
    ('evidence-rounded-score', 'host/semantic.c',
     '            rows[i].score = ranked[i].score;',
     '            rows[i].score = round(ranked[i].score * 1e6) / 1e6;',
     'test_evidence_old_new_agree_and_keep_score_precision', 'evidence_score_full_precision'),
    ('evidence-truncated-raw', 'host/semantic.c',
     '            rows[i].raw_length = raw_length;',
     '            rows[i].raw_length = raw_length > 160u ? 160u : raw_length;',
     'test_evidence_full_raw_identity_and_late_publication', 'evidence_full_raw_length'),
)

NO_EFFECT = ('host/semantic.c',
             '    if (a->score > b->score) {\n        return -1;\n    }',
             '    if ((a->score) > b->score) {\n        return -1;\n    }')


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
    root = Path(tempfile.mkdtemp(prefix='semantic-', dir=args.work))
    unity = root / 'unity.o'
    built = subprocess.run(
        [args.cc, '-std=c17', '-I', str(args.unity / 'src'), '-c',
         str(args.unity / 'src/unity.c'), '-o', str(unity)],
        capture_output=True, timeout=90)
    if built.returncode:
        raise HarnessError('Unity compilation failed')

    def variant(label, changed=None, old=None, new=None):
        sources = []
        for filename in SOURCES:
            path = args.repo / filename
            if filename == changed and old is not None:
                text = path.read_text(encoding='utf-8')
                if text.count(old) != 1:
                    raise HarnessError('replacement is not unique: ' + label)
                target = root / (label + '-' + Path(filename).name)
                target.write_text(text.replace(old, new, 1), encoding='utf-8')
                path = target
            sources.append(str(path))
        binary = root / label
        command = [args.cc, '-std=c17', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                   '-I', str(args.repo / 'host'), '-I', str(args.repo / 'os'),
                   '-I', str(args.unity / 'src'), *sources,
                   str(args.repo / 'tests/test_semantic.c'), str(unity),
                   '-lm', '-o', str(binary)]
        compiled = subprocess.run(command, capture_output=True, timeout=120)
        (root / (label + '-build.log')).write_bytes(compiled.stdout + compiled.stderr)
        if compiled.returncode:
            raise HarnessError('compilation failed: ' + label)
        try:
            ran = subprocess.run([str(binary)], capture_output=True, timeout=60)
            result = ran.returncode, ran.stdout, ran.stderr
        except subprocess.TimeoutExpired:
            result = None, b'', b''
        (root / (label + '.log')).write_bytes(result[1] + result[2])
        return result

    baseline = variant('baseline')
    if classify_run(*baseline, None) != 'pass' or parse_unity(baseline[1])[2]:
        raise HarnessError('baseline must be complete, green, unignored')

    no_effect = variant('no-effect', NO_EFFECT[0], NO_EFFECT[1], NO_EFFECT[2])
    if classify_suites({SUITE: no_effect}, 'test_search_ranking_raw_and_ties',
                       'semantic_rank_score_desc', SUITE) != 'pass':
        raise HarnessError('no-effect control credited')

    for label, changed, old, new, owner, assertion in MUTATIONS:
        result = variant(label, changed, old, new)
        if classify_suites({SUITE: result}, owner, assertion, SUITE) != 'owner-fail':
            raise HarnessError('designated assertion did not detect: ' + label)
        if classify_run(*result, 'absent_owner', assertion) != 'wrong-owner':
            raise HarnessError('wrong-owner control credited: ' + label)
        if classify_run(*result, owner, 'absent_assertion') != 'wrong-assertion':
            raise HarnessError('wrong-assertion control credited: ' + label)
        print('DETECTED', label, SUITE, owner, assertion, flush=True)

    print('PASS:', len(MUTATIONS), 'actual semantic mutations; designated assertions; logs', root)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (RuntimeError, subprocess.TimeoutExpired, OSError) as error:
        print('harness:', error)
        sys.exit(1)
