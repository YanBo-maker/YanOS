#!/usr/bin/env python3
"""Independent Host verification for yan_mkfs (docs/specs/0021-yanfs.md).

The oracle is this file: it decodes the image byte by byte and recomputes the
metadata CRC with Python's zlib, so an image that only satisfied the tool's own
encoder would still be caught. The tool is exercised through subprocess only;
no C helper of the repository is linked in.

Exit codes follow the repository's gate discipline:
  0  every check passed
  1  a check failed (each one printed as ":FAIL: ..."), including a missing
     implementation under test -- missing implementation is never a skip
  2  HARNESS-ERROR: timeout, signal death, sanitizer report, or a fixture the
     host cannot create. This is an inconclusive run, not a detection.
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile
import zlib

BLOCK = 4096
MAGIC = b"YANFS01\x00"
FAILURES = []


class HarnessError(RuntimeError):
    """A condition that is not a clean pass/fail of the tool under test."""


def check(condition, message):
    if not condition:
        FAILURES.append(message)
        print(":FAIL: " + message, flush=True)


def run(command, env=None, timeout=30):
    try:
        proc = subprocess.run(
            command,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired as exc:
        raise HarnessError("timeout running %r" % (command,)) from exc
    if proc.returncode < 0:
        raise HarnessError("%r died with signal %d" % (command, -proc.returncode))
    stderr = proc.stderr
    if (b"Sanitizer" in stderr or b"runtime error:" in stderr
            or b"HARNESS-ERROR" in stderr):
        raise HarnessError("%r produced sanitizer/harness output: %r"
                           % (command, stderr[:200]))
    return proc


def invoke(binary, image, blocks, env=None):
    return run([binary, "--image", image, "--blocks", str(blocks)], env=env)


def read_file(path):
    with open(path, "rb") as handle:
        return handle.read()


def write_file(path, data):
    with open(path, "wb") as handle:
        handle.write(data)


def verify_directory(data, capacity):
    """Decode block 0 with this file's own expectations, not the tool's."""
    check(len(data) == BLOCK, "block 0 is exactly 4096 bytes")
    check(data[0:8] == MAGIC, "magic is YANFS01 0x00")
    check(struct.unpack_from("<I", data, 8)[0] == 1, "version is 1")
    check(struct.unpack_from("<I", data, 12)[0] == BLOCK, "block_size is 4096")
    check(struct.unpack_from("<I", data, 16)[0] == capacity,
          "capacity_blocks matches --blocks")
    check(struct.unpack_from("<I", data, 20)[0] == 63, "entry_capacity is 63")
    check(all(byte == 0 for byte in data[24:60]),
          "header reserved bytes are zero")
    expected = zlib.crc32(data[:60] + b"\x00\x00\x00\x00" + data[64:]) & 0xFFFFFFFF
    check(struct.unpack_from("<I", data, 60)[0] == expected,
          "metadata CRC matches the Python zlib oracle")
    check(all(byte == 0 for byte in data[64:]),
          "all 63 directory slots are empty")


def normal_case(mkfs, work):
    for capacity in (1, 16):
        path = os.path.join(work, "normal-%d.img" % capacity)
        proc = invoke(mkfs, path, capacity)
        check(proc.returncode == 0,
              "mkfs %d blocks exits 0, got %d" % (capacity, proc.returncode))
        check(os.path.exists(path), "mkfs %d creates the image" % capacity)
        check(proc.stdout != b"", "success reports on stdout")
        check(os.fsencode(path) in proc.stdout, "stdout names the image")
        check(str(capacity).encode() in proc.stdout, "stdout names the capacity")
        check(os.path.getsize(path) == capacity * BLOCK,
              "image size is capacity * 4096")
        data = read_file(path)
        verify_directory(data[:BLOCK], capacity)
        check(all(byte == 0 for byte in data[BLOCK:]),
              "every data block is zero (capacity %d)" % capacity)


def existing_case(mkfs, work):
    path = os.path.join(work, "existing.img")
    original = bytes((index * 7 + 3) & 0xFF for index in range(9000))
    write_file(path, original)
    proc = invoke(mkfs, path, 2)
    check(proc.returncode == 5, "existing file exits 5, got %d" % proc.returncode)
    check(read_file(path) == original, "existing file bytes are unchanged")

    target = os.path.join(work, "target.img")
    write_file(target, b"TARGET")
    link = os.path.join(work, "link.img")
    dangling = os.path.join(work, "dangling.img")
    absent = os.path.join(work, "absent.img")
    try:
        os.symlink(target, link)
        os.symlink(absent, dangling)
    except (OSError, NotImplementedError) as exc:
        raise HarnessError("cannot create symlink fixtures: %s" % exc)

    proc = invoke(mkfs, link, 1)
    check(proc.returncode == 5, "symlink exits 5, got %d" % proc.returncode)
    check(read_file(target) == b"TARGET", "symlink target is unchanged")

    proc = invoke(mkfs, dangling, 1)
    check(proc.returncode == 5, "dangling symlink exits 5, got %d" % proc.returncode)
    check(os.path.lexists(dangling), "dangling symlink itself is untouched")
    check(not os.path.exists(absent), "a dangling link does not create its target")


def arguments_case(mkfs, work):
    path = os.path.join(work, "arg.img")
    cases = [
        [mkfs],
        [mkfs, "--image", path],
        [mkfs, "--blocks", "1"],
        [mkfs, "--image", path, "--blocks", "0"],
        [mkfs, "--image", path, "--blocks", "abc"],
        [mkfs, "--image", path, "--blocks", "1.5"],
        [mkfs, "--image", path, "--blocks", "0x10"],
        [mkfs, "--image", path, "--blocks", "-1"],
        [mkfs, "--image", path, "--blocks", "4294967296"],
        [mkfs, "--image", path, "--blocks", "18446744073709551616"],
        [mkfs, "--image", path, "--blocks", "1", "--blocks", "2"],
        [mkfs, "--image", path, "--image", path, "--blocks", "1"],
        [mkfs, "--image", path, "--blocks", "1", "extra"],
        [mkfs, "--blocks", "1", "--image"],
        [mkfs, "--unknown", path, "--blocks", "1"],
    ]
    for command in cases:
        proc = run(command)
        check(proc.returncode == 2,
              "invalid arguments exit 2: %r got %d" % (command[1:], proc.returncode))
    check(not os.path.exists(path), "rejected arguments create no image")

    missing_parent = os.path.join(work, "no-such-dir", "img")
    proc = invoke(mkfs, missing_parent, 1)
    check(proc.returncode == 5,
          "missing parent exits 5, got %d" % proc.returncode)


def build_metadata(capacity):
    """The canonical empty directory for capacity, built by this file's own
    oracle. Used for the partial-directory fault, where the tool's encoder is
    never allowed to be the judge."""
    block = bytearray(BLOCK)
    block[0:8] = MAGIC
    struct.pack_into("<I", block, 8, 1)
    struct.pack_into("<I", block, 12, BLOCK)
    struct.pack_into("<I", block, 16, capacity)
    struct.pack_into("<I", block, 20, 63)
    crc = zlib.crc32(bytes(block[:60]) + b"\x00\x00\x00\x00"
                     + bytes(block[64:])) & 0xFFFFFFFF
    struct.pack_into("<I", block, 60, crc)
    return bytes(block)


def fault_case(mkfs, fault_mkfs, work, mode):
    path = os.path.join(work, "fault-%s.img" % mode)
    env = dict(os.environ, YAN_TEST_MKFS_FAULT=mode)
    proc = invoke(fault_mkfs, path, 4, env=env)
    check(proc.returncode == 5,
          "%s fault exits 5, got %d" % (mode, proc.returncode))
    marker = b"YAN_TEST_MKFS_FAULT:" + mode.encode()
    check(marker in proc.stderr, "%s fault marker on stderr" % mode)
    check(b"wrote" not in proc.stdout and b"formatted" not in proc.stdout,
          "%s fault does not claim success" % mode)
    check(os.path.exists(path), "%s fault keeps the partial image" % mode)

    data = read_file(path)
    full = 4 * BLOCK
    if mode == "short-write":
        check(0 < len(data) < full,
              "short-write really left a partial image (size %d)" % len(data))
    elif mode == "seek":
        check(len(data) == full,
              "seek fault left the full-size zero image (size %d)" % len(data))
        check(all(byte == 0 for byte in data),
              "seek fault wrote no metadata: the whole image is still zero")
    elif mode == "metadata-write":
        check(len(data) == full,
              "metadata-write fault left the full-size image (size %d)" % len(data))
        want = build_metadata(4)
        check(data[:32] == want[:32],
              "metadata-write wrote the real first 32 metadata bytes")
        check(all(byte == 0 for byte in data[32:BLOCK]),
              "the rest of block 0 is still zero, not a complete directory")
        check(all(byte == 0 for byte in data[BLOCK:]),
              "metadata-write kept every data block zero")
    else:
        check(len(data) == full,
              "%s fault left the full-size image (size %d)" % (mode, len(data)))

    again = invoke(mkfs, path, 4)
    check(again.returncode == 5,
          "%s leftover is refused by a normal mkfs, got %d"
          % (mode, again.returncode))
    check(read_file(path) == data,
          "%s leftover bytes are unchanged by the refused overwrite" % mode)


def control_case(fault_mkfs, work):
    path = os.path.join(work, "control.img")
    env = {key: value for key, value in os.environ.items()
           if key != "YAN_TEST_MKFS_FAULT"}
    proc = invoke(fault_mkfs, path, 2, env=env)
    check(proc.returncode == 0,
          "fault binary without the env succeeds, got %d" % proc.returncode)
    check(b"YAN_TEST_MKFS_FAULT" not in proc.stderr,
          "control run prints no fault marker")
    data = read_file(path)
    verify_directory(data[:BLOCK], 2)
    check(all(byte == 0 for byte in data[BLOCK:]), "control image data is zero")


def main():
    parser = argparse.ArgumentParser(description="verify yan_mkfs independently")
    parser.add_argument("--mkfs", required=True)
    parser.add_argument("--fault-mkfs", required=True)
    parser.add_argument("--work", required=True)
    args = parser.parse_args()

    # A missing implementation is a hard failure, never a skip: this test needs
    # no external reference model or cross toolchain.
    for path in (args.mkfs, args.fault_mkfs):
        if not os.path.isfile(path) or not os.access(path, os.X_OK):
            print(":FAIL: implementation missing or not executable: %s" % path)
            return 1

    os.makedirs(args.work, exist_ok=True)
    try:
        with tempfile.TemporaryDirectory(prefix="yanfs-mkfs-", dir=args.work) as work:
            normal_case(args.mkfs, work)
            existing_case(args.mkfs, work)
            arguments_case(args.mkfs, work)
            for mode in ("short-write", "flush", "close", "seek", "metadata-write"):
                fault_case(args.mkfs, args.fault_mkfs, work, mode)
            control_case(args.fault_mkfs, work)
    except HarnessError as exc:
        print("HARNESS-ERROR: %s" % exc, file=sys.stderr)
        return 2

    if FAILURES:
        print("%d failure(s)" % len(FAILURES))
        return 1
    print("PASS yanfs_mkfs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
