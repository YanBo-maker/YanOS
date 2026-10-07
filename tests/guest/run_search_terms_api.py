#!/usr/bin/env python3
"""RV32IM public-API probe for the 0026 term-search facade.

Builds tests/guest/search_terms_api_check.c together with the production term
facade, the shared core and text primitives, and the shared Guest start/memory
sources with the cross toolchain, then runs the image through the real executor.
The probe uses a fake backend and links no filesystem and no index, so nothing
large is placed on the task stack and no disk is touched. A run is a pass only
when the image both prints the unique signature SEARCH_TERMS_API_PASS and leaves
tohost 1; the marker and the tohost word are two required pieces of evidence and
neither alone is accepted.

A missing in-repo source, header, script or the executor is a hard failure
(exit 1), checked before any external tool. Only the cross compiler and the
symbol tool live outside the repository, so their absence is exit 77. A build
error, crash, timeout, sanitizer report or an unexpected executor exit is a
harness error (exit 2); a nonzero Guest tohost is a failure (exit 1) that names
the exact check code.
"""

import argparse
import struct
import subprocess
import sys
from pathlib import Path

BLOCK_TIMEOUT = 300

INPUTS = [
    "os/search_terms.c",
    "os/search_terms.h",
    "os/search_terms_core.c",
    "os/search_terms_core.h",
    "os/search_text.c",
    "os/search_text.h",
    "os/platform.h",
    "tests/guest/search_terms_api_check.c",
    "tests/guest/start.S",
    "tests/guest/guest.h",
    "os/memory.c",
    "os/memory.h",
    "tests/guest/link.ld",
    "tests/guest/run_search_terms_api.py",
]

CROSS_FLAGS = [
    "-std=c17", "-march=rv32im", "-mabi=ilp32", "-mcmodel=medany",
    "-static", "-nostdlib", "-nostartfiles", "-ffreestanding", "-fno-builtin",
    "-fno-stack-protector", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
]

MAX_STEPS = "200000000"

SIGNATURE = b"SEARCH_TERMS_API_PASS"

# tohost code -> check name, so a failure names the exact rule.
CODES = {
    1: "facade reached its final check",
    2: "init accepted a null facade",
    3: "init accepted a backend without query",
    4: "init accepted a backend without read_match",
    5: "init accepted a nonempty span with a null base",
    6: "init accepted a span range that leaves uintptr",
    7: "init accepted a facade overlapping the backend context",
    8: "init accepted a facade overlapping the borrowed source",
    9: "init accepted overlapping context and source spans",
    10: "init rejected a well formed fake backend",
    11: "query accepted a null query",
    12: "query accepted an empty query",
    13: "query accepted a length over the query maximum",
    14: "query accepted a query range that leaves uintptr",
    15: "query accepted a NUL byte",
    16: "query accepted an LF byte",
    17: "query accepted a bare CR byte",
    18: "query accepted a DEL byte",
    19: "query accepted invalid UTF-8",
    20: "query accepted 17 distinct groups",
    21: "query accepted a separator-only query",
    22: "query accepted an over-long ASCII word",
    23: "query accepted a query overlapping the facade",
    24: "query accepted a query overlapping the backend context",
    25: "query accepted a query overlapping the borrowed source",
    26: "query accepted a null match callback",
    27: "query accepted a summary overlapping the facade",
    28: "query accepted a summary overlapping the backend context",
    29: "query accepted a summary overlapping the query",
    30: "an invalid query reached the backend",
    31: "17 canonical duplicates were not accepted as one group",
    32: "the duplicate-canonical query did not reach the backend exactly once",
    33: "read_snippet was legal outside a match callback",
    34: "a valid keep query did not return OK",
    35: "the reader did not return its one static byte",
    36: "a successful query did not publish its summary",
    37: "a holder-alias callback did not end with STOPPED",
    38: "read_snippet accepted an output overlapping the facade",
    39: "a dual-alias callback did not end with STOPPED",
    40: "read_snippet accepted overlapping output holders",
    41: "an empty-read callback did not end with STOPPED",
    42: "a zero-capacity or end-offset read was not a successful empty read",
    43: "an ignored sticky reader IO error was not reported",
    44: "zero progress before the snippet end was not PROTOCOL",
    45: "a failed query did not report the reader IO error",
    46: "a failed query published a summary",
    47: "a query with an optional NULL summary did not succeed",
    48: "an optional NULL summary still delivered the match",
    49: "a reentrant query callback did not end with STOPPED",
    50: "a reentrant query was not BUSY",
    51: "the outer status call was not OK",
    52: "a reentrant status was not BUSY",
    53: "a BUSY status wrote its holder",
    54: "the outer status call for the clear probe was not OK",
    55: "a reentrant clear was not BUSY",
    56: "the outer status call for the rebuild probe was not OK",
    57: "a reentrant rebuild was not BUSY",
    58: "the outer status call for the init probe was not OK",
    59: "a reentrant init was not BUSY",
    60: "a summary holder whose range leaves uintptr was accepted",
    61: "a holder-wrap callback did not end with STOPPED",
    62: "read_snippet accepted an output holder whose range leaves uintptr",
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
    if SIGNATURE not in result.stdout:
        return 1, "SEARCH_TERMS_API_PASS signature is missing"
    return 0, "SEARCH_TERMS_API_PASS, tohost 1"


def main():
    parser = argparse.ArgumentParser(
        description="RV32IM public-API probe for the 0026 term-search facade")
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
    elf = args.work / "search_terms_api.elf"
    argv = [args.gcc] + CROSS_FLAGS + [
        "-I", str(source / "os"),
        "-I", str(source / "tests" / "guest"),
        "-T", str(source / "tests" / "guest" / "link.ld"),
        "-o", str(elf),
        str(source / "os" / "search_terms.c"),
        str(source / "os" / "search_terms_core.c"),
        str(source / "os" / "search_text.c"),
        str(source / "tests" / "guest" / "start.S"),
        str(source / "os" / "memory.c"),
        str(source / "tests" / "guest" / "search_terms_api_check.c"),
    ]
    try:
        build = run(argv)
    except subprocess.TimeoutExpired:
        print(":HARNESS-ERROR: build timed out", flush=True)
        return 2
    except OSError as error:
        print(":HARNESS-ERROR: cannot run the compiler: %s" % error, flush=True)
        return 2
    (args.work / "search_terms_api.build.stderr").write_bytes(build.stderr)
    if build.returncode != 0:
        print(":HARNESS-ERROR: build failed:\n%s"
              % build.stderr.decode(errors="replace"), flush=True)
        return 2

    signature = args.work / "search_terms_api.tohost"
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
    (args.work / "search_terms_api.stdout").write_bytes(result.stdout)
    (args.work / "search_terms_api.stderr").write_bytes(result.stderr)

    code, detail = classify_probe(
        result, signature.read_bytes() if signature.is_file() else b"")
    prefix = "PASS search_terms_api" if code == 0 else (
        ":FAIL:" if code == 1 else ":HARNESS-ERROR:")
    print("%s (%s)" % (prefix, detail), flush=True)
    return code


if __name__ == "__main__":
    sys.exit(main())
