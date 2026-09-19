#!/usr/bin/env bash
# Runs the official RISC-V ACT4 test corpus against YanOS.
#
# Three parts take part in every case:
#   1. test generation      - the test image is an official ACT4 source from
#                             riscv-arch-test, assembled here into an ELF
#   2. reference signature  - Sail (`sail_riscv_sim --test-signature`) writes the
#                             expected signature of that image
#   3. DUT executor         - `yan_run --signature` writes the same region
# and the comparison is a byte-for-byte `cmp`, not a pass/fail word.
#
# Scope and known boundaries:
#   * Only `tests/rv32i/I` and `tests/rv32i/M` are selected. YanOS has no
#     interrupt controller, no timer, no S/U mode and no PMP, so rv32i/Zicsr,
#     rv32i/priv and everything above go unrun.
#   * `UNROLLSZ=0` is required: the framework pads the entry with 2-byte
#     compressed instructions, which a hart without the C extension cannot
#     execute.
#   * Tests that need a trap handler (misaligned branch and jump targets) are
#     reported as SKIP: the framework's M-mode boot path needs CSR state
#     (mie/mip/medeleg/mideleg) that YanOS does not implement.
#   * The tests' own pass/fail word is not used as the verdict here. The ACT4
#     framework normally runs the reference model during the build and compiles
#     the expected results into the image; this script compares signatures
#     instead, so a case counts as passed only when both signatures exist and
#     are identical.
#
# Exit codes: 0 every comparable case matched, 1 at least one signature
# differed, 2 usage, 77 a dependency is missing.
set -u

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
config_dir="$(cd "$script_dir/../act4" && pwd)"
gcc=""
run=""
sail=""
suite=""
work=""
inst_limit=500000

while [ $# -gt 0 ]; do
    case "$1" in
        --gcc) gcc="$2"; shift 2 ;;
        --run) run="$2"; shift 2 ;;
        --sail) sail="$2"; shift 2 ;;
        --suite) suite="$2"; shift 2 ;;
        --work) work="$2"; shift 2 ;;
        --inst-limit) inst_limit="$2"; shift 2 ;;
        *) echo "run_act4.sh: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

if [ -z "$gcc" ] || [ -z "$run" ] || [ -z "$sail" ] || [ -z "$suite" ] || [ -z "$work" ]; then
    echo "usage: run_act4.sh --gcc GCC --run YAN_RUN --sail SAIL --suite RISCV_ARCH_TEST --work DIR" >&2
    exit 2
fi
for required in "$gcc" "$run" "$sail"; do
    [ -x "$required" ] || { echo "SKIP: missing $required"; exit 77; }
done
[ -d "$suite/tests/rv32i/I" ] || { echo "SKIP: $suite is not an ACT4 checkout"; exit 77; }
command -v python3 > /dev/null 2>&1 || { echo "SKIP: python3 is required"; exit 77; }

# The symbol reader belongs to the same toolchain as the compiler.
nm_tool="${gcc%gcc}nm"
[ -x "$nm_tool" ] || { echo "SKIP: no symbol reader at $nm_tool"; exit 77; }

gcc_dir="$(cd "$(dirname "$gcc")" && pwd)"
PATH="$gcc_dir:$PATH"
export PATH
mkdir -p "$work"

matched=0
differed=0
skipped=0
declare -a skip_reasons=()

# Report which guard is actually in effect. The DUT header defines the
# interrupt macros to `.error`, but the framework includes sail_macros.h after
# it and replaces them; that is worth stating out loud rather than claiming a
# protection that does not exist.
probe="$work/guard-probe.S"
printf '#include "riscv_arch_test.h"\nRVMODEL_SET_MEXT_INT(t0, t1)\n' > "$probe"
if "$gcc" -E -march=rv32im -mabi=ilp32 -DTEST_FLEN=32 -DUNROLLSZ=0 \
        -DSAIL_CLINT_BASE_ADDRESS=0x02000000 \
        -DSAIL_SIMPLE_INTERRUPT_GENERATOR_BASE_ADDRESS=0x0 \
        -I "$suite/tests/env" -I "$config_dir" "$probe" 2> /dev/null |
   grep -q 'YanOS has no external interrupt controller'; then
    echo "note: the DUT interrupt guard is in effect"
else
    echo "note: the framework overrides the DUT interrupt guard (sail_macros.h); the per-test source check is what holds"
fi
rm -f "$probe"

to_hex_words() {
    python3 - "$1" "$2" <<'PYTHON'
import struct
import sys

raw = open(sys.argv[1], "rb").read()
with open(sys.argv[2], "w") as out:
    for at in range(0, len(raw), 4):
        out.write("%08x\n" % struct.unpack_from("<I", raw, at)[0])
PYTHON
}

for source in "$suite"/tests/rv32i/I/*.S "$suite"/tests/rv32i/M/*.S; do
    name="$(basename "$source" .S)"
    elf="$work/$name.elf"

    # The framework includes sail_macros.h after the DUT header and #undefs the
    # RVMODEL interrupt macros there, so a DUT header cannot guard them with
    # .error: the framework always wins (the probe below reports that). The
    # effective guard is here: a test whose source invokes an interrupt or timer
    # macro is refused before it runs.
    if grep -qE 'RVMODEL_(SET|CLR)_(MEXT|MSW)_INT|RVMODEL_MSIP_ADDRESS|RVMODEL_MTIME_ADDRESS' "$source"; then
        skipped=$((skipped + 1))
        skip_reasons+=("$name: invokes an interrupt or timer macro, which YanOS does not implement")
        continue
    fi

    if ! "$gcc" -march=rv32im -mabi=ilp32 -mcmodel=medany -static -nostdlib \
            -nostartfiles -DTEST_FILE="\"$name.S\"" -DTEST_FLEN=32 -DUNROLLSZ=0 \
            -DSAIL_CLINT_BASE_ADDRESS=0x02000000 \
            -DSAIL_SIMPLE_INTERRUPT_GENERATOR_BASE_ADDRESS=0x0 \
            -I "$suite/tests/env" -I "$config_dir" -T "$config_dir/link.ld" \
            "$source" -Wl,--build-id=none -o "$elf" > "$work/$name.build.log" 2>&1; then
        skipped=$((skipped + 1))
        skip_reasons+=("$name: build failed ($(grep -m1 -i 'error' "$work/$name.build.log" | cut -c1-90))")
        continue
    fi

    begin="$("$nm_tool" "$elf" | awk '$3 == "begin_signature" { print "0x" $1 }')"
    end="$("$nm_tool" "$elf" | awk '$3 == "end_signature" { print "0x" $1 }')"
    if [ -z "$begin" ] || [ -z "$end" ]; then
        skipped=$((skipped + 1))
        skip_reasons+=("$name: image defines no signature region")
        continue
    fi

    "$sail" --rv32 --inst-limit "$inst_limit" --test-signature "$work/$name.ref" \
        "$elf" > "$work/$name.sail.log" 2>&1
    "$run" --image "$elf" --max-steps "$inst_limit" --ignore-tohost \
        --signature "$work/$name.raw" "$begin" "$end" > "$work/$name.run.log" 2>&1

    if [ ! -s "$work/$name.ref" ] && [ ! -s "$work/$name.raw" ]; then
        # Both models refuse the image the same way: it needs a trap handler.
        skipped=$((skipped + 1))
        skip_reasons+=("$name: needs a trap handler ($(tail -n 1 "$work/$name.sail.log" | cut -c1-70))")
        continue
    fi
    if [ ! -s "$work/$name.ref" ] || [ ! -s "$work/$name.raw" ]; then
        echo "FAIL $name (only one model produced a signature)"
        differed=$((differed + 1))
        continue
    fi

    if ! to_hex_words "$work/$name.raw" "$work/$name.hex"; then
        echo "FAIL $name (cannot convert the DUT signature)"
        differed=$((differed + 1))
        continue
    fi

    if cmp -s "$work/$name.ref" "$work/$name.hex"; then
        echo "PASS $name ($(wc -c < "$work/$name.raw") byte signature identical to Sail)"
        matched=$((matched + 1))
    else
        echo "FAIL $name (signature differs from Sail)"
        diff "$work/$name.ref" "$work/$name.hex" | head -4 | sed 's/^/    /'
        differed=$((differed + 1))
    fi
done

echo "ACT4: $matched matched, $differed differed, $skipped skipped"
if [ "$skipped" -gt 0 ]; then
    echo "skipped cases:"
    printf '    %s\n' "${skip_reasons[@]}"
fi
[ "$differed" -eq 0 ] && [ "$matched" -gt 0 ]
