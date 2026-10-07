#!/usr/bin/env python3
"""0026 search-term mutation gate: do the term suites detect real defects?

Four native suites are carried through one exact, single-occurrence source
replacement per mutant, each in its own private copy of a minimal tree:

  core        tests/test_search_terms.c over the term facade, the shared core,
              the text primitives, the linear backend and os/yanfs.c
  index       tests/test_search_index.c over the same term sources
  shell       tests/test_shell_terms.c over os/shell.c, the literal search and
              the term sources
  exhaustion  tests/test_yanfs_source_exhaustion.c over a second os/yanfs.c
              compiled with the near-UINT64_MAX allocator seed

The strict Unity decode and the verdict precedence are reused from
tests/run_yanfs_mutation.py by a no-pyc import, so this gate and the YanFS gate
agree on what a detection is and neither script is edited. A mutant is credited
only when the declared owning test fails on the declared assertion marker and no
harness-level condition (build failure, crash, timeout, sanitizer, fixture
abort, Unity summary/exit disagreement) is present; a survivor, a wrong-owner or
a same-owner wrong assertion is a failure, never a detection.

The baseline builds every suite with the full strict bar
(-std=c17 -Wall -Wextra -Wpedantic -Werror); a mutant is allowed to warn, so a
warning-only change is still judged by the run.

Exit codes: 0 all detected, 1 survivor/wrong-owner/wrong-assertion, 2 harness
error or a failed baseline/control, 77 the external C compiler or Unity is
missing. Every in-repo compiled input is checked before the external tools, so a
missing source/header/test/script is a hard 1, never 77.
"""
import argparse
import importlib.util
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

STRICT_FLAGS = ["-std=c17", "-Wall", "-Wextra", "-Wpedantic", "-Werror"]
MUTANT_FLAGS = ["-std=c17", "-Wall", "-Wextra", "-Wpedantic"]
BUILD_TIMEOUT = 240
RUN_TIMEOUT = 240

# Headers copied for every suite. os/memory.c itself is not compiled here: the
# native C library supplies memcpy/memset/memcmp/strlen, and only the
# declarations in memory.h are needed by the term index.
OS_HEADERS = ["yanfs.h", "memory.h", "search.h", "search_linear.h",
              "search_terms.h", "search_terms_core.h", "search_terms_linear.h",
              "search_terms_index.h", "search_text.h", "shell.h"]
OS_SOURCES = ["yanfs.c", "search.c", "search_linear.c", "search_terms.c",
              "search_terms_core.c", "search_terms_linear.c",
              "search_terms_index.c", "search_text.c", "shell.c"]
TEST_FILES = ["test_search_terms.c", "test_search_index.c",
              "test_shell_terms.c", "test_yanfs_source_exhaustion.c"]
REQUIRED_FILES = TEST_FILES + ["run_search_terms_mutation.py"]

TERM_CORE = ["os/search_terms.c", "os/search_terms_core.c", "os/search_text.c",
             "os/search_terms_linear.c", "os/yanfs.c"]
EXHAUSTION_SEED = "YAN_FS_SOURCE_TOKEN_SEED=18446744073709551551ULL"

# suite -> compiled sources, target name and any extra compile define.
SUITES = {
    "core": {
        "sources": ["tests/test_search_terms.c"] + TERM_CORE,
        "target": "test_search_terms",
        "define": None,
        "test": "tests/test_search_terms.c",
    },
    "index": {
        "sources": ["tests/test_search_index.c", "os/search_terms_index.c"] + TERM_CORE,
        "target": "test_search_index",
        "define": None,
        "test": "tests/test_search_index.c",
    },
    "shell": {
        "sources": ["tests/test_shell_terms.c", "os/shell.c", "os/search.c",
                    "os/search_linear.c", "os/search_terms_index.c"] + TERM_CORE,
        "target": "test_shell_terms",
        "define": None,
        "test": "tests/test_shell_terms.c",
    },
    "exhaustion": {
        "sources": ["tests/test_yanfs_source_exhaustion.c", "os/yanfs.c"],
        "target": "test_yanfs_source_exhaustion",
        "define": EXHAUSTION_SEED,
        "test": "tests/test_yanfs_source_exhaustion.c",
    },
}

# mutation name -> (suite, owning Unity test, required assertion marker)
OWNING_ASSERTIONS = {
    # os/search_terms_core.c
    "canonical_dedup_dropped":
        ("core", "semantic_duplicate_groups_do_not_multiply_the_score",
         "one canonical group found three times must score 3, not 6"),
    "ascii_right_boundary_dropped":
        ("core", "semantic_ascii_whole_word_and_case",
         "interrupt must not match interruption"),
    "overlap_reset_dropped":
        ("core", "semantic_overlapping_chinese_counts_each_start",
         "overlapping matches must count each start"),
    "group_cap_removed":
        ("core", "semantic_group_score_saturates_above_255",
         "a group must cap accepted occurrences at 255"),
    "nul_preflight_dropped":
        ("core", "semantic_nul_file_is_skipped_once",
         "a NUL file must be skipped once"),
    "utf8_preflight_dropped":
        ("core", "semantic_skips_invalid_utf8_at_end_and_continues",
         "a whole invalid file must be skipped once"),
    "bare_cr_ends_snippet":
        ("core", "semantic_bare_cr_is_content_and_only_lf_ends_a_line",
         "a bare CR stays in the raw snippet"),
    "snippet_head_uncapped":
        ("core", "semantic_snippet_window_truncates_and_caps",
         "a match 100 scalars in must be left-truncated"),
    "rank_line_order_reversed":
        ("core", "semantic_top20_total_and_score_order",
         "equal-score rows must retain ascending line order"),
    # os/search_terms_index.c
    "index_clamp_removed":
        ("index", "index_capacity_postings_exact_ready",
         "indexed group count must clamp at 255"),
    "index_invalid_file_skip_uncounted":
        ("index", "index_invalid_files_are_wholly_skipped",
         "index must count whole invalid files"),
    "index_preflight_protocol_swallowed":
        ("index", "index_preflight_protocol_does_not_publish",
         "index preflight protocol fault must propagate"),
    "index_raw_compare_dropped":
        ("index", "index_hash_collision_keys_are_raw_compared",
         "the first colliding key must be found"),
    "index_limit_rebuilds":
        ("index", "index_same_identity_limit_does_not_rebuild",
         "a same-identity LIMIT query scans once, like the linear baseline"),
    "index_stale_detection_dropped":
        ("index", "index_create_invalidates_ready_to_stale",
         "a published mutation must invalidate READY"),
    # os/shell.c
    "shell_bare_dash_accepted":
        ("shell", "search_grammar_is_usage_without_io",
         "invalid search grammar must print USAGE"),
    "shell_uint64_divisor":
        ("shell", "search_summary_prints_a_uint64_total",
         "the uint64 total must print exactly"),
    "shell_output_failure_ignored":
        ("shell", "search_output_failure_first_middle_last_is_fatal",
         "a refused byte is always fatal"),
    # os/search_terms_index.c, owner in the shell suite
    "index_clear_noop":
        ("shell", "index_status_and_clear_do_no_io_and_clear_drops_ready",
         "clear must drop the READY cache"),
    # os/yanfs.c
    "older_instance_cacheable_after_exhaustion":
        ("exhaustion", "source_allocator_exhaustion_crosses_max_without_wrap",
         "global exhaustion must disable caching for older instances too"),
}

MUTATIONS = [
    # --- os/search_terms_core.c -------------------------------------------
    {
        "name": "canonical_dedup_dropped",
        "file": "os/search_terms_core.c",
        "old": ("        if (duplicate) {\n"
                "            continue;\n"
                "        }\n"),
        "new": ("        if (false) {\n"
                "            continue;\n"
                "        }\n"),
    },
    {
        "name": "ascii_right_boundary_dropped",
        "file": "os/search_terms_core.c",
        "old": ("            if (accepted && pattern->right_word[g] && !next_eof &&\n"
                "                yan_search_text_is_ascii_word(next_byte)) {\n"
                "                accepted = false;\n"
                "            }\n"),
        "new": ("            if (false) {\n"
                "                accepted = false;\n"
                "            }\n"),
    },
    {
        "name": "overlap_reset_dropped",
        "file": "os/search_terms_core.c",
        "old": "            kmp = pattern->prefix[base + length - 1u];\n",
        "new": "            kmp = 0u;\n",
    },
    {
        "name": "group_cap_removed",
        "file": "os/search_terms_core.c",
        "old": ("                if (state->counts[g] < 255u) {\n"
                "                    ++state->counts[g];\n"
                "                }\n"),
        "new": "                ++state->counts[g];\n",
    },
    {
        "name": "nul_preflight_dropped",
        "file": "os/search_terms_core.c",
        "old": ("            if (byte == 0u) {\n"
                "                *skip = true;\n"
                "                return YAN_SEARCH_TERMS_OK;\n"
                "            }\n"),
        "new": ("            if (false) {\n"
                "                *skip = true;\n"
                "                return YAN_SEARCH_TERMS_OK;\n"
                "            }\n"),
    },
    {
        "name": "utf8_preflight_dropped",
        "file": "os/search_terms_core.c",
        "old": ("    if (carry_length != 0u) {\n"
                "        *skip = true; /* an unfinished sequence at end of file is damage */\n"
                "    }\n"),
        "new": ("    if (false) {\n"
                "        *skip = true; /* an unfinished sequence at end of file is damage */\n"
                "    }\n"),
    },
    {
        "name": "bare_cr_ends_snippet",
        "file": "os/search_terms_core.c",
        "old": ("        if (byte == (uint8_t)'\\r') {\n"
                "            if (i + 1u < got && scratch[i + 1u] == (uint8_t)'\\n') {\n"
                "                break; /* exclude the CR of a CRLF */\n"
                "            }\n"
                "            i += 1u;\n"
                "            right_end = i;\n"
                "            ++right_count;\n"
                "            continue;\n"
                "        }\n"),
        "new": ("        if (byte == (uint8_t)'\\r') {\n"
                "            break; /* mutant: any CR ends the snippet */\n"
                "        }\n"),
    },
    {
        "name": "snippet_head_uncapped",
        "file": "os/search_terms_core.c",
        "old": "    uint32_t head = anchor < 255u ? anchor : 255u;\n",
        "new": "    uint32_t head = anchor < 255u ? 0u : 255u;\n",
    },
    {
        "name": "rank_line_order_reversed",
        "file": "os/search_terms_core.c",
        "old": "    return candidate->line_number < existing->line_number;\n",
        "new": "    return candidate->line_number > existing->line_number;\n",
    },
    # --- os/search_terms_index.c ------------------------------------------
    {
        "name": "index_clamp_removed",
        "file": "os/search_terms_index.c",
        "old": ("                if (count < 255u) {\n"
                "                    ++count;\n"
                "                }\n"),
        "new": "                ++count;\n",
    },
    {
        "name": "index_invalid_file_skip_uncounted",
        "file": "os/search_terms_index.c",
        "old": ("        if (skip) {\n"
                "            ++index->skipped;\n"
                "            continue;\n"
                "        }\n"),
        "new": ("        if (skip) {\n"
                "            continue;\n"
                "        }\n"),
    },
    {
        "name": "index_preflight_protocol_swallowed",
        "file": "os/search_terms_index.c",
        "old": ("        if (result != YAN_SEARCH_TERMS_OK) {\n"
                "            index_clear_cache(index);\n"
                "            return result;\n"
                "        }\n"
                "        if (skip) {\n"),
        "new": ("        if (false) {\n"
                "            index_clear_cache(index);\n"
                "            return result;\n"
                "        }\n"
                "        if (skip) {\n"),
    },
    {
        "name": "index_raw_compare_dropped",
        "file": "os/search_terms_index.c",
        "old": ("        uint32_t ki = entry - 1u;\n"
                "        const YanSearchTermsIndexKey *k = &index->keys[ki];\n"
                "        if (k->key_length == length &&\n"
                "            memcmp(index->key_pool + k->key_offset, key, length) == 0) {\n"
                "            *out_key = ki;\n"
                "            return YAN_SEARCH_TERMS_OK;\n"
                "        }\n"),
        "new": ("        uint32_t ki = entry - 1u;\n"
                "        const YanSearchTermsIndexKey *k = &index->keys[ki];\n"
                "        if (k->key_length == length) {\n"
                "            *out_key = ki;\n"
                "            return YAN_SEARCH_TERMS_OK;\n"
                "        }\n"),
    },
    {
        "name": "index_limit_rebuilds",
        "file": "os/search_terms_index.c",
        "old": "    if (index->state == YAN_SEARCH_INDEX_LIMIT && token_match) {\n",
        "new": "    if (false && token_match) {\n",
    },
    {
        "name": "index_stale_detection_dropped",
        "file": "os/search_terms_index.c",
        "old": ("    if ((index->state == YAN_SEARCH_INDEX_READY ||\n"
                "         index->state == YAN_SEARCH_INDEX_LIMIT) &&\n"
                "        !token_match) {\n"
                "        out->state = YAN_SEARCH_INDEX_STALE;\n"
                "        return YAN_SEARCH_TERMS_OK;\n"
                "    }\n"),
        "new": ("    if (false) {\n"
                "        out->state = YAN_SEARCH_INDEX_STALE;\n"
                "        return YAN_SEARCH_TERMS_OK;\n"
                "    }\n"),
    },
    {
        "name": "index_clear_noop",
        "file": "os/search_terms_index.c",
        "old": ("static YanSearchTermsResult index_clear(void *context)\n"
                "{\n"
                "    YanSearchTermsIndex *index = (YanSearchTermsIndex *)context;\n"
                "    YanSearchTermsResult guard = index_guard(index);\n"
                "    if (guard != YAN_SEARCH_TERMS_OK) {\n"
                "        return guard;\n"
                "    }\n"
                "    index_clear_cache(index);\n"
                "    return YAN_SEARCH_TERMS_OK;\n"
                "}\n"),
        "new": ("static YanSearchTermsResult index_clear(void *context)\n"
                "{\n"
                "    YanSearchTermsIndex *index = (YanSearchTermsIndex *)context;\n"
                "    YanSearchTermsResult guard = index_guard(index);\n"
                "    if (guard != YAN_SEARCH_TERMS_OK) {\n"
                "        return guard;\n"
                "    }\n"
                "    (void)index; /* mutant: clear does not drop the cache */\n"
                "    return YAN_SEARCH_TERMS_OK;\n"
                "}\n"),
    },
    # --- os/shell.c --------------------------------------------------------
    {
        "name": "shell_bare_dash_accepted",
        "file": "os/shell.c",
        "old": ("    if (line[position] == (uint8_t)'-') {\n"
                "        return false;\n"
                "    }\n"),
        "new": ("    if (false) {\n"
                "        return false;\n"
                "    }\n"),
    },
    {
        "name": "shell_uint64_divisor",
        "file": "os/shell.c",
        "old": "            digits[i - 1u] = (uint8_t)(digit % 10u);\n",
        "new": "            digits[i - 1u] = (uint8_t)(digit % 9u);\n",
    },
    {
        "name": "shell_output_failure_ignored",
        "file": "os/shell.c",
        "old": ("    YanSearchTermsResult result =\n"
                "        yan_search_terms(shell->terms, query, query_length, sink, &summary);\n"
                "    if (state.output_failed) {\n"
                "        return YAN_SHELL_FATAL;\n"
                "    }\n"),
        "new": ("    YanSearchTermsResult result =\n"
                "        yan_search_terms(shell->terms, query, query_length, sink, &summary);\n"
                "    if (false) {\n"
                "        return YAN_SHELL_FATAL;\n"
                "    }\n"),
    },
    # --- os/yanfs.c --------------------------------------------------------
    {
        "name": "older_instance_cacheable_after_exhaustion",
        "file": "os/yanfs.c",
        "old": "    out->cacheable = fs->source_cacheable && !source_exhausted;\n",
        "new": "    out->cacheable = fs->source_cacheable;\n",
    },
]

# A real text replacement that changes nothing: the suites must still pass.
NO_EFFECT = {
    "name": "no_effect_core_comment",
    "file": "os/search_terms_core.c",
    "old": "#include <stddef.h>\n",
    "new": "#include <stddef.h> /* no-effect control */\n",
    "suite": "core",
    "owner": "semantic_ascii_whole_word_and_case",
    "assertion": "no-effect-control",
}

# A real defect declared against a passing test: it must be wrong-owner.
WRONG_OWNER = {
    "name": "wrong_owner_group_cap",
    "file": "os/search_terms_core.c",
    "old": ("                if (state->counts[g] < 255u) {\n"
            "                    ++state->counts[g];\n"
            "                }\n"),
    "new": "                ++state->counts[g];\n",
    "suite": "core",
    "owner": "semantic_ascii_whole_word_and_case",
    "assertion": "wrong-owner-control",
}

# The real owner fails, but the declared assertion is unrelated.
WRONG_ASSERTION = dict(
    WRONG_OWNER,
    name="wrong_assertion_group_cap",
    owner="semantic_group_score_saturates_above_255",
    assertion="UNRELATED-ASSERTION-MUST-NOT-BE-CREDITED",
)

def _load(path, name):
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


_IMPORT_ERROR = None
try:
    _YANFS = _load(Path(__file__).with_name("run_yanfs_mutation.py"),
                   "search_terms_yanfs_mut_ref")
except Exception as error:  # a missing or broken shared helper is a hard failure
    _YANFS = None
    _IMPORT_ERROR = error
if _YANFS is not None:
    classify_run = _YANFS.classify_run
    unity_suite = _YANFS.unity_suite
    HarnessError = _YANFS.HarnessError
    apply_replacement = _YANFS.apply_replacement
    format_build_report = _YANFS.format_build_report
    unity_object = _YANFS.unity_object
    write_log = _YANFS.write_log
    owner_fail_line = _YANFS.owner_fail_line
    run_binary = _YANFS.run_binary
else:
    classify_run = None
    unity_suite = None
    apply_replacement = None
    format_build_report = None
    unity_object = None
    write_log = None
    owner_fail_line = None
    run_binary = None

    class HarnessError(RuntimeError):
        """Stand-in so the module still imports when the helper is missing."""


def copy_tree(source, unity, destination):
    (destination / "os").mkdir(parents=True)
    (destination / "tests").mkdir()
    (destination / "unity").mkdir()
    for name in OS_HEADERS + OS_SOURCES:
        shutil.copy2(source / "os" / name, destination / "os" / name)
    for name in TEST_FILES:
        shutil.copy2(source / "tests" / name, destination / "tests" / name)
    for item in sorted((unity / "src").iterdir()):
        if item.is_file():
            shutil.copy2(item, destination / "unity" / item.name)


def compile_suite(tree, cc, suite, strict):
    flags = STRICT_FLAGS if strict else MUTANT_FLAGS
    spec = SUITES[suite]
    unity = tree / "unity"
    out_dir = tree / "bin"
    out_dir.mkdir(exist_ok=True)
    obj = unity_object(tree, cc)
    sources = [tree / rel for rel in spec["sources"]]
    target = out_dir / spec["target"]
    command = [cc] + flags
    if spec["define"] is not None:
        command += ["-D", spec["define"]]
    command += ["-I", str(tree / "os"), "-I", str(unity)]
    command += [str(path) for path in sources] + [str(obj), "-o", str(target)]
    try:
        proc = subprocess.run(command, capture_output=True, timeout=BUILD_TIMEOUT)
    except subprocess.TimeoutExpired as error:
        raise HarnessError("build timeout: %s" % suite) from error
    return proc, target, command


def run_one(binary, owner, assertion):
    rc, out, err = run_binary(binary)
    verdict = classify_run(rc, out, err, owner, assertion)
    return verdict, out + err


def run_suites(binaries, owner, assertion, owner_suite=None):
    verdicts = []
    blob = b""
    for suite, binary in binaries.items():
        verdict, output = run_one(binary, owner, assertion)
        if owner_suite is not None and suite != owner_suite:
            if verdict in ("owner-fail", "wrong-assertion"):
                verdict = "wrong-owner"
        verdicts.append(verdict)
        blob += ("\n# run %s\n" % suite).encode() + output
    for verdict in ("harness", "owner-fail", "wrong-assertion", "wrong-owner"):
        if verdict in verdicts:
            return verdict, blob
    return "pass", blob


def owner_exists(source, owner, suite):
    text = (source / SUITES[suite]["test"]).read_text(encoding="utf-8-sig")
    return ("RUN_TEST(%s)" % owner) in text and \
           ("static void %s(void)" % owner) in text


def run_mutation(source, tree_root, cc, mutation, logdir):
    suite = mutation["suite"]
    owner = mutation["owner"]
    assertion = mutation.get("assertion")
    if not assertion:
        raise HarnessError("%s: no required assertion marker" % mutation["name"])
    tree = tree_root / ("mut-" + mutation["name"])
    shutil.copytree(tree_root / "baseline", tree,
                    ignore=shutil.ignore_patterns("bin"))
    apply_replacement(tree / mutation["file"], mutation["old"], mutation["new"])
    binaries = {}
    reports = []
    for name in SUITES:
        proc, binary, command = compile_suite(tree, cc, name, False)
        reports.append((name, command, proc))
        if proc.returncode != 0:
            write_log(logdir, mutation["name"], format_build_report(reports))
            raise HarnessError("%s: mutant build failed" % mutation["name"])
        binaries[name] = binary
    report = format_build_report(reports)
    verdict, blob = run_suites(binaries, owner, assertion, suite)
    write_log(logdir, mutation["name"], report + b"\n" + blob)
    return verdict, blob


def require_in_repo(source):
    missing = [str(source / "os" / name) for name in OS_HEADERS + OS_SOURCES
               if not (source / "os" / name).is_file()]
    missing += [str(source / "tests" / name) for name in REQUIRED_FILES
                if not (source / "tests" / name).is_file()]
    return missing


def self_test_verdict_precedence():
    """Pure mocked controls for run_one/classify_run precedence: a harness
    symptom on one suite must win over an owner failure on another, and the
    required assertion marker is mandatory."""
    marker = "OWNER-ASSERTION-MARKER"
    owner_suite = (1, unity_suite([("owner", "FAIL", marker)]), b"")
    pass_suite = (0, unity_suite([("a", "PASS"), ("b", "PASS")]), b"")
    wrong_assert = (1, unity_suite([("owner", "FAIL", "other value")]), b"")
    other_owner = (1, unity_suite([("other", "FAIL", marker)]), b"")
    cases = [
        (pass_suite, marker, "pass"),
        (owner_suite, marker, "owner-fail"),
        (wrong_assert, marker, "wrong-assertion"),
        (other_owner, marker, "wrong-owner"),
        (owner_suite, None, "harness"),
        ((None, b"", b""), marker, "harness"),
        ((139, unity_suite([("owner", "FAIL", marker)]), b""), marker, "harness"),
        ((0, b"", b""), marker, "harness"),
    ]
    original = run_binary
    try:
        for index, (response, assertion, expected) in enumerate(cases):
            globals()["run_binary"] = lambda _path, _r=response: _r
            verdict, _blob = run_one("/mock/bin", "owner", assertion)
            if verdict != expected:
                raise HarnessError(
                    "run_one control %d: got %s, expected %s"
                    % (index, verdict, expected))
        for responses, expected in [
            ([owner_suite, pass_suite], "owner-fail"),
            ([owner_suite, (139, b"", b"")], "harness"),
            ([(None, b"", b""), owner_suite], "harness"),
            ([wrong_assert, owner_suite], "owner-fail"),
            ([other_owner, wrong_assert], "wrong-assertion"),
        ]:
            sequence = iter(responses)
            globals()["run_binary"] = lambda _path: next(sequence)
            verdict, _ = run_suites({"first": "one", "second": "two"},
                                    "owner", marker)
            if verdict != expected:
                raise HarnessError("multi-suite precedence: %s expected %s"
                                   % (verdict, expected))
    finally:
        globals()["run_binary"] = original

    try:
        sequence = iter([pass_suite, owner_suite])
        globals()["run_binary"] = lambda _path: next(sequence)
        verdict, _ = run_suites({"first": "one", "second": "two"},
                                "owner", marker, "first")
        if verdict != "wrong-owner":
            raise HarnessError("non-designated suite credited an owner failure")
    finally:
        globals()["run_binary"] = original


def main():
    parser = argparse.ArgumentParser(description="0026 search-term mutation gate")
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--work", required=True, type=Path)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--unity", required=True, type=Path)
    args = parser.parse_args()

    if _YANFS is None:
        print("FAIL the shared Unity classifier could not be imported: %r"
              % _IMPORT_ERROR, file=sys.stderr)
        return 1
    try:
        _YANFS.self_test_run_suites()
        self_test_verdict_precedence()
    except HarnessError as error:
        print("HARNESS-ERROR: %s" % error, file=sys.stderr)
        return 2
    print("PASS mocked classifier precedence controls")

    source = args.source.resolve()
    missing = require_in_repo(source)
    if missing:
        print("FAIL the source/tests/scripts under test are missing: %s"
              % ", ".join(missing), file=sys.stderr)
        return 1
    for name, (suite, owner, _marker) in OWNING_ASSERTIONS.items():
        if not owner_exists(source, owner, suite):
            print("FAIL owning test %s is not registered for %s" % (owner, name),
                  file=sys.stderr)
            return 1

    if shutil.which(args.cc) is None:
        print("SKIP: external C compiler missing: %s" % args.cc)
        return 77
    unity = args.unity.resolve()
    if not (unity / "src/unity.c").is_file() or \
            not (unity / "src/unity.h").is_file():
        print("SKIP: external Unity missing under %s/src" % unity)
        return 77

    args.work.mkdir(parents=True, exist_ok=True)
    logdir = args.work / "search-terms-mutation"
    logdir.mkdir(exist_ok=True)
    tree_root = Path(tempfile.mkdtemp(prefix="search-terms-mut-", dir=args.work))
    harness = []
    survivors = []
    try:
        copy_tree(source, unity, tree_root / "baseline")
        binaries = {}
        reports = []
        baseline_ok = True
        for suite in SUITES:
            proc, target, command = compile_suite(tree_root / "baseline",
                                                  args.cc, suite, True)
            reports.append((suite, command, proc))
            binaries[suite] = target
            if proc.returncode != 0:
                baseline_ok = False
        report = format_build_report(reports)
        if not baseline_ok:
            write_log(logdir, "baseline", report)
            print("HARNESS-ERROR: strict baseline build failed", file=sys.stderr)
            return 2
        verdict, baseline_output = run_suites(binaries, "", None)
        write_log(logdir, "baseline", report + baseline_output)
        if verdict != "pass":
            print("HARNESS-ERROR: baseline suites are %s, not a clean pass"
                  % verdict, file=sys.stderr)
            return 2
        print("PASS baseline builds strict and all suites pass")

        for control, expected in ((NO_EFFECT, "pass"),
                                  (WRONG_OWNER, "wrong-owner"),
                                  (WRONG_ASSERTION, "wrong-assertion")):
            entry = dict(control)
            outcome, _ = run_mutation(source, tree_root, args.cc, entry, logdir)
            if outcome != expected:
                print("HARNESS-ERROR: control %s was %s, expected %s"
                      % (control["name"], outcome, expected), file=sys.stderr)
                return 2
            print("PASS control %s: %s" % (control["name"], outcome))

        for mutation in MUTATIONS:
            mutation = dict(mutation)
            suite, owner, assertion = OWNING_ASSERTIONS[mutation["name"]]
            mutation["suite"] = suite
            mutation["owner"] = owner
            mutation["assertion"] = assertion
            try:
                outcome, blob = run_mutation(source, tree_root, args.cc,
                                             mutation, logdir)
            except HarnessError as error:
                harness.append((mutation["name"], str(error)))
                print("HARNESS-ERROR %s: %s" % (mutation["name"], error))
                continue
            if outcome == "owner-fail":
                print("PASS mutant %s: owner %s failed on %r -> %s"
                      % (mutation["name"], owner, assertion,
                         owner_fail_line(blob, owner)))
            elif outcome == "harness":
                harness.append((mutation["name"], "run classified as harness"))
                print("HARNESS-ERROR %s: run classified as harness (never "
                      "detected)" % mutation["name"])
            elif outcome == "wrong-assertion":
                survivors.append((mutation["name"], "wrong assertion"))
                print("FAIL mutant %s: owner %s failed, but not on the required"
                      " assertion %r" % (mutation["name"], owner, assertion))
            elif outcome == "wrong-owner":
                survivors.append((mutation["name"], "wrong owner"))
                print("FAIL mutant %s: a test other than %s failed"
                      % (mutation["name"], owner))
            else:
                survivors.append((mutation["name"], outcome))
                print("FAIL mutant %s: %s" % (mutation["name"], outcome))
    finally:
        shutil.rmtree(tree_root, ignore_errors=True)

    print("---")
    print("%d mutants detected, %d survivors/wrong-owner, %d harness errors; "
          "logs in %s" % (len(MUTATIONS) - len(survivors) - len(harness),
                          len(survivors), len(harness), logdir))
    if harness:
        return 2
    if survivors:
        return 1
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except HarnessError as error:
        print("HARNESS-ERROR: %s" % error, file=sys.stderr)
        sys.exit(2)
