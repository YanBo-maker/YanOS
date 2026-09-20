#!/usr/bin/env bash
# Builds and runs the Guest trap-environment self-check.
#
# This is the only layer that exercises the Guest sources themselves
# (mtrap_entry.S, mtrap.c, guest_devices.h) on the real CPU model: the Host
# tests can check the ABI constants and the device encodings, but only a
# compiled Guest proves the vector, the frame and the MRET path work together.
#
# The image uses CSR and interrupt semantics that the NEMU riscv32 reference
# cannot mirror, so it runs through `yan_run` only, never through the
# differential tester.
#
# Exit codes: 0 the Guest reached its final check, 1 it reported a failing check
# (the code is the check number), 77 a dependency is missing so CTest can SKIP.
set -u

source=""
gcc=""
run=""
work=""

while [ $# -gt 0 ]; do
    case "$1" in
        --source) source="$2"; shift 2 ;;
        --gcc) gcc="$2"; shift 2 ;;
        --run) run="$2"; shift 2 ;;
        --work) work="$2"; shift 2 ;;
        *) echo "run_trap_env.sh: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

if [ -z "$source" ] || [ -z "$gcc" ] || [ -z "$run" ] || [ -z "$work" ]; then
    echo "usage: run_trap_env.sh --source DIR --gcc RISCV_GCC --run YAN_RUN --work DIR" >&2
    exit 2
fi

guest_dir="$source/tests/guest"
for required in "$gcc" "$run" "$guest_dir/mtrap_entry.S" "$guest_dir/mtrap.c" \
                "$guest_dir/trap_env.c" "$guest_dir/guest_devices.h"; do
    [ -e "$required" ] || { echo "SKIP: missing $required"; exit 77; }
done

# A cross toolchain locates `as` and `ld` through its own directory.
gcc_dir="$(cd "$(dirname "$gcc")" && pwd)"
PATH="$gcc_dir:$PATH"
export PATH
mkdir -p "$work"

elf="$work/trap_env.elf"
if ! "$gcc" -march=rv32im -mabi=ilp32 -mcmodel=medany -static -nostdlib \
        -nostartfiles -ffreestanding -fno-builtin -fno-stack-protector \
        -Wall -Wextra -O2 -T "$guest_dir/link.ld" \
        "$guest_dir/mtrap_entry.S" "$guest_dir/mtrap.c" \
        "$guest_dir/trap_env.c" "$guest_dir/guest_lib.c" \
        -Wl,--build-id=none -o "$elf" > "$work/build.log" 2>&1; then
    echo "FAIL the Guest trap environment does not build:"
    sed 's/^/    /' "$work/build.log"
    exit 1
fi

"$run" --image "$elf" --max-steps 200000 > "$work/run.log" 2>&1
status=$?
case "$status" in
    0) echo "PASS the Guest trap environment booted, trapped and returned" ;;
    6) echo "FAIL a Guest check failed:"; sed 's/^/    /' "$work/run.log"; exit 1 ;;
    *) echo "FAIL the Guest run ended with exit $status:"; sed 's/^/    /' "$work/run.log"; exit 1 ;;
esac
exit 0
