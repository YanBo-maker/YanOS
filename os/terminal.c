/* The UART-backed terminal of 0022.
 *
 * The line grammar and the wait discipline live in os/line.c; this file only
 * supplies the machine side of YanLineIo and owns the single terminal slot.
 * The device-facing callbacks map onto os/platform.h accessors, and the block
 * is yan_os_task_wait on the UART event, which is a real blocking wait, never
 * a poll.
 *
 * Output deliberately goes through the frozen console driver (os/console.c)
 * rather than poking TXDATA here: the console already enforces "connection
 * first, then TX_READY" and "a refusal is reported, not retried". The only
 * thing added on top is 0022's post-write readiness recheck, which catches a
 * host backend that failed inside tx_write while TX_READY still read ready.
 * The console contract is unchanged. */
#include "terminal.h"

#include <stddef.h>

#include "console.h"
#include "platform.h"
#include "task.h"

/* The one terminal that owns the UART receive path in this image. The per
 * object field alone cannot be the authority: two different zero-initialized
 * YanTerminal objects would each look unowned and both could open. The owner
 * pointer is the single authority, and open checks it before any MMIO. */
static YanTerminal *yan_terminal_owner;

/* One session-wide output failure flag. It is the single place a refused byte
 * is remembered, so the application sink and the line echo that also writes
 * through this layer see the same state. Once set, yan_terminal_putc refuses
 * every later byte without touching the device. */
static bool yan_terminal_output_broken;

bool yan_terminal_output_failed(void)
{
    return yan_terminal_output_broken;
}

static int terminal_connected(void *context)
{
    (void)context;
    return yan_os_uart_connected();
}

static int terminal_ready(void *context)
{
    (void)context;
    return yan_os_uart_rx_ready();
}

/* The reader only asks after ready returned non-zero, so the 0 branch is a
 * report of a state that changed between the two calls, not an expected path. */
static int terminal_get_byte(void *context, uint8_t *byte)
{
    (void)context;
    if (!yan_os_uart_connected()) {
        return -1;
    }
    return yan_os_uart_get(byte) == 0 ? 1 : 0;
}

static bool terminal_put_byte(void *context, uint8_t byte)
{
    (void)context;
    return yan_terminal_putc(byte);
}

static void terminal_arm_rx(void *context, bool enable)
{
    (void)context;
    yan_os_uart_set_rx_irq(enable ? 1 : 0);
}

static void terminal_ack_rx(void *context)
{
    (void)context;
    yan_os_uart_ack_rx();
}

/* The reader's predicate is read-only and runs with interrupts off; handing it
 * straight to the runtime keeps that guarantee, because yan_os_task_wait asks
 * it inside its own critical section. */
static void terminal_wait(void *context, int (*predicate)(void *),
                          void *predicate_context)
{
    (void)context;
    yan_os_task_wait(YAN_OS_EVENT_UART, predicate, predicate_context);
}

bool yan_terminal_putc(uint8_t byte)
{
    if (yan_terminal_output_broken) {
        /* The host already refused a byte; every later attempt would be
         * discarded, so the layer does not touch the device again. */
        return false;
    }
    if (yan_os_console_putc((char)byte) != YAN_OS_OK) {
        yan_terminal_output_broken = true;
        return false;
    }
    /* 0022 asks for a recheck after the write, including the last byte: a
     * host tx_write failure turns TX_READY off without the console's own
     * observation having seen it. A failed recheck means the last byte may not
     * have reached the host, so the session's output is broken. */
    if (!yan_os_uart_connected() || !yan_os_uart_tx_ready()) {
        yan_terminal_output_broken = true;
        return false;
    }
    return true;
}

YanLineResult yan_terminal_open(YanTerminal *terminal)
{
    if (terminal == NULL) {
        return YAN_LINE_INVALID;
    }
    if (yan_terminal_owner != NULL) {
        /* Any other object owning the path is a second owner. Nothing is
         * touched: no MMIO, no line initialization, no interrupt change. */
        return YAN_LINE_BUSY;
    }
    if (!yan_os_uart_connected()) {
        /* No terminal backend: do not take ownership, do not initialize the
         * line and do not touch the interrupt route. The application ends with
         * a non-1 code instead of waiting for a terminal that is not there. */
        return YAN_LINE_UNAVAILABLE;
    }
    YanLineIo io;
    io.context = NULL;
    io.connected = terminal_connected;
    io.ready = terminal_ready;
    io.get_byte = terminal_get_byte;
    io.put_byte = terminal_put_byte;
    io.arm_rx = terminal_arm_rx;
    io.ack_rx = terminal_ack_rx;
    io.wait = terminal_wait;
    const YanLineResult result = yan_line_init(&terminal->line, io);
    if (result != YAN_LINE_OK) {
        return result;
    }
    /* New session: a previous refusal belongs to the previous terminal. */
    yan_terminal_output_broken = false;
    yan_terminal_owner = terminal;
    return YAN_LINE_OK;
}

YanLineResult yan_terminal_next(YanTerminal *terminal)
{
    if (terminal == NULL || yan_terminal_owner != terminal) {
        return YAN_LINE_INVALID;
    }
    return yan_line_next(&terminal->line);
}

YanLineResult yan_terminal_close(YanTerminal *terminal)
{
    if (terminal == NULL || yan_terminal_owner != terminal) {
        return YAN_LINE_INVALID;
    }
    if (terminal->line.busy) {
        /* A read owns the interrupt route right now; masking here would pull
         * the line out from under it. */
        return YAN_LINE_BUSY;
    }
    yan_os_uart_set_rx_irq(0);
    yan_os_uart_ack_rx();
    yan_terminal_owner = NULL;
    return YAN_LINE_OK;
}

const uint8_t *yan_terminal_line(const YanTerminal *terminal)
{
    if (terminal == NULL || (const YanTerminal *)yan_terminal_owner != terminal) {
        return NULL;
    }
    return terminal->line.buffer;
}

uint32_t yan_terminal_line_length(const YanTerminal *terminal)
{
    if (terminal == NULL || (const YanTerminal *)yan_terminal_owner != terminal) {
        return 0u;
    }
    return terminal->line.length;
}
