#ifndef YAN_HOST_SEMANTIC_CORPUS_H
#define YAN_HOST_SEMANTIC_CORPUS_H

#include "semantic_sha256.h"
#include "semantic_text.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define YAN_SEMANTIC_SEGMENT_MAX UINT32_C(8192)
/* 0036-R6: the hard directory capacity ceiling. A source's file_capacity is
 * the logical image's runtime N: 63 for v1, 64*D-1 for v2 (D=1..64). */
#define YAN_SEMANTIC_CAPACITY_MAX UINT32_C(4095)

/* Source borrows a stable logical YanFS view. next uses the physical directory
 * cursor and returns MISSING at EOF; read must fill exactly the requested span.
 * Adapters own file descriptors and close errors; the facade closes before any
 * result sink or index publication. No callback writes source bytes. */
typedef struct {
    char name[32];
    uint32_t slot;
    uint32_t size;
} YanSemanticFile;

typedef struct {
    void *context;
    size_t context_size;
    YanSemanticResult (*next)(void *, uint32_t *cursor, YanSemanticFile *);
    YanSemanticResult (*read)(void *, const char *name, uint32_t offset,
                              uint8_t *out, uint32_t length);
    /* Runtime directory capacity N of this logical image: 63 for a v1 image,
     * 64*D-1 for a v2 image, at most YAN_SEMANTIC_CAPACITY_MAX. Every source
     * sets it explicitly after a successful mount; 0 is invalid, never a
     * shorthand for 63. */
    uint32_t file_capacity;
} YanSemanticSource;

typedef struct {
    char name[32];
    uint32_t start, end, first_line, last_line, scalar_count;
} YanSemanticProvenance;

typedef struct {
    YanSemanticFile file;
    bool eligible;
    uint32_t body_offset, body_line;
    uint16_t title_length;
    uint8_t title[257];
} YanSemanticCorpusFile;

/* Caller-owned bounded workspace, never Guest stack storage. `files` is
 * calloc'd for the source's runtime file_capacity by corpus_scan and stays
 * owned by this struct until yan_semantic_corpus_release; it is NULL before
 * the first scan and after release. A failed scan leaves counts/hash
 * unpublished; only OK makes them usable. Entries and segments are canonical
 * ASCII-name/byte-offset order. */
typedef struct {
    YanSemanticCorpusFile *files;
    uint32_t file_count, segment_count, skipped;
    uint8_t source_hash[32];
    YanSemanticProvenance segments[YAN_SEMANTIC_SEGMENT_MAX];
    YanSemanticText validator;
    YanSemanticChunker chunker;
    YanSemanticSha256 hasher;
    uint8_t bytes[4096];
} YanSemanticCorpus;

YanSemanticResult yan_semantic_corpus_scan(YanSemanticCorpus *,
                                           const YanSemanticSource *);

/* Frees the capacity-sized `files` table and clears the counts. Safe on an
 * all-zero or already-released struct, so every rebuild/status/search cleanup
 * path and test tearDown can call it unconditionally. */
void yan_semantic_corpus_release(YanSemanticCorpus *);

/* 0033 pure source identity. Enumerates the same logical view and rejects the
 * same cursor/slot/name/duplicate violations as corpus_scan, sorts by ASCII
 * name, then hashes for each file: slot u32LE, name length u32LE, name bytes,
 * file size u32LE, and every raw file byte. It performs no metadata parse, no
 * text validation and no segmentation, so it is not subject to the
 * 8192-segment cap and a bad unrelated header cannot fail it. It still
 * enforces the logical view itself: files in slots below the source's runtime
 * file_capacity (at most file_capacity files), a valid NUL-terminated note
 * name, strictly increasing cursors and no duplicate slot/name, so one file
 * past that capacity is CORRUPT. A capacity of 0 or above
 * YAN_SEMANTIC_CAPACITY_MAX is INVALID. Binary files contribute their bytes
 * unchanged.
 * source must provide next/read and a context span; out belongs to the caller
 * and must be a 32-byte span disjoint from the source struct and its context.
 * Invalid or failing sources return their real first error. out is published
 * only after every read and the SHA-256 final succeed; every failure leaves
 * all 32 bytes byte-identical. This function borrows no store, index, model or
 * runner; its capacity-sized temporary table is freed before every return. */
YanSemanticResult yan_semantic_source_digest(const YanSemanticSource *,
                                             uint8_t out[32]);

#endif
