#ifndef YAN_HOST_SEMANTIC_CORPUS_H
#define YAN_HOST_SEMANTIC_CORPUS_H

#include "semantic_sha256.h"
#include "semantic_text.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define YAN_SEMANTIC_SEGMENT_MAX UINT32_C(8192)
#define YAN_SEMANTIC_FILE_MAX UINT32_C(63)

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

/* Caller-owned bounded workspace (~0.5 MiB), never Guest stack storage.
 * A failed scan leaves the workspace unpublished; only OK makes counts/hash
 * usable. Entries and segments are canonical ASCII-name/byte-offset order. */
typedef struct {
    YanSemanticCorpusFile files[YAN_SEMANTIC_FILE_MAX];
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

/* 0033 pure source identity. Enumerates the same logical view and rejects the
 * same cursor/slot/name/duplicate violations as corpus_scan, sorts by ASCII
 * name, then hashes for each file: slot u32LE, name length u32LE, name bytes,
 * file size u32LE, and every raw file byte. It performs no metadata parse, no
 * text validation and no segmentation, so it is not subject to the
 * 8192-segment cap and a bad unrelated header cannot fail it. It still
 * enforces the logical view itself: files in slots 0..62 (at most 63 files), a
 * valid NUL-terminated note name, strictly increasing cursors and no duplicate
 * slot/name, so a 64th file is CORRUPT. Binary files contribute their bytes
 * unchanged.
 * source must provide next/read and a context span; out belongs to the caller
 * and must be a 32-byte span disjoint from the source struct and its context.
 * Invalid or failing sources return their real first error. out is published
 * only after every read and the SHA-256 final succeed; every failure leaves
 * all 32 bytes byte-identical. This function allocates nothing and borrows no
 * store, index, model or runner. */
YanSemanticResult yan_semantic_source_digest(const YanSemanticSource *,
                                             uint8_t out[32]);

#endif
