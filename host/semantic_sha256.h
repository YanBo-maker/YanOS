#ifndef YAN_HOST_SEMANTIC_SHA256_H
#define YAN_HOST_SEMANTIC_SHA256_H

#include "semantic_result.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Portable C17 streaming SHA-256 for the 0029 Host identity hashes.
 *
 * The contract is docs/specs/0029-host-semantic-search.md, "派生索引、身份与
 * 发布": the source, model, runner and configuration identities are SHA-256
 * digests. This translation unit is deliberately boring: explicit unsigned
 * 32-bit arithmetic, a caller-owned context, no allocation, no I/O, no Host
 * header and no locale. It must compile the same for every Host the semantic
 * tool supports. The digest is always 32 bytes in big-endian byte order, as
 * FIPS 180-4 prints it.
 *
 * Streaming ownership and state:
 *   - The caller owns the YanSemanticSha256 object and must zero-initialize it
 *     before yan_semantic_sha256_init. The object is small (about 120 bytes)
 *     so it can live in a struct field, on the Host stack, or in static
 *     storage; it never holds a pointer to the input.
 *   - init starts a new message and returns OK.
 *   - update may be called any number of times with any byte lengths and any
 *     split points, including length 0. Feed boundaries never change the
 *     digest because bytes are buffered to a 64-byte block boundary.
 *   - final writes exactly YAN_SEMANTIC_SHA256_DIGEST_SIZE bytes and consumes
 *     the context: after a successful final the context is no longer
 *     initialized and must be re-initialized before reuse. final is the only
 *     call that owns the padded tail and the length field; update never pads.
 *
 * Result contract (the pure layer keeps this vocabulary for the facade):
 *   - init: INVALID when ctx is NULL, otherwise OK.
 *   - update: INVALID when ctx is NULL, the context is not initialized, when
 *     bytes is NULL with a nonzero length, or when the offered bytes span
 *     overlaps the context object. Otherwise OK. A length that would push the
 *     message past UINT64_MAX / 8 bytes (the largest byte length whose bit
 *     length fits the 64-bit SHA-256 length field) is TOO_LARGE and leaves the
 *     context unchanged.
 *   - final: INVALID when ctx or out is NULL, the context is not initialized,
 *     or out overlaps the context. Otherwise OK. Any error leaves out
 *     byte-identical.
 *
 * This public header is the seam the later index builder uses; the tests in
 * tests/test_semantic_sha256.c pin the FIPS vectors and the chunking
 * invariance. Tests-first history: the header and suite were introduced
 * against a deliberate no-digest stub so the vector assertions were RED; the
 * current source implements the digest and that stub no longer exists. */

#define YAN_SEMANTIC_SHA256_DIGEST_SIZE UINT32_C(32)
#define YAN_SEMANTIC_SHA256_BLOCK_SIZE UINT32_C(64)

typedef struct {
    /* Working chaining value, eight big-endian 32-bit words. */
    uint32_t state[8];
    /* Total message bytes fed so far; the length field is this times 8. */
    uint64_t total_bytes;
    /* Bytes currently buffered in `block`, 0..63. */
    size_t block_length;
    /* True between init and a successful final. */
    bool initialized;
    /* Partial/tail block; bytes past block_length are undefined. */
    uint8_t block[YAN_SEMANTIC_SHA256_BLOCK_SIZE];
} YanSemanticSha256;

_Static_assert(sizeof(YanSemanticSha256) <= 256u,
               "the SHA-256 context must stay a small caller-owned object");

/* Starts a new message. */
YanSemanticResult yan_semantic_sha256_init(YanSemanticSha256 *ctx);

/* Feeds `length` bytes. `bytes` may be NULL only when length is 0. */
YanSemanticResult yan_semantic_sha256_update(YanSemanticSha256 *ctx,
                                             const uint8_t *bytes,
                                             size_t length);

/* Writes the 32-byte digest and consumes the context. `out` is a required
 * 32-byte buffer and is left unchanged on any error. */
YanSemanticResult yan_semantic_sha256_final(
    YanSemanticSha256 *ctx,
    uint8_t out[YAN_SEMANTIC_SHA256_DIGEST_SIZE]);

#endif
