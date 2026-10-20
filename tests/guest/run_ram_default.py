#!/usr/bin/env python3
"""0036-R3-AC4: yan_run's default RAM and the Guest arena boundary.

A tiny Guest reads the transport ring base register and `__yan_arena_end`, stores
both in a signature region and halts with tohost 1. yan_run is invoked without
`--ram`, so the values prove the default is 64 MiB: ring base 0x83FFC000 means
RAM end 0x84000000. The same test asserts that the four Guest drivers no longer
carry a 16 MiB literal; golden/difftest keep theirs and are not touched.
"""
import argparse
import re
import struct
import subprocess
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_terminal_runtime as runtime  # noqa: E402

BASE = 0x80000000
RING_SIZE = 8192
RING_BYTES = 2 * RING_SIZE
EXPECTED_RING = 0x83FFC000
EXPECTED_ARENA_END = 0x82000000
DRIVERS = [
    "tests/guest/block_drive.c",
    "tests/guest/run_runtime.sh",
    "tests/guest/run_console.sh",
    "tests/terminal_runtime_driver.c",
]
GOLDEN_KEEP = ["tests/guest/run_golden.sh", "tools/yan_difftest.c"]
FAILURES = []


def check(condition, message):
    if not condition:
        FAILURES.append(message)
        print(":FAIL: " + message, flush=True)


def symbol_address(readelf, elf, name):
    proc = subprocess.run([str(readelf), "-s", str(elf)],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if proc.returncode != 0:
        raise RuntimeError("readelf failed: %s"
                           % proc.stderr.decode(errors="replace"))
    for line in proc.stdout.decode(errors="replace").splitlines():
        parts = line.split()
        if len(parts) >= 8 and parts[-1] == name:
            return int(parts[1], 16)
    raise RuntimeError("symbol %s not found" % name)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--work", required=True, type=Path)
    parser.add_argument("--gcc", required=True)
    parser.add_argument("--readelf", required=True)
    parser.add_argument("--run", required=True, type=Path)
    args = parser.parse_args()
    if not runtime.tool_available(args.gcc):
        print("SKIP cross compiler")
        return 77
    args.work.mkdir(parents=True, exist_ok=True)

    probe = args.work / "ram_probe.c"
    probe.write_text(
        '#include <stdint.h>\n'
        '#include "platform.h"\n'
        'extern volatile uint32_t tohost;\n'
        'extern uint8_t __yan_arena_start[];\n'
        'extern uint8_t __yan_arena_end[];\n'
        'volatile uint32_t r3_layout[3];\n'
        'int main(void)\n'
        '{\n'
        '    r3_layout[0] = yan_os_transport_ring_base();\n'
        '    r3_layout[1] = (uint32_t)(uintptr_t)__yan_arena_end;\n'
        '    r3_layout[2] = (uint32_t)(uintptr_t)__yan_arena_start;\n'
        '    tohost = 1;\n'
        '    for (;;) {}\n'
        '}\n')
    elf = args.work / "ram_probe.elf"
    built = subprocess.run(
        [str(args.gcc), *runtime.CROSS_FLAGS, "-I", str(args.source / "os"),
         "-T", str(args.source / "os" / "guest.ld"), "-Wl,--build-id=none",
         "-o", str(elf), str(args.source / "os" / "trap_entry.S"),
         str(args.source / "os" / "task.c"),
         str(args.source / "os" / "task_switch.S"), str(probe)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if built.returncode != 0:
        print(":HARNESS-ERROR: probe build failed:\n%s"
              % built.stderr.decode(errors="replace"))
        return 2
    address = symbol_address(args.readelf, elf, "r3_layout")
    signature = args.work / "ram_probe.sig"
    result = subprocess.run(
        [str(args.run), "--image", str(elf), "--disk", "1",
         "--signature", str(signature), hex(address), hex(address + 12),
         "--max-steps", "1000000"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode != 0:
        print(":HARNESS-ERROR: probe run exit %d: %s"
              % (result.returncode, result.stderr.decode(errors="replace")))
        return 2
    ring_base, arena_end, arena_start = struct.unpack("<III",
                                                      signature.read_bytes())
    ram_bytes = ring_base + RING_BYTES - BASE
    print("0036-R3-AC4 ram=%d ring_base=0x%08x arena_start=0x%08x"
          " arena_end=0x%08x" % (ram_bytes, ring_base, arena_start, arena_end),
          flush=True)
    check(ring_base == EXPECTED_RING,
          "ring base 0x%08x != 0x%08x" % (ring_base, EXPECTED_RING))
    check(arena_end == EXPECTED_ARENA_END,
          "arena end 0x%08x != 0x%08x" % (arena_end, EXPECTED_ARENA_END))
    check(arena_start <= arena_end, "arena start is above its end")
    check(arena_end <= ring_base, "arena end is above the ring base")
    check(ram_bytes == 64 * 1024 * 1024,
          "default RAM is %d bytes, not 64 MiB" % ram_bytes)

    obsolete = re.compile(r"16777216|16[uU]?\s*\*\s*1024[uU]?\s*\*\s*1024[uU]?")
    for rel in DRIVERS:
        text = (args.source / rel).read_text()
        check(obsolete.search(text) is None,
              "%s still contains a 16 MiB literal" % rel)
    for rel in GOLDEN_KEEP:
        text = (args.source / rel).read_text()
        check(obsolete.search(text) is not None,
              "%s lost the frozen 16 MiB value" % rel)
    print("0036-R3-AC4 drivers=clean golden=kept", flush=True)
    for failure in FAILURES:
        print(":FAIL: " + failure, flush=True)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, struct.error) as error:
        print(":HARNESS-ERROR: %s" % error)
        sys.exit(2)
