#!/usr/bin/env bash
# Official signature flow: a reference model computes the expected signature of
# a Guest image, the DUT executes the same image, and the two signatures must
# match byte for byte.
#
# Reference model: Sail (`sail_riscv_sim --test-signature`), an independent
# third-party RISC-V implementation. It writes the region delimited by the
# `begin_signature` and `end_signature` symbols of the ELF as one 32-bit word
# per line. DUT: `yan_run --signature`, which exports the same region raw.
#
# The three parts the flow needs are all present and all take part:
#   1. test generation  - the Guest image is built here (hand written or random)
#   2. reference signature - Sail produces <case>.ref
#   3. DUT executor     - yan_run produces <case>.dut, converted to the same text
# and the comparison is a byte-for-byte `cmp`, not a pass/fail word.
#
# Exit codes: 0 all cases matched, 1 at least one differed, 77 a dependency is
# missing (CTest reports SKIP, never PASS).
set -u

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
guest_dir="$(cd "$script_dir/../guest" && pwd)"
gcc=""
run=""
sail=""
gen=""
work=""
seeds=4
inst_limit=2000000

while [ $# -gt 0 ]; do
    case "$1" in
        --gcc) gcc="$2"; shift 2 ;;
        --run) run="$2"; shift 2 ;;
        --sail) sail="$2"; shift 2 ;;
        --gen) gen="$2"; shift 2 ;;
        --work) work="$2"; shift 2 ;;
        --seeds) seeds="$2"; shift 2 ;;
        --inst-limit) inst_limit="$2"; shift 2 ;;
        *) echo "run_signature.sh: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

if [ -z "$gcc" ] || [ -z "$run" ] || [ -z "$sail" ] || [ -z "$gen" ] || [ -z "$work" ]; then
    echo "usage: run_signature.sh --gcc GCC --run YAN_RUN --sail SAIL --gen YAN_GEN --work DIR [--seeds N]" >&2
    exit 2
fi
for required in "$gcc" "$run" "$sail" "$gen"; do
    [ -x "$required" ] || { echo "SKIP: missing $required"; exit 77; }
done
command -v python3 > /dev/null 2>&1 || { echo "SKIP: python3 is required"; exit 77; }

# The symbol reader belongs to the same toolchain as the compiler.
nm_tool="${gcc%gcc}nm"
[ -x "$nm_tool" ] || { echo "SKIP: no symbol reader at $nm_tool"; exit 77; }

gcc_dir="$(cd "$(dirname "$gcc")" && pwd)"
PATH="$gcc_dir:$PATH"
export PATH
mkdir -p "$work"

passed=0
failed=0

# yan_run writes the signature as raw little-endian bytes; Sail writes one
# 32-bit word per line. Convert the DUT output into Sail's text form.
to_hex_words() {
    python3 - "$1" "$2" <<'PYTHON'
import struct
import sys

raw = open(sys.argv[1], "rb").read()
if len(raw) % 4 != 0:
    sys.exit("signature size is not a multiple of four")
with open(sys.argv[2], "w") as out:
    for at in range(0, len(raw), 4):
        out.write("%08x\n" % struct.unpack_from("<I", raw, at)[0])
PYTHON
}

check_case() {
    local name="$1"
    local flavour="$2"
    shift 2
    local elf="$work/$name.elf"
    local sources=("$@")

    if [ "$flavour" = "guest" ]; then
        sources=("$guest_dir/start.S" "$guest_dir/guest_lib.c" "$@")
    fi

    if ! "$gcc" -march=rv32im -mabi=ilp32 -mcmodel=medany -static -nostdlib \
            -nostartfiles -ffreestanding -fno-builtin -fno-stack-protector \
            -O0 -T "$guest_dir/link.ld" "${sources[@]}" \
            -Wl,--build-id=none -o "$elf" > "$work/$name.build.log" 2>&1; then
        echo "FAIL $name (build)"; sed 's/^/    /' "$work/$name.build.log" | head -5
        failed=$((failed + 1)); return
    fi

    local begin end
    begin="$("$nm_tool" "$elf" | awk '$3 == "begin_signature" { print "0x" $1 }')"
    end="$("$nm_tool" "$elf" | awk '$3 == "end_signature" { print "0x" $1 }')"
    if [ -z "$begin" ] || [ -z "$end" ]; then
        echo "FAIL $name (the image defines no signature region)"
        failed=$((failed + 1)); return
    fi

    if ! "$sail" --rv32 --inst-limit "$inst_limit" \
            --test-signature "$work/$name.ref" "$elf" > "$work/$name.sail.log" 2>&1 ||
       [ ! -s "$work/$name.ref" ]; then
        echo "FAIL $name (sail produced no signature)"
        sed 's/^/    /' "$work/$name.sail.log" | tail -3
        failed=$((failed + 1)); return
    fi

    "$run" --image "$elf" --max-steps "$inst_limit" \
        --signature "$work/$name.dut.raw" "$begin" "$end" > "$work/$name.run.log" 2>&1
    local status=$?
    if [ ! -s "$work/$name.dut.raw" ]; then
        echo "FAIL $name (yan_run produced no signature, exit $status)"
        failed=$((failed + 1)); return
    fi
    if ! to_hex_words "$work/$name.dut.raw" "$work/$name.dut" 2> "$work/$name.convert.log"; then
        echo "FAIL $name (cannot convert the DUT signature)"
        sed 's/^/    /' "$work/$name.convert.log"
        failed=$((failed + 1)); return
    fi

    if cmp -s "$work/$name.ref" "$work/$name.dut"; then
        echo "PASS $name ($(wc -c < "$work/$name.dut.raw") byte signature identical to Sail)"
        passed=$((passed + 1))
    else
        echo "FAIL $name (signature differs from the Sail reference)"
        diff "$work/$name.ref" "$work/$name.dut" | head -6 | sed 's/^/    /'
        failed=$((failed + 1))
    fi
}

for source in alu memory control muldiv recursion; do
    check_case "$source" guest "$guest_dir/$source.c"
done

seed=1
while [ "$seed" -le "$seeds" ]; do
    generated="$work/generated$seed.S"
    if "$gen" --seed "$seed" --ops 96 --output "$generated"; then
        check_case "generated$seed" generated "$generated"
    else
        echo "FAIL generated$seed (generator failed)"
        failed=$((failed + 1))
    fi
    seed=$((seed + 1))
done

echo "signature: $((passed + failed)) cases, $passed passed, $failed failed"
[ "$failed" -eq 0 ] && [ "$passed" -gt 0 ]
