#!/usr/bin/env python3
"""Real production Guest acceptance for the 0025 `grep` front end.

This drives the *production* terminal ELF over the real executor: it never
compiles its own shell and never reimplements the search core. Fixtures are
valid images written by this file's own small encoder (or by the real mkfs for
a fresh directory), the expected records are hand-listed bytes produced by
CPython's strict UTF-8 decoder, and every read-only case compares the whole
image byte for byte before and after.

The output interval of one command is bounded exactly: the record starts after
the echoed command's CRLF and ends at the next prompt, so an echoed pattern can
never be mistaken for a result.

A missing in-repo source, header, script or the provided real ELF is a hard
failure (exit 1); no case is allowed to skip. A crash, timeout or sanitizer
report is a harness error (exit 2). A named case failure prints
":FAIL: <owner>: ..." and makes the script return 1.
"""

import argparse
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_terminal_files as base  # noqa: E402  (path installed just above)
import run_editor as editor  # noqa: E402  (path installed just above)

BLOCKS = base.BLOCKS
MAX_STEPS = base.MAX_STEPS
TIMEOUT = base.TIMEOUT
display = base.display

# Every repository input this script reads, plus the shared scripts it imports.
PRODUCTION_INPUTS = tuple(editor.PRODUCTION_INPUTS) + (
    "tests/guest/run_search.py", "tests/guest/run_editor.py",
)


HarnessError = base.HarnessError


# ------------------------------------------------------------- small helpers

def fresh_image(mkfs, path):
    # Each invocation owns this fixture path. mkfs deliberately refuses to
    # overwrite an existing image, so remove our previous run's fixture first.
    Path(path).unlink(missing_ok=True)
    base.fresh_image(mkfs, path)


def seed_image(path, files):
    return base.seed_image(path, files)


def run_guest(executor, elf, image, stdin, work, label):
    return base.run_guest(executor, elf, image, stdin, work, label,
                          max_steps=MAX_STEPS)


def require_rc(result, expected, owner):
    base.check_not_harness(result, owner)
    if result.returncode not in (0, 6):
        raise HarnessError("%s: unexpected executor exit %d"
                           % (owner, result.returncode))
    base.require_rc(result, expected, owner)


def require_fragment(result, text, owner):
    base.require_fragment(result, text, owner)


def unchanged(disk, before, owner):
    if Path(disk).read_bytes() != before:
        raise AssertionError("%s: the image changed" % owner)


def check_image(disk, entries, blocks, owner):
    editor.check_image(disk, entries, blocks, owner)


def record(name, line, content):
    """One FILE:LINE:CONTENT\\r\\n record, content through the cat oracle."""
    return (name.encode() + b":" + str(line).encode() + b":" +
            display(content) + b"\r\n")


def ok():
    return b"OK grep\r\n"


def command_interval(stdout, command, owner):
    """The exact bytes one command produced: after its echoed CRLF, up to the
    next SHELL prompt. The echo itself is excluded, so a pattern repeated in
    the echoed line cannot be read as a record."""
    prefix = b"yanfs> " + command + b"\r\n"
    at = stdout.find(prefix)
    if at < 0:
        raise AssertionError("%s: cannot find the echoed %r" % (owner, prefix))
    start = at + len(prefix)
    end = stdout.find(b"yanfs> ", start)
    if end < 0:
        raise AssertionError("%s: no prompt after the interval" % owner)
    return stdout[start:end]


def expect_interval(stdout, command, expected, owner):
    got = command_interval(stdout, command, owner)
    if got != expected:
        raise AssertionError(
            "%s: interval for %r differs\nactual   %r\nexpected %r"
            % (owner, command, got, expected))


# ------------------------------------------------------------------- cases

def case_grammar_miss_and_literal(case_dir, args):
    """One directory with a spaced, dash-prefixed line: the approved grammar
    accepts exactly one non-empty pattern and every other form is USAGE."""
    disk = Path(case_dir) / "grammar.img"
    seed_image(disk, [("f", b"-i here\nplain", 1)])
    before = disk.read_bytes()
    run = run_guest(args.run, args.guest, disk,
                    b"grep needle\n"
                    b"grep -i\n"
                    b"grep \"\"\n"
                    b"grep \"needle\n"
                    b"grep needle extra\n"
                    b"grep \"needle\" \"other\"\n"
                    b"grep nee\"dle\n"
                    b"grep \"-i\"\n"
                    b"grep \" \"\n"
                    b"exit\n",
                    case_dir, "grammar")
    require_rc(run, 0, "grammar")
    out = run.stdout
    expect_interval(out, b"grep needle", ok(), "grammar-miss")
    expect_interval(out, b"grep -i", b"ERROR USAGE\r\n", "grammar-bare-minus")
    expect_interval(out, b"grep \"\"", b"ERROR USAGE\r\n", "grammar-empty-quote")
    expect_interval(out, b"grep \"needle", b"ERROR USAGE\r\n", "grammar-unclosed")
    expect_interval(out, b"grep needle extra", b"ERROR USAGE\r\n",
                    "grammar-extra-token")
    expect_interval(out, b"grep \"needle\" \"other\"", b"ERROR USAGE\r\n",
                    "grammar-second-quoted")
    expect_interval(out, b"grep nee\"dle", b"ERROR USAGE\r\n",
                    "grammar-inner-quote")
    expect_interval(out, b"grep \"-i\"", record("f", 1, b"-i here") + ok(),
                    "grammar-quoted-dash")
    expect_interval(out, b"grep \" \"", record("f", 1, b"-i here") + ok(),
                    "grammar-quoted-space")
    unchanged(disk, before, "grammar")


def case_slot_hole_order(case_dir, args):
    """Physical slot order with an empty slot between live entries."""
    disk = Path(case_dir) / "hole.img"
    entries = [(0, "a", 1, 1, 1), (2, "c", 1, 2, 1), (3, "d", 1, 3, 1)]
    image = base.expected_image(entries, {1: b"x", 2: b"x", 3: b"x"})
    disk.write_bytes(image)
    run = run_guest(args.run, args.guest, disk, b"grep x\nexit\n",
                    case_dir, "hole")
    require_rc(run, 0, "hole")
    expected = (record("a", 1, b"x") + record("c", 1, b"x") +
                record("d", 1, b"x") + ok())
    expect_interval(run.stdout, b"grep x", expected, "slot-hole")
    unchanged(disk, image, "slot-hole")


def case_line_endings(case_dir, args):
    """LF and CRLF end logical lines; a bare CR remains content. A final
    unterminated line is still a line."""
    disk = Path(case_dir) / "lines.img"
    seed_image(disk, [("f", b"x\r\nx\rx\nx", 1)])
    before = disk.read_bytes()
    run = run_guest(args.run, args.guest, disk, b"grep x\nexit\n",
                    case_dir, "lines")
    require_rc(run, 0, "lines")
    expected = (record("f", 1, b"x") + record("f", 2, b"x\rx") +
                record("f", 3, b"x") + ok())
    expect_interval(run.stdout, b"grep x", expected, "line-endings")
    unchanged(disk, before, "line-endings")

    disk2 = Path(case_dir) / "noeof.img"
    seed_image(disk2, [("g", b"A\nB", 1)])
    before2 = disk2.read_bytes()
    run2 = run_guest(args.run, args.guest, disk2, b"grep B\nexit\n",
                     case_dir, "noeof")
    require_rc(run2, 0, "noeof")
    expect_interval(run2.stdout, b"grep B", record("g", 2, b"B") + ok(),
                    "final-unterminated")
    unchanged(disk2, before2, "final-unterminated")


def case_late_nul_excludes_the_file(case_dir, args):
    """A NUL anywhere in a file makes the whole file a non-result, even when a
    hit precedes it; the next file is still searched."""
    disk = Path(case_dir) / "binary.img"
    binary = bytearray(b"x" * 9000)
    binary[0:7] = b"needle\n"
    binary[8999] = 0
    seed_image(disk, [("binary", bytes(binary), 1), ("f", b"needle", 4)])
    before = disk.read_bytes()
    run = run_guest(args.run, args.guest, disk, b"grep needle\nexit\n",
                    case_dir, "binary")
    require_rc(run, 0, "binary")
    expect_interval(run.stdout, b"grep needle",
                    record("f", 1, b"needle") + ok(), "late-nul")
    unchanged(disk, before, "late-nul")


def case_encodings(case_dir, args):
    """A multi-byte pattern whose bytes straddle the 4096 block boundary, and a
    line holding invalid UTF-8 and a C1 control, both through the cat oracle."""
    word = "\u4e2d".encode("utf-8")
    disk = Path(case_dir) / "chinese.img"
    cn = bytearray(b"x" * 4200)
    cn[4095:4098] = word
    seed_image(disk, [("cn", bytes(cn), 1)])
    before = disk.read_bytes()
    run = run_guest(args.run, args.guest, disk,
                    b"grep " + word + b"\nexit\n", case_dir, "chinese")
    require_rc(run, 0, "chinese")
    expect_interval(run.stdout, b"grep " + word,
                    record("cn", 1, bytes(cn)) + ok(), "chinese-boundary")
    unchanged(disk, before, "chinese-boundary")

    disk2 = Path(case_dir) / "badutf8.img"
    body = b"a\x1b[2J\\\t\xff\xc2\x85\n"
    seed_image(disk2, [("bad", body, 1)])
    before2 = disk2.read_bytes()
    run2 = run_guest(args.run, args.guest, disk2, b"grep a\nexit\n",
                     case_dir, "badutf8")
    require_rc(run2, 0, "badutf8")
    expect_interval(run2.stdout, b"grep a",
                    record("bad", 1, body[:-1]) + ok(), "invalid-utf8")
    unchanged(disk2, before2, "invalid-utf8")


def case_long_single_line(case_dir, args):
    """A 20 KiB single line is displayed complete, beyond the editor's 16 KiB
    draft capacity; the hit is at the very end of the line."""
    disk = Path(case_dir) / "long.img"
    big = bytearray(b"A" * 20480)
    big[20474:20480] = b"needle"
    seed_image(disk, [("big", bytes(big), 1)])
    before = disk.read_bytes()
    run = run_guest(args.run, args.guest, disk, b"grep needle\nexit\n",
                    case_dir, "long")
    require_rc(run, 0, "long")
    expect_interval(run.stdout, b"grep needle",
                    record("big", 1, bytes(big)) + ok(), "long-line")
    unchanged(disk, before, "long-line")


def case_lifecycle_rename_copy(case_dir, args):
    """Two real processes: edit creates and saves a note, mv and cp publish new
    names, then a later process greps the new names; the whole image matches an
    independent hand-listed oracle."""
    disk = Path(case_dir) / "lifecycle.img"
    fresh_image(args.mkfs, disk)
    stdin = (b"edit notes.txt\n"
             b"a alpha\n"
             b"a beta keyword\n"
             b"w\n"
             b"mv notes.txt diary.txt\n"
             b"cp diary.txt backup.txt\n"
             b"exit\n")
    run = run_guest(args.run, args.guest, disk, stdin, case_dir, "lifecycle-a")
    require_rc(run, 0, "lifecycle-a")
    require_fragment(run, b"OK w\r\n", "lifecycle-a")
    require_fragment(run, b"OK mv\r\n", "lifecycle-a")
    require_fragment(run, b"OK cp\r\n", "lifecycle-a")
    require_fragment(run, b"yanfs: exit\r\n", "lifecycle-a")
    draft = b"alpha\nbeta keyword\n"
    check_image(disk,
                [(0, "diary.txt", len(draft), 1, 1),
                 (1, "backup.txt", len(draft), 2, 1)],
                {1: draft, 2: draft}, "lifecycle-a")
    before = disk.read_bytes()
    run2 = run_guest(args.run, args.guest, disk, b"grep keyword\nexit\n",
                     case_dir, "lifecycle-b")
    require_rc(run2, 0, "lifecycle-b")
    expected = (record("diary.txt", 2, b"beta keyword") +
                record("backup.txt", 2, b"beta keyword") + ok())
    expect_interval(run2.stdout, b"grep keyword", expected, "lifecycle-new-names")
    unchanged(disk, before, "lifecycle-b")


def case_edit_revise_then_grep(case_dir, args):
    """An edit replaces the only line; the next process misses the old word and
    hits the new one. The raw file is the only truth."""
    disk = Path(case_dir) / "revise.img"
    fresh_image(args.mkfs, disk)
    run = run_guest(args.run, args.guest, disk,
                    b"edit note.txt\na alpha\nr 1 omega\nw\nexit\n",
                    case_dir, "revise-a")
    require_rc(run, 0, "revise-a")
    draft = b"omega\n"
    check_image(disk, [(0, "note.txt", len(draft), 1, 1)], {1: draft},
                "revise-a")
    before = disk.read_bytes()
    run_miss = run_guest(args.run, args.guest, disk, b"grep alpha\nexit\n",
                         case_dir, "revise-miss")
    require_rc(run_miss, 0, "revise-miss")
    expect_interval(run_miss.stdout, b"grep alpha", ok(), "revise-old-miss")
    run_hit = run_guest(args.run, args.guest, disk, b"grep omega\nexit\n",
                        case_dir, "revise-hit")
    require_rc(run_hit, 0, "revise-hit")
    expect_interval(run_hit.stdout, b"grep omega",
                    record("note.txt", 1, b"omega") + ok(), "revise-new-hit")
    unchanged(disk, before, "revise")


def case_editor_state_and_uncommitted_draft(case_dir, args):
    """While EDITING, grep is an unknown editor command and the uncommitted
    draft is invisible; after q discards it, the shell finds nothing."""
    disk = Path(case_dir) / "editor-state.img"
    fresh_image(args.mkfs, disk)
    before = disk.read_bytes()
    stdin = (b"edit note.txt\n"
             b"a hiddenkeyword\n"
             b"grep hiddenkeyword\n"
             b"q\n"
             b"grep hiddenkeyword\n"
             b"exit\n")
    run = run_guest(args.run, args.guest, disk, stdin, case_dir, "editor-state")
    require_rc(run, 0, "editor-state")
    require_fragment(run,
                     b"edit> grep hiddenkeyword\r\nERROR UNKNOWN_COMMAND\r\n",
                     "editor-state-editor")
    require_fragment(run, b"OK q\r\n", "editor-state-q")
    expect_interval(run.stdout, b"grep hiddenkeyword", ok(),
                    "editor-state-shell")
    unchanged(disk, before, "editor-state")


CASES = [
    ("grammar-miss-and-literal", case_grammar_miss_and_literal),
    ("slot-hole-order", case_slot_hole_order),
    ("line-endings", case_line_endings),
    ("late-nul", case_late_nul_excludes_the_file),
    ("encodings", case_encodings),
    ("long-single-line", case_long_single_line),
    ("lifecycle-rename-copy", case_lifecycle_rename_copy),
    ("edit-revise", case_edit_revise_then_grep),
    ("editor-state", case_editor_state_and_uncommitted_draft),
]


def require_inputs(source):
    missing = []
    for rel in PRODUCTION_INPUTS:
        if not (source / rel).is_file():
            missing.append(str(source / rel))
    return missing


def main():
    parser = argparse.ArgumentParser(
        description="real production Guest acceptance for 0025 grep")
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--work", required=True, type=Path)
    parser.add_argument("--run", required=True, type=Path)
    parser.add_argument("--mkfs", required=True, type=Path)
    parser.add_argument("--guest", required=True, type=Path)
    args = parser.parse_args()

    source = args.source.resolve()
    missing = require_inputs(source)
    if missing:
        for line in missing:
            print(":FAIL: missing required input: %s" % line, flush=True)
        return 1
    for label, path in (("executor", args.run), ("mkfs", args.mkfs),
                        ("guest ELF", args.guest)):
        if not Path(path).is_file():
            print(":FAIL: missing required input: %s %s" % (label, path),
                  flush=True)
            return 1

    args.work.mkdir(parents=True, exist_ok=True)
    try:
        base.display_self_check()
    except AssertionError as error:
        print(":HARNESS-ERROR: display oracle: %s" % error, flush=True)
        return 2

    results = []
    for name, case in CASES:
        case_dir = args.work / name
        case_dir.mkdir(parents=True, exist_ok=True)
        try:
            case(case_dir, args)
            results.append((name, None))
        except HarnessError as error:
            results.append((name, "HARNESS: " + str(error)))
        except AssertionError as error:
            results.append((name, str(error)))
        except (OSError, subprocess.TimeoutExpired) as error:
            results.append((name, "HARNESS: " + str(error)))

    for name, failure in results:
        if failure is None:
            print("PASS %s" % name, flush=True)
        elif failure.startswith("HARNESS:"):
            print(":HARNESS-ERROR: %s: %s"
                  % (name, failure[len("HARNESS:"):].strip()), flush=True)
        else:
            print(":FAIL: %s: %s" % (name, failure), flush=True)

    passed = sum(1 for _, failure in results if failure is None)
    failed = sum(1 for _, failure in results
                 if failure is not None and not failure.startswith("HARNESS:"))
    harness = sum(1 for _, failure in results
                  if failure is not None and failure.startswith("HARNESS:"))
    print("search: %d passed, %d failed, %d harness errors"
          % (passed, failed, harness), flush=True)
    if harness:
        return 2
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
