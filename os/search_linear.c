/* No-index linear search backend for 0025.
 *
 * The public contract is os/search_linear.h and docs/specs/0025-knowledge-
 * search.md. The backend lists a borrowed YanFs through its public API only and
 * scans each file in a fixed 4096-byte stream: a NUL preflight pass, then the
 * KMP match pass, then a bounded matched-range reader. It never reads the
 * filesystem metadata cache or an extent directly and it never allocates a
 * file-sized object.
 *
 * Only this translation unit (together with its header) knows about YanFs; the
 * facade and every consumer stay independent of the filesystem. The backend
 * borrows the caller's YanFs and never holds fs->busy across the whole query:
 * each yan_fs_* call owns its own busy guard. The whole-query stability of the
 * borrowed filesystem is an explicit caller precondition.
 *
 * The scan buffer and the matched-range reader buffer are separate, so a read
 * of the current match cannot clobber an unconsumed scan chunk. The KMP state
 * persists across scan chunks and is reset at every line start. */

#include "search_linear.h"

#include <stddef.h>

/* Freestanding Guest builds use os/memory.c, not the Host libc headers. */
extern void *memset(void *destination, int value, size_t length);

/* Same overlap rule as the facade: a range that would leave uintptr_t counts as
 * overlapping. No byte is read or written through either range. */
static bool linear_ranges_overlap(const void *first, size_t first_length,
                                  const void *second, size_t second_length)
{
    uintptr_t first_begin = (uintptr_t)first;
    uintptr_t second_begin = (uintptr_t)second;
    if (first_length == 0u || second_length == 0u) {
        return false;
    }
    if ((uint64_t)first_length > (uint64_t)(UINTPTR_MAX - first_begin) ||
        (uint64_t)second_length > (uint64_t)(UINTPTR_MAX - second_begin)) {
        return true;
    }
    uintptr_t first_end = first_begin + (uintptr_t)first_length;
    uintptr_t second_end = second_begin + (uintptr_t)second_length;
    return first_begin < second_end && second_begin < first_end;
}

/* Every YanFsResult has exactly one unified class. Values the search path
 * cannot produce are an inconsistent directory, which is a protocol error, not
 * a silent skip. */
static YanSearchResult linear_map(YanFsResult result)
{
    switch (result) {
    case YAN_FS_OK: return YAN_SEARCH_OK;
    case YAN_FS_END: return YAN_SEARCH_OK;
    case YAN_FS_INVALID: return YAN_SEARCH_INVALID;
    case YAN_FS_BUSY: return YAN_SEARCH_BUSY;
    case YAN_FS_NOT_MOUNTED: return YAN_SEARCH_NOT_MOUNTED;
    case YAN_FS_FAULTED: return YAN_SEARCH_FAULTED;
    case YAN_FS_EXISTS: return YAN_SEARCH_PROTOCOL;
    case YAN_FS_NOT_FOUND: return YAN_SEARCH_NOT_FOUND;
    case YAN_FS_DIRECTORY_FULL: return YAN_SEARCH_PROTOCOL;
    case YAN_FS_NOSPACE: return YAN_SEARCH_PROTOCOL;
    case YAN_FS_CORRUPT: return YAN_SEARCH_CORRUPT;
    case YAN_FS_UNSUPPORTED: return YAN_SEARCH_UNSUPPORTED;
    case YAN_FS_IO: return YAN_SEARCH_IO;
    case YAN_FS_PROTOCOL: return YAN_SEARCH_PROTOCOL;
    }
    return YAN_SEARCH_PROTOCOL;
}

/* Copies a directory name into the context's own 32-byte buffer. The list API
 * already NUL-terminates within 32 bytes; the loop keeps that guarantee even if
 * a future caller hands in a different array. */
static void linear_copy_name(char out[32], const char in[32])
{
    uint32_t i = 0u;
    while (i < YAN_FS_NAME_MAX && in[i] != '\0') {
        out[i] = in[i];
        ++i;
    }
    out[i] = '\0';
}

/* Standard KMP prefix table. Only prefix[0..length) is touched, and pattern[q]
 * is read only for q < length, so the guard against reading one past the
 * pattern is structural. */
static void linear_build_prefix(YanSearchLinear *self, const uint8_t *pattern,
                                uint32_t length)
{
    self->prefix[0] = 0u;
    uint32_t matched = 0u;
    for (uint32_t q = 1u; q < length; ++q) {
        while (matched > 0u && pattern[q] != pattern[matched]) {
            matched = self->prefix[matched - 1u];
        }
        if (pattern[q] == pattern[matched]) {
            ++matched;
        }
        self->prefix[q] = (uint16_t)matched;
    }
}

/* First pass: read the file in 4096-byte aligned chunks and stop at the first
 * NUL. The caller then skips the whole file with no match callback and no read
 * of the bytes after the NUL. */
static YanFsResult linear_preflight(YanSearchLinear *self, const char *name,
                                    bool *has_nul)
{
    uint32_t offset = 0u;
    *has_nul = false;
    for (;;) {
        uint32_t got = 0u;
        YanFsResult result =
            yan_fs_read(self->fs, name, offset, self->scan,
                        YAN_SEARCH_LINEAR_SCAN_SIZE, &got);
        if (result != YAN_FS_OK) {
            return result;
        }
        if (got == 0u) {
            return YAN_FS_OK;
        }
        for (uint32_t i = 0u; i < got; ++i) {
            if (self->scan[i] == 0u) {
                *has_nul = true;
                return YAN_FS_OK;
            }
        }
        offset += got;
    }
}

/* Feeds one content byte to the KMP machine. A hit only sets the per-line flag;
 * scanning continues so the line's full range is known when it ends. */
static void linear_feed_content(YanSearchLinear *self, uint8_t byte)
{
    ++self->line_content;
    uint32_t q = self->kmp;
    const uint8_t *pattern = self->pattern;
    while (q > 0u && byte != pattern[q]) {
        q = self->prefix[q - 1u];
    }
    if (byte == pattern[q]) {
        ++q;
    }
    if (q == self->pattern_length) {
        self->line_has_match = true;
        q = self->prefix[q - 1u];
    }
    self->kmp = q;
}

/* A line ended (LF, CRLF, or end of file). Emit one match if any hit was seen,
 * copying the file name and the content range into the context so the
 * matched-range reader can serve it while the match callback runs. */
static void linear_end_line(YanSearchLinear *self, YanSearchMatchFn match,
                            void *match_context, bool *stop)
{
    if (!self->line_has_match) {
        return;
    }
    YanSearchMatch value;
    value.name = self->match_name;
    value.line_number = self->line_number;
    value.content_length = self->line_content;
    self->match_start = self->line_start;
    self->match_length = self->line_content;
    self->serving = true;
    bool keep = match(match_context, &value);
    self->serving = false;
    self->line_has_match = false;
    if (!keep) {
        *stop = true;
    }
}

/* One byte of the match pass. The pending-CR latch decides whether a CR is a
 * CRLF terminator or ordinary content without ever looking ahead: the next byte
 * (or EOF) settles it. */
static void linear_feed_byte(YanSearchLinear *self, uint8_t byte, uint32_t pos,
                             YanSearchMatchFn match, void *match_context,
                             bool *stop)
{
    if (self->pending_cr) {
        self->pending_cr = false;
        if (byte == (uint8_t)'\n') {
            linear_end_line(self, match, match_context, stop);
            if (*stop) {
                return;
            }
            self->pending_new_line = true;
            return;
        }
        /* The CR was not followed by LF, so it is content. */
        linear_feed_content(self, (uint8_t)'\r');
    }
    if (self->pending_new_line) {
        ++self->line_number;
        self->line_start = pos;
        self->line_content = 0u;
        self->kmp = 0u;
        self->line_has_match = false;
        self->pending_new_line = false;
    }
    if (byte == (uint8_t)'\r') {
        self->pending_cr = true;
        return;
    }
    if (byte == (uint8_t)'\n') {
        linear_end_line(self, match, match_context, stop);
        if (*stop) {
            return;
        }
        self->pending_new_line = true;
        return;
    }
    linear_feed_content(self, byte);
}

/* End of file: a pending CR is content, and a final unterminated line is
 * reported only when it actually holds bytes. A file that ended with LF leaves
 * pending_new_line set and produces no phantom line. */
static void linear_finish_eof(YanSearchLinear *self, YanSearchMatchFn match,
                              void *match_context, bool *stop)
{
    if (self->pending_cr) {
        self->pending_cr = false;
        linear_feed_content(self, (uint8_t)'\r');
    }
    if (self->pending_new_line || self->line_content == 0u) {
        return;
    }
    linear_end_line(self, match, match_context, stop);
}

/* Second pass over one file: stream it in 4096-byte chunks, feed each byte, and
 * stop the whole query as soon as a callback asks to stop. */
static YanFsResult linear_scan_file(YanSearchLinear *self,
                                    const YanFsInfo *info,
                                    YanSearchMatchFn match, void *match_context,
                                    bool *stop)
{
    bool has_nul = false;
    YanFsResult result = linear_preflight(self, info->name, &has_nul);
    if (result != YAN_FS_OK) {
        return result;
    }
    if (has_nul) {
        return YAN_FS_OK;
    }
    linear_copy_name(self->match_name, info->name);
    self->line_number = 1u;
    self->line_start = 0u;
    self->line_content = 0u;
    self->kmp = 0u;
    self->line_has_match = false;
    self->pending_cr = false;
    self->pending_new_line = false;

    uint32_t offset = 0u;
    for (;;) {
        uint32_t got = 0u;
        result = yan_fs_read(self->fs, info->name, offset, self->scan,
                             YAN_SEARCH_LINEAR_SCAN_SIZE, &got);
        if (result != YAN_FS_OK) {
            return result;
        }
        if (got == 0u) {
            break;
        }
        for (uint32_t i = 0u; i < got; ++i) {
            linear_feed_byte(self, self->scan[i], offset + i, match,
                             match_context, stop);
            if (*stop) {
                return YAN_FS_OK;
            }
        }
        offset += got;
    }
    linear_finish_eof(self, match, match_context, stop);
    return YAN_FS_OK;
}

static YanSearchResult linear_run(YanSearchLinear *self, const uint8_t *pattern,
                                  uint32_t pattern_length,
                                  YanSearchMatchFn match, void *match_context)
{
    self->pattern = pattern;
    self->pattern_length = pattern_length;
    linear_build_prefix(self, pattern, pattern_length);

    uint32_t cursor = 0u;
    for (;;) {
        YanFsInfo info;
        YanFsResult result = yan_fs_list(self->fs, &cursor, &info);
        if (result == YAN_FS_END) {
            return YAN_SEARCH_OK;
        }
        if (result != YAN_FS_OK) {
            return linear_map(result);
        }
        bool stop = false;
        result = linear_scan_file(self, &info, match, match_context, &stop);
        if (result != YAN_FS_OK) {
            return linear_map(result);
        }
        if (stop) {
            return YAN_SEARCH_OK;
        }
    }
}

YanSearchResult yan_search_linear_init(YanSearchLinear *linear, YanFs *fs)
{
    if (linear == NULL || fs == NULL) {
        return YAN_SEARCH_INVALID;
    }
    /* Pure address arithmetic before the first memset, so a context placed
     * inside the borrowed filesystem is rejected without overwriting it. */
    if (linear_ranges_overlap(linear, sizeof *linear, fs, sizeof *fs)) {
        return YAN_SEARCH_INVALID;
    }
    if (linear->initialized && linear->busy) {
        return YAN_SEARCH_BUSY;
    }
    /* Clear the whole fixed context, then record the borrowed filesystem. The
     * object is caller-owned and long-lived, so this ~10 KiB store never lands
     * on the task stack. */
    memset(linear, 0, sizeof *linear);
    linear->fs = fs;
    linear->initialized = true;
    linear->busy = false;
    return YAN_SEARCH_OK;
}

YanSearchResult yan_search_linear_query(void *context, const uint8_t *pattern,
                                        uint32_t pattern_length,
                                        YanSearchMatchFn match,
                                        void *match_context)
{
    YanSearchLinear *self = (YanSearchLinear *)context;
    if (self == NULL || !self->initialized) {
        return YAN_SEARCH_INVALID;
    }
    if (self->busy) {
        return YAN_SEARCH_BUSY;
    }
    if (pattern == NULL || pattern_length == 0u ||
        pattern_length > YAN_SEARCH_PATTERN_MAX) {
        return YAN_SEARCH_INVALID;
    }
    self->busy = true;
    YanSearchResult result = linear_run(self, pattern, pattern_length, match,
                                        match_context);
    self->busy = false;
    self->serving = false;
    return result;
}

YanSearchResult yan_search_linear_read_match(void *context, uint32_t offset,
                                             uint32_t capacity,
                                             const uint8_t **bytes,
                                             uint32_t *length)
{
    YanSearchLinear *self = (YanSearchLinear *)context;
    if (self == NULL || !self->initialized) {
        return YAN_SEARCH_INVALID;
    }
    if (!self->busy || !self->serving) {
        return YAN_SEARCH_INVALID;
    }
    if (bytes == NULL || length == NULL) {
        return YAN_SEARCH_INVALID;
    }
    if (offset >= self->match_length) {
        *bytes = NULL;
        *length = 0u;
        return YAN_SEARCH_OK;
    }
    uint32_t remaining = self->match_length - offset;
    uint32_t want = capacity < remaining ? capacity : remaining;
    if (want > YAN_SEARCH_LINEAR_SCAN_SIZE) {
        want = YAN_SEARCH_LINEAR_SCAN_SIZE;
    }
    if (want == 0u) {
        *bytes = NULL;
        *length = 0u;
        return YAN_SEARCH_OK;
    }
    /* match_start + offset cannot wrap: the range lies inside the line, which
     * lies inside the file, so the sum is at most the file size. A stable
     * filesystem returns exactly the requested prefix, otherwise the source
     * changed under us and that is a protocol error. */
    uint32_t got = 0u;
    YanFsResult result =
        yan_fs_read(self->fs, self->match_name, self->match_start + offset,
                    self->match_reader, want, &got);
    if (result != YAN_FS_OK) {
        return linear_map(result);
    }
    if (got != want) {
        return YAN_SEARCH_PROTOCOL;
    }
    *bytes = self->match_reader;
    *length = got;
    return YAN_SEARCH_OK;
}

YanSearchBackend yan_search_linear_backend(YanSearchLinear *linear)
{
    YanSearchBackend backend;
    backend.context = linear;
    /* The whole fixed context is off limits to the pattern and to the reader
     * output holders: a write there could clobber the scan or reader buffer. */
    backend.context_size = sizeof(YanSearchLinear);
    /* The borrowed filesystem is off limits for the same reason; before the
     * context has been initialized against a filesystem there is no source span
     * to declare. */
    backend.source_context = linear != NULL ? linear->fs : NULL;
    backend.source_size =
        (linear != NULL && linear->fs != NULL) ? sizeof(YanFs) : 0u;
    backend.query = yan_search_linear_query;
    backend.read_match = yan_search_linear_read_match;
    return backend;
}
