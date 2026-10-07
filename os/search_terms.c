/* Unified term-search facade for 0026.
 *
 * The contract is os/search_terms.h and docs/specs/0026-term-search-index.md.
 * This translation unit owns validation order, the busy lifecycle, the
 * synchronous callback window, the sticky reader error and the mapping of a
 * callback stop to YAN_SEARCH_TERMS_STOPPED. It performs no filesystem and no
 * console I/O and does not include yanfs.h; the backend vtable is the only way
 * out.
 *
 * The shape mirrors the 0025 literal facade on purpose: a consumer that already
 * understands the literal busy/sticky/stopped contract does not have to learn a
 * second one, and the backend cannot tell the two apart beyond its own result
 * type. What is new here is the summary holder and the snippet reader.
 *
 *   validate before the backend    facade, spans, query bytes, query alias,
 *   summary holder and the required match callback all run before
 *   backend.query. An invalid call performs no I/O and reaches no backend.
 *
 *   protected spans                the backend declares its own context and its
 *   borrowed source as opaque (base,size) spans. init, query, the snippet reader
 *   and status all reject an alias with pure uintptr_t arithmetic before they
 *   read a bool field or write a holder.
 *
 *   a reader only inside a callback    read_snippet is the allowed busy-window
 *   entry point. Outside the callback it is INVALID, a nested read while the
 *   facade streams that snippet is BUSY, and a manual read from the match
 *   callback itself is legal.
 *
 *   sticky source errors    a backend read error is latched and wins over a
 *   callback stop or an ignored return value.
 *
 *   summary only on success    a cancelled or failed query publishes no
 *   summary, so a caller cannot mistake a zeroed or partial summary for a
 *   complete run.
 *
 *   one owner at a time    busy spans the whole query, so a reentrant query,
 *   status, clear or rebuild returns BUSY without touching the backend. */

#include "search_terms.h"

#include "search_terms_core.h"

#include <stddef.h>

/* Internal bridge between the consumer sink and the backend match callback. */
typedef struct {
    YanSearchTerms *terms;
    YanSearchTermsSink sink;
} YanSearchTermsBridge;

/* True when two byte ranges intersect. A range that would leave uintptr_t is
 * reported as overlapping, matching the filesystem core and the literal
 * facade. Nothing is read or written through either range. */
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

/* A backend may only report one of the enumerators this API defines; anything
 * else is an inconsistent backend and becomes a protocol error. */
static bool result_known(YanSearchTermsResult result)
{
    return (int)result >= (int)YAN_SEARCH_TERMS_OK &&
           (int)result <= (int)YAN_SEARCH_TERMS_INDEX_LIMIT;
}

static void clear_match(YanSearchTerms *terms)
{
    terms->current_match.name = NULL;
    terms->current_match.line_number = 0u;
    terms->current_match.score = 0u;
    terms->current_match.snippet_length = 0u;
    terms->current_match.left_truncated = false;
    terms->current_match.right_truncated = false;
}

/* One backend snippet read with the post-conditions this facade relies on. A
 * failure is latched like any reader error; a protocol violation is never a
 * healthy end. */
static YanSearchTermsResult terms_fetch(YanSearchTerms *terms, uint32_t offset,
                                        uint32_t capacity,
                                        const uint8_t **bytes, uint32_t *length)
{
    if (terms->reader_error != YAN_SEARCH_TERMS_OK) {
        return terms->reader_error;
    }
    const uint8_t *got = NULL;
    uint32_t got_length = 0u;
    YanSearchTermsResult result = terms->backend.read_match(
        terms->backend.context, offset, capacity, &got, &got_length);
    if (!result_known(result)) {
        result = YAN_SEARCH_TERMS_PROTOCOL;
    }
    if (result != YAN_SEARCH_TERMS_OK) {
        terms->reader_error = result;
        return result;
    }
    uint32_t remaining = terms->current_match.snippet_length - offset;
    if ((got_length == 0u && remaining > 0u) || got_length > capacity ||
        got_length > remaining || (got_length > 0u && got == NULL) ||
        (got_length > 0u &&
         (uint64_t)got_length > (uint64_t)(UINTPTR_MAX - (uintptr_t)got))) {
        terms->reader_error = YAN_SEARCH_TERMS_PROTOCOL;
        return YAN_SEARCH_TERMS_PROTOCOL;
    }
    *bytes = got;
    *length = got_length;
    return YAN_SEARCH_TERMS_OK;
}

/* Streams the current snippet to the sink chunk callback. reader_busy is set by
 * the caller, so a chunk callback that calls read_snippet again sees BUSY. */
static bool terms_stream(YanSearchTermsBridge *bridge)
{
    YanSearchTerms *terms = bridge->terms;
    uint32_t total = terms->current_match.snippet_length;
    uint32_t offset = 0u;
    while (offset < total) {
        const uint8_t *bytes = NULL;
        uint32_t length = 0u;
        YanSearchTermsResult result =
            terms_fetch(terms, offset, total - offset, &bytes, &length);
        if (result != YAN_SEARCH_TERMS_OK) {
            return false;
        }
        if (length == 0u || length > total - offset) {
            terms->reader_error = YAN_SEARCH_TERMS_PROTOCOL;
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
 * any call made after the query already stopped, copies the value for the
 * snippet reader, hands it to the consumer, and only then streams the snippet. */
static bool terms_facade_match(void *context, const YanSearchTermsMatch *match)
{
    YanSearchTermsBridge *bridge = (YanSearchTermsBridge *)context;
    YanSearchTerms *terms = bridge->terms;
    if (terms->reader_error != YAN_SEARCH_TERMS_OK || terms->stopped) {
        return false;
    }
    if (match == NULL || match->name == NULL || match->line_number == 0u ||
        match->snippet_length > YAN_SEARCH_TERMS_SNIPPET_MAX) {
        terms->reader_error = YAN_SEARCH_TERMS_PROTOCOL;
        return false;
    }
    terms->current_match = *match;
    terms->in_callback = true;
    bool keep = bridge->sink.match(bridge->sink.context, match);
    if (keep && terms->reader_error != YAN_SEARCH_TERMS_OK) {
        keep = false;
    }
    if (keep && bridge->sink.chunk != NULL) {
        terms->reader_busy = true;
        keep = terms_stream(bridge);
        terms->reader_busy = false;
    }
    terms->in_callback = false;
    if (!keep) {
        terms->stopped = true;
    }
    return keep;
}

/* The shared prefix of the memory-only management entry points: context, then
 * BUSY. It does not reject FAULTED or UNMOUNTED, because status/clear/rebuild
 * own their source policy; an invalid source is reported by the backend. */
static YanSearchTermsResult manage_guard(const YanSearchTerms *terms)
{
    if (terms == NULL) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    if (!terms->initialized) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    if (terms->busy) {
        return YAN_SEARCH_TERMS_BUSY;
    }
    return YAN_SEARCH_TERMS_OK;
}

YanSearchTermsResult yan_search_terms_init(YanSearchTerms *terms,
                                           YanSearchTermsBackend backend)
{
    if (terms == NULL) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    /* Pure address arithmetic before the first bool field is read, so a facade
     * placed inside a span is rejected without an undefined load. */
    if (!span_valid(backend.context, backend.context_size) ||
        !span_valid(backend.source_context, backend.source_size)) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    if (ranges_overlap(terms, sizeof *terms, backend.context,
                       backend.context_size) ||
        ranges_overlap(terms, sizeof *terms, backend.source_context,
                       backend.source_size) ||
        ranges_overlap(backend.context, backend.context_size,
                       backend.source_context, backend.source_size)) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    if (terms->initialized && terms->busy) {
        return YAN_SEARCH_TERMS_BUSY;
    }
    if (backend.query == NULL || backend.read_match == NULL) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    terms->backend = backend;
    terms->initialized = true;
    terms->busy = false;
    terms->in_callback = false;
    terms->reader_busy = false;
    terms->stopped = false;
    terms->reader_error = YAN_SEARCH_TERMS_OK;
    clear_match(terms);
    return YAN_SEARCH_TERMS_OK;
}

YanSearchTermsResult yan_search_terms(YanSearchTerms *terms,
                                      const uint8_t *query,
                                      uint32_t query_length,
                                      YanSearchTermsSink sink,
                                      YanSearchTermsSummary *summary)
{
    if (terms == NULL || !terms->initialized) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    if (terms->busy) {
        return YAN_SEARCH_TERMS_BUSY;
    }
    if (query == NULL || query_length == 0u ||
        query_length > YAN_SEARCH_TERMS_QUERY_MAX) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    /* Range and alias arithmetic runs before the first query byte is read, so a
     * pointer plus a length that leaves the address space is rejected without a
     * dereference and an aliased query never reaches the backend. */
    if ((uint64_t)query_length > (uint64_t)(UINTPTR_MAX - (uintptr_t)query)) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    if (ranges_overlap(terms, sizeof *terms, query, (size_t)query_length) ||
        ranges_overlap(terms->backend.context, terms->backend.context_size,
                       query, (size_t)query_length) ||
        ranges_overlap(terms->backend.source_context,
                       terms->backend.source_size, query,
                       (size_t)query_length)) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    if (sink.match == NULL) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    if (summary != NULL) {
        if ((uint64_t)sizeof *summary >
            (uint64_t)(UINTPTR_MAX - (uintptr_t)summary)) {
            return YAN_SEARCH_TERMS_INVALID;
        }
        if (ranges_overlap(terms, sizeof *terms, summary, sizeof *summary) ||
            ranges_overlap(terms->backend.context, terms->backend.context_size,
                           summary, sizeof *summary) ||
            ranges_overlap(terms->backend.source_context,
                           terms->backend.source_size, summary,
                           sizeof *summary) ||
            ranges_overlap(query, (size_t)query_length, summary,
                           sizeof *summary)) {
            return YAN_SEARCH_TERMS_INVALID;
        }
    }

    /* Pure grammar validation runs before the backend: a NUL, a control byte,
     * invalid UTF-8, an over-long word, more than 16 distinct groups or a
     * separator-only query is rejected here with no backend call and no I/O.
     * The small parsed table lives on the stack (~128 bytes). */
    YanSearchTermsQuery parsed;
    if (yan_search_terms_parse(query, query_length, &parsed) !=
        YAN_SEARCH_TERMS_OK) {
        return YAN_SEARCH_TERMS_INVALID;
    }

    terms->reader_error = YAN_SEARCH_TERMS_OK;
    terms->stopped = false;
    terms->in_callback = false;
    terms->reader_busy = false;
    clear_match(terms);

    YanSearchTermsBridge bridge;
    bridge.terms = terms;
    bridge.sink = sink;

    /* The backend always receives a real summary object; the caller's holder is
     * written only on OK, so a cancelled or failed query leaves it alone. */
    YanSearchTermsSummary internal_summary;
    internal_summary.total = 0u;
    internal_summary.shown = 0u;
    internal_summary.skipped = 0u;
    internal_summary.mode = YAN_SEARCH_MODE_SCAN;

    terms->busy = true;
    YanSearchTermsResult result = terms->backend.query(
        terms->backend.context, query, query_length, terms_facade_match,
        &bridge, &internal_summary);
    terms->busy = false;
    terms->in_callback = false;
    terms->reader_busy = false;

    YanSearchTermsResult final = result;
    if (terms->reader_error != YAN_SEARCH_TERMS_OK) {
        final = terms->reader_error;
    } else if (!result_known(result)) {
        final = YAN_SEARCH_TERMS_PROTOCOL;
    } else if (terms->stopped && result == YAN_SEARCH_TERMS_OK) {
        final = YAN_SEARCH_TERMS_STOPPED;
    }
    if (final == YAN_SEARCH_TERMS_OK && summary != NULL) {
        *summary = internal_summary;
    }
    return final;
}

YanSearchTermsResult yan_search_terms_read_snippet(
    YanSearchTerms *terms, uint32_t offset, uint32_t capacity,
    const uint8_t **bytes, uint32_t *length)
{
    if (terms == NULL || !terms->initialized) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    if (!terms->in_callback) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    if (terms->reader_busy) {
        return YAN_SEARCH_TERMS_BUSY;
    }
    if (bytes == NULL || length == NULL) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    if ((uint64_t)sizeof *bytes >
            (uint64_t)(UINTPTR_MAX - (uintptr_t)bytes) ||
        (uint64_t)sizeof *length >
            (uint64_t)(UINTPTR_MAX - (uintptr_t)length)) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    /* The holders are judged before any write, including the empty write of a
     * zero-capacity or end-offset read. */
    if (ranges_overlap(terms, sizeof *terms, bytes, sizeof *bytes) ||
        ranges_overlap(terms, sizeof *terms, length, sizeof *length) ||
        ranges_overlap(terms->backend.context, terms->backend.context_size,
                       bytes, sizeof *bytes) ||
        ranges_overlap(terms->backend.context, terms->backend.context_size,
                       length, sizeof *length) ||
        ranges_overlap(terms->backend.source_context,
                       terms->backend.source_size, bytes, sizeof *bytes) ||
        ranges_overlap(terms->backend.source_context,
                       terms->backend.source_size, length, sizeof *length) ||
        ranges_overlap(bytes, sizeof *bytes, length, sizeof *length)) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    if (offset > terms->current_match.snippet_length) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    if (terms->reader_error != YAN_SEARCH_TERMS_OK) {
        return terms->reader_error;
    }
    if (capacity == 0u || offset == terms->current_match.snippet_length) {
        *bytes = NULL;
        *length = 0u;
        return YAN_SEARCH_TERMS_OK;
    }

    terms->reader_busy = true;
    const uint8_t *got = NULL;
    uint32_t got_length = 0u;
    YanSearchTermsResult result =
        terms_fetch(terms, offset, capacity, &got, &got_length);
    terms->reader_busy = false;
    if (result == YAN_SEARCH_TERMS_OK) {
        *bytes = got;
        *length = got_length;
    }
    return result;
}

YanSearchTermsResult yan_search_terms_status(YanSearchTerms *terms,
                                             YanSearchTermsStatus *out)
{
    YanSearchTermsResult guard = manage_guard(terms);
    if (guard != YAN_SEARCH_TERMS_OK) {
        return guard;
    }
    if (out == NULL) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    if ((uint64_t)sizeof *out > (uint64_t)(UINTPTR_MAX - (uintptr_t)out) ||
        ranges_overlap(terms, sizeof *terms, out, sizeof *out) ||
        ranges_overlap(terms->backend.context, terms->backend.context_size,
                       out, sizeof *out) ||
        ranges_overlap(terms->backend.source_context,
                       terms->backend.source_size, out, sizeof *out)) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    if (terms->backend.status == NULL) {
        return YAN_SEARCH_TERMS_UNSUPPORTED;
    }
    /* Busy spans the backend call, so a management callback that reenters any
     * entry point sees BUSY instead of a half-updated backend. */
    terms->busy = true;
    YanSearchTermsResult result =
        terms->backend.status(terms->backend.context, out);
    terms->busy = false;
    if (!result_known(result)) {
        return YAN_SEARCH_TERMS_PROTOCOL;
    }
    return result;
}

YanSearchTermsResult yan_search_terms_clear(YanSearchTerms *terms)
{
    YanSearchTermsResult guard = manage_guard(terms);
    if (guard != YAN_SEARCH_TERMS_OK) {
        return guard;
    }
    if (terms->backend.clear == NULL) {
        return YAN_SEARCH_TERMS_UNSUPPORTED;
    }
    terms->busy = true;
    YanSearchTermsResult result = terms->backend.clear(terms->backend.context);
    terms->busy = false;
    if (!result_known(result)) {
        return YAN_SEARCH_TERMS_PROTOCOL;
    }
    return result;
}

YanSearchTermsResult yan_search_terms_rebuild(YanSearchTerms *terms)
{
    YanSearchTermsResult guard = manage_guard(terms);
    if (guard != YAN_SEARCH_TERMS_OK) {
        return guard;
    }
    if (terms->backend.rebuild == NULL) {
        return YAN_SEARCH_TERMS_UNSUPPORTED;
    }
    terms->busy = true;
    YanSearchTermsResult result = terms->backend.rebuild(terms->backend.context);
    terms->busy = false;
    if (!result_known(result)) {
        return YAN_SEARCH_TERMS_PROTOCOL;
    }
    return result;
}
