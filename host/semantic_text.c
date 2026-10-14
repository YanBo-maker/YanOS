/* Pure Host text eligibility and chunking for 0029.
 *
 * The behavioural contract is host/semantic_text.h and
 * docs/specs/0029-host-semantic-search.md, "只读来源与文本资格" and "确定分段
 * 与出处", plus the fixed Unicode White_Space set of
 * docs/specs/0027-note-metadata.md. This translation unit owns the streaming
 * whole-file text validator, the bounded query validator, the White_Space
 * helper and the streaming one-file chunker. It allocates nothing, does no
 * I/O and reads only caller-offered bytes. Tests-first history: the header and
 * suite existed first and were RED against an UNSUPPORTED/false stub; this
 * source replaced that stub.
 *
 * Whole-file classification is a byte-driven strict-UTF-8 state machine with a
 * sticky invalid flag: a bad sequence is recorded and the remaining feeds are
 * still consumed. A CR must be followed by LF across the feed boundary; a
 * pending CR or a truncated scalar at finish is invalid. This is the same
 * strict decoder the query validator and the chunker use in bounded form.
 *
 * The chunker keeps a bounded linear byte window and an index of decoded
 * scalar starts. It decodes at most one scalar past the 512-scalar candidate
 * so the CR/LF end decision is exact, emits (or skips a blank segment), then
 * drops everything before the 64/65-scalar overlap start. The context is
 * ~6 KiB and never grows with the body; the last EOF-covering segment is
 * emitted once by finish and no overlap-only tail follows. */

#include "semantic_text.h"

#include <string.h>

/* Decoded-scalar flags used by the chunker index. */
#define YAN_TEXT_FLAG_WHITE UINT32_C(0x01)
#define YAN_TEXT_FLAG_CR UINT32_C(0x02)
#define YAN_TEXT_FLAG_LF UINT32_C(0x04)

typedef enum {
    YAN_TEXT_DEC_OK = 0,
    YAN_TEXT_DEC_INCOMPLETE = 1,
    YAN_TEXT_DEC_INVALID = 2
} YanTextDecode;

/* True when two caller spans intersect; an overflowing span counts as
 * overlapping, and a zero-length span never does. Pure address arithmetic. */
static bool yan_text_spans_overlap(const void *first, size_t first_length,
                                   const void *second, size_t second_length)
{
    uintptr_t first_begin = (uintptr_t)first;
    uintptr_t second_begin = (uintptr_t)second;
    if (first_length == 0u || second_length == 0u) {
        return false;
    }
    if ((uint64_t)first_length > (uint64_t)(UINTPTR_MAX - first_begin) ||
        (uint64_t)second_length > (uint64_t)(UINTPTR_MAX - second_begin)) {
        return true;
    }
    return first_begin < second_begin + second_length &&
           second_begin < first_begin + first_length;
}

/* Decodes exactly one strict UTF-8 scalar from up to `length` bytes. Returns
 * INCOMPLETE when the prefix could become a legal scalar with more bytes,
 * INVALID for overlong forms, surrogates, scalars above U+10FFFF, stray
 * continuation bytes and impossible leads, and OK with *length_out set for a
 * complete scalar. `length` must be at least 1. */
static YanTextDecode yan_text_decode_one(const uint8_t *bytes, size_t length,
                                         uint32_t *scalar,
                                         uint32_t *length_out)
{
    const uint32_t first = bytes[0];
    if (first < 0x80u) {
        *scalar = first;
        *length_out = 1u;
        return YAN_TEXT_DEC_OK;
    }
    if (first < 0xC2u) {
        return YAN_TEXT_DEC_INVALID;
    }
    if (first < 0xE0u) {
        if (length < 2u) {
            return YAN_TEXT_DEC_INCOMPLETE;
        }
        const uint32_t second = bytes[1];
        if ((second & 0xC0u) != 0x80u) {
            return YAN_TEXT_DEC_INVALID;
        }
        *scalar = ((first & 0x1Fu) << 6) | (second & 0x3Fu);
        *length_out = 2u;
        return YAN_TEXT_DEC_OK;
    }
    if (first < 0xF0u) {
        if (length < 3u) {
            return YAN_TEXT_DEC_INCOMPLETE;
        }
        const uint32_t second = bytes[1];
        const uint32_t third = bytes[2];
        if ((second & 0xC0u) != 0x80u || (third & 0xC0u) != 0x80u) {
            return YAN_TEXT_DEC_INVALID;
        }
        const uint32_t value = ((first & 0x0Fu) << 12) |
                               ((second & 0x3Fu) << 6) |
                               (third & 0x3Fu);
        if (value < 0x800u || (value >= 0xD800u && value <= 0xDFFFu)) {
            return YAN_TEXT_DEC_INVALID;
        }
        *scalar = value;
        *length_out = 3u;
        return YAN_TEXT_DEC_OK;
    }
    if (first < 0xF5u) {
        if (length < 4u) {
            return YAN_TEXT_DEC_INCOMPLETE;
        }
        const uint32_t second = bytes[1];
        const uint32_t third = bytes[2];
        const uint32_t fourth = bytes[3];
        if ((second & 0xC0u) != 0x80u || (third & 0xC0u) != 0x80u ||
            (fourth & 0xC0u) != 0x80u) {
            return YAN_TEXT_DEC_INVALID;
        }
        const uint32_t value = ((first & 0x07u) << 18) |
                               ((second & 0x3Fu) << 12) |
                               ((third & 0x3Fu) << 6) |
                               (fourth & 0x3Fu);
        if (value < 0x10000u || value > 0x10FFFFu) {
            return YAN_TEXT_DEC_INVALID;
        }
        *scalar = value;
        *length_out = 4u;
        return YAN_TEXT_DEC_OK;
    }
    return YAN_TEXT_DEC_INVALID;
}

/* A completed multi-byte scalar is legal only when it is not overlong, not a
 * surrogate, not above U+10FFFF and not a C1 control. */
static bool yan_text_scalar_forbidden(uint32_t scalar, uint32_t minimum)
{
    if (scalar < minimum) {
        return true;
    }
    if (scalar >= 0xD800u && scalar <= 0xDFFFu) {
        return true;
    }
    if (scalar > 0x10FFFFu) {
        return true;
    }
    if (scalar >= 0x80u && scalar <= 0x9Fu) {
        return true;
    }
    return false;
}

bool yan_semantic_white_space(uint32_t scalar)
{
    if (scalar >= 0x09u && scalar <= 0x0Du) {
        return true;
    }
    if (scalar == 0x20u || scalar == 0x85u || scalar == 0xA0u) {
        return true;
    }
    if (scalar == 0x1680u) {
        return true;
    }
    if (scalar >= 0x2000u && scalar <= 0x200Au) {
        return true;
    }
    if (scalar == 0x2028u || scalar == 0x2029u || scalar == 0x202Fu) {
        return true;
    }
    if (scalar == 0x205Fu || scalar == 0x3000u) {
        return true;
    }
    return false;
}

/* ------------------------------------------------ streaming whole file */

YanSemanticResult yan_semantic_text_init(YanSemanticText *ctx)
{
    if (ctx == NULL) {
        return YAN_SEMANTIC_INVALID;
    }
    memset(ctx, 0, sizeof *ctx);
    ctx->initialized = true;
    return YAN_SEMANTIC_OK;
}

YanSemanticResult yan_semantic_text_update(YanSemanticText *ctx,
                                           const uint8_t *bytes,
                                           size_t length)
{
    if (ctx == NULL || !ctx->initialized) {
        return YAN_SEMANTIC_INVALID;
    }
    if (ctx->finished) {
        return YAN_SEMANTIC_INVALID;
    }
    if (bytes == NULL && length > 0u) {
        return YAN_SEMANTIC_INVALID;
    }
    if (yan_text_spans_overlap(ctx, sizeof *ctx, bytes, length)) {
        return YAN_SEMANTIC_INVALID;
    }

    for (size_t i = 0u; i < length; ++i) {
        const uint32_t byte = bytes[i];

        if (ctx->continuation_remaining > 0u) {
            if ((byte & 0xC0u) == 0x80u) {
                ctx->pending_scalar = (ctx->pending_scalar << 6) |
                                      (uint32_t)(byte & 0x3Fu);
                ctx->continuation_remaining -= 1u;
                if (ctx->continuation_remaining == 0u) {
                    if (yan_text_scalar_forbidden(ctx->pending_scalar,
                                                  ctx->pending_min)) {
                        ctx->invalid = true;
                    }
                    ctx->scalar_count += 1u;
                }
                continue;
            }
            /* A non-continuation byte aborts the open sequence; the byte is
             * then classified again as a fresh start byte. */
            ctx->invalid = true;
            ctx->continuation_remaining = 0u;
        }

        if (ctx->pending_cr) {
            if (byte == 0x0Au) {
                ctx->pending_cr = false;
                ctx->scalar_count += 1u;
                continue;
            }
            /* A bare CR is invalid; its follower is classified as a start. */
            ctx->invalid = true;
            ctx->pending_cr = false;
        }

        if (byte == 0x0Du) {
            ctx->pending_cr = true;
            ctx->scalar_count += 1u;
        } else if (byte == 0x0Au || byte == 0x09u) {
            ctx->scalar_count += 1u;
        } else if (byte < 0x20u || byte == 0x7Fu) {
            ctx->invalid = true;
            ctx->scalar_count += 1u;
        } else if (byte < 0x80u) {
            ctx->scalar_count += 1u;
        } else if (byte < 0xC2u) {
            ctx->invalid = true;
            ctx->scalar_count += 1u;
        } else if (byte < 0xE0u) {
            ctx->pending_scalar = (uint32_t)(byte & 0x1Fu);
            ctx->pending_min = 0x80u;
            ctx->continuation_remaining = 1u;
        } else if (byte < 0xF0u) {
            ctx->pending_scalar = (uint32_t)(byte & 0x0Fu);
            ctx->pending_min = 0x800u;
            ctx->continuation_remaining = 2u;
        } else if (byte < 0xF5u) {
            ctx->pending_scalar = (uint32_t)(byte & 0x07u);
            ctx->pending_min = 0x10000u;
            ctx->continuation_remaining = 3u;
        } else {
            ctx->invalid = true;
            ctx->scalar_count += 1u;
        }
    }
    return YAN_SEMANTIC_OK;
}

YanSemanticResult yan_semantic_text_finish(YanSemanticText *ctx)
{
    if (ctx == NULL || !ctx->initialized) {
        return YAN_SEMANTIC_INVALID;
    }
    if (!ctx->finished) {
        if (ctx->continuation_remaining > 0u || ctx->pending_cr) {
            ctx->invalid = true;
        }
        ctx->finished = true;
    }
    return ctx->invalid ? YAN_SEMANTIC_INVALID : YAN_SEMANTIC_OK;
}

bool yan_semantic_text_is_eligible(const YanSemanticText *ctx)
{
    return ctx != NULL && ctx->initialized && ctx->finished && !ctx->invalid;
}

/* ------------------------------------------------------ query validator */

bool yan_semantic_query_valid(const uint8_t *bytes, size_t length)
{
    if (bytes == NULL || length == 0u || length > YAN_SEMANTIC_QUERY_MAX) {
        return false;
    }
    bool any_non_white = false;
    size_t offset = 0u;
    while (offset < length) {
        uint32_t scalar = 0u;
        uint32_t width = 0u;
        if (yan_text_decode_one(bytes + offset, length - offset, &scalar,
                                &width) != YAN_TEXT_DEC_OK) {
            return false;
        }
        if (scalar == 0x0Au || scalar == 0x0Du || scalar == 0x00u ||
            scalar == 0x7Fu) {
            return false;
        }
        if (scalar < 0x20u && scalar != 0x09u) {
            return false;
        }
        if (scalar >= 0x80u && scalar <= 0x9Fu) {
            return false;
        }
        if (!yan_semantic_white_space(scalar)) {
            any_non_white = true;
        }
        offset += width;
    }
    return any_non_white;
}

/* --------------------------------------------------------------- chunker */

static uint8_t yan_chunk_classify(uint32_t scalar)
{
    uint8_t flags = 0u;
    if (yan_semantic_white_space(scalar)) {
        flags = (uint8_t)(flags | YAN_TEXT_FLAG_WHITE);
    }
    if (scalar == 0x0Du) {
        flags = (uint8_t)(flags | YAN_TEXT_FLAG_CR);
    }
    if (scalar == 0x0Au) {
        flags = (uint8_t)(flags | YAN_TEXT_FLAG_LF);
    }
    return flags;
}

/* Drops the first `n` decoded scalars: advances window_line over their LFs,
 * moves the retained bytes to the front and rebases the scalar index. */
static void yan_chunk_drop_prefix(YanSemanticChunker *ctx, uint32_t n)
{
    if (n == 0u || n >= ctx->scalar_count) {
        return;
    }
    const uint32_t prefix = (uint32_t)ctx->scalar_offset[n];
    for (uint32_t i = 0u; i < n; ++i) {
        if ((ctx->scalar_flags[i] & YAN_TEXT_FLAG_LF) != 0u) {
            ctx->window_line += 1u;
        }
    }

    const uint32_t remaining = ctx->window_length - prefix;
    memmove(ctx->scratch, ctx->scratch + prefix, remaining);
    const uint32_t new_count = ctx->scalar_count - n;
    for (uint32_t i = 0u; i < new_count; ++i) {
        ctx->scalar_offset[i] =
            (uint16_t)((uint32_t)ctx->scalar_offset[i + n] - prefix);
        ctx->scalar_flags[i] = ctx->scalar_flags[i + n];
    }
    ctx->scalar_count = new_count;
    ctx->window_start += prefix;
    ctx->window_length = remaining;
    ctx->scan_pos -= prefix;
}

/* Delivers one segment of `count` scalars ending at scratch byte `end_byte`.
 * A White_Space-only segment is skipped, not delivered. */
static YanSemanticResult yan_chunk_emit(YanSemanticChunker *ctx, uint32_t count,
                                        uint32_t end_byte,
                                        const YanSemanticChunkSink *sink)
{
    if (count == 0u) {
        return YAN_SEMANTIC_OK;
    }
    bool white = true;
    uint32_t lf_before_last = 0u;
    for (uint32_t i = 0u; i < count; ++i) {
        if ((ctx->scalar_flags[i] & YAN_TEXT_FLAG_WHITE) == 0u) {
            white = false;
        }
        if (i + 1u < count &&
            (ctx->scalar_flags[i] & YAN_TEXT_FLAG_LF) != 0u) {
            lf_before_last += 1u;
        }
    }
    if (white) {
        return YAN_SEMANTIC_OK;
    }

    YanSemanticChunk chunk;
    chunk.body = ctx->scratch + ctx->scalar_offset[0];
    chunk.body_length = end_byte - (uint32_t)ctx->scalar_offset[0];
    chunk.scalar_count = count;
    chunk.start = ctx->body_offset + ctx->window_start +
                  (uint32_t)ctx->scalar_offset[0];
    chunk.end = ctx->body_offset + ctx->window_start + end_byte;
    chunk.first_line = ctx->window_line;
    chunk.last_line = ctx->window_line + lf_before_last;

    if (!sink->chunk(sink->context, &chunk)) {
        ctx->stopped = true;
        return YAN_SEMANTIC_STOPPED;
    }
    return YAN_SEMANTIC_OK;
}

/* Emits the full 512-scalar candidate at the front of the window, applying
 * the CRLF end-shrink and start-rollback rules, then drops to the next start. */
static YanSemanticResult yan_chunk_emit_full(YanSemanticChunker *ctx,
                                             const YanSemanticChunkSink *sink)
{
    uint32_t end_scalar = YAN_SEMANTIC_CHUNK_MAX_SCALARS;
    if ((ctx->scalar_flags[end_scalar - 1u] & YAN_TEXT_FLAG_CR) != 0u &&
        (ctx->scalar_flags[end_scalar] & YAN_TEXT_FLAG_LF) != 0u) {
        end_scalar -= 1u;
    }
    uint32_t next = end_scalar - YAN_SEMANTIC_CHUNK_OVERLAP_SCALARS;
    if (next > 0u &&
        (ctx->scalar_flags[next - 1u] & YAN_TEXT_FLAG_CR) != 0u &&
        (ctx->scalar_flags[next] & YAN_TEXT_FLAG_LF) != 0u) {
        next -= 1u;
    }

    const YanSemanticResult result =
        yan_chunk_emit(ctx, end_scalar, (uint32_t)ctx->scalar_offset[end_scalar],
                       sink);
    if (result == YAN_SEMANTIC_OK) {
        yan_chunk_drop_prefix(ctx, next);
    }
    return result;
}

/* Decodes complete scalars and emits full candidates until the window holds
 * fewer than 513 scalars or no complete scalar remains. */
static YanSemanticResult yan_chunk_pump(YanSemanticChunker *ctx,
                                        const YanSemanticChunkSink *sink)
{
    for (;;) {
        while (ctx->scalar_count < YAN_SEMANTIC_CHUNKER_MAX_SCALARS &&
               ctx->scan_pos < ctx->window_length) {
            uint32_t scalar = 0u;
            uint32_t width = 0u;
            const YanTextDecode status = yan_text_decode_one(
                ctx->scratch + ctx->scan_pos,
                (size_t)(ctx->window_length - ctx->scan_pos), &scalar, &width);
            if (status == YAN_TEXT_DEC_INCOMPLETE) {
                break;
            }
            if (status == YAN_TEXT_DEC_INVALID) {
                ctx->invalid = true;
                return YAN_SEMANTIC_INVALID;
            }
            ctx->scalar_offset[ctx->scalar_count] = (uint16_t)ctx->scan_pos;
            ctx->scalar_flags[ctx->scalar_count] = yan_chunk_classify(scalar);
            ctx->scan_pos += width;
            ctx->scalar_count += 1u;
        }

        if (ctx->scalar_count >= YAN_SEMANTIC_CHUNK_MAX_SCALARS + 1u) {
            const YanSemanticResult result = yan_chunk_emit_full(ctx, sink);
            if (result != YAN_SEMANTIC_OK) {
                return result;
            }
            continue;
        }
        break;
    }
    return YAN_SEMANTIC_OK;
}

static YanSemanticResult yan_chunk_feed_bytes(YanSemanticChunker *ctx,
                                              const uint8_t *bytes,
                                              size_t length,
                                              const YanSemanticChunkSink *sink)
{
    size_t offset = 0u;
    while (offset < length) {
        if (ctx->window_length >= YAN_SEMANTIC_CHUNKER_SCRATCH_SIZE) {
            const YanSemanticResult result = yan_chunk_pump(ctx, sink);
            if (result != YAN_SEMANTIC_OK) {
                return result;
            }
            if (ctx->window_length >= YAN_SEMANTIC_CHUNKER_SCRATCH_SIZE) {
                return YAN_SEMANTIC_TOO_LARGE;
            }
            continue;
        }

        const size_t room = (size_t)YAN_SEMANTIC_CHUNKER_SCRATCH_SIZE -
                            (size_t)ctx->window_length;
        size_t take = length - offset;
        if (take > room) {
            take = room;
        }
        if ((uint64_t)ctx->body_offset + (uint64_t)ctx->window_start +
                (uint64_t)ctx->window_length + (uint64_t)take >
            (uint64_t)UINT32_MAX) {
            return YAN_SEMANTIC_TOO_LARGE;
        }
        memcpy(ctx->scratch + ctx->window_length, bytes + offset, take);
        ctx->window_length += (uint32_t)take;
        offset += take;

        const YanSemanticResult result = yan_chunk_pump(ctx, sink);
        if (result != YAN_SEMANTIC_OK) {
            return result;
        }
    }
    return YAN_SEMANTIC_OK;
}

YanSemanticResult yan_semantic_chunker_init(YanSemanticChunker *ctx,
                                            uint32_t body_offset,
                                            uint32_t first_line)
{
    if (ctx == NULL) {
        return YAN_SEMANTIC_INVALID;
    }
    if (ctx->busy) {
        return YAN_SEMANTIC_BUSY;
    }
    memset(ctx, 0, sizeof *ctx);
    ctx->body_offset = body_offset;
    ctx->first_line = first_line;
    ctx->window_line = first_line;
    ctx->initialized = true;
    return YAN_SEMANTIC_OK;
}

YanSemanticResult yan_semantic_chunker_feed(YanSemanticChunker *ctx,
                                            const uint8_t *bytes,
                                            size_t length,
                                            const YanSemanticChunkSink *sink)
{
    if (ctx == NULL || !ctx->initialized) {
        return YAN_SEMANTIC_INVALID;
    }
    if (ctx->busy) {
        return YAN_SEMANTIC_BUSY;
    }
    if (ctx->invalid) {
        return YAN_SEMANTIC_INVALID;
    }
    if (ctx->stopped) {
        return YAN_SEMANTIC_STOPPED;
    }
    if (ctx->finished) {
        return YAN_SEMANTIC_INVALID;
    }
    if (sink == NULL || sink->chunk == NULL) {
        return YAN_SEMANTIC_INVALID;
    }
    if (bytes == NULL && length > 0u) {
        return YAN_SEMANTIC_INVALID;
    }
    if (yan_text_spans_overlap(ctx, sizeof *ctx, bytes, length) ||
        yan_text_spans_overlap(ctx, sizeof *ctx, sink, sizeof *sink)) {
        return YAN_SEMANTIC_INVALID;
    }

    ctx->busy = true;
    const YanSemanticResult result =
        yan_chunk_feed_bytes(ctx, bytes, length, sink);
    ctx->busy = false;
    return result;
}

YanSemanticResult yan_semantic_chunker_finish(YanSemanticChunker *ctx,
                                              const YanSemanticChunkSink *sink)
{
    if (ctx == NULL || !ctx->initialized) {
        return YAN_SEMANTIC_INVALID;
    }
    if (ctx->busy) {
        return YAN_SEMANTIC_BUSY;
    }
    if (ctx->invalid) {
        return YAN_SEMANTIC_INVALID;
    }
    if (ctx->stopped) {
        return YAN_SEMANTIC_STOPPED;
    }
    if (ctx->finished) {
        return YAN_SEMANTIC_INVALID;
    }
    if (sink == NULL || sink->chunk == NULL) {
        return YAN_SEMANTIC_INVALID;
    }
    if (yan_text_spans_overlap(ctx, sizeof *ctx, sink, sizeof *sink)) {
        return YAN_SEMANTIC_INVALID;
    }

    ctx->busy = true;
    YanSemanticResult result = yan_chunk_pump(ctx, sink);
    if (result == YAN_SEMANTIC_OK) {
        if (ctx->scan_pos != ctx->window_length) {
            /* EOF landed inside a scalar: the body was truncated. */
            ctx->invalid = true;
            result = YAN_SEMANTIC_INVALID;
        } else if (ctx->scalar_count > 0u) {
            result = yan_chunk_emit(ctx, ctx->scalar_count, ctx->scan_pos,
                                    sink);
        }
    }
    if (result == YAN_SEMANTIC_OK || result == YAN_SEMANTIC_INVALID) {
        ctx->finished = true;
    }
    ctx->busy = false;
    return result;
}
