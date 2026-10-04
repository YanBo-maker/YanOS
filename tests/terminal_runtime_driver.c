/* Host driver for the real UART receive window and the production faults.
 *
 * This is the Host half of the windows 0022 needs and of the fault surface it
 * promises. It links the real machine library (RAM, Bus, CPU, CLINT, PLIC,
 * UART, transport) and the real block service (tools/host_block.c), so the
 * Guest image runs over the production devices and never over a fake line
 * reader.
 *
 * Two modes share one step loop:
 *
 *   fixture     tests/guest/terminal_wait_check.c. The driver injects the UART
 *               bytes at a chosen instruction window, watches the UART
 *               registers, the PLIC in-service register and the Guest task
 *               states at every boundary, and then re-reads the Guest probe by
 *               symbol before it accepts tohost 1.
 *
 *   production  apps/yanfs_terminal over a memory-backed image and a real
 *               terminal backend whose tx_ready can be turned off after a
 *               chosen byte, or whose block replies can be faulted.
 *
 * Private task-array layout (observation only, not an ABI). os/task.h fixes
 * YAN_OS_TASK_CTX_BYTES = 56 (ra, sp, s0-s11), then the task adds entry (4)
 * and arg (4), so state sits at 56 + 4 + 4 = 64. The 16-aligned 4096-byte
 * stack pushes the struct size to 80 + 4096 = 4176. The fixture asserts the
 * same 56/4096/8 constants, so a change on either side fails a build; the
 * static assertions below pin the arithmetic.
 *
 * Exit codes match tools/yan_run.c: 0 PASS, 2 usage, 4 no termination,
 * 5 Host error, 6 the Guest reported failure, 7 a drive invariant failed. A
 * status file records the exact tohost and the observations so a later gate
 * does not have to re-parse stderr.
 */
#define _POSIX_C_SOURCE 200809L

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_block.h"
#include "host_file.h"
#include "yan/cpu.h"
#include "yan/image.h"
#include "yan/interrupt.h"
#include "yan/machine.h"
#include "yan/ram.h"
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

#define RING_SIZE UINT32_C(8192)
#define UART_SOURCE_BIT (UINT32_C(1) << YAN_MACHINE_PLIC_SOURCE_UART)
#define UART_RX_ENABLE UINT32_C(1)
#define OUTPUT_CAPACITY 65536u

#define TASK_STATE_OFFSET UINT32_C(64)
#define TASK_STRIDE UINT32_C(4176)
#define TASK_MAX UINT32_C(8)
#define TASK_STATE_RUNNABLE UINT32_C(1)
#define TASK_STATE_RUNNING UINT32_C(2)
#define TASK_STATE_BLOCKED UINT32_C(3)
#define TASKS_SYMBOL "tasks"
#define READER_SLOT UINT32_C(1) /* fixture: counter spawned first, reader second */

_Static_assert(TASK_STATE_OFFSET == (56u + 4u + 4u),
               "state offset is context + entry + arg");
_Static_assert(TASK_STRIDE == (80u + 4096u),
               "stride is the 16-aligned header plus one stack");

/* tests/guest/terminal_wait_check.c, word for word. */
enum {
    TW_MAGIC = 0,
    TW_SCENARIO = 1,
    TW_READER_STAGE = 2,
    TW_TICKS = 3,
    TW_CHECKS_DONE = 4,
    TW_SECOND_OPEN = 5,
    TW_SECOND_CTX_ZERO = 6,
    TW_WRONG_NEXT = 7,
    TW_WRONG_CLOSE = 8,
    TW_CTRL_BEFORE = 9,
    TW_CTRL_AFTER = 10,
    TW_BUSY_CLOSE = 11,
    TW_REJECT_RESULT = 12,
    TW_REJECT_LENGTH = 13,
    TW_LINE0_LENGTH = 14,
    TW_LINE1_LENGTH = 15,
    TW_LINE0_WORD = 16,
    TW_LINE1_WORD = 17,
    TW_WORDS = 18
};
#define TW_MAGIC_VALUE UINT32_C(0x54574c4b)
#define TW_LINE_OK 0u
#define TW_LINE_INVALID_INPUT 2u
#define TW_LINE_INVALID 4u
#define TW_LINE_BUSY 5u

enum { MODE_FIXTURE = 1, MODE_PRODUCTION = 2 };
enum {
    WINDOW_READY = 1,
    WINDOW_PREDICATE = 2,
    WINDOW_BLOCKED = 3,
    WINDOW_WAKE = 4,
    WINDOW_REJECT = 5
};
enum { FAULT_NONE = 0, FAULT_IO = 1, FAULT_PROTOCOL = 2 };

/* Known Guest-reported codes. Anything else - a runtime panic (0x80000000 |
 * reason), a trap, or an application code with an unknown reason - is a harness
 * error, never a named owner failure. */
#define APP_FAIL_TOP UINT32_C(0x71000000)
#define APP_REASON_LAST 17u
#define FIXTURE_FAIL_TOP UINT32_C(0x72000000)
#define FIXTURE_FAIL_LAST 17u

typedef struct {
    FILE *capture;
    uint32_t delivered;
    uint32_t tx_fault_at;
    bool tx_failed;
    const uint8_t *feed;
    size_t feed_length;
    size_t feed_at;
    bool feed_started;
    bool feed_owned;
    char out[OUTPUT_CAPACITY];
    size_t out_length;
    uint32_t putc_entries_after_fail;
    uint32_t console_entries_after_fail;
} Backend;

typedef struct {
    uint32_t magic, scenario, reader_stage, ticks, checks_done;
    uint32_t second_open_result, second_ctx_zero, wrong_next_result;
    uint32_t wrong_close_result, ctrl_before, ctrl_after, busy_close_result;
    uint32_t reject_result, reject_length, line0_length, line1_length;
    uint32_t line0_word, line1_word;
} TwProbe;

typedef struct {
    uint32_t control, irq_status, in_service;
    bool rx_available;
    int running_slot;
    int running_count;
} Snapshot;

typedef struct {
    int mode, window, fault;
    uint32_t corrupt_response, tx_fault_at;
    Backend backend;
    YanHostBlock block;
    uint8_t *storage;
    uint64_t capacity;
    bool fault_armed;
    bool protocol_fired;
    const uint8_t *baseline;
    size_t baseline_length;
    bool baseline_loaded;
    uint32_t tohost_addr, probe_addr, tasks_addr, predicate_addr;
    uint32_t putc_addr, console_addr;
    bool tohost_known, probe_known, tasks_known, predicate_known;
    bool putc_known, console_known;
    uint32_t predicate_ra, predicate_entries;
    bool predicate_entered, window_fired, observed_blocked, wake_seen, rearm_seen;
    size_t line2_start;
    bool io_error;
    uint32_t isr_count;
    uint32_t tohost_value;
    bool tohost_seen;
} Drive;

/* ------------------------------------------------------------- UART backend */

static void note_output(Backend *backend, uint8_t byte)
{
    if (backend->out_length + 1u < OUTPUT_CAPACITY) {
        backend->out[backend->out_length] = (char)byte;
        ++backend->out_length;
        backend->out[backend->out_length] = '\0';
    }
}

static bool backend_tx_ready(void *context)
{
    Backend *backend = context;
    return !backend->tx_failed;
}

static void backend_tx_write(void *context, uint8_t byte)
{
    Backend *backend = context;
    ++backend->delivered;
    note_output(backend, byte);
    if (backend->capture != NULL) {
        (void)fputc(byte, backend->capture);
    }
    if (backend->tx_fault_at != 0u && backend->delivered >= backend->tx_fault_at) {
        backend->tx_failed = true;
    }
}

static void transport_notify(void *context)
{
    (void)context;
}

/* --------------------------------------------------------------- utilities */

static int number(const char *text, uint64_t *value)
{
    char *end = NULL;
    *value = strtoull(text, &end, 0);
    return end != text && *end == '\0';
}

static int parse_hex(const char *text, uint32_t *value)
{
    char *end = NULL;
    *value = (uint32_t)strtoul(text, &end, 16);
    return end != text && *end == '\0';
}

static int bus_read32(const YanBus *bus, uint32_t address, uint32_t *value)
{
    return yan_bus_read(bus, address, 4, value).status == YAN_OK;
}

static uint32_t bus_word(const YanBus *bus, uint32_t address)
{
    uint32_t value = 0;
    (void)bus_read32(bus, address, &value);
    return value;
}

static int read_probe(const YanBus *bus, uint32_t address, TwProbe *probe)
{
    uint32_t *words = (uint32_t *)probe;
    for (uint32_t index = 0; index < TW_WORDS; ++index) {
        if (!bus_read32(bus, address + 4u * index, &words[index])) {
            return 0;
        }
    }
    return 1;
}

static int task_state_of(Drive *drive, const YanMachine *machine, uint32_t slot,
                         uint32_t *state)
{
    if (!drive->tasks_known) {
        return 0;
    }
    if (yan_bus_read(&machine->bus,
                     drive->tasks_addr + slot * TASK_STRIDE + TASK_STATE_OFFSET,
                     4, state).status != YAN_OK) {
        drive->io_error = true;
        return 0;
    }
    return 1;
}

static int snapshot_machine(Drive *drive, const YanMachine *machine,
                            Snapshot *snapshot)
{
    snapshot->control = machine->uart.control;
    snapshot->irq_status = machine->uart.irq_status;
    snapshot->rx_available = machine->uart.rx_available;
    snapshot->in_service = machine->plic.in_service_m;
    snapshot->running_slot = -1;
    snapshot->running_count = 0;
    if (!drive->tasks_known) {
        return 1;
    }
    for (uint32_t slot = 0; slot < TASK_MAX; ++slot) {
        uint32_t state = 0;
        if (!task_state_of(drive, machine, slot, &state)) {
            return 0;
        }
        if (state == TASK_STATE_RUNNING) {
            snapshot->running_slot = (int)slot;
            ++snapshot->running_count;
        }
    }
    return 1;
}

/* --------------------------------------------------------------- assertions */

#define DRIVE_FAIL(...) do { \
    fprintf(stderr, ":FAIL: [drive] " __VA_ARGS__); \
    fprintf(stderr, "\n"); \
    return 0; \
} while (0)

static int check_boundary(Drive *drive, const YanMachine *machine,
                          const Snapshot *before, const Snapshot *after,
                          uint64_t step)
{
    if (after->running_count > 1) {
        DRIVE_FAIL("step %" PRIu64 ": %d tasks were RUNNING at once (a context"
                   " switch happened inside an interrupt)", step,
                   after->running_count);
    }
    const bool active_before = (before->in_service & UART_SOURCE_BIT) != 0u;
    const bool active_after = (after->in_service & UART_SOURCE_BIT) != 0u;
    if ((active_before || active_after) &&
        before->running_slot != after->running_slot) {
        DRIVE_FAIL("step %" PRIu64 ": the running task changed during UART"
                   " service (%d -> %d)", step, before->running_slot,
                   after->running_slot);
    }
    if (active_before && before->rx_available != after->rx_available) {
        DRIVE_FAIL("step %" PRIu64 ": the UART handler consumed RXDATA while the"
                   " source was in service", step);
    }
    if (active_before && !active_after) {
        if ((after->control & UART_RX_ENABLE) != 0u) {
            DRIVE_FAIL("step %" PRIu64 ": completed UART service with"
                       " RX_IRQ_ENABLE still set (control=%08" PRIx32 ")",
                       step, after->control);
        }
        if ((after->irq_status & UART_RX_ENABLE) != 0u) {
            DRIVE_FAIL("step %" PRIu64 ": completed UART service with the"
                       " arrival latch still set", step);
        }
        if ((machine->plic.pending & UART_SOURCE_BIT) != 0u) {
            DRIVE_FAIL("step %" PRIu64 ": the UART source re-pended at"
                       " completion", step);
        }
    }
    if (!active_before && active_after) {
        ++drive->isr_count;
    }
    return 1;
}

/* ------------------------------------------------------------------ feeding */

static int feed_throttled(Drive *drive, YanMachine *machine)
{
    if (!drive->backend.feed_started ||
        drive->backend.feed_at >= drive->backend.feed_length) {
        return 1;
    }
    if (machine->uart.rx_available) {
        return 1; /* single receive cell; wait for the Guest to take it */
    }
    if (drive->line2_start != 0 && drive->backend.feed_at == drive->line2_start) {
        uint32_t reader = 0;
        if (!task_state_of(drive, machine, READER_SLOT, &reader)) {
            return 0;
        }
        if (reader == TASK_STATE_BLOCKED &&
            (machine->uart.control & UART_RX_ENABLE) == 0u) {
            /* Blocked and waiting, but the receive interrupt is not armed: the
             * next byte could never wake it. Fail now, not by timeout. */
            DRIVE_FAIL("the second line arrived while the reader was BLOCKED"
                       " with RX_IRQ_ENABLE clear (control=%08" PRIx32 ")",
                       machine->uart.control);
        }
        if (reader != TASK_STATE_BLOCKED) {
            return 1; /* let it settle at the wait first */
        }
        uint32_t owner_checks = 0;
        if (!bus_read32(&machine->bus, drive->probe_addr + 4u * TW_CHECKS_DONE,
                        &owner_checks)) {
            drive->io_error = true;
            return 0;
        }
        if (owner_checks == 0u) {
            return 1; /* keep the reader blocked while the counter checks ownership */
        }
        drive->rearm_seen = true;
    }
    if (yan_uart_push_rx(&machine->uart,
                         drive->backend.feed[drive->backend.feed_at]) == YAN_OK) {
        ++drive->backend.feed_at;
    }
    return 1;
}

static void note_wake(Drive *drive, const YanMachine *machine)
{
    if (drive->window != WINDOW_WAKE || !drive->window_fired) {
        return;
    }
    uint32_t reader = 0;
    if (task_state_of(drive, machine, READER_SLOT, &reader) &&
        reader == TASK_STATE_RUNNABLE) {
        drive->wake_seen = true; /* RUNNABLE, before it is switched back to */
    }
}

/* ------------------------------------------------------------- fixture mode */

static int fixture_update_window(Drive *drive, YanMachine *machine)
{
    /* One true entry count for every scenario: the PC is observed at the
     * predicate's first instruction, before the step that executes it. A
     * yield/poll reader re-enters it, which the window checks below report as
     * an owner failure instead of a timeout. */
    if (drive->predicate_known && machine->cpu.pc == drive->predicate_addr) {
        ++drive->predicate_entries;
    }
    uint32_t reader = 0;
    if (!task_state_of(drive, machine, READER_SLOT, &reader)) {
        return 0;
    }
    if (drive->window_fired) {
        return 1;
    }
    const uint32_t ticks = bus_word(&machine->bus, drive->probe_addr + 4u * TW_TICKS);
    if (reader == TASK_STATE_BLOCKED) {
        drive->observed_blocked = true;
    }
    switch (drive->window) {
    case WINDOW_READY:
    case WINDOW_REJECT:
        if (bus_word(&machine->bus, drive->probe_addr + 4u * TW_READER_STAGE) >= 1u) {
            drive->window_fired = true;
        }
        break;
    case WINDOW_PREDICATE: {
        if (drive->predicate_entries > 1u) {
            DRIVE_FAIL("line_predicate was entered %" PRIu32 " times before the"
                       " window fired (a yield/poll reader)",
                       drive->predicate_entries);
        }
        if (!drive->predicate_entered && machine->cpu.pc == drive->predicate_addr) {
            drive->predicate_entered = true;
            drive->predicate_ra = machine->cpu.regs[1];
            break;
        }
        if (drive->predicate_entered && machine->cpu.pc == drive->predicate_ra) {
            if (machine->cpu.csr.mstatus & YAN_MSTATUS_MIE) {
                DRIVE_FAIL("the predicate returned with MIE=1 (mstatus=%08" PRIx32
                           ")", machine->cpu.csr.mstatus);
            }
            if (machine->cpu.regs[10] != 0u) {
                DRIVE_FAIL("the predicate window fired after line_predicate"
                           " answered true (a0=%" PRIu32 ")",
                           machine->cpu.regs[10]);
            }
            drive->window_fired = true;
        }
        break;
    }
    case WINDOW_BLOCKED:
    case WINDOW_WAKE:
        if (drive->predicate_entries > 1u) {
            DRIVE_FAIL("line_predicate was entered %" PRIu32 " times before the"
                       " blocked window (a yield/poll reader)",
                       drive->predicate_entries);
        }
        if (ticks >= 8u && !drive->observed_blocked) {
            DRIVE_FAIL("the counter reached %" PRIu32 " ticks but the reader was"
                       " never BLOCKED (a yield/poll reader)", ticks);
        }
        if (reader == TASK_STATE_BLOCKED && ticks >= 4u) {
            if ((machine->uart.control & UART_RX_ENABLE) == 0u) {
                DRIVE_FAIL("the reader is BLOCKED but RX_IRQ_ENABLE is clear"
                           " (control=%08" PRIx32 ")", machine->uart.control);
            }
            drive->window_fired = true;
        }
        break;
    default:
        break;
    }
    if (drive->window_fired) {
        drive->backend.feed_started = true;
    }
    return 1;
}

/* ---------------------------------------------------------- production mode */

static int corrupt_reply_tag(YanMachine *machine, uint32_t head_before)
{
    const uint32_t mask = machine->transport.ring_size - 1u;
    const uint32_t length = (machine->transport.h2g_head - head_before) & mask;
    if (length < 4u) {
        return 0;
    }
    const uint32_t offset = (machine->transport.ring_base - machine->ram_base) +
                            machine->transport.ring_size +
                            ((head_before + 2u) & mask);
    uint32_t tag = 0;
    if (yan_ram_read(&machine->ram, offset, 1, &tag) != YAN_OK) {
        return 0;
    }
    return yan_ram_write(&machine->ram, offset, 1, (tag ^ 0xA5u)) == YAN_OK;
}

static int production_service(Drive *drive, YanMachine *machine)
{
    const uint32_t head_before = machine->transport.h2g_head;
    const uint32_t answered =
        yan_host_block_service(&drive->block, &machine->transport,
                               &machine->ram, machine->ram_base);
    if (answered > 0u && drive->fault == FAULT_PROTOCOL &&
        drive->block.served >= drive->corrupt_response) {
        if (!corrupt_reply_tag(machine, head_before)) {
            /* The test backend could not inject the fault. That is a harness
             * limitation, not a Guest assertion, so it must never be credited
             * as an owner failure. */
            fprintf(stderr, ":HARNESS-ERROR: [drive] could not corrupt the"
                    " published response tag\n");
            return -1;
        }
        drive->protocol_fired = true;
        drive->fault = FAULT_NONE;
    }
    if (drive->fault == FAULT_IO && !drive->fault_armed &&
        strstr(drive->backend.out, "yanfs> ") != NULL) {
        drive->block.fail_next = true;
        drive->fault_armed = true;
    }
    return 1;
}

/* ------------------------------------------------------------ probe verdict */

/* Everything the Guest recorded is asserted here, by symbol, before tohost 1
 * is accepted. A wrong recorded result is a drive failure, not a pass. */
static int verify_probe(Drive *drive, const YanMachine *machine)
{
    TwProbe probe;
    if (!read_probe(&machine->bus, drive->probe_addr, &probe)) {
        drive->io_error = true;
        return 0;
    }
    if (probe.magic != TW_MAGIC_VALUE) {
        DRIVE_FAIL("probe magic is 0x%08" PRIx32 " not 0x%08" PRIx32,
                   probe.magic, (uint32_t)TW_MAGIC_VALUE);
    }
    const uint32_t expected_scenario =
        drive->window == WINDOW_READY ? 1u
        : drive->window == WINDOW_PREDICATE ? 2u
        : drive->window == WINDOW_BLOCKED ? 3u
        : drive->window == WINDOW_WAKE ? 4u
        : 5u;
    if (probe.scenario != expected_scenario) {
        DRIVE_FAIL("probe scenario is %" PRIu32 " not %" PRIu32,
                   probe.scenario, expected_scenario);
    }
    if (probe.checks_done != 1u) {
        DRIVE_FAIL("the counter never completed the second-object checks");
    }
    if (probe.reader_stage != 5u) {
        DRIVE_FAIL("reader_stage is %" PRIu32 " not 5", probe.reader_stage);
    }
    if (probe.second_open_result != TW_LINE_BUSY) {
        DRIVE_FAIL("open(second) while first owns answered %" PRIu32
                   " not BUSY", probe.second_open_result);
    }
    if (probe.second_ctx_zero != 1u) {
        DRIVE_FAIL("the second object changed across the refused open");
    }
    if (probe.wrong_next_result != TW_LINE_INVALID) {
        DRIVE_FAIL("next(second) with first as owner answered %" PRIu32
                   " not INVALID", probe.wrong_next_result);
    }
    if (probe.wrong_close_result != TW_LINE_INVALID) {
        DRIVE_FAIL("close(second) with first as owner answered %" PRIu32
                   " not INVALID", probe.wrong_close_result);
    }
    if (probe.busy_close_result != TW_LINE_BUSY) {
        DRIVE_FAIL("close(first) during a read answered %" PRIu32 " not BUSY",
                   probe.busy_close_result);
    }
    if (probe.ctrl_before != probe.ctrl_after) {
        DRIVE_FAIL("the refused close changed UART CONTROL (%08" PRIx32
                   " -> %08" PRIx32 ")", probe.ctrl_before, probe.ctrl_after);
    }
    if ((probe.ctrl_after & UART_RX_ENABLE) == 0u) {
        DRIVE_FAIL("the owner's RX_IRQ_ENABLE was cleared by the refused close"
                   " (control=%08" PRIx32 ")", probe.ctrl_after);
    }
    if (probe.line0_length != 2u || probe.line0_word != UINT32_C(0x6b6f)) {
        DRIVE_FAIL("line0 is length %" PRIu32 " word 0x%08" PRIx32
                   " not 'ok'", probe.line0_length, probe.line0_word);
    }
    if (probe.line1_length != 2u || probe.line1_word != UINT32_C(0x6f67)) {
        DRIVE_FAIL("line1 is length %" PRIu32 " word 0x%08" PRIx32
                   " not 'go'", probe.line1_length, probe.line1_word);
    }
    if (drive->window == WINDOW_REJECT) {
        if (probe.reject_result != TW_LINE_INVALID_INPUT ||
            probe.reject_length != 0u) {
            DRIVE_FAIL("the refused line recorded result %" PRIu32
                       " length %" PRIu32, probe.reject_result,
                       probe.reject_length);
        }
    }
    return 1;
}

/* -------------------------------------------------------------------- main */

#define HARNESS_FAIL(...) do { \
    fprintf(stderr, ":HARNESS-ERROR: [drive] " __VA_ARGS__); \
    fprintf(stderr, "\n"); \
    result = EXIT_HOST_ERROR; \
    goto done; \
} while (0)

static void usage(void)
{
    fprintf(stderr, "usage: terminal_runtime_driver --image ELF --capture FILE"
            " --status FILE --mode fixture|production [--max-steps N]\n"
            "  fixture:    --scenario ready|predicate|blocked|wake|reject\n"
            "  production: --disk-image FILE --stdin FILE"
            " [--fault none|io|protocol] [--corrupt-response N]"
            " [--tx-fault-at N] [--baseline FILE] [--expect-tohost HEX]\n");
}

int main(int argc, char **argv)
{
    const char *image_path = NULL, *capture_path = NULL, *status_path = NULL;
    const char *disk_image = NULL, *stdin_path = NULL, *scenario = NULL;
    const char *fault_name = "none", *baseline_path = NULL;
    uint64_t base = UINT32_C(0x80000000);
    uint64_t ram_size = 16u * 1024u * 1024u;
    uint64_t max_steps = 3000000;
    uint64_t corrupt_response = 0, tx_fault_at = 0;
    uint32_t expect_tohost = 0;
    int have_expect_tohost = 0;
    int mode = 0;

    for (int i = 1; i < argc; ++i) {
        uint64_t value = 0;
        if (strcmp(argv[i], "--image") == 0 && i + 1 < argc) image_path = argv[++i];
        else if (strcmp(argv[i], "--capture") == 0 && i + 1 < argc) capture_path = argv[++i];
        else if (strcmp(argv[i], "--status") == 0 && i + 1 < argc) status_path = argv[++i];
        else if (strcmp(argv[i], "--disk-image") == 0 && i + 1 < argc) disk_image = argv[++i];
        else if (strcmp(argv[i], "--stdin") == 0 && i + 1 < argc) stdin_path = argv[++i];
        else if (strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) scenario = argv[++i];
        else if (strcmp(argv[i], "--fault") == 0 && i + 1 < argc) fault_name = argv[++i];
        else if (strcmp(argv[i], "--baseline") == 0 && i + 1 < argc) baseline_path = argv[++i];
        else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            const char *text = argv[++i];
            if (strcmp(text, "fixture") == 0) mode = MODE_FIXTURE;
            else if (strcmp(text, "production") == 0) mode = MODE_PRODUCTION;
        } else if (strcmp(argv[i], "--expect-tohost") == 0 && i + 1 < argc) {
            if (!parse_hex(argv[++i], &expect_tohost)) return EXIT_USAGE;
            have_expect_tohost = 1;
        } else if (strcmp(argv[i], "--base") == 0 && i + 1 < argc) {
            if (!number(argv[++i], &value)) return EXIT_USAGE;
            base = value;
        } else if (strcmp(argv[i], "--ram") == 0 && i + 1 < argc) {
            if (!number(argv[++i], &value)) return EXIT_USAGE;
            ram_size = value;
        } else if (strcmp(argv[i], "--max-steps") == 0 && i + 1 < argc) {
            if (!number(argv[++i], &value)) return EXIT_USAGE;
            max_steps = value;
        } else if (strcmp(argv[i], "--corrupt-response") == 0 && i + 1 < argc) {
            if (!number(argv[++i], &value)) return EXIT_USAGE;
            corrupt_response = value;
        } else if (strcmp(argv[i], "--tx-fault-at") == 0 && i + 1 < argc) {
            if (!number(argv[++i], &value)) return EXIT_USAGE;
            tx_fault_at = value;
        } else {
            usage();
            return EXIT_USAGE;
        }
    }
    if (image_path == NULL || capture_path == NULL || status_path == NULL ||
        mode == 0 || ram_size == 0) {
        usage();
        return EXIT_USAGE;
    }

    Drive drive;
    memset(&drive, 0, sizeof drive);
    drive.mode = mode;
    drive.backend.tx_fault_at = (uint32_t)tx_fault_at;
    drive.corrupt_response = (uint32_t)corrupt_response;
    if (strcmp(fault_name, "io") == 0) drive.fault = FAULT_IO;
    else if (strcmp(fault_name, "protocol") == 0) drive.fault = FAULT_PROTOCOL;
    else if (strcmp(fault_name, "none") != 0) return EXIT_USAGE;
    if (mode == MODE_FIXTURE) {
        if (scenario == NULL) return EXIT_USAGE;
        if (strcmp(scenario, "ready") == 0) drive.window = WINDOW_READY;
        else if (strcmp(scenario, "predicate") == 0) drive.window = WINDOW_PREDICATE;
        else if (strcmp(scenario, "blocked") == 0) drive.window = WINDOW_BLOCKED;
        else if (strcmp(scenario, "wake") == 0) drive.window = WINDOW_WAKE;
        else if (strcmp(scenario, "reject") == 0) drive.window = WINDOW_REJECT;
        else return EXIT_USAGE;
    } else if (disk_image == NULL || stdin_path == NULL) {
        usage();
        return EXIT_USAGE;
    }

    int result = EXIT_HOST_ERROR;
    const char *verdict = "harness";
    YanMachine machine;
    memset(&machine, 0, sizeof machine);
    uint8_t *image = NULL;
    size_t image_size = 0;
    int machine_ready = 0;
    int stopped = 0;

    drive.backend.capture = fopen(capture_path, "wb");
    if (drive.backend.capture == NULL) {
        fprintf(stderr, ":HARNESS-ERROR: [drive] cannot write '%s'\n", capture_path);
        return EXIT_HOST_ERROR;
    }
    FILE *status_file = fopen(status_path, "wb");
    if (status_file == NULL) {
        fprintf(stderr, ":HARNESS-ERROR: [drive] cannot write '%s'\n", status_path);
        (void)fclose(drive.backend.capture);
        return EXIT_HOST_ERROR;
    }
    if (yan_machine_init_with(&machine, (uint32_t)base, (size_t)ram_size) != YAN_OK) {
        HARNESS_FAIL("cannot assemble the machine");
    }
    machine_ready = 1;
    const YanUartTerminal terminal = {
        .context = &drive.backend,
        .tx_ready = backend_tx_ready,
        .tx_write = backend_tx_write,
    };
    if (yan_uart_set_terminal(&machine.uart, &terminal) != YAN_OK) {
        HARNESS_FAIL("cannot attach the UART backend");
    }

    if (mode == MODE_PRODUCTION) {
        size_t storage_size = 0;
        drive.storage = yan_host_read_file(disk_image, &storage_size);
        if (drive.storage == NULL || storage_size % YAN_HOST_BLOCK_BLOCK_SIZE != 0) {
            HARNESS_FAIL("'%s' is not a block image", disk_image);
        }
        drive.capacity = storage_size / YAN_HOST_BLOCK_BLOCK_SIZE;
        if (yan_host_block_init(&drive.block, drive.storage, drive.capacity) != YAN_OK) {
            HARNESS_FAIL("cannot init the block service");
        }
        drive.backend.feed = yan_host_read_file(stdin_path, &drive.backend.feed_length);
        drive.backend.feed_owned = true;
        drive.backend.feed_started = true;
        if (drive.backend.feed == NULL) {
            HARNESS_FAIL("cannot read '%s'", stdin_path);
        }
        if (baseline_path != NULL) {
            drive.baseline = yan_host_read_file(baseline_path, &drive.baseline_length);
            if (drive.baseline == NULL) {
                HARNESS_FAIL("cannot read the baseline '%s'", baseline_path);
            }
            drive.baseline_loaded = true;
        }
        const uint32_t ring_base = (uint32_t)base + (uint32_t)ram_size - 2u * RING_SIZE;
        if (yan_transport_configure(&machine.transport, ring_base, RING_SIZE,
                                    (uint32_t)base, (uint32_t)ram_size) != YAN_OK) {
            HARNESS_FAIL("cannot place the transport rings");
        }
        yan_transport_set_notify(&machine.transport, transport_notify, NULL);
    } else {
        static const uint8_t healthy[] = {'o', 'k', '\n', 'g', 'o', '\n'};
        static const uint8_t rejected[] = {'x', 0x01u, 'y', '\n',
                                           'o', 'k', '\n', 'g', 'o', '\n'};
        if (drive.window == WINDOW_REJECT) {
            drive.backend.feed = rejected;
            drive.backend.feed_length = sizeof rejected;
        } else {
            drive.backend.feed = healthy;
            drive.backend.feed_length = sizeof healthy;
        }
        for (size_t index = 1; index < drive.backend.feed_length; ++index) {
            if (drive.backend.feed[index - 1] == (uint8_t)'\n') {
                drive.line2_start = index;
                break;
            }
        }
    }

    image = yan_host_read_file(image_path, &image_size);
    if (image == NULL) {
        HARNESS_FAIL("cannot read '%s'", image_path);
    }
    YanImageInfo info;
    memset(&info, 0, sizeof info);
    if (yan_image_load_elf(&machine.ram, (uint32_t)base, image, image_size, &info) != YAN_OK) {
        HARNESS_FAIL("'%s' is not a loadable ELF", image_path);
    }
    if (yan_cpu_reset(&machine.cpu, info.entry) != YAN_OK) {
        HARNESS_FAIL("cannot reset the CPU to the image entry");
    }
    if (yan_image_find_symbol(image, image_size, "tohost", &drive.tohost_addr) == YAN_OK) {
        drive.tohost_known = 1;
    }
    if (yan_image_find_symbol(image, image_size, "tw_probe", &drive.probe_addr) == YAN_OK) {
        drive.probe_known = 1;
    }
    if (yan_image_find_symbol(image, image_size, TASKS_SYMBOL, &drive.tasks_addr) == YAN_OK) {
        drive.tasks_known = 1;
    }
    if (yan_image_find_symbol(image, image_size, "line_predicate",
                              &drive.predicate_addr) == YAN_OK) {
        drive.predicate_known = 1;
    }
    if (yan_image_find_symbol(image, image_size, "yan_terminal_putc",
                              &drive.putc_addr) == YAN_OK) {
        drive.putc_known = 1;
    }
    if (yan_image_find_symbol(image, image_size, "yan_os_console_putc",
                              &drive.console_addr) == YAN_OK) {
        drive.console_known = 1;
    }
    /* A symbol a mode needs is required, never guessed or bypassed. */
    if (!drive.tohost_known) {
        HARNESS_FAIL("the image has no tohost symbol");
    }
    if (mode == MODE_FIXTURE) {
        if (!drive.probe_known) HARNESS_FAIL("the fixture image has no tw_probe");
        if (!drive.tasks_known) HARNESS_FAIL("the fixture image has no tasks");
        if (!drive.predicate_known) HARNESS_FAIL("the fixture image has no line_predicate");
    }
    if (drive.backend.tx_fault_at != 0u && !drive.putc_known) {
        HARNESS_FAIL("the production image has no yan_terminal_putc symbol");
    }

    for (uint64_t step = 0; step < max_steps; ++step) {
        if (drive.backend.tx_failed) {
            if (drive.putc_known && machine.cpu.pc == drive.putc_addr) {
                ++drive.backend.putc_entries_after_fail;
            }
            if (drive.console_known && machine.cpu.pc == drive.console_addr) {
                ++drive.backend.console_entries_after_fail;
            }
        }

        if (mode == MODE_FIXTURE) {
            note_wake(&drive, &machine);
            if (!fixture_update_window(&drive, &machine)) {
                result = EXIT_DRIVE_ASSERTION;
                verdict = "drive";
                goto done;
            }
            if (drive.io_error) {
                HARNESS_FAIL("a task-array read failed");
            }
        } else {
            const int serviced = production_service(&drive, &machine);
            if (serviced < 0) {
                HARNESS_FAIL("the block response tag could not be injected");
            }
            if (serviced == 0) {
                result = EXIT_DRIVE_ASSERTION;
                verdict = "drive";
                goto done;
            }
        }

        Snapshot before;
        if (!snapshot_machine(&drive, &machine, &before)) {
            HARNESS_FAIL("a task-array read failed");
        }
        if (!feed_throttled(&drive, &machine)) {
            if (drive.io_error) {
                HARNESS_FAIL("a task-array read failed");
            }
            result = EXIT_DRIVE_ASSERTION;
            verdict = "drive";
            goto done;
        }

        const YanStatus step_status = yan_machine_step(&machine);
        /* YAN_TRAP means the CPU entered the Guest trap vector, including
         * ordinary UART/transport interrupts. The Guest handles that step. */
        if (step_status != YAN_OK && step_status != YAN_TRAP) {
            HARNESS_FAIL("yan_machine_step failed at step %" PRIu64, step);
        }

        if (mode == MODE_PRODUCTION) {
            const int serviced = production_service(&drive, &machine);
            if (serviced < 0) {
                HARNESS_FAIL("the block response tag could not be injected");
            }
            if (serviced == 0) {
                result = EXIT_DRIVE_ASSERTION;
                verdict = "drive";
                goto done;
            }
        }

        Snapshot after;
        if (!snapshot_machine(&drive, &machine, &after)) {
            HARNESS_FAIL("a task-array read failed");
        }
        if (!check_boundary(&drive, &machine, &before, &after, step)) {
            result = EXIT_DRIVE_ASSERTION;
            verdict = "drive";
            goto done;
        }

        uint32_t host_word = 0;
        if (!bus_read32(&machine.bus, drive.tohost_addr, &host_word)) {
            HARNESS_FAIL("cannot read tohost at step %" PRIu64, step);
        }
        if (host_word != 0u) {
            drive.tohost_value = host_word;
            drive.tohost_seen = true;
            stopped = 1;
            if (host_word == 1u) {
                result = EXIT_PASS;
                verdict = "pass";
            } else if (mode == MODE_FIXTURE && host_word > FIXTURE_FAIL_TOP &&
                       host_word <= FIXTURE_FAIL_TOP + FIXTURE_FAIL_LAST) {
                result = EXIT_GUEST_FAILURE;
                verdict = "guest";
            } else if (mode == MODE_PRODUCTION && host_word > APP_FAIL_TOP &&
                       host_word <= APP_FAIL_TOP + APP_REASON_LAST) {
                result = EXIT_GUEST_FAILURE;
                verdict = "guest";
            } else {
                /* A runtime panic (0x80000000 | reason), a trap or any code
                 * outside the known reasons is a harness error, never an owner
                 * failure, and it must not be re-labelled by a later check. */
                fprintf(stderr, ":HARNESS-ERROR: [drive] unexpected tohost"
                        " 0x%08" PRIx32 " outside the known reason ranges\n",
                        host_word);
                result = EXIT_HOST_ERROR;
                verdict = "harness";
            }
            break;
        }
    }

    if (!stopped) {
        fprintf(stderr, "terminal_runtime_driver: stopped after %" PRIu64
                " steps without tohost\n", max_steps);
        result = EXIT_NO_TERMINATION;
        verdict = "timeout";
    }
    if (mode == MODE_FIXTURE && result == EXIT_PASS) {
        if (!verify_probe(&drive, &machine)) {
            if (drive.io_error) {
                HARNESS_FAIL("a probe read failed");
            }
            result = EXIT_DRIVE_ASSERTION;
            verdict = "drive";
        }
        if (result == EXIT_PASS && !drive.rearm_seen) {
            fprintf(stderr, "FAIL [drive] the second line was never delivered to"
                    " a blocked, re-armed reader\n");
            result = EXIT_DRIVE_ASSERTION;
            verdict = "drive";
        }
        if (result == EXIT_PASS && drive.window == WINDOW_WAKE && !drive.wake_seen) {
            fprintf(stderr, "FAIL [drive] the reader was never observed RUNNABLE"
                    " after the byte arrived\n");
            result = EXIT_DRIVE_ASSERTION;
            verdict = "drive";
        }
        if (result == EXIT_PASS && drive.isr_count == 0u) {
            fprintf(stderr, "FAIL [drive] no UART source-2 service was observed\n");
            result = EXIT_DRIVE_ASSERTION;
            verdict = "drive";
        }
    }
    if (stopped && result != EXIT_HOST_ERROR && drive.backend.tx_fault_at != 0u) {
        if (!drive.backend.tx_failed) {
            fprintf(stderr, "FAIL [drive] the requested tx fault at byte %" PRIu32
                    " never fired (delivered %" PRIu32 ")\n",
                    drive.backend.tx_fault_at, drive.backend.delivered);
            result = EXIT_DRIVE_ASSERTION;
            verdict = "drive";
        } else if (drive.backend.delivered != drive.backend.tx_fault_at ||
                   drive.backend.out_length != drive.backend.tx_fault_at) {
            fprintf(stderr, "FAIL [drive] after the tx fault delivered=%" PRIu32
                    " out=%zu expected exactly %" PRIu32 "\n",
                    drive.backend.delivered, drive.backend.out_length,
                    drive.backend.tx_fault_at);
            result = EXIT_DRIVE_ASSERTION;
            verdict = "drive";
        } else if (!drive.baseline_loaded ||
                   drive.backend.tx_fault_at > drive.baseline_length ||
                   memcmp(drive.backend.out, drive.baseline,
                          drive.backend.tx_fault_at) != 0) {
            fprintf(stderr, "FAIL [drive] the faulted output is not the healthy"
                    " baseline prefix\n");
            result = EXIT_DRIVE_ASSERTION;
            verdict = "drive";
        } else if (drive.tohost_seen && drive.tohost_value == 1u) {
            fprintf(stderr, "FAIL [drive] a tx fault reached tohost PASS\n");
            result = EXIT_DRIVE_ASSERTION;
            verdict = "drive";
        } else if (drive.backend.putc_entries_after_fail != 0u ||
                   drive.backend.console_entries_after_fail != 0u) {
            fprintf(stderr, "FAIL [drive] %" PRIu32 " yan_terminal_putc and %"
                    PRIu32 " console entries after the output failure\n",
                    drive.backend.putc_entries_after_fail,
                    drive.backend.console_entries_after_fail);
            result = EXIT_DRIVE_ASSERTION;
            verdict = "drive";
        }
    }
    if (stopped && result != EXIT_HOST_ERROR && have_expect_tohost &&
        drive.tohost_seen && drive.tohost_value != expect_tohost) {
        fprintf(stderr, "FAIL [drive] tohost is 0x%08" PRIx32
                " not the expected 0x%08" PRIx32 "\n",
                drive.tohost_value, expect_tohost);
        result = EXIT_DRIVE_ASSERTION;
        verdict = "drive";
    }

done:
    if (status_file != NULL) {
        (void)fprintf(status_file, "verdict=%s\ntohost=0x%08" PRIx32
                      "\ndelivered=%" PRIu32 "\nout_length=%zu\nisr_count=%"
                      PRIu32 "\npredicate_entries=%" PRIu32 "\nchecks=%" PRIu32
                      "\nreader_stage=%" PRIu32 "\nticks=%" PRIu32
                      "\nprotocol_fired=%d\ntx_fired=%d\n",
                      verdict, drive.tohost_value, drive.backend.delivered,
                      drive.backend.out_length, drive.isr_count,
                      drive.predicate_entries,
                      drive.probe_known
                          ? bus_word(&machine.bus,
                                     drive.probe_addr + 4u * TW_CHECKS_DONE)
                          : 0u,
                      drive.probe_known
                          ? bus_word(&machine.bus,
                                     drive.probe_addr + 4u * TW_READER_STAGE)
                          : 0u,
                      drive.probe_known
                          ? bus_word(&machine.bus,
                                     drive.probe_addr + 4u * TW_TICKS)
                          : 0u,
                      (int)drive.protocol_fired, (int)drive.backend.tx_failed);
        if (fflush(status_file) != 0 || ferror(status_file)) {
            result = EXIT_HOST_ERROR;
            verdict = "harness";
        }
        if (fclose(status_file) != 0) {
            result = EXIT_HOST_ERROR;
            verdict = "harness";
        }
    }
    if (drive.backend.capture != NULL) {
        if (fflush(drive.backend.capture) != 0 ||
            ferror(drive.backend.capture) || fclose(drive.backend.capture) != 0) {
            result = EXIT_HOST_ERROR;
            verdict = "harness";
        }
    }
    if (machine_ready) {
        if (drive.storage != NULL && disk_image != NULL) {
            FILE *write_back = fopen(disk_image, "wb");
            if (write_back == NULL) {
                result = EXIT_HOST_ERROR;
            } else {
                const size_t bytes =
                    (size_t)drive.capacity * YAN_HOST_BLOCK_BLOCK_SIZE;
                if (fwrite(drive.storage, 1, bytes, write_back) != bytes) {
                    result = EXIT_HOST_ERROR;
                }
                if (fclose(write_back) != 0) {
                    result = EXIT_HOST_ERROR;
                }
            }
        }
        yan_machine_destroy(&machine);
    }
    if (drive.backend.feed_owned) {
        free((void *)drive.backend.feed);
    }
    free((void *)drive.baseline);
    free(drive.storage);
    free(image);
    return result;
}
