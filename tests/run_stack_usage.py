#!/usr/bin/env python3
"""0036-R5-AC2/R8-AC4: directory algorithm frames do not grow with N.

`allocate_extent` no longer keeps `used_start`/`used_count` arrays; N is a
runtime value (`fs->entry_capacity`), not a compile-time constant. One
`-fstack-usage` build therefore proves independence: the .su record is a fixed
byte count marked `static`, so no code path can grow the frame with the image.
The current allocator sorts extents in its caller-owned workspace. Historical
`.su` files may also contain `extent_candidate_fits`; that optional record must
be static and small too.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

FAILURES = []
LIMIT = 128
SU_LINE = re.compile(r"^(?P<path>.+):(?P<line>\d+):(?P<column>\d+):"
                     r"(?P<function>[A-Za-z_][A-Za-z0-9_.]*)\t"
                     r"(?P<bytes>\d+)\t(?P<kind>[^\t]+)$")


def check(condition, message):
    if not condition:
        FAILURES.append(message)
        print(":FAIL: " + message, flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--work", required=True, type=Path)
    parser.add_argument("--gcc", required=True)
    args = parser.parse_args()
    for relative in ("os/yanfs.c", "os/yanfs.h", "os/memory.h", "os/sort.h"):
        if not (args.source / relative).is_file():
            print(":FAIL: missing required local input: " + str(args.source / relative))
            return 1
    if not Path(args.gcc).exists() and not shutil_which(args.gcc):
        print("SKIP cross compiler")
        return 77
    args.work.mkdir(parents=True, exist_ok=True)
    obj = args.work / "yanfs_stack.o"
    command = [args.gcc, "-std=c17", "-march=rv32im", "-mabi=ilp32",
               "-mcmodel=medany", "-ffreestanding", "-fno-builtin",
               "-fno-stack-protector", "-O2", "-Wall", "-Wextra",
               "-Wpedantic", "-Werror", "-fstack-usage", "-fno-inline",
               "-I", str(args.source / "os"), "-c",
               str(args.source / "os" / "yanfs.c"), "-o", str(obj)]
    proc = subprocess.run(command, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE)
    if proc.returncode != 0:
        print(":HARNESS-ERROR: stack-usage build failed:\n%s"
              % proc.stderr.decode(errors="replace"))
        return 2
    su = obj.with_suffix(".su")
    if not su.is_file():
        candidates = list(args.work.glob("*.su"))
        if not candidates:
            print(":FAIL: -fstack-usage produced no .su file")
            return 1
        su = candidates[0]
    records = {}
    for line in su.read_text().splitlines():
        match = SU_LINE.match(line)
        if match:
            # GCC appends .constprop/.isra suffixes at -O2; the base name is
            # the function the spec names.
            name = match.group("function").split(".", 1)[0]
            record = (int(match.group("bytes")), match.group("kind").strip())
            previous = records.get(name)
            if previous is None or record[0] > previous[0]:
                records[name] = record
    if "allocate_extent" not in records:
        print(":FAIL: .su has no allocate_extent record")
        return 1
    alloc_bytes, alloc_kind = records["allocate_extent"]
    print("0036-R5-AC2 allocate_extent=%d kind=%s" % (alloc_bytes, alloc_kind),
          flush=True)
    check(alloc_kind.startswith("static"),
          "allocate_extent frame is %r, not static" % alloc_kind)
    check(alloc_bytes <= LIMIT,
          "allocate_extent frame %d exceeds %d bytes" % (alloc_bytes, LIMIT))
    validation = records.get("validate_directory")
    check(validation is not None, ".su has no validate_directory record")
    if validation is not None:
        frame, kind = validation
        print("0036-R8-AC4 validate_directory=%d kind=%s; N is runtime (63..4095)"
              % (frame, kind), flush=True)
        check(kind == "static", "validate_directory frame is not static")
        check(frame <= LIMIT, "validate_directory frame exceeds %d bytes" % LIMIT)
    if "extent_candidate_fits" in records:
        fit_bytes, fit_kind = records["extent_candidate_fits"]
        print("0036-R5-AC2 extent_candidate_fits=%d kind=%s"
              % (fit_bytes, fit_kind), flush=True)
        check(fit_kind.startswith("static"),
              "extent_candidate_fits frame is %r, not static" % fit_kind)
        check(fit_bytes <= LIMIT,
              "extent_candidate_fits frame %d exceeds %d bytes"
              % (fit_bytes, LIMIT))
    else:
        print("0036-R5-AC2 extent_candidate_fits=not emitted", flush=True)
    for failure in FAILURES:
        print(":FAIL: " + failure, flush=True)
    return 1 if FAILURES else 0


def shutil_which(name):
    import shutil
    return shutil.which(name) is not None


if __name__ == "__main__":
    sys.exit(main())
