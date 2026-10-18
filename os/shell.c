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
 * that follows it.
 *
 * 0027 adds an optional metadata layer behind yan_shell_bind_metadata. It
 * changes no old rule: an unbound shell is byte-for-byte the 0022-0026 shell.
 * When bound, `meta show|set|tag|link|list` are routed after the filesystem
 * health guard, a bare `search` query is split into exact `tag:` operators and
 * keyword tokens, and a whole outer-quoted query keeps the old keyword grammar.
 * The shell never traverses the filesystem, never parses a header and never
 * filters a ranked list: it validates grammar and values, calls the note
 * service or the filtered term entry point, and formats the result. Every
 * command validates its complete grammar before the first service call, so a
 * USAGE answer performs zero filesystem I/O, and every metadata declaration
 * value is validated before that call as well.
 *
 * 0028 adds an optional relation layer behind yan_shell_bind_relations. It
 * changes no old rule: an unbound shell is byte-for-byte the 0022-0027 shell
 * and `links`, `backlinks` and `related` stay UNKNOWN_COMMAND. When bound, each
 * command parses exactly one legal name, calls the independent note-relations
 * service once, formats one CRLF row per callback and then the summary receipt.
 * The shell never walks the directory, prechecks a header or ranks a row, and a
 * refused output byte makes the service STOPPED, which returns fatal with no
 * receipt and no retry. */
#include "shell.h"

#include "memory.h"

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

/* 0027 help for a shell with a bound metadata context. It is the original text
 * plus the metadata and tag-search lines, so an unbound shell prints HELP_TEXT
 * byte for byte and a bound shell advertises only the capabilities it has. */
static const char HELP_METADATA[] =
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
    "meta show NAME: print the note's declared metadata\r\n"
    "meta set NAME title|created|modified VALUE: declare or clear a field\r\n"
    "meta tag NAME add|del TAG: change one tag\r\n"
    "meta link NAME add|del TARGET: change one link\r\n"
    "meta list [tag:TAG ...]: list notes whose tags match\r\n"
    "search tag:TAG QUERY: search only notes carrying every tag\r\n"
    "exit: end the session\r\n"
    "line: at most 1023 bytes, ended by CR or LF\r\n"
    "separators: ASCII spaces; commands and names are case sensitive\r\n"
    "names: 1 to 31 bytes of A-Z a-z 0-9 . _ - and never . or ..\r\n"
    "create creates only; write replaces only; no LF is added to text\r\n"
    "cat: printable ASCII as is, a backslash as two backslashes, C0 and DEL as \\xHH\r\n"
    "cat: valid UTF-8 as is except C1 U+0080 to U+009F, escaped byte by byte\r\n";

/* 0028 help fragment for a shell with a bound relation context. It is appended
 * after the base help (metadata-bound or not), so the three relation commands
 * are advertised independently of the metadata binding and an unbound shell
 * still prints only HELP_TEXT. */
static const char HELP_RELATIONS[] =
    "links NAME: list a note's declared outgoing links\r\n"
    "backlinks NAME: list files that declare NAME\r\n"
    "related NAME: list files related by a link or a shared tag\r\n";

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
    case YAN_SEARCH_TERMS_METADATA: return "METADATA";
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
    case YAN_SEARCH_TERMS_METADATA:
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
    /* The bare form is the whole remainder. It may contain runs of spaces; the
     * term parser treats them as equivalent group separators. */
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

/* 0027 tag operator for a bound shell. It runs only for a bare query: the
 * whole outer-quoted form keeps the old 0026 grammar. Each ASCII token is
 * either an exact `tag:` operator with a raw suffix, or a keyword token copied
 * into the shell's keyword window. At least one keyword token must remain, tag
 * suffixes are de-duplicated before the 16-slot bound, and the keyword tokens
 * keep the old no-inner-quote rule. */
static bool search_parse_tags(YanShell *shell, const uint8_t *line,
                              uint32_t length, uint32_t position,
                              uint32_t *keyword_start,
                              uint32_t *keyword_length)
{
    YanNoteTagFilter *tag_filter = &shell->metadata_filter;
    memset(tag_filter, 0, sizeof *tag_filter);
    bool have_keyword = false;
    uint32_t written = 0u;
    uint32_t at = position;
    for (;;) {
        while (at < length && line[at] == (uint8_t)' ') {
            ++at;
        }
        if (at == length) {
            break;
        }
        uint32_t token_start = at;
        while (at < length && line[at] != (uint8_t)' ') {
            ++at;
        }
        uint32_t token_length = at - token_start;
        if (token_length >= 4u &&
            line[token_start] == (uint8_t)'t' &&
            line[token_start + 1u] == (uint8_t)'a' &&
            line[token_start + 2u] == (uint8_t)'g' &&
            line[token_start + 3u] == (uint8_t)':') {
            uint32_t suffix_start = token_start + 4u;
            uint32_t suffix_length = token_length - 4u;
            /* The authoritative rule already enforces strict UTF-8, the fixed
             * White_Space set and the control ranges, so a raw suffix such as
             * `a"b` is legal and no second validator is needed. */
            if (!yan_note_tag_valid(line + suffix_start, suffix_length)) {
                return false;
            }
            bool duplicate = false;
            for (uint16_t i = 0u; i < tag_filter->count; ++i) {
                if (tag_filter->length[i] == (uint16_t)suffix_length &&
                    memcmp(tag_filter->tag[i], line + suffix_start,
                           suffix_length) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                continue;
            }
            if (tag_filter->count >= YAN_NOTE_TAG_MAX_COUNT) {
                return false; /* the 17th distinct tag is USAGE */
            }
            tag_filter->length[tag_filter->count] =
                (uint16_t)suffix_length;
            memcpy(tag_filter->tag[tag_filter->count], line + suffix_start,
                   suffix_length);
            tag_filter->count += 1u;
            continue;
        }
        /* Tag removal must preserve the old bare keyword grammar: only its
         * first keyword may not start with '-', even when tags preceded it. */
        if (!have_keyword && line[token_start] == (uint8_t)'-') {
            return false;
        }
        /* A keyword token keeps the old rule: no inner double quote. */
        for (uint32_t i = token_start; i < token_start + token_length; ++i) {
            if (line[i] == (uint8_t)'"') {
                return false;
            }
        }
        /* Only keyword tokens enter the keyword window, each separated by one
         * ASCII space, so an interleaved tag operator leaves the keyword query
         * with exactly the meaning it would have had without the operator. */
        if (have_keyword) {
            if (written >= YAN_SEARCH_TERMS_QUERY_MAX) {
                return false;
            }
            shell->keywords[written] = (uint8_t)' ';
            written += 1u;
        }
        if (written + token_length > YAN_SEARCH_TERMS_QUERY_MAX ||
            written + token_length > sizeof shell->keywords - 1u) {
            return false;
        }
        memcpy(shell->keywords + written, line + token_start, token_length);
        written += token_length;
        have_keyword = true;
    }
    if (!have_keyword || written == 0u) {
        return false; /* tag-only or an empty bare query is USAGE */
    }
    *keyword_start = 0u;
    *keyword_length = written;
    return true;
}

static YanShellResult run_search_terms(YanShell *shell, const uint8_t *query,
                                       uint32_t query_length,
                                       const YanNoteTagFilter *filter)
{
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
        (filter != NULL)
            ? yan_search_terms_filtered(shell->terms, query, query_length,
                                        filter, sink, &summary)
            : yan_search_terms(shell->terms, query, query_length, sink,
                               &summary);
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

/* `search QUERY`: the first non-space byte decides the grammar. A whole
 * outer-quoted query, and every query on an unbound shell, keeps the old 0026
 * parse, which rejects any inner double quote. A bare query on a bound shell
 * bypasses that whole-query check and is split per token: an exact `tag:`
 * operator takes its raw suffix (quotes and backslashes are literal bytes), and
 * each remaining keyword token keeps the old no-inner-quote rule. The keyword
 * window is reassembled from the keyword tokens only, so an interleaved tag
 * leaves the keyword query unchanged. Only a real tag selection reaches the
 * filtered entry point. */
static YanShellResult run_search(YanShell *shell, const uint8_t *line,
                                 uint32_t length, uint32_t position)
{
    uint32_t first = position;
    while (first < length && line[first] == (uint8_t)' ') {
        ++first;
    }
    bool outer_quoted = first < length && line[first] == (uint8_t)'"';
    if (shell->metadata == NULL || outer_quoted) {
        const uint8_t *query = NULL;
        uint32_t query_length = 0u;
        if (!search_parse_query(line, length, position, &query,
                                &query_length)) {
            return emit_status(shell, "ERROR USAGE\r\n");
        }
        return run_search_terms(shell, query, query_length, NULL);
    }
    uint32_t keyword_start = 0u;
    uint32_t keyword_length = 0u;
    if (!search_parse_tags(shell, line, length, position, &keyword_start,
                           &keyword_length)) {
        return emit_status(shell, "ERROR USAGE\r\n");
    }
    const YanNoteTagFilter *filter = shell->metadata_filter.count > 0u
                                         ? &shell->metadata_filter
                                         : NULL;
    /* A bound bare query with no tag still goes through the keyword window,
     * which holds exactly the old query text with canonical single separators;
     * the facade applies the old grammar to it. */
    return run_search_terms(shell, shell->keywords, keyword_length, filter);
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

/* ------------------------------------------------------- 0027 metadata ---- */

/* One ASCII token: spaces around it are skipped, and a token runs to the next
 * space or the end of the line. Returns false at end of line. */
static bool meta_next_token(const uint8_t *line, uint32_t length,
                            uint32_t *position, uint32_t *start,
                            uint32_t *token_length)
{
    uint32_t at = skip_spaces(line, length, *position);
    if (at == length) {
        return false;
    }
    uint32_t begin = at;
    while (at < length && line[at] != (uint8_t)' ') {
        ++at;
    }
    *position = at;
    *start = begin;
    *token_length = at - begin;
    return true;
}

/* True when, after spaces, the line has no token whose first byte is not a
 * space: i.e. only spaces remain. */
static bool meta_only_spaces_left(const uint8_t *line, uint32_t length,
                                  uint32_t position)
{
    return skip_spaces(line, length, position) == length;
}

static bool meta_add_or_del(const uint8_t *line, uint32_t start,
                            uint32_t token_length, YanNoteManageMode *mode)
{
    if (token_is(line, start, token_length, "add")) {
        *mode = YAN_NOTE_MANAGE_ADD;
        return true;
    }
    if (token_is(line, start, token_length, "del")) {
        *mode = YAN_NOTE_MANAGE_DELETE;
        return true;
    }
    return false;
}

static const char *note_error_suffix(YanNoteResult result)
{
    switch (result) {
    case YAN_NOTE_INVALID: return "INVALID";
    case YAN_NOTE_BUSY: return "BUSY";
    case YAN_NOTE_STOPPED: return "STOPPED";
    case YAN_NOTE_METADATA: return "METADATA";
    case YAN_NOTE_TOO_LARGE: return "TOO_LARGE";
    case YAN_NOTE_INVALID_TEXT: return "INVALID_TEXT";
    case YAN_NOTE_NOT_MOUNTED: return "NOT_MOUNTED";
    case YAN_NOTE_FAULTED: return "FAULTED";
    case YAN_NOTE_NOT_FOUND: return "NOT_FOUND";
    case YAN_NOTE_EXISTS: return "EXISTS";
    case YAN_NOTE_NOSPACE: return "NOSPACE";
    case YAN_NOTE_CORRUPT: return "CORRUPT";
    case YAN_NOTE_DIRECTORY_FULL: return "DIRECTORY_FULL";
    case YAN_NOTE_IO: return "IO";
    case YAN_NOTE_PROTOCOL: return "PROTOCOL";
    case YAN_NOTE_UNSUPPORTED: return "UNSUPPORTED";
    case YAN_NOTE_OK: return "OK";
    }
    return NULL;
}

static bool note_error_is_fatal(YanNoteResult result)
{
    switch (result) {
    case YAN_NOTE_NOT_MOUNTED:
    case YAN_NOTE_FAULTED:
    case YAN_NOTE_IO:
    case YAN_NOTE_PROTOCOL:
    case YAN_NOTE_CORRUPT:
    case YAN_NOTE_UNSUPPORTED:
        return true;
    case YAN_NOTE_OK:
    case YAN_NOTE_INVALID:
    case YAN_NOTE_BUSY:
    case YAN_NOTE_STOPPED:
    case YAN_NOTE_METADATA:
    case YAN_NOTE_TOO_LARGE:
    case YAN_NOTE_INVALID_TEXT:
    case YAN_NOTE_NOT_FOUND:
    case YAN_NOTE_EXISTS:
    case YAN_NOTE_NOSPACE:
    case YAN_NOTE_DIRECTORY_FULL:
        return false;
    }
    return true;
}

static YanShellResult finish_note_error(YanShell *shell, YanNoteResult result)
{
    const char *suffix = note_error_suffix(result);
    bool known = suffix != NULL;
    if (suffix == NULL) {
        suffix = "PROTOCOL";
    }
    if (!shell_puts(shell, "ERROR ") || !shell_puts(shell, suffix) ||
        !shell_puts(shell, "\r\n")) {
        return YAN_SHELL_FATAL;
    }
    if (!known) {
        return YAN_SHELL_FATAL;
    }
    return note_error_is_fatal(result) ? YAN_SHELL_FATAL : YAN_SHELL_OK;
}

/* One declaration token or the rest of the line as raw title text. */
static YanShellResult run_meta_set(YanShell *shell, const uint8_t *line,
                                   uint32_t length, uint32_t position)
{
    uint32_t name_start = 0u;
    uint32_t name_length = 0u;
    if (!meta_next_token(line, length, &position, &name_start, &name_length)) {
        return emit_status(shell, "ERROR USAGE\r\n");
    }
    uint32_t field_start = 0u;
    uint32_t field_length = 0u;
    if (!meta_next_token(line, length, &position, &field_start,
                         &field_length)) {
        return emit_status(shell, "ERROR USAGE\r\n");
    }
    char name[YAN_FS_NAME_MAX + 2u];
    copy_name(line, name_start, name_length, name);
    if (!yan_note_name_valid((const uint8_t *)name, name_length)) {
        return emit_status(shell, "ERROR USAGE\r\n");
    }
    if (token_is(line, field_start, field_length, "title")) {
        /* The title value starts exactly one separator after the `title` token:
         * any further leading and trailing spaces are part of the value, and an
         * absent or empty value clears the field. The value is validated here,
         * so an over-long or control-carrying title is USAGE with zero I/O. */
        uint32_t text_start = position < length ? position + 1u : length;
        uint32_t text_length = length - text_start;
        if (text_length > 0u &&
            !yan_note_title_valid(line + text_start, text_length)) {
            return emit_status(shell, "ERROR USAGE\r\n");
        }
        YanNoteResult result = yan_note_set_title(
            shell->metadata, name, line + text_start, text_length);
        if (result != YAN_NOTE_OK) {
            return finish_note_error(shell, result);
        }
        return emit_ok(shell, "meta set") ? YAN_SHELL_OK : YAN_SHELL_FATAL;
    }
    YanNoteDateField field;
    if (token_is(line, field_start, field_length, "created")) {
        field = YAN_NOTE_DATE_CREATED;
    } else if (token_is(line, field_start, field_length, "modified")) {
        field = YAN_NOTE_DATE_MODIFIED;
    } else {
        return emit_status(shell, "ERROR USAGE\r\n");
    }
    uint32_t date_start = 0u;
    uint32_t date_length = 0u;
    if (!meta_next_token(line, length, &position, &date_start, &date_length)) {
        return emit_status(shell, "ERROR USAGE\r\n");
    }
    if (!meta_only_spaces_left(line, length, position)) {
        return emit_status(shell, "ERROR USAGE\r\n");
    }
    bool clearing = date_length == 1u && line[date_start] == (uint8_t)'-';
    if (!clearing && !yan_note_date_valid(line + date_start, date_length)) {
        return emit_status(shell, "ERROR USAGE\r\n");
    }
    YanNoteResult result =
        yan_note_set_date(shell->metadata, name, field,
                          clearing ? NULL : line + date_start,
                          clearing ? 0u : date_length);
    if (result != YAN_NOTE_OK) {
        return finish_note_error(shell, result);
    }
    return emit_ok(shell, "meta set") ? YAN_SHELL_OK : YAN_SHELL_FATAL;
}

static YanShellResult run_meta_tag_or_link(YanShell *shell,
                                           const uint8_t *line,
                                           uint32_t length, uint32_t position,
                                           bool link)
{
    uint32_t name_start = 0u;
    uint32_t name_length = 0u;
    if (!meta_next_token(line, length, &position, &name_start, &name_length)) {
        return emit_status(shell, "ERROR USAGE\r\n");
    }
    uint32_t mode_start = 0u;
    uint32_t mode_length = 0u;
    if (!meta_next_token(line, length, &position, &mode_start, &mode_length)) {
        return emit_status(shell, "ERROR USAGE\r\n");
    }
    YanNoteManageMode mode;
    if (!meta_add_or_del(line, mode_start, mode_length, &mode)) {
        return emit_status(shell, "ERROR USAGE\r\n");
    }
    uint32_t value_start = 0u;
    uint32_t value_length = 0u;
    if (!meta_next_token(line, length, &position, &value_start,
                         &value_length)) {
        return emit_status(shell, "ERROR USAGE\r\n");
    }
    if (!meta_only_spaces_left(line, length, position)) {
        return emit_status(shell, "ERROR USAGE\r\n");
    }
    char name[YAN_FS_NAME_MAX + 2u];
    copy_name(line, name_start, name_length, name);
    /* The source name is validated here, so a name with an illegal byte or an
     * over-long token is USAGE before any service call. */
    if (!yan_note_name_valid((const uint8_t *)name, name_length)) {
        return emit_status(shell, "ERROR USAGE\r\n");
    }
    YanNoteResult result;
    if (link) {
        char target[YAN_FS_NAME_MAX + 2u];
        copy_name(line, value_start, value_length, target);
        if (!yan_note_name_valid((const uint8_t *)target, value_length)) {
            return emit_status(shell, "ERROR USAGE\r\n");
        }
        result = yan_note_link(shell->metadata, name, mode, target);
        if (result != YAN_NOTE_OK) {
            return finish_note_error(shell, result);
        }
        return emit_ok(shell, "meta link") ? YAN_SHELL_OK : YAN_SHELL_FATAL;
    }
    if (!yan_note_tag_valid(line + value_start, value_length)) {
        return emit_status(shell, "ERROR USAGE\r\n");
    }
    result = yan_note_tag(shell->metadata, name, mode, line + value_start,
                          value_length);
    if (result != YAN_NOTE_OK) {
        return finish_note_error(shell, result);
    }
    return emit_ok(shell, "meta tag") ? YAN_SHELL_OK : YAN_SHELL_FATAL;
}

/* Value formatting reuses the cat stream: valid UTF-8 passes through, invalid
 * bytes and the forbidden control ranges are escaped byte by byte. */
static bool note_emit_value(YanShell *shell, const uint8_t *bytes,
                            uint32_t length)
{
    Utf8Stream stream;
    stream.pending_len = 0u;
    stream.expected = 0u;
    for (uint32_t i = 0u; i < length; ++i) {
        if (!cat_feed(shell, &stream, bytes[i])) {
            return false;
        }
    }
    return cat_flush(shell, &stream);
}

static YanShellResult run_meta_show(YanShell *shell, const uint8_t *line,
                                    uint32_t length, uint32_t position)
{
    uint32_t name_start = 0u;
    uint32_t name_length = 0u;
    if (!meta_next_token(line, length, &position, &name_start, &name_length)) {
        return emit_status(shell, "ERROR USAGE\r\n");
    }
    if (!meta_only_spaces_left(line, length, position)) {
        return emit_status(shell, "ERROR USAGE\r\n");
    }
    char name[YAN_FS_NAME_MAX + 2u];
    copy_name(line, name_start, name_length, name);
    if (!yan_note_name_valid((const uint8_t *)name, name_length)) {
        return emit_status(shell, "ERROR USAGE\r\n");
    }
    YanNoteResult result =
        yan_note_show(shell->metadata, name, &shell->metadata_info);
    if (result != YAN_NOTE_OK) {
        return finish_note_error(shell, result);
    }
    const YanNoteInfo *info = &shell->metadata_info;
    if (!shell_puts(shell, "META ") ||
        !shell_put_bytes(shell, (const uint8_t *)name, name_length) ||
        !shell_puts(shell, "\r\n")) {
        return YAN_SHELL_FATAL;
    }
    if (!shell_puts(shell, "TITLE ")) {
        return YAN_SHELL_FATAL;
    }
    if (info->has_title) {
        if (!note_emit_value(shell, info->title, info->title_length)) {
            return YAN_SHELL_FATAL;
        }
    } else if (!shell_puts(shell, "UNKNOWN")) {
        return YAN_SHELL_FATAL;
    }
    if (!shell_puts(shell, "\r\n")) {
        return YAN_SHELL_FATAL;
    }
    if (!shell_puts(shell, "CREATED ")) {
        return YAN_SHELL_FATAL;
    }
    if (info->has_created) {
        if (!shell_put_bytes(shell, info->created, info->created_length)) {
            return YAN_SHELL_FATAL;
        }
    } else if (!shell_puts(shell, "UNKNOWN")) {
        return YAN_SHELL_FATAL;
    }
    if (!shell_puts(shell, "\r\n")) {
        return YAN_SHELL_FATAL;
    }
    if (!shell_puts(shell, "MODIFIED ")) {
        return YAN_SHELL_FATAL;
    }
    if (info->has_modified) {
        if (!shell_put_bytes(shell, info->modified, info->modified_length)) {
            return YAN_SHELL_FATAL;
        }
    } else if (!shell_puts(shell, "UNKNOWN")) {
        return YAN_SHELL_FATAL;
    }
    if (!shell_puts(shell, "\r\n")) {
        return YAN_SHELL_FATAL;
    }
    if (info->tag_count == 0u) {
        if (!shell_puts(shell, "TAG NONE\r\n")) {
            return YAN_SHELL_FATAL;
        }
    } else {
        for (uint16_t i = 0u; i < info->tag_count; ++i) {
            /* A tag may carry a backslash or any other raw byte, so it goes
             * through the same cat-safe stream as the title instead of raw. */
            if (!shell_puts(shell, "TAG ") ||
                !note_emit_value(shell, info->tag[i], info->tag_length[i]) ||
                !shell_puts(shell, "\r\n")) {
                return YAN_SHELL_FATAL;
            }
        }
    }
    if (info->link_count == 0u) {
        if (!shell_puts(shell, "LINK NONE\r\n")) {
            return YAN_SHELL_FATAL;
        }
    } else {
        for (uint16_t i = 0u; i < info->link_count; ++i) {
            if (!shell_puts(shell, "LINK ") ||
                !note_emit_value(shell, info->link[i], info->link_length[i]) ||
                !shell_puts(shell, info->link_exists[i] ? " OK\r\n"
                                                        : " DANGLING\r\n")) {
                return YAN_SHELL_FATAL;
            }
        }
    }
    return emit_ok(shell, "meta show") ? YAN_SHELL_OK : YAN_SHELL_FATAL;
}

typedef struct {
    YanShell *shell;
    bool output_failed;
    uint32_t count;
} MetaListState;

static bool meta_list_callback(void *context, const char *name)
{
    MetaListState *state = (MetaListState *)context;
    if (!shell_puts(state->shell, name) ||
        !shell_puts(state->shell, "\r\n")) {
        state->output_failed = true;
        return false;
    }
    state->count += 1u;
    return true;
}

static YanShellResult run_meta_list(YanShell *shell, const uint8_t *line,
                                    uint32_t length, uint32_t position)
{
    YanNoteTagFilter *tag_filter = &shell->metadata_filter;
    memset(tag_filter, 0, sizeof *tag_filter);
    uint32_t at = position;
    for (;;) {
        uint32_t value_start = 0u;
        uint32_t value_length = 0u;
        if (!meta_next_token(line, length, &at, &value_start, &value_length)) {
            break;
        }
        if (value_length < 4u ||
            !token_is(line, value_start, 4u, "tag:")) {
            return emit_status(shell, "ERROR USAGE\r\n");
        }
        uint32_t suffix_start = value_start + 4u;
        uint32_t suffix_length = value_length - 4u;
        /* The authoritative rule already enforces strict UTF-8, the fixed
         * White_Space set and the control ranges, so a raw suffix such as
         * `a"b` is legal and no second validator is needed. */
        if (!yan_note_tag_valid(line + suffix_start, suffix_length)) {
            return emit_status(shell, "ERROR USAGE\r\n");
        }
        /* Canonical de-duplication happens before the physical 16-slot bound:
         * a repeated tag adds nothing, and the 17th *distinct* tag is USAGE. */
        bool duplicate = false;
        for (uint16_t i = 0u; i < tag_filter->count; ++i) {
            if (tag_filter->length[i] == (uint16_t)suffix_length &&
                memcmp(tag_filter->tag[i], line + suffix_start,
                       suffix_length) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        if (tag_filter->count >= YAN_NOTE_TAG_MAX_COUNT) {
            return emit_status(shell, "ERROR USAGE\r\n");
        }
        tag_filter->length[tag_filter->count] = (uint16_t)suffix_length;
        memcpy(tag_filter->tag[tag_filter->count], line + suffix_start,
               suffix_length);
        tag_filter->count += 1u;
    }
    MetaListState state;
    state.shell = shell;
    state.output_failed = false;
    state.count = 0u;
    const YanNoteTagFilter *filter =
        tag_filter->count == 0u ? NULL : tag_filter;
    YanNoteResult result =
        yan_note_list(shell->metadata, filter, meta_list_callback, &state,
                      NULL);
    if (state.output_failed || result == YAN_NOTE_STOPPED) {
        return state.output_failed ? YAN_SHELL_FATAL : YAN_SHELL_OK;
    }
    if (result != YAN_NOTE_OK) {
        return finish_note_error(shell, result);
    }
    if (!shell_puts(shell, "OK meta list shown=") ||
        !shell_put_decimal(shell, state.count) ||
        !shell_puts(shell, "\r\n")) {
        return YAN_SHELL_FATAL;
    }
    return YAN_SHELL_OK;
}

static YanShellResult run_meta(YanShell *shell, const uint8_t *line,
                               uint32_t length, uint32_t position)
{
    uint32_t sub_start = 0u;
    uint32_t sub_length = 0u;
    if (!meta_next_token(line, length, &position, &sub_start, &sub_length)) {
        return emit_status(shell, "ERROR USAGE\r\n");
    }
    if (token_is(line, sub_start, sub_length, "show")) {
        return run_meta_show(shell, line, length, position);
    }
    if (token_is(line, sub_start, sub_length, "set")) {
        return run_meta_set(shell, line, length, position);
    }
    if (token_is(line, sub_start, sub_length, "tag")) {
        return run_meta_tag_or_link(shell, line, length, position, false);
    }
    if (token_is(line, sub_start, sub_length, "link")) {
        return run_meta_tag_or_link(shell, line, length, position, true);
    }
    if (token_is(line, sub_start, sub_length, "list")) {
        return run_meta_list(shell, line, length, position);
    }
    return emit_status(shell, "ERROR USAGE\r\n");
}

/* -------------------------------------------------------- relation commands */

/* Which of the three 0028 commands the shared emitter formats. */
typedef enum {
    RELATION_CMD_LINKS = 0,
    RELATION_CMD_BACKLINKS = 1,
    RELATION_CMD_RELATED = 2
} RelationCommand;

/* One synchronous callback's formatting state. It lives on the run_relation
 * frame, never in the service, and the borrowed row is used only until the
 * callback returns. */
typedef struct {
    YanShell *shell;
    RelationCommand command;
} RelationEmit;

/* Formats exactly one CRLF row for the borrowed relation row. Returning false
 * stops the service immediately (YAN_NOTE_STOPPED), which run_relation turns
 * into a fatal exit without a receipt or a retry. */
static bool relation_emit(void *context, const YanRelationRow *row)
{
    RelationEmit *emit = context;
    if (emit->command == RELATION_CMD_LINKS) {
        return shell_puts(emit->shell, "LINK ") &&
               shell_puts(emit->shell, row->name) &&
               shell_puts(emit->shell,
                          row->exists ? " OK\r\n" : " DANGLING\r\n");
    }
    if (emit->command == RELATION_CMD_BACKLINKS) {
        return shell_puts(emit->shell, "BACKLINK ") &&
               shell_puts(emit->shell, row->name) &&
               shell_puts(emit->shell, "\r\n");
    }
    return shell_puts(emit->shell, "RELATED ") &&
           shell_puts(emit->shell, row->name) &&
           shell_puts(emit->shell, " out=") &&
           shell_put_decimal(emit->shell, row->outgoing ? 1u : 0u) &&
           shell_puts(emit->shell, " in=") &&
           shell_put_decimal(emit->shell, row->incoming ? 1u : 0u) &&
           shell_puts(emit->shell, " tags=") &&
           shell_put_decimal(emit->shell, (uint32_t)row->shared_tags) &&
           shell_puts(emit->shell, "\r\n");
}

/* One relation command: exactly one legal name, one service call and one
 * summary receipt. The shell never walks the directory or ranks rows; every
 * row byte comes from the callback. Arity and name validity are checked here so
 * a malformed line is USAGE with zero I/O; a missing source or a broken header
 * is the ordinary service error through the shared note mapping, and an
 * I/O/protocol failure is fatal. */
static YanShellResult run_relation(YanShell *shell, const uint8_t *line,
                                   uint32_t length, uint32_t position,
                                   RelationCommand command)
{
    uint32_t name_start = 0u;
    uint32_t name_length = 0u;
    if (!parse_one_name(line, length, position, &name_start, &name_length)) {
        return emit_status(shell, "ERROR USAGE\r\n");
    }
    char name[YAN_FS_NAME_MAX + 2u];
    copy_name(line, name_start, name_length, name);
    if (!yan_note_name_valid((const uint8_t *)name, name_length)) {
        return emit_status(shell, "ERROR USAGE\r\n");
    }
    RelationEmit emit;
    emit.shell = shell;
    emit.command = command;
    YanRelationSummary summary = {0u, 0u, false};
    YanNoteResult result;
    if (command == RELATION_CMD_LINKS) {
        result = yan_relation_links(shell->relations, name, relation_emit,
                                    &emit, &summary);
    } else if (command == RELATION_CMD_BACKLINKS) {
        result = yan_relation_backlinks(shell->relations, name, relation_emit,
                                        &emit, &summary);
    } else {
        result = yan_relation_related(shell->relations, name, relation_emit,
                                      &emit, &summary);
    }
    if (result == YAN_NOTE_STOPPED) {
        /* The callback refused a byte: stop now, with no receipt and no retry. */
        return YAN_SHELL_FATAL;
    }
    if (result != YAN_NOTE_OK) {
        return finish_note_error(shell, result);
    }
    if (command == RELATION_CMD_LINKS) {
        if (!shell_puts(shell, "OK links shown=") ||
            !shell_put_decimal(shell, summary.shown) ||
            !shell_puts(shell, "\r\n")) {
            return YAN_SHELL_FATAL;
        }
        return YAN_SHELL_OK;
    }
    if (command == RELATION_CMD_BACKLINKS) {
        if (!shell_puts(shell, "OK backlinks shown=") ||
            !shell_put_decimal(shell, summary.shown) ||
            !shell_puts(shell, " target=") ||
            !shell_puts(shell, summary.target_exists ? "OK\r\n"
                                                     : "DANGLING\r\n")) {
            return YAN_SHELL_FATAL;
        }
        return YAN_SHELL_OK;
    }
    if (!shell_puts(shell, "OK related total=") ||
        !shell_put_decimal(shell, summary.total) ||
        !shell_puts(shell, " shown=") ||
        !shell_put_decimal(shell, summary.shown) ||
        !shell_puts(shell, "\r\n")) {
        return YAN_SHELL_FATAL;
    }
    return YAN_SHELL_OK;
}

typedef enum { BLOCK_LIST, BLOCK_SHOW, BLOCK_REFS, BLOCK_BACKLINKS } BlockCommand;
typedef struct {
    YanShell *shell;
    BlockCommand command;
    Utf8Stream stream;
    uint32_t line;
    bool line_open;
} BlockOutput;

/* Decimal doubling avoids requiring a freestanding 64-bit division helper on
 * RV32. The full occurrence count must not be narrowed to 32 bits for display. */
static bool shell_put_decimal64(YanShell *shell, uint64_t value)
{
    uint8_t digits[20] = {0};
    uint32_t hi = (uint32_t)(value >> 32), lo = (uint32_t)value;
    for (uint32_t bit = 64; bit > 0; --bit) {
        uint32_t index = bit - 1u;
        uint32_t carry = index >= 32u ? (hi >> (index - 32u)) & 1u : (lo >> index) & 1u;
        for (uint32_t i = 20; i > 0; --i) {
            uint32_t n = digits[i - 1u] * 2u + carry;
            digits[i - 1u] = (uint8_t)(n % 10u); carry = n / 10u;
        }
    }
    uint32_t start = 0;
    while (start < 19u && digits[start] == 0u) ++start;
    for (uint32_t i = start; i < 20u; ++i)
        if (!shell_putc(shell, (uint8_t)('0' + digits[i]))) return false;
    return true;
}
static bool block_output_row(void *opaque, const YanBlockRow *row)
{
    BlockOutput *out = opaque;
    YanShell *s = out->shell;
    if (out->command == BLOCK_SHOW) { out->line = row->block.first_line; return true; }
    if (out->command == BLOCK_LIST) {
        const YanBlockDecl *b = &row->block;
        return shell_puts(s, "BLOCK ") && shell_puts(s, row->source) && shell_puts(s, "#") &&
            shell_puts(s, b->id) && shell_puts(s, " marker=") && shell_put_decimal(s, b->marker_line) &&
            shell_puts(s, " first=") && shell_put_decimal(s, b->first_line) &&
            shell_puts(s, " last=") && shell_put_decimal(s, b->last_line) &&
            shell_puts(s, " bytes=") && shell_put_decimal(s, b->start) && shell_puts(s, ":") &&
            shell_put_decimal(s, b->end) && shell_puts(s, "\r\n");
    }
    const YanBlockReference *r = &row->reference;
    if (!shell_puts(s, out->command == BLOCK_REFS ? "REF source=" : "BACKLINK source=") ||
        !shell_puts(s, row->source) || !shell_puts(s, " block=")) return false;
    if (r->has_source_id) {
        if (!shell_puts(s, "#") || !shell_puts(s, r->source_id)) return false;
    } else if (!shell_puts(s, "NONE")) return false;
    if (!shell_puts(s, " line=") || !shell_put_decimal(s, r->line) ||
        !shell_puts(s, " bytes=") || !shell_put_decimal(s, r->start) || !shell_puts(s, ":") ||
        !shell_put_decimal(s, r->end) || !shell_puts(s, " target=") ||
        !shell_puts(s, r->target.name) || !shell_puts(s, "#") || !shell_puts(s, r->target.id)) return false;
    if (out->command == BLOCK_REFS &&
        (!shell_puts(s, " state=") || !shell_puts(s, yan_block_target_name(row->state)))) return false;
    return shell_puts(s, "\r\n");
}
static bool block_output_chunk(void *opaque, uint32_t offset, const uint8_t *bytes, uint32_t length)
{
    (void)offset;
    BlockOutput *out = opaque;
    for (uint32_t i = 0; i < length; ++i) {
        uint8_t byte = bytes[i];
        if (!out->line_open) {
            if (!shell_put_decimal(out->shell, out->line) || !shell_puts(out->shell, " ")) return false;
            out->line_open = true;
        }
        if (byte == '\n') {
            if (!cat_flush(out->shell, &out->stream) || !shell_puts(out->shell, "\r\n")) return false;
            out->line_open = false; ++out->line;
        } else if (byte != '\r' && !cat_feed(out->shell, &out->stream, byte)) return false;
    }
    return true;
}
static YanShellResult block_error(YanShell *shell, YanBlockResult result)
{
    if (result == YAN_BLOCK_STOPPED) return YAN_SHELL_FATAL;
    if (!shell_puts(shell, "ERROR ") || !shell_puts(shell, yan_block_result_name(result)) ||
        !shell_puts(shell, "\r\n")) return YAN_SHELL_FATAL;
    switch (result) {
    case YAN_BLOCK_NOT_MOUNTED: case YAN_BLOCK_FAULTED: case YAN_BLOCK_IO:
    case YAN_BLOCK_PROTOCOL: case YAN_BLOCK_CORRUPT: case YAN_BLOCK_UNSUPPORTED:
        return YAN_SHELL_FATAL;
    default: return YAN_SHELL_OK;
    }
}
static YanShellResult run_block(YanShell *shell, const uint8_t *line, uint32_t length, uint32_t position)
{
    uint32_t sub, sub_length, arg, arg_length;
    if (!meta_next_token(line, length, &position, &sub, &sub_length) ||
        !meta_next_token(line, length, &position, &arg, &arg_length) ||
        !meta_only_spaces_left(line, length, position)) return emit_status(shell, "ERROR USAGE\r\n");
    BlockCommand command;
    if (token_is(line, sub, sub_length, "list")) command = BLOCK_LIST;
    else if (token_is(line, sub, sub_length, "show")) command = BLOCK_SHOW;
    else if (token_is(line, sub, sub_length, "refs")) command = BLOCK_REFS;
    else if (token_is(line, sub, sub_length, "backlinks")) command = BLOCK_BACKLINKS;
    else return emit_status(shell, "ERROR USAGE\r\n");
    YanBlockAddress address;
    if (command == BLOCK_LIST || command == BLOCK_REFS) {
        if (!yan_note_name_valid(line + arg, arg_length)) return emit_status(shell, "ERROR USAGE\r\n");
        /* The legacy copy helper deliberately accepts a 32-byte invalid token
         * into 33 bytes. This grammar already validated the 31-byte maximum. */
        memcpy(shell->names[0], line + arg, arg_length);
        shell->names[0][arg_length] = '\0';
    } else if (yan_block_address_parse(line + arg, arg_length, &address) != YAN_BLOCK_OK) {
        return emit_status(shell, "ERROR USAGE\r\n");
    }
    BlockOutput out = {shell, command, {{0}, 0, 0}, 0, false};
    YanBlockSummary summary;
    YanBlockResult result;
    switch (command) {
    case BLOCK_LIST:
        result = yan_blocks_list(shell->blocks, shell->names[0], block_output_row, &out, &summary); break;
    case BLOCK_REFS:
        result = yan_blocks_refs(shell->blocks, shell->names[0], block_output_row, &out, &summary); break;
    case BLOCK_BACKLINKS:
        result = yan_blocks_backlinks(shell->blocks, &address, block_output_row, &out, &summary); break;
    case BLOCK_SHOW:
        result = yan_blocks_show(shell->blocks, &address, block_output_row, block_output_chunk, &out, &summary); break;
    default: return YAN_SHELL_FATAL;
    }
    if (result == YAN_BLOCK_STOPPED) return YAN_SHELL_FATAL;
    if (out.line_open && (!cat_flush(shell, &out.stream) || !shell_puts(shell, "\r\n"))) return YAN_SHELL_FATAL;
    if (result != YAN_BLOCK_OK) return block_error(shell, result);
    const char *verb = command == BLOCK_LIST ? "list" : command == BLOCK_SHOW ? "show" :
                       command == BLOCK_REFS ? "refs" : "backlinks";
    if (!shell_puts(shell, "OK block ") || !shell_puts(shell, verb) || !shell_puts(shell, " total=") ||
        !shell_put_decimal64(shell, summary.total) || !shell_puts(shell, " shown=") ||
        !shell_put_decimal(shell, summary.shown)) return YAN_SHELL_FATAL;
    if (command == BLOCK_BACKLINKS &&
        (!shell_puts(shell, " skipped=") || !shell_put_decimal(shell, summary.skipped) ||
         !shell_puts(shell, " target=") || !shell_puts(shell, yan_block_target_name(summary.target_state)))) return YAN_SHELL_FATAL;
    return shell_puts(shell, "\r\n") ? YAN_SHELL_OK : YAN_SHELL_FATAL;
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
        /* A bound shell advertises the metadata capabilities; an unbound one
         * keeps the original help text byte for byte. 0028 appends the relation
         * lines independently of the metadata binding. */
        const char *help = shell->metadata != NULL ? HELP_METADATA : HELP_TEXT;
        if (!shell_puts(shell, help)) {
            return YAN_SHELL_FATAL;
        }
        if (shell->relations != NULL && !shell_puts(shell, HELP_RELATIONS)) {
            return YAN_SHELL_FATAL;
        }
        if (shell->blocks != NULL && !shell_puts(shell,
            "block list NAME: list named blocks\r\n"
            "block show NAME#ID: show a named block\r\n"
            "block refs NAME: list exact body references\r\n"
            "block backlinks NAME#ID: find exact body references\r\n")) return YAN_SHELL_FATAL;
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
    if (token_is(line, command_start, command_length, "meta")) {
        /* 0027: the metadata command family exists only after a successful
         * binding. An unbound shell keeps the old UNKNOWN_COMMAND verdict. */
        if (shell->metadata == NULL) {
            return emit_status(shell, "ERROR UNKNOWN_COMMAND\r\n");
        }
        return run_meta(shell, line, length, position);
    }
    bool links = token_is(line, command_start, command_length, "links");
    bool backlinks = token_is(line, command_start, command_length, "backlinks");
    bool related = token_is(line, command_start, command_length, "related");
    if (links || backlinks || related) {
        /* 0028: the three relation commands exist only after a successful
         * binding; an unbound shell keeps the old UNKNOWN_COMMAND verdict. */
        if (shell->relations == NULL) {
            return emit_status(shell, "ERROR UNKNOWN_COMMAND\r\n");
        }
        RelationCommand relation_command =
            links ? RELATION_CMD_LINKS
                  : (backlinks ? RELATION_CMD_BACKLINKS : RELATION_CMD_RELATED);
        return run_relation(shell, line, length, position, relation_command);
    }
    if (token_is(line, command_start, command_length, "block")) {
        if (shell->blocks == NULL) return emit_status(shell, "ERROR UNKNOWN_COMMAND\r\n");
        return run_block(shell, line, length, position);
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
    /* 0027: the original init leaves the shell unbound so the old command set
     * and the old query behaviour are unchanged until a caller opts in. */
    shell->metadata = NULL;
    shell->relations = NULL;
    shell->blocks = NULL;
    shell->output = output;
    shell->initialized = true;
    shell->busy = false;
    return YAN_SHELL_OK;
}

YanShellResult yan_shell_bind_metadata(YanShell *shell, YanNoteContext *note)
{
    /* Extent rules run first, for both the unbind and the bind form, so a shell
     * pointer outside its own object is rejected before any field is read. No
     * byte is emitted and no filesystem call is made on any path. */
    if (!shell_span_valid((uintptr_t)(const void *)shell,
                          (uint64_t)sizeof(YanShell))) {
        return YAN_SHELL_INVALID;
    }
    if (!shell->initialized) {
        /* The original init is the only way to obtain the facade pointers this
         * binding validates against; without it neither form is legal. */
        return YAN_SHELL_INVALID;
    }
    if (shell->busy) {
        return YAN_SHELL_BUSY;
    }
    if (note == NULL) {
        /* The documented unbind: an initialized, idle shell drops its binding
         * and returns to the old command set. */
        shell->metadata = NULL;
        return YAN_SHELL_OK;
    }
    uintptr_t note_base = (uintptr_t)(const void *)note;
    if (!shell_span_valid(note_base, (uint64_t)sizeof(YanNoteContext))) {
        return YAN_SHELL_INVALID;
    }
    /* Both facades are known valid because the shell was initialized through
     * yan_shell_init, which requires an initialized literal and term facade. */
    const void *search_context = shell->search->backend.context;
    uint64_t search_context_size = (uint64_t)shell->search->backend.context_size;
    const void *search_source = shell->search->backend.source_context;
    uint64_t search_source_size = (uint64_t)shell->search->backend.source_size;
    const void *term_context = shell->terms->backend.context;
    uint64_t term_context_size = (uint64_t)shell->terms->backend.context_size;
    const void *term_source = shell->terms->backend.source_context;
    uint64_t term_source_size = (uint64_t)shell->terms->backend.source_size;
    if (!shell_span_valid((uintptr_t)search_context, search_context_size) ||
        !shell_span_valid((uintptr_t)search_source, search_source_size) ||
        !shell_span_valid((uintptr_t)term_context, term_context_size) ||
        !shell_span_valid((uintptr_t)term_source, term_source_size)) {
        return YAN_SHELL_INVALID;
    }
    if (ranges_overlap((uintptr_t)(const void *)shell, sizeof(YanShell),
                       note_base, sizeof(YanNoteContext)) ||
        ranges_overlap((uintptr_t)(const void *)shell->fs, sizeof(YanFs),
                       note_base, sizeof(YanNoteContext)) ||
        ranges_overlap((uintptr_t)(const void *)shell->search,
                       sizeof(YanSearch), note_base,
                       sizeof(YanNoteContext)) ||
        ranges_overlap((uintptr_t)(const void *)shell->terms,
                       sizeof(YanSearchTerms), note_base,
                       sizeof(YanNoteContext)) ||
        ranges_overlap((uintptr_t)search_context, search_context_size,
                       note_base, sizeof(YanNoteContext)) ||
        ranges_overlap((uintptr_t)search_source, search_source_size,
                       note_base, sizeof(YanNoteContext)) ||
        ranges_overlap((uintptr_t)term_context, term_context_size, note_base,
                       sizeof(YanNoteContext)) ||
        ranges_overlap((uintptr_t)term_source, term_source_size, note_base,
                       sizeof(YanNoteContext))) {
        return YAN_SHELL_INVALID;
    }
    /* 0028: a bound relation context is borrowed too, so the metadata context
     * may not overlap it in either direction. */
    if (shell->relations != NULL &&
        ranges_overlap((uintptr_t)(const void *)shell->relations,
                       sizeof(YanRelationContext), note_base,
                       sizeof(YanNoteContext))) {
        return YAN_SHELL_INVALID;
    }
    if (shell->blocks != NULL &&
        ranges_overlap((uintptr_t)shell->blocks, sizeof *shell->blocks,
                       note_base, sizeof *note)) return YAN_SHELL_INVALID;
    /* The borrowed source must be the shell's own filesystem, and the service
     * must be initialized and idle. The source state is deliberately left to
     * each command's own service call, which maps it with the note error
     * policy. */
    if (!note->initialized) {
        return YAN_SHELL_INVALID;
    }
    if (note->busy) {
        return YAN_SHELL_BUSY;
    }
    if (note->fs == NULL || note->fs != shell->fs) {
        return YAN_SHELL_INVALID;
    }
    shell->metadata = note;
    return YAN_SHELL_OK;
}

YanShellResult yan_shell_bind_relations(YanShell *shell,
                                        YanRelationContext *relations)
{
    /* The extent rules run first, for both forms, so a shell pointer outside
     * its own object is rejected before any field is read. No byte is emitted
     * and no filesystem call is made on any path. */
    if (!shell_span_valid((uintptr_t)(const void *)shell,
                          (uint64_t)sizeof(YanShell))) {
        return YAN_SHELL_INVALID;
    }
    if (!shell->initialized) {
        return YAN_SHELL_INVALID;
    }
    if (shell->busy) {
        return YAN_SHELL_BUSY;
    }
    if (relations == NULL) {
        /* The documented unbind: an initialized, idle shell drops its binding
         * and returns the relation commands to UNKNOWN_COMMAND. */
        shell->relations = NULL;
        return YAN_SHELL_OK;
    }
    uintptr_t relations_base = (uintptr_t)(const void *)relations;
    if (!shell_span_valid(relations_base,
                          (uint64_t)sizeof(YanRelationContext))) {
        return YAN_SHELL_INVALID;
    }
    /* Both facades are known valid because the shell was initialized through
     * yan_shell_init, which requires an initialized literal and term facade. */
    const void *search_context = shell->search->backend.context;
    uint64_t search_context_size = (uint64_t)shell->search->backend.context_size;
    const void *search_source = shell->search->backend.source_context;
    uint64_t search_source_size = (uint64_t)shell->search->backend.source_size;
    const void *term_context = shell->terms->backend.context;
    uint64_t term_context_size = (uint64_t)shell->terms->backend.context_size;
    const void *term_source = shell->terms->backend.source_context;
    uint64_t term_source_size = (uint64_t)shell->terms->backend.source_size;
    if (!shell_span_valid((uintptr_t)search_context, search_context_size) ||
        !shell_span_valid((uintptr_t)search_source, search_source_size) ||
        !shell_span_valid((uintptr_t)term_context, term_context_size) ||
        !shell_span_valid((uintptr_t)term_source, term_source_size)) {
        return YAN_SHELL_INVALID;
    }
    if (ranges_overlap((uintptr_t)(const void *)shell, sizeof(YanShell),
                       relations_base, sizeof(YanRelationContext)) ||
        ranges_overlap((uintptr_t)(const void *)shell->fs, sizeof(YanFs),
                       relations_base, sizeof(YanRelationContext)) ||
        ranges_overlap((uintptr_t)(const void *)shell->search,
                       sizeof(YanSearch), relations_base,
                       sizeof(YanRelationContext)) ||
        ranges_overlap((uintptr_t)(const void *)shell->terms,
                       sizeof(YanSearchTerms), relations_base,
                       sizeof(YanRelationContext)) ||
        ranges_overlap((uintptr_t)search_context, search_context_size,
                       relations_base, sizeof(YanRelationContext)) ||
        ranges_overlap((uintptr_t)search_source, search_source_size,
                       relations_base, sizeof(YanRelationContext)) ||
        ranges_overlap((uintptr_t)term_context, term_context_size,
                       relations_base, sizeof(YanRelationContext)) ||
        ranges_overlap((uintptr_t)term_source, term_source_size,
                       relations_base, sizeof(YanRelationContext)) ||
        (shell->metadata != NULL &&
         ranges_overlap((uintptr_t)(const void *)shell->metadata,
                        sizeof(YanNoteContext), relations_base,
                        sizeof(YanRelationContext)))) {
        return YAN_SHELL_INVALID;
    }
    if (shell->blocks != NULL &&
        ranges_overlap((uintptr_t)shell->blocks, sizeof *shell->blocks,
                       relations_base, sizeof *relations)) return YAN_SHELL_INVALID;
    /* The relation field is read only after every span and alias check. The
     * borrowed source must be the shell's own filesystem and the service must
     * be initialized and idle. */
    if (!relations->initialized) {
        return YAN_SHELL_INVALID;
    }
    if (relations->busy) {
        return YAN_SHELL_BUSY;
    }
    if (relations->fs == NULL || relations->fs != shell->fs) {
        return YAN_SHELL_INVALID;
    }
    shell->relations = relations;
    return YAN_SHELL_OK;
}

YanShellResult yan_shell_bind_blocks(YanShell *shell, YanBlockContext *blocks)
{
    if (!shell_span_valid((uintptr_t)shell, sizeof *shell)) return YAN_SHELL_INVALID;
    if (!shell->initialized) return YAN_SHELL_INVALID;
    if (shell->busy) return YAN_SHELL_BUSY;
    if (blocks == NULL) { shell->blocks = NULL; return YAN_SHELL_OK; }
    if (!shell_span_valid((uintptr_t)blocks, sizeof *blocks)) return YAN_SHELL_INVALID;
    const void *bases[] = {shell, shell->fs, shell->search, shell->terms,
        shell->search->backend.context, shell->search->backend.source_context,
        shell->terms->backend.context, shell->terms->backend.source_context,
        shell->metadata, shell->relations};
    const uint64_t sizes[] = {sizeof *shell, sizeof *shell->fs, sizeof *shell->search,
        sizeof *shell->terms, shell->search->backend.context_size, shell->search->backend.source_size,
        shell->terms->backend.context_size, shell->terms->backend.source_size,
        shell->metadata != NULL ? sizeof *shell->metadata : 0u,
        shell->relations != NULL ? sizeof *shell->relations : 0u};
    for (uint32_t i = 0; i < sizeof bases / sizeof bases[0]; ++i) {
        if (!shell_span_valid((uintptr_t)bases[i], sizes[i]) ||
            ranges_overlap((uintptr_t)bases[i], sizes[i], (uintptr_t)blocks, sizeof *blocks)) return YAN_SHELL_INVALID;
    }
    if (!blocks->initialized || blocks->fs != shell->fs) return YAN_SHELL_INVALID;
    if (blocks->busy) return YAN_SHELL_BUSY;
    shell->blocks = blocks;
    return YAN_SHELL_OK;
}

YanShellResult yan_shell_execute(YanShell *shell, const uint8_t *line,
                                 uint32_t length)
{
    if (shell == NULL ||
        (uint64_t)sizeof *shell > UINTPTR_MAX - (uintptr_t)shell ||
        !shell->initialized) {
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
        /* 0027: a bound metadata context is borrowed for the whole call too, so
         * the command line may not alias it. An unbound shell skips the test. */
        if (shell->metadata != NULL &&
            ranges_overlap((uintptr_t)(const void *)shell->metadata,
                           (uint64_t)sizeof(YanNoteContext), begin,
                           (uint64_t)length)) {
            return YAN_SHELL_INVALID;
        }
        /* 0028: a bound relation context is borrowed for the whole call too, so
         * the command line may not alias it. An unbound shell skips the test. */
        if (shell->relations != NULL &&
            ranges_overlap((uintptr_t)(const void *)shell->relations,
                           (uint64_t)sizeof(YanRelationContext), begin,
                           (uint64_t)length)) {
            return YAN_SHELL_INVALID;
        }
        if (shell->blocks != NULL &&
            ranges_overlap((uintptr_t)shell->blocks, sizeof *shell->blocks,
                           begin, length)) return YAN_SHELL_INVALID;
    }

    shell->busy = true;
    YanShellResult result = execute_busy(shell, line, length);
    shell->busy = false;
    return result;
}
