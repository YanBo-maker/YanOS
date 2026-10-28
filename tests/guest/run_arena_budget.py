#!/usr/bin/env python3
"""0036-R3-AC2: the terminal image plus the largest arena stays inside 32 MiB.

The budget is the produced terminal ELF's independent PT_LOAD spans plus the
arena high-water mark actually observed while that same ELF boots an N=4095
image. The measurement runs in this test's own work directory through the
Host-side `yan_arena_alloc` boundary trace; it does not read the Guest's final
`yan_arena_used_bytes` word. The allocation-order formula is only a
supplementary consistency check and never replaces the measured value.

The three 64 KiB context assertions are compile-time: this test runs only after
the Host suites and the RV32 terminal target built. The ELF decoder below is
this file's own, not readelf.
"""
import argparse
import struct
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import arena_measure  # noqa: E402
import run_terminal_runtime as runtime  # noqa: E402

BLOCK = 4096
LIMIT = 32 * 1024 * 1024
ARENA_END = 0x80000000 + LIMIT
ORACLE_4095 = 1003358
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
    """The documented allocation order, used only as a supplementary check."""
    sizes = [
        (files + 1) * 64,
        files * 36,
        files * 32,
        files * 36,
        files,
        files * 36,
        files * 32,
        files * 2,  # name_order
        files * 2,  # metadata sort indices
        files * 2,  # relation sort indices
        files * 2,  # block sort indices
    ]
    current = start
    for size in sizes:
        current = (current + 15) & ~15
        current += size
    return current - start


def boundary_failures(data, arena_bytes):
    """Judge one ELF's load spans plus a measured arena against the 32 MiB
    boundary. Exposed so the pure-Python harness control can test the decoder
    without a compiler or a Guest run."""
    try:
        spans = load_segments(data)
    except (ValueError, struct.error) as error:
        return ["cannot decode the terminal ELF: %s" % error]
    if not spans:
        return ["the terminal ELF has no PT_LOAD segment"]
    load_bytes = sum(end - start for start, end in spans)
    arena_start = (max(end for _, end in spans) + 15) & ~15
    failures = []
    total = load_bytes + arena_bytes
    if arena_start % 16 != 0:
        failures.append("arena_start is not 16-byte aligned")
    if arena_start + arena_bytes > ARENA_END:
        failures.append("arena_start + measured arena(4095) exceeds 0x82000000")
    if total > LIMIT:
        failures.append("load segments + measured arena(4095) exceed 32 MiB")
    return failures


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--work", required=True, type=Path)
    parser.add_argument("--mkfs", required=True, type=Path)
    parser.add_argument("--guest", required=True, type=Path)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--max-steps", type=int, default=60000000)
    args = parser.parse_args()
    local_problem = arena_measure.local_input_problem(
        args.source, args.guest, args.mkfs)
    if local_problem is not None:
        print(":FAIL: " + local_problem)
        return 1
    if not runtime.tool_available(args.cc):
        print("SKIP host compiler")
        return 77
    args.work.mkdir(parents=True, exist_ok=True)
    try:
        driver = runtime.build_host(args.source, args.work, args.cc)

        class RunArgs:
            pass

        run_args = RunArgs()
        run_args.source = args.source
        run_args.work = args.work
        run_args.guest = args.guest
        image = arena_measure.make_image(
            args.mkfs, args.work / "ac2-4095.img", 4095, 65)
        trace, values, _disk, _capture = arena_measure.measure(
            driver, run_args, args.work, image, "ac2-4095", args.max_steps)
    except runtime.HarnessError as error:
        print(":HARNESS-ERROR: %s" % error)
        return 2

    measured = trace["highwater"]
    print("0036-R3-AC2 measured calls=%d successes=%d highwater=%d"
          " legacy_snapshot=%s"
          % (trace["calls"], trace["successes"], measured,
             values.get("arena_used", "?")), flush=True)
    if trace["calls"] != arena_measure.EXPECTED_CALLS:
        FAILURES.append("N=4095 observed %d allocation calls, expected %d"
                        % (trace["calls"], arena_measure.EXPECTED_CALLS))
    if trace["successes"] != arena_measure.EXPECTED_CALLS:
        FAILURES.append("N=4095 observed %d successful allocations, expected %d"
                        % (trace["successes"], arena_measure.EXPECTED_CALLS))
    if trace["failures"] != 0:
        FAILURES.append("N=4095 observed %d failed allocations"
                        % trace["failures"])

    data = args.guest.read_bytes()
    try:
        spans = load_segments(data)
    except (ValueError, struct.error) as error:
        print(":FAIL: cannot decode the terminal ELF: %s" % error)
        return 1
    if not spans:
        print(":FAIL: the terminal ELF has no PT_LOAD segment")
        return 1
    load_bytes = sum(end - start for start, end in spans)
    arena_start = (max(end for _, end in spans) + 15) & ~15
    supplementary = expected_arena(4095, arena_start)
    print("0036-R3-AC2 load_bytes=%d arena4095=%d formula=%d oracle=%d"
          " limit=%d"
          % (load_bytes, measured, supplementary, ORACLE_4095, LIMIT),
          flush=True)
    if supplementary != ORACLE_4095:
        FAILURES.append("the supplementary formula is %d, expected the"
                        " approved %d" % (supplementary, ORACLE_4095))
    if measured != supplementary:
        FAILURES.append("the measured arena %d differs from the documented"
                        " allocation order %d" % (measured, supplementary))
    FAILURES.extend(boundary_failures(data, measured))
    for failure in FAILURES:
        print(":FAIL: " + failure, flush=True)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (runtime.HarnessError, OSError, RuntimeError) as error:
        print(":HARNESS-ERROR: %s" % error)
        sys.exit(2)
