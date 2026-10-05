#ifndef YAN_HOST_TERMINAL_H
#define YAN_HOST_TERMINAL_H

#include <stdbool.h>

#include "yan/uart.h"

/* The Host end of the UART device: standard output is the byte sink, standard
 * input is the byte source. The device keeps its own rules (see
 * docs/specs/0015-uart-device.md); this backend only answers the two questions
 * the device asks.
 *
 * `tx_ready` reports whether standard output can take a byte right now, and
 * `tx_write` accepts the byte when asked. The write therefore never refuses:
 * the device calls it only after `tx_ready` said yes, and a refusal would be a
 * byte silently lost with no way to tell the Guest.
 *
 * Input is a stream, not a terminal: a pipe, a regular file and a TTY all work
 * and none of them is required. "Nothing to read" is a normal answer, not an
 * error, so the caller decides when to poll and a run without input still
 * finishes on its own. */
typedef struct {
    /* Cleared once standard output stops accepting bytes, so the device stops
     * advertising TX_READY instead of losing them. */
    bool output_ok;
    /* Cleared at end of input or after a read error, so a closed standard input
     * is not polled on every step. */
    bool input_open;
} YanHostTerminal;

/* Arms the backend: standard output writable, standard input open. Call it
 * before taking a backend from yan_host_terminal_backend. */
void yan_host_terminal_init(YanHostTerminal *terminal);

/* A YanUartTerminal bound to this backend, for yan_uart_set_terminal. The
 * backend must outlive the attachment: the device copies the callbacks and
 * their context pointer, not the storage they point at. */
YanUartTerminal yan_host_terminal_backend(YanHostTerminal *terminal);

/* Move at most one byte from standard input into the UART's receive buffer.
 * Never blocks and never reports an error: false means no byte was handed over,
 * which covers "nothing available right now", "the Guest has not read the
 * previous byte yet" and "standard input ended". Call it between instructions;
 * the byte then waits in the device until the Guest reads RXDATA. */
bool yan_host_terminal_poll_rx(YanHostTerminal *terminal, YanUart *uart);

#endif
