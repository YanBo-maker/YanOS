#!/usr/bin/env python3
"""RV32IM public-API probe for the 0025 Search facade.

Builds tests/guest/search_api_check.c together with os/search.c and the shared
Guest start/memory sources with the cross toolchain and runs the image through
the real executor. The probe uses a fake backend, so no filesystem is linked.
It must print the unique signature SEARCH_API_PASS and end with tohost 1; a
compiled-out probe cannot print the marker, so a missing check is a failure.

A missing in-repo source, header, script or the executor is a hard failure
(exit 1), checked before any external tool. Only the cross compiler and the
symbol tool live outside the repository, so their absence is exit 77. A build
error or a crash is a harness error (exit 2); a nonzero Guest tohost is a
failure (exit 1) that names the exact check code.
"""

import argparse
import struct
import subprocess
import sys
from pathlib import Path

BLOCK_TIMEOUT = 300

INPUTS = [
    "os/search.c",
    "os/search.h",
    "os/platform.h",
    "tests/guest/search_api_check.c",
    "tests/guest/start.S",
    "tests/guest/guest.h",
    "os/memory.c",
    "tests/guest/link.ld",
    "tests/guest/run_search_api.py",
]

CROSS_FLAGS = [
    "-std=c17", "-march=rv32im", "-mabi=ilp32", "-mcmodel=medany",
    "-static", "-nostdlib", "-nostartfiles", "-ffreestanding", "-fno-builtin",
    "-fno-stack-protector", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
]

MAX_STEPS = "200000000"

# tohost code -> check name, so a failure names the exact rule.
CODES = {
    1: "facade reached its final check",
    2: "init accepted a null Search or incomplete backend",
    3: "init accepted a backend without read_match",
    4: "init accepted a nonempty span with a null base",
    5: "init accepted a span range that leaves uintptr",
    6: "init accepted a Search that overlaps the backend context",
    7: "init accepted a Search that overlaps the borrowed source",
    8: "init accepted overlapping context and source spans",
    9: "init rejected a well formed fake backend",
    10: "query accepted a null pattern",
    11: "query accepted an empty pattern",
    12: "query accepted a pattern range that leaves uintptr",
    13: "query accepted a NUL pattern byte",
    14: "query accepted an LF pattern byte",
    15: "query accepted a pattern overlapping the Search",
    16: "query accepted a pattern overlapping the backend context",
    17: "query accepted a null match callback",
    18: "read_match was legal outside a match callback",
    19: "a one-read match callback did not end with STOPPED",
    20: "the bounded reader did not return its one static byte",
    21: "a holder-alias callback did not end with STOPPED",
    22: "read_match accepted holders overlapping the Search",
    23: "an empty-read callback did not end with STOPPED",
    24: "a zero-capacity or end-offset read was not a successful empty read",
    25: "a reentrant match callback did not end with STOPPED",
    26: "a reentrant query was not BUSY",
    27: "an ignored sticky reader IO error was not reported",
    28: "zero progress before the line end was not PROTOCOL",
    29: "the near-UINT32_MAX probe did not end with STOPPED",
    30: "the near-UINT32_MAX relative offset was not served as one byte",
}


def tool_available(tool):
    if "/" in tool or "\\" in tool:
        return Path(tool).is_file()
    from shutil import which
    return which(tool) is not None


def run(argv, timeout=BLOCK_TIMEOUT):
    return subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          timeout=timeout)


def classify_probe(result, signature):
    """Judge the actual tohost bytes; process rc is not the Guest check code."""
    blob = result.stdout + result.stderr
    if (result.returncode < 0 or result.returncode >= 128 or
            any(hint in blob for hint in (b"Sanitizer", b"runtime error:"))):
        return 2, "probe crashed or reported a sanitizer error"
    if result.returncode not in (0, 6) or len(signature) != 4:
        return 2, "unexpected executor exit or missing four-byte tohost signature"
    code = struct.unpack("<I", signature)[0]
    if code != 1:
        return 1, "check code %d (%s)" % (code, CODES.get(code, "unknown"))
    if result.returncode != 0:
        return 2, "executor exit disagrees with tohost 1"
    if b"SEARCH_API_PASS" not in result.stdout:
        return 1, "SEARCH_API_PASS signature is missing"
    return 0, "SEARCH_API_PASS, tohost 1"


def main():
    parser = argparse.ArgumentParser(
        description="RV32IM public-API probe for the 0025 Search facade")
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--work", required=True, type=Path)
    parser.add_argument("--gcc", required=True)
    parser.add_argument("--run", required=True)
    parser.add_argument("--nm", default="nm")
    args = parser.parse_args()

    source = args.source.resolve()
    missing = [str(source / rel) for rel in INPUTS
               if not (source / rel).is_file()]
    if missing:
        for line in missing:
            print(":FAIL: missing required input: %s" % line, flush=True)
        return 1
    if not Path(args.run).is_file():
        print(":FAIL: missing required input: --run %s" % args.run, flush=True)
        return 1
    if not tool_available(args.gcc) or not tool_available(args.nm):
        print("SKIP: no cross toolchain or symbol tool (%s, %s)"
              % (args.gcc, args.nm), flush=True)
        return 77

    args.work.mkdir(parents=True, exist_ok=True)
    elf = args.work / "search_api.elf"
    argv = [args.gcc] + CROSS_FLAGS + [
        "-I", str(source / "os"),
        "-I", str(source / "tests" / "guest"),
        "-T", str(source / "tests" / "guest" / "link.ld"),
        "-o", str(elf),
        str(source / "os" / "search.c"),
        str(source / "tests" / "guest" / "start.S"),
        str(source / "os" / "memory.c"),
        str(source / "tests" / "guest" / "search_api_check.c"),
    ]
    try:
        build = run(argv)
    except subprocess.TimeoutExpired:
        print(":HARNESS-ERROR: build timed out", flush=True)
        return 2
    except OSError as error:
        print(":HARNESS-ERROR: cannot run the compiler: %s" % error, flush=True)
        return 2
    (args.work / "search_api.build.stderr").write_bytes(build.stderr)
    if build.returncode != 0:
        print(":HARNESS-ERROR: build failed:\n%s"
              % build.stderr.decode(errors="replace"), flush=True)
        return 2

    signature = args.work / "search_api.tohost"
    if signature.exists():
        signature.unlink()
    try:
        symbols = run([args.nm, "-n", str(elf)])
        if symbols.returncode != 0:
            print(":HARNESS-ERROR: symbol lookup failed", flush=True)
            return 2
        addresses = [int(row.split()[0], 16)
                     for row in symbols.stdout.decode().splitlines()
                     if len(row.split()) == 3 and row.split()[2] == "tohost"]
        if len(addresses) != 1:
            print(":HARNESS-ERROR: expected one tohost symbol", flush=True)
            return 2
        address = addresses[0]
        result = run([str(args.run), "--image", str(elf), "--terminal",
                      "--max-steps", MAX_STEPS, "--signature", str(signature),
                      hex(address), hex(address + 4)])
    except subprocess.TimeoutExpired:
        print(":HARNESS-ERROR: the probe timed out", flush=True)
        return 2
    except OSError as error:
        print(":HARNESS-ERROR: cannot run the executor: %s" % error, flush=True)
        return 2
    (args.work / "search_api.stdout").write_bytes(result.stdout)
    (args.work / "search_api.stderr").write_bytes(result.stderr)

    code, detail = classify_probe(result,
        signature.read_bytes() if signature.is_file() else b"")
    prefix = "PASS search_api" if code == 0 else (
        ":FAIL:" if code == 1 else ":HARNESS-ERROR:")
    print("%s (%s)" % (prefix, detail), flush=True)
    return code


if __name__ == "__main__":
    sys.exit(main())
