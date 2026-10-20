#!/usr/bin/env python3
"""0036-R3-AC2: the terminal image plus the largest arena stays inside 32 MiB.

The three 64 KiB context assertions are compile-time: this test runs only after
the Host suites and the RV32 terminal target built. Here the produced terminal
ELF's PT_LOAD spans and the documented allocation order give the Guest's
worst-case footprint. The oracle is this file's own ELF decoder, not readelf.
"""
import argparse
import struct
import sys
from pathlib import Path

BLOCK = 4096
LIMIT = 32 * 1024 * 1024
ARENA_END = 0x80000000 + LIMIT
FAILURES = []


def u16(data, offset):
    return struct.unpack_from("<H", data, offset)[0]


def u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def load_segments(data):
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        raise ValueError("not a little-endian ELF32 image")
    phoff = u32(data, 28)
    phentsize = u16(data, 42)
    phnum = u16(data, 44)
    if phentsize != 32:
        raise ValueError("unexpected program header size")
    spans = []
    for index in range(phnum):
        at = phoff + index * phentsize
        if u32(data, at) != 1:  # PT_LOAD
            continue
        start = u32(data, at + 8)
        memsz = u32(data, at + 20)
        spans.append((start, start + memsz))
    return spans


def expected_arena(files, start):
    sizes = [
        (files + 1) * 64,
        files * 36,
        files * 32,
        files * 36,
        files,
        files * 36,
        files * 32,
    ]
    current = start
    for size in sizes:
        current = (current + 15) & ~15
        current += size
    return current - start


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--guest", required=True, type=Path)
    args = parser.parse_args()
    data = args.guest.read_bytes()
    try:
        spans = load_segments(data)
    except ValueError as error:
        print(":FAIL: cannot decode the terminal ELF: %s" % error)
        return 1
    if not spans:
        print(":FAIL: the terminal ELF has no PT_LOAD segment")
        return 1
    load_bytes = sum(end - start for start, end in spans)
    arena_start = (max(end for _, end in spans) + 15) & ~15
    oracle = {63: 15008, 1023: 242528, 2047: 485216, 4095: 970592}
    arena = {}
    for files in (63, 1023, 2047, 4095):
        arena[files] = expected_arena(files, arena_start)
        print("0036-R3-AC2 N=%d arena=%d oracle=%d"
              % (files, arena[files], oracle[files]), flush=True)
        if arena[files] != oracle[files]:
            FAILURES.append("N=%d is not the approved arena size" % files)
    total = load_bytes + arena[4095]
    print("0036-R3-AC2 load_bytes=%d arena4095=%d total=%d limit=%d"
          % (load_bytes, arena[4095], total, LIMIT), flush=True)
    if arena_start % 16 != 0:
        FAILURES.append("arena_start is not 16-byte aligned")
    if arena_start + arena[4095] > ARENA_END:
        FAILURES.append("arena_start + arena(4095) exceeds 0x82000000")
    if total > LIMIT:
        FAILURES.append("load segments + arena(4095) exceed 32 MiB")
    for failure in FAILURES:
        print(":FAIL: " + failure, flush=True)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
