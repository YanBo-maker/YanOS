#!/usr/bin/env python3
"""Issue #106: the Guest ELF must be byte-reproducible across builds.

Two independent builds of the same source must produce the same bytes. The build
links with `-Wl,--build-id=none` and then runs `strip --strip-debug`; together
those remove the two known sources of nondeterminism (the build id and the
linker's per-object FILE symbols, which are named after random temp files).

Why `--strip-debug` and not `-s`: `-s` removes .symtab entirely, and with it the
`tohost` symbol. yan_run finds the halt word through yan_image_find_symbol(),
which reads .symtab, so a fully stripped ELF never halts -- measured as
"stopped after 5000000 steps without reaching tohost". This test therefore also
asserts that `tohost` is still present, so a future switch to `-s` fails here
instead of silently breaking every Guest test.

CONCURRENCY (why this test builds in a private directory):
An earlier revision re-linked the shared `${CMAKE_BINARY_DIR}` ELF in place,
deleting it first. Measured: the file was absent for 3.5 s, so any Guest test
running in parallel saw a missing ELF and died with ERROR OUTPUT_OR_IO (exit 5).
That made a `ctest -j4` run fail on a different set of tests each time.

The fix keeps the property under test -- two builds of the REAL terminal recipe
must be byte-identical -- while never touching the shared artefact: the same
compiler and strip commands are replayed by this script into a private temp
directory, using the recipe CMake passes in. Building some unrelated small
target instead would be cheaper but would not prove anything about
`yanfs_terminal.elf`, which is the actual subject of #106.

The shared ELF is only READ (for the initial hash and the tohost check), which
is safe for concurrent readers.
"""

import argparse
import hashlib
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def build_once(recipe, out_dir, tag):
    """Replay the terminal link+strip recipe into out_dir; return the ELF path."""
    out_dir.mkdir(parents=True, exist_ok=True)
    unstripped = out_dir / ("terminal-%s.unstripped" % tag)
    final = out_dir / ("terminal-%s.elf" % tag)

    def run(parts, label):
        done = subprocess.run([str(p) for p in parts],
                              capture_output=True, text=True)
        if done.returncode != 0:
            raise AssertionError(
                "%s failed (rc=%d)\n%s\n%s"
                % (label, done.returncode, done.stdout[-2000:],
                   done.stderr[-2000:]))

    run([unstripped if p == "__OUT__" else p for p in recipe["link"]], "link")
    run([unstripped if p == "__IN__" else
         (final if p == "__OUT__" else p) for p in recipe["strip"]], "strip")
    return final


def main():
    parser = argparse.ArgumentParser(
        description="Issue #106 Guest ELF reproducibility (concurrency-safe)")
    parser.add_argument("--elf", required=True, type=Path)
    parser.add_argument("--readelf", required=True)
    parser.add_argument("--link-command", required=True,
                        help="terminal link command; __OUT__ marks the output")
    parser.add_argument("--strip-command", required=True,
                        help="strip command; __IN__ input, __OUT__ output")
    args = parser.parse_args()

    if not args.elf.is_file():
        print("FAIL: ELF missing: %s" % args.elf, file=sys.stderr)
        return 1

    link_parts = shlex.split(args.link_command)
    strip_parts = shlex.split(args.strip_command)
    if "__OUT__" not in link_parts:
        print("FAIL: the link command must contain __OUT__", file=sys.stderr)
        return 1
    if "__IN__" not in strip_parts or "__OUT__" not in strip_parts:
        print("FAIL: the strip command must contain __IN__ and __OUT__",
              file=sys.stderr)
        return 1
    recipe = {"link": link_parts, "strip": strip_parts}

    # The shipped ELF must carry `tohost`; checked on the shared artefact,
    # read-only, so concurrent Guest tests are unaffected.
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

    with tempfile.TemporaryDirectory(prefix="guest-repro-") as tmp:
        root = Path(tmp)
        # Three independent builds in a private directory: the shared ELF is
        # never deleted or rewritten, so concurrent tests cannot observe a
        # half-written file.
        built = [build_once(recipe, root / ("b%d" % i), "b%d" % i)
                 for i in range(3)]
        hashes = [sha256(path) for path in built]

        for index, value in enumerate(hashes, start=1):
            print("build %d: %s" % (index, value))

        if len(set(hashes)) != 1:
            print("FAIL: the ELF is not reproducible across builds",
                  file=sys.stderr)
            return 1

        # The tohost check belongs on the FRESHLY BUILT artefact, not only on
        # the shipped one: if the recipe produced a fully stripped ELF, the
        # build is still reproducible but the Guest can never halt, and that
        # must fail HERE. Checking only the shipped ELF would let a future
        # `strip -s` in the recipe pass, because the shipped file is built
        # before this test runs.
        for path in built:
            syms_built = subprocess.run([args.readelf, "-s", str(path)],
                                        capture_output=True, text=True)
            if syms_built.returncode != 0:
                print("FAIL: readelf failed on %s" % path, file=sys.stderr)
                return 1
            if " tohost" not in syms_built.stdout:
                print("FAIL: the build recipe drops `tohost` from .symtab; "
                      "yan_run cannot find the halt word (strip -s breaks the "
                      "Guest)", file=sys.stderr)
                return 1
        print("tohost symbol present in the built artefact: yes")

    print("PASS guest ELF reproducible: 3 builds byte-identical")
    return 0


if __name__ == "__main__":
    sys.exit(main())
