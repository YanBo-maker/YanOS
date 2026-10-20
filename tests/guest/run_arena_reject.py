#!/usr/bin/env python3
"""0036-R3-AC3: the loader rejects an arena past the rings, and a too-small
arena fails startup without touching the image.

The linker script's `__yan_arena_end` is an absolute assignment, so GNU ld's
`--defsym` does not override it (verified while writing this test). Each case
therefore links with a generated copy of os/guest.ld whose end value differs;
that changes exactly the declared arena length and nothing else.
"""
import argparse
import hashlib
import re
import struct
import subprocess
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_terminal_runtime as runtime  # noqa: E402

APP_REASON_ARENA = 24
ARENA_FAIL_CODE = runtime.APP_FAIL_TOP | APP_REASON_ARENA
FAILURES = []


def check(condition, message):
    if not condition:
        FAILURES.append(message)
        print(":FAIL: " + message, flush=True)


def u16(data, offset):
    return struct.unpack_from("<H", data, offset)[0]


def u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def arena_start_of(elf):
    data = elf.read_bytes()
    phoff = u32(data, 28)
    phentsize = u16(data, 42)
    phnum = u16(data, 44)
    ends = []
    for index in range(phnum):
        at = phoff + index * phentsize
        if u32(data, at) == 1:
            ends.append(u32(data, at + 8) + u32(data, at + 20))
    return (max(ends) + 15) & ~15


def patched_ld(source, path, end):
    text = (source / "os" / "guest.ld").read_text()
    replaced, count = re.subn(r"__yan_arena_end\s*=\s*[^;]+;",
                              "__yan_arena_end = 0x%08x;" % end, text)
    if count != 1:
        raise RuntimeError("os/guest.ld does not define __yan_arena_end once")
    path.write_text(replaced)


def run(argv, stdin=None):
    return subprocess.run([str(a) for a in argv],
                          input=stdin,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--work", required=True, type=Path)
    parser.add_argument("--gcc", required=True)
    parser.add_argument("--run", required=True, type=Path)
    parser.add_argument("--mkfs", required=True, type=Path)
    parser.add_argument("--guest", required=True, type=Path)
    args = parser.parse_args()
    if not runtime.tool_available(args.gcc):
        print("SKIP cross compiler")
        return 77
    if not args.guest.is_file():
        print(":FAIL: missing production ELF %s" % args.guest)
        return 1
    args.work.mkdir(parents=True, exist_ok=True)
    start = arena_start_of(args.guest)

    tiny = args.work / "arena_over.S"
    tiny.write_text(
        '.section .text.start,"ax"\n'
        '.globl _start\n'
        "_start:\n"
        "  la t0, tohost\n"
        "  li t1, 1\n"
        "  sw t1, 0(t0)\n"
        "1: j 1b\n"
        '.section .tohost,"aw"\n'
        '.globl tohost\n'
        "tohost: .word 0\n")
    over_ld = args.work / "arena_over.ld"
    patched_ld(args.source, over_ld, 0x84000000)
    over_elf = args.work / "arena_over.elf"
    built = run([args.gcc, *runtime.CROSS_FLAGS, "-T", over_ld,
                 "-Wl,--build-id=none", "-o", over_elf, tiny])
    if built.returncode != 0:
        print(":HARNESS-ERROR: over-arena build failed:\n%s"
              % built.stderr.decode(errors="replace"))
        return 2
    result = run([args.run, "--image", over_elf, "--disk", "1",
                  "--max-steps", "100000"])
    check(result.returncode != 0,
          "over-arena ELF ran to completion (exit 0)")
    check(b"arena" in result.stderr,
          "over-arena ELF was not rejected with an arena diagnostic")
    check(result.stdout == b"",
          "over-arena Guest executed and produced output")
    print("0036-R3-AC3 over=rejected exec=no rc=%d" % result.returncode,
          flush=True)

    small_end = start + 0x1000
    small_ld = args.work / "arena_small.ld"
    patched_ld(args.source, small_ld, small_end)
    small_elf = args.work / "arena_small.elf"
    built = run([args.gcc, *runtime.CROSS_FLAGS, "-I", args.source / "os",
                 "-T", small_ld, "-Wl,--build-id=none", "-o", small_elf,
                 *[args.source / rel for rel in runtime.PRODUCTION_SOURCES]])
    if built.returncode != 0:
        print(":HARNESS-ERROR: small-arena build failed:\n%s"
              % built.stderr.decode(errors="replace"))
        return 2
    image_path = args.work / "ac3-4095.img"
    if image_path.exists():
        image_path.unlink()
    made = run([args.mkfs, "--image", image_path, "--blocks", "128",
                "--files", "4095"])
    if made.returncode != 0:
        print(":HARNESS-ERROR: mkfs failed: %s"
              % made.stderr.decode(errors="replace"))
        return 2
    before = hashlib.sha256(image_path.read_bytes()).hexdigest()
    result = run([args.run, "--image", small_elf, "--disk-image", image_path,
                  "--terminal", "--max-steps", "20000000"], stdin=b"")
    after = hashlib.sha256(image_path.read_bytes()).hexdigest()
    check(result.returncode == 6,
          "small-arena run exit %d, expected Guest failure 6"
          % result.returncode)
    check(("failure code %d" % ARENA_FAIL_CODE).encode() in result.stderr,
          "small-arena run did not report APP_REASON_ARENA (%d)"
          % ARENA_FAIL_CODE)
    check(b"ERROR ARENA" in result.stdout,
          "small-arena run did not print ERROR ARENA")
    check(before == after, "small-arena run changed the image")
    print("0036-R3-AC3 small=ARENA code=%d sha=unchanged start=0x%08x end=0x%08x"
          % (ARENA_FAIL_CODE, start, small_end), flush=True)
    for failure in FAILURES:
        print(":FAIL: " + failure, flush=True)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError) as error:
        print(":HARNESS-ERROR: %s" % error)
        sys.exit(2)
