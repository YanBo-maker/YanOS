#!/usr/bin/env bash
# Combined acceptance case for M2a: block I/O waited for by the cooperative
# runtime. Guest image: tests/guest/m2a_combined.c.
#
# docs/specs/0018-block-protocol.md and docs/specs/0019-cooperative-runtime.md
# were each verified on their own, and never together: the block suite links
# tests/guest/mtrap_entry.S and carries no runtime, and the runtime suite never
# speaks the block protocol. This script runs the one image that does both, on
# the production host path (`yan_run --disk`, which is where the ring geometry,
# the notify callback and publish -> PLIC -> MEIP come from).
#
# What the run has to show (a plain `exit 0` is not enough):
#
#   * `m2a: PASS`, and every phase's evidence line before it: the capacity
#     query, both write/read round trips with byte-exact payload comparison,
#     and the untouched block reading back as zeroes;
#   * for each of the two blocked windows, the line
#     `m2a: window round=N budget=B ticks=B polls=1`. `ticks` is how much
#     progress the *other* task made while the I/O task was blocked; `polls` is
#     how many times the runtime asked the blocked task's predicate in that
#     same window. A block-and-wake runtime asks exactly once. A runtime that
#     fakes wait as "yield and poll again" asks once per round of its loop, so
#     `polls` tracks `ticks` - which is what the second window (96 ticks instead
#     of 32) is for: the waiter's own activity must not scale with it.
#   * the documented trap: `m2a: take-predicate calls=1 caller-take=again`, the
#     case that shows yan_os_block_take() cannot be a wait predicate because its
#     success path consumes the frame.
#
# A transcript that reaches tohost without those lines is a HARNESS error, not
# a pass: the checks this case exists for would not have run.
#
# --mutation plants defects in a *private copy* of os/task.c and reruns the same
# image against it. Nothing in the repository is written to; the scratch space
# is $work/m2a-mutation.<pid>. A defect counts as detected only when the run
# ends in an assertion (the Guest's own failure code, yan_run exit 6) *and* the
# code is the one that assertion owns:
#
#   fake-wait    yan_os_task_wait() = `while (!predicate(context))
#                yan_os_task_yield();` - the "yield + busy-wait" the 0019
#                INTENTION forbids. Every functional check in the image still
#                passes (the response arrives either way); it must be killed by
#                M2A_FAIL_POLLED_WHILE_BLOCKED (0x4000000e), and by nothing
#                else, or the discriminating assertion has no detection power.
#   yield-once   yan_os_task_wait() = one yield and return - a different fake
#                wait; it must be killed by M2A_FAIL_NO_WINDOW (0x4000000f),
#                the window assertion.
#   pure-hang    the control: wait() never returns and never yields. This is not
#                a defect of the case but a check of *this script*: the run must
#                be classified INCONCLUSIVE (no verdict within the step limit),
#                never as a detection. A timeout, a signal death and a broken
#                harness are not detections.
#
# Exit codes: 0 every check that ran passed (in --mutation: every defect was
# detected AND the control was classified inconclusive), 1 a check failed, a
# defect survived, or a source or executor this case needs is missing from the
# repository, 2 usage, 77 this machine has no cross toolchain - the only
# dependency that lives outside the repository.
#
# CTest: register it the way tests/guest/run_block.sh is registered
# (--source/--gcc/--run/--work, SKIP_RETURN_CODE 77, TIMEOUT 300). The plain run
# takes well under a second; --mutation adds four builds and four runs.
set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
source="$(cd "$script_dir/../.." && pwd)"
gcc=""
run=""
work=""
cc="${CC:-cc}"
guest_c=""
task_c=""
blocks=8
max_steps=5000000
mutation=0

while [ $# -gt 0 ]; do
    case "$1" in
        --source) source="$2"; shift 2 ;;
        --gcc) gcc="$2"; shift 2 ;;
        --run) run="$2"; shift 2 ;;
        --work) work="$2"; shift 2 ;;
        --cc) cc="$2"; shift 2 ;;
        --guest) guest_c="$2"; shift 2 ;;
        --task-c) task_c="$2"; shift 2 ;;
        --blocks) blocks="$2"; shift 2 ;;
        --max-steps) max_steps="$2"; shift 2 ;;
        --mutation) mutation=1; shift ;;
        *) echo "run_m2a_combined.sh: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

if [ -z "$gcc" ] || [ -z "$run" ] || [ -z "$work" ]; then
    echo "usage: run_m2a_combined.sh --gcc RISCV_GCC --run YAN_RUN --work DIR" \
         "[--source DIR] [--cc HOST_CC] [--guest FILE] [--task-c FILE]" \
         "[--blocks N] [--max-steps N] [--mutation]" >&2
    exit 2
fi

guest_dir="$source/tests/guest"
os_dir="$source/os"
[ -n "$guest_c" ] || guest_c="$guest_dir/m2a_combined.c"
if [ -n "$task_c" ]; then
    # --task-c is how --mutation links a planted copy. A path that does not
    # exist must not fall back to the real file: that would silently run the
    # unmutated runtime and report a defect as detected.
    [ -f "$task_c" ] || { echo "run_m2a_combined.sh: --task-c '$task_c' does not exist" >&2; exit 2; }
else
    task_c="$os_dir/task.c"
fi

# 77 means "a dependency this machine does not have", and nothing else: the
# cross toolchain is the only thing outside the repository this check needs.
# Everything else - the executor the case runs through, the runtime, the framing
# layer and the Guest case itself - is the thing under test, so a missing one is
# a hard failure. Deleting an implementation must never be a way to a green
# suite: CTest records 77 as a skip, and a skip is not a pass.
if [ ! -x "$gcc" ] && ! command -v "$gcc" > /dev/null 2>&1; then
    echo "SKIP: no cross toolchain at '$gcc'"
    exit 77
fi
missing=0
# An executor that was named but is not there is a build that did not happen,
# not a missing dependency.
if [ ! -x "$run" ]; then
    echo "FAIL the executor under test is not executable: $run"
    missing=1
fi
for required in "$guest_c" "$task_c" "$guest_dir/guest_lib.c" \
                "$guest_dir/link.ld" "$os_dir/trap_entry.S" "$os_dir/task_switch.S" \
                "$os_dir/task.h" "$os_dir/block.c" "$os_dir/block.h" \
                "$os_dir/platform.h" "$os_dir/console.c"; do
    if [ ! -e "$required" ]; then
        echo "FAIL the source under test is missing: $required"
        missing=1
    fi
done
[ "$missing" -eq 0 ] || exit 1

# The success path is the production one. Without --disk the channel has no ring
# and no notify callback, and the case cannot run at all. A named executor that
# does not offer the option is a tool missing a capability this case needs, not a
# missing dependency: 77 here would let a tool-side regression skip the case and
# still leave the suite green.
if ! "$run" --help 2>&1 | grep -q -- '--disk'; then
    echo "FAIL the executor under test has no --disk option: $run"
    exit 1
fi

# A cross toolchain locates `as` and `ld` through its own directory.
gcc_dir="$(cd "$(dirname "$gcc")" && pwd)"
PATH="$gcc_dir:$PATH"
export PATH
nm_tool="$gcc_dir/$(basename "$gcc" | sed 's/gcc$/nm/')"

mkdir -p "$work/images" "$work/logs"
rm -f "$work"/images/m2a-*.elf 2>/dev/null

# ------------------------------------------------------------------ building

# The trap entry is os/trap_entry.S on purpose: the case links the runtime, and
# tests/guest/mtrap_entry.S defines _start, the vector, tohost and a boot stack
# too, so linking both is a duplicate-definition error. It also means the
# yan_guest_* helpers of tests/guest/mtrap.c cannot be used here.
build_image() { # $1 = task.c to link, $2 = output elf, $3 = build log
    "$gcc" -march=rv32im -mabi=ilp32 -mcmodel=medany -static -nostdlib \
        -nostartfiles -ffreestanding -fno-builtin -fno-stack-protector \
        -Wall -Wextra -O2 -I "$guest_dir" -I "$os_dir" \
        -DYAN_M2A_DISK_BLOCKS="$blocks" -T "$guest_dir/link.ld" \
        "$os_dir/trap_entry.S" "$1" "$os_dir/task_switch.S" "$os_dir/block.c" \
        "$os_dir/console.c" "$guest_dir/guest_lib.c" "$guest_c" \
        -Wl,--build-id=none -o "$2" > "$3" 2>&1
}

# The image has to be the runtime one and not the M1 verification image: the
# runtime's vector and its task table are the point of the combination. This is
# also what catches a build that silently fell back to tests/guest/mtrap_entry.S.
image_is_runtime() { # $1 = elf
    [ -n "$nm_tool" ] && [ -x "$nm_tool" ] || return 0
    local symbols
    symbols="$("$nm_tool" "$1" 2>/dev/null)" || return 1
    for wanted in __yan_os_trap_vector yan_os_trap_handler yan_os_task_wait \
                  yan_os_block_take yan_os_block_submit tohost; do
        printf '%s\n' "$symbols" | grep -q " $wanted\$" || return 1
    done
    return 0
}

# ------------------------------------------------------------------- running

run_image() { # $1 = elf, $2 = log; leaves the exit status in $?
    "$run" --image "$1" --disk "$blocks" --terminal --max-steps "$max_steps" \
        < /dev/null > "$2" 2>&1
}

# How a run ended, from yan_run's exit code alone:
#
#   none          the Guest reached its final check (0)
#   assertion     the Guest reported a failure code (6)
#   inconclusive  the run hit the step limit without a verdict (4)
#   harness       the host failed (5), the tool was misused (2), a signature was
#                 exported (7), or the process died on a signal (128+N)
#
# Only `assertion` is a detection. A run that never reached a verdict proves
# nothing about the defect, and a broken harness proves nothing about anything.
failure_kind() { # $1 = exit status
    case "$1" in
        0) echo none ;;
        6) echo assertion ;;
        4) echo inconclusive ;;
        *) echo harness ;;
    esac
}

# The Guest's failure code, from the console line the image prints before it
# finishes. Empty when the run never got that far.
guest_code() { # $1 = log; prints decimal, or nothing
    local hex
    hex="$(sed -n 's/^m2a: FAIL code=0x\([0-9a-f]*\).*/\1/p' "$1" | head -n 1)"
    [ -n "$hex" ] || return 0
    printf '%d' "0x$hex"
}

# The evidence lines, without which an exit 0 would only mean "the image
# finished". The window lines are matched with polls=1 - the whole point of the
# case - and the budget/ticks pair is checked to be equal, so a window that did
# not actually elapse cannot pass.
evidence_missing() { # $1 = log; prints what is missing, empty when complete
    local log="$1" line="" round budget ticks missing="" plain
    # The console lines end with CRLF, so the anchored patterns below are
    # matched against a copy with the CR removed.
    plain="$log.plain"
    tr -d '\r' < "$log" > "$plain" 2>/dev/null || plain="$log"
    grep -q '^m2a: PASS' "$plain" || missing="m2a: PASS"
    grep -q '^m2a: take-predicate calls=1 caller-take=again$' "$plain" || \
        missing="$missing the take-predicate case"
    for round in 0 1; do
        line="$(sed -n "s/^m2a: window round=$round budget=\([0-9]*\) ticks=\([0-9]*\) polls=\([0-9]*\)$/\1 \2 \3/p" "$plain" | head -n 1)"
        if [ -z "$line" ]; then
            missing="$missing window[$round]"
            continue
        fi
        set -- $line
        budget="$1" ticks="$2"
        [ "$3" = "1" ] || missing="$missing window[$round] (polls=$3)"
        [ "$budget" = "$ticks" ] || missing="$missing window[$round] (ticks=$ticks of $budget)"
    done
    printf '%s' "$missing"
}

report_run() { # $1 = label, $2 = exit status, $3 = log; returns 0 when the case passed
    local label="$1" status="$2" log="$3" kind code missing
    kind="$(failure_kind "$status")"
    case "$kind" in
        none)
            missing="$(evidence_missing "$log")"
            if [ -n "$missing" ]; then
                echo "HARNESS $label: the Guest reached its final check, but the"
                echo "     evidence this case exists for is missing:$missing"
                sed 's/^/    /' "$log" | tail -n 5
                return 1
            fi
            echo "PASS $label: tohost PASS with the window and trap evidence"
            grep '^m2a: ' "$log" | sed 's/^/    /'
            return 0 ;;
        assertion)
            code="$(guest_code "$log")"
            echo "FAIL $label: the Guest reported failure code ${code:-unknown}"
            grep '^m2a: ' "$log" | sed 's/^/    /'
            grep 'reported failure code' "$log" | sed 's/^/    /'
            return 1 ;;
        inconclusive)
            echo "INCONCLUSIVE $label: no verdict within $max_steps instructions"
            echo "     a timeout is not a detection, and not a pass either"
            sed 's/^/    /' "$log" | tail -n 3
            return 1 ;;
        *)
            echo "HARNESS $label: the host drive exited $status"
            echo "     a broken harness is not a detection"
            sed 's/^/    /' "$log" | tail -n 5
            return 1 ;;
    esac
}

# ------------------------------------------------------------------- seeding

# The scratch space is $work/m2a-mutation.<pid>: two mutation runs never share a
# directory, and no source file is ever written outside it.
scratch="$work/m2a-mutation.$$"
failures=0

baseline_elf="$work/images/m2a-baseline.elf"
baseline_log="$work/logs/m2a-baseline.log"
if ! build_image "$task_c" "$baseline_elf" "$work/logs/m2a-baseline-build.log"; then
    echo "FAIL the Guest image does not build from $guest_c and $task_c:"
    tail -n 10 "$work/logs/m2a-baseline-build.log" | sed 's/^/    /'
    exit 1
fi
if ! image_is_runtime "$baseline_elf"; then
    echo "HARNESS the image does not carry the cooperative runtime"
    echo "     (it must link os/trap_entry.S and os/task.c, not tests/guest/mtrap_entry.S)"
    exit 1
fi

run_image "$baseline_elf" "$baseline_log"
baseline_status=$?
echo "m2a combined: gcc=$gcc run=$run work=$work blocks=$blocks mutation=$mutation"
if ! report_run "combined case" "$baseline_status" "$baseline_log"; then
    echo "FAIL the combined case does not pass on the unmutated tree, so a caught"
    echo "     defect below would prove nothing"
    exit 1
fi

if [ "$mutation" -eq 0 ]; then
    echo "verdict: 1 passed, 0 failed"
    echo "PASS the combination case: block I/O was waited for by the runtime, and"
    echo "     the waiting task was asked exactly once per window while the other"
    echo "     task made 32 and 96 rounds of progress"
    exit 0
fi

# ----------------------------------------------------------------- mutation

# Planted defects, by name. Each one replaces the whole body of
# yan_os_task_wait() in a private copy; the range is the function's own
# signature line down to the first closing brace in column 0.
#
# The expected code is the assertion that *owns* the defect. A run that fails
# some other assertion is reported apart: for fake-wait, "some other assertion
# caught it" means the discriminating assertion did not bite, which is the
# thing this whole case is built to avoid.
mutants=(fake-wait yield-once pure-hang)
expected_codes=("0x4000000e" "0x4000000f" "")   # pure-hang has no failure code
expected_names=("M2A_FAIL_POLLED_WHILE_BLOCKED" "M2A_FAIL_NO_WINDOW" "")
planted_what=(
    "yan_os_task_wait() = while (!predicate(context)) yan_os_task_yield(); (the forbidden yield + busy-wait)"
    "yan_os_task_wait() = one yield, then return without the event"
    "CONTROL: yan_os_task_wait() spins forever without yielding")

plant() { # $1 = mutant name, $2 = copy of os/task.c
    case "$1" in
        fake-wait)
            sed -i '/^void yan_os_task_wait(YanOsEvent event, int (\*predicate)(void \*), void \*context)$/,/^}$/c\
void yan_os_task_wait(YanOsEvent event, int (*predicate)(void *), void *context)\
{\
    (void)event;\
    while (!predicate(context)) {\
        yan_os_task_yield(); /* MUTANT: yield + busy-wait instead of blocking */\
    }\
}' "$2"
            grep -q 'MUTANT: yield + busy-wait' "$2" &&
                ! grep -q 'waiters\[event\] = current;' "$2" ;;
        yield-once)
            sed -i '/^void yan_os_task_wait(YanOsEvent event, int (\*predicate)(void \*), void \*context)$/,/^}$/c\
void yan_os_task_wait(YanOsEvent event, int (*predicate)(void *), void *context)\
{\
    (void)event;\
    if (!predicate(context)) {\
        yan_os_task_yield(); /* MUTANT: one yield, then return anyway */\
    }\
}' "$2"
            grep -q 'MUTANT: one yield' "$2" &&
                ! grep -q 'waiters\[event\] = current;' "$2" ;;
        pure-hang)
            sed -i '/^void yan_os_task_wait(YanOsEvent event, int (\*predicate)(void \*), void \*context)$/,/^}$/c\
void yan_os_task_wait(YanOsEvent event, int (*predicate)(void *), void *context)\
{\
    (void)event;\
    (void)predicate;\
    (void)context;\
    for (volatile uint32_t spin = 0;; ++spin) { } /* CONTROL: never returns */\
}' "$2"
            grep -q 'CONTROL: never returns' "$2" ;;
        *) echo "run_m2a_combined.sh: unknown mutant '$1'" >&2; return 1 ;;
    esac
}

rm -rf "$scratch"
mkdir -p "$scratch"

detected=0
survivors=0
controls=0

for index in "${!mutants[@]}"; do
    name="${mutants[index]}"
    copy="$scratch/$name.c"
    cp "$task_c" "$copy"
    if ! plant "$name" "$copy"; then
        echo "HARNESS mutation $name was not applied to the copy (pattern mismatch)"
        failures=$((failures + 1))
        continue
    fi
    elf="$work/images/m2a-$name.elf"
    build_log="$work/logs/m2a-$name-build.log"
    if ! build_image "$copy" "$elf" "$build_log"; then
        echo "FAIL mutation $name does not build, so it is not a defect at all:"
        grep -E 'error|Error' "$build_log" | head -n 5 | sed 's/^/    /'
        failures=$((failures + 1))
        continue
    fi
    log="$work/logs/m2a-$name.log"
    run_image "$elf" "$log"
    status=$?
    kind="$(failure_kind "$status")"
    code="$(guest_code "$log")"
    plain="$log.plain"
    tr -d '\r' < "$log" > "$plain" 2>/dev/null || plain="$log"
    echo "mutation $name: planted ${planted_what[index]}"
    echo "    exit=$status kind=$kind code=${code:-none}"

    if [ "$name" = "pure-hang" ]; then
        # The control is not a defect: what it checks is this script's
        # classification. It must land in `inconclusive`, and it must NOT be
        # reported as a detection.
        if [ "$kind" = "inconclusive" ]; then
            echo "PASS control pure-hang: classified INCONCLUSIVE, not a detection"
            controls=$((controls + 1))
        else
            echo "FAIL control pure-hang: a hang was classified '$kind', so a"
            echo "     timeout could be counted as a detection somewhere else"
            failures=$((failures + 1))
        fi
        continue
    fi

    if [ "$kind" != "assertion" ]; then
        echo "FAIL mutation $name survived: the run did not assert (kind=$kind)"
        echo "     a timeout, a signal death or a broken harness is not a detection"
        sed 's/^/    /' "$log" | tail -n 3
        survivors=$((survivors + 1))
        continue
    fi
    want="$(printf '%d' "${expected_codes[index]}")"
    echo "    expected ${expected_names[index]} (${expected_codes[index]})"
    grep -E '^m2a: (window|FAIL)' "$plain" | sed 's/^/    /'
    # The point of the discriminating assertion: the fake waits deliver the
    # response like the real one does, so every functional phase of the image
    # passes and only the window assertion can tell them apart.
    if grep -q '^m2a: read lba=0 zero=1$' "$plain" &&
       [ "$(grep -c '^m2a: FAIL' "$plain")" = "1" ]; then
        echo "    note: every functional check passed (capacity, both writes, the"
        echo "          read-back of both patterns, the untouched block); the"
        echo "          response arrives either way, so only the window assertion"
        echo "          can tell this defect apart from the real runtime"
    fi
    if [ "$code" = "$want" ]; then
        echo "PASS mutation $name detected by ${expected_names[index]}"
        detected=$((detected + 1))
    else
        echo "FAIL mutation $name was caught by another assertion (code ${code:-unknown},"
        echo "     expected ${expected_codes[index]}): the assertion that owns this"
        echo "     defect did not bite"
        survivors=$((survivors + 1))
    fi
done

rm -rf "$scratch"
echo "verdict: $detected detected, $survivors survived, $controls control(s) classified"
if [ "$failures" -ne 0 ] || [ "$survivors" -ne 0 ]; then
    echo "FAIL the mutation check reported a survivor or a broken step"
    exit 1
fi
echo "PASS every planted defect was detected by the assertion that owns it, and"
echo "     the hang control was classified as inconclusive"
exit 0
