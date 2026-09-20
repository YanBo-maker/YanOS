#!/usr/bin/env bash
# Mutation check for the differential tester itself.
#
# A differential tester that passes a planted bug is broken, not lucky. This
# script plants one deliberate YanCPU defect at a time in a throw-away copy of
# the tree and requires the tester to report an architectural mismatch naming
# the offending pc and instruction. Every mutation is verified to have actually
# changed the source, so a stale pattern cannot make the check vacuous.
#
# Exit codes: 0 every mutation was detected, 1 a mutation survived or a source
# this check compiles is missing or does not build, 2 usage, 77 this machine has
# no cross toolchain or no reference model.
set -u

source=""
cc="${CC:-cc}"
gcc=""
ref=""
work=""

while [ $# -gt 0 ]; do
    case "$1" in
        --source) source="$2"; shift 2 ;;
        --cc) cc="$2"; shift 2 ;;
        --gcc) gcc="$2"; shift 2 ;;
        --ref) ref="$2"; shift 2 ;;
        --work) work="$2"; shift 2 ;;
        *) echo "run_mutation.sh: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

if [ -z "$source" ] || [ -z "$gcc" ] || [ -z "$ref" ] || [ -z "$work" ]; then
    echo "usage: run_mutation.sh --source DIR --cc CC --gcc RISCV_GCC --ref REF_SO --work DIR" >&2
    exit 2
fi
# 77 is only for a dependency this machine does not have: the cross toolchain and
# the NEMU reference shared object live outside the repository, so a missing one
# stays a skip. Everything else this script compiles is the thing under test -
# the CPU whose defects are planted, and the tester and generator that report
# them - so a missing source is a hard failure. CTest records 77 as a skip, and a
# skip is not a pass.
if [ ! -x "$gcc" ] && ! command -v "$gcc" > /dev/null 2>&1; then
    echo "SKIP: no cross toolchain at '$gcc'"
    exit 77
fi
if [ ! -e "$ref" ]; then
    echo "SKIP: no reference model at '$ref'"
    exit 77
fi
missing=0
for required in "$source/src/cpu.c" "$source/tools/yan_difftest.c" \
                "$source/tools/host_file.c" "$source/tools/yan_gen.c" \
                "$source/tests/guest/start.S" "$source/tests/guest/guest_lib.c" \
                "$source/tests/guest/alu.c" "$source/tests/guest/memory.c" \
                "$source/tests/guest/control.c" "$source/tests/guest/muldiv.c" \
                "$source/tests/guest/link.ld"; do
    if [ ! -e "$required" ]; then
        echo "FAIL the source under test is missing: $required"
        missing=1
    fi
done
[ "$missing" -eq 0 ] || exit 1

gcc_dir="$(cd "$(dirname "$gcc")" && pwd)"
PATH="$gcc_dir:$PATH"
export PATH

tree="$work/tree"
rm -rf "$tree"
mkdir -p "$work" "$tree"
cp -a "$source/include" "$source/src" "$source/tools" "$source/tests" "$tree/"

build_tester() {
    "$cc" -O1 -std=c17 -I "$tree/include" -o "$tree/yan_difftest" \
        "$tree/tools/yan_difftest.c" "$tree/tools/host_file.c" \
        "$tree"/src/*.c -ldl 2> "$work/build.log"
}

apply_mutation() {
    case "$1" in
        slti-unsigned)
            sed -i 's/case 2: return (left ^ UINT32_C(0x80000000)) < (right ^ UINT32_C(0x80000000));/case 2: return left < right;/' "$tree/src/cpu.c" ;;
        jalr-bit-zero)
            sed -i 's/next_pc = (source + sign_extend(instruction >> 20, 12)) & UINT32_C(0xfffffffe);/next_pc = source + sign_extend(instruction >> 20, 12);/' "$tree/src/cpu.c" ;;
        div-by-zero)
            sed -i 's/if (right == 0) return UINT32_MAX;/if (right == 0) return 0;/' "$tree/src/cpu.c" ;;
        lb-sign-extension)
            sed -i 's/\*value = sign_extend(\*value, kind == 0 ? 8 : 16);/if (kind != 0) { *value = sign_extend(*value, 16); }/' "$tree/src/cpu.c" ;;
    esac
}

if ! build_tester; then
    echo "FAIL the tester under test does not build: $(tail -n 1 "$work/build.log")"
    exit 1
fi
"$cc" -O1 -std=c17 -I "$tree/include" -o "$tree/yan_gen" "$tree/tools/yan_gen.c" \
    > "$work/gen.log" 2>&1 ||
    { echo "FAIL the generator under test does not build: $(tail -n 1 "$work/gen.log")"; exit 1; }
for seed in 1 2 3 4; do
    "$tree/yan_gen" --seed "$seed" --ops 96 --output "$work/generated$seed.S" ||
        { echo "FAIL the generator under test failed to generate seed $seed"; exit 1; }
done

survivors=0
detected=0

for name in slti-unsigned jalr-bit-zero div-by-zero lb-sign-extension; do
    cp -a "$source/src/cpu.c" "$tree/src/cpu.c"
    before="$(md5sum < "$tree/src/cpu.c")"
    apply_mutation "$name"
    after="$(md5sum < "$tree/src/cpu.c")"
    if [ "$before" = "$after" ]; then
        echo "FAIL mutant $name was not applied (pattern mismatch)"
        survivors=$((survivors + 1))
        continue
    fi
    if ! build_tester; then
        echo "FAIL mutant $name did not build: $(tail -n 1 "$work/build.log")"
        survivors=$((survivors + 1))
        continue
    fi

    caught=0
    for case_name in alu memory control muldiv generated1 generated2 generated3 generated4; do
        if [ "${case_name#generated}" != "$case_name" ]; then
            sources=("$work/$case_name.S")
        else
            sources=("$tree/tests/guest/start.S" "$tree/tests/guest/guest_lib.c"
                     "$tree/tests/guest/$case_name.c")
        fi
        elf="$work/$case_name.elf"
        if ! "$gcc" -march=rv32im -mabi=ilp32 -mcmodel=medany -static -nostdlib \
                -nostartfiles -ffreestanding -fno-builtin -O0 \
                -T "$tree/tests/guest/link.ld" "${sources[@]}" \
                -Wl,--build-id=none -o "$elf" > "$work/elf.log" 2>&1; then
            continue
        fi
        output="$("$tree/yan_difftest" --image "$elf" --ref-so "$ref" \
                  --max-steps 2000000 2>&1)"
        status=$?
        # A planted defect shows up either as a direct state divergence (1)
        # or as the DUT trapping because the mutated instruction left the
        # executable region (3). Both must name the offending pc and insn.
        if { [ "$status" = "1" ] || [ "$status" = "3" ]; } &&
           printf '%s' "$output" | grep -q 'pc = ' &&
           printf '%s' "$output" | grep -q 'insn = '; then
            echo "PASS mutant $name detected by $case_name (exit $status)"
            echo "    $(printf '%s' "$output" | grep -m 1 -E 'MISMATCH|entered a trap')"
            caught=1
            detected=$((detected + 1))
            break
        fi
    done
    if [ "$caught" -eq 0 ]; then
        echo "FAIL mutant $name survived every corpus case"
        survivors=$((survivors + 1))
    fi
done

cp -a "$source/src/cpu.c" "$tree/src/cpu.c"
echo "mutation: $detected detected, $survivors survived"
[ "$survivors" -eq 0 ]
