#!/usr/bin/env python3
"""Production Guest file-management acceptance over the real executor (0024).

This drives the production ELF built from apps/yanfs_terminal/ through the real
``yan_run --terminal``, with standard input as a pipe carrying the UART bytes.
It does not build anything (the caller passes the already built ELF) and it does
not use a cross compiler, so nothing here may return 77: a missing in-repo
source, header, script, executor, formatter or ELF is a hard failure (exit 1).

The image is judged by the 0022 independent struct/zlib oracle, imported from
``run_terminal_files`` rather than copied: ``seed_image``, ``empty_image``,
``expected_image``, ``parse_entries``, ``assert_image``, ``display`` and
``run_guest``. Block 0 is rebuilt entry by entry, data blocks are written from
patterns defined here, and the whole image is compared byte for byte. Nothing is
decoded by calling the production encoder. The production shell/editor state
machines are not cloned; expected displays come from the reused cat oracle.

Coverage boundary: Host-driver output/TX fault injection belongs to a later
Host-driver batch. This script exercises the pipe-provided UART path and what
the real executor plus the whole-image oracle can decide; the dirty source
padding is seeded and expected explicitly, never compared against an all-zero
expectation.

Interface:
  --source ROOT   repository root (required)
  --work DIR      directory for case images and per-run stdout/stderr/rc (required)
  --run EXE       yan_run executor (required)
  --guest ELF     the production terminal ELF (required)
  --mkfs EXE      yan_mkfs formatter (optional; used for the empty-image preflight)

A crash, a timeout or a sanitizer report is a harness error (exit 2), never a
pass; harness errors take priority over assertion failures.
"""

import argparse
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_terminal_files as base  # noqa: E402  (path installed just above)
import run_editor as editor_base  # noqa: E402  (for the extended input list)

BLOCKS = base.BLOCKS
MAX_STEPS = base.MAX_STEPS

# The same 0022-safe display rule, reused as the independent cat oracle.
display = base.display


# ------------------------------------------------------------- small helpers

def pattern(length, seed):
    """A repeatable, non-constant byte pattern so copied bytes are identifiable."""
    return bytes((seed + i * 7 + (i >> 8)) & 0xFF for i in range(length))


def run_guest(args, image, stdin, work, label):
    """Run the production ELF over the real executor; write per-run logs."""
    return base.run_guest(args.run, args.guest, image, stdin, work, label,
                          max_steps=MAX_STEPS)


def count(result, text, expected, owner):
    got = result.stdout.count(text)
    if got != expected:
        raise AssertionError("%s: saw %d of %r, expected %d"
                             % (owner, got, text, expected))


def check_image(disk, entries, blocks, owner):
    """Decode block 0 independently, then compare the whole image with the
    hand-listed entries and data this file expects."""
    actual = Path(disk).read_bytes()
    parsed = base.parse_entries(actual, BLOCKS)
    expected_entries = [tuple(item) for item in entries]
    if parsed != expected_entries:
        raise AssertionError("%s: decoded entries %r != expected %r"
                             % (owner, parsed, expected_entries))
    base.assert_image(disk, entries, blocks, owner)


def check_unchanged(disk, before, owner):
    if Path(disk).read_bytes() != before:
        raise AssertionError("%s: the image changed" % owner)


# ------------------------------------------------------------------- cases

def case_preflight(case_dir, args):
    """The empty image matches the oracle, through the real formatter when one
    was given."""
    disk = Path(case_dir) / "empty.img"
    if args.mkfs:
        base.fresh_image(args.mkfs, disk)
    else:
        base.seed_image(disk, [])
    if disk.read_bytes() != base.empty_image():
        raise AssertionError("preflight: the empty image differs from the oracle")


def case_rename_5000_keeps_slot_extent_and_data(case_dir, args):
    """mv keeps the file in its original slot and extent: one metadata write,
    5000 bytes across blocks 1-2, whole image checked."""
    owner = "rename-5000"
    data = pattern(5000, 3)
    disk = Path(case_dir) / "rename.img"
    base.seed_image(disk, [("note.txt", data, 1)])
    run = run_guest(args, disk, b"mv note.txt diary.txt\nls\nexit\n",
                    case_dir, owner)
    base.require_rc(run, 0, owner)
    base.require_fragment(run, b"OK mv\r\n", owner)
    base.require_fragment(run, b"5000 diary.txt\r\nOK ls\r\n", owner)
    base.require_fragment(run, b"yanfs: exit\r\n", owner)
    check_image(disk, [(0, "diary.txt", 5000, 1, 2)],
                {1: data[:4096], 2: data[4096:]}, owner)


def case_error_paths_and_argument_priority_leave_image_unchanged(case_dir, args):
    """Self mv, self cp, EXISTS, NOT_FOUND, INVALID-before-missing-source and
    missing/extra arity all leave the image byte-identical."""
    owner = "errors"
    disk = Path(case_dir) / "errors.img"
    base.seed_image(disk, [("a", b"A", 1), ("b", b"B", 2)])
    before = disk.read_bytes()
    stdin = (b"mv a a\ncp a a\nmv a b\ncp a b\n"
             b"mv ghost det\ncp ghost det\n"
             b"mv ghost bad/name\ncp ghost bad/name\n"
             b"mv\ncp a\nmv a b c\nls\nexit\n")
    run = run_guest(args, disk, stdin, case_dir, owner)
    base.require_rc(run, 0, owner)
    base.require_fragment(run, b"OK mv\r\n", owner)
    base.require_fragment(run, b"1 a\r\n1 b\r\nOK ls\r\n", owner)
    base.require_fragment(run, b"yanfs: exit\r\n", owner)
    count(run, b"ERROR EXISTS\r\n", 3, owner)
    count(run, b"ERROR NOT_FOUND\r\n", 2, owner)
    count(run, b"ERROR INVALID\r\n", 2, owner)
    count(run, b"ERROR USAGE\r\n", 3, owner)
    check_unchanged(disk, before, owner)


def case_copy_binary_with_nul_and_invalid_utf8(case_dir, args):
    """cp is byte-exact for NUL, a lone continuation, an overlong, a surrogate,
    above U+10FFFF and a backslash, in a file that still fits one block."""
    owner = "copy-binary"
    data = bytes([0x00, 0x01, 0x7F, 0x80, 0xC0, 0x80, 0xED, 0xA0, 0x80,
                  0xF4, 0x90, 0x80, 0x80, 0xFF, 0x5C]) + "中".encode("utf-8")
    disk = Path(case_dir) / "binary.img"
    base.seed_image(disk, [("bin", data, 1)])
    run = run_guest(args, disk, b"cp bin bin2\nstat bin2\nexit\n",
                    case_dir, owner)
    base.require_rc(run, 0, owner)
    base.require_fragment(run, b"OK cp\r\n", owner)
    base.require_fragment(run, ("%d bin2\r\nOK stat\r\n" % len(data)).encode(),
                          owner)
    check_image(disk,
                [(0, "bin", len(data), 1, 1), (1, "bin2", len(data), 2, 1)],
                {1: data, 2: data}, owner)


def case_copy_20000_own_extent_zero_tail_keeps_dirty_source_padding(case_dir, args):
    """A 20000-byte source (5 blocks) copies into its own 5-block extent; the
    source's deliberately non-zero tail padding is preserved and the target's
    tail padding is zero, all pinned by a whole-image oracle."""
    owner = "copy-20000"
    data = pattern(20000, 5)
    disk = Path(case_dir) / "big.img"
    base.seed_image(disk, [("src", data, 1)])
    source_tail = data[4 * base.BLOCK:]
    dirty = b"\xA5" * (base.BLOCK - len(source_tail))
    image = bytearray(disk.read_bytes())
    tail_at = 5 * base.BLOCK
    image[tail_at + len(source_tail):tail_at + base.BLOCK] = dirty
    disk.write_bytes(bytes(image))
    before = disk.read_bytes()

    run = run_guest(args, disk, b"cp src dst\nls\nexit\n", case_dir, owner)
    base.require_rc(run, 0, owner)
    base.require_fragment(run, b"OK cp\r\n", owner)
    base.require_fragment(run, b"20000 src\r\n20000 dst\r\nOK ls\r\n", owner)
    base.require_fragment(run, b"yanfs: exit\r\n", owner)

    after = disk.read_bytes()
    if after[base.BLOCK:6 * base.BLOCK] != before[base.BLOCK:6 * base.BLOCK]:
        raise AssertionError("%s: the source extent changed" % owner)
    if after[5 * base.BLOCK + len(source_tail):6 * base.BLOCK] != dirty:
        raise AssertionError("%s: the source padding was overwritten" % owner)
    if any(after[10 * base.BLOCK + len(source_tail):11 * base.BLOCK]):
        raise AssertionError("%s: the copy tail is not zero" % owner)

    check_image(disk,
                [(0, "src", 20000, 1, 5), (1, "dst", 20000, 6, 5)],
                {1: data[0:4096], 2: data[4096:8192],
                 3: data[8192:12288], 4: data[12288:16384],
                 5: source_tail + dirty,
                 6: data[0:4096], 7: data[4096:8192],
                 8: data[8192:12288], 9: data[12288:16384],
                 10: source_tail + b"\x00" * len(dirty)}, owner)


def case_copy_sizes_zero_one_4096_4097(case_dir, args):
    """Empty, 1-byte, exact-block and block-plus-one copies all equal the
    source, with only the final logical tail zeroed."""
    owner = "copy-sizes"
    cases = [(0, "empty", "empty2"), (1, "one", "one2"),
             (4096, "exact", "exact2"), (4097, "over", "over2")]
    for size, src, dst in cases:
        label = "%s-%d" % (owner, size)
        disk = Path(case_dir) / (src + ".img")
        data = pattern(size, (size % 7) + 1)
        base.seed_image(disk, [(src, data, 1)] if size else [(src, b"", 0)])
        run = run_guest(args, disk,
                        ("cp %s %s\nexit\n" % (src, dst)).encode(),
                        case_dir, label)
        base.require_rc(run, 0, label)
        base.require_fragment(run, b"OK cp\r\n", label)
        if size == 0:
            check_image(disk, [(0, src, 0, 0, 0), (1, dst, 0, 0, 0)], {},
                        label)
            continue
        blocks_needed = (size + base.BLOCK - 1) // base.BLOCK
        destination_start = 1 + blocks_needed
        blocks = {}
        for index in range(blocks_needed):
            chunk = data[index * base.BLOCK:(index + 1) * base.BLOCK]
            blocks[1 + index] = chunk
            blocks[destination_start + index] = chunk
        check_image(disk,
                    [(0, src, size, 1, blocks_needed),
                     (1, dst, size, destination_start, blocks_needed)],
                    blocks, label)


def case_copy_uses_first_slot_and_low_hole_below_source(case_dir, args):
    """After rm frees slot 0 and block 1, cp takes that first empty slot and the
    low first-fit hole below the source, not a slot or block after it."""
    owner = "copy-low-hole"
    disk = Path(case_dir) / "hole.img"
    base.seed_image(disk, [("low", b"L", 1), ("src", b"S", 2)])
    run = run_guest(args, disk, b"rm low\ncp src dst\nexit\n", case_dir, owner)
    base.require_rc(run, 0, owner)
    base.require_fragment(run, b"OK rm\r\n", owner)
    base.require_fragment(run, b"OK cp\r\n", owner)
    check_image(disk, [(0, "dst", 1, 1, 1), (1, "src", 1, 2, 1)],
                {1: b"S", 2: b"S"}, owner)


def case_full_directory_and_full_storage_rename_cp_directory_full(case_dir, args):
    """63 slots and all 15 data blocks in use: mv still renames in place, and cp
    reports DIRECTORY_FULL before any allocation."""
    owner = "full"
    files = [("f%d" % i, b"x", i + 1) for i in range(15)]
    files += [("f%d" % i, b"", 0) for i in range(15, 63)]
    disk = Path(case_dir) / "full.img"
    base.seed_image(disk, files)
    run = run_guest(args, disk, b"mv f0 moved\ncp f1 copy\nexit\n",
                    case_dir, owner)
    base.require_rc(run, 0, owner)
    base.require_fragment(run, b"OK mv\r\n", owner)
    base.require_fragment(run, b"ERROR DIRECTORY_FULL\r\n", owner)
    base.require_fragment(run, b"yanfs: exit\r\n", owner)
    entries = []
    for slot in range(63):
        if slot == 0:
            entries.append((0, "moved", 1, 1, 1))
        elif slot < 15:
            entries.append((slot, "f%d" % slot, 1, slot + 1, 1))
        else:
            entries.append((slot, "f%d" % slot, 0, 0, 0))
    check_image(disk, entries, {lba: b"x" for lba in range(1, 16)}, owner)


def case_fragmentation_nospace_leaves_image_unchanged(case_dir, args):
    """Six isolated one-block holes cannot hold the two-block source: cp is
    NOSPACE and the image is byte-identical."""
    owner = "fragmentation"
    big = pattern(5000, 11)
    files = [("big", big, 1)]
    files += [("h%d" % i, b"h", 3 + 2 * i) for i in range(7)]
    disk = Path(case_dir) / "frag.img"
    base.seed_image(disk, files)
    before = disk.read_bytes()
    run = run_guest(args, disk, b"cp big copy\nexit\n", case_dir, owner)
    base.require_rc(run, 0, owner)
    base.require_fragment(run, b"ERROR NOSPACE\r\n", owner)
    base.require_fragment(run, b"yanfs: exit\r\n", owner)
    check_unchanged(disk, before, owner)


def case_postcopy_write_and_remove_keep_the_other_file(case_dir, args):
    """After cp, a write to one file and a remove of the other leave the
    untouched file's bytes and display intact, across three processes."""
    owner = "postcopy"
    disk = Path(case_dir) / "postcopy.img"
    base.seed_image(disk, [("master", b"hello", 1)])

    copy_run = run_guest(args, disk, b"cp master backup\nexit\n",
                         case_dir, owner + "-copy")
    base.require_rc(copy_run, 0, owner + "-copy")
    base.require_fragment(copy_run, b"OK cp\r\n", owner + "-copy")
    check_image(disk, [(0, "master", 5, 1, 1), (1, "backup", 5, 2, 1)],
                {1: b"hello", 2: b"hello"}, owner + "-copy")

    write_run = run_guest(args, disk,
                          b"write backup new\nstat master\ncat master\nexit\n",
                          case_dir, owner + "-write")
    base.require_rc(write_run, 0, owner + "-write")
    base.require_fragment(write_run, b"OK write\r\n", owner + "-write")
    base.require_fragment(write_run, b"5 master\r\nOK stat\r\n",
                          owner + "-write")
    base.require_fragment(write_run, b"hello\r\nOK cat\r\n", owner + "-write")
    check_image(disk, [(0, "master", 5, 1, 1), (1, "backup", 3, 3, 1)],
                {1: b"hello", 2: b"hello", 3: b"new"}, owner + "-write")

    remove_run = run_guest(args, disk,
                           b"rm master\nstat backup\ncat backup\nexit\n",
                           case_dir, owner + "-remove")
    base.require_rc(remove_run, 0, owner + "-remove")
    base.require_fragment(remove_run, b"OK rm\r\n", owner + "-remove")
    base.require_fragment(remove_run, b"3 backup\r\nOK stat\r\n",
                          owner + "-remove")
    base.require_fragment(remove_run, b"new\r\nOK cat\r\n",
                          owner + "-remove")
    check_image(disk, [(1, "backup", 3, 3, 1)],
                {1: b"hello", 2: b"hello", 3: b"new"}, owner + "-remove")


def case_cross_process_stat_and_cat_confirm_rename_and_copy(case_dir, args):
    """A rename and a copy are published, then a fresh process stats and cats
    both names exactly, leaving the image untouched."""
    owner = "cross-process"
    data = bytes([0x00, 0x80, 0xFF, 0x5C, 0xC3, 0xA9]) + b"tail"
    disk = Path(case_dir) / "cross.img"
    base.seed_image(disk, [("note.txt", data, 1)])

    publish = run_guest(args, disk,
                        b"mv note.txt diary.txt\ncp diary.txt backup.txt\nexit\n",
                        case_dir, owner + "-publish")
    base.require_rc(publish, 0, owner + "-publish")
    base.require_fragment(publish, b"OK mv\r\n", owner + "-publish")
    base.require_fragment(publish, b"OK cp\r\n", owner + "-publish")
    check_image(disk,
                [(0, "diary.txt", len(data), 1, 1),
                 (1, "backup.txt", len(data), 2, 1)],
                {1: data, 2: data}, owner + "-publish")

    before = disk.read_bytes()
    size = str(len(data)).encode()
    observe = run_guest(args, disk,
                        b"stat diary.txt\ncat diary.txt\n"
                        b"stat backup.txt\ncat backup.txt\nexit\n",
                        case_dir, owner + "-observe")
    base.require_rc(observe, 0, owner + "-observe")
    base.require_fragment(observe, size + b" diary.txt\r\nOK stat\r\n",
                          owner + "-observe")
    base.require_fragment(observe, size + b" backup.txt\r\nOK stat\r\n",
                          owner + "-observe")
    count(observe, display(data) + b"\r\nOK cat\r\n", 2, owner + "-observe")
    check_unchanged(disk, before, owner + "-observe")


def case_editing_receives_mv_cp_then_q(case_dir, args):
    """In EDITING, mv/cp/stat are editor commands; neither primitive runs, and
    after q the shell's own mv/cp work."""
    owner = "editing-q"
    disk = Path(case_dir) / "editing-q.img"
    base.seed_image(disk, [("a", b"A", 1)])
    stdin = (b"edit note.txt\nmv a b\ncp a c\nstat a\nq\n"
             b"cp a c\nmv a b\nls\nexit\n")
    run = run_guest(args, disk, stdin, case_dir, owner)
    base.require_rc(run, 0, owner)
    base.require_fragment(run, b"OK edit\r\n", owner)
    base.require_fragment(run, b"OK q\r\n", owner)
    count(run, b"ERROR UNKNOWN_COMMAND\r\n", 3, owner)
    count(run, b"OK cp\r\n", 1, owner)
    count(run, b"OK mv\r\n", 1, owner)
    base.require_fragment(run, b"1 b\r\n1 c\r\nOK ls\r\n", owner)
    base.require_fragment(run, b"yanfs: exit\r\n", owner)
    if b"OK stat\r\n" in run.stdout:
        raise AssertionError("%s: an editing line reached the shell's stat" % owner)
    check_image(disk, [(0, "b", 1, 1, 1), (1, "c", 1, 2, 1)],
                {1: b"A", 2: b"A"}, owner)


def case_editing_receives_mv_cp_then_w(case_dir, args):
    """In EDITING, mv is an editor command; after w saves the draft and returns
    to SHELL, the shell's own mv/cp work."""
    owner = "editing-w"
    disk = Path(case_dir) / "editing-w.img"
    base.seed_image(disk, [("a", b"A", 1)])
    stdin = (b"edit note.txt\na hi\nmv a b\nw\n"
             b"cp a c\nmv a b\nls\nexit\n")
    run = run_guest(args, disk, stdin, case_dir, owner)
    base.require_rc(run, 0, owner)
    base.require_fragment(run, b"OK edit\r\n", owner)
    base.require_fragment(run, b"OK a\r\n", owner)
    base.require_fragment(run, b"OK w\r\n", owner)
    count(run, b"ERROR UNKNOWN_COMMAND\r\n", 1, owner)
    count(run, b"OK cp\r\n", 1, owner)
    count(run, b"OK mv\r\n", 1, owner)
    base.require_fragment(run, b"1 b\r\n3 note.txt\r\n1 c\r\nOK ls\r\n", owner)
    base.require_fragment(run, b"yanfs: exit\r\n", owner)
    check_image(disk,
                [(0, "b", 1, 1, 1), (1, "note.txt", 3, 2, 1),
                 (2, "c", 1, 3, 1)],
                {1: b"A", 2: b"hi\n", 3: b"A"}, owner)


def case_same_process_multiple_file_commands(case_dir, args):
    """One Guest runs create, cp, mv, stat, cat, rm and ls; the final image is
    exactly the surviving independent copy."""
    owner = "same-process"
    disk = Path(case_dir) / "same.img"
    base.seed_image(disk, [])
    stdin = (b"create note.txt hello\ncp note.txt backup.txt\n"
             b"mv backup.txt draft.txt\nstat draft.txt\ncat draft.txt\n"
             b"rm note.txt\nls\nexit\n")
    run = run_guest(args, disk, stdin, case_dir, owner)
    base.require_rc(run, 0, owner)
    base.require_fragment(run, b"OK create\r\n", owner)
    base.require_fragment(run, b"OK cp\r\n", owner)
    base.require_fragment(run, b"OK mv\r\n", owner)
    base.require_fragment(run, b"5 draft.txt\r\nOK stat\r\n", owner)
    base.require_fragment(run, b"hello\r\nOK cat\r\n", owner)
    base.require_fragment(run, b"OK rm\r\n", owner)
    base.require_fragment(run, b"5 draft.txt\r\nOK ls\r\n", owner)
    base.require_fragment(run, b"yanfs: exit\r\n", owner)
    check_image(disk, [(1, "draft.txt", 5, 2, 1)],
                {1: b"hello", 2: b"hello"}, owner)


CASES = [
    ("image_oracle_preflight", case_preflight),
    ("rename_5000_keeps_slot_extent_and_data",
     case_rename_5000_keeps_slot_extent_and_data),
    ("error_paths_and_argument_priority_leave_image_unchanged",
     case_error_paths_and_argument_priority_leave_image_unchanged),
    ("copy_binary_with_nul_and_invalid_utf8",
     case_copy_binary_with_nul_and_invalid_utf8),
    ("copy_20000_own_extent_zero_tail_keeps_dirty_source_padding",
     case_copy_20000_own_extent_zero_tail_keeps_dirty_source_padding),
    ("copy_sizes_zero_one_4096_4097", case_copy_sizes_zero_one_4096_4097),
    ("copy_uses_first_slot_and_low_hole_below_source",
     case_copy_uses_first_slot_and_low_hole_below_source),
    ("full_directory_and_full_storage_rename_cp_directory_full",
     case_full_directory_and_full_storage_rename_cp_directory_full),
    ("fragmentation_nospace_leaves_image_unchanged",
     case_fragmentation_nospace_leaves_image_unchanged),
    ("postcopy_write_and_remove_keep_the_other_file",
     case_postcopy_write_and_remove_keep_the_other_file),
    ("cross_process_stat_and_cat_confirm_rename_and_copy",
     case_cross_process_stat_and_cat_confirm_rename_and_copy),
    ("editing_receives_mv_cp_then_q", case_editing_receives_mv_cp_then_q),
    ("editing_receives_mv_cp_then_w", case_editing_receives_mv_cp_then_w),
    ("same_process_multiple_file_commands",
     case_same_process_multiple_file_commands),
]


# ------------------------------------------------------------------ driver

PRODUCTION_INPUTS = tuple(editor_base.PRODUCTION_INPUTS) + (
    "tests/guest/run_editor.py",
    "tests/guest/run_file_management.py",
)


def require_inputs(source, args):
    missing = []
    for rel in PRODUCTION_INPUTS:
        path = source / rel
        if not path.is_file():
            missing.append(str(path))
    for label, path in (("executor", args.run), ("guest ELF", args.guest)):
        if not Path(path).is_file():
            missing.append("%s: %s" % (label, path))
    if args.mkfs and not Path(args.mkfs).is_file():
        missing.append("mkfs: %s" % args.mkfs)
    return missing


def main():
    parser = argparse.ArgumentParser(
        description="production Guest file-management acceptance over "
                    "yan_run --terminal")
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--work", required=True, type=Path)
    parser.add_argument("--run", required=True)
    parser.add_argument("--guest", required=True)
    parser.add_argument("--mkfs", default=None)
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

    print("file-management: %d passed, %d failed, %d harness errors"
          % (passed, failed, harness))
    if harness:
        return 2
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
