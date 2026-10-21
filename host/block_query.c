#include "block_query.h"

#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* 0033 Task B: validation, cancellation-wrapped A/B identity observation and
 * bounded service adapters. The Guest block service owns grammar and media
 * reads; this file owns only the Host-facing ordering, paging and publication.
 * See host/block_query.h for the public lifecycle and failure contract. */

static bool span(const void *p, size_t n)
{
    return (p != NULL || n == 0) && n <= UINTPTR_MAX - (uintptr_t)p;
}
static bool overlaps(const void *a, size_t an, const void *b, size_t bn)
{
    return an && bn && (uintptr_t)a < (uintptr_t)b + bn &&
           (uintptr_t)b < (uintptr_t)a + an;
}
static bool cancelled(const volatile sig_atomic_t *flag)
{
    return flag != NULL && *flag != 0;
}
static YanBlockQueryResult map_fs(YanFsResult result)
{
    switch (result) {
    case YAN_FS_OK: return YAN_BLOCK_QUERY_OK;
    case YAN_FS_INVALID: return YAN_BLOCK_QUERY_INVALID;
    case YAN_FS_NOT_MOUNTED: return YAN_BLOCK_QUERY_NOT_MOUNTED;
    case YAN_FS_FAULTED: return YAN_BLOCK_QUERY_FAULTED;
    case YAN_FS_BUSY: return YAN_BLOCK_QUERY_BUSY;
    case YAN_FS_NOT_FOUND: return YAN_BLOCK_QUERY_NOT_FOUND;
    case YAN_FS_IO: return YAN_BLOCK_QUERY_IO;
    case YAN_FS_PROTOCOL: return YAN_BLOCK_QUERY_PROTOCOL;
    case YAN_FS_CORRUPT: return YAN_BLOCK_QUERY_CORRUPT;
    case YAN_FS_UNSUPPORTED: return YAN_BLOCK_QUERY_UNSUPPORTED;
    /* Iteration END and write-side outcomes cannot describe a healthy read. */
    case YAN_FS_END: case YAN_FS_EXISTS: case YAN_FS_DIRECTORY_FULL:
    case YAN_FS_NOSPACE: return YAN_BLOCK_QUERY_PROTOCOL;
    }
    return YAN_BLOCK_QUERY_PROTOCOL;
}
static YanBlockQueryResult map_semantic(YanSemanticResult result,
                                        const volatile sig_atomic_t *cancel)
{
    switch (result) {
    case YAN_SEMANTIC_OK: return YAN_BLOCK_QUERY_OK;
    case YAN_SEMANTIC_INVALID: return YAN_BLOCK_QUERY_INVALID;
    case YAN_SEMANTIC_BUSY: return YAN_BLOCK_QUERY_BUSY;
    case YAN_SEMANTIC_STOPPED:
        return cancelled(cancel) ? YAN_BLOCK_QUERY_CANCELLED
                                 : YAN_BLOCK_QUERY_STOPPED;
    case YAN_SEMANTIC_UNSUPPORTED: return YAN_BLOCK_QUERY_UNSUPPORTED;
    case YAN_SEMANTIC_METADATA: return YAN_BLOCK_QUERY_METADATA;
    case YAN_SEMANTIC_TOO_LARGE: return YAN_BLOCK_QUERY_LIMIT;
    case YAN_SEMANTIC_LIMIT: return YAN_BLOCK_QUERY_LIMIT;
    case YAN_SEMANTIC_CORRUPT: return YAN_BLOCK_QUERY_CORRUPT;
    case YAN_SEMANTIC_STALE: return YAN_BLOCK_QUERY_STALE;
    case YAN_SEMANTIC_MISSING: return YAN_BLOCK_QUERY_NOT_FOUND;
    case YAN_SEMANTIC_DEPENDENCY: return YAN_BLOCK_QUERY_IO;
    case YAN_SEMANTIC_PROVIDER: return YAN_BLOCK_QUERY_PROTOCOL;
    case YAN_SEMANTIC_TIMEOUT: return YAN_BLOCK_QUERY_PROTOCOL;
    case YAN_SEMANTIC_IO: return YAN_BLOCK_QUERY_IO;
    case YAN_SEMANTIC_CANCELLED: return YAN_BLOCK_QUERY_CANCELLED;
    case YAN_SEMANTIC_NOSPACE: return YAN_BLOCK_QUERY_LIMIT;
    case YAN_SEMANTIC_NOT_FOUND: return YAN_BLOCK_QUERY_NOT_FOUND;
    }
    return YAN_BLOCK_QUERY_PROTOCOL;
}
static YanBlockQueryResult map_block(YanBlockResult result,
                                     const volatile sig_atomic_t *cancel)
{
    switch (result) {
    case YAN_BLOCK_OK: return YAN_BLOCK_QUERY_OK;
    case YAN_BLOCK_INVALID: return YAN_BLOCK_QUERY_INVALID;
    case YAN_BLOCK_BUSY: return YAN_BLOCK_QUERY_BUSY;
    case YAN_BLOCK_STOPPED:
        return cancelled(cancel) ? YAN_BLOCK_QUERY_CANCELLED
                                 : YAN_BLOCK_QUERY_STOPPED;
    case YAN_BLOCK_METADATA: return YAN_BLOCK_QUERY_METADATA;
    case YAN_BLOCK_FORMAT: return YAN_BLOCK_QUERY_BLOCK_FORMAT;
    case YAN_BLOCK_AMBIGUOUS: return YAN_BLOCK_QUERY_AMBIGUOUS;
    case YAN_BLOCK_LIMIT: return YAN_BLOCK_QUERY_LIMIT;
    case YAN_BLOCK_INVALID_TEXT: return YAN_BLOCK_QUERY_INVALID_TEXT;
    case YAN_BLOCK_NOT_MOUNTED: return YAN_BLOCK_QUERY_NOT_MOUNTED;
    case YAN_BLOCK_FAULTED: return YAN_BLOCK_QUERY_FAULTED;
    case YAN_BLOCK_NOT_FOUND: return YAN_BLOCK_QUERY_NOT_FOUND;
    case YAN_BLOCK_IO: return YAN_BLOCK_QUERY_IO;
    case YAN_BLOCK_PROTOCOL: return YAN_BLOCK_QUERY_PROTOCOL;
    case YAN_BLOCK_CORRUPT: return YAN_BLOCK_QUERY_CORRUPT;
    case YAN_BLOCK_UNSUPPORTED: return YAN_BLOCK_QUERY_UNSUPPORTED;
    }
    return YAN_BLOCK_QUERY_PROTOCOL;
}
static bool query_usable(const YanBlockQuery *query)
{
    return query != NULL && span(query, sizeof *query) && query->initialized &&
           query->adapter != NULL && span(query->adapter, sizeof *query->adapter);
}
static bool output_usable(const YanBlockQuery *query, const void *out, size_t size)
{
    return out != NULL && span(out, size) &&
           !overlaps(out, size, query, sizeof *query) &&
           !overlaps(out, size, query->adapter, sizeof *query->adapter);
}
/* Copies a bounded NUL-terminated logical name/ID so the rest of the call does
 * not keep a caller pointer that could alias context memory cleared by the
 * post-observation remount. */
static bool field_copy(const char *text, char out[32], uint32_t *length)
{
    if (text == NULL) return false;
    for (uint32_t i = 0; i < 32; ++i) {
        if ((uintptr_t)text > UINTPTR_MAX - i) return false;
        if (text[i] == '\0') {
            *length = i;
            memset(out, 0, 32);
            memcpy(out, text, i);
            return true;
        }
    }
    return false;
}
static bool name_copy(const char *name, char out[32])
{
    uint32_t length = 0;
    return field_copy(name, out, &length) &&
           yan_note_name_valid((const uint8_t *)out, length);
}
static bool id_copy(const char *id, char out[32])
{
    uint32_t length = 0;
    return field_copy(id, out, &length) &&
           yan_block_id_valid((const uint8_t *)out, length);
}
static YanBlockAddress address_copy(const char name[32], const char id[32])
{
    YanBlockAddress address = {{0}, {0}};
    memcpy(address.name, name, sizeof address.name);
    memcpy(address.id, id, sizeof address.id);
    return address;
}
/* A source wrapper checks the caller flag between reads so pre/post digests
 * are interruptible without changing the A adapter. */
typedef struct {
    const YanSemanticSource *base;
    const volatile sig_atomic_t *cancel;
} CancelSource;
static YanSemanticResult cancel_next(void *opaque, uint32_t *cursor,
                                     YanSemanticFile *out)
{
    CancelSource *source = opaque;
    if (cancelled(source->cancel)) return YAN_SEMANTIC_CANCELLED;
    return source->base->next(source->base->context, cursor, out);
}
static YanSemanticResult cancel_read(void *opaque, const char *name,
                                     uint32_t offset, uint8_t *out, uint32_t length)
{
    CancelSource *source = opaque;
    if (cancelled(source->cancel)) return YAN_SEMANTIC_CANCELLED;
    return source->base->read(source->base->context, name, offset, out, length);
}
/* Observation: dropping the cache and mounting again forces a real read of the
 * directory block; the digest then covers the directory and every raw byte. */
static YanBlockQueryResult observe(YanBlockQuery *query, uint8_t digest[32])
{
    if (cancelled(query->cancel)) return YAN_BLOCK_QUERY_CANCELLED;
    YanFsResult result = yan_fs_unmount(&query->adapter->fs);
    if (result != YAN_FS_OK) return map_fs(result);
    if (cancelled(query->cancel)) return YAN_BLOCK_QUERY_CANCELLED;
    result = yan_fs_mount(&query->adapter->fs);
    if (result != YAN_FS_OK) return map_fs(result);
    if (cancelled(query->cancel)) return YAN_BLOCK_QUERY_CANCELLED;
    CancelSource context = {&query->source, query->cancel};
    YanSemanticSource source = {&context, sizeof context, cancel_next, cancel_read};
    YanSemanticResult semantic = yan_semantic_source_digest(&source, digest);
    if (semantic != YAN_SEMANTIC_OK)
        return map_semantic(semantic, query->cancel);
    if (cancelled(query->cancel)) return YAN_BLOCK_QUERY_CANCELLED;
    return YAN_BLOCK_QUERY_OK;
}
/* Shared validation before any source I/O. expected is optional; when present
 * its 32 bytes are copied so a later remount or output write cannot move it. */
static YanBlockQueryResult validate_request(const YanBlockQuery *query,
    const char *name, const void *out, size_t out_size,
    const uint8_t expected[32], char name_value[32], uint8_t expect[32],
    bool *has_expected)
{
    if (!query_usable(query)) return YAN_BLOCK_QUERY_INVALID;
    if (!output_usable(query, out, out_size)) return YAN_BLOCK_QUERY_INVALID;
    if (!name_copy(name, name_value)) return YAN_BLOCK_QUERY_INVALID;
    *has_expected = expected != NULL;
    if (*has_expected) {
        if (!span(expected, 32)) return YAN_BLOCK_QUERY_INVALID;
        memcpy(expect, expected, 32);
    }
    return YAN_BLOCK_QUERY_OK;
}
static bool expected_differs(bool has_expected, const uint8_t expected[32],
                             const uint8_t observed[32])
{
    return has_expected && memcmp(expected, observed, 32) != 0;
}
static YanBlockQueryResult observe_pair(YanBlockQuery *query,
    const uint8_t before[32], uint8_t after[32])
{
    if (cancelled(query->cancel)) return YAN_BLOCK_QUERY_CANCELLED;
    YanBlockQueryResult result = observe(query, after);
    if (result != YAN_BLOCK_QUERY_OK) return result;
    if (memcmp(before, after, 32) != 0) return YAN_BLOCK_QUERY_STALE;
    if (cancelled(query->cancel)) return YAN_BLOCK_QUERY_CANCELLED;
    return YAN_BLOCK_QUERY_OK;
}
static bool cancel_sink(void *opaque, const YanBlockRow *row)
{
    (void)row;
    return !cancelled(((YanBlockQuery *)opaque)->cancel);
}
/* The Guest block service performs several full preflight passes before it
 * reaches our row/chunk callbacks. Install a cancellation-aware view of the
 * borrowed block IO only for the service call, then restore it on the single
 * return path. A cancelled read never starts another physical read; any base
 * error is returned unchanged. */
typedef struct {
    YanFsBlockIo base;
    const volatile sig_atomic_t *cancel;
} ServiceIo;
static YanFsIoResult service_capacity(void *opaque, uint64_t *blocks)
{
    ServiceIo *io = opaque;
    return io->base.capacity(io->base.context, blocks);
}
static YanFsIoResult service_read_block(void *opaque, uint32_t lba, uint8_t *out)
{
    ServiceIo *io = opaque;
    if (cancelled(io->cancel)) return YAN_FS_IO_ERROR;
    YanFsIoResult result = io->base.read_block(io->base.context, lba, out);
    if (result != YAN_FS_IO_OK) return result;
    if (cancelled(io->cancel)) return YAN_FS_IO_ERROR;
    return YAN_FS_IO_OK;
}
static YanFsIoResult service_write_block(void *opaque, uint32_t lba,
                                          const uint8_t *in)
{
    ServiceIo *io = opaque;
    return io->base.write_block(io->base.context, lba, in);
}
static void service_io_install(YanBlockQuery *query, ServiceIo *context)
{
    context->base = query->adapter->fs.io;
    context->cancel = query->cancel;
    YanFsBlockIo io = {context, service_capacity, service_read_block,
                       service_write_block};
    query->adapter->fs.io = io;
}
static void service_io_restore(YanBlockQuery *query, const ServiceIo *context)
{
    query->adapter->fs.io = context->base;
}
static YanBlockResult service_list(YanBlockQuery *query, const char *name,
    YanBlockRowFn sink, void *opaque, YanBlockSummary *summary)
{
    ServiceIo context;
    service_io_install(query, &context);
    YanBlockResult result = yan_blocks_list(&query->blocks, name, sink,
                                            opaque, summary);
    service_io_restore(query, &context);
    return result;
}
static YanBlockResult service_refs(YanBlockQuery *query, const char *name,
    YanBlockRowFn sink, void *opaque, YanBlockSummary *summary)
{
    ServiceIo context;
    service_io_install(query, &context);
    YanBlockResult result = yan_blocks_refs(&query->blocks, name, sink,
                                            opaque, summary);
    service_io_restore(query, &context);
    return result;
}
static YanBlockResult service_backlinks(YanBlockQuery *query,
    const YanBlockAddress *address, YanBlockRowFn sink, void *opaque,
    YanBlockSummary *summary)
{
    ServiceIo context;
    service_io_install(query, &context);
    YanBlockResult result = yan_blocks_backlinks(&query->blocks, address,
        sink, opaque, summary);
    service_io_restore(query, &context);
    return result;
}
static YanBlockResult service_show(YanBlockQuery *query,
    const YanBlockAddress *address, YanBlockRowFn row, YanBlockChunkFn chunk,
    void *opaque, YanBlockSummary *summary)
{
    ServiceIo context;
    service_io_install(query, &context);
    YanBlockResult result = yan_blocks_show(&query->blocks, address, row,
                                            chunk, opaque, summary);
    service_io_restore(query, &context);
    return result;
}

void yan_block_query_destroy(YanBlockQuery *query)
{
    if (query == NULL) return;
    free(query->block_sort);
    query->block_sort = NULL;
    free(query->block_names);
    query->block_names = NULL;
    query->initialized = false;
    query->blocks.initialized = false;
}

YanBlockQueryResult yan_block_query_init(YanBlockQuery *query,
    YanSemanticPosix *adapter, const volatile sig_atomic_t *cancel)
{
    if (query == NULL || adapter == NULL || !span(query, sizeof *query) ||
        !span(adapter, sizeof *adapter) ||
        overlaps(query, sizeof *query, adapter, sizeof *adapter))
        return YAN_BLOCK_QUERY_INVALID;
    if (query->initialized && query->blocks.busy) return YAN_BLOCK_QUERY_BUSY;
    /* Free a previous table before the struct is cleared; the pointer is owned
     * by this object and init is the only other reset path. */
    free(query->block_sort);
    free(query->block_names);
    memset(query, 0, sizeof *query);
    query->adapter = adapter;
    query->cancel = cancel;
    yan_semantic_posix_bind_source(adapter, &query->source);
    if (adapter->fs.state != YAN_FS_MOUNTED) {
        return YAN_BLOCK_QUERY_NOT_MOUNTED;
    }
    uint32_t capacity = adapter->fs.entry_capacity;
    if (capacity == 0u) return YAN_BLOCK_QUERY_INVALID;
    query->block_names = calloc(capacity, 32u);
    query->block_sort = calloc(capacity, sizeof *query->block_sort);
    if (query->block_names == NULL || query->block_sort == NULL) {
        return YAN_BLOCK_QUERY_IO;
    }
    YanBlockResult result = yan_blocks_init(&query->blocks, &adapter->fs,
                                            query->block_names,
                                            query->block_sort);
    if (result != YAN_BLOCK_OK) {
        free(query->block_sort);
        query->block_sort = NULL;
        free(query->block_names);
        query->block_names = NULL;
        return map_block(result, cancel);
    }
    query->initialized = true;
    return YAN_BLOCK_QUERY_OK;
}
YanBlockQueryResult yan_block_query_list(YanBlockQuery *query, const char *name,
    const uint8_t expected[32], YanBlockQueryList *out)
{
    char name_value[32];
    uint8_t expect[32], before[32], after[32];
    bool has_expected = false;
    YanBlockQueryResult result = validate_request(query, name, out,
        sizeof *out, expected, name_value, expect, &has_expected);
    if (result != YAN_BLOCK_QUERY_OK) return result;
    if (cancelled(query->cancel)) return YAN_BLOCK_QUERY_CANCELLED;
    result = observe(query, before);
    if (result != YAN_BLOCK_QUERY_OK) return result;
    if (expected_differs(has_expected, expect, before)) return YAN_BLOCK_QUERY_STALE;
    if (cancelled(query->cancel)) return YAN_BLOCK_QUERY_CANCELLED;
    YanBlockSummary summary;
    YanBlockResult block = service_list(query, name_value, cancel_sink,
                                        query, &summary);
    if (cancelled(query->cancel)) return YAN_BLOCK_QUERY_CANCELLED;
    if (block != YAN_BLOCK_OK) return map_block(block, query->cancel);
    result = observe_pair(query, before, after);
    if (result != YAN_BLOCK_QUERY_OK) return result;
    if (summary.shown > YAN_BLOCK_MAX_COUNT) return YAN_BLOCK_QUERY_PROTOCOL;
    memcpy(query->source_sha256, after, 32);
    memset(out, 0, sizeof *out);
    out->total = summary.total;
    out->shown = summary.shown;
    for (uint32_t i = 0; i < summary.shown; ++i)
        out->blocks[i] = query->blocks.rows[i].block;
    return YAN_BLOCK_QUERY_OK;
}
YanBlockQueryResult yan_block_query_refs(YanBlockQuery *query, const char *name,
    const uint8_t expected[32], YanBlockQueryRefs *out)
{
    char name_value[32];
    uint8_t expect[32], before[32], after[32];
    bool has_expected = false;
    YanBlockQueryResult result = validate_request(query, name, out,
        sizeof *out, expected, name_value, expect, &has_expected);
    if (result != YAN_BLOCK_QUERY_OK) return result;
    if (cancelled(query->cancel)) return YAN_BLOCK_QUERY_CANCELLED;
    result = observe(query, before);
    if (result != YAN_BLOCK_QUERY_OK) return result;
    if (expected_differs(has_expected, expect, before)) return YAN_BLOCK_QUERY_STALE;
    if (cancelled(query->cancel)) return YAN_BLOCK_QUERY_CANCELLED;
    YanBlockSummary summary;
    YanBlockResult block = service_refs(query, name_value, cancel_sink,
                                        query, &summary);
    if (cancelled(query->cancel)) return YAN_BLOCK_QUERY_CANCELLED;
    if (block != YAN_BLOCK_OK) return map_block(block, query->cancel);
    result = observe_pair(query, before, after);
    if (result != YAN_BLOCK_QUERY_OK) return result;
    if (summary.shown > YAN_BLOCK_REFS_SHOWN) return YAN_BLOCK_QUERY_PROTOCOL;
    memcpy(query->source_sha256, after, 32);
    memset(out, 0, sizeof *out);
    out->total = summary.total;
    out->shown = summary.shown;
    for (uint32_t i = 0; i < summary.shown; ++i)
        out->rows[i] = query->blocks.rows[i];
    return YAN_BLOCK_QUERY_OK;
}
YanBlockQueryResult yan_block_query_backlinks(YanBlockQuery *query,
    const char *name, const char *id, const uint8_t expected[32],
    YanBlockQueryBacklinks *out)
{
    char name_value[32], id_value[32];
    uint8_t expect[32], before[32], after[32];
    bool has_expected = false;
    YanBlockQueryResult result = validate_request(query, name, out,
        sizeof *out, expected, name_value, expect, &has_expected);
    if (result != YAN_BLOCK_QUERY_OK) return result;
    if (!id_copy(id, id_value)) return YAN_BLOCK_QUERY_INVALID;
    YanBlockAddress address = address_copy(name_value, id_value);
    if (cancelled(query->cancel)) return YAN_BLOCK_QUERY_CANCELLED;
    result = observe(query, before);
    if (result != YAN_BLOCK_QUERY_OK) return result;
    if (expected_differs(has_expected, expect, before)) return YAN_BLOCK_QUERY_STALE;
    if (cancelled(query->cancel)) return YAN_BLOCK_QUERY_CANCELLED;
    YanBlockSummary summary;
    YanBlockResult block = service_backlinks(query, &address, cancel_sink,
                                             query, &summary);
    if (cancelled(query->cancel)) return YAN_BLOCK_QUERY_CANCELLED;
    if (block != YAN_BLOCK_OK) return map_block(block, query->cancel);
    result = observe_pair(query, before, after);
    if (result != YAN_BLOCK_QUERY_OK) return result;
    if (summary.shown > YAN_BLOCK_REFS_SHOWN) return YAN_BLOCK_QUERY_PROTOCOL;
    memcpy(query->source_sha256, after, 32);
    memset(out, 0, sizeof *out);
    out->total = summary.total;
    out->shown = summary.shown;
    out->skipped = summary.skipped;
    out->target_state = summary.target_state;
    for (uint32_t i = 0; i < summary.shown; ++i)
        out->rows[i] = query->blocks.rows[i];
    return YAN_BLOCK_QUERY_OK;
}

typedef struct {
    YanBlockQuery *query;
    YanBlockDecl block;
    uint32_t offset;
    uint32_t p_rel;
    uint32_t block_length;
    uint32_t lf_before;
    uint32_t collected;
    uint8_t previous;
    bool have_previous;
    YanBlockQueryResult error;
    /* Filled by prepare_read after the show stream was fully consumed. */
    uint32_t page_start, page_end, page_length;
    uint32_t page_first_line, page_last_line;
    bool complete;
    uint32_t next_offset;
} ReadState;

static bool read_row(void *opaque, const YanBlockRow *row)
{
    ReadState *state = opaque;
    if (cancelled(state->query->cancel)) return false;
    state->block = row->block;
    state->block_length = row->block.end - row->block.start;
    if (state->offset > state->block_length) {
        state->error = YAN_BLOCK_QUERY_INVALID;
        return false;
    }
    state->p_rel = state->offset;
    state->lf_before = 0;
    state->collected = 0;
    state->previous = 0;
    state->have_previous = false;
    return true;
}
static bool read_chunk(void *opaque, uint32_t offset,
                       const uint8_t *bytes, uint32_t length)
{
    ReadState *state = opaque;
    if (cancelled(state->query->cancel)) return false;
    uint64_t end = (uint64_t)offset + length;
    /* Count LFs before P and remember the byte immediately before P. */
    if (offset < state->p_rel) {
        uint64_t stop = end < state->p_rel ? end : state->p_rel;
        for (uint64_t i = offset; i < stop; ++i)
            if (bytes[i - offset] == '\n') ++state->lf_before;
        if (stop == state->p_rel) {
            state->previous = bytes[stop - 1u - offset];
            state->have_previous = true;
        }
    }
    /* Copy only the bounded page/lookahead window; keep consuming the stream. */
    uint64_t wanted_end = (uint64_t)state->p_rel + sizeof state->query->page;
    if (end > state->p_rel && (uint64_t)offset < wanted_end) {
        uint64_t from = offset > state->p_rel ? offset : state->p_rel;
        uint64_t to = end < wanted_end ? end : wanted_end;
        uint32_t count = (uint32_t)(to - from);
        memcpy(state->query->page + (uint32_t)(from - state->p_rel),
               bytes + (uint32_t)(from - offset), count);
        uint32_t used = (uint32_t)(from - state->p_rel) + count;
        if (used > state->collected) state->collected = used;
    }
    return true;
}
static YanBlockQueryResult prepare_read(ReadState *state)
{
    YanBlockQuery *query = state->query;
    uint32_t start = state->block.start, end = state->block.end;
    uint32_t length = end - start;
    uint32_t offset = state->offset;
    if (offset > length) return YAN_BLOCK_QUERY_INVALID;
    uint32_t page_start = start + offset;
    if (offset == length) {
        state->page_start = page_start; state->page_end = page_start;
        state->page_length = 0;
        state->page_first_line = 0; state->page_last_line = 0;
        state->complete = true; state->next_offset = 0;
        return YAN_BLOCK_QUERY_OK;
    }
    uint32_t remaining = end - page_start;
    if (state->block_length != length || state->p_rel != offset ||
        state->collected == 0 ||
        state->collected < (remaining < 65537u ? remaining : 65537u))
        return YAN_BLOCK_QUERY_PROTOCOL;
    /* Reject a start inside a UTF-8 scalar or between CR and LF. */
    if ((query->page[0] & 0xC0u) == 0x80u) return YAN_BLOCK_QUERY_INVALID;
    if (offset > 0 && state->have_previous && state->previous == '\r' &&
        query->page[0] == '\n') return YAN_BLOCK_QUERY_INVALID;
    uint32_t page_length = remaining > 65536u ? 65536u : remaining;
    if (page_length < remaining) {
        if (state->collected <= page_length) return YAN_BLOCK_QUERY_PROTOCOL;
        while (page_length > 0 && (query->page[page_length] & 0xC0u) == 0x80u)
            --page_length;
        if (page_length > 0 && query->page[page_length - 1u] == '\r' &&
            query->page[page_length] == '\n') --page_length;
    }
    if (page_length == 0) return YAN_BLOCK_QUERY_INVALID;
    uint32_t page_end = page_start + page_length;
    uint32_t first_line = state->block.first_line + state->lf_before;
    uint32_t last_line = first_line;
    for (uint32_t i = 0; i + 1u < page_length; ++i)
        if (query->page[i] == '\n') ++last_line;
    state->page_start = page_start; state->page_end = page_end;
    state->page_length = page_length;
    state->page_first_line = first_line; state->page_last_line = last_line;
    state->complete = page_end == end;
    state->next_offset = state->complete ? 0u : page_end - start;
    return YAN_BLOCK_QUERY_OK;
}
static void publish_read(const ReadState *state, YanBlockQueryRead *out)
{
    memset(out, 0, sizeof *out);
    out->block_start = state->block.start; out->block_end = state->block.end;
    out->block_first_line = state->block.first_line;
    out->block_last_line = state->block.last_line;
    out->page_start = state->page_start; out->page_end = state->page_end;
    out->page_first_line = state->page_first_line;
    out->page_last_line = state->page_last_line;
    out->text_length = state->page_length;
    memcpy(out->text, state->query->page, state->page_length);
    out->complete = state->complete;
    out->next_offset = state->next_offset;
}
YanBlockQueryResult yan_block_query_read(YanBlockQuery *query, const char *name,
    const char *id, uint32_t offset, const uint8_t expected[32],
    YanBlockQueryRead *out)
{
    char name_value[32], id_value[32];
    uint8_t expect[32], before[32], after[32];
    bool has_expected = false;
    YanBlockQueryResult result = validate_request(query, name, out,
        sizeof *out, expected, name_value, expect, &has_expected);
    if (result != YAN_BLOCK_QUERY_OK) return result;
    if (!id_copy(id, id_value)) return YAN_BLOCK_QUERY_INVALID;
    YanBlockAddress address = address_copy(name_value, id_value);
    if (cancelled(query->cancel)) return YAN_BLOCK_QUERY_CANCELLED;
    result = observe(query, before);
    if (result != YAN_BLOCK_QUERY_OK) return result;
    if (expected_differs(has_expected, expect, before)) return YAN_BLOCK_QUERY_STALE;
    if (cancelled(query->cancel)) return YAN_BLOCK_QUERY_CANCELLED;
    ReadState state;
    memset(&state, 0, sizeof state);
    state.query = query;
    state.offset = offset;
    state.error = YAN_BLOCK_QUERY_OK;
    YanBlockResult block = service_show(query, &address, read_row, read_chunk,
                                        &state, NULL);
    if (cancelled(query->cancel)) return YAN_BLOCK_QUERY_CANCELLED;
    if (state.error != YAN_BLOCK_QUERY_OK) return state.error;
    if (block != YAN_BLOCK_OK) return map_block(block, query->cancel);
    result = prepare_read(&state);
    if (result != YAN_BLOCK_QUERY_OK) return result;
    result = observe_pair(query, before, after);
    if (result != YAN_BLOCK_QUERY_OK) return result;
    memcpy(query->source_sha256, after, 32);
    publish_read(&state, out);
    return YAN_BLOCK_QUERY_OK;
}

const char *yan_block_query_result_name(YanBlockQueryResult result)
{
    switch (result) {
    case YAN_BLOCK_QUERY_OK: return "OK";
    case YAN_BLOCK_QUERY_INVALID: return "INVALID";
    case YAN_BLOCK_QUERY_BUSY: return "BUSY";
    case YAN_BLOCK_QUERY_STOPPED: return "STOPPED";
    case YAN_BLOCK_QUERY_CANCELLED: return "CANCELLED";
    case YAN_BLOCK_QUERY_STALE: return "STALE";
    case YAN_BLOCK_QUERY_METADATA: return "METADATA";
    case YAN_BLOCK_QUERY_BLOCK_FORMAT: return "BLOCK_FORMAT";
    case YAN_BLOCK_QUERY_AMBIGUOUS: return "AMBIGUOUS";
    case YAN_BLOCK_QUERY_LIMIT: return "LIMIT";
    case YAN_BLOCK_QUERY_INVALID_TEXT: return "INVALID_TEXT";
    case YAN_BLOCK_QUERY_NOT_MOUNTED: return "NOT_MOUNTED";
    case YAN_BLOCK_QUERY_FAULTED: return "FAULTED";
    case YAN_BLOCK_QUERY_NOT_FOUND: return "NOT_FOUND";
    case YAN_BLOCK_QUERY_IO: return "IO";
    case YAN_BLOCK_QUERY_PROTOCOL: return "PROTOCOL";
    case YAN_BLOCK_QUERY_CORRUPT: return "CORRUPT";
    case YAN_BLOCK_QUERY_UNSUPPORTED: return "UNSUPPORTED";
    }
    return "UNKNOWN";
}
