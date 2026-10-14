#include "semantic_corpus.h"
#include "unity.h"
#include <string.h>

typedef struct {
    YanSemanticFile file;
    const uint8_t *bytes;
    uint8_t repeated;
} Entry;
typedef struct {
    Entry entries[63];
    uint32_t count, reads, nexts;
    bool read_fail;
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
    ++*cursor;
    return YAN_SEMANTIC_OK;
}
static YanSemanticResult read_file(void *opaque, const char *name,
                                    uint32_t offset, uint8_t *out, uint32_t length)
{
    Fixture *f = opaque;
    ++f->reads;
    if (f->read_fail) return YAN_SEMANTIC_IO;
    for (uint32_t i = 0; i < f->count; ++i) {
        Entry *e = &f->entries[i];
        if (strcmp(name, e->file.name) != 0) continue;
        if (offset > e->file.size || length > e->file.size - offset)
            return YAN_SEMANTIC_IO;
        if (e->bytes) memcpy(out, e->bytes + offset, length);
        else memset(out, e->repeated, length);
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
void setUp(void)
{
    memset(&fixture, 0, sizeof fixture);
    memset(&corpus, 0, sizeof corpus);
    source = (YanSemanticSource){&fixture, sizeof fixture, next_file, read_file};
}
void tearDown(void) {}

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
    return UNITY_END();
}
