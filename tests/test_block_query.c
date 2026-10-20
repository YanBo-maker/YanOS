#define _POSIX_C_SOURCE 200809L
#include "block_query.h"
#include "fs_test_support.h"
#include "unity.h"

#include <stdio.h>
#include <string.h>

#define BLOCKS 256u
static uint8_t medium[BLOCKS][4096];
static uint8_t alt_metadata[4096];
static uint8_t file_buffer[300000];
static YanFs store;
static YanSemanticPosix adapter;
static YanBlockQuery query;
static YanBlockQueryList list_result, list_sentinel;
static YanBlockQueryRefs refs_result, refs_sentinel;
static YanBlockQueryBacklinks backlinks_result, backlinks_sentinel;
static YanBlockQueryRead read_result, read_sentinel;

static uint32_t reads, writes, mounts;
static uint32_t fail_at;
static YanFsIoResult fail_code;
static bool change_metadata, fail_metadata, fail_data_after_second_mount;
static uint32_t change_lba, change_count, cancel_at_read;
static uint32_t fail_lba, fail_lba_at, fail_lba_reads;
static bool mutate_directory_on_data_read;
static uint32_t mutate_lba, mutate_reads;
static volatile sig_atomic_t cancel_flag;

static YanFsIoResult capacity(void *opaque, uint64_t *blocks)
{
    (void)opaque; *blocks = BLOCKS; return YAN_FS_IO_OK;
}
static YanFsIoResult store_read(void *opaque, uint32_t lba, uint8_t *out)
{
    (void)opaque;
    if (lba >= BLOCKS) return YAN_FS_IO_ERROR;
    memcpy(out, medium[lba], 4096);
    return YAN_FS_IO_OK;
}
static YanFsIoResult store_write(void *opaque, uint32_t lba, const uint8_t *in)
{
    (void)opaque;
    if (lba >= BLOCKS) return YAN_FS_IO_ERROR;
    memcpy(medium[lba], in, 4096);
    return YAN_FS_IO_OK;
}
static YanFsIoResult read_only_write(void *opaque, uint32_t lba, const uint8_t *in)
{
    (void)opaque; (void)lba; (void)in;
    ++writes;
    return YAN_FS_IO_ERROR;
}
static void fix_crc(uint8_t block[4096])
{
    uint32_t crc = yan_fs_metadata_crc(block);
    block[60] = (uint8_t)crc;
    block[61] = (uint8_t)(crc >> 8);
    block[62] = (uint8_t)(crc >> 16);
    block[63] = (uint8_t)(crc >> 24);
}
static YanFsIoResult fault_read(void *opaque, uint32_t lba, uint8_t *out)
{
    (void)opaque;
    ++reads;
    if (fail_at != 0u && reads == fail_at) return fail_code;
    if (fail_lba != 0u && lba == fail_lba) {
        ++fail_lba_reads;
        if (fail_lba_reads == fail_lba_at) return fail_code;
    }
    if (fail_data_after_second_mount && lba != 0u && mounts >= 2u)
        return fail_code;
    if (cancel_at_read != 0u && reads == cancel_at_read) cancel_flag = 1;
    if (lba >= BLOCKS) return YAN_FS_IO_ERROR;
    if (lba == 0u) {
        ++mounts;
        if (fail_metadata && mounts == 2u) return fail_code;
        if (change_metadata && mounts == 2u) {
            memcpy(out, alt_metadata, 4096);
            return YAN_FS_IO_OK;
        }
    }
    memcpy(out, medium[lba], 4096);
    if (change_lba != 0u && lba == change_lba) {
        ++change_count;
        if (change_count == 2u) out[0] = 'X';
    }
    /* Persistently replace the directory while the service is reading data,
     * after the pre-observation digest. Only a post-service remount can see
     * this change on the next observation. */
    if (mutate_directory_on_data_read && lba == mutate_lba) {
        ++mutate_reads;
        if (mutate_reads == 2u) memcpy(medium[0], alt_metadata, 4096);
    }
    return YAN_FS_IO_OK;
}
static void make(const char *name, const void *bytes, uint32_t length)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK,
        yan_fs_create(&store, name, bytes, length), "block_query_fixture_create");
}
static void text_file(const char *name, const char *bytes)
{
    make(name, bytes, (uint32_t)strlen(bytes));
}
static void block_file(const char *name, const char *id,
                       const uint8_t *content, uint32_t length)
{
    int prefix = snprintf((char *)file_buffer, sizeof file_buffer,
                          "@block %s\n", id);
    TEST_ASSERT_GREATER_THAN_INT(0, prefix);
    memcpy(file_buffer + prefix, content, length);
    make(name, file_buffer, (uint32_t)prefix + length);
}
static uint32_t blocks_text(uint8_t *out, uint32_t count)
{
    uint32_t used = 0;
    for (uint32_t i = 0; i < count; ++i) {
        int n = snprintf((char *)out + used, 64u, "@block b%02u\nx\n", (unsigned)i);
        TEST_ASSERT_GREATER_THAN_INT(0, n);
        used += (uint32_t)n;
    }
    return used;
}
static void build_alt_metadata(void)
{
    memcpy(alt_metadata, medium[0], sizeof alt_metadata);
    alt_metadata[64] = 'Q'; /* slot 0's first name byte; stays a valid name */
    fix_crc(alt_metadata);
}
static void reset_hooks(void)
{
    reads = writes = mounts = 0;
    fail_at = 0; fail_code = YAN_FS_IO_ERROR;
    change_metadata = fail_metadata = fail_data_after_second_mount = false;
    change_lba = change_count = cancel_at_read = 0;
    fail_lba = fail_lba_at = fail_lba_reads = 0;
    mutate_directory_on_data_read = false;
    mutate_lba = mutate_reads = 0;
    cancel_flag = 0;
}
static void adapter_mount(void)
{
    memset(&adapter, 0, sizeof adapter);
    adapter.image_fd = adapter.index_fd = -1;
    YanFsBlockIo io = {NULL, capacity, fault_read, read_only_write};
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, test_fs_init(&adapter.fs, io));
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_mount(&adapter.fs));
    yan_block_query_destroy(&query);
    memset(&query, 0, sizeof query);
    reset_hooks();
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_OK,
        yan_block_query_init(&query, &adapter, &cancel_flag));
    TEST_ASSERT_EQUAL_INT(-1, adapter.image_fd);
}
void setUp(void)
{
    memset(medium, 0, sizeof medium);
    memset(alt_metadata, 0, sizeof alt_metadata);
    memset(&store, 0, sizeof store);
    memset(&list_result, 0xA5, sizeof list_result); list_sentinel = list_result;
    memset(&refs_result, 0xA5, sizeof refs_result); refs_sentinel = refs_result;
    memset(&backlinks_result, 0xA5, sizeof backlinks_result);
    backlinks_sentinel = backlinks_result;
    memset(&read_result, 0xA5, sizeof read_result); read_sentinel = read_result;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_format_metadata(medium[0], BLOCKS));
    YanFsBlockIo io = {NULL, capacity, store_read, store_write};
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, test_fs_init(&store, io));
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_mount(&store));
}
void tearDown(void)
{
    yan_block_query_destroy(&query);
}
static void list_fresh(const char *name, const uint8_t expected[32])
{
    memset(&list_result, 0xA5, sizeof list_result);
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_OK,
        yan_block_query_list(&query, name, expected, &list_result));
}
static void refs_fresh(const char *name)
{
    memset(&refs_result, 0xA5, sizeof refs_result);
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_OK,
        yan_block_query_refs(&query, name, NULL, &refs_result));
}
static void backlinks_fresh(const char *name, const char *id)
{
    memset(&backlinks_result, 0xA5, sizeof backlinks_result);
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_OK,
        yan_block_query_backlinks(&query, name, id, NULL, &backlinks_result));
}
static YanBlockQueryResult read_fresh(const char *name, const char *id,
                                      uint32_t offset)
{
    memset(&read_result, 0xA5, sizeof read_result);
    return yan_block_query_read(&query, name, id, offset, NULL, &read_result);
}
static void assert_list_untouched(void)
{
    TEST_ASSERT_EQUAL_MEMORY(&list_sentinel, &list_result, sizeof list_result);
}
static void assert_read_untouched(void)
{
    TEST_ASSERT_EQUAL_MEMORY(&read_sentinel, &read_result, sizeof read_result);
}
static void assert_refs_untouched(void)
{
    TEST_ASSERT_EQUAL_MEMORY(&refs_sentinel, &refs_result, sizeof refs_result);
}
static void assert_backlinks_untouched(void)
{
    TEST_ASSERT_EQUAL_MEMORY(&backlinks_sentinel, &backlinks_result,
                             sizeof backlinks_result);
}
static uint32_t count_lf(const uint8_t *bytes, uint32_t start, uint32_t end)
{
    uint32_t count = 0;
    for (uint32_t i = start; i < end; ++i) if (bytes[i] == '\n') ++count;
    return count;
}

static void test_list_ranges_header_and_empty(void)
{
    const char *data = "@yan-note 1\ntitle: T\n@end\npreamble\n"
                       "@block first\r\nA\r\nB\n@block empty\n@block end";
    text_file("note", data);
    adapter_mount();
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_OK,
        yan_block_query_list(&query, "note", NULL, &list_result));
    TEST_ASSERT_EQUAL_UINT64(3, list_result.total);
    TEST_ASSERT_EQUAL_UINT32(3, list_result.shown);
    TEST_ASSERT_EQUAL_STRING("first", list_result.blocks[0].id);
    TEST_ASSERT_EQUAL_UINT32(5, list_result.blocks[0].marker_line);
    TEST_ASSERT_EQUAL_UINT32(6, list_result.blocks[0].first_line);
    TEST_ASSERT_EQUAL_UINT32(7, list_result.blocks[0].last_line);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(strstr(data, "A\r\n") - data),
                             list_result.blocks[0].start);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(strstr(data, "@block empty") - data),
                             list_result.blocks[0].end);
    TEST_ASSERT_EQUAL_UINT32(0, list_result.blocks[1].first_line);
    TEST_ASSERT_EQUAL_UINT32(0, list_result.blocks[1].last_line);
    TEST_ASSERT_EQUAL_UINT32(list_result.blocks[1].start, list_result.blocks[1].end);
    TEST_ASSERT_EQUAL_UINT32(0, list_result.blocks[2].first_line);
    TEST_ASSERT_EQUAL_UINT32(0, list_result.blocks[2].last_line);
    YanSemanticSource digest_source;
    uint8_t expected_digest[32];
    yan_semantic_posix_bind_source(&adapter, &digest_source);
    TEST_ASSERT_EQUAL_INT(YAN_SEMANTIC_OK,
        yan_semantic_source_digest(&digest_source, expected_digest));
    TEST_ASSERT_EQUAL_MEMORY(expected_digest, query.source_sha256, 32);
}
static void test_list_64_65_and_duplicate(void)
{
    uint8_t text[64u * 20u];
    uint32_t used = blocks_text(text, 64);
    make("many", text, used);
    adapter_mount();
    list_fresh("many", NULL);
    TEST_ASSERT_EQUAL_UINT64(64, list_result.total);
    TEST_ASSERT_EQUAL_UINT32(64, list_result.shown);
    TEST_ASSERT_EQUAL_STRING("b00", list_result.blocks[0].id);
    TEST_ASSERT_EQUAL_STRING("b63", list_result.blocks[63].id);

    used = blocks_text(text, 65);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_create(&store, "too_many", text, used));
    memset(&list_result, 0xA5, sizeof list_result);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_BLOCK_QUERY_LIMIT,
        yan_block_query_list(&query, "too_many", NULL, &list_result), "block_limit_map");
    assert_list_untouched();

    text_file("duplicate", "@block same\nx\n@block same\n");
    memset(&list_result, 0xA5, sizeof list_result);
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_AMBIGUOUS,
        yan_block_query_list(&query, "duplicate", NULL, &list_result));
    assert_list_untouched();
}
static void test_read_pages_and_terminal(void)
{
    static uint8_t content[65537];
    memset(content, 'A', sizeof content);
    block_file("p65535", "a", content, 65535);
    block_file("p65536", "a", content, 65536);
    block_file("p65537", "a", content, 65537);
    adapter_mount();

    memset(&read_result, 0xA5, sizeof read_result);
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_OK,
        yan_block_query_read(&query, "p65535", "a", 0, NULL, &read_result));
    TEST_ASSERT_EQUAL_UINT32(65535, read_result.text_length);
    TEST_ASSERT_TRUE(read_result.complete);
    TEST_ASSERT_EQUAL_UINT32(0, read_result.next_offset);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(read_result.page_start, read_result.block_start,
        "block_absolute_page_offset");

    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_OK, read_fresh("p65536", "a", 0));
    TEST_ASSERT_EQUAL_UINT32(65536, read_result.text_length);
    TEST_ASSERT_TRUE(read_result.complete);
    TEST_ASSERT_EQUAL_UINT32(read_result.block_start + 65536, read_result.page_end);

    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_OK, read_fresh("p65537", "a", 0));
    TEST_ASSERT_EQUAL_UINT32(65536, read_result.text_length);
    TEST_ASSERT_FALSE(read_result.complete);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(65536, read_result.next_offset,
        "block_relative_next_offset");
    TEST_ASSERT_GREATER_THAN_UINT32(0, read_result.next_offset);
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_OK, read_fresh("p65537", "a", 65536));
    TEST_ASSERT_EQUAL_UINT32(1, read_result.text_length);
    TEST_ASSERT_TRUE(read_result.complete);
    TEST_ASSERT_EQUAL_UINT8('A', read_result.text[0]);

    /* offset == block length is the allowed empty terminal page. */
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_OK, read_fresh("p65537", "a", 65537));
    TEST_ASSERT_EQUAL_UINT32(0, read_result.text_length);
    TEST_ASSERT_TRUE(read_result.complete);
    TEST_ASSERT_EQUAL_UINT32(read_result.block_end, read_result.page_start);
    TEST_ASSERT_EQUAL_UINT32(0, read_result.page_first_line);
    TEST_ASSERT_EQUAL_UINT32(0, read_result.page_last_line);
}
static void test_read_multipage_and_lines(void)
{
    static uint8_t content[100000];
    for (uint32_t i = 0; i < sizeof content; ++i)
        content[i] = (uint8_t)(i % 1000u == 999u ? '\n' : 'x');
    block_file("lines", "a", content, sizeof content);
    adapter_mount();
    uint32_t offset = 0, joined = 0;
    static uint8_t joined_bytes[sizeof content];
    for (;;) {
        TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_OK, read_fresh("lines", "a", offset));
        TEST_ASSERT_TRUE(read_result.page_start >= read_result.block_start);
        uint32_t page_rel = read_result.page_start - read_result.block_start;
        uint32_t expected_first = 2u + count_lf(content, 0, page_rel);
        uint32_t expected_last = expected_first +
            count_lf(content, page_rel, read_result.page_end -
                      read_result.block_start - 1u);
        TEST_ASSERT_EQUAL_UINT32(expected_first, read_result.page_first_line);
        TEST_ASSERT_EQUAL_UINT32(expected_last, read_result.page_last_line);
        for (uint32_t i = 0; i < read_result.text_length; ++i)
            joined_bytes[joined++] = read_result.text[i];
        if (read_result.complete) break;
        TEST_ASSERT_TRUE(read_result.next_offset > offset);
        TEST_ASSERT_EQUAL_UINT32(read_result.page_end - read_result.block_start,
                                 read_result.next_offset);
        offset = read_result.next_offset;
    }
    TEST_ASSERT_EQUAL_UINT32(sizeof content, joined);
    TEST_ASSERT_EQUAL_MEMORY(content, joined_bytes, sizeof content);
}
static void test_read_utf8_boundaries(void)
{
    static const uint8_t two[] = {0xc3, 0xa9};
    static const uint8_t three[] = {0xe4, 0xb8, 0xad};
    static const uint8_t four[] = {0xf0, 0x9f, 0x98, 0x80};
    static uint8_t content[65548];
    const uint8_t *scalars[] = {two, three, four};
    const uint32_t widths[] = {2, 3, 4};
    const char *names[] = {"u2", "u3", "u4"};
    for (uint32_t k = 0; k < 3; ++k) {
        uint32_t start = 65536u - (widths[k] - 1u);
        memset(content, 'a', start);
        memcpy(content + start, scalars[k], widths[k]);
        memcpy(content + start + widths[k], "tail", 4);
        block_file(names[k], "a", content, start + widths[k] + 4);
    }
    adapter_mount();
    for (uint32_t k = 0; k < 3; ++k) {
        uint32_t start = 65536u - (widths[k] - 1u);
        memset(content, 'a', start);
        memcpy(content + start, scalars[k], widths[k]);
        memcpy(content + start + widths[k], "tail", 4);
        TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_OK, read_fresh(names[k], "a", 0));
        TEST_ASSERT_EQUAL_UINT32(start, read_result.text_length);
        TEST_ASSERT_FALSE(read_result.complete);
        TEST_ASSERT_EQUAL_UINT32(start, read_result.next_offset);
        TEST_ASSERT_EQUAL_MEMORY(content, read_result.text, start);
        TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_OK,
            read_fresh(names[k], "a", start));
        TEST_ASSERT_EQUAL_MEMORY(scalars[k], read_result.text, widths[k]);
        TEST_ASSERT_EQUAL_UINT32(widths[k] + 4, read_result.text_length);
        TEST_ASSERT_TRUE(read_result.complete);
    }
    /* P inside a continuation byte must be rejected before any page. */
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_INVALID, read_fresh("u3", "a", 65535));
    assert_read_untouched();
}
static void test_read_crlf_boundary(void)
{
    static uint8_t content[65540];
    memset(content, 'a', 65535);
    content[65535] = '\r'; content[65536] = '\n'; content[65537] = 'b';
    block_file("crlf", "a", content, 65538);
    adapter_mount();
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_OK, read_fresh("crlf", "a", 0));
    TEST_ASSERT_EQUAL_UINT32(65535, read_result.text_length);
    TEST_ASSERT_EQUAL_UINT32(65535, read_result.next_offset);
    TEST_ASSERT_EQUAL_UINT32(2, read_result.page_first_line);
    TEST_ASSERT_EQUAL_UINT32(2, read_result.page_last_line);
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_OK, read_fresh("crlf", "a", 65535));
    TEST_ASSERT_EQUAL_UINT32(3, read_result.text_length);
    TEST_ASSERT_EQUAL_MEMORY("\r\nb", read_result.text, 3);
    TEST_ASSERT_TRUE(read_result.complete);
    TEST_ASSERT_EQUAL_UINT32(2, read_result.page_first_line);
    TEST_ASSERT_EQUAL_UINT32(3, read_result.page_last_line);
    /* P between CR and LF is INVALID. */
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_INVALID, read_fresh("crlf", "a", 65536));
    assert_read_untouched();
}
static void test_read_long_single_line(void)
{
    static uint8_t content[100000];
    memset(content, 'x', sizeof content);
    block_file("long", "a", content, sizeof content);
    adapter_mount();
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_OK, read_fresh("long", "a", 0));
    TEST_ASSERT_EQUAL_UINT32(65536, read_result.text_length);
    TEST_ASSERT_EQUAL_UINT32(2, read_result.page_first_line);
    TEST_ASSERT_EQUAL_UINT32(2, read_result.page_last_line);
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_OK, read_fresh("long", "a", 65536));
    TEST_ASSERT_EQUAL_UINT32(sizeof content - 65536, read_result.text_length);
    TEST_ASSERT_TRUE(read_result.complete);
    TEST_ASSERT_EQUAL_UINT32(2, read_result.page_first_line);
    TEST_ASSERT_EQUAL_UINT32(2, read_result.page_last_line);
}
static void test_empty_block_and_missing(void)
{
    text_file("note", "@block empty\n@block next\nx");
    adapter_mount();
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_OK, read_fresh("note", "empty", 0));
    TEST_ASSERT_EQUAL_UINT32(0, read_result.text_length);
    TEST_ASSERT_TRUE(read_result.complete);
    TEST_ASSERT_EQUAL_UINT32(0, read_result.page_first_line);
    TEST_ASSERT_EQUAL_UINT32(0, read_result.page_last_line);
    TEST_ASSERT_EQUAL_UINT32(read_result.block_start, read_result.block_end);
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_NOT_FOUND, read_fresh("note", "gone", 0));
    assert_read_untouched();
}
static void test_refs_and_target_states(void)
{
    text_file("target", "@block a\nx");
    text_file("missingblock", "@block b\nx");
    uint32_t used = 0;
    for (uint32_t i = 0; i < 25; ++i) {
        memcpy(file_buffer + used, "[[target#a]] ", 13);
        used += 13;
    }
    make("source", file_buffer, used);
    text_file("ambig", "@block a\n@block a\n");
    used = 0;
    for (uint32_t i = 0; i < 20; ++i) {
        memcpy(file_buffer + used, "[[target#a]] ", 13);
        used += 13;
    }
    memcpy(file_buffer + used, "[[ambig#a]]", 11);
    used += 11;
    make("late", file_buffer, used);
    adapter_mount();
    refs_fresh("source");
    TEST_ASSERT_EQUAL_UINT64(25, refs_result.total);
    TEST_ASSERT_EQUAL_UINT32(20, refs_result.shown);
    TEST_ASSERT_EQUAL_STRING("source", refs_result.rows[0].source);
    TEST_ASSERT_EQUAL_STRING("target", refs_result.rows[0].reference.target.name);
    TEST_ASSERT_EQUAL_STRING("a", refs_result.rows[0].reference.target.id);
    TEST_ASSERT_EQUAL_UINT32(0, refs_result.rows[0].reference.start);
    TEST_ASSERT_EQUAL_UINT32(12, refs_result.rows[0].reference.end);
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_TARGET_OK, refs_result.rows[0].state);

    /* The 21st target is a bad block declaration, but only the shown 20 are
     * validated, so refs still succeeds with total 21 and shown 20. */
    refs_fresh("late");
    TEST_ASSERT_EQUAL_UINT64(21, refs_result.total);
    TEST_ASSERT_EQUAL_UINT32(20, refs_result.shown);
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_TARGET_OK, refs_result.rows[19].state);

    text_file("gone", "[[missing#a]][[missingblock#missing]]");
    refs_fresh("gone");
    TEST_ASSERT_EQUAL_UINT64(2, refs_result.total);
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_DANGLING_FILE, refs_result.rows[0].state);
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_DANGLING_BLOCK, refs_result.rows[1].state);
}
static void test_backlinks_order_skip_and_target(void)
{
    text_file("target", "@block a\nx");
    text_file("A", "@block s\n[[gone#a]][[gone#a]]");
    text_file("z", "[[gone#a]]");
    const uint8_t binary[] = {'[', '[', 'g', 'o', 'n', 'e', '#', 'a', ']', ']', 0};
    make("binary", binary, sizeof binary);
    adapter_mount();
    backlinks_fresh("gone", "a");
    TEST_ASSERT_EQUAL_UINT64(3, backlinks_result.total);
    TEST_ASSERT_EQUAL_UINT32(3, backlinks_result.shown);
    TEST_ASSERT_EQUAL_UINT32(1, backlinks_result.skipped);
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_DANGLING_FILE, backlinks_result.target_state);
    TEST_ASSERT_EQUAL_STRING("A", backlinks_result.rows[0].source);
    TEST_ASSERT_EQUAL_STRING("z", backlinks_result.rows[2].source);
    TEST_ASSERT_EQUAL_STRING("s", backlinks_result.rows[0].reference.source_id);
    TEST_ASSERT_TRUE(backlinks_result.rows[0].reference.has_source_id);

    backlinks_fresh("target", "a");
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_TARGET_OK, backlinks_result.target_state);
}
static void test_backlinks_top20_and_header_positions(void)
{
    text_file("target", "@block a\nx");
    text_file("hdr", "@block hdr\nx");
    uint32_t used = 0;
    for (uint32_t i = 0; i < 15; ++i) {
        memcpy(file_buffer + used, "[[target#a]] ", 13);
        used += 13;
    }
    make("A", file_buffer, used);
    used = 0;
    for (uint32_t i = 0; i < 10; ++i) {
        memcpy(file_buffer + used, "[[target#a]] ", 13);
        used += 13;
    }
    make("z", file_buffer, used);
    const char *hsrc = "@yan-note 1\ntitle: T\n@end\n@block s\n[[hdr#hdr]]\n";
    text_file("headersrc", hsrc);
    adapter_mount();

    backlinks_fresh("target", "a");
    TEST_ASSERT_EQUAL_UINT64(25, backlinks_result.total);
    TEST_ASSERT_EQUAL_UINT32(20, backlinks_result.shown);
    TEST_ASSERT_EQUAL_STRING("A", backlinks_result.rows[0].source);
    TEST_ASSERT_EQUAL_STRING("A", backlinks_result.rows[14].source);
    TEST_ASSERT_EQUAL_STRING("z", backlinks_result.rows[15].source);
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_TARGET_OK, backlinks_result.target_state);

    refs_fresh("headersrc");
    TEST_ASSERT_EQUAL_UINT64(1, refs_result.total);
    TEST_ASSERT_EQUAL_UINT32(5, refs_result.rows[0].reference.line);
    uint32_t header_start = (uint32_t)(strstr(hsrc, "[[hdr#hdr]]") - hsrc);
    TEST_ASSERT_EQUAL_UINT32(header_start, refs_result.rows[0].reference.start);
    TEST_ASSERT_EQUAL_UINT32(header_start + 11,
                             refs_result.rows[0].reference.end);
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_TARGET_OK, refs_result.rows[0].state);

    backlinks_fresh("hdr", "hdr");
    TEST_ASSERT_EQUAL_UINT64(1, backlinks_result.total);
    TEST_ASSERT_EQUAL_UINT32(1, backlinks_result.shown);
    TEST_ASSERT_EQUAL_STRING("headersrc", backlinks_result.rows[0].source);
    TEST_ASSERT_EQUAL_UINT32(5, backlinks_result.rows[0].reference.line);
    TEST_ASSERT_EQUAL_UINT32(header_start, backlinks_result.rows[0].reference.start);
    TEST_ASSERT_EQUAL_UINT32(header_start + 11,
                             backlinks_result.rows[0].reference.end);
}
static void test_error_scope_and_invalid_inputs(void)
{
    text_file("bad", "@yan-note 9\n@end\n");
    const uint8_t binary[] = "@block a\n\0";
    make("binary", binary, sizeof binary);
    adapter_mount();
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_NOT_FOUND,
        yan_block_query_list(&query, "missing", NULL, &list_result));
    assert_list_untouched();
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_METADATA,
        yan_block_query_list(&query, "bad", NULL, &list_result));
    assert_list_untouched();
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_INVALID_TEXT,
        yan_block_query_list(&query, "binary", NULL, &list_result));
    assert_list_untouched();

    /* A service error must win over a failing post observation: with the
     * second mount deliberately broken, these still return the service token
     * and never reach mount #2. */
    reset_hooks(); fail_metadata = true;
    memset(&list_result, 0xA5, sizeof list_result);
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_NOT_FOUND,
        yan_block_query_list(&query, "missing", NULL, &list_result));
    assert_list_untouched();
    TEST_ASSERT_EQUAL_UINT32(1, mounts);
    reset_hooks(); fail_metadata = true;
    memset(&list_result, 0xA5, sizeof list_result);
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_METADATA,
        yan_block_query_list(&query, "bad", NULL, &list_result));
    assert_list_untouched();
    TEST_ASSERT_EQUAL_UINT32(1, mounts);
    reset_hooks(); fail_data_after_second_mount = true;
    memset(&list_result, 0xA5, sizeof list_result);
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_INVALID_TEXT,
        yan_block_query_list(&query, "binary", NULL, &list_result));
    assert_list_untouched();
    TEST_ASSERT_EQUAL_UINT32(1, mounts);
    /* Clear the injected post-mount failure before later subcases, or the next
     * call's observation A would itself become mount #2. */
    reset_hooks();

    reads = 0;
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_INVALID,
        yan_block_query_list(&query, "a/b", NULL, &list_result));
    assert_list_untouched();
    TEST_ASSERT_EQUAL_UINT32(0, reads);
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_INVALID,
        yan_block_query_read(&query, "bad", "a/b", 0, NULL, &read_result));
    assert_read_untouched();
    TEST_ASSERT_EQUAL_UINT32(0, reads);
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_INVALID,
        yan_block_query_list(&query, "bad", NULL, NULL));
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_INVALID,
        yan_block_query_list(NULL, "bad", NULL, &list_result));
    YanBlockQuery empty;
    memset(&empty, 0, sizeof empty);
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_INVALID,
        yan_block_query_list(&empty, "bad", NULL, &list_result));

    uint8_t wrong[32];
    memset(wrong, 0x42, sizeof wrong);
    text_file("badref", "[[bad#a]]");
    memset(&refs_result, 0xA5, sizeof refs_result);
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_NOT_FOUND,
        yan_block_query_refs(&query, "missing", NULL, &refs_result));
    assert_refs_untouched();
    memset(&refs_result, 0xA5, sizeof refs_result);
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_METADATA,
        yan_block_query_refs(&query, "badref", NULL, &refs_result));
    assert_refs_untouched();
    reset_hooks(); fail_metadata = true;
    memset(&refs_result, 0xA5, sizeof refs_result);
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_METADATA,
        yan_block_query_refs(&query, "badref", NULL, &refs_result));
    assert_refs_untouched();
    TEST_ASSERT_EQUAL_UINT32(1, mounts);
    reset_hooks();
    memset(&refs_result, 0xA5, sizeof refs_result);
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_STALE,
        yan_block_query_refs(&query, "bad", wrong, &refs_result));
    assert_refs_untouched();

    memset(&backlinks_result, 0xA5, sizeof backlinks_result);
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_METADATA,
        yan_block_query_backlinks(&query, "bad", "a", NULL, &backlinks_result));
    assert_backlinks_untouched();
    reset_hooks(); fail_metadata = true;
    memset(&backlinks_result, 0xA5, sizeof backlinks_result);
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_METADATA,
        yan_block_query_backlinks(&query, "bad", "a", NULL, &backlinks_result));
    assert_backlinks_untouched();
    TEST_ASSERT_EQUAL_UINT32(1, mounts);
    reset_hooks();
    memset(&backlinks_result, 0xA5, sizeof backlinks_result);
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_STALE,
        yan_block_query_backlinks(&query, "bad", "a", wrong,
                                  &backlinks_result));
    assert_backlinks_untouched();
}
static void test_expected_source_success_and_digest_publication(void)
{
    text_file("target", "@block a\nx");
    text_file("source", "@block s\n[[target#a]]");
    adapter_mount();
    YanSemanticSource digest_source;
    uint8_t expected[32], wrong[32];
    yan_semantic_posix_bind_source(&adapter, &digest_source);
    TEST_ASSERT_EQUAL_INT(YAN_SEMANTIC_OK,
        yan_semantic_source_digest(&digest_source, expected));
    memset(wrong, 0x42, sizeof wrong);

    memset(&list_result, 0xA5, sizeof list_result);
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_OK,
        yan_block_query_list(&query, "source", expected, &list_result));
    TEST_ASSERT_EQUAL_MEMORY(expected, query.source_sha256, 32);
    memset(&read_result, 0xA5, sizeof read_result);
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_OK,
        yan_block_query_read(&query, "source", "s", 0, expected, &read_result));
    memset(&refs_result, 0xA5, sizeof refs_result);
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_OK,
        yan_block_query_refs(&query, "source", expected, &refs_result));
    memset(&backlinks_result, 0xA5, sizeof backlinks_result);
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_OK,
        yan_block_query_backlinks(&query, "target", "a", expected,
                                  &backlinks_result));

    /* A failing call must not publish a new digest. */
    memset(query.source_sha256, 0xA5, sizeof query.source_sha256);
    memset(&list_result, 0xA5, sizeof list_result);
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_STALE,
        yan_block_query_list(&query, "source", wrong, &list_result));
    assert_list_untouched();
    for (unsigned i = 0; i < sizeof query.source_sha256; ++i)
        TEST_ASSERT_EQUAL_UINT8(0xA5, query.source_sha256[i]);
}
static void test_stale_expected_and_source_change(void)
{
    uint8_t wrong[32];
    memset(wrong, 0x42, sizeof wrong);
    text_file("note", "@block a\nAAAA");
    adapter_mount();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_BLOCK_QUERY_STALE,
        yan_block_query_list(&query, "note", wrong, &list_result), "block_stale_identity");
    assert_list_untouched();

    /* A changed directory block on the post-observation mount is STALE. */
    text_file("alpha", "@block a\nx");
    text_file("beta", "@block b\ny");
    build_alt_metadata();
    change_metadata = true;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_BLOCK_QUERY_STALE,
        yan_block_query_list(&query, "alpha", NULL, &list_result), "block_stale_identity");
    assert_list_untouched();

    /* A data block changed between observations is STALE too. */
    reset_hooks();
    change_lba = 1; /* first file's data starts at block 1 */
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_STALE,
        yan_block_query_list(&query, "alpha", NULL, &list_result));
    assert_list_untouched();
}
static void test_post_observation_after_service(void)
{
    text_file("alpha", "@block a\nAAAA");
    text_file("beta", "@block b\nBBBB");
    adapter_mount();
    build_alt_metadata();
    /* Mutate the persistent directory during the service's data read, after
     * the pre digest. A correct implementation uses cached metadata for the
     * service, then its post-service remount sees the new directory and
     * returns STALE. A pre-only implementation compares the old directory
     * twice, runs the service and returns OK. */
    mutate_directory_on_data_read = true;
    mutate_lba = 1; /* alpha is the first file, so its data starts at block 1 */
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_BLOCK_QUERY_STALE,
        yan_block_query_list(&query, "alpha", NULL, &list_result), "block_stale_identity");
    assert_list_untouched();
    TEST_ASSERT_TRUE(mutate_reads >= 2);
}
static void test_observation_errors_and_cancel(void)
{
    text_file("note", "@block a\nAAAA");
    adapter_mount();
    fail_at = 1;
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_IO,
        yan_block_query_list(&query, "note", NULL, &list_result));
    assert_list_untouched();

    reset_hooks();
    fail_metadata = true;
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_IO,
        yan_block_query_list(&query, "note", NULL, &list_result));
    assert_list_untouched();

    reset_hooks();
    fail_data_after_second_mount = true;
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_IO,
        yan_block_query_list(&query, "note", NULL, &list_result));
    assert_list_untouched();

    reset_hooks();
    cancel_flag = 1;
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_CANCELLED,
        yan_block_query_list(&query, "note", NULL, &list_result));
    assert_list_untouched();
    TEST_ASSERT_EQUAL_UINT32(0, reads);

    reset_hooks();
    cancel_at_read = 3;
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_CANCELLED,
        yan_block_query_list(&query, "note", NULL, &list_result));
    assert_list_untouched();

    /* Page-focused late failures: the block service completed and buffered
     * the page, then the post-observation data read or mount fails. */
    reset_hooks();
    fail_data_after_second_mount = true;
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_IO, read_fresh("note", "a", 0));
    assert_read_untouched();
    reset_hooks();
    fail_metadata = true;
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_IO, read_fresh("note", "a", 0));
    assert_read_untouched();
    reset_hooks();
    cancel_at_read = 3;
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_CANCELLED,
                          read_fresh("note", "a", 0));
    assert_read_untouched();
}
static void test_read_count_and_no_writes(void)
{
    text_file("note", "@block a\nAAAA");
    adapter_mount();
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_OK, read_fresh("note", "a", 0));
    /* A mount + digest; service text, prefix and parser passes plus one show
     * chunk; B mount + digest. No extra whole-file identity pass. */
    TEST_ASSERT_EQUAL_UINT32(8, reads);
    TEST_ASSERT_EQUAL_UINT32(2, mounts);
    TEST_ASSERT_EQUAL_UINT32(0, writes);
}
static void test_read_late_show_io_preserves_output(void)
{
    static uint8_t content[80000];
    memset(content, 'x', sizeof content);
    block_file("lateio", "a", content, sizeof content);
    adapter_mount();
    /* lba 18 is first reached in the show chunk after the 65540-byte page
     * window was already collected (file_scan reads it twice earlier). Failing
     * its third read must surface IO, not a successful prefix. */
    fail_lba = 18; fail_lba_at = 3;
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_IO, read_fresh("lateio", "a", 0));
    assert_read_untouched();
}
static void test_cancel_interrupts_service_preflight(void)
{
    static uint8_t content[200000];
    memset(content, 'a', sizeof content);
    block_file("long", "a", content, sizeof content);
    adapter_mount();

    for (unsigned operation = 0; operation < 4; ++operation) {
        reset_hooks();
        cancel_at_read = 52;
        memset(query.source_sha256, 0xA5, sizeof query.source_sha256);
        YanBlockQueryResult result = YAN_BLOCK_QUERY_INVALID;
        if (operation == 0) {
            memset(&list_result, 0xA5, sizeof list_result);
            result = yan_block_query_list(&query, "long", NULL, &list_result);
        } else if (operation == 1) {
            result = read_fresh("long", "a", 0);
        } else if (operation == 2) {
            memset(&refs_result, 0xA5, sizeof refs_result);
            result = yan_block_query_refs(&query, "long", NULL, &refs_result);
        } else {
            memset(&backlinks_result, 0xA5, sizeof backlinks_result);
            result = yan_block_query_backlinks(&query, "long", "a", NULL,
                                               &backlinks_result);
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_BLOCK_QUERY_CANCELLED, result,
            "block_preflight_cancel");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(52, reads, "block_preflight_cancel");
        TEST_ASSERT_EQUAL_UINT32(0, writes);
        if (operation == 0) assert_list_untouched();
        else if (operation == 1) assert_read_untouched();
        else if (operation == 2) assert_refs_untouched();
        else assert_backlinks_untouched();
        for (unsigned i = 0; i < sizeof query.source_sha256; ++i)
            TEST_ASSERT_EQUAL_UINT8(0xA5, query.source_sha256[i]);
    }
}
static void test_read_only_no_writes(void)
{
    text_file("target", "@block a\nx");
    text_file("source", "@block s\n[[target#a]]");
    adapter_mount();
    list_fresh("source", NULL);
    refs_fresh("source");
    backlinks_fresh("target", "a");
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_OK, read_fresh("source", "s", 0));
    TEST_ASSERT_EQUAL_UINT32(0, writes);
}
static void test_result_names(void)
{
    static const struct { YanBlockQueryResult result; const char *name; } names[] = {
        {YAN_BLOCK_QUERY_OK, "OK"},
        {YAN_BLOCK_QUERY_INVALID, "INVALID"},
        {YAN_BLOCK_QUERY_BUSY, "BUSY"},
        {YAN_BLOCK_QUERY_STOPPED, "STOPPED"},
        {YAN_BLOCK_QUERY_CANCELLED, "CANCELLED"},
        {YAN_BLOCK_QUERY_STALE, "STALE"},
        {YAN_BLOCK_QUERY_METADATA, "METADATA"},
        {YAN_BLOCK_QUERY_BLOCK_FORMAT, "BLOCK_FORMAT"},
        {YAN_BLOCK_QUERY_AMBIGUOUS, "AMBIGUOUS"},
        {YAN_BLOCK_QUERY_LIMIT, "LIMIT"},
        {YAN_BLOCK_QUERY_INVALID_TEXT, "INVALID_TEXT"},
        {YAN_BLOCK_QUERY_NOT_MOUNTED, "NOT_MOUNTED"},
        {YAN_BLOCK_QUERY_FAULTED, "FAULTED"},
        {YAN_BLOCK_QUERY_NOT_FOUND, "NOT_FOUND"},
        {YAN_BLOCK_QUERY_IO, "IO"},
        {YAN_BLOCK_QUERY_PROTOCOL, "PROTOCOL"},
        {YAN_BLOCK_QUERY_CORRUPT, "CORRUPT"},
        {YAN_BLOCK_QUERY_UNSUPPORTED, "UNSUPPORTED"}
    };
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; ++i)
        TEST_ASSERT_EQUAL_STRING(names[i].name,
                                 yan_block_query_result_name(names[i].result));
}
static void test_init_rejects_unmounted_adapter(void)
{
    YanSemanticPosix unmounted;
    memset(&unmounted, 0, sizeof unmounted);
    unmounted.image_fd = unmounted.index_fd = -1;
    YanFsBlockIo io = {NULL, capacity, fault_read, read_only_write};
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, test_fs_init(&unmounted.fs, io));
    YanBlockQuery fresh;
    memset(&fresh, 0, sizeof fresh);
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_NOT_MOUNTED,
        yan_block_query_init(&fresh, &unmounted, NULL));
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_INVALID,
        yan_block_query_init(NULL, &unmounted, NULL));
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_INVALID,
        yan_block_query_init(&fresh, NULL, NULL));

    text_file("note", "@block a\nx");
    adapter_mount();
    query.blocks.busy = true;
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_QUERY_BUSY,
        yan_block_query_init(&query, &adapter, NULL));
    query.blocks.busy = false;
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_list_ranges_header_and_empty);
    RUN_TEST(test_list_64_65_and_duplicate);
    RUN_TEST(test_read_pages_and_terminal);
    RUN_TEST(test_read_multipage_and_lines);
    RUN_TEST(test_read_utf8_boundaries);
    RUN_TEST(test_read_crlf_boundary);
    RUN_TEST(test_read_long_single_line);
    RUN_TEST(test_empty_block_and_missing);
    RUN_TEST(test_refs_and_target_states);
    RUN_TEST(test_backlinks_order_skip_and_target);
    RUN_TEST(test_backlinks_top20_and_header_positions);
    RUN_TEST(test_error_scope_and_invalid_inputs);
    RUN_TEST(test_expected_source_success_and_digest_publication);
    RUN_TEST(test_stale_expected_and_source_change);
    RUN_TEST(test_post_observation_after_service);
    RUN_TEST(test_observation_errors_and_cancel);
    RUN_TEST(test_read_count_and_no_writes);
    RUN_TEST(test_read_late_show_io_preserves_output);
    RUN_TEST(test_cancel_interrupts_service_preflight);
    RUN_TEST(test_read_only_no_writes);
    RUN_TEST(test_result_names);
    RUN_TEST(test_init_rejects_unmounted_adapter);
    return UNITY_END();
}
