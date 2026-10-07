#ifndef YAN_OS_SEARCH_H
#define YAN_OS_SEARCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Unified knowledge-search facade for 0025.
 *
 * The contract is docs/specs/0025-knowledge-search.md. This header repeats the
 * externally visible pieces of that contract and leaves the byte-level literal
 * search algorithm to a backend behind a vtable. A consumer (the shell's future
 * `grep` front end, or a native test) configures a caller-owned Search with a
 * borrowed backend and then runs a whole query; it never sees YanFS, a
 * directory slot or an extent. Only os/search_linear.h includes yanfs.h, which
 * is what keeps that boundary real rather than a naming convention.
 *
 * Layering:
 *
 *   consumer (shell)        yan_search_query / yan_search_read_match
 *       |
 *   Search facade           validation, busy, callback lifetime, sticky errors
 *       |
 *   backend vtable          query + read_match on a borrowed backend context
 *       |
 *   linear backend          public yan_fs_list / yan_fs_read
 *
 * Lifetime rules the facade enforces: a match value and the raw bytes of the
 * current match are borrowed only during the synchronous match callback. A
 * caller may copy them, but may not keep the pointers. The facade makes no
 * promise that an arbitrary saved stale pointer is detected on a later
 * callback; only the documented entry points are guarded.
 *
 * The facade performs no I/O by itself and does not include a console, a test
 * header or yanfs.h, so the same source compiles on the Host and for the Guest.
 * The backend context, the Search and any borrowed YanFS live in separate
 * long-lived storage owned by the caller. */

/* A pattern is an independent byte string of 1..1023 bytes. Only NUL and LF are
 * rejected: TAB, bare CR, invalid UTF-8 and every other byte are legal because
 * the matcher is a raw byte matcher. Matching is case sensitive and does not
 * cross a logical line boundary. */
#define YAN_SEARCH_PATTERN_MAX UINT32_C(1023)

/* Unified query outcome. The enum is independent of YanFS: the linear backend
 * is the only place a YanFsResult becomes one of these. The source classes
 * exist so the front end can reuse its existing ERROR token and fatal mapping
 * instead of collapsing an image failure into "zero matches" or an ordinary
 * cancellation. */
typedef enum {
    YAN_SEARCH_OK = 0,
    /* The call itself was invalid: a null or uninitialized Search, an invalid
     * pattern, a missing match callback, or a reader argument this API can
     * prove is not addressable. No output and no I/O. */
    YAN_SEARCH_INVALID = 1,
    /* The Search is already inside a query (or inside a reader it started), so
     * a reentrant query, init or reader call is refused without I/O. */
    YAN_SEARCH_BUSY = 2,
    /* A callback asked to stop and no source or reader error was latched. This
     * is a healthy early end, not a failure. */
    YAN_SEARCH_STOPPED = 3,
    /* Source classes mirroring the YanFS results the shell already maps to its
     * fatal verdicts. The facade never produces these from its own state. */
    YAN_SEARCH_NOT_MOUNTED = 4,
    YAN_SEARCH_FAULTED = 5,
    YAN_SEARCH_IO = 6,
    YAN_SEARCH_PROTOCOL = 7,
    YAN_SEARCH_CORRUPT = 8,
    YAN_SEARCH_UNSUPPORTED = 9,
    /* The linear backend lists a directory and reads files; a source object
     * that changed mid-query can report NOT_FOUND. It is an ordinary source
     * error, propagated rather than silently skipped, and the front end maps it
     * like the other source classes. */
    YAN_SEARCH_NOT_FOUND = 10
} YanSearchResult;

/* Value-typed description of one match. `name` is a NUL-terminated file name
 * borrowed from the backend; `line_number` is 1-based within the file and
 * restarts at 1 for every file; `content_length` is the full byte length of the
 * matched logical line, terminator excluded, and is not truncated to any
 * display or editor limit. Every pointer and length here is valid only during
 * the callback that received it. */
typedef struct {
    const char *name;
    uint32_t line_number;
    uint32_t content_length;
} YanSearchMatch;

/* Called synchronously once per matched line, in physical directory-slot order
 * and then ascending line number. Returning false requests an early stop. The
 * match value is borrowed for this call only. `context` is the sink context. */
typedef bool (*YanSearchMatchFn)(void *context, const YanSearchMatch *match);

/* Optional raw-byte delivery of the current match content. The facade requests
 * the content from the backend in bounded chunks and calls this once per chunk;
 * `offset` is the byte offset of the chunk within the matched line's content.
 * `bytes` points at borrowed backend storage and is valid only for this call.
 * Returning false requests an early stop. */
typedef bool (*YanSearchChunkFn)(void *context, uint32_t offset,
                                 const uint8_t *bytes, uint32_t length);

/* Callback bundle. `match` is required; `chunk` is optional. When `chunk` is
 * NULL the match callback may pull content itself with yan_search_read_match.
 * When `chunk` is non-NULL the facade streams after the match callback returns.
 * That match callback may still read manually; only a nested reader call from
 * the chunk callback is BUSY. */
typedef struct {
    void *context;
    YanSearchMatchFn match;
    YanSearchChunkFn chunk;
} YanSearchSink;

/* Backend entry points. Both receive the backend's own context. */
typedef YanSearchResult (*YanSearchBackendQueryFn)(
    void *context, const uint8_t *pattern, uint32_t pattern_length,
    YanSearchMatchFn match, void *match_context);

/* Reads up to capacity bytes of the current match content starting at relative
 * offset. On success it borrows *bytes for the current callback and reports the
 * chunk length in *length (0 at the end of the line). A source error is
 * returned as a YanSearchResult. */
typedef YanSearchResult (*YanSearchBackendReadFn)(
    void *context, uint32_t offset, uint32_t capacity,
    const uint8_t **bytes, uint32_t *length);

/* Protected spans the backend declares to the facade.
 *
 * The facade cannot include yanfs.h, so it cannot know that the linear backend
 * borrows a whole YanFs. The backend therefore hands the facade up to two
 * opaque spans and the facade treats their bytes as untouchable:
 *
 *   context/context_size         the backend's own context object
 *   source_context/source_size   the borrowed source object (for the linear
 *                                backend, the YanFs it scans)
 *
 * Contract for each span:
 *   - size == 0 means "no such span"; base may then be NULL and the span is
 *     ignored.
 *   - size != 0 requires base != NULL and that base + size does not leave
 *     uintptr_t. The facade rejects a violating span as INVALID with pure
 *     address arithmetic, before it reads any bool field or performs any I/O.
 *
 * The two spans and the Search object must be pairwise non-overlapping. The
 * pattern and reader output holders must not overlap any of those objects;
 * the holders must also be disjoint from each other. These aliases are rejected
 * as INVALID. Keeping the borrowed pattern unchanged is the caller's separate
 * responsibility. The spans are a declaration of storage ownership, not a
 * capability: the facade never dereferences them. A backend with no borrowed
 * source (a fake backend in a test) declares source_size == 0. */
typedef struct {
    void *context;
    size_t context_size;
    void *source_context;
    size_t source_size;
    YanSearchBackendQueryFn query;
    YanSearchBackendReadFn read_match;
} YanSearchBackend;

/* Memory context only. The fields are private to os/search.c; a caller
 * zero-initializes before yan_search_init and never edits them afterwards. The
 * object is small: the scan and reader buffers live in the backend context. */
typedef struct {
    YanSearchBackend backend;
    bool initialized;
    bool busy;
    bool in_callback;
    bool reader_busy;
    bool stopped;
    YanSearchResult reader_error;
    YanSearchMatch current_match;
} YanSearch;

/* Validates `search`, the backend callbacks and the two protected spans, then
 * records the borrowed backend. No I/O and no dereference of either span:
 * every span/alias decision is pure uintptr_t arithmetic and runs before any
 * bool field of the Search is read. Re-initializing an instance that is
 * currently inside a query returns YAN_SEARCH_BUSY; an idle instance may be
 * re-initialized, which is how a caller swaps backends. A null Search, an
 * incomplete backend, a nonempty span with a NULL base, a span whose range
 * leaves uintptr_t, or spans/Search that overlap each other all return
 * YAN_SEARCH_INVALID. */
YanSearchResult yan_search_init(YanSearch *, YanSearchBackend);

/* Runs one whole query. The order is fixed: a null or uninitialized Search, the
 * BUSY guard, the pattern checks (including that the pattern does not overlap
 * the Search or either protected span), the sink checks, and only then the
 * backend. The pattern is borrowed until the query returns and must not be
 * modified meanwhile; it must be non-empty, at most YAN_SEARCH_PATTERN_MAX
 * bytes, and contain neither NUL nor LF. The pattern range must be addressable.
 * `sink.match` must be non-NULL.
 *
 * The whole query - preflight, scan, callbacks and matched-range reads - keeps
 * the borrowed filesystem and its image stable. That is an explicit caller
 * precondition, not an fs->busy lock held across the query: the backend's own
 * calls take and release the filesystem busy guard per operation. The returned
 * result is the backend's source result, a latched reader error (which wins
 * over a callback stop or an ignored reader result), YAN_SEARCH_STOPPED when a
 * callback stopped cleanly, or YAN_SEARCH_OK. Before returning, the query
 * always clears its busy and callback state, so an early stop or source error
 * leaves the instance reusable. */
YanSearchResult yan_search_query(YanSearch *, const uint8_t *pattern,
                                 uint32_t pattern_length, YanSearchSink sink);

/* Reads the current match content. Legal only from inside the synchronous match
 * callback that received the match; outside the callback it returns
 * YAN_SEARCH_INVALID without I/O. A nested call while the facade is streaming
 * that match's content returns YAN_SEARCH_BUSY without I/O. A match callback
 * that was handed a chunk callback may still pull one manual chunk here; the
 * auto-stream then starts from the beginning, so the two views do not race.
 * `offset` is relative to the matched line's content; an offset past
 * content_length is INVALID. A zero capacity or an offset exactly at the end is
 * a successful empty read, but the output holders are still validated: the
 * holders are written only after every address, alias (Search and both
 * protected spans) and range check passes, so a holder aliasing a protected
 * span is INVALID even when no bytes would be produced and no known-bad pointer
 * is ever dereferenced. If a reader error has latched, that sticky source error
 * is returned without another backend read, and it is what the query ultimately
 * reports even when the caller ignores this return value. */
YanSearchResult yan_search_read_match(YanSearch *, uint32_t offset,
                                      uint32_t capacity,
                                      const uint8_t **bytes, uint32_t *length);

#endif
