#!/usr/bin/env python3
"""Editor mutation gate: does the 0023 native suite actually detect real defects?

What this does
--------------
It copies a minimal tree out of the repository into a scratch directory
(os/editor.{h,c}, os/yanfs.{h,c}, os/shell.h, os/search.h, tests/test_editor.c
and the host Unity sources), builds tests/test_editor.c against the real editor
and filesystem with the repository's strict C17 warning bar
(-std=c17 -O2 -Wall -Wextra -Wpedantic -Werror), runs it, then applies one
exact, single-occurrence source replacement per mutation and repeats the
build/run. A mutation counts as *detected* only when the owning test named in
OWNING_ASSERTIONS fails on the required assertion marker:

    detected       the owner test reports :FAIL: on the marker and no
                   harness-level condition (signal, timeout, sanitizer, fixture
                   abort, build failure, Unity summary disagreement) is present
    survivor       the mutant run still passes
    wrong-owner    some test fails, but not the declared owner
    wrong-assertion the owner fails, but not on the required marker
    harness-error  build failure, crash, signal, timeout, sanitizer, abort, or an
                   exit code and Unity summary that disagree

Only "detected" is a pass. The strict classifier is reused from
tests/run_yanfs_mutation.py by import (classify_run / parse_unity /
unity_object / run_binary / format_build_report); the shared script is not
edited and no classifier is weakened.

A baseline and two real negative controls run before the mutants: a no-effect
comment replacement that must survive, and a wrong-owner table entry (a real
mutation declared against a test that passes) that must not be credited.

Exit codes: 0 all detected, 1 survivor/wrong-owner, 2 harness error or a failed
baseline/control, 77 the external C compiler or Unity is missing. Everything in
the repository that is compiled is a hard failure (1), never 77.
"""
import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

_SHARED = None
_IMPORT_ERROR = None
try:
    import run_yanfs_mutation as _SHARED
except Exception as error:  # the shared helper is a repository input
    _IMPORT_ERROR = error

if _SHARED is not None:
    classify_run = _SHARED.classify_run
    parse_unity = _SHARED.parse_unity
    unity_object = _SHARED.unity_object
    run_binary = _SHARED.run_binary
    format_build_report = _SHARED.format_build_report
    apply_replacement = _SHARED.apply_replacement
    HarnessError = _SHARED.HarnessError
    owner_fail_line = _SHARED.owner_fail_line
else:  # a stand-in so the module still imports; main() fails closed
    class HarnessError(RuntimeError):
        """Placeholder when the shared helper could not be imported."""

    classify_run = None
    parse_unity = None
    unity_object = None
    run_binary = None
    format_build_report = None
    apply_replacement = None
    owner_fail_line = None

STRICT_FLAGS = ["-std=c17", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror"]
MUTANT_FLAGS = ["-std=c17", "-O2", "-Wall", "-Wextra", "-Wpedantic"]
BUILD_TIMEOUT = 180

# The whole tree the editor suite needs. Every one is a hard repository input.
EDITOR_SOURCES = ["tests/test_editor.c", "os/editor.c", "os/yanfs.c"]
EDITOR_OS = ["editor.h", "editor.c", "yanfs.h", "yanfs.c", "shell.h", "search.h"]
REQUIRED_REPO = (
    ["os/" + name for name in EDITOR_OS]
    + ["tests/test_editor.c", "tests/run_yanfs_mutation.py"]
)

# mutation name -> (owning Unity test function, required assertion marker).
# The marker is the exact FAIL message the intended owning assertion prints in
# the verified run; a same-owner failure on any other assertion is rejected.
OWNING_ASSERTIONS = {
    "closing_lf_dropped":
        ("append_creates_lf_terminated_lines",
         "EDT append closing LF dropped"),
    "separator_always_added":
        ("append_after_a_terminated_line_adds_no_separator",
         "EDT append added a separator after a terminated draft"),
    "loaded_crlf_normalized":
        ("start_loads_existing_bytes_without_normalizing",
         "EDT load normalized CRLF"),
    "oversize_truncated_to_capacity":
        ("start_rejects_oversize_file_without_publishing_a_prefix",
         "EDT oversize load truncated to capacity"),
    "quit_calls_create_or_replace":
        ("quit_discards_without_any_io",
         "EDT quit performed filesystem IO"),
    "append_immediate_write":
        ("append_separates_an_unterminated_last_line",
         "EDT append performed filesystem IO"),
    "save_create_replace_swapped":
        ("save_new_draft_creates_the_file",
         "EDT save used the wrong operation for the existing flag"),
    "load_read_failure_publishes_prefix":
        ("start_rejects_an_incomplete_load_and_leaves_nothing_savable",
         "EDT failed load published a draft prefix"),
    "capacity_refusal_mutates_draft":
        ("append_at_exact_capacity_succeeds_and_one_more_is_refused",
         "EDT capacity refusal mutated the draft"),
    "output_failure_w_returns_exit":
        ("save_output_failure_after_commit_keeps_the_file",
         "EDT failed save output returned exit"),
    "embedded_nul_guard_removed":
        ("start_validates_the_entire_length_delimited_name",
         "EDT full name cannot truncate at NUL"),
}

MUTATIONS = [
    {
        # `a` stops writing the closing LF, so the draft is one byte short.
        "name": "closing_lf_dropped",
        "file": "os/editor.c",
        "old": "    at += text_length;\n"
               "    editor->draft[at] = EDITOR_LF;\n"
               "    ++at;\n"
               "    editor->length = at;\n",
        "new": "    at += text_length;\n"
               "    editor->length = at;\n",
    },
    {
        # A separator LF is always added, even after a terminated draft.
        "name": "separator_always_added",
        "file": "os/editor.c",
        "old": "    bool separator =\n"
               "        editor->length > 0u && editor->draft[editor->length - 1u] != EDITOR_LF;\n",
        "new": "    bool separator = editor->length > 0u;\n",
    },
    {
        # A loaded CR byte is rewritten to LF: the draft is no longer the file.
        "name": "loaded_crlf_normalized",
        "file": "os/editor.c",
        "old": "        if (!draft_is_valid(editor->draft, info.size_bytes)) {\n"
               "            return editor_emit_own_error(editor, \"INVALID_TEXT\");\n"
               "        }\n"
               "        editor->existing = true;\n"
               "        editor->length = info.size_bytes;\n",
        "new": "        if (!draft_is_valid(editor->draft, info.size_bytes)) {\n"
               "            return editor_emit_own_error(editor, \"INVALID_TEXT\");\n"
               "        }\n"
               "        for (uint32_t i = 0u; i + 1u < info.size_bytes; ++i) {\n"
               "            if (editor->draft[i] == EDITOR_CR && editor->draft[i + 1u] == EDITOR_LF) {\n"
               "                editor->draft[i] = EDITOR_LF; /* mutant: normalize loaded CRLF */\n"
               "            }\n"
               "        }\n"
               "        editor->existing = true;\n"
               "        editor->length = info.size_bytes;\n",
    },
    {
        # An oversize file is truncated to capacity instead of refused whole.
        "name": "oversize_truncated_to_capacity",
        "file": "os/editor.c",
        "old": "        if (info.size_bytes > YAN_EDITOR_CAPACITY) {\n"
               "            return editor_emit_own_error(editor, \"TOO_LARGE\");\n"
               "        }\n",
        "new": "        if (info.size_bytes > YAN_EDITOR_CAPACITY) {\n"
               "            info.size_bytes = YAN_EDITOR_CAPACITY;\n"
               "        }\n",
    },
    {
        # `q` writes the draft before discarding it.
        "name": "quit_calls_create_or_replace",
        "file": "os/editor.c",
        "old": "static YanEditorResult run_quit(YanEditor *editor)\n"
               "{\n"
               "    editor->active = false;\n",
        "new": "static YanEditorResult run_quit(YanEditor *editor)\n"
               "{\n"
               "    const uint8_t *bytes = editor->length > 0u ? editor->draft : NULL;\n"
               "    (void)(editor->existing\n"
               "               ? yan_fs_replace(editor->fs, editor->name, bytes, editor->length)\n"
               "               : yan_fs_create(editor->fs, editor->name, bytes, editor->length));\n"
               "    editor->active = false;\n",
    },
    {
        # `a` writes the whole draft immediately instead of editing memory.
        "name": "append_immediate_write",
        "file": "os/editor.c",
        "old": "    editor->length = at;\n"
               "    return editor_emit_ok(editor, \"a\") ? YAN_EDITOR_OK : YAN_EDITOR_FATAL;\n",
        "new": "    editor->length = at;\n"
               "    {\n"
               "        const uint8_t *bytes = editor->length > 0u ? editor->draft : NULL;\n"
               "        (void)(editor->existing\n"
               "                   ? yan_fs_replace(editor->fs, editor->name, bytes, editor->length)\n"
               "                   : yan_fs_create(editor->fs, editor->name, bytes, editor->length));\n"
               "    }\n"
               "    return editor_emit_ok(editor, \"a\") ? YAN_EDITOR_OK : YAN_EDITOR_FATAL;\n",
    },
    {
        # create/replace are swapped for the recorded existing flag.
        "name": "save_create_replace_swapped",
        "file": "os/editor.c",
        "old": "    YanFsResult result =\n"
               "        editor->existing\n"
               "            ? yan_fs_replace(editor->fs, editor->name, bytes, editor->length)\n"
               "            : yan_fs_create(editor->fs, editor->name, bytes, editor->length);\n",
        "new": "    YanFsResult result =\n"
               "        editor->existing\n"
               "            ? yan_fs_create(editor->fs, editor->name, bytes, editor->length)\n"
               "            : yan_fs_replace(editor->fs, editor->name, bytes, editor->length);\n",
    },
    {
        # A failed load publishes the partial prefix and reports success.
        "name": "load_read_failure_publishes_prefix",
        "file": "os/editor.c",
        "old": "        result = yan_fs_read(editor->fs, local_name, 0u, editor->draft,\n"
               "                             info.size_bytes, &read_bytes);\n"
               "        if (result != YAN_FS_OK) {\n"
               "            return editor_finish_fs_error(editor, result);\n"
               "        }\n",
        "new": "        result = yan_fs_read(editor->fs, local_name, 0u, editor->draft,\n"
               "                             info.size_bytes, &read_bytes);\n"
               "        if (result != YAN_FS_OK) {\n"
               "            editor->existing = true;\n"
               "            editor->length = read_bytes;\n"
               "            if (!editor_emit_ok(editor, \"edit\")) {\n"
               "                return YAN_EDITOR_FATAL;\n"
               "            }\n"
               "            editor->active = true;\n"
               "            return YAN_EDITOR_OK;\n"
               "        }\n",
    },
    {
        # A capacity refusal mutates draft[0] before reporting the error.
        "name": "capacity_refusal_mutates_draft",
        "file": "os/editor.c",
        "old": "    if (added > YAN_EDITOR_CAPACITY - editor->length) {\n"
               "        return editor_emit_own_error(editor, \"TOO_LARGE\");\n"
               "    }\n",
        "new": "    if (added > YAN_EDITOR_CAPACITY - editor->length) {\n"
               "        if (editor->length > 0u) {\n"
               "            editor->draft[0] = (uint8_t)'Z';\n"
               "        }\n"
               "        return editor_emit_own_error(editor, \"TOO_LARGE\");\n"
               "    }\n",
    },
    {
        # A failed `w` confirmation is ignored and reported as a healthy exit.
        "name": "output_failure_w_returns_exit",
        "file": "os/editor.c",
        "old": "    if (!editor_emit_ok(editor, \"w\")) {\n"
               "        return YAN_EDITOR_FATAL;\n"
               "    }\n"
               "    return YAN_EDITOR_EXIT;\n",
        "new": "    (void)editor_emit_ok(editor, \"w\");\n"
               "    return YAN_EDITOR_EXIT;\n",
    },
    {
        # The embedded-NUL guard is removed, so the name truncates inside YanFS.
        "name": "embedded_nul_guard_removed",
        "file": "os/editor.c",
        "old": "    for (uint32_t i = 0u; i < name_length; ++i) {\n"
               "        if (name[i] == 0u) {\n"
               "            return editor_emit_own_error(editor, \"INVALID\");\n"
               "        }\n"
               "    }\n",
        "new": "    /* mutant: the embedded-NUL guard is removed */\n",
    },
]

# Two real controls. The no-effect replacement compiles and must pass; the
# wrong-owner entry reuses a real mutation but declares a test that passes.
NO_EFFECT = {
    "name": "no-effect-control",
    "file": "os/editor.c",
    "old": "#define EDITOR_LF UINT8_C(0x0a)\n",
    "new": "#define EDITOR_LF UINT8_C(0x0a) /* no-effect control */\n",
    "owner": "append_creates_lf_terminated_lines",
    "assertion": "no-effect-control",
}
WRONG_OWNER = {
    "name": "wrong-owner-control",
    "file": "os/editor.c",
    "old": "    at += text_length;\n"
           "    editor->draft[at] = EDITOR_LF;\n"
           "    ++at;\n"
           "    editor->length = at;\n",
    "new": "    at += text_length;\n"
           "    editor->length = at;\n",
    "owner": "print_of_an_empty_draft_only_confirms",
    "assertion": "wrong-owner-control",
}


def run_command(argv, timeout):
    try:
        return subprocess.run(argv, capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired as error:
        raise HarnessError("timeout: %s" % " ".join(argv)) from error
    except OSError as error:
        raise HarnessError("cannot run %s: %s" % (argv[0], error)) from error


def compile_editor(tree, cc, strict):
    """Build tests/test_editor.c over the real editor and filesystem."""
    flags = STRICT_FLAGS if strict else MUTANT_FLAGS
    out_dir = tree / "bin"
    out_dir.mkdir(exist_ok=True)
    obj = unity_object(tree, cc)
    target = out_dir / "test_editor"
    command = ([cc] + flags + ["-I", str(tree / "os"), "-I", str(tree / "unity")]
               + [str(tree / rel) for rel in EDITOR_SOURCES]
               + [str(obj), "-o", str(target)])
    proc = run_command(command, BUILD_TIMEOUT)
    return proc, target, command


def copy_tree(source, unity, destination):
    (destination / "os").mkdir(parents=True)
    (destination / "tests").mkdir()
    (destination / "unity").mkdir()
    for name in EDITOR_OS:
        shutil.copy2(source / "os" / name, destination / "os" / name)
    shutil.copy2(source / "tests/test_editor.c",
                 destination / "tests/test_editor.c")
    for item in sorted((unity / "src").iterdir()):
        if item.is_file():
            shutil.copy2(item, destination / "unity" / item.name)


def write_log(logdir, name, blob):
    (logdir / (name + ".log")).write_bytes(blob)


def run_mutation(source, tree_root, cc, mutation, logdir):
    owner = mutation["owner"]
    assertion = mutation["assertion"]
    if not assertion:
        raise HarnessError("%s: no required assertion marker" % mutation["name"])
    tree = tree_root / ("mut-" + mutation["name"])
    shutil.copytree(tree_root / "baseline", tree,
                    ignore=shutil.ignore_patterns("bin"))
    apply_replacement(tree / mutation["file"], mutation["old"], mutation["new"])
    proc, target, command = compile_editor(tree, cc, False)
    report = format_build_report([("editor", command, proc)])
    if proc.returncode != 0:
        write_log(logdir, mutation["name"], report)
        raise HarnessError("%s: mutant build failed" % mutation["name"])
    rc, out, err = run_binary(target)
    write_log(logdir, mutation["name"],
              report + b"\nrc=%d\n" % (rc if rc is not None else -1) + out + err)
    verdict = classify_run(rc, out, err, owner, assertion)
    return verdict, out + err, report


def owner_exists(source, owner):
    text = (source / "tests/test_editor.c").read_text()
    return (("RUN_TEST(%s)" % owner) in text
            and ("static void %s(void)" % owner) in text)


def main():
    parser = argparse.ArgumentParser(description="editor mutation gate")
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--work", required=True, type=Path)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--unity", required=True, type=Path)
    args = parser.parse_args()

    # External first: only the compiler and Unity can skip.
    if shutil.which(args.cc) is None:
        print("SKIP: external C compiler missing: %s" % args.cc)
        return 77
    unity = args.unity.resolve()
    if not (unity / "src/unity.c").is_file() or not (unity / "src/unity.h").is_file():
        print("SKIP: external Unity missing under %s/src" % unity)
        return 77

    source = args.source.resolve()
    missing = [str(source / rel) for rel in REQUIRED_REPO
               if not (source / rel).is_file()]
    if missing:
        print("FAIL the source/tests under test are missing: %s"
              % ", ".join(missing), file=sys.stderr)
        return 1
    if _SHARED is None:
        print("FAIL the shared mutation helper tests/run_yanfs_mutation.py is not"
              " importable: %r" % _IMPORT_ERROR, file=sys.stderr)
        return 1
    for name, (owner, _marker) in OWNING_ASSERTIONS.items():
        if not owner_exists(source, owner):
            print("FAIL owning test %s is not registered for %s" % (owner, name),
                  file=sys.stderr)
            return 1

    args.work.mkdir(parents=True, exist_ok=True)
    logdir = args.work / "editor-mutation"
    logdir.mkdir(exist_ok=True)
    tree_root = Path(tempfile.mkdtemp(prefix="editor-mut-", dir=args.work))
    harness = []
    survivors = []
    try:
        copy_tree(source, unity, tree_root / "baseline")
        proc, target, command = compile_editor(tree_root / "baseline", args.cc,
                                               True)
        report = format_build_report([("editor", command, proc)])
        if proc.returncode != 0:
            write_log(logdir, "baseline", report)
            print("HARNESS-ERROR: strict baseline build failed", file=sys.stderr)
            return 2
        rc, out, err = run_binary(target)
        write_log(logdir, "baseline",
                  report + b"\nrc=%d\n" % (rc if rc is not None else -1)
                  + out + err)
        verdict = classify_run(rc, out, err, "", "")
        if verdict != "pass":
            print("HARNESS-ERROR: baseline is %s, not a clean pass" % verdict,
                  file=sys.stderr)
            return 2
        parsed = parse_unity(out)
        print("PASS baseline builds strict and passes (%d tests)" % parsed[0])

        for control, expected in ((NO_EFFECT, "pass"), (WRONG_OWNER, "wrong-owner")):
            entry = dict(control)
            try:
                outcome, _blob, _report = run_mutation(source, tree_root, args.cc,
                                                       entry, logdir)
            except HarnessError as error:
                print("HARNESS-ERROR: control %s: %s"
                      % (control["name"], error), file=sys.stderr)
                return 2
            if outcome != expected:
                print("HARNESS-ERROR: control %s was %s, expected %s"
                      % (control["name"], outcome, expected), file=sys.stderr)
                return 2
            print("PASS control %s: %s" % (control["name"], outcome))

        for mutation in MUTATIONS:
            entry = dict(mutation)
            owner, assertion = OWNING_ASSERTIONS[mutation["name"]]
            entry["owner"] = owner
            entry["assertion"] = assertion
            try:
                outcome, blob, _report = run_mutation(source, tree_root, args.cc,
                                                      entry, logdir)
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
                print("HARNESS-ERROR %s: run classified as harness (never"
                      " detected)" % mutation["name"])
            elif outcome == "wrong-assertion":
                survivors.append((mutation["name"], "wrong assertion"))
                print("FAIL mutant %s: owner %s failed, but not on the required"
                      " assertion %r" % (mutation["name"], owner, assertion))
            elif outcome == "wrong-owner":
                survivors.append((mutation["name"], "wrong owner"))
                print("FAIL mutant %s: wrong owner (a test other than %s failed)"
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
