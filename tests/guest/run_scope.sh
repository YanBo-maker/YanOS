#!/usr/bin/env bash
# Regression check for the boundary between a mismatch and an out-of-scope run.
#
# tests/guest/trap_scope.S performs a misaligned word load: YanCPU traps on it
# while the NEMU riscv32 reference, which has no alignment check, does not. The
# tester must classify that as exit 3 (a category the reference cannot mirror)
# and must print its own dedicated message rather than a MISMATCH line. Before
# this check existed the comparison ran first and the trap branch was
# unreachable, so a run that was out of scope was reported as an architectural
# mismatch.
#
# Exit codes: 0 the classification is correct, 1 it is not (or a source this
# check builds is missing), 2 usage, 77 this machine has no cross toolchain or
# no reference model - the two dependencies that live outside the repository.
set -u

source=""
gcc=""
dut=""
ref=""
work=""

while [ $# -gt 0 ]; do
    case "$1" in
        --source) source="$2"; shift 2 ;;
        --gcc) gcc="$2"; shift 2 ;;
        --dut) dut="$2"; shift 2 ;;
        --ref) ref="$2"; shift 2 ;;
        --work) work="$2"; shift 2 ;;
        *) echo "run_scope.sh: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

if [ -z "$source" ] || [ -z "$gcc" ] || [ -z "$dut" ] || [ -z "$ref" ] || [ -z "$work" ]; then
    echo "usage: run_scope.sh --source DIR --gcc RISCV_GCC --dut YAN_DIFFTEST --ref REF_SO --work DIR" >&2
    exit 2
fi
# 77 is only for a dependency this machine does not have: the cross toolchain
# and the NEMU reference shared object both live outside the repository, so a
# missing one stays a skip. The tester is built from this tree and the trap Guest
# is the thing under test, so a missing one is a hard failure - CTest records 77
# as a skip, and a skip is not a pass.
if [ ! -x "$gcc" ] && ! command -v "$gcc" > /dev/null 2>&1; then
    echo "SKIP: no cross toolchain at '$gcc'"
    exit 77
fi
if [ ! -e "$ref" ]; then
    echo "SKIP: no reference model at '$ref'"
    exit 77
fi
missing=0
# An executor that was named but is not there is a build that did not happen,
# not a missing dependency.
if [ ! -x "$dut" ]; then
    echo "FAIL the tester under test is not executable: $dut"
    missing=1
fi
for required in "$source/tests/guest/trap_scope.S" "$source/tests/guest/link.ld"; do
    if [ ! -e "$required" ]; then
        echo "FAIL the source under test is missing: $required"
        missing=1
    fi
done
[ "$missing" -eq 0 ] || exit 1

gcc_dir="$(cd "$(dirname "$gcc")" && pwd)"
PATH="$gcc_dir:$PATH"
export PATH
mkdir -p "$work"

elf="$work/trap_scope.elf"
if ! "$gcc" -march=rv32im -mabi=ilp32 -mcmodel=medany -static -nostdlib \
        -nostartfiles -ffreestanding -fno-builtin -O0 \
        -T "$source/tests/guest/link.ld" "$source/tests/guest/trap_scope.S" \
        -Wl,--build-id=none -o "$elf" > "$work/build.log" 2>&1; then
    echo "FAIL the trap Guest does not build: $(tail -n 1 "$work/build.log")"
    exit 1
fi

output="$("$dut" --image "$elf" --ref-so "$ref" --max-steps 100000 2>&1)"
status=$?
printf '%s\n' "$output" | sed 's/^/    /'

if [ "$status" != "3" ]; then
    echo "FAIL expected exit 3 (out of scope), got $status"
    exit 1
fi
if ! printf '%s' "$output" | grep -q 'mcause = 4'; then
    echo "FAIL the report does not name the trap cause"
    exit 1
fi
if printf '%s' "$output" | grep -q 'MISMATCH'; then
    echo "FAIL an out-of-scope run was reported as an architectural mismatch"
    exit 1
fi
if ! printf '%s' "$output" | grep -q 'pc = 8000'; then
    echo "FAIL the report does not name the faulting instruction address"
    exit 1
fi
echo "PASS a trapped run is reported as out of scope (exit 3, mcause = 4)"
exit 0
