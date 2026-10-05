#include "console.h"

#include <stddef.h>

#include "platform.h"

/* YanOS console: the driver side of docs/specs/0017-console-and-os-layout.md.
 *
 * The UART device below this file reports whether a terminal is attached, and
 * docs/specs/0015-uart-device.md binds the driver to that report: with
 * CONNECTED = 0 nothing here may wait for a ready bit that will never be set.
 * The interface in os/console.h carries no blocking call for the same reason,
 * and every entry point below turns "the device is not there" into
 * YAN_OS_UNAVAILABLE at once, so a headless run keeps going instead of hanging.
 *
 * A refusal is never silent. putc reports a refused byte instead of retrying or
 * dropping it, puts stops at the first one, and a line that cannot be echoed is
 * reported rather than half-processed: the caller always learns that some output
 * did not reach the terminal.
 */

/* One bit of line state is enough to see a CRLF pair as one line ending: the LF
 * a CRLF pair sends after the CR belongs to the line the CR already ended. It is
 * file-static because it describes the byte stream, not one call - the swallow
 * happens at the start of the *next* getline, which is the call that sees the
 * LF. */
static int swallow_lf;

int yan_os_console_connected(void)
{
    return yan_os_uart_connected();
}

YanOsResult yan_os_console_putc(char c)
{
    /* Connection first, then the ready bit: that is the order 0015's driver
     * contract asks for, and it keeps "there is no terminal" distinct from "the
     * terminal is not accepting a byte right now" for anyone reading the state
     * while debugging, even though both are YAN_OS_UNAVAILABLE here. */
    if (!yan_os_uart_connected()) {
        return YAN_OS_UNAVAILABLE;
    }
    /* yan_os_uart_put consults the same TX_READY bit that STATUS reports, so a
     * refusal is a real refusal: the byte is not delivered, not buffered and not
     * retried. One observation, one answer. */
    if (yan_os_uart_put((uint8_t)c) != 0) {
        return YAN_OS_UNAVAILABLE;
    }
    return YAN_OS_OK;
}

YanOsResult yan_os_console_puts(const char *text)
{
    if (text == NULL) {
        return YAN_OS_INVALID_ARGUMENT;
    }
    /* The connection is checked once for the whole string rather than only
     * inside the loop, so that an empty string still answers "no terminal"
     * instead of reporting a silent success. */
    if (!yan_os_uart_connected()) {
        return YAN_OS_UNAVAILABLE;
    }
    for (const char *at = text; *at != '\0'; ++at) {
        const YanOsResult result = yan_os_console_putc(*at);
        if (result != YAN_OS_OK) {
            /* Stop at the first refused character and report that result: the
             * bytes already accepted stay on the wire, which locates the
             * failure better than silent truncation and terminates better than
             * an endless retry. */
            return result;
        }
    }
    return YAN_OS_OK;
}

YanOsResult yan_os_console_getline(char *buffer, unsigned capacity,
                                   unsigned *length)
{
    /* Arguments first, before the device is consulted at all: an invalid call is
     * invalid with or without a terminal, so B8 stays answerable in a headless
     * run. This is not a wait, so it does not touch the driver contract. */
    if (buffer == NULL || capacity == 0 || length == NULL) {
        return YAN_OS_INVALID_ARGUMENT;
    }

    unsigned stored = 0;
    for (;;) {
        /* Re-read on every iteration instead of once before the loop. A
         * terminal that disappears while this call waits would otherwise leave
         * the driver polling a device that can no longer produce a byte; here it
         * ends the wait with a reported failure. With CONNECTED = 0 this is also
         * the first thing that happens, so a headless call returns immediately. */
        if (!yan_os_uart_connected()) {
            return YAN_OS_UNAVAILABLE;
        }
        uint8_t byte = 0;
        if (yan_os_uart_get(&byte) != 0) {
            continue;
        }
        /* Consume the LF of a CRLF pair: it belongs to the line the CR ended,
         * and treating it as input would turn one Enter into two lines. */
        if (swallow_lf) {
            swallow_lf = 0;
            if (byte == '\n') {
                continue;
            }
        }
        if (byte == '\r' || byte == '\n') {
            swallow_lf = byte == '\r';
            if (yan_os_console_puts("\r\n") != YAN_OS_OK) {
                /* The line ended, but its ending could not be echoed. Keep the
                 * buffer terminated and report the refusal; *length stays
                 * untouched because the caller must not read a line the driver
                 * could not complete. */
                buffer[stored] = '\0';
                return YAN_OS_UNAVAILABLE;
            }
            buffer[stored] = '\0';
            *length = stored;
            return YAN_OS_OK;
        }
        if (byte == 0x08 || byte == 0x7f) {
            /* Backspace at the start of a line has nothing to erase, so it
             * neither changes the buffer nor echoes an erase sequence. */
            if (stored > 0) {
                --stored;
                if (yan_os_console_puts("\b \b") != YAN_OS_OK) {
                    buffer[stored] = '\0';
                    return YAN_OS_UNAVAILABLE;
                }
            }
            continue;
        }
        if (byte < 0x20) {
            /* Every other control character is ignored: not stored, not echoed,
             * and it does not end the line. 0x7f was taken by backspace above,
             * and bytes beyond it are not control characters: the spec passes
             * non-ASCII bytes through as printable, which the store below does
             * unchanged. */
            continue;
        }
        if (stored + 1 >= capacity) {
            /* The buffer holds at most capacity - 1 characters plus the NUL, so
             * one more would either overflow the caller's buffer or cost the
             * terminator. An overlong line therefore ignores the extra
             * characters and keeps reading: ending the line here would silently
             * turn a paste longer than the buffer into a short line. */
            continue;
        }
        buffer[stored] = (char)byte;
        ++stored;
        if (yan_os_console_putc((char)byte) != YAN_OS_OK) {
            /* The echo is part of reading a line, so a refused echo is reported
             * instead of swallowed; the characters accepted so far stay in the
             * buffer and it stays terminated for the caller. */
            buffer[stored] = '\0';
            return YAN_OS_UNAVAILABLE;
        }
    }
}
