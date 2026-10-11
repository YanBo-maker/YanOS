"""Owner-specific 0027 format/service/filter mutations. Not regression evidence."""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

REQUIRED = ("os/note_metadata_format.c", "os/note_metadata_format.h",
            "os/memory.h", "tests/test_note_metadata_format.c",
            "tests/run_yanfs_mutation.py", "tests/run_note_metadata_mutation.py",
            "os/note_metadata.c", "os/note_metadata.h", "os/yanfs.c", "os/yanfs.h",
            "os/search_terms.c", "os/search_terms.h", "os/search_terms_linear.c",
            "os/search_terms_linear.h", "os/search_terms_index.c", "os/search_terms_index.h",
            "os/search_terms_core.c", "os/search_terms_core.h", "os/search_text.c", "os/search_text.h",
            "tests/test_note_metadata.c", "tests/test_note_metadata_faults.c",
            "tests/test_search_metadata.c", "tests/test_shell_metadata.c",
            "os/shell.c", "os/shell.h", "os/search.c", "os/search.h",
            "os/search_linear.c", "os/search_linear.h")
SUITE = "format"
# Exact suite / Unity owner / assertion; a failure elsewhere is insufficient.
MUTATIONS = (
    ("century-leap", "return (year % 4u == 0u && year % 100u != 0u) || year % 400u == 0u;",
     "return year % 4u == 0u;", "note_date_validator_rejects_bad_gregorian",
     "date_1900_02_29_nonleap_rejected"),
    ("exact-capacity", "capacity < total", "capacity <= total",
     "note_encoder_accepts_exact_capacity", "encode_exact_17_capacity_ok"),
    ("title-capacity", "length > YAN_NOTE_TITLE_MAX", "length >= YAN_NOTE_TITLE_MAX",
     "note_title_validator_accepts_and_preserves", "title_256_bytes_ok"),
    ("tag-capacity", "length > YAN_NOTE_TAG_MAX", "length > YAN_NOTE_TAG_MAX + 1u",
     "note_tag_validator_rejects_boundaries", "tag_64_bytes_rejected"),
    ("complete-prefix", "length < YAN_NOTE_PREFIX_SIZE", "length <= YAN_NOTE_PREFIX_SIZE",
     "note_parse_prefix_without_version_is_metadata", "nine_byte_prefix_metadata"),
    ("date-delimiter", 'emit_literal(cursor, "created: ", 9u)',
     'emit_literal(cursor, "created: ", 8u)',
     "note_encode_all_fields_roundtrips_canonically", "roundtrip_bytes_canonical"),
)


def classify_suites(runs, owner, assertion, designated_suite=SUITE):
    from run_yanfs_mutation import classify_run, parse_unity
    if designated_suite not in runs:
        return "harness"
    verdicts = {}
    for suite, run in runs.items():
        parsed = parse_unity(run[1])
        verdict = classify_run(*run, owner, assertion)
        if parsed is not None and parsed[2]:
            verdict = "harness"
        verdicts[suite] = verdict
    if "harness" in verdicts.values():
        return "harness"
    designated = verdicts[designated_suite]
    if designated == "pass" and "owner-fail" in verdicts.values():
        return "wrong-suite"
    return designated


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--unity", type=Path, required=True)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--work", type=Path, required=True)
    args = parser.parse_args()
    # Required repository inputs fail before optional external dependency skips.
    missing = [item for item in REQUIRED if not (args.repo / item).is_file()]
    if missing:
        print("harness: missing repository inputs:", missing)
        return 1
    if not shutil.which(args.cc) or not (args.unity / "src/unity.c").is_file():
        print("SKIP: compiler or Unity unavailable")
        return 77
    sys.path.insert(0, str(args.repo / "tests"))
    from run_yanfs_mutation import classify_run, parse_unity, HarnessError
    args.work.mkdir(parents=True, exist_ok=True)
    run_root = Path(tempfile.mkdtemp(prefix="note-format-", dir=args.work))
    unity = run_root / "unity.o"
    dep = subprocess.run([args.cc, "-std=c17", "-I", str(args.unity / "src"),
                          "-c", str(args.unity / "src/unity.c"), "-o", str(unity)],
                         capture_output=True, timeout=90)
    if dep.returncode:
        raise HarnessError("Unity compilation failed")

    groups = {
        "format": ("test_note_metadata_format.c", ["note_metadata_format.c"]),
        "service": ("test_note_metadata.c", ["note_metadata_format.c", "note_metadata.c", "yanfs.c"]),
        "fault": ("test_note_metadata_faults.c", ["note_metadata_format.c", "note_metadata.c", "yanfs.c"]),
        "filter": ("test_search_metadata.c", ["note_metadata_format.c", "yanfs.c", "search_terms.c", "search_terms_linear.c", "search_terms_index.c", "search_terms_core.c", "search_text.c"]),
        "shell": ("test_shell_metadata.c", ["shell.c", "note_metadata.c", "note_metadata_format.c", "yanfs.c", "search.c", "search_linear.c", "search_terms.c", "search_terms_linear.c", "search_terms_index.c", "search_terms_core.c", "search_text.c"]),
    }
    def run_variant(name, old=None, new=None, suite="format", changed="note_metadata_format.c"):
        sources = []
        for filename in groups[suite][1]:
            path = args.repo / "os" / filename
            if filename == changed and old is not None:
                text = path.read_text(encoding="utf-8")
                if text.count(old) != 1:
                    raise HarnessError("replacement is not unique: " + name)
                target = run_root / (name + "-" + filename)
                target.write_text(text.replace(old, new, 1), encoding="utf-8")
                path = target
            sources.append(str(path))
        binary = run_root / name
        command = [args.cc, "-std=c17", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                   "-I", str(args.repo / "os"), "-I", str(args.unity / "src"),
                   *sources, str(args.repo / "tests" / groups[suite][0]),
                   str(unity), "-o", str(binary)]
        built = subprocess.run(command, capture_output=True, timeout=90)
        if built.returncode:
            (run_root / (name + "-build.log")).write_bytes(built.stdout + built.stderr)
            raise HarnessError("compilation failed: " + name)
        try:
            ran = subprocess.run([str(binary)], capture_output=True, timeout=30)
            result = ran.returncode, ran.stdout, ran.stderr
        except subprocess.TimeoutExpired:
            result = None, b"", b""
        (run_root / (name + ".log")).write_bytes(result[1] + result[2])
        return result

    extra = (
        ("old-exact-capacity", "service", "note_metadata.c", "old_size > YAN_NOTE_CANDIDATE_BYTES", "old_size >= YAN_NOTE_CANDIDATE_BYTES", "note_exact_candidate_capacity_is_accepted", "edge_real_change_ok"),
        ("invalid-text-accepted", "service", "note_metadata.c", "if (!editor_text_valid(context->candidate, old_size))", "if (old_size == UINT32_MAX && !editor_text_valid(context->candidate, old_size))", "note_real_change_requires_valid_editor_text", "invalid_text_real_change_rejected"),
        ("body-offset-zero", "service", "note_metadata.c", "uint32_t old_header = context->current.header_bytes;", "uint32_t old_header = 0u;", "note_set_title_rewrites_and_keeps_body_bytes", "title_change_body_length_preserved"),
        ("premature-cache", "fault", "note_metadata.c", "    YanFsResult replace_result =", "    context->current = context->working;\n    YanFsResult replace_result =", "data_write_fault_never_publishes_directory", "cache_not_published_during_any_device_write"),
        ("success-cache-offset", "fault", "note_metadata.c", "context->working.header_bytes = encoded;", "context->working.header_bytes = 0u;", "success_publishes_cache_after_directory_commit", "success_cache_body_offset_25"),
        ("scan-unfiltered-slots", "filter", "search_terms_linear.c", "if (linear->metadata_filtered) {", "if (false && linear->metadata_filtered) {", "filter_untagged_files_are_excluded_after_ranking", "untagged_filtered_no_matches"),
        ("index-unfiltered-slots", "filter", "search_terms_index.c", "if (index->linear.metadata_filtered &&", "if (false && index->linear.metadata_filtered &&", "filter_eligible_beyond_plain_top20_is_found", "beyond20_index_total_one"),
        ("binary-skip-count", "filter", "search_terms_linear.c", "++rank->skipped;", "rank->skipped += 2u;", "filter_binary_file_is_a_preflight_skip_not_metadata", "binary_filtered_skipped_once"),
    )
    extra += (("bad-head-hidden", "filter", "search_terms_linear.c",
        "if (parsed == YAN_NOTE_METADATA) return YAN_SEARCH_TERMS_METADATA;",
        "if (parsed == YAN_NOTE_METADATA) return YAN_SEARCH_TERMS_OK;",
        "filter_late_valid_text_bad_header_is_metadata", "late_bad_head_status_metadata"),)
    extra += (
        ("shell-title-trim", "shell", "shell.c", "uint32_t text_start = position < length ? position + 1u : length;", "uint32_t text_start = skip_spaces(line, length, position);", "shell_meta_update_receipts_rewrite_canonical_file", "update_total_length"),
        ("shell-raw-quote-rejected", "shell", "shell.c", "if (shell->metadata == NULL || outer_quoted)", "if (shell->metadata != NULL || outer_quoted)", "shell_tag_quotes_backslash_and_interleaved_keywords", "output_length_exact"),
        ("shell-tag-contaminates-keywords", "shell", "shell.c", "tag_filter->count += 1u;\n            continue;", "tag_filter->count += 1u;\n            if (!have_keyword) continue;", "shell_tag_quotes_backslash_and_interleaved_keywords", "output_length_exact"),
        ("shell-tag-raw-display", "shell", "shell.c", "!note_emit_value(shell, info->tag[i], info->tag_length[i])", "!shell_put_bytes(shell, info->tag[i], info->tag_length[i])", "shell_tag_quotes_backslash_and_interleaved_keywords", "output_length_exact"),
        ("shell-invalid-title-is-not-usage", "shell", "shell.c", "if (text_length > 0u &&\n            !yan_note_title_valid", "if (false && text_length > 0u &&\n            !yan_note_title_valid", "shell_invalid_title_and_names_are_usage_before_io", "output_length_exact"),
    )
    extra += (("leading-minus-accepted", "shell", "shell.c",
        "if (!have_keyword && line[token_start] == (uint8_t)'-')",
        "if (false && !have_keyword && line[token_start] == (uint8_t)'-')",
        "shell_bound_search_keeps_leading_minus_grammar", "output_length_exact"),)
    for suite in groups:
        baseline = run_variant("baseline-" + suite, suite=suite)
        if classify_run(*baseline, None) != "pass" or parse_unity(baseline[1])[2]:
            raise HarnessError("baseline must be complete, green, unignored: " + suite)
    control = run_variant("no-effect", "capacity < total", "(capacity) < total")
    if classify_run(*control, MUTATIONS[1][3], MUTATIONS[1][4]) != "pass":
        raise HarnessError("no-effect control was credited")
    for name, old, new, owner, assertion in MUTATIONS:
        result = run_variant(name, old, new)
        if classify_suites({SUITE: result}, owner, assertion) != "owner-fail":
            raise HarnessError("designated assertion did not detect: " + name)
        if classify_run(*result, "absent_owner", assertion) != "wrong-owner":
            raise HarnessError("wrong-owner control credited: " + name)
        if classify_run(*result, owner, "absent_assertion") != "wrong-assertion":
            raise HarnessError("wrong-assertion control credited: " + name)
        print("DETECTED", name, SUITE, owner, assertion)
    for name, suite, changed, old, new, owner, assertion in extra:
        result = run_variant(name, old, new, suite, changed)
        if classify_suites({suite: result}, owner, assertion, suite) != "owner-fail":
            raise HarnessError("designated assertion did not detect: " + name)
        if classify_run(*result, "absent_owner", assertion) != "wrong-owner":
            raise HarnessError("wrong-owner control credited: " + name)
        if classify_run(*result, owner, "absent_assertion") != "wrong-assertion":
            raise HarnessError("wrong-assertion control credited: " + name)
        print("DETECTED", name, suite, owner, assertion)
    print("PASS:", len(MUTATIONS) + len(extra), "actual metadata mutations; designated assertions; logs", run_root)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.TimeoutExpired, OSError) as error:
        print("harness:", error)
        raise SystemExit(1)
