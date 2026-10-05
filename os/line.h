#ifndef YAN_OS_LINE_H
#define YAN_OS_LINE_H

#include <stdbool.h>
#include <stdint.h>

/* Interrupt-driven line reader for the 0022 terminal.
 *
 * This layer is pure C17: it carries no MMIO, no Host header and no device
 * knowledge. Everything it needs from the machine arrives through YanLineIo,
 * a set of injected callbacks. That is what lets the same source run on the
 * Guest and be exercised natively against a fake byte stream.
 *
 * The grammar is exactly 0022's: CR or LF ends a line, a CR consumed in one
 * call swallows a following LF at the start of the next, 0x20..0x7e and
 * 0x80..0xff are stored and echoed as they are, 0x08 and 0x7f delete the last
 * byte (echoing "\b \b"), and any other C0 byte rejects the whole line as
 * INVALID_INPUT. A line is at most 1023 bytes; the byte that would make it
 * 1024 rejects it as TOO_LONG. A rejected line is drained to its end, no
 * normal byte is echoed, and the buffer comes back empty so a caller cannot
 * execute the prefix that was already accepted.
 *
 * Lifetime and stacks: a YanLine is long-lived, like the object that owns it,
 * and it holds the whole 1024-byte line, so it must not live on the 4 KiB
 * task stack of 0019. Nothing here allocates. */

/* The line buffer is 1024 bytes so that 1023 data bytes and one NUL fit.
 * The byte that would need a 1024th data byte rejects the line instead. */
#define YAN_LINE_BUFFER_SIZE 1024u
#define YAN_LINE_MAX 1023u

typedef enum {
    /* A complete line was stored: buffer holds `length` bytes plus a NUL. */
    YAN_LINE_OK = 0,
    /* The line grew past 1023 bytes. buffer is empty and length is zero. */
    YAN_LINE_TOO_LONG,
    /* A C0 byte other than CR, LF, BS or DEL appeared. The whole line is
     * rejected, so bytes on either side of the control are never spliced. */
    YAN_LINE_INVALID_INPUT,
    /* No terminal is attached, a device callback reported the device gone, or
     * an echo was refused. buffer is empty and length is zero. */
    YAN_LINE_UNAVAILABLE,
    /* The call itself was invalid: a NULL line, an uninitialized line, or an
     * address range the reader can prove is unusable. The line object is left
     * exactly as it was; no field is written. */
    YAN_LINE_INVALID,
    /* The line is already inside yan_line_next. The object is left exactly as
     * it was; no field is written. */
    YAN_LINE_BUSY
} YanLineResult;

/* The machine side of the reader. Every callback receives `context`.
 *
 *   context    Long-lived caller data; may be NULL. It must not point into
 *              the YanLine object, because the callbacks must not be able to
 *              overwrite the line they are helping to read. yan_line_init
 *              rejects an overlapping context.
 *   connected  Read-only query: non-zero when a terminal is attached. It must
 *              not block, consume a byte or change device state.
 *   ready      Read-only query: non-zero when get_byte can return a byte right
 *              now. It must not consume the byte.
 *   get_byte   Takes one byte when ready returned non-zero. Returns 1 and
 *              stores it, 0 when none was available after all, or a negative
 *              value when the device is unusable. It never blocks.
 *   put_byte   Echoes one byte. Returns true when accepted and false when the
 *              terminal refused it. It never blocks and never touches the
 *              line.
 *   arm_rx     Sets (enable true) or clears (enable false) the receive
 *              interrupt enable. The line reader masks before every read and
 *              re-arms before every wait; the callback must not block.
 *   ack_rx     Clears the arrival latch (write-one-to-clear). It must not read
 *              RXDATA and must not block.
 *   wait       Blocks the calling task on the device event. The runtime asks
 *              predicate(predicate_context) inside its critical section and
 *              returns at once when it is already true; otherwise it registers
 *              the caller and blocks until the event wakes it. A wake does not
 *              imply the predicate is true, so the reader rechecks the
 *              condition after wait returns. The predicate is short,
 *              read-only and called with interrupts off; wait itself never
 *              consumes a byte and never polls the device.
 *
 * A callback that does not match its contract is a caller bug; the reader
 * cannot enforce the contracts, only the detectable address rules above. */
typedef struct {
    void *context;
    int (*connected)(void *context);
    int (*ready)(void *context);
    int (*get_byte)(void *context, uint8_t *byte);
    bool (*put_byte)(void *context, uint8_t byte);
    void (*arm_rx)(void *context, bool enable);
    void (*ack_rx)(void *context);
    void (*wait)(void *context, int (*predicate)(void *), void *predicate_context);
} YanLineIo;

/* Memory context only. Callers zero-initialize it and then call
 * yan_line_init; the reader owns the fields and no caller writes them. The
 * terminal reads `busy` to refuse a close during a read, and the application
 * reads `buffer`/`length` after YAN_LINE_OK. `buffer` is last on purpose, so a
 * test can place a guard right after the object and detect a write past
 * buffer[1023]. */
typedef struct {
    YanLineIo io;
    bool initialized;
    bool busy;
    bool swallow_lf;
    uint32_t length;
    uint8_t buffer[YAN_LINE_BUFFER_SIZE];
} YanLine;

/* Validates every callback, the line pointer, and that io.context does not
 * overlap the line object; then records the callbacks. No device access. A
 * NULL line or a missing callback returns YAN_LINE_INVALID. Re-initializing an
 * idle instance is allowed; re-entering an instance that is inside
 * yan_line_next returns YAN_LINE_BUSY and changes nothing. */
YanLineResult yan_line_init(YanLine *, YanLineIo);

/* Reads one net line. On YAN_LINE_OK `buffer` holds `length` bytes followed by
 * a NUL. A line that was read but refused (TOO_LONG, INVALID_INPUT) or that
 * ended because the device became unavailable (UNAVAILABLE) comes back with an
 * empty buffer and `length` zero, so a refused line is never mistaken for a
 * short one. YAN_LINE_INVALID and YAN_LINE_BUSY are API-level refusals before
 * the reader touches the line: they leave the object unchanged. The
 * CR-swallows-LF state survives between calls. The reader masks the receive
 * interrupt before each read and re-arms before each wait; it masks and
 * acknowledges on every exit. */
YanLineResult yan_line_next(YanLine *);

#endif
