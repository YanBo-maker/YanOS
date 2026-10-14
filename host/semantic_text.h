#ifndef YAN_HOST_SEMANTIC_TEXT_H
#define YAN_HOST_SEMANTIC_TEXT_H

#include "semantic_result.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Pure Host text eligibility and deterministic chunking for 0029.
 *
 * The contract is docs/specs/0029-host-semantic-search.md, "只读来源与文本资
 * 格" and "确定分段与出处", plus the fixed Unicode White_Space set of
 * docs/specs/0027-note-metadata.md. This header is the pure seam: it owns the
 * streaming whole-file text validator, the bounded query validator, the
 * White_Space helper and the one-file chunker. It does not know YanFS, the
 * console, the shell, the model or the facade, allocates nothing, and reads
 * only the bytes the caller offers.
 *
 * Whole-file eligibility (streaming):
 *   A file is legal text only when every byte belongs to a scalar that is
 *   valid strict UTF-8, no NUL and no C0 other than TAB (U+0009) and LF
 *   (U+000A), no DEL (U+007F), no C1 (U+0080..U+009F), and every CR
 *   (U+000D) is immediately followed by LF. A body U+FEFF is legal text.
 *   Overlong forms, UTF-16 surrogates, scalars above U+10FFFF, stray
 *   continuation bytes and a sequence truncated at EOF are all invalid.
 *
 *   The validator must not see one byte of a scalar without the rest: the
 *   caller feeds arbitrary byte blocks whose boundaries may fall inside a
 *   multi-byte scalar or between a CR and its LF. The context remembers the
 *   partial scalar and the pending CR, so a legal file split at any byte is
 *   still legal. Violations are sticky: update records the first one and
 *   keeps accepting bytes, because the caller still hashes and reads the
 *   remainder of the file. There is deliberately no full-file buffer; the
 *   context is a few dozen bytes.
 *
 * Query eligibility (bounded one-shot):
 *   A query is 1..1023 UTF-8 bytes, contains no LF, CR, NUL, DEL, C1 or C0
 *   other than TAB, and is not made only of Unicode White_Space. U+FEFF is a
 *   legal query scalar. The validator takes an explicit length, never scans
 *   for NUL and never consults the locale.
 *
 * Chunker:
 *   The chunker walks the body that a whole-file preflight already accepted.
 *   Each emitted chunk is at most 512 body scalars and carries the borrowed
 *   body bytes, the body scalar count, the absolute half-open file byte range
 *   [start, end) and the physical 1-based first/last line. Segments overlap
 *   by 64 scalars. If the 512-scalar end falls between CR and LF, the end
 *   shrinks by one scalar. The next start is end minus 64 scalars; if that
 *   falls between CR and LF, it rolls back one more scalar. The overlap is
 *   therefore 64 or 65 scalars and the minimum forward progress is 446
 *   scalars. The last segment that covers EOF is emitted exactly once and no
 *   overlap-only tail is produced. A segment whose scalars are all Unicode
 *   White_Space is not delivered, but its boundaries still advance. A body
 *   with no scalars (empty file, title-only file) produces no chunks at all.
 *   This module does not synthesize a title prefix and does not enforce the
 *   source-wide 8192-segment cap; both belong to the facade.
 *
 * Ownership and lifecycle:
 *   - The caller owns and zero-initializes both contexts before init.
 *   - init records the immutable configuration and returns OK (INVALID for a
 *     NULL context). init does not start I/O and does not retain the body.
 *   - feed may be called any number of times. The offered bytes are borrowed
 *     only for the duration of the call; the chunker copies the bounded
 *     window it must retain into its own scratch. Bytes of a call that
 *     produces no chunk are not retained beyond that window.
 *   - finish declares EOF, flushes the final covering chunk once and ends the
 *     stream. A later feed or finish is INVALID.
 *   - A sink chunk callback receives borrowed bytes that are valid only for
 *     the duration of that callback; the caller must copy or consume them
 *     synchronously. Returning false stops the chunker with STOPPED; the
 *     stop is sticky and no further chunk is delivered. A NULL sink or a
 *     NULL callback is INVALID before any byte is scanned.
 *   - A malformed UTF-8 sequence in a body is INVALID from feed/finish rather
 *     than a guessed boundary; the facade only calls the chunker after its
 *     preflight accepted the file, so this is a defensive check.
 *   - Detection order is: NULL/uninitialized, BUSY, STOPPED, finished,
 *     sink/span arguments, then mutation. A feed or finish whose byte span
 *     overlaps the context, or whose sink object overlaps the context, is
 *     INVALID and no field is written. A feed/finish/init re-entered from
 *     inside a sink callback returns BUSY and leaves the active state alone;
 *     the outer call owns state until it returns.
 *
 * Tests-first history: the header and tests/test_semantic_text.c existed
 * first and were RED against an UNSUPPORTED/false stub; the current source
 * implements the rules above and that stub no longer exists. */

/* Fixed caps from the 0029 spec. */
#define YAN_SEMANTIC_QUERY_MAX UINT32_C(1023)
#define YAN_SEMANTIC_CHUNK_MAX_SCALARS UINT32_C(512)
#define YAN_SEMANTIC_CHUNK_OVERLAP_SCALARS UINT32_C(64)
/* Bounded retained window: one candidate segment (512 scalars), the 64/65
 * scalar overlap and one scalar of CRLF lookahead fit below 2.5 KiB, so 4 KiB
 * is a safe caller-owned cap. The scalar boundary/flags arrays hold the largest
 * decoded prefix the decision can need (512 + lookahead + margin). */
#define YAN_SEMANTIC_CHUNKER_SCRATCH_SIZE UINT32_C(4096)
#define YAN_SEMANTIC_CHUNKER_MAX_SCALARS UINT32_C(520)

/* True for the fixed Unicode White_Space set of 0027, independent of locale:
 * U+0009..U+000D, U+0020, U+0085, U+00A0, U+1680, U+2000..U+200A, U+2028,
 * U+2029, U+202F, U+205F and U+3000. All other scalars are false. */
bool yan_semantic_white_space(uint32_t scalar);

/* Streaming whole-file text validator. Zero-initialize before init. */
typedef struct {
    /* Partial scalar accumulation, private to semantic_text.c. */
    uint32_t pending_scalar;
    uint32_t pending_min;
    uint32_t continuation_remaining;
    /* Decoded scalars, including White_Space; informational. */
    uint64_t scalar_count;
    /* A CR was seen and its LF has not arrived yet. */
    bool pending_cr;
    /* A violation was recorded and stays recorded. */
    bool invalid;
    /* finish was called once. */
    bool finished;
    /* init was called. */
    bool initialized;
} YanSemanticText;

/* Starts a whole-file preflight. Returns INVALID for a NULL context, else OK.
 * The context is fully reset, so a previous file needs no separate cleanup. */
YanSemanticResult yan_semantic_text_init(YanSemanticText *ctx);

/* Feeds `length` bytes. `bytes` may be NULL only when length is 0. Returns
 * INVALID for a NULL/uninitialized context, for NULL bytes with a nonzero
 * length, for a byte span overlapping the context, or after finish; otherwise
 * OK. A text violation is recorded, not returned, so the caller can continue
 * hashing and reading the file. */
YanSemanticResult yan_semantic_text_update(YanSemanticText *ctx,
                                           const uint8_t *bytes,
                                           size_t length);

/* Declares EOF, checks for a pending CR or a truncated scalar, and returns OK
 * when the whole file is eligible text, else INVALID. Idempotent: a second
 * finish returns the first verdict. */
YanSemanticResult yan_semantic_text_finish(YanSemanticText *ctx);

/* True only after finish when the whole file is eligible text. */
bool yan_semantic_text_is_eligible(const YanSemanticText *ctx);

/* One-shot bounded query validator. Returns true only for a legal query as
 * described above. A NULL pointer is legal only with length 0, which is
 * always false because a query needs at least one byte. */
bool yan_semantic_query_valid(const uint8_t *bytes, size_t length);

/* One delivered body chunk. All fields are borrowed for the synchronous
 * callback that received the chunk. */
typedef struct {
    /* Borrowed body bytes, exactly [start, end) of the original file. */
    const uint8_t *body;
    /* Bytes in this chunk, equal to end - start. */
    uint32_t body_length;
    /* Body scalars in this chunk, 1..512. */
    uint32_t scalar_count;
    /* Absolute file byte offset of the first chunk byte (inclusive). */
    uint32_t start;
    /* Absolute file byte offset past the last chunk byte (exclusive). */
    uint32_t end;
    /* Physical 1-based line of the byte at start. */
    uint32_t first_line;
    /* Physical 1-based line of the byte at end - 1. A trailing LF belongs to
     * the line it ends. */
    uint32_t last_line;
} YanSemanticChunk;

/* Synchronous sink. Returning false stops the chunker with STOPPED. */
typedef bool (*YanSemanticChunkFn)(void *context,
                                   const YanSemanticChunk *chunk);

typedef struct {
    void *context;
    YanSemanticChunkFn chunk;
} YanSemanticChunkSink;

/* Streaming one-file chunker. Zero-initialize before init. The context is
 * caller-owned and bounded; its fields are private to semantic_text.c.
 *
 * scratch[0..window_length) holds the retained body bytes starting at
 * body-relative window_start. Bytes [0, scan_pos) belong to the
 * scalar_count decoded scalars recorded in scalar_offset/scalar_flags; the
 * tail [scan_pos, window_length) is an at most three-byte incomplete scalar
 * prefix waiting for the next feed. After an emitted (or skipped) segment the
 * prefix before the next start is dropped, so scalar index 0 is always the
 * start of the open segment. window_line is the physical line at
 * window_start. */
typedef struct {
    /* Immutable configuration, set by init. */
    uint32_t body_offset;
    uint32_t first_line;

    /* Retained window and decoded-scalar index, private to semantic_text.c. */
    uint32_t window_start;      /* body-relative offset of scratch[0] */
    uint32_t window_length;     /* valid bytes in scratch */
    uint32_t scan_pos;          /* scratch index of the first undecoded byte */
    uint32_t window_line;       /* physical line at scratch[0] */
    uint32_t scalar_count;      /* decoded scalars recorded below */
    uint16_t scalar_offset[YAN_SEMANTIC_CHUNKER_MAX_SCALARS];
    uint8_t scalar_flags[YAN_SEMANTIC_CHUNKER_MAX_SCALARS];

    bool invalid;
    bool stopped;
    bool finished;
    bool initialized;
    bool busy;
    uint8_t scratch[YAN_SEMANTIC_CHUNKER_SCRATCH_SIZE];
} YanSemanticChunker;

/* Records body_offset (absolute file byte offset of the first body byte) and
 * first_line (physical 1-based line of that byte, with header lines already
 * counted by the source adapter). Returns INVALID for a NULL context, else
 * OK. No bytes are read here. */
YanSemanticResult yan_semantic_chunker_init(YanSemanticChunker *ctx,
                                            uint32_t body_offset,
                                            uint32_t first_line);

/* Feeds `length` body bytes and delivers every complete chunk through `sink`.
 * `bytes` may be NULL only when length is 0. Returns INVALID for a NULL or
 * uninitialized context, a NULL sink/callback, NULL bytes with a nonzero
 * length, a byte/sink span overlapping the context, or after finish; BUSY for
 * a re-entrant call from a sink callback; STOPPED once a sink returned false;
 * TOO_LARGE when the accumulated absolute body length would leave the uint32
 * offset range (the 0029 index records use uint32 start/end); otherwise OK. */
YanSemanticResult yan_semantic_chunker_feed(YanSemanticChunker *ctx,
                                            const uint8_t *bytes,
                                            size_t length,
                                            const YanSemanticChunkSink *sink);

/* Declares EOF, delivers the final covering chunk at most once, and finishes
 * the stream. Returns INVALID for a NULL or uninitialized/finished context, a
 * NULL sink/callback, or a sink object overlapping the context; BUSY for a
 * re-entrant call from a sink callback; STOPPED after a sink stop; otherwise
 * OK. */
YanSemanticResult yan_semantic_chunker_finish(YanSemanticChunker *ctx,
                                              const YanSemanticChunkSink *sink);

#endif
