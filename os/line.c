/* Interrupt-driven line reader: docs/specs/0022-terminal-file-operations.md.
 *
 * The contract is os/line.h. This file knows nothing about a UART: every
 * device fact arrives through YanLineIo, so the same code runs on the Guest
 * and under a native fake. Three properties shape the loop:
 *
 *   mask before read, re-arm before wait  the receive interrupt is maskable
 *   while the reader owns the byte it is about to take, and enabled again
 *   before it blocks. A byte that arrives around the block is either seen by
 *   the predicate or wakes the registered waiter; either way RXDATA still
 *   holds it, so the byte is never lost.
 *
 *   one result, one buffer state  a completed line is `length` bytes plus a
 *   NUL; a rejected or unavailable line comes back empty. A caller can never
 *   execute a prefix of a line the reader refused, and an echo failure never
 *   dispatches a half-read line.
 *
 *   reject once, drain to the end  the first reason a line was refused wins.
 *   While draining, the offending controls and every later byte are dropped
 *   without echo, backspace cannot recover the line, and only the line ending
 *   is echoed before the one error result is returned. */
#include "line.h"

#include <stddef.h>

#define LINE_CR UINT8_C(0x0d)
#define LINE_LF UINT8_C(0x0a)
#define LINE_BS UINT8_C(0x08)
#define LINE_DEL UINT8_C(0x7f)

/* True when `pointer` addresses one byte inside [object, object + size). The
 * reader only needs this to reject an io.context that points into the line it
 * is reading; it never dereferences the pointer. */
static bool pointer_inside(const void *pointer, const void *object, uint64_t size)
{
    if (pointer == NULL) {
        return false;
    }
    const uintptr_t value = (uintptr_t)pointer;
    const uintptr_t begin = (uintptr_t)object;
    if (value < begin) {
        return false;
    }
    if (size > (uint64_t)(UINTPTR_MAX - begin)) {
        return true;
    }
    return value < begin + (uintptr_t)size;
}

/* Read-only: it asks the device whether a byte is there and whether the
 * terminal still is, and consumes neither. It runs with interrupts off inside
 * yan_os_task_wait, which is why it must stay this short. */
static int line_predicate(void *context)
{
    YanLine *line = (YanLine *)context;
    return line->io.ready(line->io.context) ||
           !line->io.connected(line->io.context);
}

static bool line_echo_eol(YanLine *line)
{
    return line->io.put_byte(line->io.context, LINE_CR) &&
           line->io.put_byte(line->io.context, LINE_LF);
}

static bool line_echo_backspace(YanLine *line)
{
    return line->io.put_byte(line->io.context, LINE_BS) &&
           line->io.put_byte(line->io.context, (uint8_t)' ') &&
           line->io.put_byte(line->io.context, LINE_BS);
}

YanLineResult yan_line_init(YanLine *line, YanLineIo io)
{
    if (line == NULL) {
        return YAN_LINE_INVALID;
    }
    /* Address arithmetic first: a line pointer whose object would leave the
     * uintptr_t space is invalid before any field is read, so a wild pointer is
     * rejected without a load through it. */
    if ((uint64_t)sizeof(YanLine) > (uint64_t)(UINTPTR_MAX - (uintptr_t)line)) {
        return YAN_LINE_INVALID;
    }
    if (line->initialized && line->busy) {
        return YAN_LINE_BUSY;
    }
    if (io.connected == NULL || io.ready == NULL || io.get_byte == NULL ||
        io.put_byte == NULL || io.arm_rx == NULL || io.ack_rx == NULL ||
        io.wait == NULL) {
        return YAN_LINE_INVALID;
    }
    /* The device context must not point into the line object: a callback
     * writes through it, and the line is what the callback is helping to
     * build. This is the one alias the reader can detect without knowing the
     * context's size. */
    if (pointer_inside(io.context, line, (uint64_t)sizeof(YanLine))) {
        return YAN_LINE_INVALID;
    }
    line->io = io;
    line->initialized = true;
    line->busy = false;
    line->swallow_lf = false;
    line->length = 0;
    line->buffer[0] = '\0';
    return YAN_LINE_OK;
}

YanLineResult yan_line_next(YanLine *line)
{
    if (line == NULL || !line->initialized) {
        return YAN_LINE_INVALID;
    }
    if (line->busy) {
        return YAN_LINE_BUSY;
    }

    line->busy = true;
    line->length = 0;
    line->buffer[0] = '\0';

    YanLineResult result = YAN_LINE_UNAVAILABLE;
    YanLineResult reject_result = YAN_LINE_OK;
    bool rejecting = false;

    for (;;) {
        /* Re-read on every round: a terminal that leaves mid-line must not
         * leave the reader waiting for a byte that can no longer arrive. */
        if (!line->io.connected(line->io.context)) {
            result = YAN_LINE_UNAVAILABLE;
            break;
        }
        if (line->io.ready(line->io.context)) {
            /* Withdraw this UART's own interrupt line first: RX_READY &&
             * RX_IRQ_ENABLE is what PLIC source 2 sees, so clearing the enable
             * keeps the UART handler from running between the take and the
             * state change the byte causes. It does not stop the CPU from
             * taking any other source; those are unaffected. */
            line->io.arm_rx(line->io.context, false);
            uint8_t byte = 0;
            const int got = line->io.get_byte(line->io.context, &byte);
            if (got < 0) {
                result = YAN_LINE_UNAVAILABLE;
                break;
            }
            if (got == 0) {
                /* The device reported ready but had nothing; re-arm and look
                 * again from the top rather than spin here. */
                line->io.arm_rx(line->io.context, true);
                continue;
            }

            bool finished = false;
            if (rejecting) {
                if (byte == LINE_CR || byte == LINE_LF) {
                    line->swallow_lf = byte == LINE_CR;
                    if (!line_echo_eol(line)) {
                        result = YAN_LINE_UNAVAILABLE;
                        break;
                    }
                    result = reject_result;
                    finished = true;
                }
                /* Every other byte of a rejected line is dropped, without
                 * echo, and backspace cannot recover it. */
            } else {
                if (line->swallow_lf) {
                    line->swallow_lf = false;
                    if (byte == LINE_LF) {
                        line->io.arm_rx(line->io.context, true);
                        continue;
                    }
                }
                if (byte == LINE_CR || byte == LINE_LF) {
                    line->swallow_lf = byte == LINE_CR;
                    if (!line_echo_eol(line)) {
                        result = YAN_LINE_UNAVAILABLE;
                        break;
                    }
                    result = YAN_LINE_OK;
                    finished = true;
                } else if (byte == LINE_BS || byte == LINE_DEL) {
                    if (line->length > 0u) {
                        --line->length;
                        line->buffer[line->length] = '\0';
                        if (!line_echo_backspace(line)) {
                            result = YAN_LINE_UNAVAILABLE;
                            break;
                        }
                    }
                } else if (byte < UINT8_C(0x20)) {
                    /* NUL, TAB, ESC and every other C0 byte: refuse the whole
                     * line so the bytes around it are not spliced together. */
                    rejecting = true;
                    reject_result = YAN_LINE_INVALID_INPUT;
                } else if (line->length >= YAN_LINE_MAX) {
                    /* The byte that would make the line 1024 bytes long. */
                    rejecting = true;
                    reject_result = YAN_LINE_TOO_LONG;
                } else {
                    line->buffer[line->length] = byte;
                    ++line->length;
                    line->buffer[line->length] = '\0';
                    if (!line->io.put_byte(line->io.context, byte)) {
                        result = YAN_LINE_UNAVAILABLE;
                        break;
                    }
                }
            }
            if (finished) {
                break;
            }
            line->io.arm_rx(line->io.context, true);
            continue;
        }

        /* Nothing to take. Enable the interrupt, then block: the predicate is
         * re-evaluated under the runtime's critical section, so a byte that
         * arrives before registration is seen and one that arrives after it
         * wakes this task. No polling and no empty read. */
        line->io.arm_rx(line->io.context, true);
        line->io.wait(line->io.context, line_predicate, line);
    }

    /* Leaving the reader gives the line back masked and acknowledged. RXDATA
     * is deliberately not read: a byte that arrived at the boundary belongs to
     * the next call, and an exit must not discard it. */
    line->io.arm_rx(line->io.context, false);
    line->io.ack_rx(line->io.context);

    if (result != YAN_LINE_OK) {
        line->length = 0;
        line->buffer[0] = '\0';
    }
    line->busy = false;
    return result;
}
