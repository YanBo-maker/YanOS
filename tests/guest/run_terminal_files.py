#!/usr/bin/env python3
"""Production Guest terminal acceptance over the real executor (0022).

This runs the production ELF built from apps/yanfs_terminal/ through the real
``yan_run --terminal`` with standard input as a pipe carrying the UART bytes.
It does not build anything (the caller passes the already built ELF), it does
not need a cross compiler, and it never pretends a pipe is a PTY. The host's
return code and the byte-exact output fragments are the verdict; the Guest's
own report is not accepted on its own.

The filesystem is judged by this file's own struct/zlib oracle: block 0 is
rebuilt entry by entry and sealed with a fresh CRC, data blocks are written
from patterns this file defines, and the whole image is compared byte for byte.
Nothing is decoded by calling the production encoder.

Interface:
  --source ROOT        repository root (required)
  --work DIR           directory for case images and per-run logs (required)
  --run EXE            yan_run executor (required)
  --mkfs EXE           yan_mkfs formatter (required)
  --guest ELF          the production terminal ELF (required)
  --close-fault-run EXE  optional close-fault executor from the existing test

Missing in-repo source, the launcher script, the executor, the ELF or mkfs is a
hard failure (exit 1); there is no cross-compiler dependency here, so nothing
in this script may return 77. A crash, timeout or sanitizer report is a harness
error (exit 2), never a pass.

Coverage boundary: real Host-driver output first/middle/last-byte failure and a
scheduled UART window belong to a later Host-driver batch; this script keeps to
the pipe-provided UART path and to what the real executor and the image oracle
can decide.
"""

import argparse
import os
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

BLOCK = 4096
BLOCKS = 16
IMAGE_BYTES = BLOCK * BLOCKS
MAGIC = b"YANFS01\x00"
MAX_STEPS = "400000000"
TIMEOUT = 180


class HarnessError(RuntimeError):
    """A condition that is not a clean pass/fail of a Guest assertion."""


# ------------------------------------------------------------- image oracle

def metadata_block(capacity):
    """A canonical empty directory, built here and not by the production tool."""
    block = bytearray(BLOCK)
    block[0:8] = MAGIC
    struct.pack_into("<I", block, 8, 1)
    struct.pack_into("<I", block, 12, BLOCK)
    struct.pack_into("<I", block, 16, capacity)
    struct.pack_into("<I", block, 20, 63)
    reseal(block)
    return bytes(block)


def reseal(block):
    """Recompute the CRC field for a 4096-byte metadata block in place."""
    crc = zlib.crc32(bytes(block[:60]) + b"\x00\x00\x00\x00"
                     + bytes(block[64:BLOCK])) & 0xFFFFFFFF
    struct.pack_into("<I", block, 60, crc)


def put_entry(block, slot, name, size, start, count):
    entry = bytearray(64)
    encoded = name.encode()
    entry[0:len(encoded)] = encoded
    struct.pack_into("<I", entry, 32, size)
    struct.pack_into("<I", entry, 36, start)
    struct.pack_into("<I", entry, 40, count)
    block[64 + slot * 64:64 + (slot + 1) * 64] = entry


def empty_image():
    image = bytearray(IMAGE_BYTES)
    image[0:BLOCK] = metadata_block(BLOCKS)
    return bytes(image)


def expected_image(entries, blocks):
    """The whole image this file expects: entries and data, everything else 0."""
    image = bytearray(IMAGE_BYTES)
    block0 = bytearray(metadata_block(BLOCKS))
    for entry in entries:
        put_entry(block0, *entry)
    reseal(block0)
    image[0:BLOCK] = block0
    for lba, payload in blocks.items():
        image[lba * BLOCK:lba * BLOCK + len(payload)] = payload
    return bytes(image)


def parse_entries(data, capacity):
    """Independent decode of block 0; raises AssertionError on any violation."""
    if len(data) != IMAGE_BYTES:
        raise AssertionError("image length %d != %d" % (len(data), IMAGE_BYTES))
    if data[0:8] != MAGIC:
        raise AssertionError("bad magic")
    if struct.unpack_from("<I", data, 8)[0] != 1:
        raise AssertionError("bad version")
    if struct.unpack_from("<I", data, 12)[0] != BLOCK:
        raise AssertionError("bad block_size")
    if struct.unpack_from("<I", data, 16)[0] != capacity:
        raise AssertionError("bad capacity")
    if struct.unpack_from("<I", data, 20)[0] != 63:
        raise AssertionError("bad entry_capacity")
    if any(data[24:60]):
        raise AssertionError("header reserved not zero")
    stored = struct.unpack_from("<I", data, 60)[0]
    crc = zlib.crc32(data[:60] + b"\x00\x00\x00\x00" + data[64:BLOCK]) & 0xFFFFFFFF
    if stored != crc:
        raise AssertionError("metadata CRC mismatch")
    entries = []
    for slot in range(63):
        entry = data[64 + slot * 64:64 + (slot + 1) * 64]
        if entry[0] == 0:
            if any(entry):
                raise AssertionError("empty slot at %d is not zero" % slot)
            continue
        name = entry[:32].split(b"\x00", 1)[0].decode()
        size, start, count = struct.unpack_from("<III", entry, 32)
        if any(entry[44:64]):
            raise AssertionError("entry %d reserved not zero" % slot)
        entries.append((slot, name, size, start, count))
    return entries


def assert_image(path, entries, blocks, owner):
    expected = expected_image(entries, blocks)
    actual = Path(path).read_bytes()
    if actual == expected:
        return
    for at in range(min(len(actual), len(expected))):
        if actual[at] != expected[at]:
            raise AssertionError(
                "%s: image differs at byte %d (actual 0x%02x, expected 0x%02x)"
                % (owner, at, actual[at], expected[at]))
    raise AssertionError("%s: image length %d != %d"
                         % (owner, len(actual), len(expected)))


def seed_image(path, files):
    """Write a valid image with the given named files (name, data, lba)."""
    block0 = bytearray(metadata_block(BLOCKS))
    image = bytearray(IMAGE_BYTES)
    for slot, (name, data, lba) in enumerate(files):
        count = (len(data) + BLOCK - 1) // BLOCK if data else 0
        start = lba if data else 0
        put_entry(block0, slot, name, len(data), start, count)
        if data:
            image[lba * BLOCK:lba * BLOCK + len(data)] = data
    reseal(block0)
    image[0:BLOCK] = block0
    Path(path).write_bytes(bytes(image))
    return bytes(image)


# --------------------------------------------------------- display encoder

def _escape(seq):
    out = bytearray()
    for byte in seq:
        out.extend(b"\\x%02X" % byte)
    return out


def _render(text):
    """One decoded character: C0/DEL escaped, backslash doubled, a C1 control
    escaped as its own UTF-8 bytes, everything else as its own bytes."""
    cp = ord(text)
    if cp < 0x20 or cp == 0x7F:
        return _escape(bytes([cp]))
    if cp == 0x5C:
        return b"\\\\"
    if 0x80 <= cp <= 0x9F:
        return _escape(text.encode("utf-8"))
    return text.encode("utf-8")


def _candidate_length(byte):
    if byte < 0x80:
        return 1
    if 0xC0 <= byte <= 0xDF:
        return 2
    if 0xE0 <= byte <= 0xEF:
        return 3
    if 0xF0 <= byte <= 0xF7:
        return 4
    return 1


def display(data):
    """The 0022 cat rendering, built on Python's own strict UTF-8 decoder.

    At each position the candidate 1/2/3/4-byte sequence is handed to
    ``bytes.decode("utf-8", "strict")``; a valid candidate is rendered and
    skipped whole, anything else escapes exactly one byte and advances one. The
    oracle therefore never reimplements the reader's pending state machine:
    CPython itself rejects overlong, surrogate, out-of-range and truncated
    sequences, and each rejected byte is escaped byte by byte, which is the
    reader's own rule for invalid input."""
    out = bytearray()
    index = 0
    length = len(data)
    while index < length:
        candidate = _candidate_length(data[index])
        chunk = data[index:index + candidate]
        decoded = None
        if len(chunk) == candidate:
            try:
                text = chunk.decode("utf-8", "strict")
            except UnicodeDecodeError:
                text = None
            if text is not None and len(text) == 1:
                decoded = text
        if decoded is None:
            out.extend(_escape(data[index:index + 1]))
            index += 1
        else:
            out.extend(_render(decoded))
            index += candidate
    return bytes(out)


def display_self_check():
    """Explicit literal expectations for the known cat cases, so the oracle is
    pinned independently of any implementation state machine. True-EOF pending
    sequences of 1, 2 and 3 bytes are included."""
    cases = [
        (b"\xc2", b"\\xC2"),
        (b"\xe4\xb8", b"\\xE4\\xB8"),
        (b"\xf0\x9f\x92", b"\\xF0\\x9F\\x92"),
        (b"\xc2\xa0", b"\xc2\xa0"),
        (b"\xc2\x80", b"\\xC2\\x80"),
        (b"\xc2\x9f", b"\\xC2\\x9F"),
        (b"\xe4\xb8\xad", b"\xe4\xb8\xad"),
        (b"\xf0\x9f\x98\x80", b"\xf0\x9f\x98\x80"),
        (b"\\", b"\\\\"),
        (b"\x00", b"\\x00"),
        (b"\x7f", b"\\x7F"),
        (b"\x80", b"\\x80"),
        (b"\xc0\x80", b"\\xC0\\x80"),
        (b"\xed\xa0\x80", b"\\xED\\xA0\\x80"),
        (b"\xf4\x90\x80\x80", b"\\xF4\\x90\\x80\\x80"),
        (b"\xe4\xb8\xe4A", b"\\xE4\\xB8\\xE4A"),
    ]
    for data, expected in cases:
        got = display(data)
        if got != expected:
            raise AssertionError("display self-check: %r -> %r, expected %r"
                                 % (data, got, expected))


# ------------------------------------------------------------- host runner

def run_guest(executor, elf, image, stdin, work, label, terminal=True,
              max_steps=MAX_STEPS, env_extra=None):
    argv = [str(executor), "--image", str(elf)]
    if terminal:
        argv.append("--terminal")
    argv += ["--disk-image", str(image), "--max-steps", str(max_steps)]
    env = dict(os.environ)
    if env_extra:
        env.update(env_extra)
    try:
        result = subprocess.run(argv, input=stdin, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, timeout=TIMEOUT, env=env)
    except subprocess.TimeoutExpired as error:
        raise HarnessError("%s: timed out after %ds" % (label, TIMEOUT)) from error
    except OSError as error:
        raise HarnessError("%s: cannot run %s: %s" % (label, executor, error)) from error
    (Path(work) / (label + ".argv")).write_text(repr(argv) + "\n")
    (Path(work) / (label + ".stdout")).write_bytes(result.stdout)
    (Path(work) / (label + ".stderr")).write_bytes(result.stderr)
    (Path(work) / (label + ".rc")).write_text("%d\n" % result.returncode)
    return result


def check_not_harness(result, owner):
    blob = result.stdout + result.stderr
    if result.returncode < 0:
        raise HarnessError("%s: died with signal %d" % (owner, -result.returncode))
    if b"Sanitizer" in blob or b"runtime error:" in blob:
        raise HarnessError("%s: sanitizer output" % owner)


def require_rc(result, expected, owner):
    check_not_harness(result, owner)
    if result.returncode != expected:
        raise AssertionError("%s: executor returned %d, expected %d"
                             % (owner, result.returncode, expected))


def require_fragment(result, fragment, owner):
    if fragment not in result.stdout:
        raise AssertionError("%s: missing output fragment %r"
                             % (owner, fragment))


def require_stderr(result, fragment, owner):
    if fragment not in result.stderr:
        raise AssertionError("%s: missing stderr fragment %r"
                             % (owner, fragment))


def fresh_image(mkfs, path):
    result = subprocess.run([str(mkfs), "--image", str(path),
                             "--blocks", str(BLOCKS)],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            timeout=60)
    if result.returncode != 0:
        raise HarnessError("mkfs failed: %s" % result.stderr.decode(errors="replace"))
    if Path(path).read_bytes() != empty_image():
        raise AssertionError("the mkfs image differs from the oracle")


# ------------------------------------------------------------------ cases

def case_session_create_overwrite(case_dir, args):
    disk = Path(case_dir) / "session.img"
    fresh_image(args.mkfs, disk)

    run_a = run_guest(args.run, args.guest, disk,
                      b"create hello.txt hello\ncat hello.txt\nstat hello.txt\nls\nexit\n",
                      case_dir, "session-a")
    require_rc(run_a, 0, "session-a")
    require_fragment(run_a, b"OK create\r\n", "session-a")
    require_fragment(run_a, b"hello\r\nOK cat\r\n", "session-a")
    require_fragment(run_a, b"5 hello.txt\r\nOK stat\r\n", "session-a")
    require_fragment(run_a, b"5 hello.txt\r\nOK ls\r\n", "session-a")
    require_fragment(run_a, b"yanfs: exit\r\n", "session-a")
    assert_image(disk, [(0, "hello.txt", 5, 1, 1)], {1: b"hello"}, "session-a")

    run_b = run_guest(args.run, args.guest, disk, b"write hello.txt world\nexit\n",
                      case_dir, "session-b")
    require_rc(run_b, 0, "session-b")
    require_fragment(run_b, b"OK write\r\n", "session-b")
    # The old block is unallocated but its bytes are not erased; the directory
    # points at the new block.
    assert_image(disk, [(0, "hello.txt", 5, 2, 1)],
                 {1: b"hello", 2: b"world"}, "session-b")

    before = disk.read_bytes()
    run_c = run_guest(args.run, args.guest, disk,
                      b"cat hello.txt\nstat hello.txt\nls\nexit\n", case_dir,
                      "session-c")
    require_rc(run_c, 0, "session-c")
    require_fragment(run_c, b"world\r\nOK cat\r\n", "session-c")
    require_fragment(run_c, b"5 hello.txt\r\nOK stat\r\n", "session-c")
    require_fragment(run_c, b"5 hello.txt\r\nOK ls\r\n", "session-c")
    if disk.read_bytes() != before:
        raise AssertionError("session-c: the read-only run changed the image")

    utf8 = "中文".encode("utf-8")
    run_d = run_guest(args.run, args.guest, disk,
                      b"create empty.txt\ncreate spaced.txt  x y \n"
                      b"create utf8.txt " + utf8 + b"\nexit\n",
                      case_dir, "session-d")
    require_rc(run_d, 0, "session-d")
    require_fragment(run_d, b"OK create\r\n", "session-d")
    assert_image(disk,
                 [(0, "hello.txt", 5, 2, 1), (1, "empty.txt", 0, 0, 0),
                  (2, "spaced.txt", 5, 1, 1), (3, "utf8.txt", 6, 3, 1)],
                 {1: b" x y ", 2: b"world", 3: utf8}, "session-d")

    before = disk.read_bytes()
    run_e = run_guest(args.run, args.guest, disk,
                      b"create hello.txt again\nwrite absent.txt x\nexit\n",
                      case_dir, "session-e")
    require_rc(run_e, 0, "session-e")
    require_fragment(run_e, b"ERROR EXISTS\r\n", "session-e")
    require_fragment(run_e, b"ERROR NOT_FOUND\r\n", "session-e")
    if disk.read_bytes() != before:
        raise AssertionError("session-e: the refused commands changed the image")

    run_f = run_guest(args.run, args.guest, disk,
                      b"rm empty.txt\nrm spaced.txt\ncreate reuse.txt z\nexit\n",
                      case_dir, "session-f")
    require_rc(run_f, 0, "session-f")
    require_fragment(run_f, b"OK rm\r\n", "session-f")
    # The freed block 1 is reused by the new file.
    assert_image(disk,
                 [(0, "hello.txt", 5, 2, 1), (1, "reuse.txt", 1, 1, 1),
                  (3, "utf8.txt", 6, 3, 1)],
                 {1: b"z", 2: b"world", 3: utf8}, "session-f")


def case_line_boundaries(case_dir, args):
    disk = Path(case_dir) / "boundaries.img"
    fresh_image(args.mkfs, disk)
    commands = (
        b"create long.txt " + b"a" * 1007 + b"\n"          # exactly 1023 bytes
        + b"create over.txt " + b"a" * 1008 + b"\x08\n"    # 1024th byte + BS
        + b"cre\x1bate bad.txt\n"                          # ESC inside a command
        + b"create tab.txt a\tb\n"                         # TAB
        + b"create nul.txt a\x00b\n"                       # NUL
        + b"create ed.txt ab\x08c\n"                       # BS edits to "ac"
        + b"create del.txt xy\x7fz\n"                      # DEL edits to "xz"
        + b"create hi.txt \xc3\xa9\n"                      # high bytes raw
        + b"help\n"                                        # real help syntax
        + b"ls\r\nls\n"                                    # CRLF across calls
        + b"exit\n")
    result = run_guest(args.run, args.guest, disk, commands, case_dir, "boundaries")
    require_rc(result, 0, "boundaries")
    require_fragment(result, b"ERROR LINE_TOO_LONG\r\n", "boundaries")
    require_fragment(result, b"ERROR INVALID_INPUT\r\n", "boundaries")
    require_fragment(result, b"names: 1 to 31 bytes", "boundaries")
    require_fragment(result, b"2 hi.txt\r\nOK ls\r\n", "boundaries")
    require_fragment(result, b"yanfs: exit\r\n", "boundaries")
    # No rejected line created a file, and the accepted ones are exactly these.
    assert_image(disk,
                 [(0, "long.txt", 1007, 1, 1), (1, "ed.txt", 2, 2, 1),
                  (2, "del.txt", 2, 3, 1), (3, "hi.txt", 2, 4, 1)],
                 {1: b"a" * 1007, 2: b"ac", 3: b"xz", 4: b"\xc3\xa9"},
                 "boundaries")


def case_cat_edge_encodings(case_dir, args):
    display_self_check()
    disk = Path(case_dir) / "cat.img"
    utf8 = bytearray(b"A" * 1024)
    utf8[255:257] = b"\xc3\xa9"          # 2-byte sequence across chunk 0/1
    utf8[510:513] = b"\xe4\xb8\xad"      # 3-byte sequence across chunk 1/2
    utf8[765:769] = b"\xf0\x9f\x98\x80"  # 4-byte sequence across chunk 2/3
    # More than 4096 bytes: sequences cross both the 256-byte read chunk and the
    # 4096-byte filesystem block boundary.
    large = bytearray(b"B" * 5000)
    large[255:257] = b"\xc3\xa9"
    large[4095:4098] = b"\xe4\xb8\xad"
    large[4600:4604] = b"\xf0\x9f\x98\x80"
    esc = bytes([0x5C, 0x00, 0x09, 0x7F,          # backslash, NUL, TAB, DEL
                 0xC2, 0x80, 0xC2, 0x9F,          # C1 controls
                 0xC2, 0xA0,                      # not C1
                 0x80, 0xC0, 0x80,                # isolated, overlong
                 0xED, 0xA0, 0x80,                # surrogate
                 0xF4, 0x90, 0x80, 0x80,          # above U+10FFFF
                 0xE4, 0xB8, 0xE4, 0x41])         # broken then ASCII
    eof1 = b"\xc2"                 # true EOF with one pending byte
    eof2 = b"\xe4\xb8"             # true EOF with two pending bytes
    eof3 = b"\xf0\x9f\x92"         # true EOF with three pending bytes
    seeded = seed_image(disk, [("utf8.txt", bytes(utf8), 1),
                               ("esc.txt", esc, 2),
                               ("large.txt", bytes(large), 3),
                               ("eof1", eof1, 5),
                               ("eof2", eof2, 6),
                               ("eof3", eof3, 7)])
    result = run_guest(args.run, args.guest, disk,
                       b"cat utf8.txt\ncat esc.txt\ncat large.txt\n"
                       b"cat eof1\ncat eof2\ncat eof3\nexit\n",
                       case_dir, "cat-edges")
    require_rc(result, 0, "cat-edges")
    for name, payload in (("utf8.txt", bytes(utf8)), ("esc.txt", esc),
                          ("large.txt", bytes(large)), ("eof1", eof1),
                          ("eof2", eof2), ("eof3", eof3)):
        if display(payload) + b"\r\nOK cat\r\n" not in result.stdout:
            raise AssertionError("cat-edges: the %s display differs" % name)
    if disk.read_bytes() != seeded:
        raise AssertionError("cat-edges: cat changed the image")


def check_rejected_mount(case_dir, args, name, image_bytes, fragment):
    disk = Path(case_dir) / (name + ".img")
    disk.write_bytes(image_bytes)
    before = disk.read_bytes()
    result = run_guest(args.run, args.guest, disk, b"", case_dir, name)
    require_rc(result, 6, name)
    require_fragment(result, fragment, name)
    if disk.read_bytes() != before:
        raise AssertionError("%s: the rejected mount changed the image" % name)


def case_corrupt_images(case_dir, args):
    base = empty_image()

    crc = bytearray(base)
    crc[100] ^= 0x5A
    check_rejected_mount(case_dir, args, "corrupt-crc", bytes(crc),
                         b"ERROR CORRUPT\r\n")

    overlap = bytearray(base)
    block0 = bytearray(metadata_block(BLOCKS))
    put_entry(block0, 0, "a", 8192, 1, 2)
    put_entry(block0, 1, "b", 8192, 2, 2)
    reseal(block0)
    overlap[0:BLOCK] = block0
    check_rejected_mount(case_dir, args, "corrupt-overlap", bytes(overlap),
                         b"ERROR CORRUPT\r\n")

    version = bytearray(base)
    block0 = bytearray(metadata_block(BLOCKS))
    struct.pack_into("<I", block0, 8, 2)
    reseal(block0)
    version[0:BLOCK] = block0
    check_rejected_mount(case_dir, args, "corrupt-version", bytes(version),
                         b"ERROR UNSUPPORTED\r\n")


def case_no_terminal(case_dir, args):
    disk = Path(case_dir) / "no-terminal.img"
    fresh_image(args.mkfs, disk)
    before = disk.read_bytes()
    result = run_guest(args.run, args.guest, disk, b"", case_dir, "no-terminal",
                       terminal=False)
    require_rc(result, 6, "no-terminal")
    if disk.read_bytes() != before:
        raise AssertionError("no-terminal: the image changed")


def case_eof_without_exit(case_dir, args):
    disk = Path(case_dir) / "eof.img"
    fresh_image(args.mkfs, disk)
    before = disk.read_bytes()
    result = run_guest(args.run, args.guest, disk, b"ls\n", case_dir, "eof",
                       max_steps="2000000")
    check_not_harness(result, "eof")
    # No exit: the run stops at the finite step limit, which is not a pass.
    if result.returncode != 4:
        raise AssertionError("eof: executor returned %d, expected the step-limit 4"
                             % result.returncode)
    require_fragment(result, b"OK ls\r\n", "eof")
    if disk.read_bytes() != before:
        raise AssertionError("eof: the image changed")


def case_close_fault(case_dir, args):
    disk = Path(case_dir) / "close-fault.img"
    fresh_image(args.mkfs, disk)
    env = {"YAN_TEST_DISK_CLOSE_PATH": str(disk)}
    result = run_guest(args.close_fault_run, args.guest, disk,
                       b"create close.txt x\nexit\n", case_dir, "close-fault",
                       env_extra=env)
    require_rc(result, 5, "close-fault")
    require_fragment(result, b"yanfs: exit\r\n", "close-fault")
    require_stderr(result, b"TEST: disk fclose failure injected", "close-fault")


CASES = [
    ("session_create_overwrite", case_session_create_overwrite),
    ("line_boundaries", case_line_boundaries),
    ("cat_edge_encodings", case_cat_edge_encodings),
    ("corrupt_images", case_corrupt_images),
    ("no_terminal", case_no_terminal),
    ("eof_without_exit", case_eof_without_exit),
    ("close_fault", case_close_fault),
]


# ------------------------------------------------------------------ driver

def require_inputs(source, args):
    missing = []
    for rel in ("apps/yanfs_terminal/main.c", "os/trap_entry.S", "os/task.c",
                "os/task_switch.S", "os/block.c", "os/console.c", "os/yanfs.c",
                "os/yanfs_block.c", "os/shell.c", "os/search.c",
                "os/search_linear.c", "os/line.c", "os/terminal.c",
                "os/memory.c", "os/editor.c", "os/editor.h", "os/guest.ld",
                "os/search.h", "os/search_linear.h", "tools/yan_shell.py"):
        path = source / rel
        if not path.is_file():
            missing.append(str(path))
    for label, path in (("executor", args.run), ("mkfs", args.mkfs),
                        ("guest ELF", args.guest)):
        if not Path(path).is_file():
            missing.append("%s: %s" % (label, path))
    if args.close_fault_run and not Path(args.close_fault_run).is_file():
        missing.append("close-fault runner: %s" % args.close_fault_run)
    return missing


def main():
    parser = argparse.ArgumentParser(
        description="production Guest terminal acceptance over yan_run --terminal")
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--work", required=True, type=Path)
    parser.add_argument("--run", required=True)
    parser.add_argument("--mkfs", required=True)
    parser.add_argument("--guest", required=True)
    parser.add_argument("--close-fault-run", default=None)
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
        if name == "close_fault" and not args.close_fault_run:
            print("SKIP %s: no --close-fault-run executor was given" % name)
            continue
        case_dir = Path(tempfile.mkdtemp(prefix=name + "-", dir=str(args.work)))
        try:
            function(case_dir, args)
        except HarnessError as error:
            harness += 1
            print(":HARNESS-ERROR: %s: %s" % (name, error))
        except AssertionError as error:
            failed += 1
            print(":FAIL: %s: %s" % (name, error))
        else:
            passed += 1
            print("PASS %s" % name)

    print("terminal-files: %d passed, %d failed, %d harness errors"
          % (passed, failed, harness))
    if harness:
        return 2
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
