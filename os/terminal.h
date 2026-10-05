#ifndef YAN_OS_TERMINAL_H
#define YAN_OS_TERMINAL_H

#include "line.h"

#include <stdbool.h>
#include <stdint.h>

/* The UART-backed terminal of 0022: the real YanLineIo for the Guest.
 *
 * A YanTerminal carries the 1024-byte line, so it is a long-lived object -
 * static storage or a caller-owned structure - and never a local of a task.
 * It must be zero-initialized before the first open.
 *
 * Ownership is single and global: os/terminal.c keeps one owner pointer, so at
 * most one YanTerminal in the whole image can own the UART receive path even
 * when several objects are zero-initialized. `open` on an object while another
 * owns the path returns YAN_LINE_BUSY and performs no MMIO at all. `open` also
 * requires a connected UART: without one it returns YAN_LINE_UNAVAILABLE and
 * takes no ownership, initializes nothing and changes no interrupt state. The
 * PLIC route is the application's to configure; the terminal touches only the
 * device's RX enable and its arrival latch.
 *
 * `close` masks and acknowledges the UART receive interrupt and releases the
 * global owner, so a later open of any object can succeed. It refuses with
 * YAN_LINE_BUSY while a line read is in progress, because withdrawing the line
 * out from under the reader is not a safe close. Closing an object that is not
 * the current owner returns YAN_LINE_INVALID.
 *
 * The completed line is exposed read-only through the struct member and the
 * two accessors below; the application does not edit it. Output goes through
 * yan_terminal_putc, which wraps the frozen console driver and then re-checks
 * the connection and TX readiness, so a host write failure on the last byte is
 * still visible as a false return. A single session-wide flag remembers the
 * first refusal, so the application and the line echo observe the same "the
 * host is gone" state. */

typedef struct {
    YanLine line;
} YanTerminal;

/* Connects the UART to the reader. Zero-initialize the instance first.
 * YAN_LINE_BUSY when any terminal already owns the path - including this same
 * object being opened again while it is already the owner, which is a
 * self-conflict and not a re-open. YAN_LINE_UNAVAILABLE when no terminal
 * backend is attached, YAN_LINE_INVALID for a NULL instance or a reader that
 * cannot be initialized. Taking ownership clears the session's output-failure
 * flag. */
YanLineResult yan_terminal_open(YanTerminal *);

/* Reads one line. Returns the YanLineResult of the reader; YAN_LINE_INVALID
 * when this object is not the current owner. On YAN_LINE_OK the line is in
 * terminal->line.buffer with terminal->line.length bytes and a NUL. */
YanLineResult yan_terminal_next(YanTerminal *);

/* Masks and acknowledges the receive interrupt, then releases ownership.
 * YAN_LINE_BUSY while a line read owns the instance, YAN_LINE_INVALID when it
 * is NULL or not the current owner. */
YanLineResult yan_terminal_close(YanTerminal *);

/* Writes one byte through the console driver and returns false when it was
 * refused or when the post-write readiness recheck fails. It never retries and
 * does not change the console's own contract. Usable as a YanShellOutput.putc
 * through a one-line adapter that ignores its context. Once any call has
 * returned false this function keeps returning false without touching the
 * device: the host has gone and further attempts would only be discarded. */
bool yan_terminal_putc(uint8_t byte);

/* True once the host has refused an output byte in this session. The flag is
 * cleared when yan_terminal_open takes ownership and set by every failed
 * yan_terminal_putc, including the echo a line read performs. After it is true
 * the terminal cannot deliver another byte, so an application must stop
 * producing output instead of trying again. */
bool yan_terminal_output_failed(void);

/* Read-only views of the last line read by yan_terminal_next. Both are
 * NULL/zero for a NULL terminal or one that is not the current owner. */
const uint8_t *yan_terminal_line(const YanTerminal *);
uint32_t yan_terminal_line_length(const YanTerminal *);

#endif
