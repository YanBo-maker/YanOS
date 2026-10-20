#include "note_blocks.h"
#include "fs_test_support.h"
#include "unity.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BLOCKS 256u
static uint8_t medium[BLOCKS][4096], before[BLOCKS][4096];
static YanFs fs;
static YanBlockContext context;
static YanBlockSummary summary, sentinel;
static YanBlockRow rows[64];
static uint32_t reads, writes, fail_read, seen, raw_length;
static YanFsIoResult fail_code;
static uint8_t raw[120000], large[110000];
static bool stop_row, stop_chunk, reenter;
static uint32_t show_fault_after;
static YanBlockResult nested;

static YanFsIoResult capacity(void *p, uint64_t *n)
{ (void)p; *n = BLOCKS; return YAN_FS_IO_OK; }
static YanFsIoResult read_block(void *p, uint32_t lba, uint8_t *out)
{
    (void)p; ++reads;
    if (fail_read != 0u && reads == fail_read) return fail_code;
    if (lba >= BLOCKS) return YAN_FS_IO_ERROR;
    memcpy(out, medium[lba], 4096); return YAN_FS_IO_OK;
}
static YanFsIoResult write_block(void *p, uint32_t lba, const uint8_t *in)
{
    (void)p; ++writes;
    if (lba >= BLOCKS) return YAN_FS_IO_ERROR;
    memcpy(medium[lba], in, 4096); return YAN_FS_IO_OK;
}
static void make(const char *name, const void *bytes, uint32_t n)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK,
        yan_fs_create(&fs, name, bytes, n), "blocks_fixture_create");
}
static void text_file(const char *name, const char *bytes)
{ make(name, bytes, (uint32_t)strlen(bytes)); }
static YanBlockAddress address(const char *name, const char *id)
{
    YanBlockAddress a = {{0}, {0}};
    memcpy(a.name, name, strlen(name)); memcpy(a.id, id, strlen(id)); return a;
}
static bool row_sink(void *p, const YanBlockRow *row)
{
    (void)p;
    TEST_ASSERT_LESS_THAN_UINT32_MESSAGE(64u, seen, "blocks_row_bound");
    rows[seen++] = *row;
    if (show_fault_after != 0u) fail_read = reads + show_fault_after;
    if (reenter) {
        uint32_t old = reads;
        nested = test_blocks_init(&context, &fs);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(old, reads, "blocks_reentry_no_io");
    }
    return !stop_row;
}
static bool chunk_sink(void *p, uint32_t offset, const uint8_t *bytes, uint32_t n)
{
    (void)p;
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(raw_length, offset, "blocks_chunk_offset");
    TEST_ASSERT_TRUE_MESSAGE(n <= sizeof raw - raw_length, "blocks_chunk_capacity");
    memcpy(raw + raw_length, bytes, n); raw_length += n; return !stop_chunk;
}
void setUp(void)
{
    memset(medium, 0, sizeof medium); memset(&fs, 0, sizeof fs);
    memset(&context, 0, sizeof context); memset(rows, 0, sizeof rows);
    memset(&summary, 0xA5, sizeof summary); sentinel = summary;
    reads = writes = fail_read = seen = raw_length = 0;
    stop_row = stop_chunk = reenter = false; fail_code = YAN_FS_IO_ERROR;
    show_fault_after = 0;
    YanFsBlockIo io = {NULL, capacity, read_block, write_block};
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_format_metadata(medium[0], BLOCKS));
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, test_fs_init(&fs, io));
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_mount(&fs));
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_OK, test_blocks_init(&context, &fs));
}
void tearDown(void) { }

static void blocks_list_ranges_and_empty(void)
{
    const char *data = "preamble\r\n@block first\r\nA\r\nB\n@block empty\n@block end";
    text_file("note", data);
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_OK,
        yan_blocks_list(&context, "note", row_sink, NULL, &summary));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(3, seen, "blocks_list_three_rows");
    TEST_ASSERT_EQUAL_STRING("first", rows[0].block.id);
    TEST_ASSERT_EQUAL_UINT32(3, rows[0].block.first_line);
    TEST_ASSERT_EQUAL_UINT32(4, rows[0].block.last_line);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(strstr(data, "A\r\n") - data), rows[0].block.start);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(strstr(data, "@block empty") - data), rows[0].block.end);
    TEST_ASSERT_EQUAL_UINT32(0, rows[1].block.first_line);
    TEST_ASSERT_EQUAL_UINT32(0, rows[2].block.last_line);
    TEST_ASSERT_EQUAL_UINT64(3, summary.total);
}
static void blocks_show_original_bytes_and_missing(void)
{
    text_file("note", "@yan-note 1\n@end\n@block a\r\nX\tY\r\nZ");
    YanBlockAddress a = address("note", "a");
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_OK,
        yan_blocks_show(&context, &a, row_sink, chunk_sink, NULL, &summary));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(6, raw_length, "blocks_show_raw_length");
    TEST_ASSERT_EQUAL_MEMORY("X\tY\r\nZ", raw, 6);
    TEST_ASSERT_EQUAL_UINT32(4, rows[0].block.first_line);
    TEST_ASSERT_EQUAL_UINT64(2, summary.total);
    a = address("note", "missing"); seen = 0;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_BLOCK_NOT_FOUND,
        yan_blocks_show(&context, &a, row_sink, chunk_sink, NULL, NULL),
        "blocks_show_missing_id");
    TEST_ASSERT_EQUAL_UINT32(0, seen);
}
static void blocks_large_text_and_long_line(void)
{
    memcpy(large, "@block a\n", 9); memset(large + 9, 'x', 100000);
    make("large", large, 100009);
    YanBlockAddress a = address("large", "a");
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_OK,
        yan_blocks_show(&context, &a, row_sink, chunk_sink, NULL, &summary));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(100000, raw_length, "blocks_large_complete_bytes");
    TEST_ASSERT_EQUAL_UINT64(1, summary.total);
    TEST_ASSERT_EQUAL_MEMORY(large + 9, raw, raw_length);
}
static void blocks_refs_no_marker_and_unlimited_count(void)
{
    text_file("target", "@block a\nA");
    uint32_t n = 0;
    for (uint32_t i = 0; i < 300; ++i) {
        memcpy(large + n, "[[target#a]] ", 13); n += 13;
    }
    text_file("unused", "@block a\nA"); make("source", large, n);
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_OK,
        yan_blocks_refs(&context, "source", row_sink, NULL, &summary));
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(300, summary.total, "blocks_refs_full_total");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(20, seen, "blocks_refs_top20");
    TEST_ASSERT_FALSE(rows[0].reference.has_source_id);
    TEST_ASSERT_EQUAL_UINT32(0, rows[0].reference.start);
    TEST_ASSERT_EQUAL_UINT32(12, rows[0].reference.end);
}
static void blocks_refs_unshown_target_not_read(void)
{
    text_file("target", "@block a\nx"); text_file("bad", "@block a\n@block a\n");
    uint32_t n = 0;
    for (uint32_t i = 0; i < 20; ++i) { memcpy(large + n, "[[target#a]]", 12); n += 12; }
    memcpy(large + n, "[[bad#a]]", 9); n += 9; make("source", large, n);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_BLOCK_OK,
        yan_blocks_refs(&context, "source", row_sink, NULL, &summary),
        "blocks_refs_unshown_bad_target_ok");
    TEST_ASSERT_EQUAL_UINT64(21, summary.total);
}
static void blocks_refs_bad_shown_target_no_rows(void)
{
    text_file("target", "@block a\nx"); text_file("bad", "@block a\n@block a\n");
    text_file("source", "[[target#a]][[bad#a]]");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_BLOCK_AMBIGUOUS,
        yan_blocks_refs(&context, "source", row_sink, NULL, &summary),
        "blocks_refs_shown_bad_error");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, seen, "blocks_refs_no_partial_rows");
    TEST_ASSERT_EQUAL_MEMORY(&sentinel, &summary, sizeof summary);
}
static void blocks_backlinks_sort_occurrences_and_skips(void)
{
    text_file("z", "[[gone#a]]"); text_file("A", "@block source\n[[gone#a]][[gone#a]]");
    const uint8_t binary[] = "[[gone#a]]\0"; make("binary", binary, sizeof binary - 1);
    YanBlockAddress a = address("gone", "a");
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_OK,
        yan_blocks_backlinks(&context, &a, row_sink, NULL, &summary));
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(3, summary.total, "blocks_backlinks_occurrence_total");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("A", rows[0].source, "blocks_backlinks_ascii_first");
    TEST_ASSERT_EQUAL_STRING("z", rows[2].source);
    TEST_ASSERT_TRUE(rows[0].reference.has_source_id);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, summary.skipped, "blocks_backlinks_binary_skip");
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_DANGLING_FILE, summary.target_state);
}
static void blocks_backlinks_late_bad_structure_no_rows(void)
{
    text_file("a", "[[gone#a]]"); text_file("z", "@block\n");
    YanBlockAddress a = address("gone", "a");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_BLOCK_FORMAT,
        yan_blocks_backlinks(&context, &a, row_sink, NULL, &summary),
        "blocks_backlinks_late_format_error");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, seen, "blocks_backlinks_no_partial_rows");
    TEST_ASSERT_EQUAL_MEMORY(&sentinel, &summary, sizeof summary);
}
static void blocks_text_precedes_head_and_structure(void)
{
    const uint8_t bad[] = "@yan-note 9\n@block\n\0"; make("bad", bad, sizeof bad - 1);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_BLOCK_INVALID_TEXT,
        yan_blocks_list(&context, "bad", row_sink, NULL, &summary),
        "blocks_source_text_priority");
    YanBlockAddress a = address("gone", "a");
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_OK,
        yan_blocks_backlinks(&context, &a, row_sink, NULL, &summary));
    TEST_ASSERT_EQUAL_UINT32(1, summary.skipped);
    a = address("bad", "a");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_BLOCK_INVALID_TEXT,
        yan_blocks_backlinks(&context, &a, row_sink, NULL, NULL),
        "blocks_binary_target_not_skipped");
}
static void blocks_missing_source_and_bad_header(void)
{
    text_file("bad", "@yan-note 9\n@end\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_BLOCK_NOT_FOUND,
        yan_blocks_list(&context, "missing", row_sink, NULL, &summary),
        "blocks_missing_source_priority");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_BLOCK_METADATA,
        yan_blocks_list(&context, "bad", row_sink, NULL, &summary),
        "blocks_bad_header_error");
    TEST_ASSERT_EQUAL_MEMORY(&sentinel, &summary, sizeof summary);
}
static void blocks_stop_and_reentry_preserve_summary(void)
{
    text_file("n", "@block a\nx"); stop_row = reenter = true;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_BLOCK_STOPPED,
        yan_blocks_list(&context, "n", row_sink, NULL, &summary), "blocks_stop_result");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_BLOCK_BUSY, nested, "blocks_reentry_busy");
    TEST_ASSERT_EQUAL_MEMORY(&sentinel, &summary, sizeof summary);
    TEST_ASSERT_FALSE(context.busy);
}
static void blocks_guard_wrap_alias_and_busy(void)
{
    context.busy = true;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_BLOCK_BUSY,
        yan_blocks_list(&context, NULL, NULL, NULL, NULL), "blocks_busy_before_parameters");
    context.busy = false;
    uint32_t old = reads;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_BLOCK_INVALID,
        yan_blocks_list((YanBlockContext *)(UINTPTR_MAX - 2u), "n", row_sink, NULL, NULL),
        "blocks_wrapping_context");
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_INVALID,
        yan_blocks_list(&context, "n", row_sink, NULL,
                        (YanBlockSummary *)((uint8_t *)&context + sizeof context - 1u)));
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_INVALID,
        yan_blocks_list(&context, (const char *)&context, row_sink, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(old, reads, "blocks_invalid_zero_io");
}
static void blocks_preflight_io_propagates(void)
{
    text_file("n", "@block a\nx"); fail_read = reads + 1;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_BLOCK_IO,
        yan_blocks_list(&context, "n", row_sink, NULL, &summary), "blocks_io_error");
    TEST_ASSERT_EQUAL_UINT32(0, seen);
    TEST_ASSERT_EQUAL_MEMORY(&sentinel, &summary, sizeof summary);
    TEST_ASSERT_FALSE(context.busy);
}
static void blocks_queries_never_write_media(void)
{
    text_file("n", "@block a\n[[n#a]]"); memcpy(before, medium, sizeof before);
    uint32_t old = writes;
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_OK,
        yan_blocks_refs(&context, "n", row_sink, NULL, &summary));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(old, writes, "blocks_query_zero_writes");
    TEST_ASSERT_EQUAL_MEMORY(before, medium, sizeof medium);
}
static void blocks_show_late_io_preserves_prefix_without_summary(void)
{
    memcpy(large, "@block a\n", 9); memset(large + 9, 'x', 10000);
    make("n", large, 10009);
    /* The range begins at byte 9, so one 4096-byte logical chunk reads two
     * physical blocks. Fail the next chunk, after the first was delivered. */
    YanBlockAddress a = address("n", "a"); show_fault_after = 3;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_BLOCK_IO,
        yan_blocks_show(&context, &a, row_sink, chunk_sink, NULL, &summary),
        "blocks_show_late_io_error");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(4096, raw_length, "blocks_show_late_io_prefix");
    TEST_ASSERT_EQUAL_MEMORY(&sentinel, &summary, sizeof summary);
    TEST_ASSERT_FALSE(context.busy);
}
static void blocks_show_chunk_stop_prevents_more_io(void)
{
    memcpy(large, "@block a\n", 9); memset(large + 9, 'x', 10000);
    make("n", large, 10009);
    YanBlockAddress a = address("n", "a"); stop_chunk = true; show_fault_after = 3;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_BLOCK_STOPPED,
        yan_blocks_show(&context, &a, row_sink, chunk_sink, NULL, &summary),
        "blocks_show_chunk_stop");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(fail_read - 1u, reads, "blocks_show_no_read_after_stop");
    TEST_ASSERT_EQUAL_MEMORY(&sentinel, &summary, sizeof summary);
}
static void blocks_protocol_error_remains_fatal_class(void)
{
    text_file("n", "@block a\nx"); fail_read = reads + 1; fail_code = YAN_FS_IO_PROTOCOL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_BLOCK_PROTOCOL,
        yan_blocks_list(&context, "n", row_sink, NULL, &summary), "blocks_protocol_error");
    TEST_ASSERT_EQUAL_UINT32(0, seen);
    TEST_ASSERT_EQUAL_MEMORY(&sentinel, &summary, sizeof summary);
}
static void blocks_backlinks_top_twenty_reorders_after_slot_reuse(void)
{
    for (uint32_t i = 0; i < 26u; ++i) {
        char name[32];
        (void)snprintf(name, sizeof name, "s%02u", 25u - i);
        text_file(name, "[[gone#a]]");
    }
    YanBlockAddress a = address("gone", "a");
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_OK,
        yan_blocks_backlinks(&context, &a, row_sink, NULL, &summary));
    TEST_ASSERT_EQUAL_UINT64(26u, summary.total);
    TEST_ASSERT_EQUAL_UINT32(20u, seen);
    for (uint32_t i = 0; i < seen; ++i) {
        char name[32];
        (void)snprintf(name, sizeof name, "s%02u", i);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(name, rows[i].source,
            "blocks_r8_top_twenty_ascii");
    }

    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_remove(&fs, "s00"));
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_remove(&fs, "s01"));
    text_file("A", "[[gone#a]]");
    text_file("B", "[[gone#a]]");
    seen = 0;
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_OK,
        yan_blocks_backlinks(&context, &a, row_sink, NULL, &summary));
    TEST_ASSERT_EQUAL_UINT64(26u, summary.total);
    TEST_ASSERT_EQUAL_UINT32(20u, seen);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("A", rows[0].source,
        "blocks_r8_reused_slot_first");
    TEST_ASSERT_EQUAL_STRING("B", rows[1].source);
    TEST_ASSERT_EQUAL_STRING("s02", rows[2].source);
    TEST_ASSERT_EQUAL_STRING("s19", rows[19].source);

    for (uint32_t i = 2; i < 26u; ++i) {
        char name[32];
        (void)snprintf(name, sizeof name, "s%02u", i);
        TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_remove(&fs, name));
    }
    seen = 0;
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_OK,
        yan_blocks_backlinks(&context, &a, row_sink, NULL, &summary));
    TEST_ASSERT_EQUAL_UINT64(2u, summary.total);
    TEST_ASSERT_EQUAL_UINT32(2u, seen);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("A", rows[0].source,
        "blocks_r8_shrunk_candidate_first");
    TEST_ASSERT_EQUAL_STRING("B", rows[1].source);
}

static void blocks_small_file_single_read_preserves_original_ranges(void)
{
    const char data[] = "@yan-note 1\n@end\n@block a\r\n\xE4\xB8\xAD\tX\r\nZ";
    text_file("note", data);
    uint32_t start = reads;
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_OK,
        yan_blocks_list(&context, "note", row_sink, NULL, &summary));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, reads - start,
        "blocks_small_file_one_read");
    TEST_ASSERT_EQUAL_UINT32(4u, rows[0].block.first_line);
    TEST_ASSERT_EQUAL_UINT32(5u, rows[0].block.last_line);
    TEST_ASSERT_EQUAL_UINT32(sizeof data - 1u, rows[0].block.end);
    seen = 0;
    YanBlockAddress a = address("note", "a");
    start = reads;
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_OK,
        yan_blocks_show(&context, &a, row_sink, chunk_sink, NULL, &summary));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2u, reads - start,
        "blocks_small_show_preflight_and_raw_read");
    TEST_ASSERT_EQUAL_MEMORY("\xE4\xB8\xAD\tX\r\nZ", raw, 8u);
}

static void blocks_buffer_boundary_and_late_binary_remain_atomic(void)
{
    memset(large, 'x', 4096u);
    memcpy(large, "@block a\n", 9u);
    make("exact", large, 4096u);
    uint32_t start = reads;
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_OK,
        yan_blocks_list(&context, "exact", row_sink, NULL, &summary));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, reads - start,
        "blocks_exact_buffer_single_read");
    TEST_ASSERT_EQUAL_UINT32(4096u, rows[0].block.end);
    large[4096] = 0;
    make("late", large, 4097u);
    seen = 0;
    summary = sentinel;
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_INVALID_TEXT,
        yan_blocks_list(&context, "late", row_sink, NULL, &summary));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, seen, "blocks_late_binary_no_rows");
    TEST_ASSERT_EQUAL_MEMORY(&sentinel, &summary, sizeof summary);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(blocks_list_ranges_and_empty);
    RUN_TEST(blocks_show_original_bytes_and_missing);
    RUN_TEST(blocks_large_text_and_long_line);
    RUN_TEST(blocks_refs_no_marker_and_unlimited_count);
    RUN_TEST(blocks_refs_unshown_target_not_read);
    RUN_TEST(blocks_refs_bad_shown_target_no_rows);
    RUN_TEST(blocks_backlinks_sort_occurrences_and_skips);
    RUN_TEST(blocks_backlinks_late_bad_structure_no_rows);
    RUN_TEST(blocks_text_precedes_head_and_structure);
    RUN_TEST(blocks_missing_source_and_bad_header);
    RUN_TEST(blocks_stop_and_reentry_preserve_summary);
    RUN_TEST(blocks_guard_wrap_alias_and_busy);
    RUN_TEST(blocks_preflight_io_propagates);
    RUN_TEST(blocks_queries_never_write_media);
    RUN_TEST(blocks_show_late_io_preserves_prefix_without_summary);
    RUN_TEST(blocks_show_chunk_stop_prevents_more_io);
    RUN_TEST(blocks_protocol_error_remains_fatal_class);
    RUN_TEST(blocks_backlinks_top_twenty_reorders_after_slot_reuse);
    RUN_TEST(blocks_small_file_single_read_preserves_original_ranges);
    RUN_TEST(blocks_buffer_boundary_and_late_binary_remain_atomic);
    return UNITY_END();
}
