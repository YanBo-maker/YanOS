#ifndef YAN_OS_SEARCH_TERMS_LINEAR_H
#define YAN_OS_SEARCH_TERMS_LINEAR_H

#include "search_terms.h"
#include "search_terms_core.h"
#include "yanfs.h"

#include <stdint.h>

/* No-index linear term backend for 0026.
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
 * Honest status: the backend has no cache, so a mounted source reports EMPTY
 * with zero terms/postings; an unmounted, faulted or uninitialized source
 * reports UNAVAILABLE with the observed source class. A filesystem BUSY is
 * propagated as TERMS_BUSY with the status holder untouched; it is never
 * collapsed into UNINITIALIZED. `clear` is a successful no-op and `rebuild`
 * returns UNSUPPORTED because there is no index to build. */

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
