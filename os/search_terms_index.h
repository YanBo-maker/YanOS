#ifndef YAN_OS_SEARCH_TERMS_INDEX_H
#define YAN_OS_SEARCH_TERMS_INDEX_H

#include "search_terms.h"
#include "search_terms_linear.h"
#include "yanfs.h"

#include <stdbool.h>
#include <stdint.h>

/* In-memory term index for 0026.
 *
 * The behavioural contract is docs/specs/0026-term-search-index.md, "索引保存
 * 在哪里" and "修改与索引有效性". The index lives in Guest static memory, does
 * not occupy a directory slot, writes no disk and adds no block-protocol or
 * database dependency. Rebuilding it is a pure RAM read of the borrowed YanFs.
 *
 * The keys are the source's atoms: every maximal ASCII word folded to lower
 * case, and every non-ASCII UTF-8 scalar kept raw. A query group is decomposed
 * into the same atoms; complete-word keys make the ASCII word boundaries
 * implicit, and byte adjacency (prev_offset + prev_length == next_offset in the
 * same slot/line) makes a multi-atom group contiguous. Keys are stored in the
 * key pool and found through an open-addressed hash table that compares the raw
 * canonical bytes, so a hash collision cannot alias two keys.
 *
 * Fixed tables (approved caps, four uint32 fields per posting):
 *
 *   keys      8192 * 20B  key_offset/key_length/head/tail/count
 *   key_pool  262144B     raw canonical key bytes (no NUL charged)
 *   postings  65536 * 16B next/slot/line/file_byte_offset
 *   hash      16384 * 4B  open-addressing slots
 *
 * The four tables total 1540096 bytes. key.count is the real uint32 occurrence
 * count (at most the posting capacity); the per-line score caps each group at
 * 255, never the stored count. Per-query atom keys/cursors verify identity
 * through each key's posting list and exact byte adjacency. The 63-entry
 * directory snapshot, the rank table and the complete linear fallback backend
 * are bounded derived scratch. The whole context stays under the 2 MiB static
 * budget and is caller-owned; it must never live on the 4 KiB task stack. The
 * numbers below are design-time sizeof/static_assert values, not a Host or RV32
 * measured stack/size report. */

#define YAN_SEARCH_TERMS_INDEX_KEYS UINT32_C(8192)
#define YAN_SEARCH_TERMS_INDEX_KEY_POOL UINT32_C(262144)
#define YAN_SEARCH_TERMS_INDEX_POSTINGS UINT32_C(65536)
#define YAN_SEARCH_TERMS_INDEX_HASH_SLOTS UINT32_C(16384)

/* Five uint32 fields. head/tail are the posting-list bounds, count the real
 * complete occurrence count, key_offset/key_length the raw key in key_pool. */
typedef struct {
    uint32_t key_offset;
    uint32_t key_length;
    uint32_t head;
    uint32_t tail;
    uint32_t count;
} YanSearchTermsIndexKey;

/* Four uint32 fields, exactly the layout 0026 fixes. next links the posting
 * list, slot/line identify the physical directory slot and 1-based line, and
 * file_byte_offset anchors the snippet window. */
typedef struct {
    uint32_t next;
    uint32_t slot;
    uint32_t line;
    uint32_t file_byte_offset;
} YanSearchTermsIndexPosting;

/* Caller-owned, long-lived static storage. A caller zero-initializes it before
 * yan_search_terms_index_init and never edits the fields afterwards. */
typedef struct {
    YanFs *fs;
    bool initialized;
    bool busy;
    YanSearchTermsIndexState state;
    /* Source identity the current cache was built against. token_valid is false
     * while the cache is empty or was never published. */
    uint64_t token;
    bool token_valid;
    uint32_t term_count;
    uint32_t posting_count;

    /* Fixed physical tables. */
    YanSearchTermsIndexKey keys[YAN_SEARCH_TERMS_INDEX_KEYS];
    uint8_t key_pool[YAN_SEARCH_TERMS_INDEX_KEY_POOL];
    YanSearchTermsIndexPosting postings[YAN_SEARCH_TERMS_INDEX_POSTINGS];
    uint32_t hash[YAN_SEARCH_TERMS_INDEX_HASH_SLOTS];

    /* Derived, RAM-only bounded scratch. */
    uint32_t atom_key[YAN_SEARCH_TERMS_QUERY_MAX];
    uint32_t atom_cursor[YAN_SEARCH_TERMS_QUERY_MAX];
    uint16_t group_atom_start[YAN_SEARCH_TERMS_GROUP_MAX];
    uint16_t group_atom_count[YAN_SEARCH_TERMS_GROUP_MAX];
    YanFsInfo entries[YAN_FS_MAX_FILES];
    uint32_t key_pool_used;
    uint32_t skipped;

    /* Complete linear fallback backend, embedded so the index owns its SCAN
     * path and the caller keeps one long-lived object. */
    YanSearchTermsLinear linear;
} YanSearchTermsIndex;

_Static_assert(sizeof(YanSearchTermsIndexKey) == 20u,
               "index key must stay five uint32 fields");
_Static_assert(sizeof(YanSearchTermsIndexPosting) == 16u,
               "index posting must stay four uint32 fields");
_Static_assert(YAN_SEARCH_TERMS_INDEX_KEYS * 20u +
                       YAN_SEARCH_TERMS_INDEX_KEY_POOL +
                       YAN_SEARCH_TERMS_INDEX_POSTINGS * 16u +
                       YAN_SEARCH_TERMS_INDEX_HASH_SLOTS * 4u ==
                   UINT32_C(1540096),
               "the four fixed tables must total 1540096 bytes");
_Static_assert(sizeof(YanSearchTermsIndex) <= 2u * 1024u * 1024u,
               "the whole index context must fit the 2 MiB static budget");

/* Validates index and fs, rejects a pair that overlaps or a context that is
 * still serving a query, then records the borrowed filesystem and prepares the
 * embedded linear fallback. It publishes EMPTY: no cache exists yet. No disk
 * I/O. */
YanSearchTermsResult yan_search_terms_index_init(YanSearchTermsIndex *index,
                                                 YanFs *fs);

/* Builds the vtable that the caller injects into a YanSearchTerms. Every entry
 * point operates on the index context; the source span is the borrowed YanFs.
 *
 *   query      uses READY key/postings, or falls back to a complete linear SCAN
 *              when the source is uncacheable, LIMIT, or a rebuild is needed
 *   read_match serves the current snippet from the linear reader span
 *   status     real FS source state/token/cacheable; READY reports full counts,
 *              every other state reports 0, FS BUSY leaves the holder untouched
 *   clear      zero-I/O cache drop; never revives the source
 *   rebuild    full RAM rebuild; READY on success, INDEX_LIMIT on capacity, or
 *              the mapped source error
 *
 * The caller must pass a non-NULL, initialized context. */
YanSearchTermsBackend yan_search_terms_index_backend(YanSearchTermsIndex *index);

#endif
