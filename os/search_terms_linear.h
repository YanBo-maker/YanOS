#ifndef YAN_OS_SEARCH_TERMS_LINEAR_H
#define YAN_OS_SEARCH_TERMS_LINEAR_H

#include "search_terms.h"
#include "search_terms_core.h"
#include "yanfs.h"

#include <stdbool.h>
#include <stdint.h>

/* Per-slot qualification markers for the 0027 filtered search. They live in the
 * linear context's `metadata_eligible` array, one entry per physical directory
 * slot, and are shared with the embedded index backend. */
#define YAN_NOTE_META_INVALID UINT8_C(0)  /* binary/NUL: skipped once */
#define YAN_NOTE_META_TAG UINT8_C(1)      /* valid text without the tags */
#define YAN_NOTE_META_ELIGIBLE UINT8_C(2) /* valid text with all the tags */

/* No-index linear term backend for 0026, extended by 0027.
 *
 * This backend header includes yanfs.h and the backend
 * borrows a caller-owned YanFs for the whole query, while the facade and every
 * consumer use the semantic facade. A caller can inject this linear backend
 * directly; the production index also embeds it for complete scan fallback.
 *
 * The context owns the shared scan buffers, so nothing large ever lands on the
 * 4 KiB task stack: the folded query text and KMP prefix table, plus the
 * streaming chunk / history ring / snippet window. The scan itself lives in
 * search_terms_core.c and is shared with the index backend.
 *
 * The 0027 filtered query adds a bounded metadata scratch area: the current
 * file's parsed info, a caller-owned parse workspace, one header prefix window
 * and the per-file eligibility flags. It deliberately does NOT hold the whole
 * 16 KiB service candidate: a filtered query only observes headers, so the pure
 * parser plus yan_fs_read is enough. The scratch is always present and is
 * cleared at the start of each filtered query; no metadata is cached across
 * queries.
 *
 * Honest status: the backend has no cache, so a mounted source reports EMPTY
 * with zero terms/postings; an unmounted, faulted or uninitialized source
 * reports UNAVAILABLE with the observed source class. A filesystem BUSY is
 * propagated as TERMS_BUSY with the status holder untouched; it is never
 * collapsed into UNINITIALIZED. `clear` is a successful no-op and `rebuild`
 * returns UNSUPPORTED because there is no index to build. `query_filtered`
 * implements the 0027 qualification pass over this scratch and is shared with
 * the embedded index backend; the old plain `query` path is unchanged. */

typedef struct {
    /* Borrowed for the whole query: mounted, not modified and not reused by
     * another task until the query returns. Each yan_fs_* call owns the
     * filesystem busy guard. */
    YanFs *fs;
    bool initialized;
    bool busy;
    /* Current snippet exposed to the synchronous snippet reader. */
    const uint8_t *snippet;
    uint32_t snippet_length;
    /* Shared folded query text and KMP prefix tables (total query <= 1023). */
    uint8_t folded[YAN_SEARCH_TERMS_QUERY_MAX];
    uint16_t prefix[YAN_SEARCH_TERMS_QUERY_MAX];
    /* Streaming chunk, word-property history ring and snippet window. */
    YanSearchTermsScratch scratch;
    /* Fixed top-20 selection, kept here so the query frame stays small. */
    YanSearchTermsRank rank;
    /* 0027 filtered-query scratch: header prefix window, parsed info and its
     * workspace, and the per-file eligibility set. Cleared per filtered query. */
    uint8_t metadata_prefix[YAN_NOTE_HEADER_MAX];
    YanNoteInfo metadata_info;
    YanNoteParseWorkspace metadata_workspace;
    uint8_t metadata_eligible[YAN_FS_MAX_FILES];
    uint32_t metadata_skipped;
    /* True only while a filtered query is running: the SCAN and INDEX paths use
     * it to consult the eligibility set instead of searching every slot. It is
     * cleared on every exit of a public filtered query, so a later plain query
     * takes the original warm path. */
    bool metadata_filtered;
} YanSearchTermsLinear;

/* Validates `linear` and `fs`, rejects a pair that overlaps or a context that is
 * still serving a query, then records the borrowed filesystem. No filesystem
 * I/O. */
YanSearchTermsResult yan_search_terms_linear_init(YanSearchTermsLinear *,
                                                  YanFs *);

/* Backend vtable entry points; `context` is the YanSearchTermsLinear. */
YanSearchTermsResult yan_search_terms_linear_query(
    void *, const uint8_t *, uint32_t, YanSearchTermsMatchFn, void *,
    YanSearchTermsSummary *);
/* 0027 filtered entry point. It performs the whole-directory qualification pass
 * and then the shared streaming scan, using the linear context's own header
 * scratch. The facade maps its result directly. */
YanSearchTermsResult yan_search_terms_linear_query_filtered(
    void *, const uint8_t *, uint32_t, const YanNoteTagFilter *,
    YanSearchTermsMatchFn, void *, YanSearchTermsSummary *);

/* 0027 qualification helper, exported for the index backend's internal seam
 * only; it is not part of the consumer API.
 *
 * It walks the single-root directory in physical slot order and fills
 * linear->metadata_eligible[slot] with one of:
 *   YAN_NOTE_META_INVALID  a binary/NUL file, skipped once
 *   YAN_NOTE_META_TAG      valid text without the tags
 *   YAN_NOTE_META_ELIGIBLE valid text with all the tags
 *
 * Every file is preflighted for the whole body first (shared core rule), so a
 * binary/NUL file never reaches the header parser and is never METADATA. A
 * valid text file is then read for at most YAN_NOTE_HEADER_MAX prefix bytes and
 * parsed with the pure format parser: a matched prefix with a broken header is
 * YAN_SEARCH_TERMS_METADATA for the whole query, even when the file could not
 * have matched the keywords or the tags. A header-less file has empty tags.
 *
 * It performs no ranking, no callback and no facade call; it reads no body byte
 * past the prefix once preflight has accepted the file. `linear->metadata_skipped`
 * ends up as the count of invalid binary/NUL files. */
YanSearchTermsResult yan_search_terms_linear_qualify(
    YanSearchTermsLinear *linear, const YanNoteTagFilter *filter);

/* Defensive filter validation shared with the embedded index backend. The
 * facade validates a caller's filter before either backend sees it; a direct
 * call to an exported backend entry point must not trust that, so this rejects
 * an absent filter, an out-of-range count, a malformed tag and an overlap with
 * the linear context or its borrowed filesystem. */
bool yan_search_terms_linear_filter_valid(const YanSearchTermsLinear *linear,
                                          const YanNoteTagFilter *filter);
YanSearchTermsResult yan_search_terms_linear_read_match(
    void *, uint32_t, uint32_t, const uint8_t **, uint32_t *);
YanSearchTermsResult yan_search_terms_linear_status(void *,
                                                    YanSearchTermsStatus *);
YanSearchTermsResult yan_search_terms_linear_clear(void *);
YanSearchTermsResult yan_search_terms_linear_rebuild(void *);

/* Builds the vtable with `linear` as the backend context and the borrowed
 * filesystem as the source span (declaration only; the facade never
 * dereferences it). The caller must pass a non-NULL, initialized context. */
YanSearchTermsBackend yan_search_terms_linear_backend(YanSearchTermsLinear *);

#endif
