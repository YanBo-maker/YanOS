#ifndef YAN_OS_SEARCH_TERMS_CORE_H
#define YAN_OS_SEARCH_TERMS_CORE_H

#include "search_terms.h"

#include <stdbool.h>
#include <stdint.h>

/* Shared term-search core for 0026.
 *
 * The pure query grammar, the folded-pattern/KMP preparation, the streaming
 * scan and the snippet window live here so the no-index linear backend and the
 * in-memory index backend cannot drift apart. The core performs no I/O of
 * its own: the caller passes a `YanSearchTermsSourceReadFn` that returns file
 * bytes.
 *
 * Memory: every large object is caller-owned. The parser result is a tiny
 * offset/length table (100 bytes on Host/RV32), the folded text and KMP prefix tables
 * live in the backend's context, and the scan chunk / history ring / snippet
 * window are supplied as `YanSearchTermsScratch`. No entry point allocates from
 * the file size and no whole-file or whole-line buffer is ever needed. */

#define YAN_SEARCH_TERMS_CHUNK UINT32_C(4096)
#define YAN_SEARCH_TERMS_RING UINT32_C(1024)
#define YAN_SEARCH_TERMS_SNIPPET_SCRATCH UINT32_C(4096)
#define YAN_SEARCH_TERMS_SNIPPET_LEFT UINT32_C(40)
#define YAN_SEARCH_TERMS_SNIPPET_TOTAL UINT32_C(160)

/* Lightweight pure-grammar result. Offsets and lengths index the borrowed raw
 * query; the flags say whether the group's first/last byte is an ASCII word
 * character and therefore needs the outside word boundary checked. */
typedef struct {
    uint32_t count;
    uint16_t offset[YAN_SEARCH_TERMS_GROUP_MAX];
    uint16_t length[YAN_SEARCH_TERMS_GROUP_MAX];
    bool left_word[YAN_SEARCH_TERMS_GROUP_MAX];
    bool right_word[YAN_SEARCH_TERMS_GROUP_MAX];
} YanSearchTermsQuery;

/* Parses and validates one query. It rejects an empty query, a query longer
 * than YAN_SEARCH_TERMS_QUERY_MAX, NUL/LF/CR/any ASCII control except TAB and
 * DEL, invalid UTF-8, an ASCII word atom longer than YAN_SEARCH_TERMS_WORD_MAX,
 * and more than YAN_SEARCH_TERMS_GROUP_MAX distinct groups after ASCII-folded
 * canonical de-duplication. Repeated canonical groups are free. It touches no
 * filesystem and no I/O. */
YanSearchTermsResult yan_search_terms_parse(const uint8_t *query,
                                            uint32_t query_length,
                                            YanSearchTermsQuery *out);

/* Folded group patterns plus the shared KMP prefix table. folded and prefix are
 * borrowed caller buffers of at least the total group byte length; the group
 * patterns are concatenated, and prefix[fold_start[g] + j] is the KMP prefix
 * entry for group g's byte j. */
typedef struct {
    uint32_t count;
    uint16_t length[YAN_SEARCH_TERMS_GROUP_MAX];
    uint16_t fold_start[YAN_SEARCH_TERMS_GROUP_MAX];
    bool left_word[YAN_SEARCH_TERMS_GROUP_MAX];
    bool right_word[YAN_SEARCH_TERMS_GROUP_MAX];
    const uint8_t *folded;
    const uint16_t *prefix;
} YanSearchTermsPattern;

/* Folds the parsed groups, builds the KMP prefix tables and fills out. Returns
 * INVALID when the caller buffers cannot hold the whole folded query. */
YanSearchTermsResult yan_search_terms_pattern_build(
    const uint8_t *query, const YanSearchTermsQuery *parsed,
    uint8_t *folded, uint32_t folded_capacity,
    uint16_t *prefix, uint32_t prefix_capacity,
    YanSearchTermsPattern *out);

/* Pulls up to `capacity` bytes of one file from `offset` into the caller's
 * `out` buffer. On success reports the count in *length (0 at end of file). A
 * source error is returned as a YanSearchTermsResult. This is the core's own
 * callback type: the public backend reader that borrows `const uint8_t **` is a
 * different typedef in search_terms.h. */
typedef YanSearchTermsResult (*YanSearchTermsSourceReadFn)(
    void *context, uint32_t offset, uint32_t capacity,
    uint8_t *out, uint32_t *length);

/* Whole-file UTF-8/NUL preflight shared with the index build. It reads fixed
 * chunks bounded by file_size and carries an unfinished scalar across reads, so
 * a legal sequence split by a read is accepted while a NUL, an invalid byte or
 * an unfinished tail is reported by setting *skip. A callback that overruns the
 * request or stalls before file_size is PROTOCOL; a real source error is
 * returned unchanged. On OK the file is safe to index.
 *
 * This is an internal core helper exported only so the index build and the
 * linear scan share one preflight; it is not part of the consumer API. */
YanSearchTermsResult yan_search_terms_core_preflight(
    YanSearchTermsSourceReadFn read, void *read_context, uint8_t *chunk,
    uint32_t capacity, uint32_t file_size, bool *skip);

/* One ranked line. `anchor` is the absolute file offset of the line's earliest
 * complete group match; the snippet is re-read around it. */
typedef struct {
    char name[32];
    uint32_t slot;
    uint32_t line_number;
    uint32_t file_size;
    uint32_t anchor;
    uint16_t score;
} YanSearchTermsCandidate;

/* Fixed top-20 selection plus the whole-database counters. The candidates are
 * kept in ascending rank order (score desc, slot asc, line asc) as lines are
 * scanned in directory order. */
typedef struct {
    uint32_t count;
    uint64_t total;
    uint32_t skipped;
    YanSearchTermsCandidate candidate[YAN_SEARCH_TERMS_SHOWN_MAX];
} YanSearchTermsRank;

void yan_search_terms_rank_init(YanSearchTermsRank *rank);

/* Adds one qualifying line; increments total and keeps only the best 20. */
void yan_search_terms_rank_add(YanSearchTermsRank *rank, const char *name,
                               uint32_t slot, uint32_t line_number,
                               uint32_t file_size, uint32_t anchor,
                               uint16_t score);

/* Caller-owned scan buffers. chunk is the streaming read buffer, ring records
 * the ASCII-word property of the recent byte history for KMP boundary checks,
 * snippet is the snippet re-read window. */
typedef struct {
    uint8_t chunk[YAN_SEARCH_TERMS_CHUNK];
    uint8_t ring[YAN_SEARCH_TERMS_RING];
    uint8_t snippet[YAN_SEARCH_TERMS_SNIPPET_SCRATCH];
} YanSearchTermsScratch;

/* Streams one file: whole-file UTF-8/NUL preflight (a bad file increments
 * rank->skipped exactly once and is not scanned), then LF-terminated line
 * scanning with per-group KMP scoring and same-line AND. Only LF ends a line; a
 * bare CR is an ordinary content byte that acts as a word separator, while the
 * CR of a CRLF is part of the terminator and stays out of the snippet. Reads are
 * bounded by file_size, so the core never issues a read at end of file.
 * Qualifying lines are added to rank. Returns a source error from `read`,
 * PROTOCOL for a malformed read (got > request or no progress), or OK. */
YanSearchTermsResult yan_search_terms_core_scan_file(
    const char *name, uint32_t slot, uint32_t file_size,
    YanSearchTermsSourceReadFn read, void *read_context,
    const YanSearchTermsPattern *pattern,
    YanSearchTermsRank *rank, YanSearchTermsScratch *scratch);

/* Builds one snippet for a selected candidate. Reads a fixed window around the
 * anchor, takes at most 40 scalars to the left and 160 in total, stops at a line
 * break/end of file and reports whether either side was clipped. *body points
 * into `scratch` and is valid until the next call. */
YanSearchTermsResult yan_search_terms_core_snippet(
    uint32_t file_size, uint32_t anchor,
    YanSearchTermsSourceReadFn read, void *read_context,
    const uint8_t **body, uint32_t *body_length,
    bool *left_truncated, bool *right_truncated,
    uint8_t *scratch, uint32_t scratch_capacity);

#endif
