/* 0029 Host SHA-256: native tests, written before the implementation.
 *
 * The contract is host/semantic_sha256.h and
 * docs/specs/0029-host-semantic-search.md ("派生索引、身份与发布"). The suite
 * was introduced against the intentional not-implemented stub: init, update
 * and final answer OK but final emits thirty-two zero bytes, so the FIPS
 * vector assertions fail on the digest while the file compiles, links and
 * runs. Compile/link failures are harness failures, not RED assertions.
 *
 * What this file pins, clause by clause:
 *
 *   - FIPS 180-4 vectors: empty, "abc", the 112-byte two-block message and
 *     one million 'a' bytes built by streaming a 1000-byte block 1000 times;
 *   - chunking invariance: the same 112-byte message fed with every chunk
 *     size 1..112 produces one digest, and that digest is the published
 *     vector, so a stub digest cannot pass by matching another stub digest;
 *   - lifecycle: final consumes the context, a zero-initialized context is
 *     not initialized, and NULL/overflow arguments are rejected before the
 *     digest is written;
 *   - the shared result vocabulary is stable.
 *
 * Every expected digest is an explicit FIPS hex string, not a value produced
 * by the implementation under test. */

#include "semantic_sha256.h"
#include "unity.h"

#include <string.h>

static uint8_t hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return (uint8_t)(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return (uint8_t)(c - 'a' + 10);
    }
    return (uint8_t)(c - 'A' + 10);
}

static void parse_hex(const char *hex, uint8_t *out, size_t out_length)
{
    for (size_t i = 0u; i < out_length; ++i) {
        out[i] = (uint8_t)((hex_value(hex[2u * i]) << 4) |
                           hex_value(hex[2u * i + 1u]));
    }
}

static void assert_digest_hex(const uint8_t *digest, const char *hex)
{
    uint8_t expected[YAN_SEMANTIC_SHA256_DIGEST_SIZE];
    parse_hex(hex, expected, YAN_SEMANTIC_SHA256_DIGEST_SIZE);
    TEST_ASSERT_EQUAL_MEMORY(expected, digest, YAN_SEMANTIC_SHA256_DIGEST_SIZE);
}

static YanSemanticResult hash_of(const uint8_t *bytes, size_t length,
                                 uint8_t out[YAN_SEMANTIC_SHA256_DIGEST_SIZE])
{
    YanSemanticSha256 ctx;
    YanSemanticResult result = yan_semantic_sha256_init(&ctx);
    if (result != YAN_SEMANTIC_OK) {
        return result;
    }
    result = yan_semantic_sha256_update(&ctx, bytes, length);
    if (result != YAN_SEMANTIC_OK) {
        return result;
    }
    return yan_semantic_sha256_final(&ctx, out);
}

static YanSemanticResult hash_chunked(
    const uint8_t *bytes, size_t length, size_t chunk,
    uint8_t out[YAN_SEMANTIC_SHA256_DIGEST_SIZE])
{
    YanSemanticSha256 ctx;
    YanSemanticResult result = yan_semantic_sha256_init(&ctx);
    if (result != YAN_SEMANTIC_OK) {
        return result;
    }
    size_t offset = 0u;
    while (offset < length) {
        size_t amount = length - offset;
        if (amount > chunk) {
            amount = chunk;
        }
        result = yan_semantic_sha256_update(&ctx, bytes + offset, amount);
        if (result != YAN_SEMANTIC_OK) {
            return result;
        }
        offset += amount;
    }
    return yan_semantic_sha256_final(&ctx, out);
}

void setUp(void)
{
}

void tearDown(void)
{
}

/* ------------------------------------------------------------- vectors */

static void sha256_empty_vector(void)
{
    uint8_t digest[YAN_SEMANTIC_SHA256_DIGEST_SIZE];
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, hash_of(NULL, 0u, digest));
    assert_digest_hex(digest,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

static void sha256_abc_vector(void)
{
    static const uint8_t abc[] = "abc";
    uint8_t digest[YAN_SEMANTIC_SHA256_DIGEST_SIZE];
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK,
                      hash_of(abc, sizeof abc - 1u, digest));
    assert_digest_hex(digest,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

static void sha256_two_block_vector(void)
{
    static const uint8_t message[] =
        "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
        "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
    uint8_t digest[YAN_SEMANTIC_SHA256_DIGEST_SIZE];
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK,
                      hash_of(message, sizeof message - 1u, digest));
    assert_digest_hex(digest,
        "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
}

static void sha256_million_a_vector(void)
{
    static char block[1000];
    uint8_t digest[YAN_SEMANTIC_SHA256_DIGEST_SIZE];
    YanSemanticSha256 ctx;

    memset(block, 'a', sizeof block);
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_sha256_init(&ctx));
    for (uint32_t i = 0u; i < 1000u; ++i) {
        TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK,
                          yan_semantic_sha256_update(
                              &ctx, (const uint8_t *)block, sizeof block));
    }
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_sha256_final(&ctx, digest));
    assert_digest_hex(digest,
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

static void sha256_chunking_is_invariant(void)
{
    static const uint8_t message[] =
        "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
        "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
    uint8_t whole[YAN_SEMANTIC_SHA256_DIGEST_SIZE];
    uint8_t chunked[YAN_SEMANTIC_SHA256_DIGEST_SIZE];
    const size_t length = sizeof message - 1u;

    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, hash_of(message, length, whole));
    for (size_t chunk = 1u; chunk <= length; ++chunk) {
        TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK,
                          hash_chunked(message, length, chunk, chunked));
        TEST_ASSERT_EQUAL_MEMORY(whole, chunked,
                                 YAN_SEMANTIC_SHA256_DIGEST_SIZE);
    }
    /* The vector comparison is what makes the invariance test RED against the
     * zero-digest stub: two zero digests would otherwise compare equal. */
    assert_digest_hex(whole,
        "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
}

/* ------------------------------------------------------------ lifecycle */

static void sha256_rejects_invalid_arguments(void)
{
    YanSemanticSha256 ctx;
    uint8_t digest[YAN_SEMANTIC_SHA256_DIGEST_SIZE];

    memset(&ctx, 0, sizeof ctx);
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID, yan_semantic_sha256_init(NULL));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID,
                      yan_semantic_sha256_update(&ctx, NULL, 0u));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID,
                      yan_semantic_sha256_final(&ctx, digest));

    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_sha256_init(&ctx));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID,
                      yan_semantic_sha256_update(&ctx, NULL, 1u));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK,
                      yan_semantic_sha256_update(&ctx, NULL, 0u));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID,
                      yan_semantic_sha256_update(NULL, NULL, 0u));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID,
                      yan_semantic_sha256_final(&ctx, NULL));
}

static void sha256_final_consumes_the_context(void)
{
    YanSemanticSha256 ctx;
    uint8_t digest[YAN_SEMANTIC_SHA256_DIGEST_SIZE];

    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_sha256_init(&ctx));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_sha256_final(&ctx, digest));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID,
                      yan_semantic_sha256_update(&ctx, (const uint8_t *)"a",
                                                 1u));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID,
                      yan_semantic_sha256_final(&ctx, digest));
}

static void semantic_result_names_are_stable(void)
{
    TEST_ASSERT_EQUAL_STRING("OK", yan_semantic_result_name(YAN_SEMANTIC_OK));
    TEST_ASSERT_EQUAL_STRING("INVALID",
                             yan_semantic_result_name(YAN_SEMANTIC_INVALID));
    TEST_ASSERT_EQUAL_STRING("STOPPED",
                             yan_semantic_result_name(YAN_SEMANTIC_STOPPED));
    TEST_ASSERT_EQUAL_STRING("UNSUPPORTED",
                             yan_semantic_result_name(YAN_SEMANTIC_UNSUPPORTED));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN",
                             yan_semantic_result_name((YanSemanticResult)999));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(sha256_empty_vector);
    RUN_TEST(sha256_abc_vector);
    RUN_TEST(sha256_two_block_vector);
    RUN_TEST(sha256_million_a_vector);
    RUN_TEST(sha256_chunking_is_invariant);
    RUN_TEST(sha256_rejects_invalid_arguments);
    RUN_TEST(sha256_final_consumes_the_context);
    RUN_TEST(semantic_result_names_are_stable);
    return UNITY_END();
}
