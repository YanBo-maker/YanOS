#!/usr/bin/env bash
# Builds the YanOS Guest corpus and runs every image through the differential
# tester, which compares YanCPU with a reference model after each instruction.
#
# Exit codes: 0 all cases passed, 1 at least one case failed, 77 a dependency
# is missing, which CTest reports as SKIP rather than as a pass.
set -u

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
gcc=""
gen=""
dut=""
ref=""
work=""
seeds=8

while [ $# -gt 0 ]; do
    case "$1" in
        --gcc) gcc="$2"; shift 2 ;;
        --gen) gen="$2"; shift 2 ;;
        --dut) dut="$2"; shift 2 ;;
        --ref) ref="$2"; shift 2 ;;
        --work) work="$2"; shift 2 ;;
        --seeds) seeds="$2"; shift 2 ;;
        *) echo "run_corpus.sh: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

if [ -z "$gcc" ] || [ -z "$gen" ] || [ -z "$dut" ] || [ -z "$ref" ] || [ -z "$work" ]; then
    echo "usage: run_corpus.sh --gcc GCC --gen YAN_GEN --dut YAN_DIFFTEST --ref REF_SO --work DIR [--seeds N]" >&2
    exit 2
fi

missing=""
[ -x "$gcc" ] || missing="$missing $gcc"
[ -x "$gen" ] || missing="$missing $gen"
[ -x "$dut" ] || missing="$missing $dut"
[ -f "$ref" ] || missing="$missing $ref"
if [ -n "$missing" ]; then
    echo "SKIP: missing dependency:$missing"
    exit 77
fi

mkdir -p "$work"

# A cross toolchain locates `as` and `ld` through its own directory: a binary
# installed in a private prefix is not usable before that prefix is on PATH.
gcc_dir="$(cd "$(dirname "$gcc")" && pwd)"
PATH="$gcc_dir:$PATH"
export PATH

fails=0
cases=0

build_and_run() {
    local name="$1"
    local source="$2"
    local optimisation="$3"
    local flavour="$4"
    local elf="$work/$name.elf"
    local report="$work/$name.jsonl"
    local sources=("$source")

    # A generated program carries its own entry point, `tohost` and scratch
    # area, so it must not be linked with the hand-written ones.
    if [ "$flavour" = "guest" ]; then
        sources=("$script_dir/start.S" "$script_dir/guest_lib.c" "$source")
    fi

    # -march=rv32im keeps compressed instructions out, so every fetch is a
    # full word and both models decode the same stream.
    if ! "$gcc" -march=rv32im -mabi=ilp32 -mcmodel=medany -static -nostdlib \
            -nostartfiles -ffreestanding -fno-builtin -fno-stack-protector \
            "$optimisation" -T "$script_dir/link.ld" "${sources[@]}" \
            -Wl,--build-id=none -o "$elf" > "$work/$name.build.log" 2>&1; then
        echo "FAIL $name (build)"; sed 's/^/    /' "$work/$name.build.log"
        fails=$((fails + 1)); cases=$((cases + 1)); return
    fi

    "$dut" --image "$elf" --ref-so "$ref" --max-steps 2000000 \
        --report "$report" > "$work/$name.run.log" 2>&1
    local status=$?
    cases=$((cases + 1))
    case "$status" in
        0) printf 'PASS %s\n' "$name" ;;
        1) printf 'FAIL %s (architectural mismatch)\n' "$name"; sed 's/^/    /' "$work/$name.run.log"; fails=$((fails + 1)) ;;
        6) printf 'FAIL %s (guest self-check failed)\n' "$name"; sed 's/^/    /' "$work/$name.run.log"; fails=$((fails + 1)) ;;
        3) printf 'FAIL %s (guest trapped outside the reference scope)\n' "$name"; sed 's/^/    /' "$work/$name.run.log"; fails=$((fails + 1)) ;;
        *) printf 'FAIL %s (exit %s)\n' "$name" "$status"; sed 's/^/    /' "$work/$name.run.log"; fails=$((fails + 1)) ;;
    esac
}

for source in alu memory control muldiv recursion; do
    for level in -O0 -O2; do
        build_and_run "$source$level" "$script_dir/$source.c" "$level" guest
    done
done

seed=1
while [ "$seed" -le "$seeds" ]; do
    generated="$work/generated$seed.S"
    if "$gen" --seed "$seed" --ops 96 --output "$generated"; then
        build_and_run "generated$seed" "$generated" -O0 generated
    else
        echo "FAIL generated$seed (generator failed)"
        fails=$((fails + 1)); cases=$((cases + 1))
    fi
    seed=$((seed + 1))
done

echo "corpus: $cases cases, $((cases - fails)) passed, $fails failed"
[ "$fails" -eq 0 ]
