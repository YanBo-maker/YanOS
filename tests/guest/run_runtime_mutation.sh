#!/usr/bin/env bash
# Planted-defect check for the cooperative runtime:
# docs/specs/0019-cooperative-runtime.md, VERIFY group C.
#
# Each mutant is a private copy of os/task.c with one defect planted in it. The
# whole scenario suite is then rebuilt and rerun against that copy, and a defect
# counts as detected only when a scenario *asserts*: the Guest reports a failure
# code where it had to reach its final check, or the drive's invariant on the
# Guest's own trace fails. A run that merely stops terminating, traps or
# crashes is reported as INCONCLUSIVE by run_runtime.sh and is NOT a detection.
#
# The scratch space is $work/run.<pid>: two mutation runs never share a
# directory, and no source file is ever written outside it.
#
# Exit codes: 0 every defect was detected, 1 a defect survived or the check
# itself failed (a mutant that does not build, or a source under test that is
# missing), 2 usage, 77 this machine has no cross toolchain.
set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
source="$(cd "$script_dir/../.." && pwd)"
gcc=""
work=""
cc="${CC:-cc}"

while [ $# -gt 0 ]; do
    case "$1" in
        --source) source="$2"; shift 2 ;;
        --gcc) gcc="$2"; shift 2 ;;
        --work) work="$2"; shift 2 ;;
        --cc) cc="$2"; shift 2 ;;
        *) echo "run_runtime_mutation.sh: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

if [ -z "$gcc" ] || [ -z "$work" ]; then
    echo "usage: run_runtime_mutation.sh --gcc RISCV_GCC --work DIR" \
         "[--source DIR] [--cc HOST_CC]" >&2
    exit 2
fi

task_c="$source/os/task.c"
# 77 means "a dependency this machine does not have", and nothing else: the
# cross toolchain is the only thing outside the repository here. os/task.c is
# the runtime under test and the runner is this repository's own driver, so a
# missing one is a hard failure. Deleting the implementation must never be a way
# to a green suite: CTest records 77 as a skip, and this check exists to prove
# that the runtime's own scenarios detect a broken runtime - it cannot do that
# from a skip.
if [ ! -x "$gcc" ] && ! command -v "$gcc" > /dev/null 2>&1; then
    echo "SKIP: no cross toolchain at '$gcc'"
    exit 77
fi
missing=0
for required in "$task_c" "$source/os/task.h" "$source/os/task_switch.S" \
                "$source/os/trap_entry.S" "$script_dir/run_runtime.sh"; do
    if [ ! -e "$required" ]; then
        echo "FAIL the source under test is missing: $required"
        missing=1
    fi
done
[ "$missing" -eq 0 ] || exit 1

# The scenarios are the test; the mutation run only feeds them another task.c.
scratch="$work/run.$$"
rm -rf "$scratch"
mkdir -p "$scratch/mutants"

echo "runtime mutation: task.c=$task_c scratch=$scratch"

# The six defects of SPEC group C.
#
#   1. the predicate is checked with interrupts enabled, outside the critical
#      section: the window between the answer and the registration is real;
#   2. the interrupt handler calls the scheduler, so a context switch happens
#      in interrupt context;
#   3. the source is completed before the device is serviced: the level is still
#      asserted, so completion re-pends it immediately;
#   4. the saved interrupt state is not restored after the switch, it is forced
#      off;
#   5. the waiter is registered after the task is blocked, and the restore sits
#      between the two: the window is outside the critical section;
#   6. the wake marks the waiter runnable but leaves the slot behind, so the
#      next wait on that event sees a stale waiter.
mutants=(predicate-check-outside-critical-section isr-calls-scheduler \
         complete-before-service restore-after-switch-clears \
         waiter-registered-after-blocking wake-leaves-waiter-slot)

plant() { # $1 = mutant name, $2 = copy of task.c
    case "$1" in
        predicate-check-outside-critical-section)
            # Step 2 first, with interrupts still on; then the critical section.
            sed -i 's#^    if (predicate(context)) {$#    if (0) {#' "$2"
            sed -i 's#^    const uint32_t previous = yan_os_irq_save();$#    if (predicate(context)) { return; } /* MUTANT: checked with interrupts on */\n    const uint32_t previous = yan_os_irq_save();#' "$2" ;;
        isr-calls-scheduler)
            # The address range keeps the change inside yan_os_wake: the
            # same state line also appears in spawn, where a scheduler call
            # would be a different defect.
            sed -i '/^static void yan_os_wake/,/^}/ s#^        task->state = YAN_OS_TASK_RUNNABLE;$#        task->state = YAN_OS_TASK_RUNNABLE;\n        yan_os_sched_reschedule(); /* MUTANT: the handler schedules */#' "$2" ;;
        complete-before-service)
            # Remove the completion from its place, then do it right after the
            # claim, while the device condition is still asserted.
            sed -i 's#^    yan_os_plic_complete(source);$#    /* MUTANT: completion removed from here */#' "$2"
            sed -i 's#^    if (source == YAN_OS_PLIC_SOURCE_TRANSPORT) {$#    yan_os_plic_complete(source); /* MUTANT: completed before servicing */\n    if (source == YAN_OS_PLIC_SOURCE_TRANSPORT) {#' "$2" ;;
        restore-after-switch-clears)
            sed -i 's#^    yan_os_irq_restore(previous);$#    yan_os_irq_disable(); /* MUTANT: forced off instead of the saved value */#' "$2" ;;
        waiter-registered-after-blocking)
            # Register after the state change, with the restore in between.
            sed -i 's#^    yan_os_irq_restore(previous);$#    yan_os_irq_restore(previous);\n    waiters[event] = current; /* MUTANT: registered after blocking */#' "$2"
            sed -i 's#^    waiters\[event\] = current;$#    /* MUTANT: registration removed from here */#' "$2" ;;
        wake-leaves-waiter-slot)
            sed -i 's#^    waiters\[event\] = NULL;$#    /* MUTANT: the waiter slot is left behind */#' "$2" ;;
        *) echo "run_runtime_mutation.sh: unknown mutant '$1'" >&2; return 1 ;;
    esac
    return 0
}

before="$(md5sum < "$task_c")"
survivors=0
detected=0
failed=0

# The gate is only meaningful if the *unmutated* runtime passes its own suite.
# Without this, "the mutant was caught" could just mean "this suite is red".
baseline_log="$scratch/baseline.log"
bash "$script_dir/run_runtime.sh" --source "$source" --gcc "$gcc" --cc "$cc" \
    --work "$scratch/baseline" --task-c "$task_c" > "$baseline_log" 2>&1
baseline_status=$?
baseline_verdict="$(grep -m 1 '^verdict:' "$baseline_log")"
if [ "$baseline_status" -ne 0 ] || grep -qE '^(FAIL \[scenario\]|INCONCLUSIVE|HARNESS)' "$baseline_log"; then
    echo "FAIL mutation baseline: the unmutated runtime does not pass its own suite,"
    echo "     so a caught mutant would prove nothing:"
    tail -n 10 "$baseline_log" | sed 's/^/    /'
    rm -rf "$scratch"
    exit 1
fi
echo "baseline: the unmutated runtime passes its own suite (${baseline_verdict#verdict: })"

for mutant in "${mutants[@]}"; do
    copy="$scratch/mutants/$mutant.c"
    rm -f "$copy"
    cp -a "$task_c" "$copy"
    digest_before="$(md5sum < "$copy")"
    plant "$mutant" "$copy" || { failed=1; continue; }
    digest_after="$(md5sum < "$copy")"
    if [ "$digest_before" = "$digest_after" ]; then
        echo "FAIL mutation $mutant was not applied (pattern mismatch)"
        survivors=$((survivors + 1))
        continue
    fi

    log="$scratch/$mutant.log"
    bash "$script_dir/run_runtime.sh" --source "$source" --gcc "$gcc" --cc "$cc" \
        --work "$scratch/$mutant" --task-c "$copy" > "$log" 2>&1
    status=$?

    if grep -q 'does not build' "$log"; then
        echo "FAIL mutation $mutant does not build, so it is not a defect:"
        grep -m 3 -A 3 'does not build' "$log" | sed 's/^/    /'
        survivors=$((survivors + 1))
        failed=1
        continue
    fi

    # Detection is exactly the FAIL [scenario] line: the Guest reported a
    # failure code, or a drive invariant on the Guest's own trace fired.
    # INCONCLUSIVE (the step budget ran out), HARNESS (the drive itself broke)
    # and every other way a run can stop are printed apart and counted here as
    # what they are - no detection.
    caught="$(grep '^FAIL \[scenario\]' "$log" | sed 's/^FAIL \[scenario\] //')"
    others="$(grep -E '^(INCONCLUSIVE|HARNESS) \[scenario\]' "$log")"
    if [ -n "$caught" ]; then
        echo "PASS mutation $mutant detected by:"
        printf '%s\n' "$caught" | sed 's/^/    /'
        if [ -n "$others" ]; then
            echo "    (also, not detections:)"
            printf '%s\n' "$others" | sed 's/^/    /'
        fi
        detected=$((detected + 1))
    else
        echo "FAIL mutation $mutant survived: no scenario failed an assertion"
        if [ -n "$others" ]; then
            printf '%s\n' "$others" | sed 's/^/    /'
        else
            echo "    every scenario reached its expected verdict"
        fi
        sed 's/^/    /' "$log" | tail -n 3
        survivors=$((survivors + 1))
    fi
done

# ---- classification control -------------------------------------------------
#
# The gate needs a negative control of its own: a defect that must *not* be
# counted. A pure hang is that case - the runtime never returns from wait, no
# assertion ever runs, and the run simply stops executing. It used to be
# reported as a detection because the driver never reached its injection point;
# now it has to come out as INCONCLUSIVE.
control="$scratch/mutants/pure-hang-control.c"
cp -a "$task_c" "$control"
control_before="$(md5sum < "$control")"
sed -i 's#^    yan_os_irq_restore(previous);$#    yan_os_irq_restore(previous);\n    for (volatile uint32_t spin = 0;; ++spin) { } /* CONTROL: a pure hang */#' "$control"
control_after="$(md5sum < "$control")"
if [ "$control_before" = "$control_after" ]; then
    echo "FAIL control pure-hang was not applied (pattern mismatch)"
    failed=1
else
    control_log="$scratch/pure-hang-control.log"
    bash "$script_dir/run_runtime.sh" --source "$source" --gcc "$gcc" --cc "$cc" \
        --work "$scratch/pure-hang-control" --task-c "$control" > "$control_log" 2>&1
    counted="$(grep -c '^FAIL \[scenario\]' "$control_log" || true)"
    hung="$(grep -c '^INCONCLUSIVE \[scenario\]' "$control_log" || true)"
    if [ "$counted" != "0" ]; then
        echo "FAIL control pure-hang was counted as a detection:"
        grep '^FAIL \[scenario\]' "$control_log" | sed 's/^/    /'
        failed=1
    elif [ "$hung" = "0" ]; then
        echo "FAIL control pure-hang never hung, so it proves nothing"
        sed 's/^/    /' "$control_log" | tail -n 3
        failed=1
    else
        echo "PASS control pure-hang: $hung scenario(s) hit the step budget" \
             "and none was counted as a detection"
    fi
fi

after="$(md5sum < "$task_c")"
if [ "$before" != "$after" ]; then
    echo "FAIL the mutation check modified $task_c"
    failed=1
else
    echo "mutation: every defect was planted in a copy; $task_c is unchanged (md5 $after)"
fi

echo "mutation: $detected detected, $survivors survived"
[ "$survivors" -eq 0 ] || failed=1

rm -rf "$scratch"
if [ "$failed" -ne 0 ]; then
    echo "FAIL the runtime mutation check reported at least one failure"
    exit 1
fi
echo "PASS every planted defect was detected by an assertion"
