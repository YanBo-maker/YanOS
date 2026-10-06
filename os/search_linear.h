#ifndef YAN_OS_SEARCH_LINEAR_H
#define YAN_OS_SEARCH_LINEAR_H

#include "search.h"
#include "yanfs.h"

#include <stdint.h>

/* No-index linear search backend for 0025.
 *
 * This is the only search header that includes yanfs.h: the backend borrows a
 * caller-owned YanFs for the whole query, but the Search facade and every
 * consumer stay independent of the filesystem. The production application
 * creates a YanSearchLinear in long-lived storage, initializes it against a
 * mounted filesystem, and injects the matching vtable into a YanSearch.
 *
 * The context is deliberately a few fixed buffers, never a file-sized
 * allocation and never a large automatic frame:
 *
 *   scan          4096 bytes  sequential preflight / scan chunk
 *   match_reader  4096 bytes  matched-range reader, separate so a read of the
 *                             current match cannot clobber an unread scan chunk
 *   prefix        1023 * 2    KMP prefix table for the current pattern
 *
 * plus small query state, about 10 KiB. The pattern is borrowed for the whole
 * query and is not copied. The exact sizeof is a build-time measurement; this
 * comment records the buffer budget, not a hard limit. The whole structure is
 * caller-owned, so the 4 KiB task stack of 0019 is never involved. */

#define YAN_SEARCH_LINEAR_SCAN_SIZE UINT32_C(4096)
#define YAN_SEARCH_LINEAR_PREFIX_MAX YAN_SEARCH_PATTERN_MAX

typedef struct {
    /* Borrowed for the whole query: mounted, not modified, not reinitialized and
     * not used by another task until the query returns. The backend does not
     * hold fs->busy across the query; each yan_fs_* call owns that guard. */
    YanFs *fs;
    /* Own object state: a query rejects a reentrant call and the initializer
     * rejects a context that is still serving another Search. */
    bool initialized;
    bool busy;
    /* Borrowed pattern and its precomputed KMP table. */
    const uint8_t *pattern;
    uint32_t pattern_length;
    uint16_t prefix[YAN_SEARCH_LINEAR_PREFIX_MAX];
    /* Streaming scan state. */
    uint32_t line_number;
    uint32_t line_start;
    uint32_t line_content;
    uint32_t kmp;
    bool line_has_match;
    bool pending_cr;
    bool pending_new_line;
    /* Current match exposed to the matched-range reader. */
    bool serving;
    char match_name[32];
    uint32_t match_start;
    uint32_t match_length;
    /* Fixed streaming buffers. */
    uint8_t scan[YAN_SEARCH_LINEAR_SCAN_SIZE];
    uint8_t match_reader[YAN_SEARCH_LINEAR_SCAN_SIZE];
} YanSearchLinear;

/* Validates `linear` and `fs`, rejects a context that overlaps the borrowed
 * filesystem or is still serving a query, then clears the context and records
 * the borrowed filesystem. No filesystem I/O. Returns YAN_SEARCH_INVALID for a
 * null argument or an aliased pair, YAN_SEARCH_BUSY while a query is in flight.
 * Every alias decision is pure address arithmetic before the first memset. */
YanSearchResult yan_search_linear_init(YanSearchLinear *, YanFs *);

/* Backend vtable entry points. `context` is the YanSearchLinear.
 *
 * The mapping from the filesystem result to the unified class, which only this
 * backend performs:
 *
 *   YAN_FS_OK           -> YAN_SEARCH_OK
 *   YAN_FS_END          -> YAN_SEARCH_OK          (end of directory listing)
 *   YAN_FS_INVALID      -> YAN_SEARCH_INVALID
 *   YAN_FS_BUSY         -> YAN_SEARCH_BUSY
 *   YAN_FS_NOT_MOUNTED  -> YAN_SEARCH_NOT_MOUNTED
 *   YAN_FS_FAULTED      -> YAN_SEARCH_FAULTED
 *   YAN_FS_NOT_FOUND    -> YAN_SEARCH_NOT_FOUND
 *   YAN_FS_IO           -> YAN_SEARCH_IO
 *   YAN_FS_PROTOCOL     -> YAN_SEARCH_PROTOCOL
 *   YAN_FS_CORRUPT      -> YAN_SEARCH_CORRUPT
 *   YAN_FS_UNSUPPORTED  -> YAN_SEARCH_UNSUPPORTED
 *   any other value     -> YAN_SEARCH_PROTOCOL    (inconsistent directory)
 *
 * The source classes match the shell's mapping, so the front end can keep one
 * ERROR-token and fatal mapping instead of collapsing an image failure into
 * zero matches. */
YanSearchResult yan_search_linear_query(void *, const uint8_t *, uint32_t,
                                        YanSearchMatchFn, void *);
YanSearchResult yan_search_linear_read_match(void *, uint32_t, uint32_t,
                                             const uint8_t **, uint32_t *);

/* Builds the vtable that points at the two entry points above with `linear` as
 * the backend context. The caller must pass a non-NULL, initialized context.
 *
 * The vtable also declares the two protected spans the facade needs but cannot
 * derive itself: context_size is sizeof(YanSearchLinear) over the whole fixed
 * context, and the source span is the borrowed YanFs recorded by
 * yan_search_linear_init. Both are declaration-only; the facade never
 * dereferences them. */
YanSearchBackend yan_search_linear_backend(YanSearchLinear *);

#endif
