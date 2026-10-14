/* 0029 Host streaming text validator, query validator and chunker tests.
 *
 * The contract is host/semantic_text.h plus docs/specs/0029-host-semantic-
 * search.md ("只读来源与文本资格", "确定分段与出处") and the fixed Unicode
 * White_Space set of docs/specs/0027-note-metadata.md. The suite was written
 * first against the intentional UNSUPPORTED/false stub, so the positive
 * assertions are semantically RED while the file still compiles, links and
 * runs. Compile/link failures are harness failures, not RED assertions.
 *
 * What this file pins, clause by clause:
 *
 *   - the exact 0027 White_Space set and its immediate neighbours;
 *   - whole-file eligibility: plain ASCII, TAB/LF/CRLF and body U+FEFF are
 *     legal; NUL, other C0, DEL, C1, overlong forms, surrogates, scalars
 *     above U+10FFFF, stray continuation bytes and a sequence truncated at
 *     EOF are invalid; a bare CR is invalid even at EOF;
 *   - feed boundaries: a legal multi-byte scalar split at every byte is still
 *     legal, CR and LF split across feeds are still a CRLF, and an invalid
 *     byte is remembered while later feeds still return OK because the caller
 *     keeps hashing and reading the remainder;
 *   - bounded query validation: 1..1023 bytes, UTF-8 strict, no LF/CR/NUL/
 *     DEL/C1/other C0, TAB and U+FEFF allowed, all-White_Space rejected;
 *   - chunking: 512/513 ASCII, a 5000-scalar stream is split-invariant, the
 *     64/65 overlap and the 446-scalar minimum progress CRLF example, CRLF
 *     end-shrink, byte offsets versus scalar counts for 3-byte scalars,
 *     physical line numbers with a header base and end-1 LF semantics, no
 *     overlap-only tail, whitespace-only chunks skipped without moving a
 *     boundary, empty/all-whitespace bodies, byte provenance and a false sink
 *     stop.
 *
 * Every expected offset, scalar count and line number is written out
 * explicitly; the collector also compares each chunk body against the
 * original bytes so a correct-looking boundary count cannot pass with wrong
 * bytes. Large fixtures and contexts are file-scope static, never on the
 * stack. */

#include "semantic_text.h"
#include "unity.h"

#include <string.h>

#define TEXT_BUFFER_MAX 8192u
#define CHUNK_LOG_MAX 64u

static uint8_t text_buffer[TEXT_BUFFER_MAX];

typedef struct {
    uint32_t count;
    YanSemanticChunk chunks[CHUNK_LOG_MAX];
    uint32_t stop_after;
    bool provenance_ok;
} ChunkLog;

static ChunkLog chunk_log;
static const uint8_t *provenance_body;
static size_t provenance_length;
static uint32_t provenance_offset;
static uint32_t g_stop_after;

void setUp(void)
{
    memset(text_buffer, 0, sizeof text_buffer);
    memset(&chunk_log, 0, sizeof chunk_log);
    provenance_body = NULL;
    provenance_length = 0u;
    provenance_offset = 0u;
    g_stop_after = 0u;
}

void tearDown(void)
{
}

/* --------------------------------------------------------------- helpers */

static void fill_repeated(uint8_t *out, size_t length, uint8_t value)
{
    for (size_t i = 0u; i < length; ++i) {
        out[i] = value;
    }
}

static YanSemanticResult run_text_split(const uint8_t *bytes, size_t length,
                                        size_t split)
{
    static YanSemanticText ctx;
    memset(&ctx, 0, sizeof ctx);

    YanSemanticResult result = yan_semantic_text_init(&ctx);
    if (result != YAN_SEMANTIC_OK) {
        return result;
    }
    if (length == 0u) {
        return yan_semantic_text_finish(&ctx);
    }
    if (split == 0u) {
        split = length;
    }
    size_t offset = 0u;
    while (offset < length) {
        size_t amount = length - offset;
        if (amount > split) {
            amount = split;
        }
        result = yan_semantic_text_update(&ctx, bytes + offset, amount);
        if (result != YAN_SEMANTIC_OK) {
            return result;
        }
        offset += amount;
    }
    return yan_semantic_text_finish(&ctx);
}

static YanSemanticResult run_text_single(const uint8_t *bytes, size_t length)
{
    return run_text_split(bytes, length, length);
}

static YanSemanticResult run_text_two_feeds(const uint8_t *bytes, size_t length,
                                            size_t at)
{
    static YanSemanticText ctx;
    memset(&ctx, 0, sizeof ctx);

    YanSemanticResult result = yan_semantic_text_init(&ctx);
    if (result != YAN_SEMANTIC_OK) {
        return result;
    }
    if (at > length) {
        at = length;
    }
    result = yan_semantic_text_update(&ctx, bytes, at);
    if (result != YAN_SEMANTIC_OK) {
        return result;
    }
    result = yan_semantic_text_update(&ctx, bytes + at, length - at);
    if (result != YAN_SEMANTIC_OK) {
        return result;
    }
    return yan_semantic_text_finish(&ctx);
}

/* ---------------------------------------------------------- White_Space */

static void white_space_matches_the_0027_set(void)
{
    static const uint32_t members[] = {
        0x0009u, 0x000Au, 0x000Bu, 0x000Cu, 0x000Du, 0x0020u, 0x0085u,
        0x00A0u, 0x1680u, 0x2000u, 0x2001u, 0x2002u, 0x2003u, 0x2004u,
        0x2005u, 0x2006u, 0x2007u, 0x2008u, 0x2009u, 0x200Au, 0x2028u,
        0x2029u, 0x202Fu, 0x205Fu, 0x3000u
    };
    for (size_t i = 0u; i < sizeof members / sizeof members[0]; ++i) {
        TEST_ASSERT_TRUE_MESSAGE(yan_semantic_white_space(members[i]),
                                 "white_space_member");
    }
}

static void white_space_rejects_neighbours(void)
{
    static const uint32_t neighbours[] = {
        0x0008u, 0x000Eu, 0x001Fu, 0x0021u, 0x007Fu, 0x00A1u, 0x167Fu,
        0x1681u, 0x1FFFu, 0x200Bu, 0x2027u, 0x202Au, 0x202Eu, 0x2030u,
        0x205Eu, 0x2060u, 0x2FFFu, 0x3001u, 0xFEFFu, 0x10FFFFu
    };
    for (size_t i = 0u; i < sizeof neighbours / sizeof neighbours[0]; ++i) {
        TEST_ASSERT_FALSE_MESSAGE(yan_semantic_white_space(neighbours[i]),
                                  "white_space_neighbour");
    }
}

/* ------------------------------------------------- whole-file validator */

static void text_accepts_plain_ascii(void)
{
    static const uint8_t sample[] = "hello world\nsecond line\n";
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK,
                      run_text_single(sample, sizeof sample - 1u));
}

static void text_accepts_tab_lf_crlf_and_body_feff(void)
{
    static const uint8_t sample[] = "a\tb\nc\r\nd\xEF\xBB\xBF""e";
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK,
                      run_text_single(sample, sizeof sample - 1u));
    static const uint8_t lone_lf[] = "a\nb";
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK,
                      run_text_single(lone_lf, sizeof lone_lf - 1u));
}

static void text_rejects_nul_and_forbidden_c0(void)
{
    static const uint8_t nul[] = "ab\0cd";
    static const uint8_t soh[] = "a\x01""b";
    static const uint8_t us[] = "a\x1F""b";
    static const uint8_t vt[] = "a\x0B""b";
    static const uint8_t ff[] = "a\x0C""b";
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID,
                      run_text_single(nul, sizeof nul - 1u));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID,
                      run_text_single(soh, sizeof soh - 1u));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID,
                      run_text_single(us, sizeof us - 1u));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID,
                      run_text_single(vt, sizeof vt - 1u));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID,
                      run_text_single(ff, sizeof ff - 1u));
}

static void text_rejects_del_and_c1(void)
{
    static const uint8_t del[] = "a\x7F""b";
    static const uint8_t c1_82[] = "a\xC2\x82""b";       /* U+0082 */
    static const uint8_t c1_9f[] = "a\xC2\x9F""b";       /* U+009F */
    static const uint8_t c1_85[] = "a\xC2\x85""b";       /* U+0085 NEL */
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID,
                      run_text_single(del, sizeof del - 1u));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID,
                      run_text_single(c1_82, sizeof c1_82 - 1u));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID,
                      run_text_single(c1_9f, sizeof c1_9f - 1u));
    /* U+0085 is White_Space by 0027 but is still a C1 control, so a file
     * containing it is not eligible text. */
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID,
                      run_text_single(c1_85, sizeof c1_85 - 1u));
}

static void text_accepts_multibyte_utf8(void)
{
    static const uint8_t sample[] =
        "a\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x98\x80""z";
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK,
                      run_text_single(sample, sizeof sample - 1u));
}

static void text_rejects_overlong_surrogate_and_out_of_range(void)
{
    static const uint8_t overlong2[] = "\xC0\x80";
    static const uint8_t overlong3[] = "\xE0\x80\x80";
    static const uint8_t overlong4[] = "\xF0\x80\x80\x80";
    static const uint8_t surrogate_d800[] = "\xED\xA0\x80";
    static const uint8_t surrogate_dfff[] = "\xED\xBF\xBF";
    static const uint8_t above_10ffff[] = "\xF4\x90\x80\x80";
    static const uint8_t lead_f5[] = "\xF5\x80\x80\x80";
    static const uint8_t lead_fe[] = "\xFE";
    static const uint8_t lead_ff[] = "\xFF";
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID,
                      run_text_single(overlong2, sizeof overlong2 - 1u));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID,
                      run_text_single(overlong3, sizeof overlong3 - 1u));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID,
                      run_text_single(overlong4, sizeof overlong4 - 1u));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID,
                      run_text_single(surrogate_d800,
                                      sizeof surrogate_d800 - 1u));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID,
                      run_text_single(surrogate_dfff,
                                      sizeof surrogate_dfff - 1u));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID,
                      run_text_single(above_10ffff,
                                      sizeof above_10ffff - 1u));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID,
                      run_text_single(lead_f5, sizeof lead_f5 - 1u));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID,
                      run_text_single(lead_fe, sizeof lead_fe - 1u));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID,
                      run_text_single(lead_ff, sizeof lead_ff - 1u));
}

static void text_rejects_stray_continuation_and_truncated_eof(void)
{
    static const uint8_t stray[] = "a\x80""b";
    static const uint8_t two_byte_lead[] = "\xC3";
    static const uint8_t three_byte_short[] = "\xE4\xB8";
    static const uint8_t four_byte_short[] = "\xF0\x9F\x98";
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID,
                      run_text_single(stray, sizeof stray - 1u));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID,
                      run_text_single(two_byte_lead,
                                      sizeof two_byte_lead - 1u));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID,
                      run_text_single(three_byte_short,
                                      sizeof three_byte_short - 1u));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID,
                      run_text_single(four_byte_short,
                                      sizeof four_byte_short - 1u));
}

static void text_accepts_multibyte_split_at_every_byte(void)
{
    static const uint8_t sample[] =
        "a\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x98\x80""z";
    const size_t length = sizeof sample - 1u;
    for (size_t at = 0u; at <= length; ++at) {
        TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK,
                          run_text_two_feeds(sample, length, at));
    }
}

static void text_accepts_crlf_split_across_feeds(void)
{
    static const uint8_t sample[] = "line1\r\nline2\r\n";
    const size_t length = sizeof sample - 1u;
    for (size_t at = 0u; at <= length; ++at) {
        TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK,
                          run_text_two_feeds(sample, length, at));
    }
}

static void text_rejects_bare_cr_at_eof(void)
{
    static const uint8_t trailing[] = "abc\r";
    static const uint8_t alone[] = "\r";
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID,
                      run_text_single(trailing, sizeof trailing - 1u));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID,
                      run_text_single(alone, sizeof alone - 1u));
}

static void text_rejects_bare_cr_before_another_byte(void)
{
    static const uint8_t before_b[] = "a\rb";
    static const uint8_t doubled[] = "\r\r\n";
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID,
                      run_text_single(before_b, sizeof before_b - 1u));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID,
                      run_text_single(doubled, sizeof doubled - 1u));
}

static void text_late_invalid_is_remembered_not_fatal(void)
{
    static YanSemanticText ctx;
    memset(&ctx, 0, sizeof ctx);

    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_text_init(&ctx));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK,
                      yan_semantic_text_update(&ctx, (const uint8_t *)"good ",
                                               5u));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK,
                      yan_semantic_text_update(&ctx, (const uint8_t *)"\x00",
                                               1u));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK,
                      yan_semantic_text_update(&ctx, (const uint8_t *)"tail",
                                               4u));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID, yan_semantic_text_finish(&ctx));
    TEST_ASSERT_FALSE(yan_semantic_text_is_eligible(&ctx));
}

static void text_is_eligible_only_after_a_valid_finish(void)
{
    static YanSemanticText ctx;
    memset(&ctx, 0, sizeof ctx);

    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_text_init(&ctx));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK,
                      yan_semantic_text_update(&ctx, (const uint8_t *)"abc",
                                               3u));
    TEST_ASSERT_FALSE(yan_semantic_text_is_eligible(&ctx));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_text_finish(&ctx));
    TEST_ASSERT_TRUE(yan_semantic_text_is_eligible(&ctx));
}

static void text_accepts_long_ascii_stream(void)
{
    fill_repeated(text_buffer, 4096u, (uint8_t)'a');
    text_buffer[100u] = (uint8_t)'\n';
    text_buffer[4095u] = (uint8_t)'\n';
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, run_text_single(text_buffer, 4096u));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, run_text_split(text_buffer, 4096u, 1u));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, run_text_split(text_buffer, 4096u, 7u));
}

/* ------------------------------------------------------ query validator */

static void query_accepts_one_to_1023_bytes(void)
{
    static uint8_t query[YAN_SEMANTIC_QUERY_MAX + 1u];
    memset(query, 'q', sizeof query);

    TEST_ASSERT_TRUE(yan_semantic_query_valid(query, 1u));
    TEST_ASSERT_TRUE(yan_semantic_query_valid(query, 2u));
    TEST_ASSERT_TRUE(yan_semantic_query_valid(query, YAN_SEMANTIC_QUERY_MAX));
    TEST_ASSERT_FALSE(yan_semantic_query_valid(query, 0u));
    TEST_ASSERT_FALSE(yan_semantic_query_valid(query, YAN_SEMANTIC_QUERY_MAX + 1u));
}

static void query_rejects_lf_cr_nul_and_controls(void)
{
    static const uint8_t lf[] = "a\nb";
    static const uint8_t cr[] = "a\rb";
    static const uint8_t nul[] = "a\0b";
    static const uint8_t soh[] = "a\x01""b";
    static const uint8_t del[] = "a\x7F""b";
    static const uint8_t c1[] = "a\xC2\x85""b";
    TEST_ASSERT_FALSE(yan_semantic_query_valid(lf, sizeof lf - 1u));
    TEST_ASSERT_FALSE(yan_semantic_query_valid(cr, sizeof cr - 1u));
    TEST_ASSERT_FALSE(yan_semantic_query_valid(nul, sizeof nul - 1u));
    TEST_ASSERT_FALSE(yan_semantic_query_valid(soh, sizeof soh - 1u));
    TEST_ASSERT_FALSE(yan_semantic_query_valid(del, sizeof del - 1u));
    TEST_ASSERT_FALSE(yan_semantic_query_valid(c1, sizeof c1 - 1u));
}

static void query_allows_tab_and_feff(void)
{
    static const uint8_t tab[] = "interrupt\tPLIC";
    static const uint8_t feff[] = "\xEF\xBB\xBF";
    static const uint8_t feff_text[] = "\xEF\xBB\xBFq";
    TEST_ASSERT_TRUE(yan_semantic_query_valid(tab, sizeof tab - 1u));
    /* U+FEFF is not a White_Space scalar, so a query of just U+FEFF is a
     * legal non-empty query. */
    TEST_ASSERT_TRUE(yan_semantic_query_valid(feff, sizeof feff - 1u));
    TEST_ASSERT_TRUE(yan_semantic_query_valid(feff_text,
                                              sizeof feff_text - 1u));
}

static void query_rejects_all_white_space(void)
{
    static const uint8_t spaces[] = "   ";
    static const uint8_t tab_only[] = "\t";
    static const uint8_t mixed_white[] = " \t";
    static const uint8_t nbsp[] = "\xC2\xA0";
    static const uint8_t with_text[] = " \t a \t";
    TEST_ASSERT_FALSE(yan_semantic_query_valid(spaces, sizeof spaces - 1u));
    TEST_ASSERT_FALSE(yan_semantic_query_valid(tab_only,
                                               sizeof tab_only - 1u));
    TEST_ASSERT_FALSE(yan_semantic_query_valid(mixed_white,
                                               sizeof mixed_white - 1u));
    TEST_ASSERT_FALSE(yan_semantic_query_valid(nbsp, sizeof nbsp - 1u));
    TEST_ASSERT_TRUE(yan_semantic_query_valid(with_text,
                                              sizeof with_text - 1u));
}

static void query_rejects_invalid_utf8(void)
{
    static const uint8_t overlong[] = "\xC0\x80";
    static const uint8_t surrogate[] = "\xED\xA0\x80";
    static const uint8_t stray[] = "\x80";
    static const uint8_t truncated[] = "\xC3";
    TEST_ASSERT_FALSE(yan_semantic_query_valid(overlong,
                                               sizeof overlong - 1u));
    TEST_ASSERT_FALSE(yan_semantic_query_valid(surrogate,
                                               sizeof surrogate - 1u));
    TEST_ASSERT_FALSE(yan_semantic_query_valid(stray, sizeof stray - 1u));
    TEST_ASSERT_FALSE(yan_semantic_query_valid(truncated,
                                               sizeof truncated - 1u));
}

/* --------------------------------------------------------------- chunker */

static bool collect_chunk(void *user, const YanSemanticChunk *chunk)
{
    ChunkLog *log = (ChunkLog *)user;

    if (log->count < CHUNK_LOG_MAX) {
        log->chunks[log->count] = *chunk;
    }
    if (chunk->end < chunk->start ||
        (size_t)(chunk->end - chunk->start) != (size_t)chunk->body_length ||
        chunk->start < provenance_offset ||
        (size_t)(chunk->end - provenance_offset) > provenance_length) {
        log->provenance_ok = false;
    } else {
        const uint32_t base = chunk->start - provenance_offset;
        for (uint32_t i = 0u; i < chunk->body_length; ++i) {
            if (chunk->body == NULL ||
                chunk->body[i] != provenance_body[base + i]) {
                log->provenance_ok = false;
                break;
            }
        }
    }
    log->count += 1u;
    if (log->stop_after != 0u && log->count >= log->stop_after) {
        return false;
    }
    return true;
}

static YanSemanticResult run_chunker(const uint8_t *body, size_t length,
                                     uint32_t body_offset, uint32_t first_line,
                                     size_t split)
{
    static YanSemanticChunker ctx;
    YanSemanticChunkSink sink;

    memset(&ctx, 0, sizeof ctx);
    memset(&chunk_log, 0, sizeof chunk_log);
    chunk_log.provenance_ok = true;
    chunk_log.stop_after = g_stop_after;
    provenance_body = body;
    provenance_length = length;
    provenance_offset = body_offset;
    sink.context = &chunk_log;
    sink.chunk = collect_chunk;

    YanSemanticResult result =
        yan_semantic_chunker_init(&ctx, body_offset, first_line);
    if (result != YAN_SEMANTIC_OK) {
        return result;
    }
    if (length == 0u) {
        return yan_semantic_chunker_finish(&ctx, &sink);
    }
    if (split == 0u) {
        split = length;
    }
    size_t offset = 0u;
    while (offset < length) {
        size_t amount = length - offset;
        if (amount > split) {
            amount = split;
        }
        result = yan_semantic_chunker_feed(&ctx, body + offset, amount, &sink);
        if (result != YAN_SEMANTIC_OK) {
            return result;
        }
        offset += amount;
    }
    return yan_semantic_chunker_finish(&ctx, &sink);
}

static void assert_chunk(const YanSemanticChunk *chunk, uint32_t start,
                         uint32_t end, uint32_t scalar_count,
                         uint32_t first_line, uint32_t last_line)
{
    TEST_ASSERT_EQUAL_UINT32(start, chunk->start);
    TEST_ASSERT_EQUAL_UINT32(end, chunk->end);
    TEST_ASSERT_EQUAL_UINT32(end - start, chunk->body_length);
    TEST_ASSERT_EQUAL_UINT32(scalar_count, chunk->scalar_count);
    TEST_ASSERT_EQUAL_UINT32(first_line, chunk->first_line);
    TEST_ASSERT_EQUAL_UINT32(last_line, chunk->last_line);
}

static void chunker_512_ascii_is_one_chunk(void)
{
    fill_repeated(text_buffer, 512u, (uint8_t)'a');
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK,
                      run_chunker(text_buffer, 512u, 0u, 1u, 0u));
    TEST_ASSERT_EQUAL_UINT32(1u, chunk_log.count);
    assert_chunk(&chunk_log.chunks[0], 0u, 512u, 512u, 1u, 1u);
    TEST_ASSERT_TRUE(chunk_log.provenance_ok);
}

static void chunker_513_ascii_steps_by_448(void)
{
    fill_repeated(text_buffer, 513u, (uint8_t)'a');
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK,
                      run_chunker(text_buffer, 513u, 0u, 1u, 0u));
    TEST_ASSERT_EQUAL_UINT32(2u, chunk_log.count);
    assert_chunk(&chunk_log.chunks[0], 0u, 512u, 512u, 1u, 1u);
    assert_chunk(&chunk_log.chunks[1], 448u, 513u, 65u, 1u, 1u);
    TEST_ASSERT_TRUE(chunk_log.provenance_ok);
}

static void chunker_long_stream_is_split_invariant(void)
{
    static const uint32_t starts[12] = {
        0u, 448u, 896u, 1344u, 1792u, 2240u, 2688u, 3136u, 3584u, 4032u,
        4480u, 4928u
    };
    static const uint32_t scalars[12] = {
        512u, 512u, 512u, 512u, 512u, 512u, 512u, 512u, 512u, 512u, 512u,
        72u
    };
    static YanSemanticChunk reference[CHUNK_LOG_MAX];
    uint32_t reference_count;

    fill_repeated(text_buffer, 5000u, (uint8_t)'a');
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK,
                      run_chunker(text_buffer, 5000u, 0u, 1u, 0u));
    TEST_ASSERT_EQUAL_UINT32(12u, chunk_log.count);
    TEST_ASSERT_TRUE(chunk_log.provenance_ok);
    for (uint32_t i = 0u; i < 12u; ++i) {
        TEST_ASSERT_EQUAL_UINT32(starts[i], chunk_log.chunks[i].start);
        TEST_ASSERT_EQUAL_UINT32(scalars[i], chunk_log.chunks[i].scalar_count);
    }
    reference_count = chunk_log.count;
    memcpy(reference, chunk_log.chunks, sizeof reference);
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, run_chunker(text_buffer, 5000u, 0u, 1u, 1u));
    TEST_ASSERT_EQUAL_UINT32(reference_count, chunk_log.count);
    for (uint32_t i = 0u; i < reference_count; ++i) {
        TEST_ASSERT_EQUAL_UINT32(reference[i].start, chunk_log.chunks[i].start);
        TEST_ASSERT_EQUAL_UINT32(reference[i].end, chunk_log.chunks[i].end);
        TEST_ASSERT_EQUAL_UINT32(reference[i].scalar_count,
                                 chunk_log.chunks[i].scalar_count);
    }
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, run_chunker(text_buffer, 5000u, 0u, 1u, 7u));
    TEST_ASSERT_EQUAL_UINT32(reference_count, chunk_log.count);
    for (uint32_t i = 0u; i < reference_count; ++i) {
        TEST_ASSERT_EQUAL_UINT32(reference[i].start, chunk_log.chunks[i].start);
        TEST_ASSERT_EQUAL_UINT32(reference[i].end, chunk_log.chunks[i].end);
        TEST_ASSERT_EQUAL_UINT32(reference[i].scalar_count,
                                 chunk_log.chunks[i].scalar_count);
    }
}

static void chunker_crlf_overlap_example(void)
{
    /* Scalars: 446 x 'a', CR, LF, 63 x 'a', CR, LF, 7 x 'b'. */
    size_t offset = 0u;
    fill_repeated(text_buffer, 446u, (uint8_t)'a');
    offset = 446u;
    text_buffer[offset++] = (uint8_t)'\r';
    text_buffer[offset++] = (uint8_t)'\n';
    fill_repeated(text_buffer + offset, 63u, (uint8_t)'a');
    offset += 63u;
    text_buffer[offset++] = (uint8_t)'\r';
    text_buffer[offset++] = (uint8_t)'\n';
    fill_repeated(text_buffer + offset, 7u, (uint8_t)'b');
    offset += 7u;
    TEST_ASSERT_EQUAL_UINT32(520u, (uint32_t)offset);

    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK,
                      run_chunker(text_buffer, offset, 0u, 1u, 0u));
    TEST_ASSERT_EQUAL_UINT32(2u, chunk_log.count);
    assert_chunk(&chunk_log.chunks[0], 0u, 511u, 511u, 1u, 2u);
    assert_chunk(&chunk_log.chunks[1], 446u, 520u, 74u, 1u, 3u);
    TEST_ASSERT_EQUAL_UINT32(65u, chunk_log.chunks[0].end -
                                    chunk_log.chunks[1].start);
    TEST_ASSERT_TRUE(chunk_log.provenance_ok);
}

static void chunker_crlf_end_shrink_only(void)
{
    /* 511 x 'a', CR, LF, 'b', 'c': the end-shrink applies, the start does
     * not roll back and the overlap stays 64. */
    fill_repeated(text_buffer, 511u, (uint8_t)'a');
    text_buffer[511] = (uint8_t)'\r';
    text_buffer[512] = (uint8_t)'\n';
    text_buffer[513] = (uint8_t)'b';
    text_buffer[514] = (uint8_t)'c';

    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK,
                      run_chunker(text_buffer, 515u, 0u, 1u, 0u));
    TEST_ASSERT_EQUAL_UINT32(2u, chunk_log.count);
    assert_chunk(&chunk_log.chunks[0], 0u, 511u, 511u, 1u, 1u);
    assert_chunk(&chunk_log.chunks[1], 447u, 515u, 68u, 1u, 2u);
    TEST_ASSERT_EQUAL_UINT32(64u, chunk_log.chunks[0].end -
                                    chunk_log.chunks[1].start);
    TEST_ASSERT_TRUE(chunk_log.provenance_ok);
}

static void chunker_skips_whitespace_only_chunks(void)
{
    /* 512 spaces then 100 x 'a': the first candidate segment is skipped, but
     * the next start is still end - 64 = 448. */
    fill_repeated(text_buffer, 512u, (uint8_t)' ');
    fill_repeated(text_buffer + 512u, 100u, (uint8_t)'a');

    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK,
                      run_chunker(text_buffer, 612u, 0u, 1u, 0u));
    TEST_ASSERT_EQUAL_UINT32(1u, chunk_log.count);
    assert_chunk(&chunk_log.chunks[0], 448u, 612u, 164u, 1u, 1u);
    TEST_ASSERT_TRUE(chunk_log.provenance_ok);
}

static void chunker_empty_body_emits_nothing(void)
{
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK,
                      run_chunker(NULL, 0u, 17u, 4u, 0u));
    TEST_ASSERT_EQUAL_UINT32(0u, chunk_log.count);
}

static void chunker_all_whitespace_emits_nothing(void)
{
    fill_repeated(text_buffer, 5000u, (uint8_t)' ');
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK,
                      run_chunker(text_buffer, 5000u, 0u, 1u, 0u));
    TEST_ASSERT_EQUAL_UINT32(0u, chunk_log.count);
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK,
                      run_chunker(text_buffer, 5000u, 0u, 1u, 7u));
    TEST_ASSERT_EQUAL_UINT32(0u, chunk_log.count);
}

static void chunker_utf8_offsets_versus_scalar_count(void)
{
    const size_t scalars = 700u;
    for (size_t i = 0u; i < scalars; ++i) {
        text_buffer[i * 3u] = 0xE4u;
        text_buffer[i * 3u + 1u] = 0xB8u;
        text_buffer[i * 3u + 2u] = 0xADu;
    }
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK,
                      run_chunker(text_buffer, scalars * 3u, 0u, 1u, 0u));
    TEST_ASSERT_EQUAL_UINT32(2u, chunk_log.count);
    assert_chunk(&chunk_log.chunks[0], 0u, 1536u, 512u, 1u, 1u);
    assert_chunk(&chunk_log.chunks[1], 1344u, 2100u, 252u, 1u, 1u);
    TEST_ASSERT_EQUAL_UINT32(chunk_log.chunks[0].scalar_count * 3u,
                             chunk_log.chunks[0].body_length);
    TEST_ASSERT_EQUAL_UINT32(chunk_log.chunks[1].scalar_count * 3u,
                             chunk_log.chunks[1].body_length);
    TEST_ASSERT_TRUE(chunk_log.provenance_ok);
}

static void chunker_line_numbers_include_header_base(void)
{
    /* Body starts at absolute byte 17 on physical line 4. 511 x 'a', LF,
     * 100 x 'b'. The first chunk ends on the LF, which belongs to the line it
     * ends, so its last_line is the start line. */
    fill_repeated(text_buffer, 511u, (uint8_t)'a');
    text_buffer[511] = (uint8_t)'\n';
    fill_repeated(text_buffer + 512u, 100u, (uint8_t)'b');

    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK,
                      run_chunker(text_buffer, 612u, 17u, 4u, 0u));
    TEST_ASSERT_EQUAL_UINT32(2u, chunk_log.count);
    assert_chunk(&chunk_log.chunks[0], 17u, 529u, 512u, 4u, 4u);
    assert_chunk(&chunk_log.chunks[1], 465u, 629u, 164u, 4u, 5u);
    TEST_ASSERT_TRUE(chunk_log.provenance_ok);
}

static void chunker_last_chunk_has_no_extra_tail(void)
{
    fill_repeated(text_buffer, 960u, (uint8_t)'a');
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK,
                      run_chunker(text_buffer, 960u, 0u, 1u, 0u));
    TEST_ASSERT_EQUAL_UINT32(2u, chunk_log.count);
    assert_chunk(&chunk_log.chunks[0], 0u, 512u, 512u, 1u, 1u);
    assert_chunk(&chunk_log.chunks[1], 448u, 960u, 512u, 1u, 1u);

    fill_repeated(text_buffer, 1024u, (uint8_t)'a');
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK,
                      run_chunker(text_buffer, 1024u, 0u, 1u, 0u));
    TEST_ASSERT_EQUAL_UINT32(3u, chunk_log.count);
    assert_chunk(&chunk_log.chunks[0], 0u, 512u, 512u, 1u, 1u);
    assert_chunk(&chunk_log.chunks[1], 448u, 960u, 512u, 1u, 1u);
    assert_chunk(&chunk_log.chunks[2], 896u, 1024u, 128u, 1u, 1u);
}

static void chunker_callback_false_stops(void)
{
    fill_repeated(text_buffer, 2000u, (uint8_t)'a');
    g_stop_after = 1u;
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_STOPPED,
                      run_chunker(text_buffer, 2000u, 0u, 1u, 0u));
    TEST_ASSERT_EQUAL_UINT32(1u, chunk_log.count);
    g_stop_after = 0u;
}

static void chunker_rejects_invalid_arguments(void)
{
    static YanSemanticChunker ctx;
    YanSemanticChunkSink sink;
    memset(&ctx, 0, sizeof ctx);
    sink.context = &chunk_log;
    sink.chunk = collect_chunk;

    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID,
                      yan_semantic_chunker_init(NULL, 0u, 1u));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID,
                      yan_semantic_chunker_feed(NULL, NULL, 0u, &sink));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID,
                      yan_semantic_chunker_finish(NULL, &sink));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID,
                      yan_semantic_chunker_feed(&ctx, NULL, 0u, NULL));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID,
                      yan_semantic_chunker_feed(&ctx, NULL, 0u, &sink));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID,
                      yan_semantic_chunker_finish(&ctx, NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(white_space_matches_the_0027_set);
    RUN_TEST(white_space_rejects_neighbours);
    RUN_TEST(text_accepts_plain_ascii);
    RUN_TEST(text_accepts_tab_lf_crlf_and_body_feff);
    RUN_TEST(text_rejects_nul_and_forbidden_c0);
    RUN_TEST(text_rejects_del_and_c1);
    RUN_TEST(text_accepts_multibyte_utf8);
    RUN_TEST(text_rejects_overlong_surrogate_and_out_of_range);
    RUN_TEST(text_rejects_stray_continuation_and_truncated_eof);
    RUN_TEST(text_accepts_multibyte_split_at_every_byte);
    RUN_TEST(text_accepts_crlf_split_across_feeds);
    RUN_TEST(text_rejects_bare_cr_at_eof);
    RUN_TEST(text_rejects_bare_cr_before_another_byte);
    RUN_TEST(text_late_invalid_is_remembered_not_fatal);
    RUN_TEST(text_is_eligible_only_after_a_valid_finish);
    RUN_TEST(text_accepts_long_ascii_stream);
    RUN_TEST(query_accepts_one_to_1023_bytes);
    RUN_TEST(query_rejects_lf_cr_nul_and_controls);
    RUN_TEST(query_allows_tab_and_feff);
    RUN_TEST(query_rejects_all_white_space);
    RUN_TEST(query_rejects_invalid_utf8);
    RUN_TEST(chunker_512_ascii_is_one_chunk);
    RUN_TEST(chunker_513_ascii_steps_by_448);
    RUN_TEST(chunker_long_stream_is_split_invariant);
    RUN_TEST(chunker_crlf_overlap_example);
    RUN_TEST(chunker_crlf_end_shrink_only);
    RUN_TEST(chunker_skips_whitespace_only_chunks);
    RUN_TEST(chunker_empty_body_emits_nothing);
    RUN_TEST(chunker_all_whitespace_emits_nothing);
    RUN_TEST(chunker_utf8_offsets_versus_scalar_count);
    RUN_TEST(chunker_line_numbers_include_header_base);
    RUN_TEST(chunker_last_chunk_has_no_extra_tail);
    RUN_TEST(chunker_callback_false_stops);
    RUN_TEST(chunker_rejects_invalid_arguments);
    return UNITY_END();
}
