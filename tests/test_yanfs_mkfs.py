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
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
import zlib

BLOCK = 4096
MAGIC = b"YANFS01\x00"
FAILURES = []

# 0036-R1-AC1: SHA-256 of the --blocks 16 image written by yan_mkfs built from
# origin/main 41b83b5. Obtained by extracting /mnt/e/YanOS-work/t41b83b5.tar
# (Windows git archive of 41b83b5) to /tmp, compiling
#   gcc -std=c17 -O2 -I os tools/yan_mkfs.c os/yanfs.c -o /tmp/old_mkfs
# and running `/tmp/old_mkfs --image /tmp/old16.img --blocks 16`.
V1_16_BLOCK_SHA256 = (
    "7baad595689d29db2a2ee41571e6cb387caa7677b3a9c132b5008503db93fb75"
)


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


def invoke(binary, image, blocks, env=None, files=63):
    return run([binary, "--image", image, "--blocks", str(blocks),
                "--files", str(files)], env=env)


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
        [mkfs, "--image", path, "--blocks", "16", "--files", "63",
         "--files", "63"],
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


def v1_baseline_case(mkfs, work):
    """0036-R1-AC1: --files 63 is byte-identical to the 41b83b5 --blocks 16
    image, whose SHA-256 was captured outside this repository."""
    path = os.path.join(work, "v1-ac1.img")
    proc = invoke(mkfs, path, 16, files=63)
    check(proc.returncode == 0,
          "R1-AC1 v1 image exits 0, got %d" % proc.returncode)
    data = read_file(path)
    check(len(data) == 16 * BLOCK, "R1-AC1 image has 16 blocks")
    verify_directory(data[:BLOCK], 16)
    digest = hashlib.sha256(data).hexdigest()
    check(digest == V1_16_BLOCK_SHA256,
          "R1-AC1 SHA-256 %s != 41b83b5 baseline %s"
          % (digest, V1_16_BLOCK_SHA256))


def v2_header_case(mkfs, work):
    """0036-R1-AC2: the default is 1023 files. With 64 blocks that means D=16,
    N=1023, version 2; the R3 PR removed the temporary explicit --files 1023
    that PR1 needed before the default changed (spec 实施澄清, #62)."""
    path = os.path.join(work, "v2-64.img")
    proc = run([mkfs, "--image", path, "--blocks", "64"])
    check(proc.returncode == 0,
          "R1-AC2 v2 image exits 0, got %d" % proc.returncode)
    data = read_file(path)
    check(len(data) == 64 * BLOCK, "R1-AC2 image has 64 blocks")
    check(data[0:8] == MAGIC, "R1-AC2 magic is YANFS01 0x00")
    check(struct.unpack_from("<I", data, 8)[0] == 2,
          "R1-AC2 version is 2")
    check(struct.unpack_from("<I", data, 12)[0] == BLOCK,
          "R1-AC2 block_size is 4096")
    check(struct.unpack_from("<I", data, 16)[0] == 64,
          "R1-AC2 capacity_blocks is 64")
    check(struct.unpack_from("<I", data, 20)[0] == 1023,
          "R1-AC2 entry_capacity is 1023")
    check(struct.unpack_from("<I", data, 24)[0] == 16,
          "R1-AC2 directory_blocks is 16")
    check(all(byte == 0 for byte in data[28:60]),
          "R1-AC2 reserved header bytes 28..59 are zero")
    directory = bytearray(data[:16 * BLOCK])
    directory[60:64] = b"\x00\x00\x00\x00"
    expected = zlib.crc32(bytes(directory)) & 0xFFFFFFFF
    check(struct.unpack_from("<I", data, 60)[0] == expected,
          "R1-AC2 CRC covers all 16 directory blocks with field 60..63 zero")
    check(all(byte == 0 for byte in data[BLOCK:16 * BLOCK]),
          "R1-AC2 directory blocks 1..15 are zero")
    print("0036-R1-AC2 default files=1023 version=2 directory_blocks=16")


def files_rejection_case(mkfs, work):
    """0036-R1-AC3: a bad --files value or B <= D creates no image.

    The rejected set is the contract's own: a non-decimal form, a value outside
    63..4095, or a value that is not 64*D-1. Every case must exit 2, print
    nothing on stdout, explain itself on stderr and leave no path behind."""
    cases = [
        ("zero", ["--files", "0", "--blocks", "64"]),
        ("hundred", ["--files", "100", "--blocks", "64"]),
        ("4096", ["--files", "4096", "--blocks", "8192"]),
        ("8191", ["--files", "8191", "--blocks", "8192"]),
        ("too-small-blocks", ["--files", "4095", "--blocks", "64"]),
        # Range neighbours: 62 is below the floor; 64 is legal range but is not
        # 64*1-1; 4094 is below the ceiling without being 64*64-1.
        ("sixty-two", ["--files", "62", "--blocks", "64"]),
        ("sixty-four", ["--files", "64", "--blocks", "64"]),
        ("4094", ["--files", "4094", "--blocks", "8192"]),
        # Geometry neighbours of D=2, D=63 and D=64: one short and one long.
        ("126", ["--files", "126", "--blocks", "64"]),
        ("128", ["--files", "128", "--blocks", "64"]),
        ("4030", ["--files", "4030", "--blocks", "8192"]),
        ("4032", ["--files", "4032", "--blocks", "8192"]),
        # Lexical forms that are not plain unsigned decimal.
        ("empty", ["--files", "", "--blocks", "64"]),
        ("sign", ["--files", "+63", "--blocks", "64"]),
        ("negative", ["--files", "-63", "--blocks", "64"]),
        ("fraction", ["--files", "63.0", "--blocks", "64"]),
        ("hex", ["--files", "0x3f", "--blocks", "64"]),
        ("leading-space", ["--files", " 63", "--blocks", "64"]),
        ("trailing-space", ["--files", "63 ", "--blocks", "64"]),
        ("trailing-junk", ["--files", "63a", "--blocks", "64"]),
        ("overflow", ["--files", "4294967296", "--blocks", "64"]),
    ]
    for label, extra in cases:
        path = os.path.join(work, "reject-%s.img" % label)
        proc = run([mkfs, "--image", path] + extra)
        check(proc.returncode == 2,
              "R1-AC3 %s exits 2, got %d" % (label, proc.returncode))
        check(proc.stdout == b"",
              "R1-AC3 %s prints nothing on stdout" % label)
        check(proc.stderr != b"",
              "R1-AC3 %s explains itself on stderr" % label)
        check(not os.path.exists(path),
              "R1-AC3 %s creates no image" % label)

    # A malformed --files is an argument problem: the diagnostic names --files,
    # and an unusable --blocks is still reported first because --blocks is
    # validated before --files.
    path = os.path.join(work, "reject-precedence.img")
    proc = run([mkfs, "--image", path, "--blocks", "64", "--files", "100"])
    check(b"invalid --files" in proc.stderr,
          "R1-AC3 a rejected --files is named in the diagnostic: %r"
          % proc.stderr)
    proc = run([mkfs, "--image", path, "--blocks", "abc", "--files", "100"])
    check(b"invalid --blocks" in proc.stderr,
          "R1-AC3 an unusable --blocks is reported before --files: %r"
          % proc.stderr)
    # A missing option value is an argument error, not a capacity error.
    proc = run([mkfs, "--image", path, "--blocks", "64", "--files"])
    check(proc.returncode == 2,
          "R1-AC3 a --files with no value exits 2, got %d" % proc.returncode)
    check(b"usage:" in proc.stderr,
          "R1-AC3 a --files with no value prints the usage text")
    check(not os.path.exists(path),
          "R1-AC3 the rejected argument cases create no image")


def verify_v2_directory(data, capacity, files, directory_blocks):
    """Decode a v2 image with this file's own expectations.

    Everything here comes from the contract, not from the tool: the version,
    directory block count and entry capacity are read back out of the image and
    the CRC is recomputed over the directory region with field 60..63 zeroed."""
    check(len(data) == capacity * BLOCK, "v2 image is capacity * 4096 bytes")
    header = data[:BLOCK]
    check(header[0:8] == MAGIC, "v2 magic is YANFS01 0x00")
    check(struct.unpack_from("<I", header, 8)[0] == 2, "v2 version is 2")
    check(struct.unpack_from("<I", header, 12)[0] == BLOCK,
          "v2 block_size is 4096")
    check(struct.unpack_from("<I", header, 16)[0] == capacity,
          "v2 capacity_blocks matches --blocks")
    check(struct.unpack_from("<I", header, 20)[0] == files,
          "v2 entry_capacity is the requested --files")
    check(struct.unpack_from("<I", header, 24)[0] == directory_blocks,
          "v2 directory_blocks is %d for --files %d"
          % (directory_blocks, files))
    check(all(byte == 0 for byte in header[28:60]),
          "v2 reserved header bytes 28..59 are zero")
    directory = bytearray(data[:directory_blocks * BLOCK])
    directory[60:64] = b"\x00\x00\x00\x00"
    expected = zlib.crc32(bytes(directory)) & 0xFFFFFFFF
    check(struct.unpack_from("<I", header, 60)[0] == expected,
          "v2 CRC matches the Python zlib oracle over %d directory blocks"
          % directory_blocks)
    check(all(byte == 0 for byte in data[BLOCK:directory_blocks * BLOCK]),
          "v2 directory blocks 1..%d are zero" % (directory_blocks - 1))
    check(all(byte == 0 for byte in data[directory_blocks * BLOCK:]),
          "v2 data blocks are zero")


def files_acceptance_case(mkfs, work):
    """0036-R1: the accepted --files inputs, including the geometry, leading
    zeros, the D boundaries and the approved default.

    The exhaustive D=1..64 differential belongs to the private operator proof;
    the default suite keeps the cheap boundary coverage."""
    plain = os.path.join(work, "accept-plain.img")
    padded = os.path.join(work, "accept-padded.img")
    proc = run([mkfs, "--image", plain, "--blocks", "16", "--files", "63"])
    check(proc.returncode == 0,
          "leading-zero base image exits 0, got %d" % proc.returncode)
    proc = run([mkfs, "--image", padded, "--blocks", "00016",
                "--files", "00063"])
    check(proc.returncode == 0,
          "leading zeros in --blocks and --files exit 0, got %d"
          % proc.returncode)
    check(read_file(plain) == read_file(padded),
          "leading zeros produce a byte-identical image")
    check(struct.unpack_from("<I", read_file(plain), 8)[0] == 1,
          "--files 63 keeps version 1")

    plain_max = os.path.join(work, "accept-plain-max.img")
    padded_max = os.path.join(work, "accept-padded-max.img")
    proc = run([mkfs, "--image", plain_max, "--blocks", "65",
                "--files", "4095"])
    check(proc.returncode == 0,
          "--files 4095 with 65 blocks exits 0, got %d" % proc.returncode)
    proc = run([mkfs, "--image", padded_max, "--blocks", "00065",
                "--files", "04095"])
    check(proc.returncode == 0,
          "leading zeros on 4095 exit 0, got %d" % proc.returncode)
    check(read_file(plain_max) == read_file(padded_max),
          "leading zeros on 4095 produce a byte-identical image")

    # The default must be the same image as an explicit 1023: one request, two
    # spellings.
    default_path = os.path.join(work, "accept-default.img")
    explicit_path = os.path.join(work, "accept-explicit.img")
    proc = run([mkfs, "--image", default_path, "--blocks", "64"])
    check(proc.returncode == 0,
          "default image exits 0, got %d" % proc.returncode)
    proc = run([mkfs, "--image", explicit_path, "--blocks", "64",
                "--files", "1023"])
    check(proc.returncode == 0,
          "explicit 1023 exits 0, got %d" % proc.returncode)
    check(read_file(default_path) == read_file(explicit_path),
          "the default image is byte-identical to an explicit --files 1023")

    # Boundary geometry, each at the smallest --blocks the contract allows:
    # D>=2 needs at least one data block beyond the directory.
    for files, capacity, directory_blocks in (
            (127, 3, 2),
            (4031, 64, 63),
            (4095, 65, 64),
    ):
        path = os.path.join(work, "accept-files%d.img" % files)
        proc = run([mkfs, "--image", path, "--blocks", str(capacity),
                    "--files", str(files)])
        check(proc.returncode == 0,
              "accepted --files %d with --blocks %d exits 0, got %d"
              % (files, capacity, proc.returncode))
        check(str(files).encode() in proc.stdout,
              "stdout reports the accepted --files %d" % files)
        verify_v2_directory(read_file(path), capacity, files, directory_blocks)


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
            v1_baseline_case(args.mkfs, work)
            v2_header_case(args.mkfs, work)
            files_rejection_case(args.mkfs, work)
            files_acceptance_case(args.mkfs, work)
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
