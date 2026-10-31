#!/usr/bin/env python3
"""Issue #106: the Guest ELF must be byte-reproducible across builds.

Two independent builds of the same source must produce the same bytes. The
build already links with `-Wl,--build-id=none` and then runs
`strip --strip-debug`; together those remove the two known sources of
nondeterminism (the build id and the linker's per-object FILE symbols, which
are named after random temporary files).

Why `--strip-debug` and not `-s`: `-s` removes .symtab entirely, and with it the
`tohost` symbol. yan_run finds the halt word through yan_image_find_symbol(),
which reads .symtab, so a fully stripped ELF never halts -- measured as
"stopped after 5000000 steps without reaching tohost". This test therefore also
asserts that `tohost` is still present, so a future switch to `-s` fails here
instead of silently breaking every Guest test.

The build is driven through the same CMake target the rest of the suite uses;
`--build` is the build directory and `--cmake` the cmake executable, so the test
works from any configuration.
"""

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def build_elf(cmake, builddir, target):
    done = subprocess.run(
        [cmake, "--build", str(builddir), "--target", target],
        capture_output=True, text=True)
    if done.returncode != 0:
        raise AssertionError(
            "build failed (rc=%d)\n%s\n%s"
            % (done.returncode, done.stdout[-2000:], done.stderr[-2000:]))


def main():
    parser = argparse.ArgumentParser(
        description="Issue #106 Guest ELF reproducibility")
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--elf", required=True, type=Path)
    parser.add_argument("--readelf", required=True)
    args = parser.parse_args()

    if not args.elf.is_file():
        print("FAIL: ELF missing: %s" % args.elf, file=sys.stderr)
        return 1

    # Two full rebuilds, each from scratch, so this measures build
    # reproducibility rather than strip idempotence.
    first = sha256(args.elf)
    args.elf.unlink()
    build_elf(args.cmake, args.build, "yanfs_terminal")
    second = sha256(args.elf)
    args.elf.unlink()
    build_elf(args.cmake, args.build, "yanfs_terminal")
    third = sha256(args.elf)

    print("build 1: %s" % first)
    print("build 2: %s" % second)
    print("build 3: %s" % third)

    if not (first == second == third):
        print("FAIL: the ELF is not reproducible across builds", file=sys.stderr)
        return 1

    # A fully stripped ELF would still be reproducible but unusable: assert the
    # halt symbol is still there so `-s` cannot be substituted silently.
    syms = subprocess.run([args.readelf, "-s", str(args.elf)],
                          capture_output=True, text=True)
    if syms.returncode != 0:
        print("FAIL: readelf failed", file=sys.stderr)
        return 1
    if " tohost" not in syms.stdout:
        print("FAIL: `tohost` is missing from .symtab; yan_run cannot find the "
              "halt word (strip -s breaks the Guest)", file=sys.stderr)
        return 1
    print("tohost symbol present: yes")

    print("PASS guest ELF reproducible: 3 builds byte-identical")
    return 0


if __name__ == "__main__":
    sys.exit(main())
