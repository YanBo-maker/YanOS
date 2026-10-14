/* Host streaming SHA-256 for 0029 identity hashes.
 *
 * The behavioural contract is host/semantic_sha256.h and
 * docs/specs/0029-host-semantic-search.md, "派生索引、身份与发布". This
 * translation unit is the real FIPS 180-4 implementation behind the tested
 * header: caller-owned context, explicit unsigned 32-bit arithmetic, no
 * allocation, no I/O and no locale. Tests-first history: the suite and header
 * existed first and were RED against a no-digest stub; this source replaced
 * that stub.
 *
 * State machine:
 *   init    -> H0, byte count 0, empty block, initialized.
 *   update  -> alias/range/overflow checks, then append bytes and compress
 *              every complete 64-byte block; total_bytes grows by length.
 *   final   -> append 0x80, pad, write the 64-bit big-endian bit length,
 *              compress, emit the eight big-endian words, mark uninitialized.
 *
 * The overflow guard is exactly `length > max_bytes - total_bytes` with
 * max_bytes = UINT64_MAX / 8; it runs before any state changes. No shift uses
 * a count of 32 or more and every operand is unsigned. */

#include "semantic_sha256.h"

#include <string.h>

static const uint32_t yan_sha256_k[64] = {
    UINT32_C(0x428a2f98), UINT32_C(0x71374491), UINT32_C(0xb5c0fbcf),
    UINT32_C(0xe9b5dba5), UINT32_C(0x3956c25b), UINT32_C(0x59f111f1),
    UINT32_C(0x923f82a4), UINT32_C(0xab1c5ed5), UINT32_C(0xd807aa98),
    UINT32_C(0x12835b01), UINT32_C(0x243185be), UINT32_C(0x550c7dc3),
    UINT32_C(0x72be5d74), UINT32_C(0x80deb1fe), UINT32_C(0x9bdc06a7),
    UINT32_C(0xc19bf174), UINT32_C(0xe49b69c1), UINT32_C(0xefbe4786),
    UINT32_C(0x0fc19dc6), UINT32_C(0x240ca1cc), UINT32_C(0x2de92c6f),
    UINT32_C(0x4a7484aa), UINT32_C(0x5cb0a9dc), UINT32_C(0x76f988da),
    UINT32_C(0x983e5152), UINT32_C(0xa831c66d), UINT32_C(0xb00327c8),
    UINT32_C(0xbf597fc7), UINT32_C(0xc6e00bf3), UINT32_C(0xd5a79147),
    UINT32_C(0x06ca6351), UINT32_C(0x14292967), UINT32_C(0x27b70a85),
    UINT32_C(0x2e1b2138), UINT32_C(0x4d2c6dfc), UINT32_C(0x53380d13),
    UINT32_C(0x650a7354), UINT32_C(0x766a0abb), UINT32_C(0x81c2c92e),
    UINT32_C(0x92722c85), UINT32_C(0xa2bfe8a1), UINT32_C(0xa81a664b),
    UINT32_C(0xc24b8b70), UINT32_C(0xc76c51a3), UINT32_C(0xd192e819),
    UINT32_C(0xd6990624), UINT32_C(0xf40e3585), UINT32_C(0x106aa070),
    UINT32_C(0x19a4c116), UINT32_C(0x1e376c08), UINT32_C(0x2748774c),
    UINT32_C(0x34b0bcb5), UINT32_C(0x391c0cb3), UINT32_C(0x4ed8aa4a),
    UINT32_C(0x5b9cca4f), UINT32_C(0x682e6ff3), UINT32_C(0x748f82ee),
    UINT32_C(0x78a5636f), UINT32_C(0x84c87814), UINT32_C(0x8cc70208),
    UINT32_C(0x90befffa), UINT32_C(0xa4506ceb), UINT32_C(0xbef9a3f7),
    UINT32_C(0xc67178f2)
};

static const uint32_t yan_sha256_h0[8] = {
    UINT32_C(0x6a09e667), UINT32_C(0xbb67ae85), UINT32_C(0x3c6ef372),
    UINT32_C(0xa54ff53a), UINT32_C(0x510e527f), UINT32_C(0x9b05688c),
    UINT32_C(0x1f83d9ab), UINT32_C(0x5be0cd19)
};

/* Rotate right by 1..31. Callers never pass 0 or 32. */
static uint32_t yan_sha256_rotr(uint32_t value, uint32_t amount)
{
    return (value >> amount) | (value << (32u - amount));
}

/* True when two caller spans intersect. A span that would leave uintptr_t is
 * reported as overlapping, which is the safe answer for an alias rejection.
 * A zero-length span holds no byte and is never overlapping. Nothing is read
 * or written through either span. */
static bool yan_sha256_spans_overlap(const void *first, size_t first_length,
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

static void yan_sha256_compress(uint32_t state[8], const uint8_t block[64])
{
    uint32_t w[64];

    for (uint32_t i = 0u; i < 16u; ++i) {
        w[i] = ((uint32_t)block[4u * i] << 24) |
               ((uint32_t)block[4u * i + 1u] << 16) |
               ((uint32_t)block[4u * i + 2u] << 8) |
               (uint32_t)block[4u * i + 3u];
    }
    for (uint32_t i = 16u; i < 64u; ++i) {
        uint32_t s0 = yan_sha256_rotr(w[i - 15u], 7u) ^
                      yan_sha256_rotr(w[i - 15u], 18u) ^ (w[i - 15u] >> 3);
        uint32_t s1 = yan_sha256_rotr(w[i - 2u], 17u) ^
                      yan_sha256_rotr(w[i - 2u], 19u) ^ (w[i - 2u] >> 10);
        w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
    }

    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    uint32_t e = state[4];
    uint32_t f = state[5];
    uint32_t g = state[6];
    uint32_t h = state[7];

    for (uint32_t i = 0u; i < 64u; ++i) {
        uint32_t big_s1 = yan_sha256_rotr(e, 6u) ^ yan_sha256_rotr(e, 11u) ^
                          yan_sha256_rotr(e, 25u);
        uint32_t choice = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + big_s1 + choice + yan_sha256_k[i] + w[i];
        uint32_t big_s0 = yan_sha256_rotr(a, 2u) ^ yan_sha256_rotr(a, 13u) ^
                          yan_sha256_rotr(a, 22u);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = big_s0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

YanSemanticResult yan_semantic_sha256_init(YanSemanticSha256 *ctx)
{
    if (ctx == NULL) {
        return YAN_SEMANTIC_INVALID;
    }

    for (uint32_t i = 0u; i < 8u; ++i) {
        ctx->state[i] = yan_sha256_h0[i];
    }
    for (uint32_t i = 0u; i < YAN_SEMANTIC_SHA256_BLOCK_SIZE; ++i) {
        ctx->block[i] = 0u;
    }
    ctx->total_bytes = 0u;
    ctx->block_length = 0u;
    ctx->initialized = true;
    return YAN_SEMANTIC_OK;
}

YanSemanticResult yan_semantic_sha256_update(YanSemanticSha256 *ctx,
                                             const uint8_t *bytes,
                                             size_t length)
{
    if (ctx == NULL || !ctx->initialized) {
        return YAN_SEMANTIC_INVALID;
    }
    if (bytes == NULL && length > 0u) {
        return YAN_SEMANTIC_INVALID;
    }
    if (yan_sha256_spans_overlap(ctx, sizeof *ctx, bytes, length)) {
        return YAN_SEMANTIC_INVALID;
    }

    /* SHA-256 carries the message length in bits in a 64-bit field, so at most
     * UINT64_MAX / 8 bytes are legal. Check before touching any state. */
    {
        const uint64_t max_bytes = UINT64_MAX / 8u;
        if (ctx->total_bytes > max_bytes ||
            (uint64_t)length > max_bytes - ctx->total_bytes) {
            return YAN_SEMANTIC_TOO_LARGE;
        }
    }

    size_t offset = 0u;
    while (offset < length) {
        const size_t room = (size_t)YAN_SEMANTIC_SHA256_BLOCK_SIZE -
                            ctx->block_length;
        size_t take = length - offset;
        if (take > room) {
            take = room;
        }
        memcpy(ctx->block + ctx->block_length, bytes + offset, take);
        ctx->block_length += take;
        offset += take;
        if (ctx->block_length == (size_t)YAN_SEMANTIC_SHA256_BLOCK_SIZE) {
            yan_sha256_compress(ctx->state, ctx->block);
            ctx->block_length = 0u;
        }
    }
    ctx->total_bytes += (uint64_t)length;
    return YAN_SEMANTIC_OK;
}

YanSemanticResult yan_semantic_sha256_final(
    YanSemanticSha256 *ctx,
    uint8_t out[YAN_SEMANTIC_SHA256_DIGEST_SIZE])
{
    if (ctx == NULL || out == NULL || !ctx->initialized) {
        return YAN_SEMANTIC_INVALID;
    }
    if (yan_sha256_spans_overlap(ctx, sizeof *ctx, out,
                                 YAN_SEMANTIC_SHA256_DIGEST_SIZE)) {
        return YAN_SEMANTIC_INVALID;
    }

    const uint64_t bit_length = ctx->total_bytes * 8u;
    uint8_t *block = ctx->block;
    size_t index = ctx->block_length;

    block[index++] = 0x80u;
    if (index > 56u) {
        while (index < (size_t)YAN_SEMANTIC_SHA256_BLOCK_SIZE) {
            block[index++] = 0u;
        }
        yan_sha256_compress(ctx->state, block);
        index = 0u;
    }
    while (index < 56u) {
        block[index++] = 0u;
    }
    for (uint32_t i = 0u; i < 8u; ++i) {
        block[56u + i] = (uint8_t)(bit_length >> (56u - 8u * i));
    }
    yan_sha256_compress(ctx->state, block);

    for (uint32_t i = 0u; i < 8u; ++i) {
        out[4u * i] = (uint8_t)(ctx->state[i] >> 24);
        out[4u * i + 1u] = (uint8_t)(ctx->state[i] >> 16);
        out[4u * i + 2u] = (uint8_t)(ctx->state[i] >> 8);
        out[4u * i + 3u] = (uint8_t)ctx->state[i];
    }
    ctx->initialized = false;
    return YAN_SEMANTIC_OK;
}
