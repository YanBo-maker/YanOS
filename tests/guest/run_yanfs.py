#!/usr/bin/env python3
"""YanFS end-to-end Guest acceptance: writer, reader and corrupt-mount roles.

What this runs
--------------
One freshly formatted 16-block image, then the same Guest source
(tests/guest/yanfs_check.c) built for three roles and run through the
production executor `yan_run --disk-image`:

  writer   runs the whole document sequence -- create/read/stat/list, guard
           file, fold hello.txt up to 5000 bytes across blocks, EOF checks,
           remove, persist/empty/reuse, the duplicate and absent-name errors,
           and the two real 32-bit pointer-range negative controls -- then
           unmounts and exits through tohost;
  reader   is a *fresh process* on the writer's image: read/list/stat of the
           terminal state, no write;
  corrupt  mounts a deliberately damaged image and must return the exact error
           class the host built, with the instance left UNMOUNTED.

The host is the only judge of the disk bytes: this file parses block 0 with its
own struct/zlib decoder and reproduces every byte pattern independently, then
checks that the reader and the corrupt role left the image untouched. The
Guest's own PASS line and tohost=1 are both required; a marker with a
disagreeing executor code, a crash, a sanitizer report, a timeout or a step-limit
stop is a harness error, not an assertion.

Exit codes: 0 pass, 1 a check or the implementation under test failed, 2 a
broken harness, 77 the external cross compiler is missing (the only skip).
"""
import argparse
import os
import re
import shutil
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

ROLE_WRITER = 1
ROLE_READER = 2
ROLE_CORRUPT = 3
FAIL_BASE = 0x60000000
FAIL_READ_PERSIST = 30
CORRUPT = 10
UNSUPPORTED = 11

MAX_STEPS = "50000000"
TIMEOUT = 120


class HarnessError(RuntimeError):
    """A condition that is not a clean pass/fail of a Guest assertion."""


# --------------------------------------------------------------- byte patterns
# Every pattern is a function of the byte's index inside its file, matching the
# Guest source independently.

def _byte(scale, step, base, index):
    return (index * scale + (index >> 8) * step + base) & 0xFF


def guard_pattern():
    return bytes(_byte(7, 0, 9, i) for i in range(17))


def hello_pattern():
    return bytes(_byte(17, 29, 41, i) for i in range(5000))


def persist_pattern():
    return bytes(_byte(13, 5, 71, i) for i in range(5000))


def reuse_pattern():
    return bytes(_byte(3, 0, 101, i) for i in range(5))


# ---------------------------------------------------------------- the decoder

def metadata_block(capacity):
    """A canonical empty directory, built by this file and not by the tool."""
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


def parse_entries(data, capacity):
    """Independent decode of block 0. Raises AssertionError on any violation."""
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
        entries.append({"slot": slot, "name": name, "size": size,
                        "start": start, "count": count})
    return entries


# ------------------------------------------------------------- host processes

def run_host(command, timeout=60):
    try:
        return subprocess.run(command, capture_output=True, timeout=timeout,
                              stdin=subprocess.DEVNULL)
    except subprocess.TimeoutExpired as error:
        raise HarnessError("timeout running %r" % (command,)) from error
    except OSError as error:
        raise HarnessError("cannot run %r: %s" % (command, error))


def count_lines(path):
    lines = 0
    with open(path, "rb") as handle:
        for _ in handle:
            lines += 1
    return lines


def run_guest(executor, elf, image, trace):
    command = [str(executor), "--image", str(elf), "--disk-image", str(image),
               "--terminal", "--max-steps", MAX_STEPS, "--trace", str(trace)]
    result = run_host(command, timeout=TIMEOUT)
    steps = 0
    if trace.exists():
        steps = count_lines(trace)
        try:
            trace.unlink()
        except OSError:
            pass
    return result, steps


def guest_kind(result):
    blob = result.stdout + result.stderr
    if result.returncode < 0:
        return "HARNESS"
    if b"Sanitizer" in blob or b"runtime error:" in blob:
        return "HARNESS"
    if result.returncode == 0:
        return "PASS"
    if result.returncode == 6:
        return "ASSERTION"
    if result.returncode == 4:
        return "INCONCLUSIVE"
    return "HARNESS"


def guest_assertion(result):
    """The Guest's reported (code, phase), only when the executor agrees."""
    match = re.search(rb":FAIL: yanfs code=0x([0-9a-f]+) phase=(\d+)", result.stdout)
    if match is None:
        return None
    code = int(match.group(1), 16)
    phase = int(match.group(2))
    reported = re.search(rb"the Guest reported failure code (\d+)", result.stderr)
    if reported is None or int(reported.group(1)) != code:
        return None
    return code, phase


# ------------------------------------------------------------------ building

def require_sources(source):
    guest = source / "tests/guest"
    needed = [guest / "yanfs_check.c", guest / "guest_lib.c", guest / "link.ld",
              guest / "guest.h", source / "os/console.h",
              source / "os/trap_entry.S", source / "os/task.c",
              source / "os/task_switch.S", source / "os/block.c",
              source / "os/block.h", source / "os/console.c",
              source / "os/platform.h", source / "os/task.h",
              source / "os/yanfs.c", source / "os/yanfs.h",
              source / "os/yanfs_block.c", source / "os/yanfs_block.h"]
    for path in needed:
        if not path.is_file():
            print("FAIL the source under test is missing: %s" % path, file=sys.stderr)
            return False
    return True


def build_guest(gcc, source, out, role, corrupt_expect=None):
    guest = source / "tests/guest"
    command = [gcc, "-std=c17", "-march=rv32im", "-mabi=ilp32", "-mcmodel=medany",
               "-static", "-nostdlib", "-nostartfiles", "-ffreestanding",
               "-fno-builtin", "-fno-stack-protector", "-O2",
               "-Wall", "-Wextra", "-Wpedantic", "-Werror",
               "-I", str(guest), "-I", str(source / "os"),
               "-DYANFS_ROLE=%d" % role]
    if corrupt_expect is not None:
        command.append("-DYANFS_CORRUPT_EXPECT=%d" % corrupt_expect)
    command += ["-T", str(guest / "link.ld"),
                str(source / "os/trap_entry.S"), str(source / "os/task.c"),
                str(source / "os/task_switch.S"), str(source / "os/block.c"),
                str(source / "os/console.c"), str(source / "os/yanfs.c"),
                str(source / "os/yanfs_block.c"), str(guest / "guest_lib.c"),
                str(guest / "yanfs_check.c"),
                "-Wl,--build-id=none", "-o", str(out)]
    built = run_host(command, timeout=120)
    if built.returncode != 0:
        raise HarnessError("guest build failed (%s):\n%s%s"
                           % (out.name, built.stdout.decode(errors="replace"),
                              built.stderr.decode(errors="replace")))
    return out


# --------------------------------------------------------------- the fixtures

def fail(message):
    print(":FAIL: %s" % message, flush=True)
    return 1


def fresh_image_check(mkfs, path):
    proc = run_host([str(mkfs), "--image", str(path), "--blocks", str(BLOCKS)])
    if proc.returncode != 0:
        raise HarnessError("yan_mkfs failed: %s" % proc.stderr.decode(errors="replace"))
    data = path.read_bytes()
    if len(data) != IMAGE_BYTES:
        raise AssertionError("fresh image is %d bytes" % len(data))
    if data[:BLOCK] != metadata_block(BLOCKS):
        raise AssertionError("fresh metadata differs from the oracle")
    if any(data[BLOCK:]):
        raise AssertionError("fresh data blocks are not zero")


def expected_writer_block0():
    """The whole expected metadata block, built by this file and never by the
    production encoder. The 4096-byte compare below therefore guards name
    padding, every unused field and every empty slot."""
    block = bytearray(metadata_block(BLOCKS))
    put_entry(block, 0, "persist.bin", 5000, 3, 2)
    put_entry(block, 1, "guard.bin", 17, 2, 1)
    put_entry(block, 2, "empty.txt", 0, 0, 0)
    put_entry(block, 3, "reuse.txt", 5, 1, 1)
    reseal(block)
    return bytes(block)


def expected_writer_image():
    image = bytearray(BLOCK * BLOCKS)
    image[0:BLOCK] = expected_writer_block0()
    image[1 * BLOCK:1 * BLOCK + 5] = reuse_pattern()
    image[2 * BLOCK:2 * BLOCK + 17] = guard_pattern()
    persist = persist_pattern()
    image[3 * BLOCK:4 * BLOCK] = persist[:BLOCK]
    image[4 * BLOCK:4 * BLOCK + 904] = persist[BLOCK:]
    return bytes(image)


def check_writer_image(path):
    """The host's independent verdict on the image after the writer."""
    data = path.read_bytes()
    parse_entries(data, BLOCKS)
    if data[:BLOCK] != expected_writer_block0():
        raise AssertionError("block 0 differs from the independently built directory")
    if data[1 * BLOCK:1 * BLOCK + 5] != reuse_pattern():
        raise AssertionError("block 1 does not start with the reuse pattern")
    if any(data[1 * BLOCK + 5:2 * BLOCK]):
        raise AssertionError("block 1 tail is not zero")
    if data[2 * BLOCK:2 * BLOCK + 17] != guard_pattern():
        raise AssertionError("block 2 does not start with the guard pattern")
    if any(data[2 * BLOCK + 17:3 * BLOCK]):
        raise AssertionError("block 2 tail is not zero")
    persist = persist_pattern()
    if data[3 * BLOCK:4 * BLOCK] != persist[:BLOCK]:
        raise AssertionError("block 3 is not the first persist block")
    if data[4 * BLOCK:4 * BLOCK + 904] != persist[BLOCK:]:
        raise AssertionError("block 4 is not the persist tail")
    if any(data[4 * BLOCK + 904:5 * BLOCK]):
        raise AssertionError("block 4 tail is not zero")
    if any(data[5 * BLOCK:]):
        raise AssertionError("a neighbouring block was changed")


def expect_check_rejects(case, name, mutated, why):
    path = case / ("selftest-%s.img" % name)
    path.write_bytes(bytes(mutated))
    try:
        check_writer_image(path)
    except AssertionError:
        return
    raise AssertionError("self-test: %s was not caught" % why)


def writer_check_self_test(case):
    """Host-only negative controls for the decode above. Without these the
    decoder could only be proving itself."""
    good = expected_writer_image()
    check_writer_image_for = case / "selftest-good.img"
    check_writer_image_for.write_bytes(good)
    check_writer_image(check_writer_image_for)

    # A name padding byte after the terminator, with the CRC recomputed: the
    # whole-block compare must still reject it.
    padded = bytearray(good)
    padded[64 + 20] = 0x41
    reseal(padded)
    expect_check_rejects(case, "padding", padded, "a resealed name-padding change")

    # A neighbour block change and a last-block tail change.
    neighbour = bytearray(good)
    neighbour[5 * BLOCK] = 0x7F
    expect_check_rejects(case, "neighbour", neighbour, "a changed neighbour block")
    tail = bytearray(good)
    tail[4 * BLOCK + 904] = 0x7F
    expect_check_rejects(case, "tail", tail, "a non-zero last-block tail")


def run_pass(run_exe, elf, image, trace, marker, label):
    result, steps = run_guest(run_exe, elf, image, trace)
    blob = result.stdout + b"\n" + result.stderr
    if result.returncode < 0:
        raise HarnessError("%s died with signal %d" % (label, -result.returncode))
    if b"Sanitizer" in blob or b"runtime error:" in blob:
        raise HarnessError("%s produced sanitizer output" % label)
    if result.returncode == 4:
        raise HarnessError("%s stopped at the step limit without tohost" % label)
    if result.returncode == 6:
        assertion = guest_assertion(result)
        if assertion is None:
            raise HarnessError("%s reported a Guest failure whose marker and "
                               "executor code did not agree" % label)
        raise AssertionError("%s failed: %s" % (label, assertion))
    if result.returncode != 0:
        raise HarnessError("%s returned %d, neither PASS nor a Guest assertion"
                           % (label, result.returncode))
    if b":FAIL:" in result.stdout:
        raise HarnessError("%s printed a FAIL line but exited 0" % label)
    if marker not in result.stdout:
        raise HarnessError("%s exited 0 without its PASS marker" % label)
    return steps


def run_cases(args, source, case, run_exe, mkfs_exe, traces):
    base = case / "base.img"
    fresh_image_check(mkfs_exe, base)
    writer_check_self_test(case)
    print("PASS host image decoder self-test: expected accepted, padding/neighbour/"
          "tail mutations rejected")

    writer_elf = build_guest(args.gcc, source, case / "writer.elf", ROLE_WRITER)
    reader_elf = build_guest(args.gcc, source, case / "reader.elf", ROLE_READER)
    corrupt_elf = build_guest(args.gcc, source, case / "corrupt.elf", ROLE_CORRUPT,
                              corrupt_expect=CORRUPT)
    version_elf = build_guest(args.gcc, source, case / "version.elf", ROLE_CORRUPT,
                              corrupt_expect=UNSUPPORTED)

    # Negative control first: the reader on the empty image must fail on the
    # missing file with exactly the code and phase the Guest declares.
    early = case / "early.img"
    early.write_bytes(base.read_bytes())
    early_before = early.read_bytes()
    result, _ = run_guest(run_exe, reader_elf, early, traces / "early.trace")
    blob = result.stdout + b"\n" + result.stderr
    if result.returncode < 0:
        raise HarnessError("pre-writer reader died with signal %d" % -result.returncode)
    if b"Sanitizer" in blob or b"runtime error:" in blob:
        raise HarnessError("pre-writer reader produced sanitizer output")
    if result.returncode == 4:
        raise HarnessError("pre-writer reader hit the step limit")
    if result.returncode != 6:
        return fail("reader before the writer returned %d, not a Guest assertion"
                    % result.returncode)
    assertion = guest_assertion(result)
    if assertion != (FAIL_BASE | FAIL_READ_PERSIST, 2):
        return fail("reader before the writer reported %r, expected the NOT_FOUND "
                    "code at phase 2" % (assertion,))
    exact = b"yanfs: reader read persist.bin len=5000 result=7"
    if exact not in result.stdout:
        return fail("reader before the writer did not print the exact NOT_FOUND "
                    "line; the code alone cannot tell NOT_FOUND from IO/PROTOCOL")
    if early.read_bytes() != early_before:
        return fail("the pre-writer reader changed the image")
    print("PASS reader-before-writer: exact NOT_FOUND code 0x%08x at phase 2, "
          "image unchanged" % (FAIL_BASE | FAIL_READ_PERSIST))

    # Writer on the real image.
    disk = case / "disk.img"
    disk.write_bytes(base.read_bytes())
    steps = run_pass(run_exe, writer_elf, disk, traces / "writer.trace",
                     b"yanfs writer PASS", "writer")
    try:
        check_writer_image(disk)
    except AssertionError as error:
        return fail("host image decode: %s" % error)
    print("PASS writer: %d steps, %d disk bytes; host decode found persist.bin "
          "[3,5), guard.bin [2,3), reuse.txt [1,2), empty.txt no extent"
          % (steps, disk.stat().st_size))

    # Two fresh reader processes, neither changing a byte.
    after_writer = disk.read_bytes()
    for round_index in (1, 2):
        before = disk.read_bytes()
        steps = run_pass(run_exe, reader_elf, disk, traces / ("reader%d.trace" % round_index),
                         b"yanfs reader PASS", "reader round %d" % round_index)
        after = disk.read_bytes()
        if after != before:
            return fail("reader round %d changed the image" % round_index)
        print("PASS reader round %d: %d steps, image byte-identical" % (round_index, steps))
    if after_writer != disk.read_bytes():
        return fail("the readers changed the writer's image")

    # Corrupt fixtures. Each role run must reject with the exact class and write
    # nothing.
    crc_image = case / "crc.img"
    corrupted = bytearray(base.read_bytes())
    corrupted[100] ^= 0x5A
    crc_image.write_bytes(bytes(corrupted))
    crc_before = crc_image.read_bytes()
    steps = run_pass(run_exe, corrupt_elf, crc_image, traces / "crc.trace",
                     b"yanfs corrupt-rejected PASS", "corrupt CRC")
    if crc_image.read_bytes() != crc_before:
        return fail("the CRC-corrupt mount wrote to the image")
    print("PASS corrupt CRC: mount refused as CORRUPT, image unchanged (%d steps)" % steps)

    overlap_image = case / "overlap.img"
    overlap = bytearray(base.read_bytes())
    block0 = bytearray(metadata_block(BLOCKS))
    put_entry(block0, 0, "a", 8192, 1, 2)
    put_entry(block0, 1, "b", 8192, 2, 2)
    reseal(block0)
    overlap[0:BLOCK] = block0
    overlap_image.write_bytes(bytes(overlap))
    overlap_before = overlap_image.read_bytes()
    steps = run_pass(run_exe, corrupt_elf, overlap_image, traces / "overlap.trace",
                     b"yanfs corrupt-rejected PASS", "corrupt overlap")
    if overlap_image.read_bytes() != overlap_before:
        return fail("the overlapping-extent mount wrote to the image")
    print("PASS corrupt overlapping extent: valid CRC, mount refused as CORRUPT, "
          "image unchanged (%d steps)" % steps)

    version_image = case / "version.img"
    version = bytearray(base.read_bytes())
    version_block = bytearray(metadata_block(BLOCKS))
    struct.pack_into("<I", version_block, 8, 2)
    reseal(version_block)
    version[0:BLOCK] = version_block
    version_image.write_bytes(bytes(version))
    version_before = version_image.read_bytes()
    steps = run_pass(run_exe, version_elf, version_image, traces / "version.trace",
                     b"yanfs corrupt-rejected PASS", "corrupt version")
    if version_image.read_bytes() != version_before:
        return fail("the unknown-version mount wrote to the image")
    print("PASS corrupt version: valid CRC, mount refused as UNSUPPORTED, "
          "image unchanged (%d steps)" % steps)

    return 0


def main():
    parser = argparse.ArgumentParser(description="YanFS Guest end-to-end acceptance")
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--work", required=True, type=Path)
    parser.add_argument("--gcc", required=True)
    parser.add_argument("--run", required=True)
    parser.add_argument("--mkfs", required=True)
    args = parser.parse_args()

    if shutil.which(args.gcc) is None:
        print("SKIP: missing external cross compiler")
        return 77
    source = args.source.resolve()
    if not require_sources(source):
        return 1
    for label, path in (("executor", Path(args.run)), ("mkfs", Path(args.mkfs))):
        if not path.is_file() or not os.access(path, os.X_OK):
            print("FAIL the %s under test is missing: %s" % (label, path),
                  file=sys.stderr)
            return 1

    args.work.mkdir(parents=True, exist_ok=True)
    try:
        with tempfile.TemporaryDirectory(prefix="yanfs-case-", dir=args.work) as case_dir, \
             tempfile.TemporaryDirectory(prefix="yanfs-traces-") as trace_dir:
            return run_cases(args, source, Path(case_dir), Path(args.run),
                             Path(args.mkfs), Path(trace_dir))
    except HarnessError as error:
        print("HARNESS-ERROR: %s" % error, file=sys.stderr)
        return 2
    except AssertionError as error:
        print(":FAIL: %s" % error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
