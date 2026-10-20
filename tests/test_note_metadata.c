/* 0027 note-metadata service: native tests, written before the implementation.
 *
 * The contract is os/note_metadata.h, os/note_metadata_format.h and
 * docs/specs/0027-note-metadata.md ("命令及可观察输出", "观察与更新的读取范围").
 * The service is implemented. These assertions pin its observed public
 * contract over real filesystem callbacks; historical RED logs are separate.
 *
 * The device is a caller-owned static 8 MiB byte array behind the real
 * synchronous YanFsBlockIo callbacks, so every fault injection happens at a
 * boundary the filesystem really uses. All fixtures are static: the service
 * context, the 16 KiB candidate and the 20 KiB payload never touch the stack.
 *
 * What this file pins, clause by clause:
 *
 *   - init records a borrowed filesystem, reads no block, and rejects a
 *     context/fs overlap and an unusable filesystem;
 *   - show is a bounded prefix observation: a header-less binary file is a
 *     valid old note with empty metadata, a legal header over a binary body is
 *     reported, a missing link is a normal DANGLING observation, a large body
 *     stays inside a small block-read budget, and a broken header is METADATA
 *     with the caller's info byte-identical;
 *   - list reserves the whole directory first: one broken header in the last
 *     slot yields METADATA with zero callbacks and an unchanged count, a clean
 *     directory is delivered in ASCII order with precise AND filtering, and a
 *     callback stop is STOPPED;
 *   - an actual change rebuilds the canonical header, preserves the body bytes
 *     including CRLF and a missing final LF, publishes with exactly one
 *     directory write and takes a new source token;
 *   - a no-change request reads no body and writes nothing; a value that fits
 *     exactly and one that overflows the 16 KiB candidate are told apart; a
 *     real change on an over-16 KiB or invalid-text source is refused;
 *   - a fault on the header read propagates as the filesystem's own error and
 *     keeps the caller's holder;
 *   - context/FS/service argument rules are checked before any data read, so an
 *     aliased or overflowing holder is rejected with pure address arithmetic.
 *
 * Every assertion carries a stable, unique message tag for the later
 * owner-specific mutation gate. The exact canonical order of a rewritten list
 * (for example where a newly added tag lands) is authored by the service and is
 * not pinned here beyond "the stored tags are exactly the expected set". */

#include "note_metadata.h"
#include "fs_test_support.h"
#include "yanfs.h"
#include "unity.h"

#include <string.h>

/* ------------------------------------------------------------- fixtures */

#define NOTE_BLOCKS 2048u /* 8 MiB */
#define READ_BUDGET 1u    /* a header observation must not stream the body */

static uint8_t medium[NOTE_BLOCKS][YAN_FS_BLOCK_SIZE];
static uint32_t device_reads;
static uint32_t device_writes;
static uint32_t directory_writes;
static uint32_t device_read_fail_at;
static YanFsIoResult device_read_fail_code;
static bool device_armed;
static YanFs fs;
static YanNoteContext context;

/* Reentry probe run from inside a device callback while the service is busy. */
static bool reenter_armed;
static YanNoteContext *reenter_context;
static YanNoteResult reenter_result;

static uint8_t file_content[24096];
static uint8_t binary_payload[8192];
static uint8_t large_payload[20000];
static uint8_t readback[20000];
static uint8_t medium_before[NOTE_BLOCKS][YAN_FS_BLOCK_SIZE];
_Alignas(YanNoteInfo) static uint8_t info_snapshot[sizeof(YanNoteInfo)];
_Alignas(YanNoteInfo) static uint8_t holder_storage[sizeof(YanNoteInfo)];
static const uint8_t text_tag[] = "os";
static const uint8_t text_tag_extra[] = "riscv";
static YanNoteTagFilter filter;
static char list_names[YAN_FS_MAX_FILES][32];
static uint32_t list_count;
static uint32_t list_shown;
static bool list_stop_after_one;
static YanNoteInfo holder;

/* ------------------------------------------------------------ byte device */

static YanFsIoResult device_capacity(void *context_pointer, uint64_t *blocks)
{
    (void)context_pointer;
    *blocks = NOTE_BLOCKS;
    return YAN_FS_IO_OK;
}

static YanFsIoResult device_read(void *context_pointer, uint32_t lba,
                                 uint8_t out[4096])
{
    (void)context_pointer;
    ++device_reads;
    if (reenter_armed) {
        reenter_armed = false;
        reenter_result = yan_note_show(reenter_context, "busy.txt", NULL);
    }
    if (device_armed && device_read_fail_at != 0u &&
        device_reads == device_read_fail_at) {
        return device_read_fail_code;
    }
    if (lba >= NOTE_BLOCKS) {
        return YAN_FS_IO_ERROR;
    }
    memcpy(out, medium[lba], YAN_FS_BLOCK_SIZE);
    return YAN_FS_IO_OK;
}

static YanFsIoResult device_write(void *context_pointer, uint32_t lba,
                                  const uint8_t data[4096])
{
    (void)context_pointer;
    ++device_writes;
    if (lba == 0u) ++directory_writes;
    if (lba >= NOTE_BLOCKS) {
        return YAN_FS_IO_ERROR;
    }
    memcpy(medium[lba], data, YAN_FS_BLOCK_SIZE);
    return YAN_FS_IO_OK;
}

static YanFsBlockIo fs_backend(void)
{
    YanFsBlockIo io;
    io.context = NULL;
    io.capacity = device_capacity;
    io.read_block = device_read;
    io.write_block = device_write;
    return io;
}

/* --------------------------------------------------------------- helpers */

static void device_quiet(void)
{
    device_armed = false;
    device_read_fail_at = 0u;
}

static void medium_snapshot(void)
{
    memcpy(medium_before, medium, sizeof medium_before);
}

static bool medium_unchanged(void)
{
    return memcmp(medium_before, medium, sizeof medium_before) == 0;
}

static void fs_mount_fresh(void)
{
    memset(&fs, 0, sizeof fs);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK,
                                  yan_fs_format_metadata(medium[0], NOTE_BLOCKS),
                                  "fs_fixture_format");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK, test_fs_init(&fs, fs_backend()),
                                  "fs_fixture_init");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK, yan_fs_mount(&fs),
                                  "fs_fixture_mount");
    memset(&context, 0, sizeof context);
}

static void with_service(void)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_OK, test_note_init(&context, &fs),
                                  "service_init");
}

static void fs_make_file(const char *name, const uint8_t *bytes, uint32_t length)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK,
                                  yan_fs_create(&fs, name, bytes, length),
                                  "fs_fixture_create");
}

static uint32_t read_file(const char *name, uint8_t *out, uint32_t capacity)
{
    uint32_t got = 0u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK,
                                  yan_fs_read(&fs, name, 0u, out, capacity, &got),
                                  "fs_fixture_read");
    return got;
}

/* The header length of a file whose bytes are known: everything up to and
 * including the closing `@end\n`. */
static uint32_t header_bytes_of(const uint8_t *bytes)
{
    const uint8_t *end = (const uint8_t *)strstr((const char *)bytes, "@end\n");
    TEST_ASSERT_NOT_NULL_MESSAGE(end, "fixture_has_close");
    return (uint32_t)(end - bytes) + 5u;
}

/* Marker + the caller's field lines + close + the caller's body. */
static uint32_t build_note(const char *header_lines, const uint8_t *body,
                           uint32_t body_length)
{
    const char *marker = "@yan-note 1\n";
    uint32_t length = (uint32_t)strlen(marker);
    memcpy(file_content, marker, length);
    if (header_lines != NULL) {
        uint32_t lines = (uint32_t)strlen(header_lines);
        memcpy(file_content + length, header_lines, lines);
        length += lines;
    }
    memcpy(file_content + length, "@end\n", 5u);
    length += 5u;
    if (body != NULL && body_length != 0u) {
        memcpy(file_content + length, body, body_length);
        length += body_length;
    }
    return length;
}

/* Opaque bytes that are never valid editor text, used as a binary body. */
static uint32_t fill_binary(uint32_t length)
{
    for (uint32_t i = 0u; i < length; ++i) {
        binary_payload[i] = (uint8_t)(0x80u + (i & 0x3Fu));
    }
    return length;
}

static bool list_collect(void *callback_context, const char *name)
{
    (void)callback_context;
    TEST_ASSERT_TRUE_MESSAGE(list_count < YAN_FS_MAX_FILES,
                             "list_callback_within_capacity");
    memcpy(list_names[list_count], name, strlen(name) + 1u);
    ++list_count;

    if (list_stop_after_one && list_count == 1u) {
        return false;
    }
    return true;
}

static void list_reset(void)
{
    memset(list_names, 0, sizeof list_names);
    list_count = 0u;
    list_shown = 0u;
    list_stop_after_one = false;
}

static bool filename_is_present(const char *name)
{
    uint32_t cursor = 0u;
    YanFsInfo info;
    while (yan_fs_list(&fs, &cursor, &info) == YAN_FS_OK) {
        if (strcmp(info.name, name) == 0) {
            return true;
        }
    }
    return false;
}

void setUp(void)
{
    memset(medium, 0, sizeof medium);
    device_reads = 0u;
    device_writes = 0u;
    directory_writes = 0u;
    device_read_fail_at = 0u;
    device_read_fail_code = YAN_FS_IO_ERROR;
    device_armed = false;
    memset(&fs, 0, sizeof fs);
    memset(&context, 0, sizeof context);
    memset(file_content, 0, sizeof file_content);
    memset(binary_payload, 0, sizeof binary_payload);
    memset(large_payload, 0, sizeof large_payload);
    memset(readback, 0, sizeof readback);
    memset(medium_before, 0, sizeof medium_before);
    memset(info_snapshot, 0, sizeof info_snapshot);
    memset(holder_storage, 0, sizeof holder_storage);
    memset(&filter, 0, sizeof filter);
    memset(&holder, 0, sizeof holder);
    list_reset();
    reenter_armed = false;
    reenter_context = NULL;
    reenter_result = YAN_NOTE_OK;
}

void tearDown(void)
{
}

/* ------------------------------------------------------ context lifetime */

static void note_init_records_a_borrowed_fs_without_io(void)
{
    fs_mount_fresh();
    uint32_t reads = device_reads;
    uint32_t writes = device_writes;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_OK, test_note_init(&context, &fs),
                                  "init_ok");
    TEST_ASSERT_TRUE_MESSAGE(context.initialized, "init_sets_initialized");
    TEST_ASSERT_FALSE_MESSAGE(context.busy, "init_clears_busy");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(reads, device_reads,
                                     "init_reads_no_block");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(writes, device_writes,
                                     "init_writes_no_block");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_OK, test_note_init(&context, &fs),
                                  "init_idle_reuse_ok");
}

static void note_init_rejects_bad_context_and_fs(void)
{
    fs_mount_fresh();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_INVALID, test_note_init(NULL, &fs),
                                  "init_null_context_invalid");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_INVALID, test_note_init(&context, NULL),
                                  "init_null_fs_invalid");
    memset(&fs, 0, sizeof fs);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_INVALID, test_note_init(&context, &fs),
                                  "init_uninitialized_fs_invalid");
}

static void note_service_rejects_context_fs_overlap(void)
{
    fs_mount_fresh();
    YanFs *overlap = (YanFs *)(void *)&context;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_INVALID,
                                  test_note_init(&context, overlap),
                                  "init_context_fs_overlap_invalid");
    YanNoteContext *fs_alias = (YanNoteContext *)(void *)&fs;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_INVALID,
                                  test_note_init(fs_alias, &fs),
                                  "init_fs_context_overlap_invalid");
}

static void note_uninitialized_context_rejects_every_call(void)
{
    fs_mount_fresh();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_INVALID,
                                  yan_note_show(&context, "a.txt", &holder),
                                  "show_uninitialized_invalid");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_INVALID,
        yan_note_set_title(&context, "a.txt", text_tag,
                           (uint32_t)(sizeof text_tag - 1u)),
        "set_title_uninitialized_invalid");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_INVALID,
                                  yan_note_list(&context, NULL, list_collect,
                                                NULL, &list_shown),
                                  "list_uninitialized_invalid");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_INVALID,
        yan_note_tag(&context, "a.txt", YAN_NOTE_MANAGE_ADD, text_tag,
                     (uint32_t)(sizeof text_tag - 1u)),
        "tag_uninitialized_invalid");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_INVALID,
        yan_note_set_date(&context, "a.txt", YAN_NOTE_DATE_CREATED,
                          (const uint8_t *)"2026-10-07", 10u),
        "set_date_uninitialized_invalid");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_INVALID,
                                  yan_note_link(&context, "a.txt",
                                                YAN_NOTE_MANAGE_DELETE,
                                                "missing.txt"),
                                  "link_uninitialized_invalid");
}

/* -------------------------------------------------- show: prefix reads */

static void note_show_no_header_binary_is_an_old_note(void)
{
    fs_mount_fresh();
    uint32_t length = fill_binary(2048u);
    fs_make_file("bin.dat", binary_payload, length);
    with_service();
    memset(&holder, 0x5a, sizeof holder);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_OK,
                                  yan_note_show(&context, "bin.dat", &holder),
                                  "show_old_note_ok");
    TEST_ASSERT_FALSE_MESSAGE(holder.has_header,
                              "show_old_note_has_no_header");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, holder.header_bytes,
                                     "show_old_note_zero_bytes");
    TEST_ASSERT_FALSE_MESSAGE(holder.has_title, "show_old_note_no_title");
    TEST_ASSERT_FALSE_MESSAGE(holder.has_created, "show_old_note_no_created");
    TEST_ASSERT_FALSE_MESSAGE(holder.has_modified, "show_old_note_no_modified");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(0u, holder.tag_count,
                                     "show_old_note_no_tags");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(0u, holder.link_count,
                                     "show_old_note_no_links");
}

static void note_show_legal_header_over_binary_body(void)
{
    fs_mount_fresh();
    uint32_t body_length = fill_binary(4096u);
    uint32_t length = build_note("title: note\ncreated: 2026-10-07\n"
                                 "tag: os\nlink: missing.txt\n",
                                 binary_payload, body_length);
    fs_make_file("note.dat", file_content, length);
    with_service();
    memset(&holder, 0, sizeof holder);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_OK,
                                  yan_note_show(&context, "note.dat", &holder),
                                  "show_legal_ok");
    TEST_ASSERT_TRUE_MESSAGE(holder.has_header, "show_legal_header_flag");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(header_bytes_of(file_content),
                                     holder.header_bytes,
                                     "show_legal_body_offset");
    TEST_ASSERT_TRUE_MESSAGE(holder.has_title, "show_legal_has_title");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("note", (const char *)holder.title,
                                     "show_legal_title");
    TEST_ASSERT_TRUE_MESSAGE(holder.has_created, "show_legal_has_created");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("2026-10-07", (const char *)holder.created,
                                     "show_legal_created");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(1u, holder.tag_count, "show_legal_tag_count");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("os", (const char *)holder.tag[0],
                                     "show_legal_tag_value");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(1u, holder.link_count,
                                     "show_legal_link_count");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("missing.txt",
                                     (const char *)holder.link[0],
                                     "show_legal_link_value");
    TEST_ASSERT_FALSE_MESSAGE(holder.link_exists[0],
                              "show_legal_missing_link_is_dangling");
}

static void note_show_reports_existing_and_dangling_links(void)
{
    fs_mount_fresh();
    fs_make_file("target.txt", large_payload, 4u);
    uint32_t length = build_note("link: target.txt\nlink: missing.txt\n", NULL,
                                 0u);
    fs_make_file("links.txt", file_content, length);
    with_service();
    memset(&holder, 0, sizeof holder);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_OK,
                                  yan_note_show(&context, "links.txt", &holder),
                                  "show_links_ok");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(2u, holder.link_count,
                                     "show_links_count");
    TEST_ASSERT_TRUE_MESSAGE(holder.link_exists[0],
                             "show_links_present_target");
    TEST_ASSERT_FALSE_MESSAGE(holder.link_exists[1],
                              "show_links_dangling_target");
}

static void note_show_large_body_stays_in_the_prefix_budget(void)
{
    fs_mount_fresh();
    memset(large_payload, 'x', sizeof large_payload);
    uint32_t length = build_note("title: big\n", large_payload, 20000u);
    fs_make_file("big.txt", file_content, length);
    with_service();
    memset(&holder, 0, sizeof holder);
    device_reads = 0u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_OK,
                                  yan_note_show(&context, "big.txt", &holder),
                                  "show_big_ok");
    TEST_ASSERT_TRUE_MESSAGE(holder.has_title, "show_big_has_title");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("big", (const char *)holder.title,
                                     "show_big_title_value");
    TEST_ASSERT_TRUE_MESSAGE(device_reads <= READ_BUDGET,
                             "show_big_stays_within_prefix_read_budget");
}

static void note_show_missing_file_is_not_created(void)
{
    fs_mount_fresh();
    with_service();
    medium_snapshot();
    memset(&holder, 0x5a, sizeof holder);
    memcpy(info_snapshot, &holder, sizeof info_snapshot);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_NOT_FOUND,
                                  yan_note_show(&context, "absent.txt", &holder),
                                  "show_missing_not_found");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(info_snapshot, &holder, sizeof info_snapshot,
                                     "show_missing_keeps_info");
    TEST_ASSERT_TRUE_MESSAGE(medium_unchanged(),
                             "show_missing_never_creates_or_writes");
    TEST_ASSERT_FALSE_MESSAGE(filename_is_present("absent.txt"),
                              "show_missing_no_directory_entry");
}

static void note_show_broken_header_is_metadata_and_keeps_info(void)
{
    fs_mount_fresh();
    uint32_t length = build_note("kind: note\n", NULL, 0u);
    fs_make_file("broken.txt", file_content, length);
    with_service();
    memset(&holder, 0x5a, sizeof holder);
    memcpy(info_snapshot, &holder, sizeof info_snapshot);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_METADATA,
                                  yan_note_show(&context, "broken.txt", &holder),
                                  "show_broken_metadata");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(info_snapshot, &holder, sizeof info_snapshot,
                                     "show_broken_keeps_info_snapshot");
}

static void note_show_rejects_alias_and_wrapping_holders(void)
{
    fs_mount_fresh();
    uint32_t length = build_note("title: x\n", NULL, 0u);
    fs_make_file("alias.txt", file_content, length);
    with_service();
    memcpy(info_snapshot, "alias.txt", 10u);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_INVALID,
        yan_note_show(&context, (const char *)info_snapshot,
                      (YanNoteInfo *)(void *)info_snapshot),
        "show_name_out_overlap_invalid");
    YanNoteInfo *wrapping = (YanNoteInfo *)(uintptr_t)UINTPTR_MAX;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_INVALID,
                                  yan_note_show(&context, "alias.txt", wrapping),
                                  "show_wrapping_out_invalid");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_INVALID,
                                  yan_note_show(&context, NULL, &holder),
                                  "show_null_name_invalid");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_INVALID,
                                  yan_note_list(NULL, NULL, list_collect, NULL,
                                                &list_shown),
                                  "list_null_context_invalid");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_INVALID,
                                  yan_note_list(&context, NULL, NULL, NULL,
                                                &list_shown),
                                  "list_null_callback_invalid");
}

/* ------------------------------------------------------------ list rules */

static void note_list_bad_header_in_last_slot_reserves_everything(void)
{
    fs_mount_fresh();
    uint32_t length = build_note("title: ok\n", NULL, 0u);
    fs_make_file("a.txt", file_content, length);
    fs_make_file("b.txt", file_content, length);
    length = build_note("kind: note\n", NULL, 0u);
    fs_make_file("z.txt", file_content, length);
    with_service();
    list_reset();
    list_shown = 0xdeadbeefu;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_METADATA,
                                  yan_note_list(&context, NULL, list_collect,
                                                NULL, &list_shown),
                                  "list_bad_header_metadata");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, list_count,
                                     "list_bad_header_zero_callbacks");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0xdeadbeefu, list_shown,
                                     "list_bad_header_keeps_shown");
}

static void note_list_orders_names_and_applies_and_filter(void)
{
    fs_mount_fresh();
    uint32_t length = build_note("title: b\ntag: os\ntag: riscv\n", NULL, 0u);
    fs_make_file("beta.txt", file_content, length);
    length = build_note("title: a\ntag: os\n", NULL, 0u);
    fs_make_file("alpha.txt", file_content, length);
    length = build_note("title: old\n", NULL, 0u);
    fs_make_file("old.bin", file_content, length);
    with_service();
    list_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_OK,
                                  yan_note_list(&context, NULL, list_collect,
                                                NULL, &list_shown),
                                  "list_order_ok");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(3u, list_count, "list_order_count");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(3u, list_shown, "list_order_shown");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("alpha.txt", list_names[0],
                                     "list_order_ascii_first");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("beta.txt", list_names[1],
                                     "list_order_ascii_second");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("old.bin", list_names[2],
                                     "list_order_ascii_third");

    memset(&filter, 0, sizeof filter);
    filter.count = 2u;
    filter.length[0] = 2u;
    memcpy(filter.tag[0], "os", 2u);
    filter.length[1] = 5u;
    memcpy(filter.tag[1], "riscv", 5u);
    list_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_OK,
                                  yan_note_list(&context, &filter, list_collect,
                                                NULL, &list_shown),
                                  "list_filter_ok");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, list_count, "list_filter_and_count");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("beta.txt", list_names[0],
                                     "list_filter_and_name");

    memset(&filter, 0, sizeof filter);
    list_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_OK,
                                  yan_note_list(&context, &filter, list_collect,
                                                NULL, &list_shown),
                                  "list_zero_filter_ok");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(3u, list_count,
                                     "list_zero_filter_is_unfiltered");
}

static void note_list_accepts_a_stop_and_reports_stopped(void)
{
    fs_mount_fresh();
    uint32_t length = build_note("title: a\n", NULL, 0u);
    fs_make_file("a.txt", file_content, length);
    fs_make_file("b.txt", file_content, length);
    with_service();
    list_reset();
    list_stop_after_one = true;
    list_shown = 0xabcdu;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_STOPPED,
                                  yan_note_list(&context, NULL, list_collect,
                                                NULL, &list_shown),
                                  "list_stop_reports_stopped");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, list_count, "list_stop_one_callback");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0xabcdu, list_shown,
                                     "list_stop_keeps_shown");
}

/* ------------------------------------------------- actual change path */

static void note_tag_real_change_publishes_and_takes_new_token(void)
{
    fs_mount_fresh();
    uint32_t length = build_note("title: keep\n", (const uint8_t *)"body",
                                 4u);
    fs_make_file("note.txt", file_content, length);
    with_service();
    YanFsSource before;
    memset(&before, 0, sizeof before);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK, yan_fs_source(&fs, &before),
                                  "tag_change_source_before");
    device_writes = 0u;
    directory_writes = 0u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_OK,
                                  yan_note_tag(&context, "note.txt",
                                               YAN_NOTE_MANAGE_ADD, text_tag,
                                               (uint32_t)(sizeof text_tag - 1u)),
                                  "tag_change_ok");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, directory_writes,
                                     "tag_change_exactly_one_write");
    YanFsSource after;
    memset(&after, 0, sizeof after);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK, yan_fs_source(&fs, &after),
                                  "tag_change_source_after");
    TEST_ASSERT_TRUE_MESSAGE(after.token != before.token,
                             "tag_change_takes_new_token");
    memset(&holder, 0, sizeof holder);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_OK,
                                  yan_note_show(&context, "note.txt", &holder),
                                  "tag_change_show_ok");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(1u, holder.tag_count,
                                     "tag_change_has_one_tag");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("os", (const char *)holder.tag[0],
                                     "tag_change_tag_value");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("keep", (const char *)holder.title,
                                     "tag_change_keeps_title");
    uint32_t got = read_file("note.txt", readback, (uint32_t)sizeof readback);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(header_bytes_of(readback) + 4u, got,
                                     "tag_change_keeps_body_length");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE("body",
                                     readback + header_bytes_of(readback), 4u,
                                     "tag_change_keeps_body_bytes");
}

static void note_set_title_rewrites_and_keeps_body_bytes(void)
{
    fs_mount_fresh();
    uint32_t length = build_note("title: old\ncreated: 2026-10-07\n",
                                 (const uint8_t *)"line1\r\nline2", 12u);
    fs_make_file("body.txt", file_content, length);
    with_service();
    device_writes = 0u;
    directory_writes = 0u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_OK,
                                  yan_note_set_title(&context, "body.txt",
                                                     (const uint8_t *)"new", 3u),
                                  "title_change_ok");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, directory_writes,
                                     "title_change_single_publish");
    uint32_t got = read_file("body.txt", readback, (uint32_t)sizeof readback);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE("@yan-note 1\n", readback, 12u,
                                     "title_change_marker_prefix");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(header_bytes_of(readback) + 12u, got,
                                     "title_change_body_length_preserved");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE("line1\r\nline2",
                                     readback + header_bytes_of(readback),
                                     12u, "title_change_body_bytes_preserved");
    memset(&holder, 0, sizeof holder);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_OK,
                                  yan_note_show(&context, "body.txt", &holder),
                                  "title_change_show_ok");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("new", (const char *)holder.title,
                                     "title_change_new_title");
}

static void note_clear_title_and_real_change_write_once(void)
{
    fs_mount_fresh();
    uint32_t length = build_note("title: same\n", (const uint8_t *)"body", 4u);
    fs_make_file("same.txt", file_content, length);
    with_service();
    device_reads = 0u;
    device_writes = 0u;
    directory_writes = 0u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK,
        yan_note_set_title(&context, "same.txt", (const uint8_t *)"same", 4u),
        "title_noop_ok");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, device_writes, "title_noop_no_write");
    TEST_ASSERT_TRUE_MESSAGE(device_reads <= READ_BUDGET,
                             "title_noop_reads_only_the_prefix");
    device_reads = 0u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK, yan_note_set_title(&context, "same.txt", NULL, 0u),
        "title_clear_ok");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, directory_writes,
                                     "title_clear_single_publish");
    memset(&holder, 0, sizeof holder);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_OK,
                                  yan_note_show(&context, "same.txt", &holder),
                                  "title_clear_show_ok");
    TEST_ASSERT_FALSE_MESSAGE(holder.has_title, "title_clear_field_gone");
    uint32_t got = read_file("same.txt", readback, (uint32_t)sizeof readback);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(header_bytes_of(readback) + 4u, got,
                                     "title_clear_body_kept");
}

/* ----------------------------------------------------- no-op and limits */

static void note_no_change_never_reads_body_or_writes(void)
{
    fs_mount_fresh();
    uint32_t body_length = fill_binary(8192u);
    uint32_t length = build_note("title: keep\ntag: os\n", binary_payload,
                                 body_length);
    fs_make_file("binary.bin", file_content, length);
    with_service();
    device_reads = 0u;
    device_writes = 0u;
    directory_writes = 0u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK,
        yan_note_tag(&context, "binary.bin", YAN_NOTE_MANAGE_ADD, text_tag,
                     (uint32_t)(sizeof text_tag - 1u)),
        "noop_binary_add_existing_ok");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK,
        yan_note_tag(&context, "binary.bin", YAN_NOTE_MANAGE_DELETE,
                     text_tag_extra,
                     (uint32_t)(sizeof text_tag_extra - 1u)),
        "noop_binary_delete_missing_ok");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK,
        yan_note_set_title(&context, "binary.bin", (const uint8_t *)"keep", 4u),
        "noop_binary_same_title_ok");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, device_writes, "noop_binary_no_write");
    TEST_ASSERT_TRUE_MESSAGE(device_reads <= READ_BUDGET * 3u,
                             "noop_binary_no_body_stream");

    memset(large_payload, 'y', sizeof large_payload);
    length = build_note("title: big\ntag: os\n", large_payload, 20000u);
    fs_make_file("large.txt", file_content, length);
    device_reads = 0u;
    device_writes = 0u;
    directory_writes = 0u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK,
        yan_note_tag(&context, "large.txt", YAN_NOTE_MANAGE_ADD, text_tag,
                     (uint32_t)(sizeof text_tag - 1u)),
        "noop_large_add_existing_ok");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, device_writes, "noop_large_no_write");
    TEST_ASSERT_TRUE_MESSAGE(device_reads <= READ_BUDGET,
                             "noop_large_reads_only_the_prefix");
}

static void note_exact_candidate_capacity_is_accepted(void)
{
    fs_mount_fresh();
    uint32_t header = build_note("title: edge\n", NULL, 0u);
    uint32_t body_length = YAN_NOTE_CANDIDATE_BYTES - header;
    memset(large_payload, 'z', body_length);
    memcpy(file_content + header, large_payload, body_length);
    fs_make_file("edge.txt", file_content, header + body_length);
    with_service();
    device_writes = 0u;
    directory_writes = 0u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK,
        yan_note_set_title(&context, "edge.txt", (const uint8_t *)"next", 4u),
        "edge_real_change_ok");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, directory_writes,
                                     "edge_real_change_single_write");
    uint32_t got = read_file("edge.txt", readback, (uint32_t)sizeof readback);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(YAN_NOTE_CANDIDATE_BYTES, got,
                                     "edge_result_is_exactly_candidate_budget");
}

static void note_change_over_candidate_capacity_is_too_large(void)
{
    fs_mount_fresh();
    uint32_t header = build_note("title: over\n", NULL, 0u);
    uint32_t body_length = (YAN_NOTE_CANDIDATE_BYTES + 1u) - header;
    memset(large_payload, 'z', body_length);
    memcpy(file_content + header, large_payload, body_length);
    fs_make_file("over.txt", file_content, header + body_length);
    with_service();
    YanFsSource before;
    memset(&before, 0, sizeof before);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK, yan_fs_source(&fs, &before),
                                  "over_source_before");
    device_writes = 0u;
    directory_writes = 0u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_TOO_LARGE,
        yan_note_set_title(&context, "over.txt", (const uint8_t *)"next", 4u),
        "over_real_change_too_large");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, device_writes,
                                     "over_too_large_zero_writes");
    YanFsSource after;
    memset(&after, 0, sizeof after);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK, yan_fs_source(&fs, &after),
                                  "over_source_after");
    TEST_ASSERT_TRUE_MESSAGE(after.token == before.token,
                             "over_too_large_keeps_token");
}

static void note_real_change_requires_valid_editor_text(void)
{
    fs_mount_fresh();
    uint32_t body_length = fill_binary(256u);
    uint32_t length = build_note("title: bin\n", binary_payload, body_length);
    fs_make_file("invalid.txt", file_content, length);
    with_service();
    medium_snapshot();
    device_writes = 0u;
    directory_writes = 0u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_INVALID_TEXT,
        yan_note_set_title(&context, "invalid.txt", (const uint8_t *)"new", 3u),
        "invalid_text_real_change_rejected");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, device_writes,
                                     "invalid_text_zero_writes");
    TEST_ASSERT_TRUE_MESSAGE(medium_unchanged(),
                             "invalid_text_medium_unchanged");
}

/* ----------------------------------------------------------- fault paths */

static void note_prefix_fault_propagates_and_keeps_info(void)
{
    fs_mount_fresh();
    uint32_t length = build_note("title: f\n", (const uint8_t *)"body", 4u);
    fs_make_file("fault.txt", file_content, length);
    with_service();
    memset(&holder, 0x5a, sizeof holder);
    memcpy(info_snapshot, &holder, sizeof info_snapshot);
    device_armed = true;
    device_reads = 0u;
    device_read_fail_at = 1u; /* stat is cached; prefix is the first read */
    device_read_fail_code = YAN_FS_IO_ERROR;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_IO,
                                  yan_note_show(&context, "fault.txt", &holder),
                                  "fault_show_prefix_io");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(info_snapshot, &holder, sizeof info_snapshot,
                                     "fault_show_keeps_info");
    device_quiet();
    fs_mount_fresh();
    fs_make_file("fault.txt", file_content, length);
    with_service();
    device_reads = 0u;
    device_armed = true;
    device_read_fail_at = 1u;
    device_read_fail_code = YAN_FS_IO_PROTOCOL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_PROTOCOL,
                                  yan_note_show(&context, "fault.txt", &holder),
                                  "fault_show_prefix_protocol");
    device_quiet();
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(info_snapshot, &holder, sizeof info_snapshot,
                                     "fault_protocol_keeps_info");
}

static void note_ordinary_error_keeps_source_identity(void)
{
    fs_mount_fresh();
    uint32_t length = build_note("title: n\n", (const uint8_t *)"body", 4u);
    fs_make_file("ordinary.txt", file_content, length);
    with_service();
    YanFsSource before;
    memset(&before, 0, sizeof before);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK, yan_fs_source(&fs, &before),
                                  "ordinary_source_before");
    medium_snapshot();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_INVALID,
                                  yan_note_link(&context, "ordinary.txt",
                                                YAN_NOTE_MANAGE_ADD, "bad/name"),
                                  "ordinary_invalid_name_rejected");
    TEST_ASSERT_TRUE_MESSAGE(medium_unchanged(),
                             "ordinary_invalid_keeps_medium");
    YanFsSource after;
    memset(&after, 0, sizeof after);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK, yan_fs_source(&fs, &after),
                                  "ordinary_source_after");
    TEST_ASSERT_TRUE_MESSAGE(after.token == before.token,
                             "ordinary_invalid_keeps_token");
}

static void note_busy_context_wins_and_reentry_is_busy(void)
{
    fs_mount_fresh();
    uint32_t length = build_note("title: b\n", (const uint8_t *)"body", 4u);
    fs_make_file("busy.txt", file_content, length);
    with_service();
    context.busy = true;
    memset(&holder, 0x5a, sizeof holder);
    memcpy(info_snapshot, &holder, sizeof info_snapshot);
    device_reads = 0u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_BUSY,
                                  yan_note_show(&context, "busy.txt", &holder),
                                  "busy_context_show");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(info_snapshot, &holder, sizeof info_snapshot,
                                     "busy_context_keeps_info");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, device_reads, "busy_context_no_io");
    context.busy = false;
    memset(&holder, 0, sizeof holder);
    reenter_armed = true;
    reenter_context = &context;
    reenter_result = YAN_NOTE_OK;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_OK,
                                  yan_note_show(&context, "busy.txt", &holder),
                                  "busy_reentry_outer_ok");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_BUSY, reenter_result,
                                  "busy_reentry_inner_busy");
}

static void note_fs_state_error_precedes_parameter_checks(void)
{
    fs_mount_fresh();
    with_service();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK, yan_fs_unmount(&fs),
                                  "fsstate_unmount");
    memset(&holder, 0x5a, sizeof holder);
    memcpy(info_snapshot, &holder, sizeof info_snapshot);
    device_reads = 0u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_NOT_MOUNTED,
                                  yan_note_show(&context, NULL, &holder),
                                  "fsstate_not_mounted_first");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(info_snapshot, &holder, sizeof info_snapshot,
                                     "fsstate_error_keeps_info");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, device_reads, "fsstate_error_no_io");
}

static void note_aliased_arguments_are_rejected_without_io(void)
{
    fs_mount_fresh();
    uint32_t length = build_note("title: a\n", NULL, 0u);
    fs_make_file("alias2.txt", file_content, length);
    with_service();
    memcpy(context.prefix, "alias2.txt", 11u);
    memcpy(holder_storage, "alias2.txt", 11u);
    device_reads = 0u;
    device_writes = 0u;
    directory_writes = 0u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_INVALID,
        yan_note_set_title(&context, (const char *)context.prefix,
                           (const uint8_t *)"x", 1u),
        "alias2_name_inside_context_invalid");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_INVALID,
        yan_note_show(&context, (const char *)holder_storage,
                      (YanNoteInfo *)(void *)holder_storage),
        "alias2_name_inside_out_invalid");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_INVALID,
        yan_note_set_date(&context, "alias2.txt", YAN_NOTE_DATE_MODIFIED,
                          (const uint8_t *)(uintptr_t)UINTPTR_MAX, 10u),
        "alias2_wrapping_date_name_invalid");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, device_writes, "alias2_no_writes");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, device_reads, "alias2_no_reads");
}

/* ------------------------------------------------------------- registry */

static void note_date_link_tag_management_preserves_order(void)
{
    fs_mount_fresh();
    fs_make_file("note.txt", (const uint8_t *)"body\r\nlast", 10u);
    with_service();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_OK,
        yan_note_set_date(&context, "note.txt", YAN_NOTE_DATE_CREATED,
                          (const uint8_t *)"2000-02-29", 10u), "created_actual_set");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_OK,
        yan_note_set_date(&context, "note.txt", YAN_NOTE_DATE_MODIFIED,
                          (const uint8_t *)"2026-10-08", 10u), "modified_actual_set");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_OK,
        yan_note_tag(&context, "note.txt", YAN_NOTE_MANAGE_ADD,
                     (const uint8_t *)"z", 1u), "tag_first_actual_add");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_OK,
        yan_note_tag(&context, "note.txt", YAN_NOTE_MANAGE_ADD,
                     (const uint8_t *)"a", 1u), "tag_second_actual_add");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_OK,
        yan_note_link(&context, "note.txt", YAN_NOTE_MANAGE_ADD, "z.txt"),
        "link_first_actual_add");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_OK,
        yan_note_link(&context, "note.txt", YAN_NOTE_MANAGE_ADD, "a.txt"),
        "link_second_actual_add");
    static const uint8_t expected[] =
        "@yan-note 1\ncreated: 2000-02-29\nmodified: 2026-10-08\n"
        "tag: z\ntag: a\nlink: z.txt\nlink: a.txt\n@end\nbody\r\nlast";
    uint32_t got = read_file("note.txt", readback, sizeof readback);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(sizeof expected - 1u, got, "management_canonical_size");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(expected, readback, sizeof expected - 1u,
                                      "management_canonical_insertion_order_body");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_OK,
        yan_note_set_date(&context, "note.txt", YAN_NOTE_DATE_CREATED, NULL, 0u),
        "created_actual_clear");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_OK,
        yan_note_set_date(&context, "note.txt", YAN_NOTE_DATE_MODIFIED, NULL, 0u),
        "modified_actual_clear");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_OK,
        yan_note_tag(&context, "note.txt", YAN_NOTE_MANAGE_DELETE,
                     (const uint8_t *)"z", 1u), "tag_actual_delete_first");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_OK,
        yan_note_tag(&context, "note.txt", YAN_NOTE_MANAGE_DELETE,
                     (const uint8_t *)"a", 1u), "tag_actual_delete_last");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_OK,
        yan_note_link(&context, "note.txt", YAN_NOTE_MANAGE_DELETE, "z.txt"),
        "link_actual_delete_first");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_OK,
        yan_note_link(&context, "note.txt", YAN_NOTE_MANAGE_DELETE, "a.txt"),
        "link_actual_delete_last");
    static const uint8_t empty[] = "@yan-note 1\n@end\nbody\r\nlast";
    got = read_file("note.txt", readback, sizeof readback);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(sizeof empty - 1u, got, "clear_last_retains_empty_header_size");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(empty, readback, sizeof empty - 1u,
                                      "clear_last_retains_empty_header_body");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(note_date_link_tag_management_preserves_order);
    RUN_TEST(note_init_records_a_borrowed_fs_without_io);
    RUN_TEST(note_init_rejects_bad_context_and_fs);
    RUN_TEST(note_service_rejects_context_fs_overlap);
    RUN_TEST(note_uninitialized_context_rejects_every_call);
    RUN_TEST(note_show_no_header_binary_is_an_old_note);
    RUN_TEST(note_show_legal_header_over_binary_body);
    RUN_TEST(note_show_reports_existing_and_dangling_links);
    RUN_TEST(note_show_large_body_stays_in_the_prefix_budget);
    RUN_TEST(note_show_missing_file_is_not_created);
    RUN_TEST(note_show_broken_header_is_metadata_and_keeps_info);
    RUN_TEST(note_show_rejects_alias_and_wrapping_holders);
    RUN_TEST(note_list_bad_header_in_last_slot_reserves_everything);
    RUN_TEST(note_list_orders_names_and_applies_and_filter);
    RUN_TEST(note_list_accepts_a_stop_and_reports_stopped);
    RUN_TEST(note_tag_real_change_publishes_and_takes_new_token);
    RUN_TEST(note_set_title_rewrites_and_keeps_body_bytes);
    RUN_TEST(note_clear_title_and_real_change_write_once);
    RUN_TEST(note_no_change_never_reads_body_or_writes);
    RUN_TEST(note_exact_candidate_capacity_is_accepted);
    RUN_TEST(note_change_over_candidate_capacity_is_too_large);
    RUN_TEST(note_real_change_requires_valid_editor_text);
    RUN_TEST(note_prefix_fault_propagates_and_keeps_info);
    RUN_TEST(note_ordinary_error_keeps_source_identity);
    RUN_TEST(note_busy_context_wins_and_reentry_is_busy);
    RUN_TEST(note_fs_state_error_precedes_parameter_checks);
    RUN_TEST(note_aliased_arguments_are_rejected_without_io);
    return UNITY_END();
}
