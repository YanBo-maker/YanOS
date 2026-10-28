#!/usr/bin/env python3
"""0036-R3-AC1: the Guest arena usage follows the image's directory capacity.

The same production terminal ELF runs against every supported capacity class
(63, 1023, 2047 and 4095 files). A Host-side trace of the real
`yan_arena_alloc` entry/return reports the calls, successes and the actual
high-water cursor, including alignment padding. It does not read the Guest's
final `yan_arena_used_bytes` word, which is only assigned after the last
allocation. The oracle below recomputes the sum from the allocation order in
docs/specs/0036-yanfs-multi-block-directory.md. #4 appears exactly once (the
index_eligible copy the terminal actually uses) and #8 is absent because PR1
removed `allocate_extent`'s stack arrays (approved 实施澄清). R8 appends four
uint16 index tables (name_order and three service sort tables).

A header that fails probe (unknown version, D out of range, N != 64*D-1) must
stop before the first allocation, so the observed call count and high-water
mark stay 0. Missing or incomplete observation is a harness error, never a
zero-allocation pass.
"""
import argparse
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import arena_measure  # noqa: E402
import run_terminal_runtime as runtime  # noqa: E402

BLOCK = 4096
FAILURES = []


def check(condition, message):
    if not condition:
        FAILURES.append(message)
        print(":FAIL: " + message, flush=True)


def expected_arena(files):
    """Σ(align16 padding + item bytes) in the documented allocation order."""
    sizes = [
        (files + 1) * 64,  # directory (N+1) * 64, = D * 4096
        files * 36,        # #1 YanNoteEntry
        files * 32,        # #2 sort_name
        files * 36,        # #3 YanRelationRow
        files,             # #4 index_eligible, shared by both linear contexts
        files * 36,        # #5 YanFsInfo
        files * 32,        # #6 block names
        files * 2,         # R8 filesystem name_order
        files * 2,         # R8 metadata sort indices
        files * 2,         # R8 relation sort indices
        files * 2,         # R8 block sort indices
    ]
    current = 0
    used = 0
    for size in sizes:
        padding = (-current) % 16
        current += padding + size
        used += padding + size
    return used


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
    except runtime.HarnessError as error:
        print(":HARNESS-ERROR: %s" % error)
        return 2

    class Args:
        pass
    run_args = Args()
    run_args.source = args.source
    run_args.work = args.work
    run_args.guest = args.guest

    oracle = {63: 15518, 1023: 250718, 2047: 501598, 4095: 1003358}
    for files in (63, 1023, 2047, 4095):
        blocks = 65 if files == 4095 else 64
        image = arena_measure.make_image(
            args.mkfs, args.work / ("ac1-%d.img" % files), files, blocks)
        trace, values, _disk, _capture = arena_measure.measure(
            driver, run_args, args.work, image, "ac1-%d" % files,
            args.max_steps)
        expected = expected_arena(files)
        print("0036-R3-AC1 N=%d calls=%d successes=%d highwater=%d"
              " expected=%d oracle=%d legacy_snapshot=%s"
              % (files, trace["calls"], trace["successes"],
                 trace["highwater"], expected, oracle[files],
                 values.get("arena_used", "?")), flush=True)
        check(expected == oracle[files],
              "N=%d: computed expectation %d != approved oracle %d"
              % (files, expected, oracle[files]))
        check(trace["calls"] == arena_measure.EXPECTED_CALLS,
              "N=%d: observed %d allocation calls, expected %d"
              % (files, trace["calls"], arena_measure.EXPECTED_CALLS))
        check(trace["successes"] == arena_measure.EXPECTED_CALLS,
              "N=%d: observed %d successful allocations, expected %d"
              % (files, trace["successes"], arena_measure.EXPECTED_CALLS))
        check(trace["failures"] == 0,
              "N=%d: %d arena allocations failed unexpectedly"
              % (files, trace["failures"]))
        check(trace["highwater"] == expected,
              "N=%d: observed high-water %d != expected %d"
              % (files, trace["highwater"], expected))

    base = bytearray(arena_measure.make_image(
        args.mkfs, args.work / "ac1-base.img", 1023, 64))
    bad_cases = {}
    bad_cases["version"] = bytearray(base)
    bad_cases["version"][8] = 3
    bad_cases["d-65"] = bytearray(base)
    bad_cases["d-65"][8] = 2
    bad_cases["d-65"][24] = 65
    bad_cases["n-mismatch"] = bytearray(base)
    bad_cases["n-mismatch"][8] = 2
    bad_cases["n-mismatch"][24] = 2
    bad_cases["n-mismatch"][20] = 64
    for label, bad_image in bad_cases.items():
        path = args.work / ("ac1-bad-%s.img" % label)
        path.write_bytes(bad_image)
        trace, values, disk, output = arena_measure.measure(
            driver, run_args, args.work, bad_image, "ac1-bad-" + label,
            200000, expect_tohost=runtime.APP_FAIL_TOP | 6)
        print("0036-R3-AC1 invalid=%s tohost=0x%08x calls=%d successes=%d"
              " highwater=%d legacy_snapshot=%s"
              % (label, values["_tohost"], trace["calls"],
                 trace["successes"], trace["highwater"],
                 values.get("arena_used", "?")), flush=True)
        check(values["_tohost"] == runtime.APP_FAIL_TOP | 6,
              "%s: startup did not report the mount path" % label)
        check(trace["calls"] == 0,
              "%s: %d allocation calls happened before header validation"
              % (label, trace["calls"]))
        check(trace["successes"] == 0,
              "%s: %d allocations succeeded before header validation"
              % (label, trace["successes"]))
        check(trace["highwater"] == 0,
              "%s: the arena high-water mark is %d before header validation"
              % (label, trace["highwater"]))
        check(b"yanfs> " not in output,
              "%s: the shell prompt was reached" % label)
        check(b"ERROR " in output, "%s: no error token was printed" % label)
        check(disk.read_bytes() == bad_image,
              "%s: the rejected image changed" % label)

    for failure in FAILURES:
        print(":FAIL: " + failure, flush=True)
    print("arena-accounting: %d failures" % len(FAILURES), flush=True)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (runtime.HarnessError, OSError, RuntimeError) as error:
        print(":HARNESS-ERROR: %s" % error)
        sys.exit(2)
