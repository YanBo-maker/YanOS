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

/* Help text, ending before the final OK line. It lists the eight commands, the
 * 1023-byte line limit, exit and the cat display rules. Tests pin these bytes. */
static const char HELP_TEXT[] =
    "help: show commands and limits\r\n"
    "ls: list files as <size> <name>\r\n"
    "stat NAME: show a file's size\r\n"
    "cat NAME: print a file\r\n"
    "create NAME [TEXT]: create a new file\r\n"
    "write NAME [TEXT]: replace an existing file\r\n"
    "rm NAME: remove a file\r\n"
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
    YanFsResult health = shell_fs_health(shell);
    if (health != YAN_FS_OK) {
        return finish_fs_error(shell, health);
    }
    return run_line(shell, line, length);
}

YanShellResult yan_shell_init(YanShell *shell, YanFs *fs, YanShellOutput output)
{
    if (shell == NULL) {
        return YAN_SHELL_INVALID;
    }
    if (fs == NULL) {
        /* Nothing to compare addresses against; the original BUSY priority for
         * an active instance is preserved. */
        if (shell->initialized && shell->busy) {
            return YAN_SHELL_BUSY;
        }
        return YAN_SHELL_INVALID;
    }
    /* Address arithmetic only. An aliased pair is rejected before any bool
     * field of either context is read: a shell placed inside the filesystem
     * would otherwise read a byte that is not a valid _Bool. The check runs
     * before the first store, so neither context is written. */
    if (ranges_overlap((uintptr_t)(const void *)shell, (uint64_t)sizeof(YanShell),
                       (uintptr_t)(const void *)fs, (uint64_t)sizeof(YanFs))) {
        return YAN_SHELL_INVALID;
    }
    if (shell->initialized && shell->busy) {
        return YAN_SHELL_BUSY;
    }
    if (output.putc == NULL) {
        return YAN_SHELL_INVALID;
    }
    shell->fs = fs;
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
    }

    shell->busy = true;
    YanShellResult result = execute_busy(shell, line, length);
    shell->busy = false;
    return result;
}
