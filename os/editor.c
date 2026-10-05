/* Multiline text editor core: docs/specs/0023-multiline-text-editor.md.
 *
 * The contract is os/editor.h. The shape of this translation unit is decided
 * by three properties of that contract:
 *
 *   validate, then move  every `a` and `r` computes the complete resulting
 *   length first and refuses the command before touching a byte when it would
 *   exceed the capacity, and the TEXT is checked as legal draft text before
 *   the move. A refused capacity or input failure therefore leaves the whole
 *   draft byte-identical. `d` only shrinks.
 *
 *   one whole-file save  `w` calls create or replace exactly once, chosen by
 *   the existing flag recorded when the session started, and only a success
 *   leaves editing. `q`, `p`, `a`, `r` and `d` make no filesystem call.
 *
 *   terminator fidelity  the draft is raw bytes. LF, CRLF and "no final
 *   terminator" survive loading, a plain append and a replacement; only `a`
 *   adds terminators and only `d` removes them. Display escaping happens in
 *   `p` and never touches the draft.
 *
 * The line model of 0023 is reproduced by editor_find_line: LF and CRLF end a
 * line, the terminator is not part of the content, the bytes before a bare CR
 * are content, and the bytes after the last terminator form a final line with
 * no terminator. An empty file has no line at all.
 *
 * Filesystem and output are borrowed. The editor never copies a YanFs, never
 * allocates a whole block or a copy of the draft, and reaches the medium only
 * through the borrowed instance's public API. */
#include "editor.h"

#include <stddef.h>

#define EDITOR_LF UINT8_C(0x0a)
#define EDITOR_CR UINT8_C(0x0d)

/* --------------------------------------------------------------- addresses */

/* True when the two byte ranges intersect. A range that would leave uintptr_t
 * is reported as overlapping, which is the conservative answer the parameter
 * checks need for a pointer plus a huge length. Nothing is read or written
 * through either range. */
static bool ranges_overlap(uintptr_t first_begin, uint64_t first_length,
                           uintptr_t second_begin, uint64_t second_length)
{
    if (first_length == 0u || second_length == 0u) {
        return false;
    }
    if (first_length > (uint64_t)(UINTPTR_MAX - first_begin) ||
        second_length > (uint64_t)(UINTPTR_MAX - second_begin)) {
        return true;
    }
    uintptr_t first_end = first_begin + (uintptr_t)first_length;
    uintptr_t second_end = second_begin + (uintptr_t)second_length;
    return first_begin < second_end && second_begin < first_end;
}

/* Moves `length` bytes inside the draft from index `source` to index
 * `destination`. Both are checked subranges of that one draft, so an
 * overlapping move is safe when the copy runs in the direction that keeps the
 * not-yet-copied bytes intact: forward when the destination is lower, backward
 * when it is higher, and nothing at all when the two are equal or the length
 * is zero.
 *
 * The freestanding Guest must not require libc for this move.
 * The helper is static and bounded, allocates
 * no temporary (never a 16384-byte or 4096-byte object) and is the same code
 * in the native tests and the freestanding ELF, so both exercise one
 * implementation. */
static void draft_move(uint8_t *draft, uint32_t source, uint32_t destination,
                       uint32_t length)
{
    if (length == 0u || source == destination) {
        return;
    }
    if (destination < source) {
        for (uint32_t i = 0u; i < length; ++i) {
            draft[destination + i] = draft[source + i];
        }
    } else {
        for (uint32_t i = length; i > 0u; --i) {
            draft[destination + (i - 1u)] = draft[source + (i - 1u)];
        }
    }
}

/* ---------------------------------------------------------------- output */

static bool editor_putc(YanEditor *editor, uint8_t byte)
{
    return editor->output.putc(editor->output.context, byte);
}

static bool editor_put_bytes(YanEditor *editor, const uint8_t *bytes,
                             uint32_t length)
{
    for (uint32_t i = 0; i < length; ++i) {
        if (!editor_putc(editor, bytes[i])) {
            return false;
        }
    }
    return true;
}

static bool editor_puts(YanEditor *editor, const char *text)
{
    for (uint32_t i = 0; text[i] != '\0'; ++i) {
        if (!editor_putc(editor, (uint8_t)text[i])) {
            return false;
        }
    }
    return true;
}

static bool editor_put_decimal(YanEditor *editor, uint32_t value)
{
    uint8_t digits[10];
    uint32_t count = 0u;
    do {
        digits[count] = (uint8_t)('0' + (value % 10u));
        ++count;
        value /= 10u;
    } while (value != 0u);
    while (count > 0u) {
        --count;
        if (!editor_putc(editor, digits[count])) {
            return false;
        }
    }
    return true;
}

/* One byte as \xHH with uppercase hexadecimal, the same spelling 0022's cat
 * uses. The short-circuit chain is what keeps a later putc from running after
 * a refused byte. */
static bool editor_put_hex_byte(YanEditor *editor, uint8_t byte)
{
    static const char hex[] = "0123456789ABCDEF";
    return editor_putc(editor, (uint8_t)'\\') &&
           editor_putc(editor, (uint8_t)'x') &&
           editor_putc(editor, (uint8_t)hex[byte >> 4]) &&
           editor_putc(editor, (uint8_t)hex[byte & 0x0Fu]);
}

/* ------------------------------------------------------------ status lines */

/* Stable suffix of the ERROR line for a borrowed filesystem result. Every
 * enumerator is named so a compiler can tell the mapping is complete. */
static const char *fs_error_suffix(YanFsResult result)
{
    switch (result) {
    case YAN_FS_INVALID: return "INVALID";
    case YAN_FS_NOT_MOUNTED: return "NOT_MOUNTED";
    case YAN_FS_FAULTED: return "FAULTED";
    case YAN_FS_BUSY: return "BUSY";
    case YAN_FS_EXISTS: return "EXISTS";
    case YAN_FS_NOT_FOUND: return "NOT_FOUND";
    case YAN_FS_DIRECTORY_FULL: return "DIRECTORY_FULL";
    case YAN_FS_NOSPACE: return "NOSPACE";
    case YAN_FS_CORRUPT: return "CORRUPT";
    case YAN_FS_UNSUPPORTED: return "UNSUPPORTED";
    case YAN_FS_IO: return "IO";
    case YAN_FS_PROTOCOL: return "PROTOCOL";
    case YAN_FS_END: return "END";
    case YAN_FS_OK: return "OK";
    }
    return "UNKNOWN";
}

/* The same split 0022 uses: an unmounted, faulted, I/O, protocol, corrupt or
 * unsupported filesystem ends the application; the remaining results are
 * ordinary errors that keep the session alive. */
static bool fs_error_is_fatal(YanFsResult result)
{
    switch (result) {
    case YAN_FS_NOT_MOUNTED:
    case YAN_FS_FAULTED:
    case YAN_FS_IO:
    case YAN_FS_PROTOCOL:
    case YAN_FS_CORRUPT:
    case YAN_FS_UNSUPPORTED:
        return true;
    case YAN_FS_OK:
    case YAN_FS_END:
    case YAN_FS_INVALID:
    case YAN_FS_BUSY:
    case YAN_FS_EXISTS:
    case YAN_FS_NOT_FOUND:
    case YAN_FS_DIRECTORY_FULL:
    case YAN_FS_NOSPACE:
        return false;
    }
    return true;
}

/* Prints "ERROR <suffix>\r\n" and maps the filesystem result to the session
 * verdict. An output failure is always fatal; an ordinary error continues. */
static YanEditorResult editor_finish_fs_error(YanEditor *editor,
                                              YanFsResult result)
{
    if (!editor_puts(editor, "ERROR ") ||
        !editor_puts(editor, fs_error_suffix(result)) ||
        !editor_puts(editor, "\r\n")) {
        return YAN_EDITOR_FATAL;
    }
    return fs_error_is_fatal(result) ? YAN_EDITOR_FATAL : YAN_EDITOR_OK;
}

/* An ordinary error owned by the editor itself. A refused output byte turns
 * it into a fatal result. */
static YanEditorResult editor_emit_own_error(YanEditor *editor,
                                             const char *token)
{
    if (!editor_puts(editor, "ERROR ") || !editor_puts(editor, token) ||
        !editor_puts(editor, "\r\n")) {
        return YAN_EDITOR_FATAL;
    }
    return YAN_EDITOR_OK;
}

static bool editor_emit_ok(YanEditor *editor, const char *command)
{
    return editor_puts(editor, "OK ") && editor_puts(editor, command) &&
           editor_puts(editor, "\r\n");
}

/* A read-only look at the borrowed filesystem's public state, mirroring the
 * shell's health guard and the filesystem's own precedence: uninitialized,
 * then faulted, then not mounted. `busy` is deliberately not health, exactly
 * as in 0022: a command running while another filesystem operation is in
 * flight still reports BUSY from the failed call. No filesystem API is called
 * here, so the guard is also correct for the memory-only commands. */
static YanFsResult editor_fs_health(const YanEditor *editor)
{
    const YanFs *fs = editor->fs;
    if (!fs->initialized) {
        return YAN_FS_INVALID;
    }
    if (fs->state == YAN_FS_STATE_FAULTED) {
        return YAN_FS_FAULTED;
    }
    if (fs->state != YAN_FS_MOUNTED) {
        return YAN_FS_NOT_MOUNTED;
    }
    return YAN_FS_OK;
}

/* ------------------------------------------------------------------- UTF-8 */

/* 2, 3 or 4 for a legal lead byte, 0 for anything that cannot start a
 * sequence. C0 and C1 are excluded here, which is what rejects overlong
 * two-byte forms and the whole U+0080..U+00BF lead range. */
static uint32_t utf8_lead_length(uint8_t lead)
{
    if (lead >= 0xC2u && lead <= 0xDFu) {
        return 2u;
    }
    if (lead >= 0xE0u && lead <= 0xEFu) {
        return 3u;
    }
    if (lead >= 0xF0u && lead <= 0xF4u) {
        return 4u;
    }
    return 0u;
}

/* The caller has already confirmed the lead byte and that every following byte
 * is a continuation. Only the three range rules that separate a legal sequence
 * from an overlong, surrogate or out-of-range one remain. */
static bool utf8_sequence_valid(const uint8_t *bytes, uint32_t length)
{
    if (length == 3u) {
        if (bytes[0] == 0xE0u) {
            return bytes[1] >= 0xA0u && bytes[1] <= 0xBFu;
        }
        if (bytes[0] == 0xEDu) {
            return bytes[1] >= 0x80u && bytes[1] <= 0x9Fu;
        }
    } else if (length == 4u) {
        if (bytes[0] == 0xF0u) {
            return bytes[1] >= 0x90u && bytes[1] <= 0xBFu;
        }
        if (bytes[0] == 0xF4u) {
            return bytes[1] >= 0x80u && bytes[1] <= 0x8Fu;
        }
    }
    return true;
}

static uint32_t utf8_codepoint(const uint8_t *bytes, uint32_t length)
{
    if (length == 2u) {
        return ((uint32_t)(bytes[0] & 0x1Fu) << 6) |
               (uint32_t)(bytes[1] & 0x3Fu);
    }
    if (length == 3u) {
        return ((uint32_t)(bytes[0] & 0x0Fu) << 12) |
               ((uint32_t)(bytes[1] & 0x3Fu) << 6) |
               (uint32_t)(bytes[2] & 0x3Fu);
    }
    return ((uint32_t)(bytes[0] & 0x07u) << 18) |
           ((uint32_t)(bytes[1] & 0x3Fu) << 12) |
           ((uint32_t)(bytes[2] & 0x3Fu) << 6) |
           (uint32_t)(bytes[3] & 0x3Fu);
}

/* Validates one non-ASCII sequence starting at bytes[i] and returns its
 * length, or 0 when the bytes there are not legal draft text: a bad lead, a
 * missing or non-continuation byte, an overlong or surrogate form, a codepoint
 * past U+10FFFF, or a C1 control. */
static uint32_t valid_utf8_run(const uint8_t *bytes, uint32_t length,
                               uint32_t i)
{
    uint32_t need = utf8_lead_length(bytes[i]);
    if (need == 0u || (uint64_t)need > (uint64_t)(length - i)) {
        return 0u;
    }
    for (uint32_t j = 1u; j < need; ++j) {
        if (bytes[i + j] < 0x80u || bytes[i + j] > 0xBFu) {
            return 0u;
        }
    }
    if (!utf8_sequence_valid(bytes + i, need)) {
        return 0u;
    }
    uint32_t codepoint = utf8_codepoint(bytes + i, need);
    if (codepoint >= 0x80u && codepoint <= 0x9Fu) {
        return 0u;
    }
    return need;
}

/* True when `bytes` is legal typed TEXT: valid UTF-8, no C0 or DEL, and no C1
 * control codepoint. A byte-wise backspace can leave a broken sequence in the
 * input line, and that must not enter the draft. The input line's own
 * whole-line scan already refuses typed LF, CR and TAB, so this is the strict
 * form of the same text rules. */
static bool text_is_valid(const uint8_t *bytes, uint32_t length)
{
    uint32_t i = 0u;
    while (i < length) {
        uint8_t byte = bytes[i];
        if (byte < 0x80u) {
            if (byte < 0x20u || byte == 0x7Fu) {
                return false;
            }
            ++i;
            continue;
        }
        uint32_t run = valid_utf8_run(bytes, length, i);
        if (run == 0u) {
            return false;
        }
        i += run;
    }
    return true;
}

/* True when `bytes` is a legal draft, which is the text rule plus the
 * terminators the medium may already hold: LF anywhere, CR only as the first
 * byte of a CRLF, and an existing TAB. Other C0 bytes, DEL and the C1 controls
 * still reject the whole file, and a multibyte sequence never contains a byte
 * below 0x80, so the LF/CR/TAB special cases cannot be confused with content. */
static bool draft_is_valid(const uint8_t *bytes, uint32_t length)
{
    uint32_t i = 0u;
    while (i < length) {
        uint8_t byte = bytes[i];
        if (byte == EDITOR_LF) {
            ++i;
            continue;
        }
        if (byte == EDITOR_CR) {
            if (i + 1u < length && bytes[i + 1u] == EDITOR_LF) {
                i += 2u;
                continue;
            }
            return false;
        }
        if (byte == UINT8_C(0x09)) {
            ++i;
            continue;
        }
        if (byte < 0x80u) {
            if (byte < 0x20u || byte == 0x7Fu) {
                return false;
            }
            ++i;
            continue;
        }
        uint32_t run = valid_utf8_run(bytes, length, i);
        if (run == 0u) {
            return false;
        }
        i += run;
    }
    return true;
}

/* ---------------------------------------------------------- display bytes */

static bool editor_emit_escaped(YanEditor *editor, const uint8_t *bytes,
                                uint32_t length)
{
    for (uint32_t i = 0; i < length; ++i) {
        if (!editor_put_hex_byte(editor, bytes[i])) {
            return false;
        }
    }
    return true;
}

/* 0022's safe byte display, used by `p`. Printable ASCII passes through
 * except the backslash, which doubles so the \xHH escapes stay unambiguous;
 * C0, DEL and every byte of an invalid sequence become \xHH; legal UTF-8
 * passes through except the C1 controls, which are escaped byte by byte. The
 * draft is guaranteed legal, so in practice only TAB and the backslash need
 * this path, but the invalid cases are handled too so a display bug can never
 * leak a control byte. A broken sequence is escaped one byte at a time and the
 * next byte is examined from a clean state, so nothing after it is lost. */
static bool editor_emit_safe_text(YanEditor *editor, const uint8_t *bytes,
                                  uint32_t length)
{
    uint32_t i = 0u;
    while (i < length) {
        uint8_t byte = bytes[i];
        if (byte < 0x80u) {
            if (byte == (uint8_t)'\\') {
                if (!editor_putc(editor, byte) || !editor_putc(editor, byte)) {
                    return false;
                }
            } else if (byte >= 0x20u && byte <= 0x7Eu) {
                if (!editor_putc(editor, byte)) {
                    return false;
                }
            } else if (!editor_put_hex_byte(editor, byte)) {
                return false;
            }
            ++i;
            continue;
        }
        uint32_t need = utf8_lead_length(byte);
        bool complete = need != 0u && (uint64_t)need <= (uint64_t)(length - i);
        if (complete) {
            for (uint32_t j = 1u; j < need; ++j) {
                if (bytes[i + j] < 0x80u || bytes[i + j] > 0xBFu) {
                    complete = false;
                    break;
                }
            }
        }
        if (complete && utf8_sequence_valid(bytes + i, need)) {
            uint32_t codepoint = utf8_codepoint(bytes + i, need);
            if (codepoint >= 0x80u && codepoint <= 0x9Fu) {
                if (!editor_emit_escaped(editor, bytes + i, need)) {
                    return false;
                }
            } else if (!editor_put_bytes(editor, bytes + i, need)) {
                return false;
            }
            i += need;
            continue;
        }
        if (!editor_put_hex_byte(editor, byte)) {
            return false;
        }
        ++i;
    }
    return true;
}

/* ------------------------------------------------------------ line model */

typedef struct {
    uint32_t content_start;
    uint32_t content_len;
    uint32_t eol_len; /* 0, 1 (LF) or 2 (CRLF) */
} EditorLine;

/* True when line `number` (1-based) exists, and fills its span. The scan walks
 * LF-terminated and CRLF-terminated lines, then a final unterminated line if
 * bytes remain; a terminating LF never starts an extra empty line. */
static bool editor_find_line(const uint8_t *draft, uint32_t length,
                             uint32_t number, EditorLine *out)
{
    if (number == 0u) {
        return false;
    }
    uint32_t index = 0u;
    uint32_t position = 0u;
    while (position < length) {
        ++index;
        uint32_t start = position;
        while (position < length && draft[position] != EDITOR_LF) {
            ++position;
        }
        uint32_t content_end = position;
        uint32_t eol_len = 0u;
        if (position < length) {
            eol_len = 1u;
            if (content_end > start && draft[content_end - 1u] == EDITOR_CR) {
                content_end -= 1u;
                eol_len = 2u;
            }
            ++position;
        }
        if (index == number) {
            out->content_start = start;
            out->content_len = content_end - start;
            out->eol_len = eol_len;
            return true;
        }
    }
    return false;
}

/* -------------------------------------------------------------- parsing */

static uint32_t skip_spaces(const uint8_t *line, uint32_t length,
                            uint32_t position)
{
    while (position < length && line[position] == (uint8_t)' ') {
        ++position;
    }
    return position;
}

static bool rest_is_spaces(const uint8_t *line, uint32_t length,
                           uint32_t position)
{
    return skip_spaces(line, length, position) == length;
}

static bool token_is(const uint8_t *line, uint32_t start, uint32_t length,
                     const char *text)
{
    uint32_t i = 0u;
    while (text[i] != '\0') {
        if (i >= length || line[start + i] != (uint8_t)text[i]) {
            return false;
        }
        ++i;
    }
    return i == length;
}

/* Converts an already bounded, NUL-free token to the filesystem's C-string
 * interface. The filesystem still checks its ASCII name rules. */
static void copy_name(const uint8_t *name, uint32_t length,
                      char out[YAN_FS_NAME_MAX + 2u])
{
    for (uint32_t i = 0; i < length; ++i) {
        out[i] = (char)name[i];
    }
    out[length] = '\0';
}

/* Parses a 1-based line number as unsigned decimal. An empty token, a
 * non-digit byte or a value that would overflow uint32_t is a syntax error. */
static bool parse_line_number(const uint8_t *line, uint32_t start, uint32_t end,
                              uint32_t *value)
{
    if (start == end) {
        return false;
    }
    uint32_t parsed = 0u;
    for (uint32_t i = start; i < end; ++i) {
        uint8_t byte = line[i];
        if (byte < (uint8_t)'0' || byte > (uint8_t)'9') {
            return false;
        }
        uint32_t digit = (uint32_t)(byte - (uint8_t)'0');
        if (parsed > (UINT32_MAX - digit) / 10u) {
            return false;
        }
        parsed = parsed * 10u + digit;
    }
    *value = parsed;
    return true;
}

/* -------------------------------------------------------------- commands */

static YanEditorResult run_append(YanEditor *editor, const uint8_t *text,
                                  uint32_t text_length)
{
    /* A non-empty draft that does not end in LF gets one separator LF so the
     * old last line is not joined to the appended one. The separator, TEXT and
     * the closing LF are charged together, before any byte moves. */
    bool separator =
        editor->length > 0u && editor->draft[editor->length - 1u] != EDITOR_LF;
    uint32_t added = (separator ? 1u : 0u) + text_length + 1u;
    if (added > YAN_EDITOR_CAPACITY - editor->length) {
        return editor_emit_own_error(editor, "TOO_LARGE");
    }
    if (!text_is_valid(text, text_length)) {
        return editor_emit_own_error(editor, "INVALID_TEXT");
    }
    uint32_t at = editor->length;
    if (separator) {
        editor->draft[at] = EDITOR_LF;
        ++at;
    }
    for (uint32_t i = 0; i < text_length; ++i) {
        editor->draft[at + i] = text[i];
    }
    at += text_length;
    editor->draft[at] = EDITOR_LF;
    ++at;
    editor->length = at;
    return editor_emit_ok(editor, "a") ? YAN_EDITOR_OK : YAN_EDITOR_FATAL;
}

static YanEditorResult run_replace(YanEditor *editor, uint32_t number,
                                   const uint8_t *text, uint32_t text_length)
{
    EditorLine line;
    if (!editor_find_line(editor->draft, editor->length, number, &line)) {
        return editor_emit_own_error(editor, "OUT_OF_RANGE");
    }
    /* Only the content length changes; the old terminator keeps its length, so
     * the complete result is known before the move. */
    uint32_t new_length = editor->length - line.content_len + text_length;
    if (new_length > YAN_EDITOR_CAPACITY) {
        return editor_emit_own_error(editor, "TOO_LARGE");
    }
    if (!text_is_valid(text, text_length)) {
        return editor_emit_own_error(editor, "INVALID_TEXT");
    }
    /* Move the old terminator and everything after it as one tail. The new
     * content is written from the caller's line buffer afterwards, so a
     * forward or backward overlap of the tail move cannot corrupt it, and the
     * terminator is carried along instead of being rewritten. */
    uint32_t tail_start = line.content_start + line.content_len;
    uint32_t tail_length = editor->length - tail_start;
    uint32_t tail_dest = line.content_start + text_length;
    draft_move(editor->draft, tail_start, tail_dest, tail_length);
    for (uint32_t i = 0; i < text_length; ++i) {
        editor->draft[line.content_start + i] = text[i];
    }
    editor->length = new_length;
    return editor_emit_ok(editor, "r") ? YAN_EDITOR_OK : YAN_EDITOR_FATAL;
}

static YanEditorResult run_delete(YanEditor *editor, uint32_t number)
{
    EditorLine line;
    if (!editor_find_line(editor->draft, editor->length, number, &line)) {
        return editor_emit_own_error(editor, "OUT_OF_RANGE");
    }
    uint32_t remove_start = line.content_start;
    uint32_t remove_end = line.content_start + line.content_len + line.eol_len;
    uint32_t tail_length = editor->length - remove_end;
    draft_move(editor->draft, remove_end, remove_start, tail_length);
    editor->length = remove_start + tail_length;
    return editor_emit_ok(editor, "d") ? YAN_EDITOR_OK : YAN_EDITOR_FATAL;
}

static YanEditorResult run_print(YanEditor *editor)
{
    uint32_t position = 0u;
    uint32_t number = 0u;
    while (position < editor->length) {
        ++number;
        uint32_t start = position;
        while (position < editor->length &&
               editor->draft[position] != EDITOR_LF) {
            ++position;
        }
        uint32_t content_end = position;
        if (position < editor->length) {
            if (content_end > start &&
                editor->draft[content_end - 1u] == EDITOR_CR) {
                content_end -= 1u;
            }
            ++position;
        }
        if (!editor_put_decimal(editor, number) ||
            !editor_putc(editor, (uint8_t)' ') ||
            !editor_emit_safe_text(editor, editor->draft + start,
                                   content_end - start) ||
            !editor_puts(editor, "\r\n")) {
            return YAN_EDITOR_FATAL;
        }
    }
    return editor_emit_ok(editor, "p") ? YAN_EDITOR_OK : YAN_EDITOR_FATAL;
}

static YanEditorResult run_write(YanEditor *editor)
{
    const uint8_t *bytes = editor->length > 0u ? editor->draft : NULL;
    YanFsResult result =
        editor->existing
            ? yan_fs_replace(editor->fs, editor->name, bytes, editor->length)
            : yan_fs_create(editor->fs, editor->name, bytes, editor->length);
    if (result != YAN_FS_OK) {
        return editor_finish_fs_error(editor, result);
    }
    /* The medium already changed. Leaving editing is the session's own state,
     * and a failed confirmation below is fatal without any rollback. */
    editor->active = false;
    if (!editor_emit_ok(editor, "w")) {
        return YAN_EDITOR_FATAL;
    }
    return YAN_EDITOR_EXIT;
}

static YanEditorResult run_quit(YanEditor *editor)
{
    editor->active = false;
    if (!editor_emit_ok(editor, "q")) {
        return YAN_EDITOR_FATAL;
    }
    return YAN_EDITOR_EXIT;
}

/* Dispatches one editing line. The command token ends at the first space; for
 * `a` exactly one space is consumed and the rest is TEXT, and for `r` the same
 * holds after the number. `p`, `w` and `q` accept only trailing spaces. */
static YanEditorResult run_line(YanEditor *editor, const uint8_t *line,
                                uint32_t length)
{
    uint32_t command_start = skip_spaces(line, length, 0u);
    if (command_start == length) {
        /* An empty or all-space line only asks for the next prompt. */
        return YAN_EDITOR_OK;
    }
    uint32_t position = command_start;
    while (position < length && line[position] != (uint8_t)' ') {
        ++position;
    }
    uint32_t command_length = position - command_start;

    if (token_is(line, command_start, command_length, "a")) {
        uint32_t text_start = position;
        uint32_t text_length = 0u;
        if (position < length) {
            text_start = position + 1u;
            text_length = length - text_start;
        }
        return run_append(editor, line + text_start, text_length);
    }
    if (token_is(line, command_start, command_length, "r")) {
        uint32_t number_start = skip_spaces(line, length, position);
        if (number_start == length) {
            return editor_emit_own_error(editor, "USAGE");
        }
        uint32_t number_end = number_start;
        while (number_end < length && line[number_end] != (uint8_t)' ') {
            ++number_end;
        }
        uint32_t number = 0u;
        if (!parse_line_number(line, number_start, number_end, &number)) {
            return editor_emit_own_error(editor, "USAGE");
        }
        uint32_t text_start = number_end;
        uint32_t text_length = 0u;
        if (number_end < length) {
            text_start = number_end + 1u;
            text_length = length - text_start;
        }
        return run_replace(editor, number, line + text_start, text_length);
    }
    if (token_is(line, command_start, command_length, "d")) {
        uint32_t number_start = skip_spaces(line, length, position);
        if (number_start == length) {
            return editor_emit_own_error(editor, "USAGE");
        }
        uint32_t number_end = number_start;
        while (number_end < length && line[number_end] != (uint8_t)' ') {
            ++number_end;
        }
        uint32_t number = 0u;
        if (!parse_line_number(line, number_start, number_end, &number)) {
            return editor_emit_own_error(editor, "USAGE");
        }
        if (!rest_is_spaces(line, length, number_end)) {
            return editor_emit_own_error(editor, "USAGE");
        }
        return run_delete(editor, number);
    }
    if (token_is(line, command_start, command_length, "p")) {
        if (!rest_is_spaces(line, length, position)) {
            return editor_emit_own_error(editor, "USAGE");
        }
        return run_print(editor);
    }
    if (token_is(line, command_start, command_length, "w")) {
        if (!rest_is_spaces(line, length, position)) {
            return editor_emit_own_error(editor, "USAGE");
        }
        return run_write(editor);
    }
    if (token_is(line, command_start, command_length, "q")) {
        if (!rest_is_spaces(line, length, position)) {
            return editor_emit_own_error(editor, "USAGE");
        }
        return run_quit(editor);
    }
    return editor_emit_own_error(editor, "UNKNOWN_COMMAND");
}

/* ------------------------------------------------------------- entry points */

/* Runs with busy already set. The whole-line validation comes first and
 * touches no filesystem state; the health guard then reads the borrowed
 * filesystem's public fields only, so even it performs no filesystem call. */
static YanEditorResult execute_busy(YanEditor *editor, const uint8_t *line,
                                    uint32_t length)
{
    if (length > YAN_EDITOR_LINE_MAX) {
        return editor_emit_own_error(editor, "LINE_TOO_LONG");
    }
    for (uint32_t i = 0; i < length; ++i) {
        uint8_t byte = line[i];
        if (byte <= 0x1Fu || byte == 0x7Fu) {
            return editor_emit_own_error(editor, "INVALID_INPUT");
        }
    }
    YanFsResult health = editor_fs_health(editor);
    if (health != YAN_FS_OK) {
        return editor_finish_fs_error(editor, health);
    }
    return run_line(editor, line, length);
}

/* Runs with busy already set. The draft is cleared up front so no failed start
 * can leave a previous session's bytes with a length that a later save could
 * publish; a fresh draft only becomes active after a complete load and a
 * successful validation. */
static YanEditorResult start_busy(YanEditor *editor, const uint8_t *name,
                                  uint32_t name_length)
{
    editor->length = 0u;
    editor->existing = false;
    editor->active = false;
    editor->name[0] = '\0';

    YanFsResult health = editor_fs_health(editor);
    if (health != YAN_FS_OK) {
        return editor_finish_fs_error(editor, health);
    }

    /* The public input is a byte span, not a C string. An embedded NUL must
     * not silently select a shorter file name when passed to YanFS. */
    if (name_length == 0u || name_length > YAN_FS_NAME_MAX) {
        return editor_emit_own_error(editor, "INVALID");
    }
    for (uint32_t i = 0u; i < name_length; ++i) {
        if (name[i] == 0u) {
            return editor_emit_own_error(editor, "INVALID");
        }
    }

    char local_name[YAN_FS_NAME_MAX + 2u];
    copy_name(name, name_length, local_name);

    YanFsInfo info;
    YanFsResult result = yan_fs_stat(editor->fs, local_name, &info);
    if (result == YAN_FS_NOT_FOUND) {
        /* A new name starts an empty draft and creates nothing on disk. */
        editor->existing = false;
    } else if (result == YAN_FS_OK) {
        if (info.size_bytes > YAN_EDITOR_CAPACITY) {
            return editor_emit_own_error(editor, "TOO_LARGE");
        }
        uint32_t read_bytes = 0u;
        result = yan_fs_read(editor->fs, local_name, 0u, editor->draft,
                             info.size_bytes, &read_bytes);
        if (result != YAN_FS_OK) {
            return editor_finish_fs_error(editor, result);
        }
        if (read_bytes != info.size_bytes) {
            /* The filesystem returned success but not the whole file. That
             * breaks the read contract and could otherwise publish a prefix,
             * so it stops like a protocol failure. */
            return editor_finish_fs_error(editor, YAN_FS_PROTOCOL);
        }
        if (!draft_is_valid(editor->draft, info.size_bytes)) {
            return editor_emit_own_error(editor, "INVALID_TEXT");
        }
        editor->existing = true;
        editor->length = info.size_bytes;
    } else {
        return editor_finish_fs_error(editor, result);
    }

    /* A successful stat means the name passed the filesystem's own rules, so
     * it fits name[] with its terminator. */
    uint32_t copy = name_length < YAN_FS_NAME_MAX ? name_length
                                                  : YAN_FS_NAME_MAX;
    for (uint32_t i = 0; i < copy; ++i) {
        editor->name[i] = (char)name[i];
    }
    editor->name[copy] = '\0';

    if (!editor_emit_ok(editor, "edit")) {
        return YAN_EDITOR_FATAL;
    }
    editor->active = true;
    return YAN_EDITOR_OK;
}

YanEditorResult yan_editor_init(YanEditor *editor, YanFs *fs,
                                YanShellOutput output)
{
    if (editor == NULL) {
        return YAN_EDITOR_INVALID;
    }
    /* Address arithmetic first: an editor whose object would leave uintptr_t
     * is invalid before any field is read. */
    if ((uint64_t)sizeof(YanEditor) >
        (uint64_t)(UINTPTR_MAX - (uintptr_t)editor)) {
        return YAN_EDITOR_INVALID;
    }
    if (fs == NULL) {
        /* Nothing to compare addresses against; the original BUSY priority for
         * an active instance is preserved. */
        if (editor->initialized && editor->busy) {
            return YAN_EDITOR_BUSY;
        }
        return YAN_EDITOR_INVALID;
    }
    /* An aliased pair is rejected before any bool field of either context is
     * read, and before the first store, so neither context is written. */
    if (ranges_overlap((uintptr_t)(const void *)editor,
                       (uint64_t)sizeof(YanEditor), (uintptr_t)(const void *)fs,
                       (uint64_t)sizeof(YanFs))) {
        return YAN_EDITOR_INVALID;
    }
    if (editor->initialized && editor->busy) {
        return YAN_EDITOR_BUSY;
    }
    if (output.putc == NULL) {
        return YAN_EDITOR_INVALID;
    }
    editor->fs = fs;
    editor->output = output;
    editor->initialized = true;
    editor->busy = false;
    editor->active = false;
    editor->existing = false;
    editor->length = 0u;
    editor->name[0] = '\0';
    return YAN_EDITOR_OK;
}

YanEditorResult yan_editor_start(YanEditor *editor, const uint8_t *name,
                                 uint32_t name_length)
{
    if (editor == NULL || !editor->initialized) {
        return YAN_EDITOR_INVALID;
    }
    if (editor->busy) {
        return YAN_EDITOR_BUSY;
    }
    if (editor->active) {
        return YAN_EDITOR_INVALID;
    }
    if (name == NULL) {
        if (name_length != 0u) {
            return YAN_EDITOR_INVALID;
        }
    } else {
        uintptr_t begin = (uintptr_t)(const void *)name;
        if ((uint64_t)name_length > (uint64_t)(UINTPTR_MAX - begin)) {
            return YAN_EDITOR_INVALID;
        }
        if (ranges_overlap((uintptr_t)(const void *)editor,
                           (uint64_t)sizeof(YanEditor), begin,
                           (uint64_t)name_length) ||
            ranges_overlap((uintptr_t)(const void *)editor->fs,
                           (uint64_t)sizeof(YanFs), begin,
                           (uint64_t)name_length)) {
            return YAN_EDITOR_INVALID;
        }
    }

    editor->busy = true;
    YanEditorResult result = start_busy(editor, name, name_length);
    editor->busy = false;
    return result;
}

YanEditorResult yan_editor_execute(YanEditor *editor, const uint8_t *line,
                                   uint32_t length)
{
    if (editor == NULL || !editor->initialized) {
        return YAN_EDITOR_INVALID;
    }
    if (editor->busy) {
        return YAN_EDITOR_BUSY;
    }
    if (line == NULL) {
        if (length != 0u) {
            return YAN_EDITOR_INVALID;
        }
    } else {
        uintptr_t begin = (uintptr_t)(const void *)line;
        if ((uint64_t)length > (uint64_t)(UINTPTR_MAX - begin)) {
            return YAN_EDITOR_INVALID;
        }
        /* The line must not be part of the objects this call reads or writes.
         * The test happens before busy is set, so a rejected call cannot have
         * written to a caller buffer that aliases the instance. */
        if (ranges_overlap((uintptr_t)(const void *)editor,
                           (uint64_t)sizeof(YanEditor), begin,
                           (uint64_t)length) ||
            ranges_overlap((uintptr_t)(const void *)editor->fs,
                           (uint64_t)sizeof(YanFs), begin, (uint64_t)length)) {
            return YAN_EDITOR_INVALID;
        }
    }
    if (!editor->active) {
        return YAN_EDITOR_INVALID;
    }

    editor->busy = true;
    YanEditorResult result = execute_busy(editor, line, length);
    editor->busy = false;
    return result;
}

bool yan_editor_active(const YanEditor *editor)
{
    return editor != NULL && editor->initialized && editor->active;
}
