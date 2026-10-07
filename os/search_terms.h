#ifndef YAN_OS_SEARCH_TERMS_H
#define YAN_OS_SEARCH_TERMS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Unified term (token) search facade for 0026.
 *
 * The contract is docs/specs/0026-term-search-index.md. This header is
 * independent of the 0025 literal search: the old os/search.h enum, match
 * struct and entry points keep their exact meaning, and a consumer picks
 * between literal and term semantics by choosing which facade it calls. The
 * term facade itself is independent of YanFS and the console; a backend vtable
 * is the only way out; filesystem dependencies live in the backend headers.
 *
 * Layering:
 *
 *   consumer (shell, terminal)   yan_search_terms / yan_search_terms_read_snippet
 *       |
 *   Terms facade                 validation, busy, callback lifetime, sticky
 *                                errors, stopped vs. summary
 *       |
 *   backend vtable               query + snippet reader + status/rebuild/clear
 *       |
 *   indexed backend with linear  index lookup, automatic full-scan
 *   fallback                     fallback when caching is unavailable
 *
 * The application injects one backend; the shell never selects one. The
 * indexed backend owns its linear fallback and exposes the same facade.
 *
 * Lifetime: a match value and the raw snippet bytes are borrowed only during the
 * synchronous callback that received them. A caller may copy them, not keep the
 * pointers. The whole query keeps the borrowed source stable - a caller
 * precondition, not an fs->busy lock held across the query.
 *
 * This header performs no I/O, allocates nothing and reads no byte of a
 * protected span. Every object is caller-owned static or long-lived storage, so
 * the 4 KiB task stack of 0019 is never involved. */

/* A query is 1..1023 bytes of legal UTF-8. TAB is a separator; NUL, LF and the
 * other ASCII control bytes (including DEL) are rejected. The parser
 * folds ASCII case for group identity and accepts at most 16 *distinct* groups
 * after folding; repeated groups are free. */
#define YAN_SEARCH_TERMS_QUERY_MAX UINT32_C(1023)
#define YAN_SEARCH_TERMS_GROUP_MAX UINT32_C(16)
#define YAN_SEARCH_TERMS_WORD_MAX UINT32_C(255)
/* Summary shown cap and the snippet content cap from the spec. */
#define YAN_SEARCH_TERMS_SHOWN_MAX UINT32_C(20)
#define YAN_SEARCH_TERMS_SNIPPET_MAX UINT32_C(640)
/* Highest score the per-group cap of 255 and at most 16 groups can produce. */
#define YAN_SEARCH_TERMS_SCORE_MAX UINT32_C(4080)

/* Term outcome. It reuses the literal enum's numeric classes so a front end
 * keeps one ERROR/fatal mapping, but it is a separate type and the literal enum
 * is untouched. INDEX_LIMIT is an ordinary management error: the index did not
 * fit and a query falls back to a scan; it is not a fatal source failure. */
typedef enum {
    YAN_SEARCH_TERMS_OK = 0,
    YAN_SEARCH_TERMS_INVALID = 1,
    YAN_SEARCH_TERMS_BUSY = 2,
    YAN_SEARCH_TERMS_STOPPED = 3,
    YAN_SEARCH_TERMS_NOT_MOUNTED = 4,
    YAN_SEARCH_TERMS_FAULTED = 5,
    YAN_SEARCH_TERMS_IO = 6,
    YAN_SEARCH_TERMS_PROTOCOL = 7,
    YAN_SEARCH_TERMS_CORRUPT = 8,
    YAN_SEARCH_TERMS_UNSUPPORTED = 9,
    YAN_SEARCH_TERMS_NOT_FOUND = 10,
    YAN_SEARCH_TERMS_INDEX_LIMIT = 11
} YanSearchTermsResult;

/* Which execution path produced a successful summary. */
typedef enum {
    YAN_SEARCH_MODE_INDEX = 0,
    YAN_SEARCH_MODE_SCAN = 1
} YanSearchTermsMode;

/* Index lifecycle state, 0026 "index status". */
typedef enum {
    YAN_SEARCH_INDEX_EMPTY = 0,
    YAN_SEARCH_INDEX_READY = 1,
    YAN_SEARCH_INDEX_STALE = 2,
    YAN_SEARCH_INDEX_LIMIT = 3,
    YAN_SEARCH_INDEX_UNCACHEABLE = 4,
    YAN_SEARCH_INDEX_UNAVAILABLE = 5
} YanSearchTermsIndexState;

/* Source observation class, mapped by the backend from the filesystem source
 * identity. The facade never invents it. */
typedef enum {
    YAN_SEARCH_SOURCE_MOUNTED = 0,
    YAN_SEARCH_SOURCE_UNMOUNTED = 1,
    YAN_SEARCH_SOURCE_FAULTED = 2,
    YAN_SEARCH_SOURCE_UNINITIALIZED = 3
} YanSearchTermsSourceState;

/* One matched line. `name` is a NUL-terminated file name borrowed for the
 * callback only; `line_number` is 1-based; `score` is the de-duplicated group
 * count sum, at most 4080; `snippet_length` is the raw snippet content length
 * in bytes, at most 640. The truncation flags describe the two ends of that
 * snippet, not the line. The snippet bytes are read with
 * yan_search_terms_read_snippet, whose offsets are relative to the snippet. */
typedef struct {
    const char *name;
    uint32_t line_number;
    uint16_t score;
    uint32_t snippet_length;
    bool left_truncated;
    bool right_truncated;
} YanSearchTermsMatch;

/* Whole-query summary, valid only when the query returned OK. `total` is a
 * uint64 line count, `shown` is at most 20, `skipped` counts skipped files and
 * is at most 63 for the single-root directory, and `mode` says which path ran.
 * A cancelled query publishes no summary. */
typedef struct {
    uint64_t total;
    uint32_t shown;
    uint32_t skipped;
    YanSearchTermsMode mode;
} YanSearchTermsSummary;

/* Pure status view. terms/postings are only meaningful when state is READY;
 * every other state reports 0 there. */
typedef struct {
    YanSearchTermsIndexState state;
    YanSearchTermsSourceState source;
    uint32_t terms;
    uint32_t postings;
} YanSearchTermsStatus;

/* Called synchronously once per emitted match, best score first. Returning
 * false requests an early stop. The match value is borrowed for this call. */
typedef bool (*YanSearchTermsMatchFn)(void *context,
                                      const YanSearchTermsMatch *match);

/* Optional raw delivery of the current snippet. `offset` is relative to the
 * snippet start; `bytes` is borrowed for this call only. Returning false
 * requests an early stop. */
typedef bool (*YanSearchTermsChunkFn)(void *context, uint32_t offset,
                                     const uint8_t *bytes, uint32_t length);

typedef struct {
    void *context;
    YanSearchTermsMatchFn match;
    YanSearchTermsChunkFn chunk;
} YanSearchTermsSink;

/* Backend query. It fills *summary only on OK. The facade has already validated
 * the query range, the aliases and the sink before this runs. The backend calls
 * `match(match_context, value)` for each selected line. */
typedef YanSearchTermsResult (*YanSearchTermsQueryFn)(
    void *context, const uint8_t *query, uint32_t query_length,
    YanSearchTermsMatchFn match, void *match_context,
    YanSearchTermsSummary *summary);

/* Reads up to capacity bytes of the current snippet from relative offset. On
 * success borrows *bytes for the callback and reports the chunk length. */
typedef YanSearchTermsResult (*YanSearchTermsReadFn)(
    void *context, uint32_t offset, uint32_t capacity,
    const uint8_t **bytes, uint32_t *length);

typedef YanSearchTermsResult (*YanSearchTermsStatusFn)(
    void *context, YanSearchTermsStatus *out);

/* rebuild and clear. Both run outside a query and report their own result. */
typedef YanSearchTermsResult (*YanSearchTermsManageFn)(void *context);

/* Protected spans, exactly the 0025 shape: context/context_size is the
 * backend's own object and source_context/source_size is its borrowed source
 * (for the linear backend, the YanFs). size == 0 means "absent". The facade
 * never dereferences either span; it rejects aliases with pure address
 * arithmetic. A backend with no borrowed source declares source_size == 0.
 *
 * query and read_match are required. status, rebuild and clear are optional; a
 * NULL management entry is reported as YAN_SEARCH_TERMS_UNSUPPORTED rather than
 * silently succeeding. */
typedef struct {
    void *context;
    size_t context_size;
    void *source_context;
    size_t source_size;
    YanSearchTermsQueryFn query;
    YanSearchTermsReadFn read_match;
    YanSearchTermsStatusFn status;
    YanSearchTermsManageFn rebuild;
    YanSearchTermsManageFn clear;
} YanSearchTermsBackend;

/* Memory context only; the fields are private to os/search_terms.c. A caller
 * zero-initializes before yan_search_terms_init and never edits them after. */
typedef struct {
    YanSearchTermsBackend backend;
    bool initialized;
    bool busy;
    bool in_callback;
    bool reader_busy;
    bool stopped;
    YanSearchTermsResult reader_error;
    YanSearchTermsMatch current_match;
} YanSearchTerms;

/* Validates the facade, the required backend entry points and the two protected
 * spans, then records the backend. No I/O and no dereference of a span. A null
 * facade, an incomplete backend, a nonempty span with a NULL base or a range
 * that leaves uintptr_t, and any overlap between the facade and the spans are
 * INVALID. Re-initializing a facade that is inside a query is BUSY; an idle one
 * may be re-initialized to swap backends. */
YanSearchTermsResult yan_search_terms_init(YanSearchTerms *,
                                           YanSearchTermsBackend);

/* Runs one whole term query. Order: null/uninitialized, BUSY, the query range
 * and aliases, the required match callback, then the optional summary holder's
 * range and aliases. The query must be 1..1023 bytes and addressable; it
 * may not overlap the facade, either span or the summary holder. The summary
 * holder is optional (NULL means "do not report") and, when present, may not
 * overlap the facade, either span, the query or itself in a wrapping way.
 *
 * The query returns the backend's result, a latched reader error (which wins
 * over a callback stop or an ignored return value), YAN_SEARCH_TERMS_STOPPED
 * when a callback stopped cleanly and no reader error latched, or OK. A
 * non-OK result publishes no summary. Busy and callback state are cleared on
 * every exit path. */
YanSearchTermsResult yan_search_terms(YanSearchTerms *, const uint8_t *query,
                                      uint32_t query_length,
                                      YanSearchTermsSink sink,
                                      YanSearchTermsSummary *summary);

/* Reads the current snippet. Legal only from inside the synchronous match
 * callback; outside it is INVALID with no I/O. A nested call while the facade
 * streams that snippet is BUSY. `offset` past snippet_length is INVALID. A
 * latched reader error is returned without another backend read. The two output
 * holders are validated (range, overflow and mutual/alias overlap) before they
 * are written, even for a zero-capacity or end-offset read. */
YanSearchTermsResult yan_search_terms_read_snippet(
    YanSearchTerms *, uint32_t offset, uint32_t capacity,
    const uint8_t **bytes, uint32_t *length);

/* Pure status observation through the backend. It does not modify the source or
 * the cache. A missing backend entry point is UNSUPPORTED. */
YanSearchTermsResult yan_search_terms_status(YanSearchTerms *,
                                             YanSearchTermsStatus *out);

/* Drops the cache without touching the source; a missing entry point is
 * UNSUPPORTED. */
YanSearchTermsResult yan_search_terms_clear(YanSearchTerms *);

/* Rebuilds the cache from the source; a missing entry point is UNSUPPORTED. */
YanSearchTermsResult yan_search_terms_rebuild(YanSearchTerms *);

#endif
