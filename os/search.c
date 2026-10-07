/* Unified search facade for 0025.
 *
 * The contract is os/search.h and docs/specs/0025-knowledge-search.md. This
 * translation unit owns validation order, the busy lifecycle, the synchronous
 * callback window, the sticky reader error and the mapping of a callback stop
 * to YAN_SEARCH_STOPPED. It performs no filesystem and no console I/O and does
 * not include yanfs.h; the backend vtable is the only way out.
 *
 * The pieces that decide the shape of the code:
 *
 *   validate before the backend    the Search, the two protected spans, the
 *   pattern bytes, the pattern range/alias and the required match callback all
 *   run before backend.query. An invalid call performs no I/O and reaches no
 *   backend, and a known-bad pointer is never dereferenced.
 *
 *   protected spans                the backend declares its own context and its
 *   borrowed source as opaque (base,size) spans. The facade treats their bytes
 *   as untouchable: init, query and the match reader all reject an alias with
 *   pure uintptr_t arithmetic before they read a bool field or write a holder.
 *
 *   a reader only inside a callback    read_match is the allowed busy-window
 *   entry point. Outside the callback it is INVALID, a nested read while the
 *   facade streams that match is BUSY, and a manual chunk from the match
 *   callback itself is legal. Both answer without a backend read.
 *
 *   sticky source errors    a backend read error is latched and wins over a
 *   callback stop or an ignored return value. A backend that makes no progress
 *   before the end of the line, returns an over-long/null/wrapping chunk, or
 *   reports malformed match metadata is a protocol error, never a healthy stop.
 *
 *   one owner at a time    busy spans the whole query, so a reentrant query or
 *   init returns BUSY without touching the backend or the image. */

#include "search.h"

#include <stddef.h>

/* Internal bridge between the consumer sink and the backend match callback. The
 * backend receives this as its match_context and never sees the sink type. */
typedef struct {
    YanSearch *search;
    YanSearchSink sink;
} YanSearchBridge;

/* True when two byte ranges intersect. A range that would leave uintptr_t is
 * reported as overlapping, which is the same rule the filesystem core and the
 * shell use. Nothing is read or written through either range. */
static bool ranges_overlap(const void *first, size_t first_length,
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

/* A zero-length span is absent and may have a NULL base. A nonempty span needs
 * a base and a range that does not leave uintptr_t. No byte is read. */
static bool span_valid(const void *base, size_t size)
{
    if (size == 0u) {
        return true;
    }
    if (base == NULL) {
        return false;
    }
    return (uint64_t)size <= (uint64_t)(UINTPTR_MAX - (uintptr_t)base);
}

/* A backend may only report one of the enumerators this API defines. Anything
 * else is an inconsistent backend and becomes a protocol error rather than an
 * unknown value the caller cannot classify. */
static bool result_known(YanSearchResult result)
{
    return (int)result >= (int)YAN_SEARCH_OK &&
           (int)result <= (int)YAN_SEARCH_NOT_FOUND;
}

static void clear_match(YanSearch *search)
{
    search->current_match.name = NULL;
    search->current_match.line_number = 0u;
    search->current_match.content_length = 0u;
}

/* One backend read with the post-conditions this facade relies on:
 *
 *   - a nonempty request that returns nothing (no progress before the end) is a
 *     protocol error, not an early end;
 *   - the chunk must fit the requested capacity and the remaining content;
 *   - a nonempty chunk must point at addressable bytes and its range must not
 *     wrap uintptr_t.
 *
 * A violation is a protocol error from the backend, not data damage, and it is
 * latched like any other reader error. */
static YanSearchResult search_fetch(YanSearch *search, uint32_t offset,
                                    uint32_t capacity,
                                    const uint8_t **bytes, uint32_t *length)
{
    if (search->reader_error != YAN_SEARCH_OK) {
        return search->reader_error;
    }
    const uint8_t *got = NULL;
    uint32_t got_length = 0u;
    YanSearchResult result = search->backend.read_match(
        search->backend.context, offset, capacity, &got, &got_length);
    if (!result_known(result)) {
        result = YAN_SEARCH_PROTOCOL;
    }
    if (result != YAN_SEARCH_OK) {
        search->reader_error = result;
        return result;
    }
    uint32_t remaining = search->current_match.content_length - offset;
    if ((got_length == 0u && remaining > 0u) || got_length > capacity ||
        got_length > remaining || (got_length > 0u && got == NULL) ||
        (got_length > 0u &&
         (uint64_t)got_length > (uint64_t)(UINTPTR_MAX - (uintptr_t)got))) {
        search->reader_error = YAN_SEARCH_PROTOCOL;
        return YAN_SEARCH_PROTOCOL;
    }
    *bytes = got;
    *length = got_length;
    return YAN_SEARCH_OK;
}

/* Streams the current match to the sink chunk callback. The caller has already
 * set reader_busy, so a chunk callback that turns around and calls
 * yan_search_read_match sees BUSY rather than starting a second reader. */
static bool facade_stream(YanSearchBridge *bridge)
{
    YanSearch *search = bridge->search;
    uint32_t total = search->current_match.content_length;
    uint32_t offset = 0u;
    while (offset < total) {
        const uint8_t *bytes = NULL;
        uint32_t length = 0u;
        YanSearchResult result =
            search_fetch(search, offset, total - offset, &bytes, &length);
        if (result != YAN_SEARCH_OK) {
            return false;
        }
        if (length == 0u || length > total - offset) {
            search->reader_error = YAN_SEARCH_PROTOCOL;
            return false;
        }
        if (!bridge->sink.chunk(bridge->sink.context, offset, bytes, length)) {
            return false;
        }
        offset += length;
    }
    return true;
}

/* The match callback the backend receives. It rejects malformed metadata and
 * any call made after the query already stopped, hands the value to the
 * consumer, and only then streams the content to the consumer's chunk callback.
 * A manual read from the match callback is legal because reader_busy is set
 * only around the stream, not around the match callback. */
static bool facade_match(void *context, const YanSearchMatch *match)
{
    YanSearchBridge *bridge = (YanSearchBridge *)context;
    YanSearch *search = bridge->search;
    /* Once stopped or failed, no further consumer callback may run even if a
     * backend ignores the false return. */
    if (search->reader_error != YAN_SEARCH_OK || search->stopped) {
        return false;
    }
    if (match == NULL || match->name == NULL || match->line_number == 0u ||
        match->content_length == 0u) {
        search->reader_error = YAN_SEARCH_PROTOCOL;
        return false;
    }
    search->current_match = *match;
    search->in_callback = true;
    bool keep = bridge->sink.match(bridge->sink.context, match);
    if (keep && search->reader_error != YAN_SEARCH_OK) {
        keep = false;
    }
    if (keep && bridge->sink.chunk != NULL) {
        search->reader_busy = true;
        keep = facade_stream(bridge);
        search->reader_busy = false;
    }
    search->in_callback = false;
    if (!keep) {
        search->stopped = true;
    }
    return keep;
}

YanSearchResult yan_search_init(YanSearch *search, YanSearchBackend backend)
{
    if (search == NULL) {
        return YAN_SEARCH_INVALID;
    }
    /* Every span and alias decision is pure address arithmetic and runs before
     * the first bool field of the Search is read, so a Search placed inside the
     * backend context or the borrowed source is rejected without a load that
     * could be undefined. */
    if (!span_valid(backend.context, backend.context_size) ||
        !span_valid(backend.source_context, backend.source_size)) {
        return YAN_SEARCH_INVALID;
    }
    if (ranges_overlap(search, sizeof *search, backend.context,
                       backend.context_size) ||
        ranges_overlap(search, sizeof *search, backend.source_context,
                       backend.source_size) ||
        ranges_overlap(backend.context, backend.context_size,
                       backend.source_context, backend.source_size)) {
        return YAN_SEARCH_INVALID;
    }
    if (search->initialized && search->busy) {
        return YAN_SEARCH_BUSY;
    }
    if (backend.query == NULL || backend.read_match == NULL) {
        return YAN_SEARCH_INVALID;
    }
    search->backend = backend;
    search->initialized = true;
    search->busy = false;
    search->in_callback = false;
    search->reader_busy = false;
    search->stopped = false;
    search->reader_error = YAN_SEARCH_OK;
    clear_match(search);
    return YAN_SEARCH_OK;
}

YanSearchResult yan_search_query(YanSearch *search, const uint8_t *pattern,
                                 uint32_t pattern_length, YanSearchSink sink)
{
    if (search == NULL || !search->initialized) {
        return YAN_SEARCH_INVALID;
    }
    if (search->busy) {
        return YAN_SEARCH_BUSY;
    }
    if (pattern == NULL || pattern_length == 0u ||
        pattern_length > YAN_SEARCH_PATTERN_MAX) {
        return YAN_SEARCH_INVALID;
    }
    /* Address arithmetic and all alias tests run before the first pattern byte
     * is read, so a pointer plus a length that leaves the address space is
     * rejected without a dereference and an aliased pattern never reaches the
     * backend. */
    if ((uint64_t)pattern_length >
        (uint64_t)(UINTPTR_MAX - (uintptr_t)pattern)) {
        return YAN_SEARCH_INVALID;
    }
    if (ranges_overlap(search, sizeof *search, pattern,
                       (size_t)pattern_length) ||
        ranges_overlap(search->backend.context, search->backend.context_size,
                       pattern, (size_t)pattern_length) ||
        ranges_overlap(search->backend.source_context,
                       search->backend.source_size, pattern,
                       (size_t)pattern_length)) {
        return YAN_SEARCH_INVALID;
    }
    for (uint32_t i = 0; i < pattern_length; ++i) {
        if (pattern[i] == 0u || pattern[i] == (uint8_t)'\n') {
            return YAN_SEARCH_INVALID;
        }
    }
    if (sink.match == NULL) {
        return YAN_SEARCH_INVALID;
    }

    search->reader_error = YAN_SEARCH_OK;
    search->stopped = false;
    search->in_callback = false;
    search->reader_busy = false;
    clear_match(search);

    YanSearchBridge bridge;
    bridge.search = search;
    bridge.sink = sink;

    search->busy = true;
    YanSearchResult result = search->backend.query(
        search->backend.context, pattern, pattern_length, facade_match, &bridge);
    search->busy = false;
    search->in_callback = false;
    search->reader_busy = false;

    if (search->reader_error != YAN_SEARCH_OK) {
        return search->reader_error;
    }
    if (!result_known(result)) {
        return YAN_SEARCH_PROTOCOL;
    }
    if (search->stopped && result == YAN_SEARCH_OK) {
        return YAN_SEARCH_STOPPED;
    }
    return result;
}

YanSearchResult yan_search_read_match(YanSearch *search, uint32_t offset,
                                      uint32_t capacity,
                                      const uint8_t **bytes, uint32_t *length)
{
    if (search == NULL || !search->initialized) {
        return YAN_SEARCH_INVALID;
    }
    /* The reader is a capability of one match callback; outside that window it
     * is an ordinary invalid argument, checked before any backend work. */
    if (!search->in_callback) {
        return YAN_SEARCH_INVALID;
    }
    if (search->reader_busy) {
        return YAN_SEARCH_BUSY;
    }
    if (bytes == NULL || length == NULL) {
        return YAN_SEARCH_INVALID;
    }
    if ((uint64_t)sizeof *bytes >
            (uint64_t)(UINTPTR_MAX - (uintptr_t)bytes) ||
        (uint64_t)sizeof *length >
            (uint64_t)(UINTPTR_MAX - (uintptr_t)length)) {
        return YAN_SEARCH_INVALID;
    }
    /* The holders are judged before any write, including the empty write of a
     * zero-capacity or end-offset read. A holder overlapping the Search, either
     * protected span or the other holder is rejected with no backend I/O. */
    if (ranges_overlap(search, sizeof *search, bytes, sizeof *bytes) ||
        ranges_overlap(search, sizeof *search, length, sizeof *length) ||
        ranges_overlap(search->backend.context, search->backend.context_size,
                       bytes, sizeof *bytes) ||
        ranges_overlap(search->backend.context, search->backend.context_size,
                       length, sizeof *length) ||
        ranges_overlap(search->backend.source_context,
                       search->backend.source_size, bytes, sizeof *bytes) ||
        ranges_overlap(search->backend.source_context,
                       search->backend.source_size, length, sizeof *length) ||
        ranges_overlap(bytes, sizeof *bytes, length, sizeof *length)) {
        return YAN_SEARCH_INVALID;
    }
    if (offset > search->current_match.content_length) {
        return YAN_SEARCH_INVALID;
    }
    if (search->reader_error != YAN_SEARCH_OK) {
        return search->reader_error;
    }
    if (capacity == 0u || offset == search->current_match.content_length) {
        *bytes = NULL;
        *length = 0u;
        return YAN_SEARCH_OK;
    }

    search->reader_busy = true;
    const uint8_t *got = NULL;
    uint32_t got_length = 0u;
    YanSearchResult result =
        search_fetch(search, offset, capacity, &got, &got_length);
    search->reader_busy = false;
    if (result == YAN_SEARCH_OK) {
        *bytes = got;
        *length = got_length;
    }
    return result;
}
