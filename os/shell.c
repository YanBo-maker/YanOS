/* Terminal command dispatcher for 0022.
 *
 * The contract is os/shell.h and docs/specs/0022-terminal-file-operations.md.
 * This translation unit owns the exact output text and the mapping from a
 * YanFsResult to "continue" or "fatal"; it never talks to a block device
 * directly. Four properties decide the shape of the code:
 *
 *   validate before touching the image  the line length, the control-byte
 *   scan and the command/arity/name parse all run before any command reaches
 *   the filesystem and before the first output byte. A rejected line therefore
 *   leaves the image untouched; it still reports one ERROR line, because the
 *   caller has to learn why nothing ran. A read-only look at the borrowed
 *   filesystem's state fields follows the line checks and can end the session
 *   on its own, without calling a filesystem API.
 *
 *   success only after filesystem success  no command prints its OK line
 *   before the filesystem call it reports on has returned OK. cat is the one
 *   streaming command: it prints each successfully read chunk, so a later read
 *   failure keeps that prefix, then reports ERROR and never prints the OK tail.
 *
 *   stop at the first failed byte  every output helper returns as soon as putc
 *   answers false. Nothing is emitted after a failure, and the call returns
 *   YAN_SHELL_FATAL with the instance left reusable.
 *
 *   one owner at a time  busy spans the whole call, including the output-only
 *   commands, so a reentrant call from inside putc or from inside a block
 *   callback sees YAN_SHELL_BUSY and cannot emit or perform I/O.
 *
 * cat's UTF-8 handling keeps at most one 4-byte sequence between read chunks.
 * A byte that cannot continue the pending sequence is escaped, then the
 * current byte is reprocessed, so a broken sequence never swallows the ASCII
 * that follows it. */
#include "shell.h"

#include <stddef.h>

/* Help text, ending before the final OK line. It lists the commands, the
 * 1023-byte line limit, exit and the cat display rules. Tests pin these bytes;
 * 0024 adds the two-name commands after rm. */
static const char HELP_TEXT[] =
    "help: show commands and limits\r\n"
    "ls: list files as <size> <name>\r\n"
    "stat NAME: show a file's size\r\n"
    "cat NAME: print a file\r\n"
    "create NAME [TEXT]: create a new file\r\n"
    "write NAME [TEXT]: replace an existing file\r\n"
    "rm NAME: remove a file\r\n"
    "mv OLD NEW: rename a file\r\n"
    "cp SRC DEST: copy a file\r\n"
    "grep TOKEN: print matching lines as FILE:LINE:CONTENT\r\n"
    "search QUERY: print term matches as FILE:LINE:SNIPPET\r\n"
    "rebuild: rebuild the term index\r\n"
    "index status|clear: show or drop the term index\r\n"
    "exit: end the session\r\n"
    "line: at most 1023 bytes, ended by CR or LF\r\n"
    "separators: ASCII spaces; commands and names are case sensitive\r\n"
    "names: 1 to 31 bytes of A-Z a-z 0-9 . _ - and never . or ..\r\n"
    "create creates only; write replaces only; no LF is added to text\r\n"
    "cat: printable ASCII as is, a backslash as two backslashes, C0 and DEL as \\xHH\r\n"
    "cat: valid UTF-8 as is except C1 U+0080 to U+009F, escaped byte by byte\r\n";

/* True when the two byte ranges intersect. Same arithmetic as the filesystem
 * core: a range that would leave uintptr_t is reported as overlapping, which
 * is what the execute parameter check needs for a pointer plus a huge length.
 * Nothing is read or written through either range. */
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

static bool shell_putc(YanShell *shell, uint8_t byte)
{
    return shell->output.putc(shell->output.context, byte);
}

static bool shell_put_bytes(YanShell *shell, const uint8_t *bytes, uint32_t length)
{
    for (uint32_t i = 0; i < length; ++i) {
        if (!shell_putc(shell, bytes[i])) {
            return false;
        }
    }
    return true;
}

static bool shell_puts(YanShell *shell, const char *text)
{
    for (uint32_t i = 0; text[i] != '\0'; ++i) {
        if (!shell_putc(shell, (uint8_t)text[i])) {
            return false;
        }
    }
    return true;
}

static bool shell_put_decimal(YanShell *shell, uint32_t value)
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
        if (!shell_putc(shell, digits[count])) {
            return false;
        }
    }
    return true;
}

/* One C0 or DEL byte as \xHH with uppercase hexadecimal. The short-circuit
 * chain is what keeps a later putc from being called after a false. */
static bool shell_put_hex_byte(YanShell *shell, uint8_t byte)
{
    static const char hex[] = "0123456789ABCDEF";
    return shell_putc(shell, (uint8_t)'\\') &&
           shell_putc(shell, (uint8_t)'x') &&
           shell_putc(shell, (uint8_t)hex[byte >> 4]) &&
           shell_putc(shell, (uint8_t)hex[byte & 0x0Fu]);
}

/* Stable suffix of the ERROR line. Every enumerator is named so -Wswitch can
 * tell that the mapping is complete if the enum grows. */
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

/* 0022 says INVALID, EXISTS, NOT_FOUND, DIRECTORY_FULL, NOSPACE and BUSY are
 * normal errors that keep the session alive. An instance that is unmounted,
 * faulted, or that has an I/O or protocol problem, cannot be trusted, and a
 * corrupt or unsupported device is worse still: those end the application. */
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

static YanShellResult emit_status(YanShell *shell, const char *text)
{
    return shell_puts(shell, text) ? YAN_SHELL_OK : YAN_SHELL_FATAL;
}

/* Stable suffix of a Search result's ERROR line. The source classes reuse the
 * filesystem's tokens so a Guest oracle sees one vocabulary. */
static const char *search_error_suffix(YanSearchResult result)
{
    switch (result) {
    case YAN_SEARCH_NOT_MOUNTED: return "NOT_MOUNTED";
    case YAN_SEARCH_FAULTED: return "FAULTED";
    case YAN_SEARCH_NOT_FOUND: return "NOT_FOUND";
    case YAN_SEARCH_INVALID: return "INVALID";
    case YAN_SEARCH_BUSY: return "BUSY";
    case YAN_SEARCH_IO: return "IO";
    case YAN_SEARCH_PROTOCOL: return "PROTOCOL";
    case YAN_SEARCH_CORRUPT: return "CORRUPT";
    case YAN_SEARCH_UNSUPPORTED: return "UNSUPPORTED";
    case YAN_SEARCH_STOPPED: return "STOPPED";
    case YAN_SEARCH_OK: return "OK";
    }
    return "UNKNOWN";
}

/* The same trust grouping the filesystem uses: a source that is unmounted,
 * faulted, or that has an I/O, protocol, corrupt or unsupported problem cannot
 * be trusted and ends the session. A parameter, busy, not-found or stopped
 * answer is ordinary and keeps it alive. */
static bool search_error_is_fatal(YanSearchResult result)
{
    switch (result) {
    case YAN_SEARCH_NOT_MOUNTED:
    case YAN_SEARCH_FAULTED:
    case YAN_SEARCH_IO:
    case YAN_SEARCH_PROTOCOL:
    case YAN_SEARCH_CORRUPT:
    case YAN_SEARCH_UNSUPPORTED:
        return true;
    case YAN_SEARCH_OK:
    case YAN_SEARCH_INVALID:
    case YAN_SEARCH_BUSY:
    case YAN_SEARCH_NOT_FOUND:
    case YAN_SEARCH_STOPPED:
        return false;
    }
    return true;
}

/* Prints "ERROR <suffix>\r\n" for a Search result; a false return only means
 * the output sink refused a byte (the caller then reports FATAL). */
static bool emit_search_error(YanShell *shell, YanSearchResult result)
{
    return shell_puts(shell, "ERROR ") &&
           shell_puts(shell, search_error_suffix(result)) &&
           shell_puts(shell, "\r\n");
}

/* Prints "ERROR <suffix>\r\n" and maps the filesystem result to the session
 * verdict. An output failure is always fatal; a normal error still continues. */
static YanShellResult finish_fs_error(YanShell *shell, YanFsResult result)
{
    if (!shell_puts(shell, "ERROR ") ||
        !shell_puts(shell, fs_error_suffix(result)) ||
        !shell_puts(shell, "\r\n")) {
        return YAN_SHELL_FATAL;
    }
    return fs_error_is_fatal(result) ? YAN_SHELL_FATAL : YAN_SHELL_OK;
}

static bool emit_ok(YanShell *shell, const char *command)
{
    return shell_puts(shell, "OK ") && shell_puts(shell, command) &&
           shell_puts(shell, "\r\n");
}

/* -------------------------------------------------- 0026 term mapping ------ */

/* Stable suffix of a term result's ERROR line. This is a separate switch, never
 * a cast of the literal enum: the two are independent types. An unknown
 * enumerator returns NULL so the caller can report PROTOCOL rather than
 * inventing an "UNKNOWN" success. */
static const char *terms_error_suffix(YanSearchTermsResult result)
{
    switch (result) {
    case YAN_SEARCH_TERMS_INVALID: return "INVALID";
    case YAN_SEARCH_TERMS_BUSY: return "BUSY";
    case YAN_SEARCH_TERMS_STOPPED: return "STOPPED";
    case YAN_SEARCH_TERMS_NOT_MOUNTED: return "NOT_MOUNTED";
    case YAN_SEARCH_TERMS_FAULTED: return "FAULTED";
    case YAN_SEARCH_TERMS_IO: return "IO";
    case YAN_SEARCH_TERMS_PROTOCOL: return "PROTOCOL";
    case YAN_SEARCH_TERMS_CORRUPT: return "CORRUPT";
    case YAN_SEARCH_TERMS_UNSUPPORTED: return "UNSUPPORTED";
    case YAN_SEARCH_TERMS_NOT_FOUND: return "NOT_FOUND";
    case YAN_SEARCH_TERMS_INDEX_LIMIT: return "INDEX_LIMIT";
    case YAN_SEARCH_TERMS_OK: return "OK";
    }
    return NULL;
}

/* Same trust grouping as the literal and filesystem results: an untrustworthy
 * source or an actual fault is fatal, an ordinary error continues. */
static bool terms_error_is_fatal(YanSearchTermsResult result)
{
    switch (result) {
    case YAN_SEARCH_TERMS_NOT_MOUNTED:
    case YAN_SEARCH_TERMS_FAULTED:
    case YAN_SEARCH_TERMS_IO:
    case YAN_SEARCH_TERMS_PROTOCOL:
    case YAN_SEARCH_TERMS_CORRUPT:
    case YAN_SEARCH_TERMS_UNSUPPORTED:
        return true;
    case YAN_SEARCH_TERMS_OK:
    case YAN_SEARCH_TERMS_INVALID:
    case YAN_SEARCH_TERMS_BUSY:
    case YAN_SEARCH_TERMS_STOPPED:
    case YAN_SEARCH_TERMS_NOT_FOUND:
    case YAN_SEARCH_TERMS_INDEX_LIMIT:
        return false;
    }
    return true;
}

/* Prints "ERROR <suffix>\r\n". An unknown enumerator becomes ERROR PROTOCOL and
 * is treated as fatal by the caller's verdict check. A false return only means
 * the output sink refused a byte. */
static bool emit_terms_error(YanShell *shell, YanSearchTermsResult result)
{
    const char *suffix = terms_error_suffix(result);
    if (suffix == NULL) {
        suffix = "PROTOCOL";
    }
    return shell_puts(shell, "ERROR ") && shell_puts(shell, suffix) &&
           shell_puts(shell, "\r\n");
}

static YanShellResult finish_terms_error(YanShell *shell,
                                         YanSearchTermsResult result)
{
    const char *suffix = terms_error_suffix(result);
    bool known = suffix != NULL;
    if (!emit_terms_error(shell, result)) {
        return YAN_SHELL_FATAL;
    }
    if (!known) {
        return YAN_SHELL_FATAL;
    }
    return terms_error_is_fatal(result) ? YAN_SHELL_FATAL : YAN_SHELL_OK;
}

static const char *terms_state_name(YanSearchTermsIndexState state)
{
    switch (state) {
    case YAN_SEARCH_INDEX_EMPTY: return "EMPTY";
    case YAN_SEARCH_INDEX_READY: return "READY";
    case YAN_SEARCH_INDEX_STALE: return "STALE";
    case YAN_SEARCH_INDEX_LIMIT: return "LIMIT";
    case YAN_SEARCH_INDEX_UNCACHEABLE: return "UNCACHEABLE";
    case YAN_SEARCH_INDEX_UNAVAILABLE: return "UNAVAILABLE";
    }
    return NULL;
}

static const char *terms_source_name(YanSearchTermsSourceState source)
{
    switch (source) {
    case YAN_SEARCH_SOURCE_MOUNTED: return "MOUNTED";
    case YAN_SEARCH_SOURCE_UNMOUNTED: return "UNMOUNTED";
    case YAN_SEARCH_SOURCE_FAULTED: return "FAULTED";
    case YAN_SEARCH_SOURCE_UNINITIALIZED: return "UNINITIALIZED";
    }
    return NULL;
}

static const char *terms_mode_name(YanSearchTermsMode mode)
{
    switch (mode) {
    case YAN_SEARCH_MODE_INDEX: return "index";
    case YAN_SEARCH_MODE_SCAN: return "scan";
    }
    return NULL;
}

static bool shell_put_uint64(YanShell *shell, uint64_t value)
{
    /* RV32's freestanding link has no 64-bit division helpers. Accumulate
     * decimal digits one binary bit at a time using only uint32 arithmetic;
     * 20 digits cover UINT64_MAX and constant shifts need no runtime helper. */
    uint8_t digits[20] = {0};
    for (uint32_t bit = 0u; bit < 64u; ++bit) {
        uint32_t carry = (uint32_t)(value >> 63u);
        value <<= 1u;
        for (uint32_t i = 20u; i > 0u; --i) {
            uint32_t digit = (uint32_t)digits[i - 1u] * 2u + carry;
            carry = digit / 10u;
            digits[i - 1u] = (uint8_t)(digit % 10u);
        }
    }
    uint32_t start = 0u;
    while (start < 19u && digits[start] == 0u) {
        ++start;
    }
    for (uint32_t i = start; i < 20u; ++i) {
        if (!shell_putc(shell, (uint8_t)('0' + digits[i]))) {
            return false;
        }
    }
    return true;
}

/* A read-only look at the borrowed filesystem's own state. It reads the public
 * context fields only - no API call, so it cannot read the directory cache or
 * perform block I/O - and mirrors the filesystem's own precedence:
 * uninitialized, then faulted, then not mounted. `busy` is deliberately not
 * part of health: a command running while another filesystem operation is in
 * flight still reports BUSY from that operation, which is an ordinary error.
 *
 * The guard runs before any command, so a healthy `exit` cannot be reported on
 * an unmounted or faulted filesystem, and a fatal state ends the session even
 * for an empty line. The application stops after a fatal result instead of
 * looping. */
static YanFsResult shell_fs_health(const YanShell *shell)
{
    const YanFs *fs = shell->fs;
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

/* ------------------------------------------------------------- UTF-8 stream */

typedef struct {
    uint8_t pending[4];
    uint32_t pending_len;
    uint32_t expected;
} Utf8Stream;

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

/* The caller has already confirmed the lead byte and that every following
 * byte is a continuation. Only the three range rules that separate a legal
 * sequence from an overlong, surrogate or out-of-range one remain. */
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

static bool cat_emit_escaped(YanShell *shell, const uint8_t *bytes, uint32_t length)
{
    for (uint32_t i = 0; i < length; ++i) {
        if (!shell_put_hex_byte(shell, bytes[i])) {
            return false;
        }
    }
    return true;
}

/* ASCII and control bytes outside a multibyte sequence. Printable ASCII is
 * passed through except the backslash, which doubles so the \xHH escapes stay
 * unambiguous; C0 and DEL become \xHH. */
static bool cat_emit_ascii(YanShell *shell, uint8_t byte)
{
    if (byte == (uint8_t)'\\') {
        return shell_putc(shell, byte) && shell_putc(shell, byte);
    }
    if (byte >= 0x20u && byte <= 0x7Eu) {
        return shell_putc(shell, byte);
    }
    return shell_put_hex_byte(shell, byte);
}

/* Escapes whatever is buffered. Used when a read fails or the file ends with
 * an incomplete sequence, so those bytes are still accounted for. */
static bool cat_flush(YanShell *shell, Utf8Stream *stream)
{
    if (stream->pending_len == 0u) {
        return true;
    }
    bool ok = cat_emit_escaped(shell, stream->pending, stream->pending_len);
    stream->pending_len = 0u;
    return ok;
}

static bool cat_feed(YanShell *shell, Utf8Stream *stream, uint8_t byte)
{
    if (stream->pending_len == 0u) {
        if (byte < 0x80u) {
            return cat_emit_ascii(shell, byte);
        }
        if (byte >= 0xC2u && byte <= 0xDFu) {
            stream->pending[0] = byte;
            stream->pending_len = 1u;
            stream->expected = 2u;
            return true;
        }
        if (byte >= 0xE0u && byte <= 0xEFu) {
            stream->pending[0] = byte;
            stream->pending_len = 1u;
            stream->expected = 3u;
            return true;
        }
        if (byte >= 0xF0u && byte <= 0xF4u) {
            stream->pending[0] = byte;
            stream->pending_len = 1u;
            stream->expected = 4u;
            return true;
        }
        /* Lone continuation, an overlong lead, or a lead past U+10FFFF. */
        return shell_put_hex_byte(shell, byte);
    }
    if (byte >= 0x80u && byte <= 0xBFu) {
        stream->pending[stream->pending_len] = byte;
        ++stream->pending_len;
        if (stream->pending_len == stream->expected) {
            bool ok;
            if (utf8_sequence_valid(stream->pending, stream->expected)) {
                uint32_t codepoint =
                    utf8_codepoint(stream->pending, stream->expected);
                if (codepoint >= 0x80u && codepoint <= 0x9Fu) {
                    /* C1 control: escape every byte of its encoding. */
                    ok = cat_emit_escaped(shell, stream->pending,
                                          stream->expected);
                } else {
                    ok = shell_put_bytes(shell, stream->pending,
                                         stream->expected);
                }
            } else {
                ok = cat_emit_escaped(shell, stream->pending, stream->expected);
            }
            stream->pending_len = 0u;
            return ok;
        }
        return true;
    }
    /* The current byte cannot belong to the pending sequence. Escape what was
     * buffered, then handle this byte from a clean state so it is not lost. */
    if (!cat_emit_escaped(shell, stream->pending, stream->pending_len)) {
        return false;
    }
    stream->pending_len = 0u;
    return cat_feed(shell, stream, byte);
}

/* ----------------------------------------------------------- line scanning */

static uint32_t skip_spaces(const uint8_t *line, uint32_t length, uint32_t position)
{
    while (position < length && line[position] == (uint8_t)' ') {
        ++position;
    }
    return position;
}

static bool rest_is_spaces(const uint8_t *line, uint32_t length, uint32_t position)
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

/* Copies a name token out of the line. Names are at most 31 bytes plus the
 * terminator, but a longer token still has to reach yan_fs_* so that the
 * filesystem, not this layer, is the single judge of name validity. Such a
 * token is copied as 32 bytes with no terminator inside the filesystem's
 * 32-byte scan window, which the filesystem rejects as INVALID without reading
 * the bytes after the window. */
static void copy_name(const uint8_t *line, uint32_t start, uint32_t length,
                      char out[YAN_FS_NAME_MAX + 2u])
{
    uint32_t copy = length < (YAN_FS_NAME_MAX + 1u) ? length
                                                    : (YAN_FS_NAME_MAX + 1u);
    for (uint32_t i = 0; i < copy; ++i) {
        out[i] = (char)line[start + i];
    }
    out[copy] = '\0';
}

static bool parse_one_name(const uint8_t *line, uint32_t length, uint32_t position,
                           uint32_t *name_start, uint32_t *name_length)
{
    position = skip_spaces(line, length, position);
    if (position == length) {
        return false;
    }
    uint32_t start = position;
    while (position < length && line[position] != (uint8_t)' ') {
        ++position;
    }
    *name_start = start;
    *name_length = position - start;
    return rest_is_spaces(line, length, position);
}

/* create/write consume exactly one space after the name; every byte after that
 * is TEXT, including further spaces. A name with no following space, or a
 * single space and nothing after it, means zero bytes of text. */
static bool parse_name_and_text(const uint8_t *line, uint32_t length,
                                uint32_t position, uint32_t *name_start,
                                uint32_t *name_length, uint32_t *text_start,
                                uint32_t *text_length)
{
    position = skip_spaces(line, length, position);
    if (position == length) {
        return false;
    }
    uint32_t start = position;
    while (position < length && line[position] != (uint8_t)' ') {
        ++position;
    }
    *name_start = start;
    *name_length = position - start;
    if (position == length) {
        *text_start = length;
        *text_length = 0u;
    } else {
        *text_start = position + 1u;
        *text_length = length - (position + 1u);
    }
    return true;
}

/* 0024 mv/cp: exactly two non-empty names separated by ASCII spaces, with only
 * spaces allowed after the second. A missing or extra token is refused here,
 * before the command reaches the filesystem, so the arity error does no I/O.
 * The two ranges are only reported; the callers copy them into bounded local
 * name buffers, so overlapping names are harmless. */
static bool parse_two_names(const uint8_t *line, uint32_t length,
                            uint32_t position, uint32_t *first_start,
                            uint32_t *first_length, uint32_t *second_start,
                            uint32_t *second_length)
{
    position = skip_spaces(line, length, position);
    if (position == length) {
        return false;
    }
    uint32_t start = position;
    while (position < length && line[position] != (uint8_t)' ') {
        ++position;
    }
    *first_start = start;
    *first_length = position - start;

    position = skip_spaces(line, length, position);
    if (position == length) {
        return false;
    }
    start = position;
    while (position < length && line[position] != (uint8_t)' ') {
        ++position;
    }
    *second_start = start;
    *second_length = position - start;
    return rest_is_spaces(line, length, position);
}

/* --------------------------------------------------------------- commands */

static YanShellResult run_ls(YanShell *shell)
{
    uint32_t cursor = 0u;
    for (;;) {
        YanFsInfo info;
        YanFsResult result = yan_fs_list(shell->fs, &cursor, &info);
        if (result == YAN_FS_END) {
            break;
        }
        if (result != YAN_FS_OK) {
            return finish_fs_error(shell, result);
        }
        uint32_t name_length = 0u;
        while (name_length < sizeof info.name && info.name[name_length] != '\0') {
            ++name_length;
        }
        if (!shell_put_decimal(shell, info.size_bytes) ||
            !shell_putc(shell, (uint8_t)' ') ||
            !shell_put_bytes(shell, (const uint8_t *)info.name, name_length) ||
            !shell_puts(shell, "\r\n")) {
            return YAN_SHELL_FATAL;
        }
    }
    return emit_ok(shell, "ls") ? YAN_SHELL_OK : YAN_SHELL_FATAL;
}

static YanShellResult run_stat(YanShell *shell, const uint8_t *line,
                               uint32_t name_start, uint32_t name_length)
{
    char name[YAN_FS_NAME_MAX + 2u];
    copy_name(line, name_start, name_length, name);
    YanFsInfo info;
    YanFsResult result = yan_fs_stat(shell->fs, name, &info);
    if (result != YAN_FS_OK) {
        return finish_fs_error(shell, result);
    }
    if (!shell_put_decimal(shell, info.size_bytes) ||
        !shell_putc(shell, (uint8_t)' ') ||
        !shell_put_bytes(shell, line + name_start, name_length) ||
        !shell_puts(shell, "\r\nOK stat\r\n")) {
        return YAN_SHELL_FATAL;
    }
    return YAN_SHELL_OK;
}

static YanShellResult run_cat(YanShell *shell, const uint8_t *line,
                              uint32_t name_start, uint32_t name_length)
{
    char name[YAN_FS_NAME_MAX + 2u];
    copy_name(line, name_start, name_length, name);
    Utf8Stream stream;
    stream.pending_len = 0u;
    stream.expected = 0u;
    uint32_t offset = 0u;
    uint32_t shown = 0u;
    for (;;) {
        uint32_t read_bytes = 0u;
        YanFsResult result = yan_fs_read(shell->fs, name, offset, shell->scratch,
                                         YAN_SHELL_SCRATCH_SIZE, &read_bytes);
        if (result != YAN_FS_OK) {
            if (!cat_flush(shell, &stream)) {
                return YAN_SHELL_FATAL;
            }
            /* Content already shown is a prefix, not a complete display, so the
             * ERROR line starts on its own line. A first-read failure (missing
             * file for example) has no prefix and prints only the ERROR line. */
            if (shown > 0u && !shell_puts(shell, "\r\n")) {
                return YAN_SHELL_FATAL;
            }
            return finish_fs_error(shell, result);
        }
        if (read_bytes == 0u) {
            break;
        }
        shown += read_bytes;
        for (uint32_t i = 0; i < read_bytes; ++i) {
            if (!cat_feed(shell, &stream, shell->scratch[i])) {
                return YAN_SHELL_FATAL;
            }
        }
        offset += read_bytes;
    }
    if (!cat_flush(shell, &stream)) {
        return YAN_SHELL_FATAL;
    }
    return shell_puts(shell, "\r\nOK cat\r\n") ? YAN_SHELL_OK : YAN_SHELL_FATAL;
}

static YanShellResult run_create_or_write(YanShell *shell, bool create,
                                          const uint8_t *line,
                                          uint32_t name_start, uint32_t name_length,
                                          uint32_t text_start, uint32_t text_length)
{
    char name[YAN_FS_NAME_MAX + 2u];
    copy_name(line, name_start, name_length, name);
    const uint8_t *text = text_length > 0u ? line + text_start : NULL;
    YanFsResult result = create
                             ? yan_fs_create(shell->fs, name, text, text_length)
                             : yan_fs_replace(shell->fs, name, text, text_length);
    if (result != YAN_FS_OK) {
        return finish_fs_error(shell, result);
    }
    return emit_ok(shell, create ? "create" : "write") ? YAN_SHELL_OK
                                                       : YAN_SHELL_FATAL;
}

static YanShellResult run_rm(YanShell *shell, const uint8_t *line,
                             uint32_t name_start, uint32_t name_length)
{
    char name[YAN_FS_NAME_MAX + 2u];
    copy_name(line, name_start, name_length, name);
    YanFsResult result = yan_fs_remove(shell->fs, name);
    if (result != YAN_FS_OK) {
        return finish_fs_error(shell, result);
    }
    return emit_ok(shell, "rm") ? YAN_SHELL_OK : YAN_SHELL_FATAL;
}

/* 0024 mv/cp: parse two bounded names, run the filesystem primitive, then
 * print OK only after it returned OK. An ordinary error reuses finish_fs_error
 * and keeps the session alive; an output failure is fatal. */
static YanShellResult run_mv(YanShell *shell, const uint8_t *line,
                             uint32_t old_start, uint32_t old_length,
                             uint32_t new_start, uint32_t new_length)
{
    char old_name[YAN_FS_NAME_MAX + 2u];
    char new_name[YAN_FS_NAME_MAX + 2u];
    copy_name(line, old_start, old_length, old_name);
    copy_name(line, new_start, new_length, new_name);
    YanFsResult result = yan_fs_rename(shell->fs, old_name, new_name);
    if (result != YAN_FS_OK) {
        return finish_fs_error(shell, result);
    }
    return emit_ok(shell, "mv") ? YAN_SHELL_OK : YAN_SHELL_FATAL;
}

static YanShellResult run_cp(YanShell *shell, const uint8_t *line,
                             uint32_t source_start, uint32_t source_length,
                             uint32_t destination_start,
                             uint32_t destination_length)
{
    char source_name[YAN_FS_NAME_MAX + 2u];
    char destination_name[YAN_FS_NAME_MAX + 2u];
    copy_name(line, source_start, source_length, source_name);
    copy_name(line, destination_start, destination_length, destination_name);
    YanFsResult result =
        yan_fs_copy(shell->fs, source_name, destination_name);
    if (result != YAN_FS_OK) {
        return finish_fs_error(shell, result);
    }
    return emit_ok(shell, "cp") ? YAN_SHELL_OK : YAN_SHELL_FATAL;
}

/* ----------------------------------------------------------------- grep -- */

/* Callback state for one grep query. It lives on run_grep's stack and is
 * reached through the sink context, so the match callback can report an output
 * failure or an already-emitted reader diagnostic back to the caller. */
typedef struct {
    YanShell *shell;
    bool output_failed;
    bool reader_reported;
    YanSearchResult reader_result;
} GrepState;

/* Exactly one non-empty PATTERN after `grep`. Returns false for every other
 * form, so the caller prints USAGE before touching the Search or the
 * filesystem. Bare: one token, no space, no double quote, no leading '-'.
 * Quoted: one outer pair whose interior is literal and which is the last thing
 * on the line. */
static bool grep_parse_pattern(const uint8_t *line, uint32_t length,
                               uint32_t position, const uint8_t **pattern,
                               uint32_t *pattern_length)
{
    position = skip_spaces(line, length, position);
    if (position == length) {
        return false;
    }
    if (line[position] == (uint8_t)'"') {
        uint32_t start = position + 1u;
        uint32_t end = start;
        while (end < length && line[end] != (uint8_t)'"') {
            ++end;
        }
        if (end == length || end == start) {
            return false;
        }
        if (!rest_is_spaces(line, length, end + 1u)) {
            return false;
        }
        *pattern = line + start;
        *pattern_length = end - start;
        return true;
    }
    uint32_t start = position;
    uint32_t end = start;
    while (end < length && line[end] != (uint8_t)' ') {
        if (line[end] == (uint8_t)'"') {
            return false;
        }
        ++end;
    }
    if (end == start || line[start] == (uint8_t)'-') {
        return false;
    }
    if (!rest_is_spaces(line, length, end)) {
        return false;
    }
    *pattern = line + start;
    *pattern_length = end - start;
    return true;
}

/* One matched line: FILE:LINE: then the full content through the same cat
 * display stream as `cat`, so the UTF-8 decoder state persists across the
 * reader's chunks. A read failure flushes the pending UTF-8 prefix, closes the
 * record with CRLF and prints the source ERROR; an output refusal stops
 * immediately with no further read or output. */
static bool grep_match(void *context, const YanSearchMatch *match)
{
    GrepState *state = (GrepState *)context;
    YanShell *shell = state->shell;
    for (uint32_t i = 0u; match->name[i] != '\0'; ++i) {
        if (!shell_putc(shell, (uint8_t)match->name[i])) {
            state->output_failed = true;
            return false;
        }
    }
    if (!shell_putc(shell, (uint8_t)':') ||
        !shell_put_decimal(shell, match->line_number) ||
        !shell_putc(shell, (uint8_t)':')) {
        state->output_failed = true;
        return false;
    }
    Utf8Stream stream;
    stream.pending_len = 0u;
    stream.expected = 0u;
    uint32_t offset = 0u;
    while (offset < match->content_length) {
        const uint8_t *bytes = NULL;
        uint32_t got = 0u;
        YanSearchResult result = yan_search_read_match(
            shell->search, offset, match->content_length - offset, &bytes,
            &got);
        if (result != YAN_SEARCH_OK) {
            if (!cat_flush(shell, &stream) || !shell_puts(shell, "\r\n") ||
                !emit_search_error(shell, result)) {
                state->output_failed = true;
                return false;
            }
            state->reader_reported = true;
            state->reader_result = result;
            return false;
        }
        if (got == 0u || got > match->content_length - offset) {
            /* The facade contract already rejects this; the guard keeps a
             * buggy backend from hanging instead of producing a diagnostic. */
            if (!cat_flush(shell, &stream) || !shell_puts(shell, "\r\n") ||
                !emit_search_error(shell, YAN_SEARCH_PROTOCOL)) {
                state->output_failed = true;
                return false;
            }
            state->reader_reported = true;
            state->reader_result = YAN_SEARCH_PROTOCOL;
            return false;
        }
        for (uint32_t i = 0u; i < got; ++i) {
            if (!cat_feed(shell, &stream, bytes[i])) {
                state->output_failed = true;
                return false;
            }
        }
        offset += got;
    }
    if (!cat_flush(shell, &stream) || !shell_puts(shell, "\r\n")) {
        state->output_failed = true;
        return false;
    }
    return true;
}

/* `grep PATTERN`: parse the approved grammar, then run the injected Search and
 * format its value matches. The shell never enumerates the filesystem or reads
 * an extent for grep; every byte and every error comes back through the Search
 * API. A zero-match success still prints the OK receipt. */
static YanShellResult run_grep(YanShell *shell, const uint8_t *line,
                               uint32_t length, uint32_t position)
{
    const uint8_t *pattern = NULL;
    uint32_t pattern_length = 0u;
    if (!grep_parse_pattern(line, length, position, &pattern,
                            &pattern_length)) {
        return emit_status(shell, "ERROR USAGE\r\n");
    }
    if (shell->search == NULL) {
        return emit_status(shell, "ERROR INVALID\r\n");
    }
    GrepState state;
    state.shell = shell;
    state.output_failed = false;
    state.reader_reported = false;
    state.reader_result = YAN_SEARCH_OK;
    YanSearchSink sink;
    sink.context = &state;
    sink.match = grep_match;
    sink.chunk = NULL;
    YanSearchResult result = yan_search_query(shell->search, pattern,
                                              pattern_length, sink);
    if (state.output_failed) {
        return YAN_SHELL_FATAL;
    }
    if (state.reader_reported) {
        /* The match callback already closed the partial record and printed the
         * source ERROR; only the verdict is left. */
        return search_error_is_fatal(state.reader_result) ? YAN_SHELL_FATAL
                                                          : YAN_SHELL_OK;
    }
    if (result == YAN_SEARCH_OK) {
        return shell_puts(shell, "OK grep\r\n") ? YAN_SHELL_OK
                                                : YAN_SHELL_FATAL;
    }
    if (!emit_search_error(shell, result)) {
        return YAN_SHELL_FATAL;
    }
    return search_error_is_fatal(result) ? YAN_SHELL_FATAL : YAN_SHELL_OK;
}

/* -------------------------------------------------------------- search ---- */

/* Callback state for one indexed search. Like GrepState it lives on the command
 * frame and is reached through the sink context, so the match callback can
 * report an output failure or an already-emitted reader diagnostic. */
typedef struct {
    YanShell *shell;
    bool output_failed;
    bool reader_reported;
    YanSearchTermsResult reader_result;
} SearchState;

/* `search` grammar: the whole query after the command, optionally wrapped in
 * one outer pair of double quotes whose interior bytes and spaces are literal.
 * The bare form is the complete remainder (spaces included, since they separate
 * query groups); it may not start with '-' and may not contain a double quote.
 * A quoted query must be the last thing on the line. Every other form returns
 * false so the caller prints USAGE before touching the facade. */
static bool search_parse_query(const uint8_t *line, uint32_t length,
                               uint32_t position, const uint8_t **query,
                               uint32_t *query_length)
{
    position = skip_spaces(line, length, position);
    if (position == length) {
        return false;
    }
    if (line[position] == (uint8_t)'"') {
        uint32_t start = position + 1u;
        uint32_t end = start;
        while (end < length && line[end] != (uint8_t)'"') {
            ++end;
        }
        if (end == length || end == start) {
            return false;
        }
        if (!rest_is_spaces(line, length, end + 1u)) {
            return false;
        }
        *query = line + start;
        *query_length = end - start;
        return true;
    }
    if (line[position] == (uint8_t)'-') {
        return false;
    }
    for (uint32_t i = position; i < length; ++i) {
        if (line[i] == (uint8_t)'"') {
            return false;
        }
    }
    *query = line + position;
    *query_length = length - position;
    return true;
}

/* One matched row: FILE:LINE: then, on each truncated side, "...", and the raw
 * snippet bytes through the same cat display stream as `cat` (safe UTF-8,
 * escaped invalid bytes, no highlight). A reader failure flushes the pending
 * UTF-8 prefix, closes the row and prints the source ERROR. An output refusal
 * stops immediately with no further read or output. */
static bool search_match(void *context, const YanSearchTermsMatch *match)
{
    SearchState *state = (SearchState *)context;
    YanShell *shell = state->shell;
    for (uint32_t i = 0u; match->name[i] != '\0'; ++i) {
        if (!shell_putc(shell, (uint8_t)match->name[i])) {
            state->output_failed = true;
            return false;
        }
    }
    if (!shell_putc(shell, (uint8_t)':') ||
        !shell_put_decimal(shell, match->line_number) ||
        !shell_putc(shell, (uint8_t)':')) {
        state->output_failed = true;
        return false;
    }
    if (match->left_truncated && !shell_puts(shell, "...")) {
        state->output_failed = true;
        return false;
    }
    Utf8Stream stream;
    stream.pending_len = 0u;
    stream.expected = 0u;
    uint32_t offset = 0u;
    while (offset < match->snippet_length) {
        const uint8_t *bytes = NULL;
        uint32_t got = 0u;
        YanSearchTermsResult result = yan_search_terms_read_snippet(
            shell->terms, offset, match->snippet_length - offset, &bytes, &got);
        if (result != YAN_SEARCH_TERMS_OK) {
            if (!cat_flush(shell, &stream) || !shell_puts(shell, "\r\n") ||
                !emit_terms_error(shell, result)) {
                state->output_failed = true;
                return false;
            }
            state->reader_reported = true;
            state->reader_result = result;
            return false;
        }
        if (got == 0u || got > match->snippet_length - offset) {
            if (!cat_flush(shell, &stream) || !shell_puts(shell, "\r\n") ||
                !emit_terms_error(shell, YAN_SEARCH_TERMS_PROTOCOL)) {
                state->output_failed = true;
                return false;
            }
            state->reader_reported = true;
            state->reader_result = YAN_SEARCH_TERMS_PROTOCOL;
            return false;
        }
        for (uint32_t i = 0u; i < got; ++i) {
            if (!cat_feed(shell, &stream, bytes[i])) {
                state->output_failed = true;
                return false;
            }
        }
        offset += got;
    }
    if (!cat_flush(shell, &stream)) {
        state->output_failed = true;
        return false;
    }
    if (match->right_truncated && !shell_puts(shell, "...")) {
        state->output_failed = true;
        return false;
    }
    if (!shell_puts(shell, "\r\n")) {
        state->output_failed = true;
        return false;
    }
    return true;
}

/* `search QUERY`: parse the outer-quote grammar, run the injected term facade
 * and format its ranked rows plus the exact summary. A facade INVALID answer is
 * the CLI USAGE; a source error keeps the existing trust mapping. */
static YanShellResult run_search(YanShell *shell, const uint8_t *line,
                                 uint32_t length, uint32_t position)
{
    const uint8_t *query = NULL;
    uint32_t query_length = 0u;
    if (!search_parse_query(line, length, position, &query, &query_length)) {
        return emit_status(shell, "ERROR USAGE\r\n");
    }
    if (shell->terms == NULL) {
        return emit_status(shell, "ERROR INVALID\r\n");
    }
    SearchState state;
    state.shell = shell;
    state.output_failed = false;
    state.reader_reported = false;
    state.reader_result = YAN_SEARCH_TERMS_OK;
    YanSearchTermsSink sink;
    sink.context = &state;
    sink.match = search_match;
    sink.chunk = NULL;
    YanSearchTermsSummary summary;
    summary.total = 0u;
    summary.shown = 0u;
    summary.skipped = 0u;
    summary.mode = YAN_SEARCH_MODE_INDEX;
    YanSearchTermsResult result =
        yan_search_terms(shell->terms, query, query_length, sink, &summary);
    if (state.output_failed) {
        return YAN_SHELL_FATAL;
    }
    if (state.reader_reported) {
        if (terms_error_suffix(state.reader_result) == NULL) {
            return YAN_SHELL_FATAL;
        }
        return terms_error_is_fatal(state.reader_result) ? YAN_SHELL_FATAL
                                                         : YAN_SHELL_OK;
    }
    if (result == YAN_SEARCH_TERMS_INVALID) {
        return emit_status(shell, "ERROR USAGE\r\n");
    }
    if (result == YAN_SEARCH_TERMS_OK) {
        const char *mode = terms_mode_name(summary.mode);
        if (mode == NULL) {
            (void)emit_status(shell, "ERROR PROTOCOL\r\n");
            return YAN_SHELL_FATAL;
        }
        if (!shell_puts(shell, "OK search total=") ||
            !shell_put_uint64(shell, summary.total) ||
            !shell_puts(shell, " shown=") ||
            !shell_put_decimal(shell, summary.shown) ||
            !shell_puts(shell, " skipped=") ||
            !shell_put_decimal(shell, summary.skipped) ||
            !shell_puts(shell, " mode=") || !shell_puts(shell, mode) ||
            !shell_puts(shell, "\r\n")) {
            return YAN_SHELL_FATAL;
        }
        return YAN_SHELL_OK;
    }
    return finish_terms_error(shell, result);
}

/* `rebuild`: no argument, one full RAM rebuild. OK prints the receipt; a
 * capacity limit is the ordinary ERROR INDEX_LIMIT; a source error keeps the
 * existing mapping. */
static YanShellResult run_rebuild(YanShell *shell)
{
    if (shell->terms == NULL) {
        return emit_status(shell, "ERROR INVALID\r\n");
    }
    YanSearchTermsResult result = yan_search_terms_rebuild(shell->terms);
    if (result != YAN_SEARCH_TERMS_OK) {
        return finish_terms_error(shell, result);
    }
    return emit_ok(shell, "rebuild") ? YAN_SHELL_OK : YAN_SHELL_FATAL;
}

/* `index status`: pure memory observation. The whole line is one INDEX record
 * plus the OK receipt; an unknown enumerator is PROTOCOL, never a fake zero. */
static YanShellResult run_index_status(YanShell *shell)
{
    if (shell->terms == NULL) {
        return emit_status(shell, "ERROR INVALID\r\n");
    }
    YanSearchTermsStatus status;
    status.state = YAN_SEARCH_INDEX_EMPTY;
    status.source = YAN_SEARCH_SOURCE_UNINITIALIZED;
    status.terms = 0u;
    status.postings = 0u;
    YanSearchTermsResult result =
        yan_search_terms_status(shell->terms, &status);
    if (result != YAN_SEARCH_TERMS_OK) {
        return finish_terms_error(shell, result);
    }
    const char *state = terms_state_name(status.state);
    const char *source = terms_source_name(status.source);
    if (state == NULL || source == NULL) {
        (void)emit_status(shell, "ERROR PROTOCOL\r\n");
        return YAN_SHELL_FATAL;
    }
    if (!shell_puts(shell, "INDEX state=") || !shell_puts(shell, state) ||
        !shell_puts(shell, " source=") || !shell_puts(shell, source) ||
        !shell_puts(shell, " terms=") ||
        !shell_put_decimal(shell, status.terms) ||
        !shell_puts(shell, " postings=") ||
        !shell_put_decimal(shell, status.postings) ||
        !shell_puts(shell, "\r\n")) {
        return YAN_SHELL_FATAL;
    }
    return emit_ok(shell, "index") ? YAN_SHELL_OK : YAN_SHELL_FATAL;
}

/* `index clear`: drop the cache without touching the source. */
static YanShellResult run_index_clear(YanShell *shell)
{
    if (shell->terms == NULL) {
        return emit_status(shell, "ERROR INVALID\r\n");
    }
    YanSearchTermsResult result = yan_search_terms_clear(shell->terms);
    if (result != YAN_SEARCH_TERMS_OK) {
        return finish_terms_error(shell, result);
    }
    return emit_ok(shell, "index") ? YAN_SHELL_OK : YAN_SHELL_FATAL;
}

typedef enum {
    INDEX_CMD_NONE = 0,
    INDEX_CMD_STATUS = 1,
    INDEX_CMD_CLEAR = 2
} IndexCommand;

/* Recognizes exactly `index status` and `index clear` (surrounded only by
 * spaces). Every other form, including a missing or extra subcommand, is NONE
 * so the caller keeps the ordinary command/health priority. */
static IndexCommand index_management_parse(const uint8_t *line, uint32_t length)
{
    uint32_t position = skip_spaces(line, length, 0u);
    uint32_t start = position;
    while (position < length && line[position] != (uint8_t)' ') {
        ++position;
    }
    if (!token_is(line, start, position - start, "index")) {
        return INDEX_CMD_NONE;
    }
    position = skip_spaces(line, length, position);
    start = position;
    while (position < length && line[position] != (uint8_t)' ') {
        ++position;
    }
    uint32_t sub_length = position - start;
    if (!rest_is_spaces(line, length, position)) {
        return INDEX_CMD_NONE;
    }
    if (sub_length == 6u && token_is(line, start, sub_length, "status")) {
        return INDEX_CMD_STATUS;
    }
    if (sub_length == 5u && token_is(line, start, sub_length, "clear")) {
        return INDEX_CMD_CLEAR;
    }
    return INDEX_CMD_NONE;
}

static YanShellResult run_index_command(YanShell *shell, IndexCommand command)
{
    if (command == INDEX_CMD_STATUS) {
        return run_index_status(shell);
    }
    return run_index_clear(shell);
}

static YanShellResult run_line(YanShell *shell, const uint8_t *line,
                               uint32_t length)
{
    uint32_t command_start = skip_spaces(line, length, 0u);
    if (command_start == length) {
        /* An empty or all-space line only asks for the next prompt. */
        return YAN_SHELL_OK;
    }
    uint32_t position = command_start;
    while (position < length && line[position] != (uint8_t)' ') {
        ++position;
    }
    uint32_t command_length = position - command_start;

    if (token_is(line, command_start, command_length, "help")) {
        if (!rest_is_spaces(line, length, position)) {
            return emit_status(shell, "ERROR USAGE\r\n");
        }
        if (!shell_puts(shell, HELP_TEXT)) {
            return YAN_SHELL_FATAL;
        }
        return emit_ok(shell, "help") ? YAN_SHELL_OK : YAN_SHELL_FATAL;
    }
    if (token_is(line, command_start, command_length, "ls")) {
        if (!rest_is_spaces(line, length, position)) {
            return emit_status(shell, "ERROR USAGE\r\n");
        }
        return run_ls(shell);
    }
    if (token_is(line, command_start, command_length, "stat")) {
        uint32_t name_start = 0u;
        uint32_t name_length = 0u;
        if (!parse_one_name(line, length, position, &name_start, &name_length)) {
            return emit_status(shell, "ERROR USAGE\r\n");
        }
        return run_stat(shell, line, name_start, name_length);
    }
    if (token_is(line, command_start, command_length, "cat")) {
        uint32_t name_start = 0u;
        uint32_t name_length = 0u;
        if (!parse_one_name(line, length, position, &name_start, &name_length)) {
            return emit_status(shell, "ERROR USAGE\r\n");
        }
        return run_cat(shell, line, name_start, name_length);
    }
    bool create = token_is(line, command_start, command_length, "create");
    bool write = token_is(line, command_start, command_length, "write");
    if (create || write) {
        uint32_t name_start = 0u;
        uint32_t name_length = 0u;
        uint32_t text_start = 0u;
        uint32_t text_length = 0u;
        if (!parse_name_and_text(line, length, position, &name_start, &name_length,
                                 &text_start, &text_length)) {
            return emit_status(shell, "ERROR USAGE\r\n");
        }
        return run_create_or_write(shell, create, line, name_start, name_length,
                                   text_start, text_length);
    }
    if (token_is(line, command_start, command_length, "rm")) {
        uint32_t name_start = 0u;
        uint32_t name_length = 0u;
        if (!parse_one_name(line, length, position, &name_start, &name_length)) {
            return emit_status(shell, "ERROR USAGE\r\n");
        }
        return run_rm(shell, line, name_start, name_length);
    }
    bool mv = token_is(line, command_start, command_length, "mv");
    bool cp = token_is(line, command_start, command_length, "cp");
    if (mv || cp) {
        uint32_t first_start = 0u;
        uint32_t first_length = 0u;
        uint32_t second_start = 0u;
        uint32_t second_length = 0u;
        if (!parse_two_names(line, length, position, &first_start, &first_length,
                             &second_start, &second_length)) {
            return emit_status(shell, "ERROR USAGE\r\n");
        }
        return mv ? run_mv(shell, line, first_start, first_length, second_start,
                           second_length)
                  : run_cp(shell, line, first_start, first_length, second_start,
                           second_length);
    }
    if (token_is(line, command_start, command_length, "grep")) {
        return run_grep(shell, line, length, position);
    }
    if (token_is(line, command_start, command_length, "search")) {
        return run_search(shell, line, length, position);
    }
    if (token_is(line, command_start, command_length, "rebuild")) {
        if (!rest_is_spaces(line, length, position)) {
            return emit_status(shell, "ERROR USAGE\r\n");
        }
        return run_rebuild(shell);
    }
    IndexCommand index_command = index_management_parse(line, length);
    if (index_command != INDEX_CMD_NONE) {
        return run_index_command(shell, index_command);
    }
    if (token_is(line, command_start, command_length, "index")) {
        /* A valid form is routed before the health guard; any `index` that
         * reaches here has a bad subcommand or trailing token. */
        return emit_status(shell, "ERROR USAGE\r\n");
    }
    if (token_is(line, command_start, command_length, "exit")) {
        if (!rest_is_spaces(line, length, position)) {
            return emit_status(shell, "ERROR USAGE\r\n");
        }
        return emit_ok(shell, "exit") ? YAN_SHELL_EXIT : YAN_SHELL_FATAL;
    }
    return emit_status(shell, "ERROR UNKNOWN_COMMAND\r\n");
}

/* Runs with busy already set. The whole-line validation comes first and touches
 * no filesystem state: a line that is too long or carries a control byte is
 * refused before the health guard and before any command's filesystem call,
 * which is what 0022's "整行验证发生在 FS 调用前" requires. The health guard
 * then reads the borrowed filesystem's state fields only, so even it performs
 * no filesystem API call. */
static YanShellResult execute_busy(YanShell *shell, const uint8_t *line,
                                   uint32_t length)
{
    if (length > YAN_SHELL_LINE_MAX) {
        return emit_status(shell, "ERROR LINE_TOO_LONG\r\n");
    }
    for (uint32_t i = 0; i < length; ++i) {
        uint8_t byte = line[i];
        if (byte <= 0x1Fu || byte == 0x7Fu) {
            return emit_status(shell, "ERROR INVALID_INPUT\r\n");
        }
    }
    /* 0026: an exact valid `index status`/`index clear` is a pure memory
     * operation that is legal on an unmounted, faulted or uninitialized source,
     * so it is routed before the filesystem health guard. Every other line,
     * including an invalid management grammar and an empty or exit line, keeps
     * the original health priority below. */
    IndexCommand management = index_management_parse(line, length);
    if (management != INDEX_CMD_NONE) {
        return run_index_command(shell, management);
    }
    YanFsResult health = shell_fs_health(shell);
    if (health != YAN_FS_OK) {
        return finish_fs_error(shell, health);
    }
    return run_line(shell, line, length);
}

/* A zero-length span is absent and may have a NULL base. A nonempty span needs
 * a base and a range that does not leave uintptr_t. No byte is read. */
static bool shell_span_valid(uintptr_t base, uint64_t size)
{
    if (size == 0u) {
        return true;
    }
    if (base == 0u) {
        return false;
    }
    return size <= (uint64_t)(UINTPTR_MAX - base);
}

YanShellResult yan_shell_init(YanShell *shell, YanFs *fs, YanSearch *search,
                              YanSearchTerms *terms, YanShellOutput output)
{
    if (shell == NULL) {
        return YAN_SHELL_INVALID;
    }
    uintptr_t shell_base = (uintptr_t)(const void *)shell;
    /* Pure address arithmetic for the four pointers this call holds, before
     * the first bool field of any context is read: a shell placed inside the
     * filesystem, the literal Search or the term facade is rejected without an
     * undefined load. */
    if (fs != NULL &&
        ranges_overlap(shell_base, (uint64_t)sizeof(YanShell),
                       (uintptr_t)(const void *)fs, (uint64_t)sizeof(YanFs))) {
        return YAN_SHELL_INVALID;
    }
    if (search != NULL &&
        ranges_overlap(shell_base, (uint64_t)sizeof(YanShell),
                       (uintptr_t)(const void *)search,
                       (uint64_t)sizeof(YanSearch))) {
        return YAN_SHELL_INVALID;
    }
    if (terms != NULL &&
        ranges_overlap(shell_base, (uint64_t)sizeof(YanShell),
                       (uintptr_t)(const void *)terms,
                       (uint64_t)sizeof(YanSearchTerms))) {
        return YAN_SHELL_INVALID;
    }
    if (fs != NULL && search != NULL &&
        ranges_overlap((uintptr_t)(const void *)fs, (uint64_t)sizeof(YanFs),
                       (uintptr_t)(const void *)search,
                       (uint64_t)sizeof(YanSearch))) {
        return YAN_SHELL_INVALID;
    }
    if (fs != NULL && terms != NULL &&
        ranges_overlap((uintptr_t)(const void *)fs, (uint64_t)sizeof(YanFs),
                       (uintptr_t)(const void *)terms,
                       (uint64_t)sizeof(YanSearchTerms))) {
        return YAN_SHELL_INVALID;
    }
    if (search != NULL && terms != NULL &&
        ranges_overlap((uintptr_t)(const void *)search,
                       (uint64_t)sizeof(YanSearch),
                       (uintptr_t)(const void *)terms,
                       (uint64_t)sizeof(YanSearchTerms))) {
        return YAN_SHELL_INVALID;
    }
    if (fs == NULL || search == NULL || terms == NULL) {
        /* Nothing to compare addresses against; the original BUSY priority for
         * an active instance is preserved. */
        if (shell->initialized && shell->busy) {
            return YAN_SHELL_BUSY;
        }
        return YAN_SHELL_INVALID;
    }
    if (!search->initialized || !terms->initialized) {
        if (shell->initialized && shell->busy) {
            return YAN_SHELL_BUSY;
        }
        return YAN_SHELL_INVALID;
    }
    /* Both facades are initialized, so their backend records are valid. Each
     * carries a context span and a source span; all four must be well formed. */
    uintptr_t literal_context =
        (uintptr_t)(const void *)search->backend.context;
    uint64_t literal_context_size = (uint64_t)search->backend.context_size;
    uintptr_t literal_source =
        (uintptr_t)(const void *)search->backend.source_context;
    uint64_t literal_source_size = (uint64_t)search->backend.source_size;
    uintptr_t term_context = (uintptr_t)(const void *)terms->backend.context;
    uint64_t term_context_size = (uint64_t)terms->backend.context_size;
    uintptr_t term_source =
        (uintptr_t)(const void *)terms->backend.source_context;
    uint64_t term_source_size = (uint64_t)terms->backend.source_size;
    if (!shell_span_valid(literal_context, literal_context_size) ||
        !shell_span_valid(literal_source, literal_source_size) ||
        !shell_span_valid(term_context, term_context_size) ||
        !shell_span_valid(term_source, term_source_size)) {
        return YAN_SHELL_INVALID;
    }
    uintptr_t fs_base = (uintptr_t)(const void *)fs;
    uintptr_t search_base = (uintptr_t)(const void *)search;
    uintptr_t terms_base = (uintptr_t)(const void *)terms;
    /* The Shell, filesystem, the two facades and the two backend contexts are
     * all writable objects and must be pairwise disjoint. A source span may
     * legitimately be the borrowed filesystem itself (that is exactly what the
     * linear and index backends declare), and the two sources may overlap each
     * other read-only, so fs-vs-source and source-vs-source are deliberately
     * not compared. A source must still stay off every foreign object. */
    if (ranges_overlap(shell_base, (uint64_t)sizeof(YanShell),
                       literal_context, literal_context_size) ||
        ranges_overlap(shell_base, (uint64_t)sizeof(YanShell), term_context,
                       term_context_size) ||
        ranges_overlap(shell_base, (uint64_t)sizeof(YanShell), literal_source,
                       literal_source_size) ||
        ranges_overlap(shell_base, (uint64_t)sizeof(YanShell), term_source,
                       term_source_size) ||
        ranges_overlap(fs_base, (uint64_t)sizeof(YanFs), literal_context,
                       literal_context_size) ||
        ranges_overlap(fs_base, (uint64_t)sizeof(YanFs), term_context,
                       term_context_size) ||
        ranges_overlap(search_base, (uint64_t)sizeof(YanSearch),
                       literal_context, literal_context_size) ||
        ranges_overlap(search_base, (uint64_t)sizeof(YanSearch), term_context,
                       term_context_size) ||
        ranges_overlap(search_base, (uint64_t)sizeof(YanSearch),
                       literal_source, literal_source_size) ||
        ranges_overlap(search_base, (uint64_t)sizeof(YanSearch), term_source,
                       term_source_size) ||
        ranges_overlap(terms_base, (uint64_t)sizeof(YanSearchTerms),
                       literal_context, literal_context_size) ||
        ranges_overlap(terms_base, (uint64_t)sizeof(YanSearchTerms),
                       term_context, term_context_size) ||
        ranges_overlap(terms_base, (uint64_t)sizeof(YanSearchTerms),
                       literal_source, literal_source_size) ||
        ranges_overlap(terms_base, (uint64_t)sizeof(YanSearchTerms),
                       term_source, term_source_size) ||
        ranges_overlap(literal_context, literal_context_size, term_context,
                       term_context_size) ||
        ranges_overlap(literal_context, literal_context_size, literal_source,
                       literal_source_size) ||
        ranges_overlap(literal_context, literal_context_size, term_source,
                       term_source_size) ||
        ranges_overlap(literal_source, literal_source_size, term_context,
                       term_context_size) ||
        ranges_overlap(term_context, term_context_size, term_source,
                       term_source_size)) {
        return YAN_SHELL_INVALID;
    }
    if (shell->initialized && shell->busy) {
        return YAN_SHELL_BUSY;
    }
    if (output.putc == NULL) {
        return YAN_SHELL_INVALID;
    }
    shell->fs = fs;
    shell->search = search;
    shell->terms = terms;
    shell->output = output;
    shell->initialized = true;
    shell->busy = false;
    return YAN_SHELL_OK;
}

YanShellResult yan_shell_execute(YanShell *shell, const uint8_t *line,
                                 uint32_t length)
{
    if (shell == NULL || !shell->initialized) {
        return YAN_SHELL_INVALID;
    }
    if (shell->busy) {
        return YAN_SHELL_BUSY;
    }

    /* A zero-length line needs no buffer, so NULL is accepted exactly there and
     * then follows the same path as a non-NULL empty line: it must still see
     * the filesystem health guard, so a fatal filesystem cannot be continued
     * just because the line was empty. */
    if (line == NULL) {
        if (length != 0u) {
            return YAN_SHELL_INVALID;
        }
    } else {
        uintptr_t begin = (uintptr_t)(const void *)line;
        if ((uint64_t)length > (uint64_t)(UINTPTR_MAX - begin)) {
            return YAN_SHELL_INVALID;
        }
        /* The line must not be part of the objects this call reads or writes.
         * The overlap test happens before busy is set, so a rejected call
         * cannot have written to a caller buffer that aliases the instance. */
        if (ranges_overlap((uintptr_t)(const void *)shell,
                           (uint64_t)sizeof(YanShell), begin, (uint64_t)length) ||
            ranges_overlap((uintptr_t)(const void *)shell->fs,
                           (uint64_t)sizeof(YanFs), begin, (uint64_t)length)) {
            return YAN_SHELL_INVALID;
        }
        /* The injected literal Search and its two protected spans are borrowed
         * too, so the command line may not alias them either. */
        if (shell->search != NULL &&
            (ranges_overlap((uintptr_t)(const void *)shell->search,
                            (uint64_t)sizeof(YanSearch), begin,
                            (uint64_t)length) ||
             ranges_overlap(
                 (uintptr_t)(const void *)shell->search->backend.context,
                 (uint64_t)shell->search->backend.context_size, begin,
                 (uint64_t)length) ||
             ranges_overlap(
                 (uintptr_t)(const void *)shell->search->backend.source_context,
                 (uint64_t)shell->search->backend.source_size, begin,
                 (uint64_t)length))) {
            return YAN_SHELL_INVALID;
        }
        /* The term facade and its two spans are borrowed for the whole call as
         * well. */
        if (shell->terms != NULL &&
            (ranges_overlap((uintptr_t)(const void *)shell->terms,
                            (uint64_t)sizeof(YanSearchTerms), begin,
                            (uint64_t)length) ||
             ranges_overlap(
                 (uintptr_t)(const void *)shell->terms->backend.context,
                 (uint64_t)shell->terms->backend.context_size, begin,
                 (uint64_t)length) ||
             ranges_overlap(
                 (uintptr_t)(const void *)shell->terms->backend.source_context,
                 (uint64_t)shell->terms->backend.source_size, begin,
                 (uint64_t)length))) {
            return YAN_SHELL_INVALID;
        }
    }

    shell->busy = true;
    YanShellResult result = execute_busy(shell, line, length);
    shell->busy = false;
    return result;
}
