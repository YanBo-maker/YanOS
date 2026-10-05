/* Guest fixture for the real UART receive window of 0022.
 *
 * This image links the real platform stack (os/trap_entry.S, os/task_switch.S,
 * os/task.c, os/console.c, os/line.c, os/terminal.c, os/memory.c) and nothing
 * from tests/ that would replace it: the reader calls yan_terminal_next(), so
 * the bytes travel through the real UART single-byte register, the real UART
 * interrupt line, PLIC source 2 and the task waiter. There is no direct RXDATA
 * read and no manual yan_os_wake anywhere.
 *
 * Two long-lived terminals and two tasks:
 *   first   the receive owner; the reader task runs two real lines through it;
 *   second  a second object, proving the global single-owner rule from outside;
 *   counter a second task that ticks and yields, so a blocked reader can be
 *           distinguished from a yield-and-poll loop, and that owns the
 *           second-object checks while the reader is blocked in a read.
 *
 * Every check asserts. A counter operation that answers the wrong result, or
 * that changes the second object, or that withdraws the owner's receive
 * interrupt, ends the run with its own failure code; the Host driver then
 * asserts the probe words again before it accepts tohost 1. Recording a result
 * without comparing it would let a wrong answer pass.
 *
 * Verdicts: tohost == 1 is the healthy finish. Every failure writes
 * 0x72000000 | reason, which the driver checks by name and reason.
 */
#include <stddef.h>
#include <stdint.h>

#include "console.h"
#include "guest.h"
#include "line.h"
#include "platform.h"
#include "task.h"
#include "terminal.h"

#define TW_SCENARIO_READY_FIRST 1
#define TW_SCENARIO_PREDICATE_WINDOW 2
#define TW_SCENARIO_BLOCKED 3
#define TW_SCENARIO_WAKE_NEXT 4
#define TW_SCENARIO_REJECT 5

#if !defined(TW_SCENARIO)
#error "build this image with -DTW_SCENARIO=<scenario>"
#endif

#define TW_FAIL(reason) (UINT32_C(0x72000000) | (reason))

#define TW_FAIL_SPAWN 1u
#define TW_FAIL_OPEN 2u
#define TW_FAIL_LINE0_RESULT 3u
#define TW_FAIL_LINE0_BYTES 4u
#define TW_FAIL_LINE1_RESULT 5u
#define TW_FAIL_LINE1_BYTES 6u
#define TW_FAIL_REJECT 7u
#define TW_FAIL_CLOSE_FIRST 8u
#define TW_FAIL_REOPEN 9u
#define TW_FAIL_CLOSE_SECOND 10u
#define TW_FAIL_SECOND_OPEN 11u
#define TW_FAIL_SECOND_CTX 12u
#define TW_FAIL_WRONG_NEXT 13u
#define TW_FAIL_WRONG_CLOSE 14u
#define TW_FAIL_IRQ_MASKED 15u
#define TW_FAIL_BUSY_CLOSE 16u
#define TW_FAIL_SELF_REOPEN 17u

#define TW_MAGIC_VALUE UINT32_C(0x54574c4b) /* "TWLK" */

/* Static evidence for the Host's private task-array read. os/task.h fixes
 * YAN_OS_TASK_CTX_BYTES = 56 (ra, sp, s0-s11), a 4096-byte stack and eight
 * slots; the driver derives state = 56 + 4 (entry) + 4 (arg) = 64 and
 * stride = 80 + 4096 = 4176 from those same constants. If either side changes,
 * this image stops compiling instead of letting the driver misread the array. */
_Static_assert(YAN_OS_TASK_CTX_BYTES == 56u, "context is 56 bytes");
_Static_assert(YAN_OS_TASK_STACK_SIZE == 4096u, "stack is 4096 bytes");
_Static_assert(YAN_OS_TASK_MAX == 8u, "eight task slots");
_Static_assert(YAN_OS_TASK_CTX_S11 + 4u == YAN_OS_TASK_CTX_BYTES,
               "context ends at s11 + 4");

/* Words the Host driver reads by symbol name. Every field is a plain 32-bit
 * word because the Host does not share the Guest's pointer size. */
typedef struct {
    uint32_t magic;              /* TW_MAGIC_VALUE once the platform is set up */
    uint32_t scenario;           /* the TW_SCENARIO this image was built with  */
    uint32_t reader_stage;       /* 0 init, 1 opened, 2 line0, 3 line1, 4 closed first, 5 done */
    uint32_t ticks;              /* the counter task's progress                */
    uint32_t checks_done;        /* 1 once the counter ran the owner checks    */
    uint32_t second_open_result; /* yan_terminal_open(&second) while first owns */
    uint32_t second_ctx_zero;    /* second is byte-identical after that call   */
    uint32_t wrong_next_result;  /* yan_terminal_next(&second)                 */
    uint32_t wrong_close_result; /* yan_terminal_close(&second)                */
    uint32_t control_before_close; /* UART CONTROL before the wrong close      */
    uint32_t control_after_close;  /* UART CONTROL after the wrong close       */
    uint32_t busy_close_result;  /* yan_terminal_close(&first) during a read   */
    uint32_t reject_result;      /* the refused line's result                  */
    uint32_t reject_length;      /* its length (must be zero)                  */
    uint32_t line0_length;
    uint32_t line1_length;
    uint32_t line0_word;         /* the two accepted bytes, little-endian      */
    uint32_t line1_word;
} TwProbe;

volatile TwProbe tw_probe;

static YanTerminal tw_first;
static YanTerminal tw_second;

__attribute__((noreturn)) static void tw_finish(uint32_t code)
{
    guest_finish(code);
    for (;;) {
    }
}

static void tw_snapshot(const YanTerminal *terminal, uint8_t *bytes, uint32_t length)
{
    const uint8_t *source = (const uint8_t *)terminal;
    for (uint32_t i = 0; i < length; ++i) {
        bytes[i] = source[i];
    }
}

static int tw_same_bytes(const uint8_t *left, const uint8_t *right, uint32_t length)
{
    for (uint32_t i = 0; i < length; ++i) {
        if (left[i] != right[i]) {
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------ the reader */

static void tw_reader(void *argument)
{
    (void)argument;
    if (yan_terminal_open(&tw_first) != YAN_LINE_OK) {
        tw_finish(TW_FAIL(TW_FAIL_OPEN));
    }
    tw_probe.reader_stage = 1u;

#if TW_SCENARIO == TW_SCENARIO_REJECT
    {
        const YanLineResult refused = yan_terminal_next(&tw_first);
        tw_probe.reject_result = (uint32_t)refused;
        tw_probe.reject_length = tw_first.line.length;
        if (refused != YAN_LINE_INVALID_INPUT || tw_first.line.length != 0u ||
            tw_first.line.buffer[0] != 0u) {
            tw_finish(TW_FAIL(TW_FAIL_REJECT));
        }
    }
#endif

    const YanLineResult line0 = yan_terminal_next(&tw_first);
    if (line0 != YAN_LINE_OK) {
        tw_finish(TW_FAIL(TW_FAIL_LINE0_RESULT));
    }
    if (tw_first.line.length != 2u ||
        tw_first.line.buffer[0] != (uint8_t)'o' ||
        tw_first.line.buffer[1] != (uint8_t)'k') {
        tw_finish(TW_FAIL(TW_FAIL_LINE0_BYTES));
    }
    tw_probe.line0_length = tw_first.line.length;
    tw_probe.line0_word = (uint32_t)tw_first.line.buffer[0] |
                          ((uint32_t)tw_first.line.buffer[1] << 8);
    tw_probe.reader_stage = 2u;

    const YanLineResult line1 = yan_terminal_next(&tw_first);
    if (line1 != YAN_LINE_OK) {
        tw_finish(TW_FAIL(TW_FAIL_LINE1_RESULT));
    }
    if (tw_first.line.length != 2u ||
        tw_first.line.buffer[0] != (uint8_t)'g' ||
        tw_first.line.buffer[1] != (uint8_t)'o') {
        tw_finish(TW_FAIL(TW_FAIL_LINE1_BYTES));
    }
    tw_probe.line1_length = tw_first.line.length;
    tw_probe.line1_word = (uint32_t)tw_first.line.buffer[0] |
                          ((uint32_t)tw_first.line.buffer[1] << 8);
    tw_probe.reader_stage = 3u;

    /* Give the path up, then a second object must be able to take it. While it
     * owns the path, opening it again is a self-conflict and must be BUSY. */
    if (yan_terminal_close(&tw_first) != YAN_LINE_OK) {
        tw_finish(TW_FAIL(TW_FAIL_CLOSE_FIRST));
    }
    tw_probe.reader_stage = 4u;
    if (yan_terminal_open(&tw_second) != YAN_LINE_OK) {
        tw_finish(TW_FAIL(TW_FAIL_REOPEN));
    }
    if (yan_terminal_open(&tw_second) != YAN_LINE_BUSY) {
        tw_finish(TW_FAIL(TW_FAIL_SELF_REOPEN));
    }
    if (yan_terminal_close(&tw_second) != YAN_LINE_OK) {
        tw_finish(TW_FAIL(TW_FAIL_CLOSE_SECOND));
    }
    tw_probe.reader_stage = 5u;
    tw_finish(1u);
}

/* ----------------------------------------------------------- the counter */

/* The counter task runs while the reader is blocked in a read. Whenever it
 * observes the reader past its open and before its finish, it performs the
 * second-object checks once; while it runs, the reader can only be stopped at
 * yan_terminal_next's wait (the scheduler is cooperative), so the close it
 * attempts on the owner is exactly "close during a read". Every result is
 * asserted here, not merely recorded. */
static void tw_counter(void *argument)
{
    (void)argument;
    int checked = 0;
    uint8_t before[sizeof(YanTerminal)];
    uint8_t after[sizeof(YanTerminal)];
    for (;;) {
        ++tw_probe.ticks;
        /* A wake may already have masked UART before the counter runs. Wait
         * for a later armed read so the wrong-owner checks can prove that an
         * enabled receive line is preserved, rather than assuming every read
         * resumes the counter with the device still armed. */
        if (!checked && tw_probe.reader_stage >= 1u && tw_probe.reader_stage < 5u &&
            (YAN_OS_MMIO_READ32(YAN_OS_UART_BASE + YAN_OS_UART_CONTROL) &
             YAN_OS_UART_CONTROL_RX_IRQ_ENABLE) != 0u) {
            checked = 1;

            tw_snapshot(&tw_second, before, (uint32_t)sizeof before);
            tw_probe.second_open_result = (uint32_t)yan_terminal_open(&tw_second);
            if (tw_probe.second_open_result != (uint32_t)YAN_LINE_BUSY) {
                tw_finish(TW_FAIL(TW_FAIL_SECOND_OPEN));
            }
            tw_snapshot(&tw_second, after, (uint32_t)sizeof after);
            tw_probe.second_ctx_zero =
                (uint32_t)tw_same_bytes(before, after, (uint32_t)sizeof before);
            if (tw_probe.second_ctx_zero != 1u) {
                tw_finish(TW_FAIL(TW_FAIL_SECOND_CTX));
            }

            tw_probe.wrong_next_result = (uint32_t)yan_terminal_next(&tw_second);
            if (tw_probe.wrong_next_result != (uint32_t)YAN_LINE_INVALID) {
                tw_finish(TW_FAIL(TW_FAIL_WRONG_NEXT));
            }

            tw_probe.control_before_close =
                YAN_OS_MMIO_READ32(YAN_OS_UART_BASE + YAN_OS_UART_CONTROL);
            tw_probe.wrong_close_result = (uint32_t)yan_terminal_close(&tw_second);
            if (tw_probe.wrong_close_result != (uint32_t)YAN_LINE_INVALID) {
                tw_finish(TW_FAIL(TW_FAIL_WRONG_CLOSE));
            }
            tw_probe.control_after_close =
                YAN_OS_MMIO_READ32(YAN_OS_UART_BASE + YAN_OS_UART_CONTROL);
            if (tw_probe.control_before_close != tw_probe.control_after_close ||
                (tw_probe.control_after_close &
                 YAN_OS_UART_CONTROL_RX_IRQ_ENABLE) == 0u) {
                tw_finish(TW_FAIL(TW_FAIL_IRQ_MASKED));
            }

            tw_probe.busy_close_result = (uint32_t)yan_terminal_close(&tw_first);
            if (tw_probe.busy_close_result != (uint32_t)YAN_LINE_BUSY) {
                tw_finish(TW_FAIL(TW_FAIL_BUSY_CLOSE));
            }
            tw_probe.checks_done = 1u;
        }
        yan_os_task_yield();
    }
}

/* --------------------------------------------------------------- platform */

static void tw_configure_platform(void)
{
    yan_os_uart_set_rx_irq(1);
    yan_os_plic_set_priority(YAN_OS_PLIC_SOURCE_UART, 1);
    yan_os_plic_set_threshold(0);
    yan_os_plic_enable(YAN_OS_PLIC_SOURCE_UART, 1);
    tw_probe.magic = TW_MAGIC_VALUE;
    tw_probe.scenario = TW_SCENARIO;
}

int main(void)
{
    tw_configure_platform();
    /* The counter is spawned first so it holds slot 0 and the reader slot 1;
     * the Host driver reads the reader's state from that fixed slot. */
    if (yan_os_task_spawn(tw_counter, NULL) != YAN_OS_TASK_OK) {
        tw_finish(TW_FAIL(TW_FAIL_SPAWN));
    }
    if (yan_os_task_spawn(tw_reader, NULL) != YAN_OS_TASK_OK) {
        tw_finish(TW_FAIL(TW_FAIL_SPAWN));
    }
    yan_os_sched_run();
    tw_finish(TW_FAIL(TW_FAIL_SPAWN));
}
