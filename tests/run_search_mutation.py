#!/usr/bin/env python3
"""Search mutation gate: do the 0025 suites actually detect planted defects?

Two native suites are carried through one exact, single-occurrence source
replacement per mutant:

  core    tests/test_search.c over os/search.c + os/search_linear.c + os/yanfs.c
  shell   tests/test_shell.c over os/shell.c + the same search core + os/yanfs.c

The strict Unity decode and the two-suite verdict precedence are reused from
tests/run_yanfs_mutation.py by a no-pyc import, so this gate and the YanFS gate
agree on what a detection is and neither script is edited. A mutant is credited
only when the declared owning test fails on the declared assertion marker and no
harness-level condition (build failure, crash, timeout, sanitizer, fixture
abort, Unity summary/exit disagreement) is present; a survivor, a wrong-owner or
a same-owner wrong assertion is a failure, never a detection.

The baseline builds both suites with the full strict bar
(-Wall -Wextra -Wpedantic -Werror); a mutant is allowed to warn, exactly as the
other gates do, so a warning-only change is still judged by the run.

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
BUILD_TIMEOUT = 180
RUN_TIMEOUT = 120

# Headers copied for the two suites. platform/task/block/console/editor/line/
# terminal are carried even where a suite does not include them today, so a new
# include in the compiled sources cannot silently become an unfound header. The
# 0026 term headers and the freestanding memory declarations are needed because
# os/shell.c now consumes the term facade; os/memory.c itself is not compiled
# here because the native C library supplies memcpy/memset/memcmp/strlen.
OS_HEADERS = ["search.h", "search_linear.h", "yanfs.h", "shell.h", "platform.h",
              "task.h", "block.h", "console.h", "editor.h", "line.h",
              "terminal.h", "yanfs_block.h", "memory.h", "search_terms.h",
              "search_terms_core.h", "search_terms_index.h",
              "search_terms_linear.h", "search_text.h"]
OS_SOURCES = ["search.c", "search_linear.c", "yanfs.c", "shell.c",
              "search_terms.c", "search_terms_linear.c", "search_terms_core.c",
              "search_terms_index.c", "search_text.c"]
TEST_FILES = ["test_search.c", "test_shell.c"]
REQUIRED_FILES = TEST_FILES + ["run_search_mutation.py"]

SUITES = {
    "core": {
        "sources": ["tests/test_search.c", "os/search.c",
                    "os/search_linear.c", "os/yanfs.c"],
        "target": "test_search",
        "test": "tests/test_search.c",
    },
    "shell": {
        "sources": ["tests/test_shell.c", "os/shell.c", "os/search.c",
                    "os/search_linear.c", "os/search_terms.c",
                    "os/search_terms_linear.c", "os/search_terms_core.c",
                    "os/search_terms_index.c", "os/search_text.c", "os/yanfs.c"],
        "target": "test_shell",
        "test": "tests/test_shell.c",
    },
}

# mutation name -> (suite, owning Unity test, required assertion marker)
OWNING_ASSERTIONS = {
    "line_start_kmp_not_reset":
        ("core", "real_search_does_not_match_across_a_line_break",
         "bytes split by LF must not form a match across lines"),
    "scan_chunk_kmp_reset":
        ("core", "real_search_matches_a_word_across_the_scan_chunk_boundary",
         "a word straddling the 4096 boundary is still found"),
    "preflight_nul_bypassed":
        ("core", "real_search_skips_a_file_with_a_late_nul",
         "a NUL anywhere in the file means zero matches for that file"),
    "fs_io_mapped_to_ok":
        ("core", "real_search_preflight_read_failure_reports_io",
         "a preflight read failure is a source IO error, not a zero-match"
         " success"),
    "lf_pending_line_not_advanced":
        ("core", "real_search_empty_lines_still_advance_the_line_number",
         "two empty lines precede the hit, so its number is three"),
    "bare_cr_dropped":
        ("core", "real_search_treats_bare_cr_as_content",
         "a bare CR is ordinary content"),
    "final_line_ignored":
        ("core", "real_search_counts_an_unterminated_final_line",
         "the final line can match"),
    "content_stream_truncated":
        ("core", "real_search_returns_a_complete_100kib_line",
         "the full 100 KiB line is delivered, not truncated to the editor"
         " limit"),
    "zero_progress_treated_ok":
        ("core", "manual_reader_zero_progress_before_end_is_protocol",
         "a manual zero-progress read must latch a sticky protocol error"),
    "sticky_reader_error_dropped":
        ("core", "an_ignored_reader_error_still_reports_the_source_error",
         "ignoring the reader result must not turn a source error into"
         " success"),
    "alias_guard_omitted":
        ("core", "search_read_match_rejects_outputs_overlapping_backend_context",
         "output holders overlapping the backend context must be rejected"),
    "manual_read_blocked":
        ("core", "match_callback_manual_peek_succeeds_while_chunk_streams",
         "a manual read from the match callback is legal beside a chunk"
         " callback"),
    "shell_injected_backend_ignored":
        ("shell", "grep_uses_the_injected_backend_without_filesystem_io",
         "grep must query the injected backend"),
    "grammar_bare_leading_minus_accepted":
        ("shell", "grep_bad_grammar_is_usage_without_io",
         "invalid grep grammar must produce USAGE rather than search"),
    "grammar_inner_quote_accepted":
        ("shell", "grep_bad_grammar_is_usage_without_io",
         "invalid grep grammar must produce USAGE rather than search"),
    "utf8_pending_dropped_on_read_fault":
        ("shell", "grep_read_error_accounts_for_pending_utf8_before_diagnostic",
         "a split UTF-8 lead must be flushed before the diagnostic"),
    "record_crlf_doubled":
        ("shell", "grep_emits_a_matching_line_once",
         "multiple occurrences must yield one record per logical line"),
}

MUTATIONS = [
    # --- os/search_linear.c ------------------------------------------------
    {
        "name": "line_start_kmp_not_reset",
        "file": "os/search_linear.c",
        "old": ("        self->line_content = 0u;\n"
                "        self->kmp = 0u;\n"
                "        self->line_has_match = false;\n"),
        "new": ("        self->line_content = 0u;\n"
                "        self->line_has_match = false;\n"),
    },
    {
        "name": "scan_chunk_kmp_reset",
        "file": "os/search_linear.c",
        "old": ("        if (got == 0u) {\n"
                "            break;\n"
                "        }\n"
                "        for (uint32_t i = 0u; i < got; ++i) {\n"),
        "new": ("        if (got == 0u) {\n"
                "            break;\n"
                "        }\n"
                "        self->kmp = 0u;\n"
                "        for (uint32_t i = 0u; i < got; ++i) {\n"),
    },
    {
        "name": "preflight_nul_bypassed",
        "file": "os/search_linear.c",
        "old": ("            if (self->scan[i] == 0u) {\n"
                "                *has_nul = true;\n"),
        "new": ("            if (false) {\n"
                "                *has_nul = true;\n"),
    },
    {
        "name": "fs_io_mapped_to_ok",
        "file": "os/search_linear.c",
        "old": "    case YAN_FS_IO: return YAN_SEARCH_IO;\n",
        "new": "    case YAN_FS_IO: return YAN_SEARCH_OK;\n",
    },
    {
        "name": "lf_pending_line_not_advanced",
        "file": "os/search_linear.c",
        "old": ("        self->pending_new_line = true;\n"
                "        return;\n"
                "    }\n"
                "    linear_feed_content(self, byte);\n"),
        "new": ("        self->pending_new_line = false;\n"
                "        return;\n"
                "    }\n"
                "    linear_feed_content(self, byte);\n"),
    },
    {
        "name": "bare_cr_dropped",
        "file": "os/search_linear.c",
        "old": ("        /* The CR was not followed by LF, so it is content. */\n"
                "        linear_feed_content(self, (uint8_t)'\\r');\n"),
        "new": ("        /* The CR was not followed by LF, so it is dropped. */\n"
                "        (void)0;\n"),
    },
    {
        "name": "final_line_ignored",
        "file": "os/search_linear.c",
        "old": ("    if (self->pending_new_line || self->line_content == 0u) {\n"
                "        return;\n"
                "    }\n"),
        "new": ("    if (true) {\n"
                "        return;\n"
                "    }\n"),
    },
    # --- os/search.c -------------------------------------------------------
    {
        "name": "content_stream_truncated",
        "file": "os/search.c",
        "old": "    while (offset < total) {\n",
        "new": "    while (offset < total / 2u) {\n",
    },
    {
        "name": "zero_progress_treated_ok",
        "file": "os/search.c",
        "old": ("    if ((got_length == 0u && remaining > 0u) || got_length > capacity ||\n"),
        "new": ("    if (got_length > capacity ||\n"),
    },
    {
        "name": "sticky_reader_error_dropped",
        "file": "os/search.c",
        "old": ("    if (search->reader_error != YAN_SEARCH_OK) {\n"
                "        return search->reader_error;\n"
                "    }\n"
                "    if (!result_known(result)) {\n"),
        "new": ("    if (false) {\n"
                "        return search->reader_error;\n"
                "    }\n"
                "    if (!result_known(result)) {\n"),
    },
    {
        "name": "alias_guard_omitted",
        "file": "os/search.c",
        "old": ("        ranges_overlap(bytes, sizeof *bytes, length, sizeof *length)) {\n"
                "        return YAN_SEARCH_INVALID;\n"
                "    }\n"),
        "new": ("        ranges_overlap(bytes, sizeof *bytes, length, sizeof *length)) {\n"
                "        (void)0;\n"
                "    }\n"),
    },
    {
        "name": "manual_read_blocked",
        "file": "os/search.c",
        "old": ("    search->current_match = *match;\n"
                "    search->in_callback = true;\n"
                "    bool keep = bridge->sink.match(bridge->sink.context,"
                " match);\n"),
        "new": ("    search->current_match = *match;\n"
                "    search->in_callback = true;\n"
                "    search->reader_busy = true;\n"
                "    bool keep = bridge->sink.match(bridge->sink.context,"
                " match);\n"),
    },
    # --- os/shell.c --------------------------------------------------------
    {
        "name": "shell_injected_backend_ignored",
        "file": "os/shell.c",
        "old": ("    YanSearchResult result = yan_search_query(shell->search,"
                " pattern,\n"),
        "new": ("    YanSearchResult result = yan_search_query(NULL, pattern,\n"),
    },
    {
        "name": "grammar_bare_leading_minus_accepted",
        "file": "os/shell.c",
        "old": "    if (end == start || line[start] == (uint8_t)'-') {\n",
        "new": "    if (end == start) {\n",
    },
    {
        "name": "grammar_inner_quote_accepted",
        "file": "os/shell.c",
        "old": ("        if (line[end] == (uint8_t)'\"') {\n"
                "            return false;\n"
                "        }\n"),
        "new": ("        if (false) {\n"
                "            return false;\n"
                "        }\n"),
    },
    {
        "name": "utf8_pending_dropped_on_read_fault",
        "file": "os/shell.c",
        "old": ("            if (!cat_flush(shell, &stream) || !shell_puts(shell,"
                " \"\\r\\n\") ||\n"
                "                !emit_search_error(shell, result)) {\n"),
        "new": ("            if (!shell_puts(shell, \"\\r\\n\") ||\n"
                "                !emit_search_error(shell, result)) {\n"),
    },
    {
        "name": "record_crlf_doubled",
        "file": "os/shell.c",
        "old": ("    if (!cat_flush(shell, &stream) || !shell_puts(shell,"
                " \"\\r\\n\")) {\n"
                "        state->output_failed = true;\n"
                "        return false;\n"
                "    }\n"
                "    return true;\n"
                "}\n"),
        "new": ("    if (!cat_flush(shell, &stream) || !shell_puts(shell,"
                " \"\\r\\n\") ||\n"
                "        !shell_puts(shell, \"\\r\\n\")) {\n"
                "        state->output_failed = true;\n"
                "        return false;\n"
                "    }\n"
                "    return true;\n"
                "}\n"),
    },
]

# A real text replacement that changes nothing: the suites must still pass.
NO_EFFECT = {
    "name": "no_effect_parenthesised_known_result",
    "file": "os/search.c",
    "old": "    return (int)result >= (int)YAN_SEARCH_OK &&\n",
    "new": "    return ((int)result >= (int)YAN_SEARCH_OK) &&\n",
    "owner": "real_search_does_not_match_across_a_line_break",
    "assertion": "bytes split by LF must not form a match across lines",
}

# A real defect (the zero-progress facade hole) declared against the wrong
# owner: some other test fails, so this must be wrong-owner, not a detection.
WRONG_OWNER = {
    "name": "wrong_owner_zero_progress",
    "file": "os/search.c",
    "old": ("    if ((got_length == 0u && remaining > 0u) || got_length > capacity ||\n"),
    "new": ("    if (got_length > capacity ||\n"),
    "owner": "real_search_does_not_match_across_a_line_break",
    "assertion": "bytes split by LF must not form a match across lines",
}

# The real owner fails, but the declared assertion is unrelated. Merely
# finding that owner's FAIL record must not count as detecting this contract.
WRONG_ASSERTION = dict(WRONG_OWNER,
    name="same_owner_wrong_assertion_zero_progress",
    owner="manual_reader_zero_progress_before_end_is_protocol",
    assertion="UNRELATED-ASSERTION-MUST-NOT-BE-CREDITED")


def _load(path, name):
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


_IMPORT_ERROR = None
try:
    _YANFS = _load(Path(__file__).with_name("run_yanfs_mutation.py"),
                   "search_yanfs_mut_ref")
except Exception as error:  # a missing or broken shared helper is a hard failure
    _YANFS = None
    _IMPORT_ERROR = error
if _YANFS is not None:
    parse_unity = _YANFS.parse_unity
    classify_run = _YANFS.classify_run
    unity_suite = _YANFS.unity_suite
    HarnessError = _YANFS.HarnessError
    apply_replacement = _YANFS.apply_replacement
    format_build_report = _YANFS.format_build_report
    unity_object = _YANFS.unity_object
    write_log = _YANFS.write_log
    owner_fail_line = _YANFS.owner_fail_line
    run_suites = _YANFS.run_suites
else:
    parse_unity = None
    classify_run = None
    unity_suite = None
    apply_replacement = None
    format_build_report = None
    unity_object = None
    write_log = None
    owner_fail_line = None
    run_suites = None

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
    unity = tree / "unity"
    out_dir = tree / "bin"
    out_dir.mkdir(exist_ok=True)
    obj = unity_object(tree, cc)
    spec = SUITES[suite]
    sources = [tree / rel for rel in spec["sources"]]
    target = out_dir / spec["target"]
    command = [cc] + flags + ["-I", str(tree / "os"), "-I", str(unity)] + \
              [str(path) for path in sources] + [str(obj), "-o", str(target)]
    try:
        proc = subprocess.run(command, capture_output=True, timeout=BUILD_TIMEOUT)
    except subprocess.TimeoutExpired as error:
        raise HarnessError("build timeout: %s" % suite) from error
    return proc, target, command


def build_both(tree, cc, strict):
    core_proc, core, core_command = compile_suite(tree, cc, "core", strict)
    shell_proc, shell, shell_command = compile_suite(tree, cc, "shell", strict)
    report = format_build_report([("core", core_command, core_proc),
                                  ("shell", shell_command, shell_proc)])
    return core_proc, core, shell_proc, shell, report


def owner_exists(source, owner, suite):
    test = SUITES[suite]["test"]
    text = (source / test).read_text()
    return ("RUN_TEST(%s)" % owner) in text and \
           ("static void %s(void)" % owner) in text


def run_mutation(source, tree_root, cc, mutation, logdir):
    owner = mutation["owner"]
    assertion = mutation.get("assertion")
    if not assertion:
        raise HarnessError("%s: no required assertion marker" % mutation["name"])
    tree = tree_root / ("mut-" + mutation["name"])
    shutil.copytree(tree_root / "baseline", tree,
                    ignore=shutil.ignore_patterns("bin"))
    apply_replacement(tree / mutation["file"], mutation["old"], mutation["new"])
    core_build, core_bin, shell_build, shell_bin, build_report = build_both(
        tree, cc, False)
    if core_build.returncode != 0 or shell_build.returncode != 0:
        write_log(logdir, mutation["name"], build_report)
        raise HarnessError("%s: mutant build failed" % mutation["name"])
    verdict, blob = run_suites(core_bin, shell_bin, owner, assertion)
    write_log(logdir, mutation["name"], build_report + b"\n" + blob)
    return verdict, blob


def require_in_repo(source):
    missing = [str(source / "os" / name) for name in OS_HEADERS + OS_SOURCES
               if not (source / "os" / name).is_file()]
    missing += [str(source / "tests" / name) for name in REQUIRED_FILES
                if not (source / "tests" / name).is_file()]
    return missing


def main():
    parser = argparse.ArgumentParser(description="Search mutation gate")
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
    except HarnessError as error:
        print("HARNESS-ERROR: %s" % error, file=sys.stderr)
        return 2
    print("PASS mocked run_suites precedence controls")

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
    logdir = args.work / "search-mutation"
    logdir.mkdir(exist_ok=True)
    tree_root = Path(tempfile.mkdtemp(prefix="search-mut-", dir=args.work))
    harness = []
    survivors = []
    try:
        copy_tree(source, unity, tree_root / "baseline")
        core_build, core_bin, shell_build, shell_bin, build_report = build_both(
            tree_root / "baseline", args.cc, True)
        if core_build.returncode != 0 or shell_build.returncode != 0:
            write_log(logdir, "baseline", build_report)
            print("HARNESS-ERROR: strict baseline build failed", file=sys.stderr)
            return 2
        verdict, blob = run_suites(core_bin, shell_bin, "", None)
        write_log(logdir, "baseline", build_report + b"\n" + blob)
        if verdict != "pass":
            print("HARNESS-ERROR: baseline is %s, not a clean pass" % verdict,
                  file=sys.stderr)
            return 2
        print("PASS baseline builds strict and both suites pass")

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
