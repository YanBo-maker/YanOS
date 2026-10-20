#ifndef YAN_HOST_BLOCK_QUERY_H
#define YAN_HOST_BLOCK_QUERY_H

#include "note_blocks.h"
#include "semantic_posix.h"

#include <signal.h>

/* 0033 Host block navigation and paging. This module is the C seam between the
 * read-only image adapter (Task A) and the read-only Guest block service
 * (os/note_blocks); it contains no MCP schema, worker protocol or JSON.
 *
 * A YanBlockQuery is caller-owned, must be zero-initialized before its first
 * init, and is far too large for the Guest's 4 KiB task stack (it carries the
 * service workspace and a 64 KiB + 4 byte page buffer). init borrows a mounted
 * image-only YanSemanticPosix; it never takes ownership of the adapter, never
 * opens or closes descriptors, and never calls a store callback. The caller
 * owns adapter cleanup and may replace only an otherwise-OK result with its
 * close error. A manually constructed adapter whose YanFs is initialized and
 * mounted with a read-only YanFsBlockIo is accepted; image_fd is not required,
 * which lets native fault tests drive the same observation path.
 *
 * Every operation follows one order: validate name/ID/out/expected without
 * source I/O; check the optional cancellation flag; observation A (unmount +
 * mount so the directory block is really re-read, then the cancellation-wrapped
 * logical source digest); compare an optional expected digest (STALE before any
 * block work); run the existing service into bounded scratch through a
 * temporary cancellation-aware block-IO wrapper installed around that call only
 * (so preflight/target reads, not only callbacks, stop at the next physical
 * read); after the service check cancellation first (CANCELLED wins), then
 * map any service error with no post observation; then
 * observation B (remount + digest) and compare pre/post (STALE); only after all
 * of that is out published. Consequently every failure leaves out
 * byte-identical. The image must otherwise remain stable for the call; A/B
 * observation is change detection, not a snapshot or transaction.
 *
 * Result tokens are stable and distinct from every other result enum; mappings
 * are explicit switches, never numeric casts. CANCELLED wins over STOPPED only
 * when the caller's flag is set. LIMIT remains LIMIT. */
typedef enum {
    YAN_BLOCK_QUERY_OK = 0,
    YAN_BLOCK_QUERY_INVALID,
    YAN_BLOCK_QUERY_BUSY,
    YAN_BLOCK_QUERY_STOPPED,
    YAN_BLOCK_QUERY_CANCELLED,
    YAN_BLOCK_QUERY_STALE,
    YAN_BLOCK_QUERY_METADATA,
    YAN_BLOCK_QUERY_BLOCK_FORMAT,
    YAN_BLOCK_QUERY_AMBIGUOUS,
    YAN_BLOCK_QUERY_LIMIT,
    YAN_BLOCK_QUERY_INVALID_TEXT,
    YAN_BLOCK_QUERY_NOT_MOUNTED,
    YAN_BLOCK_QUERY_FAULTED,
    YAN_BLOCK_QUERY_NOT_FOUND,
    YAN_BLOCK_QUERY_IO,
    YAN_BLOCK_QUERY_PROTOCOL,
    YAN_BLOCK_QUERY_CORRUPT,
    YAN_BLOCK_QUERY_UNSUPPORTED
} YanBlockQueryResult;

/* The page scratch is 65536 + 4 bytes so one boundary byte and up to three
 * UTF-8 rollback bytes can be inspected without a second read. */
typedef struct {
    YanSemanticPosix *adapter; /* borrowed mounted image-only adapter */
    YanSemanticSource source;  /* bound once; reads the current adapter fs */
    YanBlockContext blocks;    /* 64 KiB service workspace */
    /* 0036-R3: names table for the block service, calloc'd for the mounted
     * image's entry_capacity and released by yan_block_query_destroy. */
    char (*block_names)[32];
    /* 0036-R8: index scratch for the block service's shared heapsort. */
    uint16_t *block_sort;
    uint8_t page[65536 + 4];   /* bounded stream/page scratch */
    const volatile sig_atomic_t *cancel; /* optional, borrowed */
    /* Set only by a successful operation: the pre/post-verified logical source
     * digest that C must copy into the response's source_sha256. It is
     * unspecified after a failed operation and must be copied before the next
     * call (a successful call overwrites it). */
    uint8_t source_sha256[32];
    bool initialized;
} YanBlockQuery;

typedef struct {
    uint64_t total;
    uint32_t shown;
    YanBlockDecl blocks[YAN_BLOCK_MAX_COUNT];
} YanBlockQueryList;

typedef struct {
    uint64_t total;
    uint32_t shown;
    YanBlockRow rows[YAN_BLOCK_REFS_SHOWN];
} YanBlockQueryRefs;

typedef struct {
    uint64_t total;
    uint32_t shown;
    uint32_t skipped;
    YanBlockTargetState target_state;
    YanBlockRow rows[YAN_BLOCK_REFS_SHOWN];
} YanBlockQueryBacklinks;

/* All byte offsets are absolute half-open YanFS positions. block_* describe
 * the whole block; page_* the returned page. text holds original bytes (no
 * CRLF normalization). Empty page: page_start == page_end, first/last 0.
 * complete means page_end == block_end; next_offset is Q-block_start and is
 * meaningful only when !complete. text_length is 0..65536. */
typedef struct {
    uint32_t block_start, block_end, block_first_line, block_last_line;
    uint32_t page_start, page_end, page_first_line, page_last_line;
    uint8_t text[65536];
    uint32_t text_length;
    bool complete;
    uint32_t next_offset;
} YanBlockQueryRead;

/* init performs no I/O and does not bind a store. It validates the query and
 * adapter spans and their non-overlap, requires the adapter's YanFs to be
 * initialized and mounted, binds the read source and calls yan_blocks_init.
 * It copies no name or ID; each operation validates and copies its own name/ID
 * after the context is initialized. */
/* Releases the 0036-R3 names allocation and clears the owned pointer. Safe on a
 * zero-initialized object and on a repeated call. */
void yan_block_query_destroy(YanBlockQuery *);

YanBlockQueryResult yan_block_query_init(YanBlockQuery *,
    YanSemanticPosix *, const volatile sig_atomic_t *cancel);
YanBlockQueryResult yan_block_query_list(YanBlockQuery *, const char *name,
    const uint8_t expected[32], YanBlockQueryList *);
/* offset is relative to block_start, 0..UINT32_MAX. offset > length is
 * INVALID; offset == length is the permitted empty terminal page. offset > 0
 * still works without expected here; the MCP layer decides whether to require
 * expected_source for continuation requests. */
YanBlockQueryResult yan_block_query_read(YanBlockQuery *, const char *name,
    const char *id, uint32_t offset, const uint8_t expected[32],
    YanBlockQueryRead *);
YanBlockQueryResult yan_block_query_refs(YanBlockQuery *, const char *name,
    const uint8_t expected[32], YanBlockQueryRefs *);
/* A missing target file is allowed for backlinks; the service reports the
 * target state and skipped non-text files. */
YanBlockQueryResult yan_block_query_backlinks(YanBlockQuery *, const char *name,
    const char *id, const uint8_t expected[32], YanBlockQueryBacklinks *);

const char *yan_block_query_result_name(YanBlockQueryResult);

#endif
