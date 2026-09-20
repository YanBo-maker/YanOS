#!/usr/bin/env bash
# Builds and runs the block protocol self-check:
# docs/specs/0018-block-protocol.md, Guest side os/block.c and Host side
# tools/host_block.c.
#
# Three drives carry the checks, and none of them is allowed to weaken an
# assertion:
#
#   unit    tests/guest/block_unit.c drives tools/host_block.c directly, with no
#           CPU and no Guest. It is the only drive that can put chosen bytes in
#           the guest-to-host ring: partial headers, frames that fail
#           validation, a count whose size wraps, a reply that does not fit the
#           response ring. Every receiving rule in the spec is asserted here,
#           because a Guest that behaves cannot produce those frames.
#
#   guest   tests/guest/block_check.c runs on the real CPU, Bus and transport
#           device through tests/guest/block_drive.c, which scripts the shapes
#           tools/yan_run.c cannot produce: a response published in pieces, a
#           rotated ring so a frame crosses the wrap point, an injected
#           malformed frame, a host that never answers, injected backend
#           faults. The Guest asserts what it received; the drive asserts what
#           the Guest did with the ring between the pieces, reading the
#           yan_block_check_* globals the Guest leaves in RAM.
#
#   yan-run every scenario whose channel behaviour tools/yan_run.c can produce
#           is rerun through `yan_run --disk`, the durable Host entry point.
#
# Detection criterion: a failed expectation prints ":FAIL:". Exit 1 is a Host
# assertion, exit 6 is a Guest assertion (the Guest's own check number), 4 is
# "no termination within the step limit" - a run that reached no verdict, which
# the mutation check deliberately does not count as a detection.
#
# Exit codes: 0 every check that ran passed, 1 a check failed, 2 usage, 77 a
# dependency is missing (no cross toolchain, or no yan_run).
#
# CTest: register it the way tests/guest/run_console.sh is registered
# (--source/--gcc/--run/--work, SKIP_RETURN_CODE 77, TIMEOUT 300). The whole
# script takes a few seconds; the planted-defect run is a separate script,
# tests/guest/run_block_mutation.sh.
set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
source="$(cd "$script_dir/../.." && pwd)"
gcc=""
run=""
work=""
cc="${CC:-cc}"
drive="auto"
strict=0
block_c=""
host_block_c=""
check_c=""
drive_c=""
unit_c=""
platform_c=""

while [ $# -gt 0 ]; do
    case "$1" in
        --source) source="$2"; shift 2 ;;
        --gcc) gcc="$2"; shift 2 ;;
        --run) run="$2"; shift 2 ;;
        --work) work="$2"; shift 2 ;;
        --cc) cc="$2"; shift 2 ;;
        --drive) drive="$2"; shift 2 ;;
        --block) block_c="$2"; shift 2 ;;
        --host-block) host_block_c="$2"; shift 2 ;;
        --check) check_c="$2"; shift 2 ;;
        --guest-drive) drive_c="$2"; shift 2 ;;
        --unit) unit_c="$2"; shift 2 ;;
        --platform) platform_c="$2"; shift 2 ;;
        --strict) strict=1; shift ;;
        *) echo "run_block.sh: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

if [ -z "$gcc" ] || [ -z "$work" ]; then
    echo "usage: run_block.sh --gcc RISCV_GCC --work DIR [--run YAN_RUN]" \
         "[--source DIR] [--cc HOST_CC] [--drive auto|unit|guest|yan-run]" \
         "[--block FILE] [--host-block FILE] [--platform FILE] [--check FILE]" \
         "[--guest-drive FILE] [--unit FILE] [--strict]" >&2
    exit 2
fi
case "$drive" in
    auto|unit|guest|yan-run) ;;
    *) echo "run_block.sh: --drive must be auto, unit, guest or yan-run" >&2; exit 2 ;;
esac

guest_dir="$source/tests/guest"
os_dir="$source/os"
tools_dir="$source/tools"
[ -n "$block_c" ] || block_c="$os_dir/block.c"
[ -n "$host_block_c" ] || host_block_c="$tools_dir/host_block.c"
[ -n "$check_c" ] || check_c="$guest_dir/block_check.c"
[ -n "$drive_c" ] || drive_c="$guest_dir/block_drive.c"
[ -n "$unit_c" ] || unit_c="$guest_dir/block_unit.c"
[ -n "$platform_c" ] || platform_c="$os_dir/platform.h"

# 77 means "a dependency this machine does not have", and nothing else: the
# cross toolchain is the only thing outside the repository this check needs.
# Everything else it drives is the thing *under test*, so a missing source is a
# hard failure. Deleting an implementation must never be a way to a green
# suite: CTest records 77 as a skip, and a skip is not a pass.
if [ ! -x "$gcc" ] && ! command -v "$gcc" > /dev/null 2>&1; then
    echo "SKIP: no cross toolchain at '$gcc'"
    exit 77
fi
missing=0
for required in "$block_c" "$os_dir/block.h" "$platform_c" \
                "$host_block_c" "$tools_dir/host_block.h" "$check_c" \
                "$drive_c" "$unit_c" "$guest_dir/mtrap_entry.S" \
                "$guest_dir/mtrap.c" "$guest_dir/guest_lib.c" \
                "$guest_dir/link.ld"; do
    if [ ! -e "$required" ]; then
        echo "FAIL the source under test is missing: $required"
        missing=1
    fi
done
[ "$missing" -eq 0 ] || exit 1
# An executor that was named but is not there is a build that did not happen,
# not a missing dependency.
if [ -n "$run" ] && [ ! -x "$run" ]; then
    echo "FAIL the executor under test is not executable: $run"
    exit 1
fi

# A cross toolchain locates `as` and `ld` through its own directory.
gcc_dir="$(cd "$(dirname "$gcc")" && pwd)"
PATH="$gcc_dir:$PATH"
export PATH

mkdir -p "$work/images" "$work/logs" "$work/capture"
rm -f "$work"/images/*.elf 2>/dev/null

failed=0
unrunnable=0
ran=0

# ------------------------------------------------------------------ host unit

build_unit() {
    "$cc" -O1 -std=c17 -Wall -Wextra -Wpedantic -Werror \
        -I "$source/include" -I "$tools_dir" \
        -o "$work/block_unit" "$unit_c" "$host_block_c" \
        "$source/src/ram.c" "$source/src/transport.c" > "$work/logs/unit-build.log" 2>&1
}

# The verdict of the unit drive is read from its own assertion markers, never
# from an exit status. A crash (a signal), a fixture that refuses to build, a
# sanitizer report or any other diagnostic is something that happened *around*
# the checks and says nothing about whether they have detection power, so it is
# reported as HARNESS-ERROR and never as ASSERT-FAIL. That distinction is what
# stops "the binary died" from being counted as "the checks caught it".
run_unit() {
    "$work/block_unit" > "$work/logs/unit.log" 2> "$work/logs/unit.err"
    local status=$?
    local marks
    marks="$(grep -c ':FAIL:' "$work/logs/unit.log" || true)"
    if [ "${marks:-0}" -gt 0 ]; then
        echo "ASSERT-FAIL: unit ($marks failed check(s) reported)"
        echo "FAIL the Host unit checks reported a failed assertion (exit $status)"
        grep -n ':FAIL:' "$work/logs/unit.log" | head -n 10 | sed 's/^/    /'
        failed=1
        return 1
    fi
    if [ "$status" -ne 0 ]; then
        echo "HARNESS-ERROR: unit (exit $status without a failed assertion)"
        echo "FAIL the Host unit checks stopped without reporting a failed check (exit $status)"
        tail -n 3 "$work/logs/unit.log" | sed 's/^/    /'
        [ -s "$work/logs/unit.err" ] && tail -n 2 "$work/logs/unit.err" | sed 's/^/    /'
        failed=1
        return 1
    fi
    if [ -s "$work/logs/unit.err" ]; then
        echo "HARNESS-ERROR: unit (wrote to standard error without a failed check)"
        echo "FAIL the Host unit checks wrote to standard error:"
        head -n 5 "$work/logs/unit.err" | sed 's/^/    /'
        failed=1
        return 1
    fi
    echo "PASS unit  $(tail -n 1 "$work/logs/unit.log")"
    return 0
}

if [ "$drive" != "guest" ] && [ "$drive" != "yan-run" ]; then
    if build_unit; then
        ran=$((ran + 1))
        run_unit || true
    else
        echo "FAIL the Host unit checks do not build:"
        tail -n 5 "$work/logs/unit-build.log" | sed 's/^/    /'
        failed=1
    fi
fi

# --------------------------------------------------------------- guest drives

build_guest_drive() {
    "$cc" -O1 -std=c17 -Wall -Wextra -Werror \
        -I "$source/include" -I "$tools_dir" \
        -o "$work/block_drive" "$drive_c" "$source/tools/host_file.c" \
        "$host_block_c" "$source"/src/*.c > "$work/logs/drive-build.log" 2>&1
}

build_image() { # $1 = scenario number, $2 = scenario name, $3 = output elf
    "$gcc" -march=rv32im -mabi=ilp32 -mcmodel=medany -static -nostdlib \
        -nostartfiles -ffreestanding -fno-builtin -fno-stack-protector \
        -Wall -Wextra -O2 -I "$(dirname "$platform_c")" -I "$guest_dir" -I "$os_dir" \
        -DYAN_BLOCK_SCENARIO="$1" \
        -DYAN_BLOCK_SCENARIO_NAME="\"$2\"" \
        -DYAN_BLOCK_EXPECT_BLOCKS=8 \
        -T "$guest_dir/link.ld" \
        "$guest_dir/mtrap_entry.S" "$guest_dir/mtrap.c" \
        "$guest_dir/guest_lib.c" "$check_c" "$block_c" \
        -Wl,--build-id=none -o "$3"
}

# The rows are the executable form of the spec's VERIFY list; the comment on
# each one names what the flags make the drive do that a plain `yan_run --disk`
# cannot.
row_labels=(
    "capacity"
    "read whole frames"
    "write and read back"
    "parameter errors"
    "order and tag"
    "device faults"
    "flow control: no space"
    "flow control: yield and retry"
    "one block is the v1 limit"
    "header split 1 + 15"
    "header split 8 + 8"
    "payload split"
    "header across the wrap"
    "payload across the wrap"
    "request header across the wrap"
    "stale ring content behind a one-byte first piece"
    "injected frame: reserved != 0"
    "injected frame: count cannot fit"
    "no host attached"
)
row_images=(
    capacity read write params order fault flow_short flow_retry limit
    split split split split split split split malformed malformed no_host
)
row_options=(
    "--blocks 8 --expect-served 1"
    "--blocks 8 --expect-served 6"
    "--blocks 8 --expect-served 2 --expect-written 3"
    "--blocks 8 --expect-served 12"
    "--blocks 8 --expect-served 6"
    "--blocks 8 --fail-before 1,4,6 --expect-served 8 --expect-pattern 2"
    "--blocks 8 --serve none --expect-g2h-bytes 4112 --expect-served 0"
    "--blocks 8 --expect-served 2 --expect-written 4,5"
    "--blocks 8 --expect-served 1 --expect-written 0"
    "--blocks 8 --split 1,15,2048,2048 --expect-served 1 --expect-consumed 4112 --expect-wrapped 0"
    "--blocks 8 --split 8,8,4096 --expect-served 1 --expect-consumed 4112 --expect-wrapped 0"
    "--blocks 8 --split 16,16,2048,2032 --piece-steps 1000 --expect-consumed 4112 --expect-wrapped 0"
    "--blocks 8 --ring-offset 8184 --split 1,15,4096 --expect-consumed 4112 --expect-wrapped 1"
    "--blocks 8 --ring-offset 4176 --split 16,2048,2048 --expect-consumed 4112 --expect-wrapped 1"
    "--blocks 8 --request-offset 8184 --split 8,8,4096 --expect-served 1 --expect-consumed 4112 --expect-wrapped 0"
    "--blocks 8 --ring-offset 8184 --split 1,4096 --stale-head --expect-served 1 --expect-consumed 4112 --expect-wrapped 1"
    "--blocks 8 --inject reserved --expect-served 1 --expect-consumed 32"
    "--blocks 8 --inject count --expect-served 1 --expect-consumed 4128"
    "--blocks 0 --expect-served 0 --expect-g2h-bytes 0"
)
scenario_numbers=(1 2 3 4 5 6 7 8 9 10 11 12)
scenario_names=(capacity read write params order fault flow_short flow_retry \
                limit split malformed no_host)

guest_drive_ready=0
if [ "$drive" != "unit" ] && [ "$drive" != "yan-run" ]; then
    if build_guest_drive; then
        guest_drive_ready=1
    else
        echo "BUILD-FAIL: the guest drive"
        echo "FAIL the Guest drive does not build:"
        tail -n 5 "$work/logs/drive-build.log" | sed 's/^/    /'
        failed=1
    fi
fi

if [ "$guest_drive_ready" -eq 1 ]; then
    for index in "${!scenario_numbers[@]}"; do
        elf="$work/images/${scenario_names[index]}.elf"
        if ! build_image "${scenario_numbers[index]}" "${scenario_names[index]}" \
                "$elf" > "$work/logs/build-${scenario_names[index]}.log" 2>&1; then
            echo "BUILD-FAIL: the guest image for ${scenario_names[index]}"
            echo "FAIL the Guest image for ${scenario_names[index]} does not build:"
            tail -n 5 "$work/logs/build-${scenario_names[index]}.log" | sed 's/^/    /'
            failed=1
        fi
    done

    for index in "${!row_labels[@]}"; do
        elf="$work/images/${row_images[index]}.elf"
        [ -e "$elf" ] || continue
        log="$work/logs/row-$index.log"
        # shellcheck disable=SC2086
        "$work/block_drive" --image "$elf" --max-steps 400000 \
            ${row_options[index]} > "$log" 2>&1
        status=$?
        ran=$((ran + 1))
        # The marker in the log is the evidence, the exit status only says which
        # kind of ending this was. A trap is not a failed check: the Guest's own
        # panic handler writes `tohost` too (tests/guest/mtrap.c ends the run
        # with 0xbad0 | cause), and treating that as a detection would let "the
        # Guest died" pass for "the checks noticed something".
        marked=0
        grep -q ':FAIL:' "$log" && marked=1
        case "$status" in
            0)
                if [ "$marked" -eq 1 ]; then
                    echo "ASSERT-FAIL: guest ${row_labels[index]} (printed :FAIL: but exited 0)"
                    failed=1
                else
                    echo "PASS [guest] ${row_labels[index]}"
                fi ;;
            1)
                if [ "$marked" -eq 1 ]; then
                    echo "ASSERT-FAIL: guest ${row_labels[index]} (host assertion)"
                    echo "FAIL [guest] ${row_labels[index]}: a Host assertion failed"
                    grep ':FAIL:' "$log" | head -n 5 | sed 's/^/    /'
                else
                    echo "HARNESS-ERROR: guest ${row_labels[index]} (the drive failed without a failed assertion)"
                    echo "FAIL [guest] ${row_labels[index]}: the drive exited 1 with no :FAIL: line"
                    tail -n 3 "$log" | sed 's/^/    /'
                fi
                failed=1 ;;
            6)
                if [ "$marked" -eq 1 ]; then
                    echo "ASSERT-FAIL: guest ${row_labels[index]} (guest check)"
                    echo "FAIL [guest] ${row_labels[index]}: a Guest check failed"
                    grep ':FAIL:' "$log" | head -n 5 | sed 's/^/    /'
                else
                    echo "HARNESS-ERROR: guest ${row_labels[index]} (guest failure code without a failed check)"
                    echo "FAIL [guest] ${row_labels[index]}: the run ended on a guest code with no :FAIL: line"
                    tail -n 3 "$log" | sed 's/^/    /'
                fi
                failed=1 ;;
            8)
                # A trap is not a failed check by itself, but a failed check
                # that the Guest then fell over on is still a failed check: the
                # assertion is the evidence, and hiding it would throw away
                # detection power the suite has already proved. It is labelled
                # as cascade evidence so a reviewer knows to check the causality.
                if grep -qE '^:FAIL: [a-z_]+: ' "$log"; then
                    echo "ASSERT-FAIL: guest ${row_labels[index]} (guest check, cascaded into a trap)"
                    echo "FAIL [guest] ${row_labels[index]}: a Guest check failed and the run then trapped"
                    grep -E '^:FAIL: [a-z_]+: ' "$log" | head -n 5 | sed 's/^/    /'
                    grep -m 1 -E '^TRAP:|^UNEXPECTED:' "$log" | sed 's/^/    /'
                else
                    echo "INCONCLUSIVE: guest ${row_labels[index]} (the guest trapped or ended on a non-check code)"
                    echo "FAIL [guest] ${row_labels[index]}: the run did not end in a check"
                    grep -m 2 -E '^TRAP:|^UNEXPECTED:' "$log" | sed 's/^/    /'
                fi
                failed=1 ;;
            4)
                echo "NO-VERDICT: guest ${row_labels[index]} (step limit)"
                echo "FAIL [guest] ${row_labels[index]}: no verdict within the step limit"
                tail -n 2 "$log" | sed 's/^/    /'
                failed=1 ;;
            *)
                echo "HARNESS-ERROR: guest ${row_labels[index]} (exit $status)"
                echo "FAIL [guest] ${row_labels[index]}: the drive exited $status"
                tail -n 3 "$log" | sed 's/^/    /'
                failed=1 ;;
        esac
    done
fi

# ---------------------------------------------------------- yan_run integation

if [ "$drive" = "unit" ] || [ "$drive" = "guest" ]; then
    echo "note: --drive $drive was given, so the yan_run integration run was skipped"
elif [ -z "$run" ]; then
    echo "PENDING the yan_run integration drive: --run was not given"
    unrunnable=$((unrunnable + 4))
elif [ ! -x "$run" ]; then
    echo "FAIL the executor under test is not executable: $run"
    failed=1
elif ! "$run" --help 2>&1 | grep -q -- '--disk'; then
    # A named executor that cannot carry the disk is an unimplemented feature,
    # not a missing dependency: it must not be able to pass quietly. Deleting
    # --disk from tools/yan_run.c would otherwise leave this suite green.
    echo "FAIL the executor under test has no --disk option: $run"
    failed=1
else
    # The scenarios that a plain `yan_run --disk` can carry: no scripted
    # shaping, no injected faults. The Guest's verdict and the printed
    # ":PASS:"/"...FAIL:" markers are the whole result.
    # yan_run prints "the Guest reported failure code N" and exits 6 for both a
    # failed check and a trap, so the code it names decides which one this was:
    # tests/guest/mtrap.h's panic ends the run with 0xbad0 | cause.
    is_trap_code() {
        case "${1:-}" in ''|*[!0-9]*) return 1 ;; esac
        [ "$1" -ge $((0xbad0)) ] && [ "$1" -le $((0xbaff)) ]
    }
    reported_code() {
        sed -n 's/.*reported failure code \([0-9][0-9]*\) .*/\1/p' "$1" | head -n 1
    }
    yan_run_names=(capacity read write order params)
    yan_run_numbers=(1 2 3 5 4)
    for index in "${!yan_run_names[@]}"; do
        name="${yan_run_names[index]}"
        elf="$work/images/$name.elf"
        if [ ! -e "$elf" ] && ! build_image "${yan_run_numbers[index]}" "$name" \
                "$elf" > "$work/logs/build-$name.log" 2>&1; then
            echo "BUILD-FAIL: the guest image for $name (yan_run)"
            echo "FAIL the Guest image for $name does not build for yan_run"
            failed=1
            continue
        fi
        log="$work/logs/yan-run-$name.log"
        "$run" --image "$elf" --disk 8 --terminal --max-steps 400000 \
            < /dev/null > "$log" 2>&1
        status=$?
        ran=$((ran + 1))
        code="$(reported_code "$log")"
        if [ "$status" -eq 0 ] && ! grep -q ':FAIL:' "$log"; then
            echo "PASS [yan-run] $name"
        elif grep -q ':FAIL:' "$log" && [ "$status" -eq 6 ]; then
            echo "ASSERT-FAIL: yan-run $name (guest check)"
            echo "FAIL [yan-run] $name: a Guest check failed"
            grep ':FAIL:' "$log" | head -n 3 | sed 's/^/    /'
            failed=1
        elif [ "$status" -eq 0 ]; then
            echo "ASSERT-FAIL: yan-run $name (printed :FAIL: but exited 0)"
            echo "FAIL [yan-run] $name: printed :FAIL: but exited 0"
            grep ':FAIL:' "$log" | head -n 3 | sed 's/^/    /'
            failed=1
        elif is_trap_code "$code" && grep -qE '^:FAIL: [a-z_]+: ' "$log"; then
            echo "ASSERT-FAIL: yan-run $name (guest check, cascaded into a trap)"
            echo "FAIL [yan-run] $name: a Guest check failed and the run then trapped"
            grep -E '^:FAIL: [a-z_]+: ' "$log" | head -n 3 | sed 's/^/    /'
            failed=1
        elif is_trap_code "$code"; then
            echo "INCONCLUSIVE: yan-run $name (the guest trapped, code ${code})"
            echo "FAIL [yan-run] $name: the run ended in a trap, not in a check"
            tail -n 2 "$log" | sed 's/^/    /'
            failed=1
        else
            echo "HARNESS-ERROR: yan-run $name (exit $status without a failed check)"
            echo "FAIL [yan-run] $name: the run exited $status with no :FAIL: line"
            tail -n 2 "$log" | sed 's/^/    /'
            failed=1
        fi
    done
fi

# ---------------------------------------------------------------- placeholders

for log in "$work"/logs/*.log; do
    if grep -q 'not implemented' "$log" 2>/dev/null; then
        echo "FAIL a placeholder implementation is still in the tree: $log"
        failed=1
    fi
done

# -------------------------------------------------------------------- verdict
echo "block: $ran check group(s) ran, $unrunnable driver(s) unavailable"
if [ "$failed" -ne 0 ]; then
    echo "FAIL the block protocol self-check reported at least one failure"
    exit 1
fi
if [ "$unrunnable" -ne 0 ]; then
    echo "PENDING $unrunnable driver(s) could not run (see above)"
    if [ "$strict" -eq 1 ]; then
        # 77 means "a repository-external dependency is missing". Something that
        # was asked for and did not run is a failure, and a failure may not be
        # recorded as a skip.
        echo "FAIL --strict was given and something is still pending"
        exit 1
    fi
fi
echo "PASS every block protocol check that could run passed"
exit 0
