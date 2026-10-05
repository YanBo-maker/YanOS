/* Host terminal backend for the UART device.
 *
 * This is the "other end" docs/specs/0015-uart-device.md leaves to the Host: a
 * byte the Guest writes to TXDATA ends up on standard output, and a byte read
 * from standard input is handed to yan_uart_push_rx. Until a backend is
 * attached the device reports CONNECTED=0 and refuses both directions, so
 * everything here is reachable only through an explicit attachment.
 *
 * The file is written as a standalone translation unit. It is currently
 * compiled as part of yan_run.c (see the include there) because the build lists
 * that target's sources explicitly. */
#ifndef _POSIX_C_SOURCE
/* poll/read and the standard file descriptors are POSIX; yan_difftest.c picks
 * the same feature level. The macro must come before the first system header,
 * which is why it is repeated here and in yan_run.c. */
#define _POSIX_C_SOURCE 200809L
#endif

#include "host_terminal.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <unistd.h>

/* The device asks this before it advertises STATUS.TX_READY, so the answer is
 * the promise the Guest reads: false here means TXDATA is refused with
 * YAN_UNAVAILABLE and no byte is lost. */
static bool terminal_tx_ready(void *context)
{
    const YanHostTerminal *terminal = context;
    return terminal != NULL && terminal->output_ok;
}

/* Called only after terminal_tx_ready returned true. */
static void terminal_tx_write(void *context, uint8_t byte)
{
    YanHostTerminal *terminal = context;
    if (terminal == NULL) {
        return;
    }
    /* Flush every byte: with a pipe or a file as standard output the reader must
     * see what the Guest wrote without waiting for a buffer to fill, which is
     * what both the smoke test and an interactive console rely on. A failed
     * write cannot be reported to the Guest, so it withdraws the readiness
     * announced above instead; the next TXDATA then fails loudly with
     * YAN_UNAVAILABLE rather than quietly dropping bytes. */
    if (fputc((int)byte, stdout) == EOF || fflush(stdout) != 0) {
        terminal->output_ok = false;
    }
}

void yan_host_terminal_init(YanHostTerminal *terminal)
{
    if (terminal == NULL) {
        return;
    }
    *terminal = (YanHostTerminal){
        .output_ok = stdout != NULL,
        .input_open = true,
    };
}

YanUartTerminal yan_host_terminal_backend(YanHostTerminal *terminal)
{
    return (YanUartTerminal){
        .context = terminal,
        .tx_ready = terminal_tx_ready,
        .tx_write = terminal_tx_write,
    };
}

bool yan_host_terminal_poll_rx(YanHostTerminal *terminal, YanUart *uart)
{
    if (terminal == NULL || uart == NULL || !terminal->input_open) {
        return false;
    }
    /* The receive buffer holds one byte and refuses a second one instead of
     * queueing it, so reading ahead while the Guest has not consumed the
     * previous byte would have to throw away the byte just read. Wait for the
     * Guest instead; a byte already buffered is never overwritten. */
    uint32_t status = 0;
    if (yan_uart_read(uart, YAN_UART_STATUS, &status) != YAN_OK ||
        (status & YAN_UART_STATUS_RX_READY) != 0) {
        return false;
    }
    struct pollfd input = {.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};
    if (poll(&input, 1, 0) != 1) {
        /* No data, or a signal interrupted the wait: an answer, not an error,
         * so the run continues and no diagnostic is printed. */
        return false;
    }
    uint8_t byte = 0;
    const ssize_t got = read(STDIN_FILENO, &byte, 1);
    if (got == 1) {
        /* Cannot fail here: the device is connected (poll_rx is only reachable
         * through an attached backend) and its buffer was just found empty. */
        (void)yan_uart_push_rx(uart, byte);
        return true;
    }
    if (got == 0) {
        /* End of input. Stop asking a closed pipe on every step; running
         * without input is a normal way to run. */
        terminal->input_open = false;
        return false;
    }
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        return false;
    }
    /* Unreadable standard input ends the feed silently: the run itself is still
     * valid, only the receive direction is gone. */
    terminal->input_open = false;
    return false;
}
