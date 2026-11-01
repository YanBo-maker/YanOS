#!/usr/bin/env python3
"""0036-R3-AC5: the loader accepts both arena symbols or neither, and rejects
a program that exports exactly one of them.

Why this test exists: tools/yan_run.c has had the "exports only one of" branch
for a while with no test behind it. A production terminal exports both symbols;
older test linkers export neither and are deliberately left alone. The single-
symbol case is the one that used to slip through, because a typo in a linker
script is far easier to make than a missing symbol.

Why the link script is tests/guest/link.ld and not os/guest.ld: os/guest.ld
assigns BOTH symbols itself, so anything linked against it always exports two
and the single-symbol shape cannot be produced. tests/guest/link.ld defines
neither, so the test injects exactly the symbol it wants through `.globl` plus
`.set` in the assembly.

The third case is the important half. Testing only "reject one symbol" cannot
tell a correct guard from an over-strict one that also rejects "no symbols",
which would break every older test linker. So `neither` must load and run.

Every case asserts the ELF really has the symbol count it claims, read with the
cross readelf. Without that check a mistyped `.set` would silently turn a
"single symbol" case into a "neither" case and the test would pass while
measuring nothing.
"""
import argparse
import subprocess
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_terminal_runtime as runtime  # noqa: E402

FAILURES = []

ARENA_DIAGNOSTIC = b"exports only one of"

# A minimal Guest that halts through tohost. Deliberately not the production
# terminal: this test is about the loader's symbol check, and a small program
# keeps the ELF's exported symbol set to exactly what the case injects.
GUEST_BODY = (
    '.section .text.start,"ax"\n'
    '.globl _start\n'
    "_start:\n"
    "  la t0, tohost\n"
    "  li t1, 1\n"
    "  sw t1, 0(t0)\n"
    "1: j 1b\n"
    '.section .tohost,"aw"\n'
    '.globl tohost\n'
    "tohost: .word 0\n"
)

# 0x83f00000 sits below the ring base and below the 32 MiB arena end that
# os/guest.ld would use, so a single-symbol ELF is rejected for the symbol
# count and not because its one symbol happens to be out of range.
SINGLE_VALUE = "0x83f00000"


def check(condition, message):
    if not condition:
        FAILURES.append(message)
        print(":FAIL: " + message, flush=True)
    return condition


def run(argv, stdin=None):
    return subprocess.run([str(item) for item in argv], input=stdin,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def arena_symbol_count(readelf, elf):
    """Count __yan_arena_start/__yan_arena_end entries in .symtab."""
    listing = run([readelf, "-sW", elf])
    if listing.returncode != 0:
        return -1
    total = 0
    for line in listing.stdout.decode(errors="replace").splitlines():
        if "__yan_arena_start" in line or "__yan_arena_end" in line:
            total += 1
    return total


def build_elf(gcc, source_dir, work, name, asm_text):
    asm = work / (name + ".S")
    asm.write_text(asm_text)
    elf = work / (name + ".elf")
    built = run([gcc, *runtime.CROSS_FLAGS, "-Wl,--build-id=none",
                 "-T", source_dir / "tests" / "guest" / "link.ld",
                 "-o", elf, asm])
    if built.returncode != 0:
        print(":HARNESS-ERROR: %s build failed:\n%s"
              % (name, built.stderr.decode(errors="replace")), flush=True)
        return None
    return elf


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--work", required=True, type=Path)
    parser.add_argument("--gcc", required=True)
    parser.add_argument("--readelf", default=None)
    parser.add_argument("--run", required=True, type=Path)
    args = parser.parse_args()

    if not runtime.tool_available(args.gcc):
        print("SKIP cross compiler")
        return 77
    readelf = args.readelf
    if readelf is None:
        candidate = str(args.gcc).replace("gcc", "readelf")
        readelf = candidate
    if not runtime.tool_available(readelf):
        print("SKIP cross readelf")
        return 77

    args.work.mkdir(parents=True, exist_ok=True)

    # Reject the premise early: if os/guest.ld were used the "neither" shape
    # would be impossible, and this whole test would be vacuous.
    linker = args.source / "tests" / "guest" / "link.ld"
    link_text = linker.read_text()
    if "__yan_arena" in link_text:
        print(":FAIL: tests/guest/link.ld defines an arena symbol; the "
              "single-symbol shapes cannot be produced with it")
        return 1

    cases = [
        # name, injected symbol, expected symbol count, must reject
        ("only-start", "__yan_arena_start", 1, True),
        ("only-end", "__yan_arena_end", 1, True),
        ("neither", None, 0, False),
    ]

    rejected = 0
    accepted = 0
    for name, symbol, want_symbols, must_reject in cases:
        asm_text = GUEST_BODY
        if symbol is not None:
            asm_text += (".globl %s\n.set %s, %s\n"
                         % (symbol, symbol, SINGLE_VALUE))
        elf = build_elf(args.gcc, args.source, args.work, name, asm_text)
        if elf is None:
            return 2

        # The count check is what makes the case meaningful: it proves the
        # ELF really exports the number of symbols the case is named for.
        got_symbols = arena_symbol_count(readelf, elf)
        if not check(got_symbols == want_symbols,
                     "%s: expected %d arena symbol(s), found %d"
                     % (name, want_symbols, got_symbols)):
            continue

        result = run([args.run, "--image", elf, "--disk", "1",
                      "--max-steps", "100000"])
        has_diagnostic = ARENA_DIAGNOSTIC in result.stderr

        if must_reject:
            check(result.returncode != 0,
                  "%s: exited 0; a single arena symbol must not load" % name)
            check(has_diagnostic,
                  "%s: rejected without the 'exports only one of' diagnostic, "
                  "stderr=%r" % (name, result.stderr[:120]))
            check(result.stdout == b"",
                  "%s: the Guest ran even though the loader rejected it" % name)
            rejected += 1
        else:
            # The reverse direction: an over-strict guard that rejected the
            # no-symbol shape would break every older test linker.
            check(not has_diagnostic,
                  "%s: reported the single-symbol diagnostic although no arena "
                  "symbol is exported; stderr=%r"
                  % (name, result.stderr[:120]))
            check(result.returncode == 0,
                  "%s: exited %d; a linker that exports neither arena symbol "
                  "must still load (older test linkers do exactly this)"
                  % (name, result.returncode))
            accepted += 1

        print("0036-R3-AC5 %s symbols=%d rc=%d diagnostic=%s"
              % (name, got_symbols, result.returncode,
                 "yes" if has_diagnostic else "no"), flush=True)

    if FAILURES:
        print(":FAIL: %d assertion(s) failed" % len(FAILURES), flush=True)
        return 1
    print("0036-R3-AC5 rejected=%d accepted=%d" % (rejected, accepted),
          flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
