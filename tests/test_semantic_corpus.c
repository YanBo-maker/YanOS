#include "semantic_corpus.h"
#include "unity.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    YanSemanticFile file;
    const uint8_t *bytes;
    uint8_t repeated;
} Entry;
typedef struct {
    Entry entries[64];
    uint32_t count, reads, nexts;
    bool read_fail, cursor_stall, flip;
    uint32_t fail_read_at;
    uint32_t flip_offset;
    uint8_t flip_byte;
} Fixture;
static Fixture fixture;
static YanSemanticCorpus corpus;
static YanSemanticSource source;

static YanSemanticResult next_file(void *opaque, uint32_t *cursor,
                                    YanSemanticFile *out)
{
    Fixture *f = opaque;
    ++f->nexts;
    if (*cursor >= f->count) return YAN_SEMANTIC_MISSING;
    *out = f->entries[*cursor].file;
    if (f->cursor_stall) return YAN_SEMANTIC_OK;
    ++*cursor;
    return YAN_SEMANTIC_OK;
}
static YanSemanticResult read_file(void *opaque, const char *name,
                                    uint32_t offset, uint8_t *out, uint32_t length)
{
    Fixture *f = opaque;
    ++f->reads;
    /* fail_read_at is 1-based; 0 disables it. At or after that read the
     * adapter fails, so a later file can fail after an earlier file already
     * updated the hash and text state. */
    if (f->read_fail || (f->fail_read_at && f->reads >= f->fail_read_at))
        return YAN_SEMANTIC_IO;
    for (uint32_t i = 0; i < f->count; ++i) {
        Entry *e = &f->entries[i];
        if (strcmp(name, e->file.name) != 0) continue;
        if (offset > e->file.size || length > e->file.size - offset)
            return YAN_SEMANTIC_IO;
        if (e->bytes) memcpy(out, e->bytes + offset, length);
        else memset(out, e->repeated, length);
        /* Deliberately alter one byte so a content change can be tested on
         * large repeated fixtures without allocating the whole file. */
        if (f->flip && f->flip_offset >= offset && f->flip_offset - offset < length)
            out[f->flip_offset - offset] = f->flip_byte;
        return YAN_SEMANTIC_OK;
    }
    return YAN_SEMANTIC_IO;
}
static void add(const char *name, uint32_t slot, const char *bytes)
{
    Entry *e = &fixture.entries[fixture.count++];
    memcpy(e->file.name, name, strlen(name) + 1);
    e->file.slot = slot;
    e->file.size = (uint32_t)strlen(bytes);
    e->bytes = (const uint8_t *)bytes;
}
static void reset_fixture(void)
{
    memset(&fixture, 0, sizeof fixture);
}
static void decode_hex(uint8_t out[32], const char *hex)
{
    for (unsigned i = 0; i < 32; ++i) {
        unsigned hi = (unsigned)(hex[2 * i] - '0');
        if (hi > 9) hi = hi - ('a' - '0') + 10;
        unsigned lo = (unsigned)(hex[2 * i + 1] - '0');
        if (lo > 9) lo = lo - ('a' - '0') + 10;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
}
static void assert_digest(const char *hex)
{
    uint8_t expected[32], actual[32];
    decode_hex(expected, hex);
    memset(actual, 0x5a, sizeof actual);
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_source_digest(&source, actual));
    TEST_ASSERT_EQUAL_MEMORY(expected, actual, sizeof actual);
}
static void expect_digest_failure(YanSemanticResult expected)
{
    uint8_t out[32];
    memset(out, 0xa5, sizeof out);
    TEST_ASSERT_EQUAL(expected, yan_semantic_source_digest(&source, out));
    for (unsigned i = 0; i < sizeof out; ++i)
        TEST_ASSERT_EQUAL_UINT8(0xa5, out[i]);
}

void setUp(void)
{
    memset(&fixture, 0, sizeof fixture);
    yan_semantic_corpus_release(&corpus);
    source = (YanSemanticSource){&fixture, sizeof fixture, next_file, read_file,
                                  63u};
}
void tearDown(void) { yan_semantic_corpus_release(&corpus); }

static void test_empty_and_order(void)
{
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_corpus_scan(&corpus, &source));
    TEST_ASSERT_EQUAL_UINT32(0, corpus.segment_count);
    add("z.txt", 4, "z\n"); add("a.txt", 1, "alpha\n");
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_corpus_scan(&corpus, &source));
    TEST_ASSERT_EQUAL_UINT32(2, corpus.segment_count);
    TEST_ASSERT_EQUAL_STRING("a.txt", corpus.segments[0].name);
    TEST_ASSERT_EQUAL_UINT32(6, corpus.segments[0].end);
    TEST_ASSERT_EQUAL_UINT32(1, corpus.segments[0].last_line);
    TEST_ASSERT_EQUAL_STRING("z.txt", corpus.segments[1].name);
}
static void test_metadata_body_and_line(void)
{
    const char *text = "@yan-note 1\ntitle: CPU\n@end\nhello\r\nworld\n";
    add("note", 0, text);
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_corpus_scan(&corpus, &source));
    TEST_ASSERT_EQUAL_UINT32(1, corpus.segment_count);
    TEST_ASSERT_EQUAL_UINT32(28, corpus.segments[0].start);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)strlen(text), corpus.segments[0].end);
    TEST_ASSERT_EQUAL_UINT32(4, corpus.segments[0].first_line);
    TEST_ASSERT_EQUAL_UINT32(5, corpus.segments[0].last_line);
    TEST_ASSERT_EQUAL_UINT16(3, corpus.files[0].title_length);
    TEST_ASSERT_EQUAL_MEMORY("CPU", corpus.files[0].title, 3);
}
static void test_whole_file_skip_before_bad_header(void)
{
    static uint8_t late[9000];
    memset(late, 'x', sizeof late);
    memcpy(late, "@yan-note 9\n", 12); late[8999] = 0;
    add("bad", 0, ""); fixture.entries[0].bytes = late;
    fixture.entries[0].file.size = sizeof late;
    add("good", 1, "good");
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_corpus_scan(&corpus, &source));
    TEST_ASSERT_EQUAL_UINT32(1, corpus.skipped);
    TEST_ASSERT_EQUAL_UINT32(1, corpus.segment_count);
    TEST_ASSERT_EQUAL_STRING("good", corpus.segments[0].name);
    late[8999] = 'x';
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_METADATA, yan_semantic_corpus_scan(&corpus, &source));
}
static void test_empty_title_only_and_blank_not_skipped(void)
{
    add("empty", 0, ""); add("title", 1, "@yan-note 1\ntitle: Title\n@end\n");
    add("blank", 2, " \t\r\n\xE3\x80\x80");
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_corpus_scan(&corpus, &source));
    TEST_ASSERT_EQUAL_UINT32(0, corpus.skipped);
    TEST_ASSERT_EQUAL_UINT32(0, corpus.segment_count);
}
static void test_identity_covers_slot_name_and_skipped_bytes(void)
{
    static const uint8_t binary[] = {0, 'a'};
    uint8_t old[32];
    add("a", 0, "good"); add("b", 1, "");
    fixture.entries[1].bytes = binary; fixture.entries[1].file.size = 2;
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_corpus_scan(&corpus, &source));
    memcpy(old, corpus.source_hash, 32);
    fixture.entries[1].file.slot = 2;
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_corpus_scan(&corpus, &source));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(old, corpus.source_hash, 32));
    memcpy(old, corpus.source_hash, 32);
    fixture.entries[1].bytes = (const uint8_t *)"\0b";
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_corpus_scan(&corpus, &source));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(old, corpus.source_hash, 32));
    memcpy(old, corpus.source_hash, 32);
    memcpy(fixture.entries[1].file.name, "c", 2);
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_corpus_scan(&corpus, &source));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(old, corpus.source_hash, 32));
}
static void test_exact_limit_and_next_segment(void)
{
    add("large", 0, ""); fixture.entries[0].bytes = NULL;
    fixture.entries[0].repeated = 'x';
    /* Non-CRLF 512-wide windows advance by 448; final covers EOF once. */
    fixture.entries[0].file.size = 512 + 8191 * 448;
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_corpus_scan(&corpus, &source));
    TEST_ASSERT_EQUAL_UINT32(8192, corpus.segment_count);
    ++fixture.entries[0].file.size;
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_LIMIT, yan_semantic_corpus_scan(&corpus, &source));
}
static void test_read_error_and_duplicate_slot(void)
{
    add("a", 0, "a"); fixture.read_fail = true;
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_IO, yan_semantic_corpus_scan(&corpus, &source));
    fixture.read_fail = false; add("b", 0, "b");
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_CORRUPT, yan_semantic_corpus_scan(&corpus, &source));
}
static void test_valid_header_with_large_streaming_body(void)
{
    static uint8_t text[28 + 50000];
    memcpy(text, "@yan-note 1\ntitle: CPU\n@end\n", 28);
    memset(text + 28, 'x', 50000);
    add("large", 0, ""); fixture.entries[0].bytes = text;
    fixture.entries[0].file.size = sizeof text;
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_corpus_scan(&corpus, &source));
    TEST_ASSERT_EQUAL_UINT32(0, corpus.skipped);
    TEST_ASSERT_EQUAL_UINT32(112, corpus.segment_count);
    TEST_ASSERT_EQUAL_UINT32(28, corpus.segments[0].start);
    TEST_ASSERT_EQUAL_UINT32(28 + 111 * 448, corpus.segments[111].start);
    TEST_ASSERT_EQUAL_UINT32(sizeof text, corpus.segments[111].end);
    TEST_ASSERT_EQUAL_UINT32(4, corpus.segments[111].first_line);
    TEST_ASSERT_EQUAL_UINT32(4, corpus.segments[111].last_line);
}
static void test_exact_limit_with_repeated_dual_crlf_adjustment(void)
{
    static uint8_t text[8191 * 446 + 512 + 1];
    memset(text, 'x', sizeof text);
    for (uint32_t i = 0; i < 8191; ++i) {
        uint32_t start = i * 446;
        text[start+446] = '\r'; text[start+447] = '\n';
        text[start+511] = '\r'; text[start+512] = '\n';
    }
    add("crlf", 0, ""); fixture.entries[0].bytes = text;
    fixture.entries[0].file.size = sizeof text - 1;
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_corpus_scan(&corpus, &source));
    TEST_ASSERT_EQUAL_UINT32(8192, corpus.segment_count);
    TEST_ASSERT_EQUAL_UINT32(511, corpus.segments[0].end);
    TEST_ASSERT_EQUAL_UINT32(446, corpus.segments[1].start);
    TEST_ASSERT_EQUAL_UINT32(8191 * 446, corpus.segments[8191].start);
    ++fixture.entries[0].file.size;
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_LIMIT, yan_semantic_corpus_scan(&corpus, &source));
}
static void test_digest_known_vectors(void)
{
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_corpus_scan(&corpus, &source));
    TEST_ASSERT_EQUAL_MEMORY("\xe3\xb0\xc4\x42\x98\xfc\x1c\x14\x9a\xfb\xf4\xc8"
        "\x99\x6f\xb9\x24\x27\xae\x41\xe4\x64\x9b\x93\x4c\xa4\x95\x99\x1b"
        "\x78\x52\xb8\x55", corpus.source_hash, 32);
    assert_digest("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    add("note", 0, "notes\n");
    assert_digest("c7021e371caa3073458d8292325a3c8a1fe9404f755bcd814062afab23eaad4c");
    uint8_t expected[32];
    decode_hex(expected, "c7021e371caa3073458d8292325a3c8a1fe9404f755bcd814062afab23eaad4c");
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_corpus_scan(&corpus, &source));
    TEST_ASSERT_EQUAL_MEMORY(expected, corpus.source_hash, 32);

    reset_fixture();
    add("z.txt", 4, "z\n"); add("a.txt", 1, "alpha\n");
    assert_digest("adcd84eb3cad758087027299b16913087a570a9baffa66400398bee1b12b2a32");

    reset_fixture();
    static const uint8_t binary[] = {0x00, 0xff, 0x80, 0x0a};
    add("bin", 0, ""); fixture.entries[0].bytes = binary; fixture.entries[0].file.size = 4;
    add("txt", 1, "hello");
    assert_digest("bb75bfe9ade4b13bf692f419f965563e45b1972359c992f6d1742c981e702228");
}
static void test_digest_matches_scan_and_tracks_source(void)
{
    static const uint8_t binary[] = {0x00, 0xff, 0x80, 0x0a};
    uint8_t first[32];
    add("note", 0, "notes\n");
    add("bin", 1, ""); fixture.entries[1].bytes = binary; fixture.entries[1].file.size = 4;
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_corpus_scan(&corpus, &source));
    TEST_ASSERT_EQUAL_UINT32(1, corpus.skipped);
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_source_digest(&source, first));
    TEST_ASSERT_EQUAL_MEMORY(corpus.source_hash, first, 32);

    /* Slot, name and binary content changes must move both identities. */
    fixture.entries[1].file.slot = 2;
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_corpus_scan(&corpus, &source));
    uint8_t next[32];
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_source_digest(&source, next));
    TEST_ASSERT_EQUAL_MEMORY(corpus.source_hash, next, 32);
    TEST_ASSERT_NOT_EQUAL(0, memcmp(first, next, 32));
    memcpy(first, next, 32);

    memcpy(fixture.entries[1].file.name, "bix", 4);
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_corpus_scan(&corpus, &source));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_source_digest(&source, next));
    TEST_ASSERT_EQUAL_MEMORY(corpus.source_hash, next, 32);
    TEST_ASSERT_NOT_EQUAL(0, memcmp(first, next, 32));

    memcpy(first, next, 32);
    static const uint8_t changed[] = {0x00, 0xfe, 0x80, 0x0a};
    fixture.entries[1].bytes = changed;
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_corpus_scan(&corpus, &source));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_source_digest(&source, next));
    TEST_ASSERT_EQUAL_MEMORY(corpus.source_hash, next, 32);
    TEST_ASSERT_NOT_EQUAL(0, memcmp(first, next, 32));
}
static void test_digest_ignores_bad_header_and_segment_limit(void)
{
    add("bad", 0, "@yan-note 9\n");
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_METADATA, yan_semantic_corpus_scan(&corpus, &source));
    assert_digest("533f835e2a3bef5ab934ebd57f66b8f9cb19c025c5e8ce76a04c3c8316345ca3");

    reset_fixture();
    add("large", 0, ""); fixture.entries[0].bytes = NULL;
    fixture.entries[0].repeated = 'x';
    fixture.entries[0].file.size = 512 + 8191 * 448 + 1;
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_LIMIT, yan_semantic_corpus_scan(&corpus, &source));
    uint8_t before[32], after[32];
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_source_digest(&source, before));
    fixture.flip = true; fixture.flip_offset = 12345; fixture.flip_byte = 'y';
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_LIMIT, yan_semantic_corpus_scan(&corpus, &source));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_source_digest(&source, after));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(before, after, 32));
}
static void test_digest_validation_and_unpublished_output(void)
{
    uint8_t out[32];

    add("a", 0, "a"); add("b", 0, "b");
    expect_digest_failure(YAN_SEMANTIC_CORRUPT);

    reset_fixture(); add("a/b", 0, "a");
    expect_digest_failure(YAN_SEMANTIC_CORRUPT);

    reset_fixture(); add("a", 0, "a"); add("a", 1, "b");
    expect_digest_failure(YAN_SEMANTIC_CORRUPT);

    reset_fixture(); add("a", 63, "a");
    expect_digest_failure(YAN_SEMANTIC_CORRUPT);

    reset_fixture();
    /* 63 legal files in slots 0..62 must be accepted; the next entry with an
     * in-range slot then hits the explicit file-count cap, not the slot check. */
    for (uint32_t i = 0; i < 63; ++i) {
        char name[8];
        snprintf(name, sizeof name, "f%02u", (unsigned)i);
        add(name, i, "x");
    }
    memset(out, 0x5a, sizeof out);
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_source_digest(&source, out));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_corpus_scan(&corpus, &source));
    TEST_ASSERT_EQUAL_UINT32(63, corpus.file_count);
    add("dup0", 0, "x");
    expect_digest_failure(YAN_SEMANTIC_CORRUPT);

    reset_fixture(); add("a", 0, "a"); fixture.cursor_stall = true;
    expect_digest_failure(YAN_SEMANTIC_CORRUPT);

    reset_fixture(); add("a", 0, "a"); fixture.read_fail = true;
    expect_digest_failure(YAN_SEMANTIC_IO);

    /* One 4096-byte file succeeds, then the second file's read fails. The
     * partially updated SHA state must not publish any output byte. */
    reset_fixture();
    add("a", 0, ""); fixture.entries[0].bytes = NULL;
    fixture.entries[0].repeated = 'a'; fixture.entries[0].file.size = 4096;
    add("b", 1, "b");
    fixture.fail_read_at = 2;
    expect_digest_failure(YAN_SEMANTIC_IO);
    TEST_ASSERT_EQUAL_UINT32(2, fixture.reads);

    reset_fixture();
    memset(out, 0xa5, sizeof out);
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID, yan_semantic_source_digest(&source, NULL));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID, yan_semantic_source_digest(NULL, out));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID,
                      yan_semantic_source_digest(&source, (uint8_t *)&source));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID,
                      yan_semantic_source_digest(&source, (uint8_t *)&fixture));
    for (unsigned i = 0; i < sizeof out; ++i)
        TEST_ASSERT_EQUAL_UINT8(0xa5, out[i]);

    YanSemanticSource broken = source;
    broken.next = NULL;
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID, yan_semantic_source_digest(&broken, out));
    broken = source; broken.read = NULL;
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID, yan_semantic_source_digest(&broken, out));
    broken = source; broken.context_size = SIZE_MAX;
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID, yan_semantic_source_digest(&broken, out));
}
/* 0036-R6: slot bounds come from the source's runtime capacity, not a constant
 * 63. A slot 1022 entry is legal for a 1023-capacity v2 image and corrupt for
 * a v1 image. */
static void test_runtime_capacity_source_slots(void)
{
    uint8_t out[32];
    add("deep", 1022, "x\n");
    source.file_capacity = 1023u;
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_corpus_scan(&corpus, &source));
    TEST_ASSERT_EQUAL_UINT32(1, corpus.file_count);
    TEST_ASSERT_EQUAL_UINT32(1022, corpus.files[0].file.slot);
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_source_digest(&source, out));

    /* The 4095 hard ceiling is a legal runtime capacity too. */
    fixture.entries[0].file.slot = 4094u;
    source.file_capacity = 4095u;
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_corpus_scan(&corpus, &source));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_source_digest(&source, out));

    source.file_capacity = 63u;
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_CORRUPT, yan_semantic_corpus_scan(&corpus, &source));
    memset(out, 0xa5, sizeof out);
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_CORRUPT, yan_semantic_source_digest(&source, out));
    for (unsigned i = 0; i < sizeof out; ++i)
        TEST_ASSERT_EQUAL_UINT8(0xa5, out[i]);
}
/* Invalid capacity (0, above 4095) is INVALID; a slot at or above the runtime
 * capacity is CORRUPT. Neither may silently fall back to 63. */
static void test_invalid_and_out_of_range_capacity(void)
{
    add("a", 0, "a\n");
    source.file_capacity = 0u;
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID, yan_semantic_corpus_scan(&corpus, &source));
    expect_digest_failure(YAN_SEMANTIC_INVALID);

    source.file_capacity = 4096u;
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID, yan_semantic_corpus_scan(&corpus, &source));
    expect_digest_failure(YAN_SEMANTIC_INVALID);

    source.file_capacity = 1023u;
    add("edge", 1023, "b\n");
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_CORRUPT, yan_semantic_corpus_scan(&corpus, &source));
    expect_digest_failure(YAN_SEMANTIC_CORRUPT);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_empty_and_order);
    RUN_TEST(test_metadata_body_and_line);
    RUN_TEST(test_whole_file_skip_before_bad_header);
    RUN_TEST(test_empty_title_only_and_blank_not_skipped);
    RUN_TEST(test_identity_covers_slot_name_and_skipped_bytes);
    RUN_TEST(test_exact_limit_and_next_segment);
    RUN_TEST(test_read_error_and_duplicate_slot);
    RUN_TEST(test_valid_header_with_large_streaming_body);
    RUN_TEST(test_exact_limit_with_repeated_dual_crlf_adjustment);
    RUN_TEST(test_digest_known_vectors);
    RUN_TEST(test_digest_matches_scan_and_tracks_source);
    RUN_TEST(test_digest_ignores_bad_header_and_segment_limit);
    RUN_TEST(test_digest_validation_and_unpublished_output);
    RUN_TEST(test_runtime_capacity_source_slots);
    RUN_TEST(test_invalid_and_out_of_range_capacity);
    return UNITY_END();
}
