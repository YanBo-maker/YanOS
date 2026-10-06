#ifndef YAN_OS_SHELL_H
#define YAN_OS_SHELL_H

#include "yanfs.h"

#include <stdbool.h>
#include <stdint.h>

/* Terminal command execution layer.
 *
 * This file fixes the dispatcher side of 0022: one caller-supplied line of
 * raw bytes in, command output out, and a small result enum that tells the
 * caller whether to keep running, stop healthily, or stop because the
 * filesystem or the output channel can no longer be trusted. 0024 extends the
 * command set with the two-name `mv OLD NEW` and `cp SRC DEST`; the interface
 * below is unchanged by that stage.
 *
 * The layer performs no Host I/O of its own. Every produced byte goes through
 * the borrowed YanShellOutput callback; every file operation goes through a
 * borrowed YanFs. A YanShell therefore owns neither the device nor the
 * terminal, and it is usable from the Guest as well as from a native test.
 *
 * Lifetime and stack: a YanShell is long-lived (static storage or a
 * caller-owned object). Its 256-byte scratch is the fixed read chunk of cat,
 * and nothing in the layer copies a 1024-byte line or a 4096-byte filesystem
 * block onto the stack, so the 4 KiB task stack of 0019 stays untouched.
 *
 * Concurrency: yan_shell_execute is not reentrant. It marks the instance busy
 * for the whole call, so a reentrant call made from inside the output callback
 * or from inside a filesystem block callback returns YAN_SHELL_BUSY without
 * emitting a byte or touching the filesystem. The caller, not this layer, is
 * responsible for keeping those callbacks from reordering or editing the
 * borrowed YanFs and the line buffer. */

/* A line longer than this is refused as LINE_TOO_LONG before any parse. The
 * limit counts every byte of the line, and the line is not NUL-terminated:
 * zero bytes are refused as invalid input, not treated as a terminator. */
#define YAN_SHELL_LINE_MAX UINT32_C(1023)

/* Size of YanShell.scratch. cat reads the file in chunks of this many bytes,
 * which keeps the whole-block filesystem block off the task stack. */
#define YAN_SHELL_SCRATCH_SIZE 256u

typedef enum {
    /* The session continues. A command may still have produced a normal
     * ERROR line, for example UNKNOWN_COMMAND, USAGE *, NOT_FOUND or BUSY. */
    YAN_SHELL_OK = 0,
    /* A healthy `exit`. Nothing else returns this value. */
    YAN_SHELL_EXIT = 1,
    /* The filesystem reported a state that forbids continuing, or the output
     * channel failed. Only the last attempted output is guaranteed to have
     * happened; a write command may already have changed the filesystem. */
    YAN_SHELL_FATAL = 2,
    /* The call itself was invalid: a null or uninitialized shell, or a line
     * buffer that this API can prove is not addressable. No output, no I/O. */
    YAN_SHELL_INVALID = 3,
    /* A reentrant call while another call owns the instance. No output, no
     * I/O, and the outer call is unaffected. */
    YAN_SHELL_BUSY = 4
} YanShellResult;

/* Caller-owned byte sink. context may be NULL; putc returning false stops the
 * current command immediately and makes yan_shell_execute return
 * YAN_SHELL_FATAL. The callback must not modify the borrowed YanFs or the
 * line buffer, because the shell may still be reading them. */
typedef struct {
    void *context;
    bool (*putc)(void *context, uint8_t byte);
} YanShellOutput;

/* Memory context only. The fields are private to os/shell.c; callers
 * zero-initialize before yan_shell_init and never edit them afterwards. */
typedef struct {
    YanFs *fs;
    YanShellOutput output;
    bool initialized;
    bool busy;
    uint8_t scratch[YAN_SHELL_SCRATCH_SIZE];
} YanShell;

/* Validates shell, fs, output.putc and that the borrowed filesystem does not
 * overlap the shell object, then records the borrowed objects. No filesystem
 * I/O and no output. The overlap test is pure address arithmetic and runs
 * before any bool field of either context is read, so a shell placed inside the
 * filesystem is rejected without a load that could be undefined. A null shell,
 * a null fs or a null putc returns YAN_SHELL_INVALID. Re-initializing an
 * instance that is currently inside yan_shell_execute returns YAN_SHELL_BUSY;
 * an idle instance may be re-initialized. */
YanShellResult yan_shell_init(YanShell *, YanFs *, YanShellOutput);

/* Runs one net line, already stripped of CR/LF by the caller's line reader.
 *
 * line points at length bytes that are legal to read; length may be zero, in
 * which case line may be NULL. The API rejects a line that overlaps the whole
 * YanShell or the whole YanFs, and one whose [line, line + length) range would
 * leave the uintptr_t address space, with YAN_SHELL_INVALID before any output
 * or filesystem call. What the caller does beyond that addressability is the
 * caller's precondition.
 *
 * The line length and control-byte checks run first and touch no filesystem
 * state. Only then does a read-only look at the borrowed filesystem's state
 * fields decide that the session may continue: it must be initialized, mounted
 * and not faulted, and otherwise the call reports that state - with
 * YAN_SHELL_FATAL for a state that forbids continuing. A filesystem that was
 * never initialized (YanFs.initialized false) is reported as the ordinary
 * INVALID-class error, not as a new fatal state. An initialized but unmounted
 * filesystem reports fatal NOT_MOUNTED. The production terminal initializes it
 * before the first line, so the path is unreachable there. A healthy `exit` in
 * particular cannot be reported on an unmounted or faulted filesystem, and a
 * NULL zero-length line follows the same path as a non-NULL empty line so a
 * fatal filesystem cannot be continued by pressing Enter.
 *
 * The line is not modified. A zero byte or a DEL byte, like any other C0
 * control byte, rejects the whole line as INVALID_INPUT so the bytes on either
 * side are never spliced into a command. */
YanShellResult yan_shell_execute(YanShell *, const uint8_t *line, uint32_t length);

#endif
