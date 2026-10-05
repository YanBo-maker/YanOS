#!/usr/bin/env bash
# Golden regression for the executor's default path.
#
# Why this exists: the executor was moved onto the platform's own step function,
# which is exactly the kind of change that can quietly alter what a Guest
# observes. The evidence for "the default path did not change" used to be a
# hand-run comparison of two binaries; this script turns it into an artifact the
# repository can re-run. It compiles tests/guest/golden_probe.c (a deterministic
# Guest that touches no device), runs it through yan_run with an explicitly
# stated geometry, and compares two frozen outputs byte for byte:
#
#   tests/guest/golden/probe.trace.jsonl     yan_run --trace output
#   tests/guest/golden/probe.signature.hex   the --signature bytes as hex text
#
# The probe stops through `tohost` with 1, so exit 0 means "the probe passed";
# anything else fails here instead of being compared, because comparing the
# output of a failed run would freeze the wrong behaviour.
#
# Regenerating is explicit and never automatic: `--update` rewrites both
# baselines (and refuses to do so if the probe is not deterministic). See
# tests/guest/golden/README.md for when that is the right thing to do.
#
# Exit codes: 0 the run matched the baseline, 1 it differed, the probe failed or
# a source or baseline this check needs is missing from the repository,
# 2 a usage error, 77 the machine lacks a host tool that lives outside the
# repository (the cross toolchain, timeout(1) or its nm).
set -u

gcc=""
run=""
work=""
source=""
update=0

while [ $# -gt 0 ]; do
    case "$1" in
        --source) source="$2"; shift 2 ;;
        --gcc) gcc="$2"; shift 2 ;;
        --run) run="$2"; shift 2 ;;
        --work) work="$2"; shift 2 ;;
        --update) update=1; shift ;;
        *) echo "run_golden.sh: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

if [ -z "$gcc" ] || [ -z "$run" ] || [ -z "$work" ]; then
    echo "usage: run_golden.sh --gcc RISCV_GCC --run YAN_RUN --work DIR [--source DIR] [--update]" >&2
    exit 2
fi

# Default to the checkout this script lives in, so a build tree anywhere can
# point at these sources without passing --source.
if [ -z "$source" ]; then
    source="$(cd "$(dirname "$0")/../.." && pwd)"
fi

guest_dir="$source/tests/guest"
golden_dir="$guest_dir/golden"
trace_base="$golden_dir/probe.trace.jsonl"
signature_base="$golden_dir/probe.signature.hex"

# The cross toolchain, timeout(1) and the toolchain's nm are host tools outside
# the repository: without them the check cannot run at all, so those are the
# only 77s here. The probe, the executor and the two baselines are not
# dependencies: the sources are the thing under test, and the baselines are the
# expected output this check exists to compare against. A missing one is a hard
# failure, never a skip - CTest records 77 as a skip, and a skip is not a pass.
missing=0
if [ ! -x "$gcc" ] && ! command -v "$gcc" > /dev/null 2>&1; then
    echo "SKIP: no cross toolchain at '$gcc'"
    exit 77
fi
command -v timeout > /dev/null 2>&1 || { echo "SKIP: missing timeout(1)"; exit 77; }
# The signature range comes from the linked image, so the toolchain's nm is a
# real dependency of this check.
nm_tool="${gcc%gcc}nm"
[ -x "$nm_tool" ] || { echo "SKIP: missing $nm_tool"; exit 77; }
for required in "$guest_dir/golden_probe.c" "$guest_dir/start.S" \
                "$guest_dir/guest_lib.c" "$guest_dir/link.ld"; do
    if [ ! -e "$required" ]; then
        echo "FAIL the source under test is missing: $required"
        missing=1
    fi
done
if [ ! -x "$run" ]; then
    echo "FAIL the executor under test is not executable: $run"
    missing=1
fi
if [ "$update" -eq 0 ]; then
    for baseline in "$trace_base" "$signature_base"; do
        if [ ! -e "$baseline" ]; then
            echo "FAIL the expected output baseline is missing: $baseline"
            missing=1
        fi
    done
fi
[ "$missing" -eq 0 ] || exit 1

# A cross toolchain locates `as` and `ld` through its own directory.
gcc_dir="$(cd "$(dirname "$gcc")" && pwd)"
PATH="$gcc_dir:$PATH"
export PATH
mkdir -p "$work"

elf="$work/golden_probe.elf"
if ! "$gcc" -march=rv32im -mabi=ilp32 -mcmodel=medany -static -nostdlib \
        -nostartfiles -ffreestanding -fno-builtin -fno-stack-protector \
        -Wall -Wextra -O2 -T "$guest_dir/link.ld" \
        "$guest_dir/start.S" "$guest_dir/golden_probe.c" "$guest_dir/guest_lib.c" \
        -Wl,--build-id=none -o "$elf" > "$work/build.log" 2>&1; then
    echo "FAIL the golden probe does not build:"
    sed 's/^/    /' "$work/build.log"
    exit 1
fi

# The signature window is the image's own data region, exactly as the other
# Guest validators use it; hard-coding it would silently follow the linker.
# nm prints bare hex, so the 0x prefix is added here: yan_run parses numbers
# with strtoull(..., 0), where a bare 80000520 would be read as decimal.
sig_start_raw="$("$nm_tool" "$elf" | awk '$3 == "begin_signature" { print $1 }')"
sig_end_raw="$("$nm_tool" "$elf" | awk '$3 == "end_signature" { print $1 }')"
is_hex() {
    case "$1" in
        ""|*[!0-9a-fA-F]*) return 1 ;;
        *) return 0 ;;
    esac
}
if ! is_hex "$sig_start_raw" || ! is_hex "$sig_end_raw"; then
    echo "FAIL the probe image does not carry begin_signature/end_signature" >&2
    exit 1
fi
sig_start="0x$sig_start_raw"
sig_end="0x$sig_end_raw"

# One run, into $1 as a directory holding the artifacts.
collect() {
    local into="$1"
    mkdir -p "$into"
    # The geometry is stated explicitly: this baseline is about the default
    # window being 16 MiB, so borrowing the default would weaken the check.
    timeout 300 "$run" --image "$elf" --base 0x80000000 --ram 16777216 \
        --max-steps 200000 --trace "$into/probe.trace.jsonl" \
        --signature "$into/probe.signature.bin" "$sig_start" "$sig_end" \
        > "$into/run.out" 2> "$into/run.err"
    local status=$?
    if [ "$status" -ne 0 ]; then
        echo "FAIL the golden probe run exited with $status, so there is nothing to compare:" >&2
        sed 's/^/    /' "$into/run.err" >&2
        exit 1
    fi
    # The default path has no terminal attached: it must stay silent, and the
    # probe itself reports success through tohost rather than through output.
    [ -s "$into/run.out" ] &&
        { echo "FAIL the default path wrote to standard output:" >&2; sed 's/^/    /' "$into/run.out" >&2; exit 1; }
    [ -s "$into/run.err" ] &&
        { echo "FAIL the default path wrote to standard error:" >&2; sed 's/^/    /' "$into/run.err" >&2; exit 1; }
    # Hex text, not the raw bytes: a baseline has to be readable and diffable.
    od -Ax -tx1 -v "$into/probe.signature.bin" > "$into/probe.signature.hex"
}

# First line at which two text files disagree, 1-based; empty when equal.
first_difference_line() {
    awk '
        NR == FNR { golden[FNR] = $0; total = FNR; next }
        { if (FNR > total || $0 != golden[FNR]) { print FNR; settled = 1; exit } }
        END { if (!settled && FNR < total) print FNR + 1 }
    ' "$1" "$2"
}

report_difference() {
    local label="$1" actual="$2" golden="$3" line
    echo "FAIL the $label no longer matches tests/guest/golden/$(basename "$golden")" >&2
    echo "    $(cmp "$actual" "$golden" 2>&1 | head -n 1)" >&2
    line="$(first_difference_line "$golden" "$actual")"
    if [ -n "$line" ]; then
        echo "    first difference at line $line:" >&2
        echo "      golden: $(sed -n "${line}p" "$golden")" >&2
        echo "      actual: $(sed -n "${line}p" "$actual")" >&2
    fi
    echo "    if the change is intended, regenerate with: $0 --gcc $gcc --run $run --work $work --update" >&2
    exit 1
}

collect "$work/current"

if [ "$update" -eq 1 ]; then
    # A baseline is only worth freezing when it is reproducible; two runs that
    # disagree would freeze one arbitrary sample.
    collect "$work/repeat"
    cmp -s "$work/current/probe.trace.jsonl" "$work/repeat/probe.trace.jsonl" ||
        { echo "FAIL the probe is not deterministic: two traces differ" >&2; exit 1; }
    cmp -s "$work/current/probe.signature.hex" "$work/repeat/probe.signature.hex" ||
        { echo "FAIL the probe is not deterministic: two signatures differ" >&2; exit 1; }
    mkdir -p "$golden_dir"
    cp "$work/current/probe.trace.jsonl" "$trace_base"
    cp "$work/current/probe.signature.hex" "$signature_base"
    echo "UPDATED $trace_base ($(wc -l < "$trace_base") lines)"
    echo "UPDATED $signature_base ($(wc -l < "$signature_base") lines)"
    echo "Review the diff before committing: a regenerated baseline hides every behaviour change it should have caught."
    exit 0
fi

cmp -s "$work/current/probe.trace.jsonl" "$trace_base" ||
    report_difference "trace" "$work/current/probe.trace.jsonl" "$trace_base"
cmp -s "$work/current/probe.signature.hex" "$signature_base" ||
    report_difference "signature" "$work/current/probe.signature.hex" "$signature_base"

echo "PASS the default path reproduced the golden trace ($(wc -l < "$trace_base") lines) byte for byte"
echo "PASS the default path reproduced the golden signature ($(wc -c < "$work/current/probe.signature.bin") bytes) byte for byte"
exit 0
