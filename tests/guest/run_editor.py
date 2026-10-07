#!/usr/bin/env python3
"""Production Guest multiline-editor acceptance over the real executor (0023).

This drives the production ELF built from apps/yanfs_terminal/ through the real
``yan_run --terminal``, with standard input as a pipe carrying the UART bytes.
It does not build anything (the caller passes the already built ELF) and it
needs no cross compiler, so nothing here may return 77: a missing in-repo
source, the executor, the formatter or the ELF is a hard failure (exit 1).

The image is judged by the same independent struct/zlib oracle the terminal
suite already uses, imported rather than copied:
``run_terminal_files.seed_image``, ``expected_image``, ``parse_entries``,
``run_guest`` and ``display``. Block 0 is rebuilt entry by entry, data blocks
are written from patterns defined here, and the whole image is compared byte
for byte; the directory is also decoded independently by ``parse_entries``.
Nothing is decoded by calling the production encoder, and the production
editor's draft state machine is not cloned: expected drafts are hand-built
byte literals and expected displays come from the reused cat oracle.

Coverage boundary: Host-driver runtime output/TX fault injection and a
scheduled UART window belong to a later Host-driver batch. This script only
exercises the pipe-provided UART path and what the real executor plus the image
oracle can decide; it does not claim the fault-injection coverage.

Interface:
  --source ROOT   repository root (required)
  --work DIR      directory for case images and per-run stdout/stderr/rc (required)
  --run EXE       yan_run executor (required)
  --mkfs EXE      yan_mkfs formatter (required)
  --guest ELF     the production terminal ELF (required)

A crash, a timeout or a sanitizer report is a harness error (exit 2), never a
pass; harness errors take priority over assertion failures.
"""

import argparse
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_terminal_files as base  # noqa: E402  (path installed just above)

BLOCKS = base.BLOCKS
MAX_STEPS = base.MAX_STEPS

# The same 0022-safe display rule, reused as the independent cat oracle.
display = base.display

# UTF-8 payloads this suite spells out by hand.
FIRST = "第一行".encode("utf-8")
SECOND = "第二行".encode("utf-8")
FIXED = "修订".encode("utf-8")
ZHONG = "中".encode("utf-8")


# ------------------------------------------------------------- small helpers

def fresh_image(mkfs, path):
    """Format an empty image through the real mkfs and check it against the
    oracle; used whenever a case starts from an empty directory."""
    base.fresh_image(mkfs, path)


def seed_image(path, files):
    """Write a valid image with named files and return its bytes."""
    return base.seed_image(path, files)


def run_guest(executor, elf, image, stdin, work, label):
    """Run the production ELF over the real executor; write per-run logs."""
    return base.run_guest(executor, elf, image, stdin, work, label,
                          max_steps=MAX_STEPS)


def require_rc(result, expected, owner):
    base.require_rc(result, expected, owner)


def fragment(result, text, owner):
    base.require_fragment(result, text, owner)


def count(result, text, expected, owner):
    got = result.stdout.count(text)
    if got != expected:
        raise AssertionError("%s: saw %d of %r, expected %d"
                             % (owner, got, text, expected))


def unchanged(disk, before, owner):
    if Path(disk).read_bytes() != before:
        raise AssertionError("%s: the image changed" % owner)


def check_image(disk, entries, blocks, owner):
    """Decode block 0 independently, then compare the whole image with the
    hand-listed entries and data this file expects."""
    actual = Path(disk).read_bytes()
    parsed = base.parse_entries(actual, BLOCKS)
    if parsed != list(entries):
        raise AssertionError("%s: decoded entries %r != expected %r"
                             % (owner, parsed, list(entries)))
    expected = base.expected_image(list(entries), blocks)
    if actual != expected:
        for at in range(min(len(actual), len(expected))):
            if actual[at] != expected[at]:
                raise AssertionError(
                    "%s: image differs at byte %d (actual 0x%02x, expected 0x%02x)"
                    % (owner, at, actual[at], expected[at]))
        raise AssertionError("%s: image length %d != %d"
                             % (owner, len(actual), len(expected)))


def no_edit_prompt(result, owner):
    if b"edit> " in result.stdout:
        raise AssertionError("%s: an edit session was entered unexpectedly" % owner)


# ------------------------------------------------------------------- cases

def case_new_chinese(case_dir, args):
    """New file: two Chinese lines, `r`, `d`, `p`, `w`; a new process reads it
    back with `cat` and the whole image is checked byte for byte."""
    disk = Path(case_dir) / "new-chinese.img"
    fresh_image(args.mkfs, disk)
    stdin = (b"edit note.txt\n"
             + b"a " + FIRST + b"\n"
             + b"a " + SECOND + b"\n"
             + b"r 2 " + FIXED + b"\n"
             + b"d 1\np\nw\nexit\n")
    run = run_guest(args.run, args.guest, disk, stdin, case_dir, "new-chinese")
    require_rc(run, 0, "new-chinese")
    fragment(run, b"OK edit\r\n", "new-chinese")
    count(run, b"OK a\r\n", 2, "new-chinese")
    fragment(run, b"OK r\r\n", "new-chinese")
    fragment(run, b"OK d\r\n", "new-chinese")
    fragment(run, b"1 " + FIXED + b"\r\nOK p\r\n", "new-chinese")
    fragment(run, b"OK w\r\n", "new-chinese")
    fragment(run, b"yanfs: exit\r\n", "new-chinese")
    draft = FIXED + b"\n"
    entries = [(0, "note.txt", len(draft), 1, 1)]
    check_image(disk, entries, {1: draft}, "new-chinese")

    second = run_guest(args.run, args.guest, disk, b"cat note.txt\nexit\n",
                       case_dir, "new-chinese-cat")
    require_rc(second, 0, "new-chinese-cat")
    fragment(second, display(draft) + b"\r\nOK cat\r\n", "new-chinese-cat")
    check_image(disk, entries, {1: draft}, "new-chinese-cat")


def case_existing_crlf(case_dir, args):
    """Existing "A\\r\\nB": `r` keeps the CRLF and the missing final EOL, `a`
    adds a separator LF and a new LF, `w` replaces; the released block keeps
    its old bytes and the new extent is allocated after it."""
    disk = Path(case_dir) / "crlf.img"
    seed = b"A\r\nB"
    seed_image(disk, [("note", seed, 1)])
    run = run_guest(args.run, args.guest, disk,
                    b"edit note\nr 1 C\nr 2 Y\np\na D\nw\nexit\n",
                    case_dir, "crlf")
    require_rc(run, 0, "crlf")
    fragment(run, b"1 C\r\n2 Y\r\nOK p\r\n", "crlf")
    fragment(run, b"OK w\r\n", "crlf")
    draft = b"C\r\nY\nD\n"
    check_image(disk, [(0, "note", len(draft), 2, 1)],
                {1: seed, 2: draft}, "crlf")


def case_quit_no_write(case_dir, args):
    """`q` after edits leaves both a loaded image and an empty new image
    byte-identical; the shell then takes the following `exit`."""
    disk = Path(case_dir) / "quit-loaded.img"
    before = seed_image(disk, [("note", b"A\r\nB", 1)])
    run = run_guest(args.run, args.guest, disk,
                    b"edit note\nr 1 C\na D\np\nq\nexit\n",
                    case_dir, "quit-loaded")
    require_rc(run, 0, "quit-loaded")
    fragment(run, b"OK q\r\n", "quit-loaded")
    fragment(run, b"OK exit\r\n", "quit-loaded")
    fragment(run, b"yanfs: exit\r\n", "quit-loaded")
    unchanged(disk, before, "quit-loaded")

    disk2 = Path(case_dir) / "quit-new.img"
    fresh_image(args.mkfs, disk2)
    before2 = disk2.read_bytes()
    run2 = run_guest(args.run, args.guest, disk2,
                     b"edit note.txt\na X\nq\nls\nexit\n", case_dir, "quit-new")
    require_rc(run2, 0, "quit-new")
    fragment(run2, b"OK q\r\n", "quit-new")
    fragment(run2, b"OK ls\r\n", "quit-new")
    fragment(run2, b"yanfs: exit\r\n", "quit-new")
    unchanged(disk2, before2, "quit-new")


def case_exit_unknown_until_quit(case_dir, args):
    """In EDITING `exit` is an unknown editor command: it keeps editing until
    `q`, and only then does the shell `exit` end the session."""
    disk = Path(case_dir) / "exit-unknown.img"
    fresh_image(args.mkfs, disk)
    before = disk.read_bytes()
    run = run_guest(args.run, args.guest, disk,
                    b"edit note.txt\nexit\nq\nexit\n", case_dir, "exit-unknown")
    require_rc(run, 0, "exit-unknown")
    fragment(run, b"edit> exit\r\nERROR UNKNOWN_COMMAND\r\nedit> ",
             "exit-unknown")
    fragment(run, b"edit> q\r\nOK q\r\n", "exit-unknown")
    fragment(run, b"yanfs: exit\r\n", "exit-unknown")
    unchanged(disk, before, "exit-unknown")


def case_empty_file_and_lines(case_dir, args):
    """An empty file loads with no line, an empty LF line and an empty CRLF
    line each print one number and a space, and the CRLF is not normalized."""
    disk = Path(case_dir) / "empty-file.img"
    seed_image(disk, [("empty", b"", 0)])
    run = run_guest(args.run, args.guest, disk,
                    b"edit empty\np\na X\nw\nexit\n", case_dir, "empty-file")
    require_rc(run, 0, "empty-file")
    fragment(run, b"OK p\r\n", "empty-file")
    check_image(disk, [(0, "empty", 2, 1, 1)], {1: b"X\n"}, "empty-file")

    disk2 = Path(case_dir) / "empty-lf.img"
    fresh_image(args.mkfs, disk2)
    run2 = run_guest(args.run, args.guest, disk2,
                     b"edit blank\na\np\nw\nexit\n", case_dir, "empty-lf")
    require_rc(run2, 0, "empty-lf")
    fragment(run2, b"1 \r\nOK p\r\n", "empty-lf")
    check_image(disk2, [(0, "blank", 1, 1, 1)], {1: b"\n"}, "empty-lf")

    disk3 = Path(case_dir) / "empty-crlf.img"
    before3 = seed_image(disk3, [("crlf", b"\r\n", 1)])
    run3 = run_guest(args.run, args.guest, disk3,
                     b"edit crlf\np\nq\nexit\n", case_dir, "empty-crlf")
    require_rc(run3, 0, "empty-crlf")
    fragment(run3, b"1 \r\nOK p\r\n", "empty-crlf")
    fragment(run3, b"OK q\r\n", "empty-crlf")
    unchanged(disk3, before3, "empty-crlf")


REJECT_SEEDS = [
    ("binary.bin", b"\x00\x01", 1),
    ("badutf8", b"\x80", 2),
    ("c1ctrl", b"\xc2\x80", 3),
    ("barecr", b"A\rB", 4),
    ("big", b"A" * 16385, 5),
]


def case_load_rejections(case_dir, args):
    """NUL/C0, invalid UTF-8, a C1 control, a bare CR and a 16385-byte file are
    all refused whole; the shell still serves `stat` and `ls` afterwards, no
    file changes and no editing mode is entered."""
    disk = Path(case_dir) / "reject.img"
    before = seed_image(disk, REJECT_SEEDS)
    stdin = (b"edit binary.bin\nstat binary.bin\n"
             b"edit badutf8\nstat badutf8\n"
             b"edit c1ctrl\nstat c1ctrl\n"
             b"edit barecr\nstat barecr\n"
             b"edit big\nstat big\n"
             b"ls\nexit\n")
    run = run_guest(args.run, args.guest, disk, stdin, case_dir, "reject")
    require_rc(run, 0, "reject")
    count(run, b"ERROR INVALID_TEXT\r\n", 4, "reject")
    count(run, b"ERROR TOO_LARGE\r\n", 1, "reject")
    fragment(run, b"2 binary.bin\r\nOK stat\r\n", "reject")
    fragment(run, b"1 badutf8\r\nOK stat\r\n", "reject")
    fragment(run, b"2 c1ctrl\r\nOK stat\r\n", "reject")
    fragment(run, b"3 barecr\r\nOK stat\r\n", "reject")
    fragment(run, b"16385 big\r\nOK stat\r\n", "reject")
    fragment(run, b"2 binary.bin\r\n1 badutf8\r\n2 c1ctrl\r\n3 barecr\r\n"
                   b"16385 big\r\nOK ls\r\n", "reject")
    fragment(run, b"yanfs: exit\r\n", "reject")
    no_edit_prompt(run, "reject")
    unchanged(disk, before, "reject")


def case_capacity_exact(case_dir, args):
    """A 16381-byte file plus `a x` reaches exactly 16384; a further `a y` is
    refused atomically and `w` saves the pre-refusal 16384-byte draft."""
    disk = Path(case_dir) / "cap-exact.img"
    old = b"A" * 16381
    seed_image(disk, [("big", old, 1)])
    run = run_guest(args.run, args.guest, disk,
                    b"edit big\na x\na y\nw\nexit\n", case_dir, "cap-exact")
    require_rc(run, 0, "cap-exact")
    count(run, b"ERROR TOO_LARGE\r\n", 1, "cap-exact")
    fragment(run, b"OK w\r\n", "cap-exact")
    fragment(run, b"yanfs: exit\r\n", "cap-exact")
    draft = old + b"\nx\n"
    check_image(disk, [(0, "big", 16384, 5, 4)],
                {1: old, 5: draft}, "cap-exact")


def case_capacity_replace(case_dir, args):
    """A maximal `r 1` line would make the draft 17402 bytes; the 16384-byte
    draft is kept whole and `w` still replaces with it unchanged."""
    disk = Path(case_dir) / "cap-replace.img"
    old = b"x\n" + b"A" * 16382
    seed_image(disk, [("big", old, 1)])
    line = b"r 1 " + b"B" * 1019
    run = run_guest(args.run, args.guest, disk,
                    b"edit big\n" + line + b"\nw\nexit\n",
                    case_dir, "cap-replace")
    require_rc(run, 0, "cap-replace")
    count(run, b"ERROR TOO_LARGE\r\n", 1, "cap-replace")
    fragment(run, b"OK w\r\n", "cap-replace")
    check_image(disk, [(0, "big", 16384, 5, 4)],
                {1: old, 5: old}, "cap-replace")


def case_chinese_across_boundary(case_dir, args):
    """A Chinese sequence straddles the 4096-byte read-block boundary; the
    editor loads it whole and `p` renders it with the reused display oracle,
    while `q` leaves the image untouched."""
    disk = Path(case_dir) / "cross.img"
    data = b"A" * 4095 + ZHONG + b"B" * 1000
    before = seed_image(disk, [("cross", data, 1)])
    run = run_guest(args.run, args.guest, disk,
                    b"edit cross\np\nq\nexit\n", case_dir, "cross")
    require_rc(run, 0, "cross")
    fragment(run, b"1 " + display(data) + b"\r\nOK p\r\n", "cross")
    fragment(run, b"OK q\r\n", "cross")
    unchanged(disk, before, "cross")


def case_loaded_tab(case_dir, args):
    """A loaded TAB prints as \\x09, `w` replaces the file with the raw TAB
    still inside it, and a new process `cat` escapes it again."""
    disk = Path(case_dir) / "tab.img"
    tab = b"a\tb"
    seed_image(disk, [("tab", tab, 1)])
    run = run_guest(args.run, args.guest, disk,
                    b"edit tab\np\nw\nexit\n", case_dir, "tab")
    require_rc(run, 0, "tab")
    fragment(run, b"1 " + display(tab) + b"\r\nOK p\r\n", "tab")
    fragment(run, b"OK w\r\n", "tab")
    check_image(disk, [(0, "tab", 3, 2, 1)], {1: tab, 2: tab}, "tab")

    second = run_guest(args.run, args.guest, disk, b"cat tab\nexit\n",
                       case_dir, "tab-cat")
    require_rc(second, 0, "tab-cat")
    fragment(second, display(tab) + b"\r\nOK cat\r\n", "tab-cat")


def case_nospace(case_dir, args):
    """15 data blocks are in use, so a one-block replacement cannot borrow its
    own old extent: `w` reports NOSPACE, `p` still shows the draft, `q` leaves
    the image byte-identical."""
    disk = Path(case_dir) / "nospace.img"
    note = b"seed\n"
    filler = b"F" * 57344
    before = seed_image(disk, [("note", note, 1), ("filler", filler, 2)])
    run = run_guest(args.run, args.guest, disk,
                    b"edit note\na X\nw\np\nq\nexit\n", case_dir, "nospace")
    require_rc(run, 0, "nospace")
    count(run, b"ERROR NOSPACE\r\n", 1, "nospace")
    fragment(run, b"1 seed\r\n2 X\r\nOK p\r\n", "nospace")
    fragment(run, b"OK q\r\n", "nospace")
    fragment(run, b"yanfs: exit\r\n", "nospace")
    unchanged(disk, before, "nospace")


def case_input_line_limit(case_dir, args):
    """A 1023-byte editing line is accepted; a 1024-byte one is rejected whole
    and the accepted draft is what `w` saves."""
    disk = Path(case_dir) / "line-limit.img"
    fresh_image(args.mkfs, disk)
    accepted = b"a " + b"a" * 1021      # exactly 1023 bytes
    over = b"a " + b"a" * 1022          # exactly 1024 bytes
    run = run_guest(args.run, args.guest, disk,
                    b"edit long.txt\n" + accepted + b"\n" + over + b"\n"
                    b"w\nexit\n", case_dir, "line-limit")
    require_rc(run, 0, "line-limit")
    count(run, b"ERROR LINE_TOO_LONG\r\n", 1, "line-limit")
    fragment(run, b"OK w\r\n", "line-limit")
    draft = b"a" * 1021 + b"\n"
    check_image(disk, [(0, "long.txt", 1022, 1, 1)], {1: draft},
                "line-limit")


def case_bad_arity(case_dir, args):
    """`edit` with no name or with extra arguments is an ordinary USAGE and
    never enters EDITING; `editx` stays an original-shell unknown command."""
    disk = Path(case_dir) / "arity.img"
    fresh_image(args.mkfs, disk)
    before = disk.read_bytes()
    run = run_guest(args.run, args.guest, disk,
                    b"edit\nedit a b\neditx\nls\nexit\n", case_dir, "arity")
    require_rc(run, 0, "arity")
    count(run, b"ERROR USAGE\r\n", 2, "arity")
    fragment(run, b"ERROR UNKNOWN_COMMAND\r\n", "arity")
    fragment(run, b"OK ls\r\n", "arity")
    fragment(run, b"yanfs: exit\r\n", "arity")
    no_edit_prompt(run, "arity")
    unchanged(disk, before, "arity")


def case_text_spaces(case_dir, args):
    """`a` and `r` consume exactly one space and keep every later space, in the
    middle and at the end of TEXT."""
    disk = Path(case_dir) / "spaces.img"
    fresh_image(args.mkfs, disk)
    run = run_guest(args.run, args.guest, disk,
                    b"edit sp.txt\na  x y \na  z \nr 2  q \np\nw\nexit\n",
                    case_dir, "spaces")
    require_rc(run, 0, "spaces")
    fragment(run, b"1  x y \r\n2  q \r\nOK p\r\n", "spaces")
    fragment(run, b"OK w\r\n", "spaces")
    draft = b" x y \n q \n"
    check_image(disk, [(0, "sp.txt", 10, 1, 1)], {1: draft}, "spaces")


def case_reuse_session(case_dir, args):
    """Reuse one editor for new, existing, then another new name in one Guest."""
    disk = Path(case_dir) / "reuse.img"
    fresh_image(args.mkfs, disk)
    run = run_guest(args.run, args.guest, disk,
                    b"help\nedit note\na X\nw\nedit note\nr 1 Y\nw\n"
                    b"edit next\na Z\nw\nexit\n", case_dir, "reuse")
    require_rc(run, 0, "reuse")
    fragment(run, b"edit NAME: open a file in the 16384-byte draft editor\r\n"
             b"editing: a TEXT appends, r N TEXT replaces, d N deletes a line\r\n"
             b"editing: p prints the draft, w saves it once, q discards it\r\n",
             "reuse")
    count(run, b"OK edit\r\n", 3, "reuse")
    count(run, b"OK w\r\n", 3, "reuse")
    # Replacement publishes block 2 and frees block 1, which the next create
    # reuses. These bytes are an independent final-image expectation.
    check_image(disk, [(0, "note", 2, 2, 1), (1, "next", 2, 1, 1)],
                {1: b"Z\n", 2: b"Y\n"}, "reuse")


CASES = [
    ("same_process_new_existing_new_sessions_and_help", case_reuse_session),
    ("new_chinese_lines_replace_delete_print_save", case_new_chinese),
    ("existing_crlf_replace_append_expected_lf", case_existing_crlf),
    ("quit_loaded_and_new_whole_image_unchanged", case_quit_no_write),
    ("editing_exit_unknown_until_quit", case_exit_unknown_until_quit),
    ("empty_file_empty_lf_empty_crlf", case_empty_file_and_lines),
    ("load_rejections_keep_shell_usable", case_load_rejections),
    ("capacity_exact_16k_save_and_append_refusal", case_capacity_exact),
    ("capacity_replace_refusal_retains_draft", case_capacity_replace),
    ("chinese_across_4096_read_boundary", case_chinese_across_boundary),
    ("loaded_tab_print_escape_save_preserves_tab", case_loaded_tab),
    ("nospace_retains_draft_whole_image_unchanged", case_nospace),
    ("input_1023_accepted_1024_rejected", case_input_line_limit),
    ("bad_arity_edit_never_enters_mode", case_bad_arity),
    ("text_spaces_are_exact", case_text_spaces),
]


# ------------------------------------------------------------------ driver

PRODUCTION_INPUTS = (
    "apps/yanfs_terminal/main.c",
    "os/trap_entry.S", "os/task.c", "os/task_switch.S", "os/block.c",
    "os/console.c", "os/editor.c", "os/editor.h", "os/yanfs.c",
    "os/yanfs_block.c", "os/shell.c", "os/search.c", "os/search_linear.c",
    "os/line.c", "os/terminal.c",
    "os/memory.c", "os/guest.ld", "tools/yan_shell.py",
    "os/platform.h", "os/task.h", "os/block.h", "os/console.h",
    "os/shell.h", "os/search.h", "os/search_linear.h", "os/line.h",
    "os/terminal.h", "os/yanfs.h",
    "os/yanfs_block.h", "tests/guest/run_terminal_files.py",
)


def require_inputs(source, args):
    missing = []
    for rel in PRODUCTION_INPUTS:
        path = source / rel
        if not path.is_file():
            missing.append(str(path))
    for label, path in (("executor", args.run), ("mkfs", args.mkfs),
                        ("guest ELF", args.guest)):
        if not Path(path).is_file():
            missing.append("%s: %s" % (label, path))
    return missing


def main():
    parser = argparse.ArgumentParser(
        description="production Guest multiline-editor acceptance over "
                    "yan_run --terminal")
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--work", required=True, type=Path)
    parser.add_argument("--run", required=True)
    parser.add_argument("--mkfs", required=True)
    parser.add_argument("--guest", required=True)
    args = parser.parse_args()

    source = args.source.resolve()
    missing = require_inputs(source, args)
    if missing:
        for line in missing:
            print(":FAIL: missing required input: %s" % line, file=sys.stderr)
        return 1

    args.work.mkdir(parents=True, exist_ok=True)
    passed = 0
    failed = 0
    harness = 0
    for name, function in CASES:
        case_dir = Path(tempfile.mkdtemp(prefix=name + "-", dir=str(args.work)))
        try:
            function(case_dir, args)
        except base.HarnessError as error:
            harness += 1
            print(":HARNESS-ERROR: %s: %s" % (name, error))
        except AssertionError as error:
            failed += 1
            print(":FAIL: %s: %s" % (name, error))
        else:
            passed += 1
            print("PASS %s" % name)

    print("editor: %d passed, %d failed, %d harness errors"
          % (passed, failed, harness))
    if harness:
        return 2
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
