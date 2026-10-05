#!/usr/bin/env bash
# Builds and runs the cooperative-runtime self-check:
# docs/specs/0019-cooperative-runtime.md.
#
# The Guest images are built here and run through the real CPU, Bus, PLIC and
# transport device in src/. One drive carries them: a runner built into the
# work directory from a generated source, which links the same Host libraries
# tools/yan_run.c links and adds the one thing 0019 IMPLE step 4 asks for - the
# ability to inject the device event at an exact instruction boundary.
#
# The injection point is defined by the Guest, not by the clock: the predicate
# bumps a counter (rt_probe.predicate_calls) as its last action, so the first
# boundary after that store is *inside wait's critical section* in a correct
# runtime and *before* the waiter exists in one that checks the predicate with
# interrupts enabled. That is the window the adversarial case is about.
#
# The drive also asserts two runtime invariants at every instruction boundary,
# reading only the Guest's own trace words:
#   * at most one task is executing (two set bits mean an interrupt handler
#     switched tasks);
#   * the transport source is never pended again without a new event (a
#     re-pend means complete ran before the device was serviced).
#
# Verdicts: PASS, FAIL (an assertion: the Guest reported a failure code or a
# drive invariant on the Guest's trace fired), INCONCLUSIVE (the run hit the
# step budget) and HARNESS (the drive itself broke). Only FAIL is evidence that
# a planted defect was caught; the other two are printed apart on purpose, and
# run_runtime_mutation.sh counts FAIL lines only.
#
# Exit codes: 0 every check that ran passed, 1 a check failed or a run was
# inconclusive/broken, 2 usage, 77 this machine has no cross toolchain. A
# missing runtime is a build failure, never a skip, and so is any other source
# this check compiles out of the repository.
#
# CTest: register it the way tests/guest/run_console.sh is registered
# (--source/--gcc/--work, SKIP_RETURN_CODE 77, TIMEOUT 300). This script runs
# in about 10 s.
set -u

script_dir="$(cd "$(dirname "$0")" && pwd)"
source="$(cd "$script_dir/../.." && pwd)"
gcc=""
work=""
cc="${CC:-cc}"
task_c=""

while [ $# -gt 0 ]; do
    case "$1" in
        --source) source="$2"; shift 2 ;;
        --gcc) gcc="$2"; shift 2 ;;
        --work) work="$2"; shift 2 ;;
        --cc) cc="$2"; shift 2 ;;
        --task-c) task_c="$2"; shift 2 ;;
        *) echo "run_runtime.sh: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

if [ -z "$gcc" ] || [ -z "$work" ]; then
    echo "usage: run_runtime.sh --gcc RISCV_GCC --work DIR [--source DIR]" \
         "[--cc HOST_CC] [--task-c FILE]" >&2
    exit 2
fi

guest_dir="$source/tests/guest"
os_dir="$source/os"
if [ -n "$task_c" ]; then
    # --task-c is how the mutation check builds an image against a planted
    # defect. A path that does not exist must not fall back to the real file:
    # that would silently test the unmutated runtime.
    [ -f "$task_c" ] || { echo "run_runtime.sh: --task-c '$task_c' does not exist" >&2; exit 2; }
else
    task_c="$os_dir/task.c"
fi

# 77 means "a dependency this machine does not have", and nothing else: the
# cross toolchain is the only thing outside the repository this check needs.
# Everything else it compiles is the thing under test, so a missing source is a
# hard failure - the runtime itself included, because the mutation check feeds
# the scenarios another copy of it and a missing one would leave that gate with
# nothing to mutate. Deleting an implementation must never be a way to a green
# suite: CTest records 77 as a skip, and a skip is not a pass.
if [ ! -x "$gcc" ] && ! command -v "$gcc" > /dev/null 2>&1; then
    echo "SKIP: no cross toolchain at '$gcc'"
    exit 77
fi
missing=0
for required in "$guest_dir/guest_lib.c" "$guest_dir/link.ld" \
                "$guest_dir/runtime_check.c" "$os_dir/console.c" \
                "$os_dir/console.h" "$os_dir/platform.h" "$os_dir/task.c" \
                "$os_dir/task.h" "$os_dir/task_switch.S" "$os_dir/trap_entry.S" \
                "$source/src/cpu.c" "$source/src/transport.c" \
                "$source/include/yan/machine.h" "$source/tools/host_file.c"; do
    if [ ! -e "$required" ]; then
        echo "FAIL the source under test is missing: $required"
        missing=1
    fi
done
[ "$missing" -eq 0 ] || exit 1

# A cross toolchain locates `as` and `ld` through its own directory.
gcc_dir="$(cd "$(dirname "$gcc")" && pwd)"
PATH="$gcc_dir:$PATH"
export PATH

mkdir -p "$work/images" "$work/drive" "$work/capture"
rm -f "$work"/images/*.elf 2>/dev/null

# ----------------------------------------------------------------- scenarios
#
# One row per scenario: the macro the Guest image is built with, the extra
# options the drive needs, the number of device events the drive must inject,
# and the outcome the Guest is expected to report. `pass` means tohost == 1;
# `panic:0x...` means the runtime's own failure code, which the host checks
# instead of a verdict from the Guest.
names=(predicate-true waiter-exists precondition-yield precondition-wait \
       precondition-exit wake second-wait idle context spawn no-switch)
macros=(PREDICATE_TRUE WAITER_EXISTS PRECONDITION_YIELD PRECONDITION_WAIT \
        PRECONDITION_EXIT WAKE SECOND_WAIT IDLE CONTEXT SPAWN NO_SWITCH)
options=("--inject-stage 1" "" "" "" "" "--inject-predicate 1" \
         "--inject-predicate 2" "--inject-stage 2 --hold-steps 3000" "" "" \
         "--inject-stage 2")
injections=(1 0 0 0 0 1 2 1 0 0 1)
expect=(pass panic:0x80000001 panic:0x80000002 panic:0x80000002 \
        panic:0x80000002 pass pass pass pass pass pass)
steps=(200000 60000 20000 20000 20000 200000 200000 200000 200000 60000 200000)
what=("wait returns without blocking because the event already happened" \
      "a second wait on the same event panics with its own failure code" \
      "yield with interrupts disabled panics (precondition)" \
      "wait with interrupts disabled panics (precondition)" \
      "exit with interrupts disabled panics (precondition)" \
      "the event is injected between the predicate and the block" \
      "two waits in a row: the first wake must empty the waiter slot" \
      "every task is blocked or gone: the scheduler idles until the event" \
      "two tasks alternate: stacks and callee-saved registers survive" \
      "eight slots, NO_SLOT, INVALID, and a returning entry" \
      "an interrupt returns to the task it interrupted")

# ------------------------------------------------------------------ driver
write_drive_source() {
    cat > "$work/drive/runtime_drive.c" <<'RUNTIME_DRIVE_EOF'
/* Scripted Host drive for the cooperative-runtime self-check.
 *
 * Generated into the work directory by tests/guest/run_runtime.sh and kept out
 * of the repository: the durable Host entry point for a Guest runtime is a
 * tool-side feature that does not exist yet (tools/yan_run.c cannot inject a
 * device interrupt at a chosen instruction). It links the same Host libraries
 * (RAM, Bus, CPU, CLINT, PLIC, UART, transport) and adds the three things 0019
 * IMPLE step 4 asks for:
 *
 *   - a device event injected at an exact instruction boundary, chosen by the
 *     Guest's own trace word rather than by a step count;
 *   - an assertion on the Guest's running-task trace, evaluated at every
 *     boundary: at most one task may be executing;
 *   - an assertion that the transport source is never pended again without a
 *     new event, which is what a complete-before-service handler does.
 *
 * Exit codes match tools/yan_run.c: 0 tohost PASS, 2 usage, 4 no termination,
 * 5 Host error, 6 the Guest reported a failure code, 7 the drive's own
 * invariant failed.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_file.h"
#include "yan/cpu.h"
#include "yan/image.h"
#include "yan/interrupt.h"
#include "yan/machine.h"
#include "yan/transport.h"
#include "yan/uart.h"

enum {
    EXIT_PASS = 0,
    EXIT_USAGE = 2,
    EXIT_NO_TERMINATION = 4,
    EXIT_HOST_ERROR = 5,
    EXIT_GUEST_FAILURE = 6,
    EXIT_DRIVE_ASSERTION = 7
};

/* The Guest's trace words (tests/guest/runtime_check.c). The drive runs on the
 * host and cannot include os/task.h - the Guest's pointer size is not the
 * host's - so it reads these plain 32-bit words by symbol name and nothing
 * else. */
enum {
    PROBE_MAGIC = 0,
    PROBE_PREDICATE_CALLS = 1,
    PROBE_RUNNING_BITS = 2,
    PROBE_WAKE_RETURNS = 3,
    PROBE_SWITCH_TRACES = 4,
    PROBE_STAGE = 5,
    PROBE_T_TICKS = 6,
    PROBE_STACK_MARK0 = 7,
    PROBE_STACK_MARK1 = 8,
    PROBE_WORDS = 9
};
#define PROBE_MAGIC_VALUE UINT32_C(0x59414e52) /* "YANR" */

#define RING_SIZE UINT32_C(8192)

typedef struct {
    unsigned delivered;
    FILE *capture;
} Backend;

static bool backend_tx_ready(void *context)
{
    (void)context;
    return true;
}

static void backend_tx_write(void *context, uint8_t byte)
{
    Backend *backend = context;
    ++backend->delivered;
    if (backend->capture != NULL) {
        fputc(byte, backend->capture);
    }
}

static int number(const char *text, uint64_t *value)
{
    char *end = NULL;
    *value = strtoull(text, &end, 0);
    return end != text && *end == '\0';
}

static unsigned popcount32(uint32_t value)
{
    unsigned count = 0;
    while (value != 0) {
        count += (unsigned)(value & 1u);
        value >>= 1;
    }
    return count;
}

static int read_probe(const YanBus *bus, uint32_t address, uint32_t *words)
{
    for (uint32_t index = 0; index < PROBE_WORDS; ++index) {
        uint32_t value = 0;
        if (yan_bus_read(bus, address + 4u * index, 4, &value).status != YAN_OK) {
            return 0;
        }
        words[index] = value;
    }
    return 1;
}

/* The host owns the byte: it writes it into the host-to-guest ring and tells
 * the device how far it got. The device then asserts its line, which the PLIC
 * turns into a pending source. Returns 0 on success. */
static int publish_event(YanTransport *transport, YanBus *bus, YanPlic *plic,
                         uint64_t index, uint64_t step)
{
    const uint32_t ring = transport->ring_base + transport->ring_size;
    const uint32_t slot = ring + transport->h2g_head;
    if (yan_bus_write(bus, slot, 1, (uint32_t)(0x40u + index)).status != YAN_OK ||
        yan_transport_host_publish(transport, 1) != YAN_OK) {
        fprintf(stderr, "runtime_drive: the host could not publish an event\n");
        return 0;
    }
    yan_plic_set_level(plic, YAN_MACHINE_PLIC_SOURCE_TRANSPORT,
                       yan_transport_pending(transport));
    fprintf(stderr, "note [drive] injected event %" PRIu64
            " at instruction %" PRIu64 "\n", index, step);
    return 1;
}

/* A doorbell handler. The device counts as usable, and HOST_READY as asserted,
 * only while one is registered: without it the guest is told the channel is
 * not configured and must not touch the rings. The drive has nothing to do
 * when the bell rings, but the channel needs a listener. */
static void transport_notify(void *context)
{
    (void)context;
}

int main(int argc, char **argv)
{
    const char *image_path = NULL;
    const char *capture_path = NULL;
    const char *scenario = "?";
    uint64_t base = UINT32_C(0x80000000);
    uint64_t ram_size = 16u * 1024u * 1024u;
    uint64_t max_steps = 400000;
    uint64_t inject_stage = 0;
    uint64_t inject_predicate = 0;
    uint64_t hold_steps = 0;

    for (int i = 1; i < argc; ++i) {
        uint64_t value = 0;
        if (strcmp(argv[i], "--image") == 0 && i + 1 < argc) image_path = argv[++i];
        else if (strcmp(argv[i], "--capture") == 0 && i + 1 < argc) capture_path = argv[++i];
        else if (strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) scenario = argv[++i];
        else if (strcmp(argv[i], "--base") == 0 && i + 1 < argc) {
            if (!number(argv[++i], &value)) return EXIT_USAGE;
            base = value;
        } else if (strcmp(argv[i], "--ram") == 0 && i + 1 < argc) {
            if (!number(argv[++i], &value)) return EXIT_USAGE;
            ram_size = value;
        } else if (strcmp(argv[i], "--max-steps") == 0 && i + 1 < argc) {
            if (!number(argv[++i], &value)) return EXIT_USAGE;
            max_steps = value;
        } else if (strcmp(argv[i], "--inject-stage") == 0 && i + 1 < argc) {
            if (!number(argv[++i], &value)) return EXIT_USAGE;
            inject_stage = value;
        } else if (strcmp(argv[i], "--inject-predicate") == 0 && i + 1 < argc) {
            if (!number(argv[++i], &value)) return EXIT_USAGE;
            inject_predicate = value;
        } else if (strcmp(argv[i], "--hold-steps") == 0 && i + 1 < argc) {
            if (!number(argv[++i], &value)) return EXIT_USAGE;
            hold_steps = value;
        } else {
            return EXIT_USAGE;
        }
    }
    if (image_path == NULL || ram_size == 0) {
        return EXIT_USAGE;
    }

    YanRam ram = {0};
    YanBus bus = {0};
    YanCpu cpu = {0};
    YanClint clint = {0};
    YanPlic plic = {0};
    YanUart uart = {0};
    YanTransport transport = {0};
    YanImageInfo info = {0};
    uint8_t *image = NULL;
    size_t image_size = 0;
    uint32_t tohost = 0;
    uint32_t status_word = 0;
    int tohost_known = 0;
    uint32_t probe_address = 0;
    int probe_known = 0;
    uint32_t probe[PROBE_WORDS] = {0};
    uint64_t injections = 0;
    uint64_t predicate_seen = 0;
    uint64_t pending_edges = 0;
    int pending_before = 0;
    uint64_t stage_fired = 0;
    uint64_t hold_until = 0;
    int result = EXIT_HOST_ERROR;
    int stopped = 0;

    Backend backend = {0, NULL};
    const YanUartTerminal terminal = {
        .context = &backend, .tx_ready = backend_tx_ready, .tx_write = backend_tx_write};

    if (capture_path != NULL) {
        backend.capture = fopen(capture_path, "wb");
        if (backend.capture == NULL) {
            fprintf(stderr, "runtime_drive: cannot write '%s'\n", capture_path);
            return EXIT_HOST_ERROR;
        }
    }
    if (yan_ram_init(&ram, (uint32_t)ram_size) != YAN_OK ||
        yan_bus_init(&bus, &ram, (uint32_t)base) != YAN_OK) {
        goto done;
    }
    yan_clint_reset(&clint);
    yan_plic_reset(&plic);
    yan_uart_reset(&uart);
    yan_transport_reset(&transport);
    bus.clint = &clint;
    bus.plic = &plic;
    bus.uart = &uart;
    bus.transport = &transport;
    if (yan_uart_set_terminal(&uart, &terminal) != YAN_OK) {
        goto done;
    }
    /* The rings go where yan_run --disk places them, so an image built for
     * this drive sees the same geometry the tool would give it. */
    if (yan_transport_configure(&transport,
                                (uint32_t)base + (uint32_t)ram_size - 2u * RING_SIZE,
                                RING_SIZE, (uint32_t)base, (uint32_t)ram_size) != YAN_OK) {
        fprintf(stderr, "runtime_drive: cannot place the transport rings\n");
        goto done;
    }
    yan_transport_set_notify(&transport, transport_notify, NULL);
    if (!yan_transport_pending(&transport) &&
        (yan_transport_read(&transport, YAN_TRANSPORT_STATUS, &status_word) != YAN_OK ||
         (status_word & YAN_TRANSPORT_STATUS_HOST_READY) == 0)) {
        fprintf(stderr, "runtime_drive: the channel did not report HOST_READY\n");
        goto done;
    }
    image = yan_host_read_file(image_path, &image_size);
    if (image == NULL) {
        fprintf(stderr, "runtime_drive: cannot read '%s'\n", image_path);
        goto done;
    }
    if (yan_image_load_elf(&ram, (uint32_t)base, image, image_size, &info) != YAN_OK) {
        fprintf(stderr, "runtime_drive: '%s' is not a loadable RV32 ELF image\n",
                image_path);
        goto done;
    }
    if (yan_cpu_reset(&cpu, info.entry) != YAN_OK) {
        goto done;
    }
    if (yan_image_find_symbol(image, image_size, "tohost", &tohost) == YAN_OK) {
        tohost_known = 1;
    }
    if (yan_image_find_symbol(image, image_size, "rt_probe", &probe_address) == YAN_OK) {
        probe_known = 1;
    }

    for (uint64_t step = 0; step < max_steps; ++step) {
        /* Device lines are level driven: every boundary re-reads the device and
         * drives its source, exactly as yan_machine_sample_devices does. That
         * is what makes "the source re-pended after complete" observable. */
        yan_plic_set_level(&plic, YAN_MACHINE_PLIC_SOURCE_TRANSPORT,
                           yan_transport_pending(&transport));

        if (probe_known) {
            if (!read_probe(&bus, probe_address, probe)) {
                fprintf(stderr, "runtime_drive: cannot read the Guest trace\n");
                goto done;
            }
            /* The structure reads zero until the Guest has configured the
             * platform, so only a *different* magic is an error. */
            if (probe[PROBE_MAGIC] != 0 && probe[PROBE_MAGIC] != PROBE_MAGIC_VALUE) {
                fprintf(stderr, "runtime_drive: 'rt_probe' is not the expected"
                        " structure (magic %08" PRIx32 ")\n", probe[PROBE_MAGIC]);
                goto done;
            }
        }

        /* ---- injection points ------------------------------------------- */
        if (inject_predicate != 0 && probe_known &&
            probe[PROBE_PREDICATE_CALLS] > predicate_seen) {
            predicate_seen = probe[PROBE_PREDICATE_CALLS];
            if (injections < inject_predicate) {
                if (!publish_event(&transport, &bus, &plic, ++injections, step)) {
                    goto done;
                }
            }
        }
        if (inject_stage != 0 && probe_known && stage_fired == 0 &&
            probe[PROBE_STAGE] == inject_stage) {
            stage_fired = 1;
            if (hold_steps == 0) {
                if (!publish_event(&transport, &bus, &plic, ++injections, step)) {
                    goto done;
                }
            } else {
                hold_until = step + hold_steps;
            }
        }
        if (hold_until != 0 && step >= hold_until) {
            if (!publish_event(&transport, &bus, &plic, ++injections, step)) {
                goto done;
            }
            hold_until = 0;
        }

        /* ---- invariants -------------------------------------------------- */
        if (probe_known && popcount32(probe[PROBE_RUNNING_BITS]) > 1) {
            fprintf(stderr, "FAIL [drive] %s: %u tasks were executing at once"
                    " (running_bits=%08" PRIx32 "); a context switch happened"
                    " outside the switch points\n", scenario,
                    popcount32(probe[PROBE_RUNNING_BITS]), probe[PROBE_RUNNING_BITS]);
            result = EXIT_DRIVE_ASSERTION;
            goto done;
        }
        {
            const uint32_t source_bit = UINT32_C(1) << YAN_MACHINE_PLIC_SOURCE_TRANSPORT;
            const int pending_now = (plic.pending & source_bit) != 0;
            if (pending_now && !pending_before) {
                ++pending_edges;
            }
            pending_before = pending_now;
            if (pending_edges > injections) {
                fprintf(stderr, "FAIL [drive] %s: the transport source was pended"
                        " again without a new event (%" PRIu64 " pending episodes,"
                        " %" PRIu64 " injected); complete ran before the device was"
                        " serviced\n", scenario, pending_edges, injections);
                result = EXIT_DRIVE_ASSERTION;
                goto done;
            }
        }

        /* ---- one instruction --------------------------------------------- */
        yan_clint_tick(&clint, 1);
        /* One instruction, exactly as yan_machine_step runs it. A fetch or
         * access fault is the Guest's trap to take - yan_cpu_step enters the
         * vector - so the host does not pre-fetch and does not turn a Guest
         * fault into a host error. Only a status outside the machine's own
         * vocabulary means the harness itself broke. */
        const YanStatus stepped = yan_cpu_step(&cpu, &bus);
        if (stepped != YAN_OK && stepped != YAN_TRAP) {
            fprintf(stderr, "runtime_drive: step failed at pc = %08" PRIx32
                    " (status %d)\n", cpu.pc, (int)stepped);
            goto done;
        }

        /* ---- the Guest's verdict ----------------------------------------- */
        if (tohost_known) {
            uint32_t value = 0;
            if (yan_bus_read(&bus, tohost, 4, &value).status == YAN_OK && value != 0) {
                if (hold_until != 0) {
                    /* The scheduler was supposed to keep running with every
                     * task blocked; reaching tohost means it did not. */
                    fprintf(stderr, "FAIL [drive] %s: the scheduler left its loop"
                            " while every task was blocked (tohost=%" PRIu32 ")\n",
                            scenario, value);
                    result = EXIT_DRIVE_ASSERTION;
                    goto done;
                }
                if (value != 1) {
                    fprintf(stderr, "runtime_drive: the Guest reported failure code"
                            " %" PRIu32 " after %" PRIu64 " instructions\n",
                            value, step + 1);
                } else {
                    fprintf(stderr, "runtime_drive: tohost PASS after %" PRIu64
                            " instructions, %u byte(s) delivered\n",
                            step + 1, backend.delivered);
                }
                result = value == 1 ? EXIT_PASS : EXIT_GUEST_FAILURE;
                stopped = 1;
                break;
            }
        }
    }
    if (!stopped && result == EXIT_HOST_ERROR) {
        fprintf(stderr, "runtime_drive: stopped after %" PRIu64
                " steps without reaching tohost\n", max_steps);
        result = EXIT_NO_TERMINATION;
    }
done:
    if (backend.capture != NULL) {
        fclose(backend.capture);
    }
    free(image);
    yan_ram_destroy(&ram);
    return result;
}
RUNTIME_DRIVE_EOF
}

build_scripted_drive() {
    write_drive_source
    "$cc" -O1 -std=c17 -I "$source/include" -I "$source/tools" \
        -o "$work/drive/runtime_drive" "$work/drive/runtime_drive.c" \
        "$source/tools/host_file.c" "$source"/src/*.c > "$work/drive/build.log" 2>&1
}

# ------------------------------------------------------------------ image build
build_image() { # $1 = scenario macro, $2 = task.c under test, $3 = output elf
    "$gcc" -march=rv32im -mabi=ilp32 -mcmodel=medany -static -nostdlib \
        -nostartfiles -ffreestanding -fno-builtin -fno-stack-protector \
        -Wall -Wextra -O2 -I "$guest_dir" -I "$os_dir" \
        -DYAN_RT_SCENARIO="YAN_RT_SCENARIO_$1" \
        -T "$guest_dir/link.ld" \
        "$os_dir/trap_entry.S" "$2" "$os_dir/task_switch.S" "$os_dir/console.c" \
        "$guest_dir/guest_lib.c" "$guest_dir/runtime_check.c" \
        -Wl,--build-id=none -o "$3"
}

# ------------------------------------------------------------------ helpers
reported_code() { # $1 = log file; prints the failure code the runner named
    sed -n 's/.*reported failure code \([0-9][0-9]*\) .*/\1/p' "$1" | head -n 1
}

panic_code() { # $1 = hex like 0x80000001; prints its decimal form
    printf '%d' "$1"
}

# How a scenario run ended, from the drive's exit code alone:
#
#   assertion     the Guest reported a failure code (6) or a drive invariant on
#                 the Guest's own trace failed (7). Only this kind is evidence
#                 that a planted defect was caught.
#   inconclusive  the run hit the step budget without a verdict (4): a timeout
#                 says nothing about the runtime.
#   harness       the drive itself failed (5), died on a signal (128+N) or
#                 exited for any other reason. A broken harness is not a
#                 detection either.
#   none          the Guest reached its final check (0).
#
# A mutation run classifies on this and nothing else, so "the scenario did not
# get as far as the check" can never be reported as a caught defect.
failure_kind() { # $1 = drive exit status
    case "$1" in
        0) echo none ;;
        6) echo assertion ;;
        7) echo assertion ;;
        4) echo inconclusive ;;
        *) echo harness ;;
    esac
}

# $1 = scenario index, $2 = drive exit status, $3 = log file, $4 = capture file
# Prints the verdict and returns nonzero unless the scenario reached the verdict
# it was supposed to.
report_case() {
    local index="$1" status="$2" log="$3" capture="$4"
    local name="${names[index]}" code expected injected kind
    code="$(reported_code "$log")"
    injected="$(grep -c 'note \[drive\] injected event' "$log" || true)"
    kind="$(failure_kind "$status")"

    # A count mismatch is never the verdict. It is a note here: on a run that
    # terminated with an assertion it explains what the assertion interrupted,
    # and on a run that did not terminate it is a symptom, not a detection.
    local injection_note=""
    if [ "$injected" != "${injections[index]}" ]; then
        injection_note="the drive injected $injected event(s), ${injections[index]} expected"
    fi

    case "$kind" in
        none)
            if [ "${expect[index]}" != "pass" ]; then
                echo "FAIL [scenario] ${name}: the Guest reported PASS where" \
                     "${expect[index]} was expected (${what[index]})"
                return 1
            fi
            if [ -n "$injection_note" ]; then
                # The Guest says it finished, but the check this scenario exists
                # for never ran: that is a broken test, not a caught defect.
                echo "HARNESS [scenario] ${name}: the Guest reached its final" \
                     "check, but $injection_note (${what[index]})"
                return 1
            fi
            echo "PASS [scenario] ${name}: tohost PASS, ${injected} event(s) injected"
            return 0 ;;
        assertion)
            if [ "$status" -eq 7 ]; then
                echo "FAIL [scenario] ${name}: the drive's invariant failed" \
                     "(${what[index]})"
                grep 'FAIL \[drive\]' "$log" | sed 's/^/    /'
                [ -z "$injection_note" ] || echo "    note: $injection_note"
                return 1
            fi
            if [ -z "$code" ]; then
                echo "HARNESS [scenario] ${name}: the drive reported a Guest" \
                     "failure without a code"
                sed 's/^/    /' "$log" | tail -n 3
                return 1
            fi
            if [ "${expect[index]}" = "pass" ]; then
                echo "FAIL [scenario] ${name}: the Guest reported failure code" \
                     "$code where it had to reach its final check (${what[index]})"
                [ -z "$injection_note" ] || echo "    note: $injection_note"
                return 1
            fi
            expected="$(panic_code "${expect[index]#panic:}")"
            if [ "$code" != "$expected" ]; then
                echo "FAIL [scenario] ${name}: the Guest reported $code where" \
                     "${expect[index]} was expected (${what[index]})"
                [ -z "$injection_note" ] || echo "    note: $injection_note"
                return 1
            fi
            # The panic diagnostic is part of the contract: with a terminal
            # attached a line must have been printed before the failure code.
            if ! grep -q 'yan_os: panic' "$capture"; then
                echo "FAIL [scenario] ${name}: the panic printed no diagnostic" \
                     "on the console"
                return 1
            fi
            echo "PASS [scenario] ${name}: ${expect[index]} reported with a" \
                 "diagnostic on the console"
            return 0 ;;
        inconclusive)
            echo "INCONCLUSIVE [scenario] ${name}: no termination within" \
                 "${steps[index]} instructions" \
                 "$([ -n "$injection_note" ] && printf '(%s)' "$injection_note")" \
                 "- a timeout is not a detection (${what[index]})"
            sed 's/^/    /' "$log" | tail -n 3
            return 1 ;;
        *)
            echo "HARNESS [scenario] ${name}: the Host drive failed (exit" \
                 "$status) - a broken harness is not a detection (${what[index]})"
            sed 's/^/    /' "$log" | tail -n 3
            return 1 ;;
    esac
}

# ------------------------------------------------------------------ main flow
echo "runtime: gcc=$gcc task.c=$task_c work=$work"

unrunnable=0
failed=0
verdicts=""

count_kind() { # $1 = pass | assertion | inconclusive | harness
    local count=0 entry
    for entry in $verdicts; do
        [ "$entry" = "$1" ] && count=$((count + 1))
    done
    printf '%s' "$count"
}

if build_scripted_drive; then
    :
else
    echo "FAIL the runtime drive does not build from $source/src and $source/tools:"
    tail -n 10 "$work/drive/build.log" | sed 's/^/    /'
    failed=1
fi

# Every scenario runs even after one of them fails: a mutation run needs the
# whole set of verdicts to show which check caught the planted defect.
for index in "${!names[@]}"; do
    elf="$work/images/${names[index]}.elf"
    if ! build_image "${macros[index]}" "$task_c" "$elf" \
            > "$work/images/${names[index]}.log" 2>&1; then
        echo "FAIL the Guest image for ${names[index]} does not build:"
        tail -n 10 "$work/images/${names[index]}.log" | sed 's/^/    /'
        failed=1
        continue
    fi
    capture="$work/capture/${names[index]}.txt"
    log="$work/capture/${names[index]}.log"
    read -r -a extra <<< "${options[index]}"
    "$work/drive/runtime_drive" --image "$elf" --scenario "${names[index]}" \
        --capture "$capture" --max-steps "${steps[index]}" "${extra[@]}" \
        > "$log" 2>&1
    status=$?
    # Record the verdict from the line report_case printed: the run's exit code
    # alone cannot tell "the expected panic was reported" from "an assertion
    # failed", and the summary has to.
    report="$(report_case "$index" "$status" "$log" "$capture")"
    report_status=$?
    printf '%s\n' "$report"
    case "$report" in
        PASS*) verdicts="$verdicts pass" ;;
        FAIL*) verdicts="$verdicts assertion" ;;
        INCONCLUSIVE*) verdicts="$verdicts inconclusive" ;;
        *) verdicts="$verdicts harness" ;;
    esac
    [ "$report_status" -eq 0 ] || failed=1
done

# ---- verdict ----------------------------------------------------------------
echo "verdict: $(count_kind pass) passed, $(count_kind assertion) assertion" \
     "failure(s), $(count_kind inconclusive) inconclusive, $(count_kind harness)" \
     "harness error(s), out of ${#names[@]}"
if [ "$failed" -ne 0 ]; then
    echo "FAIL the cooperative-runtime check reported at least one failure"
    exit 1
fi
if [ "$unrunnable" -ne 0 ]; then
    # Nothing increments this today; it is kept as a guard. A check that could
    # not run is a failure here, not a 77: 77 is reserved for a dependency that
    # lives outside the repository, and printing PASS over a partial run would
    # let CTest record it as green.
    echo "FAIL $unrunnable check(s) could not run (see above)"
    exit 1
fi
echo "PASS every runtime check that could run passed"
