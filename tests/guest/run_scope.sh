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
# Exit codes: 0 the classification is correct, 1 it is not, 2 usage, 77 a
# dependency is missing.
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
for required in "$gcc" "$dut" "$ref" "$source/tests/guest/trap_scope.S"; do
    [ -e "$required" ] || { echo "SKIP: missing $required"; exit 77; }
done

gcc_dir="$(cd "$(dirname "$gcc")" && pwd)"
PATH="$gcc_dir:$PATH"
export PATH
mkdir -p "$work"

elf="$work/trap_scope.elf"
if ! "$gcc" -march=rv32im -mabi=ilp32 -mcmodel=medany -static -nostdlib \
        -nostartfiles -ffreestanding -fno-builtin -O0 \
        -T "$source/tests/guest/link.ld" "$source/tests/guest/trap_scope.S" \
        -Wl,--build-id=none -o "$elf" > "$work/build.log" 2>&1; then
    echo "SKIP: cannot build the trap Guest: $(tail -n 1 "$work/build.log")"
    exit 77
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
