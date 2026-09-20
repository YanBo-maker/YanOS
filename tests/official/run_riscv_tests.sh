#!/usr/bin/env bash
# Runs the official riscv-tests ISA suites (rv32ui, rv32um) against yan_run.
#
# Every test reports PASS by storing 1 to `tohost`. yan_run resolves that
# symbol through the ELF symbol table, so no address is hard coded here: the
# suite's linker places `tohost` after .text and the address moves with the
# size of each test.
#
# Exit codes: 0 all selected tests passed, 1 at least one failed or yan_run is
# missing from the repository, 2 usage, 77 this machine lacks a dependency that
# lives outside the repository (the toolchain or the riscv-tests checkout); CTest
# reports 77 as SKIP, never as PASS.
set -u

gcc=""
run=""
suite=""
work=""
suites="rv32ui rv32um"

while [ $# -gt 0 ]; do
    case "$1" in
        --gcc) gcc="$2"; shift 2 ;;
        --run) run="$2"; shift 2 ;;
        --suite) suite="$2"; shift 2 ;;
        --work) work="$2"; shift 2 ;;
        *) echo "run_riscv_tests.sh: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

if [ -z "$gcc" ] || [ -z "$run" ] || [ -z "$suite" ] || [ -z "$work" ]; then
    echo "usage: run_riscv_tests.sh --gcc GCC --run YAN_RUN --suite DIR --work DIR" >&2
    exit 2
fi
# 77 means "a dependency this machine does not have", and nothing else: the cross
# toolchain and the riscv-tests checkout both live outside the repository, so a
# missing one stays a skip. yan_run is built from this repository: a missing one
# is a build that did not happen, so it fails instead. Deleting the executor must
# never be a way to a green suite - CTest records 77 as a skip, and a skip is not
# a pass.
if [ ! -x "$gcc" ]; then
    echo "SKIP: no RISC-V compiler at $gcc"
    exit 77
fi
[ -f "$suite/env/p/link.ld" ] || { echo "SKIP: $suite is not a riscv-tests checkout"; exit 77; }
if [ ! -x "$run" ]; then
    echo "FAIL the executor under test is missing: $run"
    exit 1
fi

gcc_dir="$(cd "$(dirname "$gcc")" && pwd)"
PATH="$gcc_dir:$PATH"
export PATH
mkdir -p "$work"

# Documented exclusions: the platform traps on misaligned data access by
# design, and `ma_data` requires such an access to succeed. It is reported as
# SKIP with the reason rather than counted as a pass.
excluded="rv32ui-p-ma_data"

is_excluded() {
    # `local`: a plain loop variable would clobber the caller's `${name}`.
    local candidate
    for candidate in $excluded; do
        [ "$candidate" = "$1" ] && return 0
    done
    return 1
}

passed=0
failed=0
skipped=0
total=0

for directory in $suites; do
    [ -d "$suite/isa/$directory" ] || continue
    for source in "$suite/isa/$directory"/*.S; do
        base="$(basename "$source" .S)"
        name="$directory-p-$base"
        total=$((total + 1))

        if is_excluded "$name"; then
            echo "SKIP $name (misaligned data access traps by design)"
            skipped=$((skipped + 1))
            continue
        fi

        elf="$work/$name.elf"
        # binutils older than 2.36 rejects the `_zicsr` subset suffix while
        # still assembling CSR instructions under plain rv32im, so the base ISA
        # is requested and Zicsr comes with it.
        if ! "$gcc" -march=rv32im -mabi=ilp32 -mcmodel=medany -static \
                -nostdlib -nostartfiles -fno-strict-aliasing \
                -I "$suite/env/p" -I "$suite/isa/macros/scalar" \
                -T "$suite/env/p/link.ld" "$source" \
                -Wl,--build-id=none -o "$elf" > "$work/$name.build.log" 2>&1; then
            echo "FAIL $name (build)"; sed 's/^/    /' "$work/$name.build.log" | head -5
            failed=$((failed + 1))
            continue
        fi

        "$run" --image "$elf" --max-steps 4000000 > "$work/$name.run.log" 2>&1
        status=$?
        case "$status" in
            0) echo "PASS $name"; passed=$((passed + 1)) ;;
            6) echo "FAIL $name (reported failure)"; sed 's/^/    /' "$work/$name.run.log"; failed=$((failed + 1)) ;;
            4) echo "FAIL $name (did not reach tohost)"; sed 's/^/    /' "$work/$name.run.log"; failed=$((failed + 1)) ;;
            *) echo "FAIL $name (exit $status)"; sed 's/^/    /' "$work/$name.run.log"; failed=$((failed + 1)) ;;
        esac
    done
done

echo "riscv-tests: $total cases, $passed passed, $failed failed, $skipped skipped"
[ "$failed" -eq 0 ] && [ "$passed" -gt 0 ]
