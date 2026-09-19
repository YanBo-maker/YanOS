#!/usr/bin/env bash
# Proves the differential tester really consumes the reference model's state.
#
# Planting a bug in the DUT only shows that the tester can report a difference:
# a broken tester that read the DUT twice and compared it with itself would
# still pass that check. This script breaks the *reference* instead. The NEMU
# source tree is copied, one instruction pattern is mutated, the shared object
# is rebuilt, and yan_difftest must still report a mismatch. A baseline run with
# the unmutated reference proves the Guest image itself is fine, so the failure
# can only come from the reference.
#
# Exit codes: 0 the tester consumed the reference state, 1 it did not, 2 usage,
# 77 a dependency is missing.
set -u

source=""
ref_dir=""
baseline_ref=""
cc="${CC:-cc}"
gcc=""
work=""

while [ $# -gt 0 ]; do
    case "$1" in
        --source) source="$2"; shift 2 ;;
        --ref-dir) ref_dir="$2"; shift 2 ;;
        --baseline-ref) baseline_ref="$2"; shift 2 ;;
        --cc) cc="$2"; shift 2 ;;
        --gcc) gcc="$2"; shift 2 ;;
        --work) work="$2"; shift 2 ;;
        *) echo "run_ref_mutation.sh: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

if [ -z "$source" ] || [ -z "$ref_dir" ] || [ -z "$gcc" ] || [ -z "$work" ]; then
    echo "usage: run_ref_mutation.sh --source DIR --ref-dir NEMU_SOURCE --cc CC --gcc RISCV_GCC --work DIR [--baseline-ref SO]" >&2
    exit 2
fi
for required in "$gcc" "$source/src/cpu.c" "$ref_dir/src/isa/riscv32/inst.c" \
                "$ref_dir/Makefile" "$ref_dir/include/config/auto.conf"; do
    [ -e "$required" ] || { echo "SKIP: missing $required"; exit 77; }
done

gcc_dir="$(cd "$(dirname "$gcc")" && pwd)"
PATH="$gcc_dir:$PATH"
export PATH
mkdir -p "$work"

tree="$work/nemu"
rm -rf "$tree" "$work/baseline"
cp -a "$ref_dir" "$tree"
rm -rf "$tree/build"

# The NEMU Makefile includes its parent directory's Makefile, which in the
# ysyx workbench installs a git tracer that commits on every build. The
# stand-in keeps the build side-effect free.
cat > "$work/Makefile" <<'MAKEFILE'
define git_commit
	@true
endef
define git_soft_checkout
	@true
endef
MAKEFILE

build_reference() {
    ( cd "$tree" && NEMU_HOME="$tree" make SHARE=1 ENGINE=interpreter \
        GUEST_ISA=riscv32 -j"$(nproc)" ) > "$work/nemu-build.log" 2>&1
}

pattern='INSTPAT("0000000 ????? ????? 010 ????? 01100 11", slt    , R, R(rd) = (sword_t)src1 < (sword_t)src2);'
replacement='INSTPAT("0000000 ????? ????? 010 ????? 01100 11", slt    , R, R(rd) = src1 < src2);'

if ! cp -a "$ref_dir/src/isa/riscv32/inst.c" "$tree/src/isa/riscv32/inst.c" ||
   ! build_reference; then
    echo "SKIP: cannot build the unmutated reference: $(tail -n 1 "$work/nemu-build.log")"
    exit 77
fi
cp -a "$tree/build/riscv32-nemu-interpreter-so" "$work/reference-baseline-so"

before="$(md5sum < "$tree/src/isa/riscv32/inst.c")"
sed -i "s|$pattern|$replacement|" "$tree/src/isa/riscv32/inst.c"
after="$(md5sum < "$tree/src/isa/riscv32/inst.c")"
if [ "$before" = "$after" ]; then
    echo "FAIL the reference mutation was not applied (pattern mismatch)"
    exit 1
fi
if ! build_reference; then
    echo "FAIL the mutated reference did not build: $(tail -n 1 "$work/nemu-build.log")"
    exit 1
fi
mutated="$tree/build/riscv32-nemu-interpreter-so"

# Build the tester and one Guest image out of the tree under test.
if ! "$cc" -O1 -std=c17 -I "$source/include" -o "$work/yan_difftest" \
        "$source/tools/yan_difftest.c" "$source/tools/host_file.c" \
        "$source"/src/*.c -ldl > "$work/tester-build.log" 2>&1; then
    echo "SKIP: cannot build the tester: $(tail -n 1 "$work/tester-build.log")"
    exit 77
fi
elf="$work/alu.elf"
if ! "$gcc" -march=rv32im -mabi=ilp32 -mcmodel=medany -static -nostdlib \
        -nostartfiles -ffreestanding -fno-builtin -O0 \
        -T "$source/tests/guest/link.ld" "$source/tests/guest/start.S" \
        "$source/tests/guest/guest_lib.c" "$source/tests/guest/alu.c" \
        -Wl,--build-id=none -o "$elf" > "$work/guest-build.log" 2>&1; then
    echo "SKIP: cannot build the Guest image"
    exit 77
fi

# Baseline: the same image against the unmutated reference must pass.
if [ -n "$baseline_ref" ] && [ -e "$baseline_ref" ]; then
    "$work/yan_difftest" --image "$elf" --ref-so "$baseline_ref" \
        --max-steps 2000000 > "$work/baseline.log" 2>&1
    if [ "$?" != "0" ]; then
        echo "FAIL the Guest image fails against the unmutated reference"
        sed 's/^/    /' "$work/baseline.log" | tail -3
        exit 1
    fi
else
    "$work/yan_difftest" --image "$elf" --ref-so "$work/reference-baseline-so" \
        --max-steps 2000000 > "$work/baseline.log" 2>&1
    if [ "$?" != "0" ]; then
        echo "FAIL the Guest image fails against the unmutated reference"
        sed 's/^/    /' "$work/baseline.log" | tail -3
        exit 1
    fi
fi

output="$("$work/yan_difftest" --image "$elf" --ref-so "$mutated" \
          --max-steps 2000000 2>&1)"
status=$?
if [ "$status" = "1" ] &&
   printf '%s' "$output" | grep -q 'pc = ' &&
   printf '%s' "$output" | grep -q 'insn = '; then
    echo "PASS the tester reported the mutated reference"
    echo "    $(printf '%s' "$output" | grep -m 1 'MISMATCH')"
    exit 0
fi
echo "FAIL a broken reference model went unnoticed (exit $status)"
printf '%s\n' "$output" | tail -3 | sed 's/^/    /'
exit 1
