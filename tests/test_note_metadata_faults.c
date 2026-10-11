#include "note_metadata.h"
#include "unity.h"
#include <string.h>

/* Real FS failures at prefix, body, data and directory phases. The device can
 * damage bytes before reporting a write error: metadata adds no rollback. */
static uint8_t disk[8][4096];
static uint8_t before[sizeof disk];
static YanFs fs;
static YanNoteContext note;
static uint32_t reads, writes, directory_writes, fail_read, fail_write;
static YanFsIoResult fault;
static bool damage;
static YanNoteInfo info;
static uint8_t holder_before[sizeof info];
static uint8_t cache_before[sizeof info];
static bool watch_cache, cache_early;
static uint8_t filler[6 * 4096];

static YanFsIoResult capacity(void *opaque, uint64_t *out)
{ (void)opaque; *out = 8u; return YAN_FS_IO_OK; }
static YanFsIoResult read_block(void *opaque, uint32_t lba, uint8_t out[4096])
{
    (void)opaque;
    ++reads;
    if (reads == fail_read) return fault;
    if (lba >= 8u) return YAN_FS_IO_ERROR;
    memcpy(out, disk[lba], 4096u);
    return YAN_FS_IO_OK;
}
static YanFsIoResult write_block(void *opaque, uint32_t lba, const uint8_t in[4096])
{
    (void)opaque;
    ++writes;
    if (watch_cache && memcmp(cache_before, &note.current, sizeof note.current) != 0)
        cache_early = true;
    if (lba == 0u) ++directory_writes;
    if (lba >= 8u) return YAN_FS_IO_ERROR;
    if (writes == fail_write) {
        if (damage) disk[lba][0] ^= 0xffu;
        return fault;
    }
    memcpy(disk[lba], in, 4096u);
    return YAN_FS_IO_OK;
}
void setUp(void)
{
    memset(disk, 0, sizeof disk);
    memset(&fs, 0, sizeof fs);
    memset(&note, 0, sizeof note);
    reads = writes = directory_writes = fail_read = fail_write = 0u;
    fault = YAN_FS_IO_ERROR; damage = false;
    watch_cache = cache_early = false;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_format_metadata(disk[0], 8u));
    YanFsBlockIo io = {NULL, capacity, read_block, write_block};
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_init(&fs, io));
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_mount(&fs));
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK,
        yan_fs_create(&fs, "n", (const uint8_t *)"body\n", 5u));
    TEST_ASSERT_EQUAL_INT(YAN_NOTE_OK, yan_note_init(&note, &fs));
    reads = writes = directory_writes = 0u;
    memcpy(before, disk, sizeof disk);
    memset(&info, 0x5a, sizeof info);
    memcpy(holder_before, &info, sizeof info);
    memcpy(cache_before, &note.current, sizeof note.current);
    watch_cache = true;
}
void tearDown(void)
{
    TEST_ASSERT_FALSE_MESSAGE(cache_early, "cache_not_published_during_any_device_write");
}
static YanNoteResult change(void)
{ return yan_note_tag(&note, "n", YAN_NOTE_MANAGE_ADD, (const uint8_t *)"os", 2u); }
static void prefix_protocol_fault_preserves_holder_and_disables_fs(void)
{
    fail_read = 1u; fault = YAN_FS_IO_PROTOCOL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_PROTOCOL,
        yan_note_show(&note, "n", &info), "prefix_protocol_original_result");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(holder_before, &info, sizeof info,
                                      "prefix_protocol_holder_unchanged");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_FAULTED, change(), "prefix_protocol_sticky_fault");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, reads, "prefix_protocol_no_retry");
}
static void body_read_fault_stops_before_writes(void)
{
    fail_read = 2u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_IO, change(), "body_read_io_result");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, writes, "body_read_zero_writes");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(before, disk, sizeof disk, "body_read_medium_unchanged");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_FAULTED, change(), "body_read_sticky_fault");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2u, reads, "body_read_no_retry");
}
static void data_write_fault_never_publishes_directory(void)
{
    fail_write = 1u; damage = true;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_IO, change(), "data_write_io_result");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, directory_writes, "data_write_no_directory_publish");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(before, disk[0], 4096u, "data_write_original_directory");
    TEST_ASSERT_TRUE_MESSAGE(memcmp(before, disk, sizeof disk) != 0,
                             "data_write_does_not_promise_rollback");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_FAULTED, change(), "data_write_sticky_fault");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, writes, "data_write_no_retry");
}
static void directory_write_fault_is_protocol_and_not_rolled_back(void)
{
    fail_write = 2u; fault = YAN_FS_IO_PROTOCOL; damage = true;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_PROTOCOL, change(), "directory_write_protocol_result");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, directory_writes, "directory_write_one_attempt");
    TEST_ASSERT_TRUE_MESSAGE(memcmp(before, disk[0], 4096u) != 0,
                             "directory_write_no_rollback_guarantee");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_FAULTED, change(), "directory_write_sticky_fault");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2u, writes, "directory_write_no_retry");
}
static void no_space_is_ordinary_and_preserves_source(void)
{
    memset(filler, 'x', sizeof filler);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_create(&fs, "full", filler, sizeof filler));
    YanFsSource a, b;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_source(&fs, &a));
    memcpy(before, disk, sizeof disk);
    writes = 0u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_NOSPACE, change(), "nospace_note_result");
    TEST_ASSERT_FALSE_MESSAGE(note.current.has_header, "nospace_no_early_cached_candidate");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, writes, "nospace_no_write");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(before, disk, sizeof disk, "nospace_original_medium");
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_source(&fs, &b));
    TEST_ASSERT_TRUE_MESSAGE(a.token == b.token, "nospace_source_identity_unchanged");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_OK, yan_note_show(&note, "n", &info),
                                  "nospace_service_still_usable");
    TEST_ASSERT_FALSE_MESSAGE(info.has_header, "nospace_candidate_not_published");
}
static void success_publishes_cache_after_directory_commit(void)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_OK, change(), "success_cache_change_ok");
    TEST_ASSERT_TRUE_MESSAGE(note.current.has_header, "success_cache_header_present");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(25u, note.current.header_bytes, "success_cache_body_offset_25");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(1u, note.current.tag_count, "success_cache_tag_count");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("os", (const char *)note.current.tag[0], "success_cache_tag_value");
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(prefix_protocol_fault_preserves_holder_and_disables_fs);
    RUN_TEST(body_read_fault_stops_before_writes);
    RUN_TEST(data_write_fault_never_publishes_directory);
    RUN_TEST(directory_write_fault_is_protocol_and_not_rolled_back);
    RUN_TEST(no_space_is_ordinary_and_preserves_source);
    RUN_TEST(success_publishes_cache_after_directory_commit);
    return UNITY_END();
}
