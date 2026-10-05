#ifndef YAN_OS_EDITOR_H
#define YAN_OS_EDITOR_H

#include "shell.h" /* YanShellOutput */
#include "yanfs.h"

#include <stdbool.h>
#include <stdint.h>

/* Multiline text editor core for docs/specs/0023-multiline-text-editor.md.
 *
 * This layer turns one caller-supplied editing line into one small in-memory
 * change of a UTF-8 draft, and on `w` into exactly one whole-file filesystem
 * call. It is the EDITING half of the application's SHELL/EDITING routing:
 * the application recognises `edit NAME`, calls yan_editor_start, and then
 * feeds every following line to yan_editor_execute until that call answers
 * YAN_EDITOR_EXIT (a healthy `w` or `q`). The editor borrows a YanFs and a
 * YanShellOutput; it owns neither the device, the terminal, the input line nor
 * the filesystem cache, and it includes shell.h only for the existing byte
 * sink type. os/shell.c is not changed by this layer and keeps its eight
 * commands.
 *
 * Draft and lifetime: the draft is YAN_EDITOR_CAPACITY bytes of raw text,
 * terminators included, and it lives in the YanEditor object. A YanEditor is
 * therefore larger than 16 KiB and must be static storage or another
 * long-lived, caller-owned object; it must not live on the 4 KiB task stack of
 * 0019, and no entry point copies the draft, a 1 KiB input line or a 4 KiB
 * filesystem block onto the stack. The object zero-initialized and passed to
 * yan_editor_init once is reused for every edit session; a later
 * yan_editor_start overwrites the previous draft after the new one has been
 * completely loaded and validated.
 *
 * Editing grammar for yan_editor_execute (the caller has already stripped the
 * line ending, exactly as in 0022):
 *
 *   a TEXT   append TEXT plus LF. When a non-empty draft ends without LF, one
 *            separator LF is written first so the old last line is not joined
 *            to the new one, then TEXT and the closing LF. The two LFs and
 *            TEXT are charged to the capacity together. TEXT may be empty.
 *   r N TEXT replace the content of line N, keeping that line's terminator:
 *            LF, CRLF and "no terminator" all stay exactly as they were. A
 *            replacement of a final line with no LF does not add one.
 *   d N      remove line N and its terminator; removing an unterminated final
 *            line removes only its content. The draft may become empty.
 *   p        print every line as "<1-based number> <safe TEXT>\r\n" and then
 *            "OK p\r\n". Display escapes do not modify the draft.
 *   w        save the whole draft with one create or replace call, then leave
 *            editing on success.
 *   q        discard the draft, touch no medium, and leave editing.
 *
 * N is a 1-based decimal number. The commands, the number and TEXT use ASCII
 * spaces; for `a` one space after the command and for `r` one space after the
 * number are consumed, and every following byte is TEXT - extra leading and
 * trailing spaces are kept. An empty line only asks for the next prompt.
 *
 * Input: a line is at most YAN_EDITOR_LINE_MAX bytes, and the whole line is
 * refused - not executed as a prefix - when it is longer or when it contains a
 * C0 byte or DEL. A typed TAB is refused here; TAB that is already inside a
 * loaded file is legal text and is preserved.
 *
 * Text rules: the draft only ever holds valid UTF-8 without NUL, other C0
 * controls, DEL or the C1 controls U+0080..U+009F. CR is allowed only as the
 * first byte of a CRLF; a bare CR, an overlong or surrogate sequence, a
 * codepoint past U+10FFFF, a lone continuation byte or a truncated sequence
 * rejects the text. Nothing is dropped, replaced or normalized. This applies
 * both to a loaded file and to the TEXT of `a`/`r` (a byte-wise backspace can
 * leave broken UTF-8 in the input line, and that must not enter the draft).
 *
 * Capacity: the draft, terminators included, is at most
 * YAN_EDITOR_CAPACITY bytes. Every `a` and `r` computes the whole resulting
 * length first and refuses the command without touching a byte when it would
 * not fit, so a capacity failure can never leave a partial edit, and an
 * oversize file is refused whole rather than truncated. `d` only shrinks.
 *
 * Output and errors: a successful command prints "OK <command>\r\n"; a normal
 * refusal prints "ERROR <TOKEN>\r\n" with a stable token and keeps the draft
 * and the editing session. The tokens owned by this layer are:
 *
 *   USAGE, UNKNOWN_COMMAND       malformed command or a line-number token that
 *                                is not an unsigned decimal number or that
 *                                overflows uint32_t
 *   OUT_OF_RANGE                 N is zero or past the last line
 *   TOO_LARGE                    the loaded file or the resulting draft would
 *                                exceed YAN_EDITOR_CAPACITY
 *   INVALID_TEXT                 loaded or typed bytes are not legal draft text
 *   LINE_TOO_LONG, INVALID_INPUT the whole-line input limits above
 *   INVALID, BUSY, EXISTS, NOT_FOUND, DIRECTORY_FULL, NOSPACE   the borrowed
 *                                filesystem's own results, passed through
 *
 * A filesystem result that forbids continuing - NOT_MOUNTED, FAULTED, IO,
 * PROTOCOL, CORRUPT, UNSUPPORTED - is reported and then stops: the same fatal
 * classification 0022 uses. A never-initialized filesystem is the ordinary
 * INVALID case, matching the existing shell health guard; the production
 * application initializes the filesystem first. Ordinary save errors
 * (EXISTS, NOT_FOUND, BUSY, NOSPACE, ...) keep the whole draft and the
 * session, so the caller may edit or `q`. A failed output byte stops every
 * later byte and filesystem call and is fatal; a `w` whose filesystem commit
 * already succeeded may have changed the medium when its confirmation byte
 * then fails, and that save is never rolled back or retried.
 *
 * The existing flag is fixed by yan_editor_start: a name that existed is saved
 * with replace and a name that did not exist is saved with create, for the
 * whole session. An EXISTS from create or a NOT_FOUND from replace is an
 * ordinary error that keeps the draft; the operation is never silently
 * switched to the other one. `q` makes no filesystem call at all.
 *
 * Concurrency and addresses: both entry points mark the instance busy for the
 * whole call, filesystem callbacks and output callbacks included. A reentrant
 * call made from either callback returns YAN_EDITOR_BUSY without emitting a
 * byte or touching the filesystem. yan_editor_init rejects a borrowed
 * filesystem that overlaps the editor by address arithmetic before it reads a
 * bool field of either context; yan_editor_start and yan_editor_execute reject
 * a name/line that overlaps the editor or the filesystem and a range that
 * would leave uintptr_t, before any output or mutation. */

/* Draft capacity in bytes, terminators included. It is a byte budget, not a
 * character budget. */
#define YAN_EDITOR_CAPACITY UINT32_C(16384)

/* Input line limit, in bytes, of one editing command. */
#define YAN_EDITOR_LINE_MAX UINT32_C(1023)

typedef enum {
    /* The call was handled. The editing session may still be active, and a
     * normal ERROR line may have been produced; the caller keeps routing lines
     * to the editor while yan_editor_active() stays true. */
    YAN_EDITOR_OK = 0,
    /* A healthy `w` or `q` left editing. Nothing else returns this value, and
     * yan_editor_active() is false. The caller returns to SHELL. */
    YAN_EDITOR_EXIT = 1,
    /* The filesystem state or the output channel forbids continuing. `w` may
     * already have changed the medium; nothing is rolled back. */
    YAN_EDITOR_FATAL = 2,
    /* The call itself was invalid: a null or uninitialized editor, an inactive
     * session asked to execute, or an address range this API can prove is
     * unusable. No output and no filesystem call. */
    YAN_EDITOR_INVALID = 3,
    /* A reentrant call while another call owns the instance. No output, no
     * filesystem call, and the outer call is unaffected. */
    YAN_EDITOR_BUSY = 4
} YanEditorResult;

/* Memory context only. Callers zero-initialize it, pass it to
 * yan_editor_init, and never edit the fields afterwards; the implementation
 * owns them. `draft` is last on purpose, so a test can place a guard right
 * after it and detect a write past draft[YAN_EDITOR_CAPACITY - 1]. */
typedef struct {
    YanFs *fs;
    YanShellOutput output;
    bool initialized;
    bool busy;
    bool active;
    bool existing;
    uint32_t length;
    char name[YAN_FS_NAME_MAX + 1u];
    uint8_t draft[YAN_EDITOR_CAPACITY];
} YanEditor;

/* Validates the editor, the borrowed filesystem, output.putc and that the
 * filesystem does not overlap the editor; then records the borrowed objects.
 * No filesystem I/O and no output. The overlap test is pure address arithmetic
 * and runs before any bool field of either context is read. A null editor, a
 * null fs or a null putc returns YAN_EDITOR_INVALID; re-initializing an
 * instance that is currently inside a call returns YAN_EDITOR_BUSY; an idle
 * instance may be re-initialized (the current session, if any, is dropped). */
YanEditorResult yan_editor_init(YanEditor *, YanFs *, YanShellOutput);

/* Starts an edit session for `name` (name_length bytes, not NUL-terminated),
 * along 0021's 1..31 byte ASCII name rules; the borrowed filesystem is the
 * single judge of the name. An existing file is loaded completely and
 * validated before the session becomes active; a missing name starts an empty
 * draft and creates nothing on disk until `w`. A name that is too long or
 * otherwise invalid, a busy filesystem, an oversize file, invalid text or a
 * failed load reports a normal ERROR and leaves the session inactive, so a
 * failed or partial load can never be saved. On success the draft is active
 * and "OK edit\r\n" is printed. A fatal filesystem or output failure returns
 * YAN_EDITOR_FATAL. Calling start while a session is already active is
 * YAN_EDITOR_INVALID. */
YanEditorResult yan_editor_start(YanEditor *, const uint8_t *name,
                                 uint32_t name_length);

/* Runs one editing line, already stripped of CR/LF by the caller's line
 * reader. line points at length readable bytes; length may be zero, in which
 * case line may be NULL, and that only asks for the next prompt. The API
 * rejects a line that overlaps the editor or the borrowed filesystem and one
 * whose range would leave uintptr_t, with YAN_EDITOR_INVALID before any output
 * or filesystem call. The call requires an active session. */
YanEditorResult yan_editor_execute(YanEditor *, const uint8_t *line,
                                   uint32_t length);

/* True while an edit session is active, i.e. while the caller routes input to
 * yan_editor_execute instead of to the shell. False for NULL and for an
 * uninitialized editor. */
bool yan_editor_active(const YanEditor *);

#endif
