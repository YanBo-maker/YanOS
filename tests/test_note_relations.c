/* 0028 knowledge-relations service: native tests, written before the query
 * implementation.
 *
 * The contract is os/note_relations.h and
 * docs/specs/0028-knowledge-relations.md ("公共服务与接口责任"). test_relation_init
 * and queries are implemented; the initial stub's semantic RED is retained
 * separately in build/search-relations-red.log. The assertions below exercise
 * the real service, not a compile/link shortcut. The device is a caller-owned static 8 MiB byte
 * array behind the real synchronous YanFsBlockIo callbacks, and every fixture
 * is static: the service context, the 20 KiB payload and the medium snapshot
 * never touch the stack.
 *
 * What this file pins, clause by clause:
 *
 *   - init records a borrowed filesystem, reads no block, allows an idle
 *     re-init and rejects a null/overlapping pair and an unusable filesystem;
 *   - every query validates the context extent before any bool, then
 *     initialized, BUSY, the borrowed FS state, the parameters/spans/aliases
 *     and the per-byte 32-byte name, with zero device bytes touched;
 *   - links follows declaration order (not ASCII), keeps a self-link, reports a
 *     dangling target as exists=false, ignores a broken target head, fails
 *     METADATA on a broken source with no row/summary, and succeeds with zero
 *     rows on an old header-less file;
 *   - backlinks allows a missing cited name, orders ASCII, keeps the target's
 *     own self-declaration, matches exact ASCII case only, prechecks the whole
 *     directory before the first row, and covers a full 63-source directory;
 *   - related reports NOT_FOUND for a missing source before a remote bad head,
 *     ranks (out+in) before shared tags, breaks ties by ASCII, keeps shared-tag
 *     identity exact (case, empty sets), caps the rows at 20 with the full
 *     total, excludes the source and dangling names from rows, prechecks
 *     non-candidates, and treats old header-less and large binary files as
 *     ordinary zero-metadata candidates;
 *   - every query is zero-write and re-observes the directory after a real
 *     filesystem mutation;
 *   - a sink stop is STOPPED with no summary, and re-entering the service from
 *     a callback is BUSY with no extra I/O;
 *   - a real prefix I/O or protocol error propagates unchanged and leaves the
 *     filesystem FAULTED for the next call.
 *
 * Every assertion carries a stable, unique message tag for the later
 * owner-specific mutation gate. */

#include "note_relations.h"
#include "fs_test_support.h"
#include "note_metadata_format.h"
#include "yanfs.h"
#include "unity.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------- fixtures */

#define RELATION_BLOCKS 2048u /* 8 MiB */
#define READ_BUDGET 1u        /* a header observation must not stream the body */

static uint8_t medium[RELATION_BLOCKS][YAN_FS_BLOCK_SIZE];
static uint8_t medium_before[RELATION_BLOCKS][YAN_FS_BLOCK_SIZE];
static uint32_t device_reads;
static uint32_t device_writes;
static uint32_t device_read_fail_at;
static YanFsIoResult device_read_fail_code;
static bool device_armed;
static YanFs fs;
static YanRelationContext context;

static uint8_t file_content[24096];
static uint8_t binary_payload[20000];
static uint8_t large_payload[20000];
static uint8_t readback[20000];

static YanRelationRow rows_seen[YAN_FS_MAX_FILES];
static uint32_t rows_seen_count;
static bool sink_stop_after_one;

static bool reenter_armed;
static YanRelationContext *reenter_context;
static YanNoteResult reenter_result;
static uint32_t io_at_reentry;

static YanRelationSummary summary;

/* ------------------------------------------------------------ byte device */

static YanFsIoResult device_capacity(void *context_pointer, uint64_t *blocks)
{
    (void)context_pointer;
    *blocks = RELATION_BLOCKS;
    return YAN_FS_IO_OK;
}

static YanFsIoResult device_read(void *context_pointer, uint32_t lba,
                                 uint8_t out[4096])
{
    (void)context_pointer;
    ++device_reads;
    if (device_armed && device_read_fail_at != 0u &&
        device_reads == device_read_fail_at) {
        return device_read_fail_code;
    }
    if (lba >= RELATION_BLOCKS) {
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
    if (lba >= RELATION_BLOCKS) {
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
    memset(&context, 0, sizeof context);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK,
                                  yan_fs_format_metadata(medium[0], RELATION_BLOCKS),
                                  "relation_fixture_format");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK, test_fs_init(&fs, fs_backend()),
                                  "relation_fixture_init");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK, yan_fs_mount(&fs),
                                  "relation_fixture_mount");
}

static void with_service(void)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_OK, test_relation_init(&context, &fs),
                                  "relation_service_init");
}

static void fs_make_file(const char *name, const uint8_t *bytes, uint32_t length)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK,
                                  yan_fs_create(&fs, name, bytes, length),
                                  "relation_fixture_create");
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

static uint32_t fill_binary(uint32_t length)
{
    for (uint32_t i = 0u; i < length; ++i) {
        binary_payload[i] = (uint8_t)(0x80u + (i & 0x3Fu));
    }
    return length;
}

static void append_text(char *buffer, size_t capacity, const char *text)
{
    size_t used = strlen(buffer);
    size_t need = strlen(text);
    TEST_ASSERT_TRUE_MESSAGE(used + need < capacity,
                             "fixture_append_buffer_room");
    memcpy(buffer + used, text, need + 1u);
}

static void append_tag(char *buffer, size_t capacity, uint32_t index)
{
    size_t used = strlen(buffer);
    TEST_ASSERT_TRUE_MESSAGE(used < capacity, "fixture_tag_buffer_room");
    (void)snprintf(buffer + used, capacity - used, "tag: t%02u\n", index);
}

static void collect_reset(void)
{
    memset(rows_seen, 0, sizeof rows_seen);
    rows_seen_count = 0u;
    sink_stop_after_one = false;
}

static bool collect_rows(void *sink_context, const YanRelationRow *row)
{
    (void)sink_context;
    TEST_ASSERT_TRUE_MESSAGE(rows_seen_count < YAN_FS_MAX_FILES,
                             "collect_row_within_capacity");
    rows_seen[rows_seen_count] = *row;
    rows_seen_count += 1u;

    if (reenter_armed) {
        reenter_armed = false;
        io_at_reentry = device_reads;
        reenter_result = yan_relation_links(reenter_context, "busy.txt",
                                            collect_rows, NULL, NULL);
    }
    if (sink_stop_after_one && rows_seen_count == 1u) {
        return false;
    }
    return true;
}

/* Publish sentinels into the summary so a non-OK result can be checked for
 * "summary untouched" without a byte snapshot. */
static void summary_mark(void)
{
    summary.total = UINT32_C(0xAAAAAAAA);
    summary.shown = UINT32_C(0xBBBBBBBB);
    summary.target_exists = true;
}

static bool summary_unpublished(void)
{
    return summary.total == UINT32_C(0xAAAAAAAA) &&
           summary.shown == UINT32_C(0xBBBBBBBB) && summary.target_exists;
}

void setUp(void)
{
    memset(medium, 0, sizeof medium);
    memset(medium_before, 0, sizeof medium_before);
    device_reads = 0u;
    device_writes = 0u;
    device_read_fail_at = 0u;
    device_read_fail_code = YAN_FS_IO_ERROR;
    device_armed = false;
    memset(&fs, 0, sizeof fs);
    memset(&context, 0, sizeof context);
    memset(file_content, 0, sizeof file_content);
    memset(binary_payload, 0, sizeof binary_payload);
    memset(large_payload, 0, sizeof large_payload);
    memset(readback, 0, sizeof readback);
    memset(&summary, 0, sizeof summary);
    collect_reset();
    reenter_armed = false;
    reenter_context = NULL;
    reenter_result = YAN_NOTE_OK;
    io_at_reentry = 0u;
    device_quiet();
}

void tearDown(void)
{
}

/* ------------------------------------------------------ context lifetime */

static void relation_init_records_a_borrowed_fs_without_io(void)
{
    fs_mount_fresh();
    uint32_t reads = device_reads;
    uint32_t writes = device_writes;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_OK, test_relation_init(&context, &fs),
                                  "relation_init_ok");
    TEST_ASSERT_TRUE_MESSAGE(context.initialized,
                             "relation_init_sets_initialized");
    TEST_ASSERT_FALSE_MESSAGE(context.busy, "relation_init_clears_busy");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(reads, device_reads,
                                     "relation_init_reads_no_block");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(writes, device_writes,
                                     "relation_init_writes_no_block");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_OK,
                                  test_relation_init(&context, &fs),
                                  "relation_init_idle_reuse_ok");
}

static void relation_init_rejects_bad_context_and_fs(void)
{
    fs_mount_fresh();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_INVALID,
                                  test_relation_init(NULL, &fs),
                                  "relation_init_null_context_invalid");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_INVALID,
                                  test_relation_init(&context, NULL),
                                  "relation_init_null_fs_invalid");
    memset(&fs, 0, sizeof fs);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_INVALID,
                                  test_relation_init(&context, &fs),
                                  "relation_init_uninitialized_fs_invalid");
}

static void relation_init_rejects_context_fs_overlap(void)
{
    fs_mount_fresh();
    YanFs *overlap = (YanFs *)(void *)&context;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_INVALID,
                                  test_relation_init(&context, overlap),
                                  "relation_init_context_fs_overlap_invalid");
    YanRelationContext *alias = (YanRelationContext *)(void *)&fs;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_INVALID,
                                  test_relation_init(alias, &fs),
                                  "relation_init_fs_context_overlap_invalid");
}

static void relation_uninitialized_context_rejects_every_query(void)
{
    fs_mount_fresh();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_INVALID,
        yan_relation_links(&context, "a.txt", collect_rows, NULL, &summary),
        "relation_links_uninitialized_invalid");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_INVALID,
        yan_relation_backlinks(&context, "a.txt", collect_rows, NULL, &summary),
        "relation_backlinks_uninitialized_invalid");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_INVALID,
        yan_relation_related(&context, "a.txt", collect_rows, NULL, &summary),
        "relation_related_uninitialized_invalid");
}

/* --------------------------------------------------- lifecycle guard order */

static void relation_query_rejects_bad_parameters_with_zero_io(void)
{
    fs_mount_fresh();
    with_service();
    device_reads = 0u;
    device_writes = 0u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_INVALID,
        yan_relation_links(&context, "a.txt", NULL, NULL, &summary),
        "relation_links_null_sink_invalid");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_INVALID,
        yan_relation_links(&context, NULL, collect_rows, NULL, &summary),
        "relation_links_null_name_invalid");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_INVALID,
        yan_relation_links(&context, (const char *)&summary, collect_rows, NULL,
                           &summary),
        "relation_name_summary_overlap_invalid");
    YanRelationSummary *wrapping =
        (YanRelationSummary *)(uintptr_t)UINTPTR_MAX;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_INVALID,
        yan_relation_links(&context, "a.txt", collect_rows, NULL, wrapping),
        "relation_wrapping_summary_invalid");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, device_writes,
                                     "relation_bad_parameters_no_write");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, device_reads,
                                     "relation_bad_parameters_no_read");
}

static void relation_query_rejects_wrapping_context_and_name(void)
{
    fs_mount_fresh();
    with_service();
    device_reads = 0u;
    device_writes = 0u;
    YanRelationContext *wrapping_context =
        (YanRelationContext *)(uintptr_t)UINTPTR_MAX;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_INVALID,
        yan_relation_links(wrapping_context, "a.txt", collect_rows, NULL,
                           &summary),
        "relation_wrapping_context_invalid");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_INVALID,
        yan_relation_links(&context, (const char *)(uintptr_t)UINTPTR_MAX,
                           collect_rows, NULL, &summary),
        "relation_wrapping_name_invalid");
    char long_name[40];
    memset(long_name, 'a', sizeof long_name - 1u);
    long_name[sizeof long_name - 1u] = '\0';
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_INVALID,
        yan_relation_links(&context, long_name, collect_rows, NULL, &summary),
        "relation_overlong_name_invalid");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, device_writes,
                                     "relation_field_guard_no_write");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, device_reads,
                                     "relation_field_guard_no_read");
}

static void relation_unmounted_state_precedes_parameter_checks(void)
{
    fs_mount_fresh();
    with_service();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK, yan_fs_unmount(&fs),
                                  "relation_fixture_unmount");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_NOT_MOUNTED,
        yan_relation_links(&context, "a.txt", NULL, NULL, &summary),
        "relation_query_unmounted_precedes_parameters");
}

static void relation_faulted_state_precedes_parameter_checks(void)
{
    fs_mount_fresh();
    fs_make_file("body.txt", (const uint8_t *)"body", 4u);
    with_service();
    uint32_t got = 0u;
    device_armed = true;
    device_read_fail_at = device_reads + 1u;
    device_read_fail_code = YAN_FS_IO_ERROR;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_FS_IO,
        yan_fs_read(&fs, "body.txt", 0u, readback, sizeof readback, &got),
        "relation_fixture_fault_read");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_FAULTED,
        yan_relation_links(&context, "body.txt", NULL, NULL, &summary),
        "relation_query_faulted_precedes_parameters");
}

/* ----------------------------------------------------------- links: walk */

static void relation_links_declaration_order_dangling_and_self(void)
{
    fs_mount_fresh();
    fs_make_file("z.txt", (const uint8_t *)"z", 1u);
    uint32_t length = build_note("link: z.txt\nlink: a.txt\nlink: self.txt\n",
                                 NULL, 0u);
    fs_make_file("self.txt", file_content, length);
    with_service();
    collect_reset();
    summary_mark();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK,
        yan_relation_links(&context, "self.txt", collect_rows, NULL, &summary),
        "relation_links_ok");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(3u, rows_seen_count,
                                     "relation_links_row_count");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("z.txt", rows_seen[0].name,
                                     "relation_links_declaration_first");
    TEST_ASSERT_TRUE_MESSAGE(rows_seen[0].exists,
                             "relation_links_first_exists");
    TEST_ASSERT_FALSE_MESSAGE(rows_seen[0].outgoing,
                              "relation_links_first_outgoing_zero");
    TEST_ASSERT_FALSE_MESSAGE(rows_seen[0].incoming,
                              "relation_links_first_incoming_zero");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0u, rows_seen[0].shared_tags,
                                    "relation_links_first_shared_zero");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("a.txt", rows_seen[1].name,
                                     "relation_links_declaration_second");
    TEST_ASSERT_FALSE_MESSAGE(rows_seen[1].exists,
                              "relation_links_dangling_exists_false");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("self.txt", rows_seen[2].name,
                                     "relation_links_self_last");
    TEST_ASSERT_TRUE_MESSAGE(rows_seen[2].exists,
                             "relation_links_self_exists");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(3u, summary.total,
                                     "relation_links_summary_total");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(3u, summary.shown,
                                     "relation_links_summary_shown");
    TEST_ASSERT_TRUE_MESSAGE(summary.target_exists,
                             "relation_links_summary_target_exists");
}

static void relation_links_ignores_a_broken_target_head(void)
{
    fs_mount_fresh();
    uint32_t length = build_note("kind: note\n", NULL, 0u);
    fs_make_file("bad.txt", file_content, length);
    length = build_note("link: bad.txt\nlink: gone.txt\n", NULL, 0u);
    fs_make_file("src.txt", file_content, length);
    with_service();
    collect_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK,
        yan_relation_links(&context, "src.txt", collect_rows, NULL, &summary),
        "relation_links_bad_target_head_ok");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2u, rows_seen_count,
                                     "relation_links_bad_target_row_count");
    TEST_ASSERT_TRUE_MESSAGE(rows_seen[0].exists,
                             "relation_links_bad_target_directory_only");
    TEST_ASSERT_FALSE_MESSAGE(rows_seen[1].exists,
                              "relation_links_dangling_target_false");
}

static void relation_links_missing_source_is_not_created(void)
{
    fs_mount_fresh();
    with_service();
    collect_reset();
    summary_mark();
    medium_snapshot();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_NOT_FOUND,
        yan_relation_links(&context, "absent.txt", collect_rows, NULL,
                           &summary),
        "relation_links_missing_source_not_found");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, rows_seen_count,
                                     "relation_links_missing_source_no_rows");
    TEST_ASSERT_TRUE_MESSAGE(summary_unpublished(),
                             "relation_links_missing_source_keeps_summary");
    TEST_ASSERT_TRUE_MESSAGE(medium_unchanged(),
                             "relation_links_missing_source_no_write");
    TEST_ASSERT_FALSE_MESSAGE(filename_is_present("absent.txt"),
                              "relation_links_missing_source_no_entry");
}

static void relation_links_broken_source_is_metadata_without_rows(void)
{
    fs_mount_fresh();
    uint32_t length = build_note("kind: note\n", NULL, 0u);
    fs_make_file("broken.txt", file_content, length);
    with_service();
    collect_reset();
    summary_mark();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_METADATA,
        yan_relation_links(&context, "broken.txt", collect_rows, NULL, &summary),
        "relation_links_broken_source_metadata");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, rows_seen_count,
                                     "relation_links_broken_source_no_rows");
    TEST_ASSERT_TRUE_MESSAGE(summary_unpublished(),
                             "relation_links_broken_source_keeps_summary");
}

static void relation_links_old_headerless_source_has_zero_rows(void)
{
    fs_mount_fresh();
    fs_make_file("old.txt", (const uint8_t *)"plain old body", 14u);
    with_service();
    collect_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK,
        yan_relation_links(&context, "old.txt", collect_rows, NULL, &summary),
        "relation_links_old_source_ok");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, rows_seen_count,
                                     "relation_links_old_source_zero_rows");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, summary.total,
                                     "relation_links_old_source_total_zero");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, summary.shown,
                                     "relation_links_old_source_shown_zero");
    TEST_ASSERT_TRUE_MESSAGE(summary.target_exists,
                             "relation_links_old_source_target_exists");
}

static void relation_links_large_body_stays_in_the_prefix_budget(void)
{
    fs_mount_fresh();
    memset(large_payload, 'x', sizeof large_payload);
    fs_make_file("target.txt", (const uint8_t *)"t", 1u);
    uint32_t length = build_note("tag: os\nlink: target.txt\n", large_payload,
                                 20000u);
    fs_make_file("big.txt", file_content, length);
    with_service();
    collect_reset();
    device_reads = 0u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK,
        yan_relation_links(&context, "big.txt", collect_rows, NULL, &summary),
        "relation_links_large_body_ok");
    TEST_ASSERT_TRUE_MESSAGE(device_reads <= READ_BUDGET,
                             "relation_links_large_body_prefix_read_budget");
}

/* -------------------------------------------------------- backlinks: walk */

static void relation_backlinks_missing_name_is_zero_results(void)
{
    fs_mount_fresh();
    uint32_t length = build_note("link: other.txt\n", NULL, 0u);
    fs_make_file("a.txt", file_content, length);
    with_service();
    collect_reset();
    summary_mark();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK,
        yan_relation_backlinks(&context, "ghost.txt", collect_rows, NULL,
                               &summary),
        "relation_backlinks_zero_ok");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, rows_seen_count,
                                     "relation_backlinks_zero_rows");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, summary.total,
                                     "relation_backlinks_zero_total");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, summary.shown,
                                     "relation_backlinks_zero_shown");
    TEST_ASSERT_FALSE_MESSAGE(summary.target_exists,
                              "relation_backlinks_missing_target_false");
}

static void relation_backlinks_orders_ascii_and_keeps_self(void)
{
    fs_mount_fresh();
    uint32_t length = build_note("link: ghost.txt\n", NULL, 0u);
    fs_make_file("zeta.txt", file_content, length);
    fs_make_file("alpha.txt", file_content, length);
    fs_make_file("ghost.txt", file_content, length);
    with_service();
    collect_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK,
        yan_relation_backlinks(&context, "ghost.txt", collect_rows, NULL,
                               &summary),
        "relation_backlinks_order_ok");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(3u, rows_seen_count,
                                     "relation_backlinks_order_count");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("alpha.txt", rows_seen[0].name,
                                     "relation_backlinks_ascii_first");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("ghost.txt", rows_seen[1].name,
                                     "relation_backlinks_self_kept");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("zeta.txt", rows_seen[2].name,
                                     "relation_backlinks_ascii_last");
    TEST_ASSERT_TRUE_MESSAGE(rows_seen[1].exists,
                             "relation_backlinks_self_exists");
    TEST_ASSERT_TRUE_MESSAGE(rows_seen[1].incoming,
                             "relation_backlinks_incoming_true");
    TEST_ASSERT_FALSE_MESSAGE(rows_seen[1].outgoing,
                              "relation_backlinks_outgoing_zero");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0u, rows_seen[1].shared_tags,
                                    "relation_backlinks_shared_zero");
    TEST_ASSERT_TRUE_MESSAGE(summary.target_exists,
                             "relation_backlinks_target_exists");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(3u, summary.total,
                                     "relation_backlinks_total");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(3u, summary.shown,
                                     "relation_backlinks_shown");
}

static void relation_backlinks_match_exact_ascii_case(void)
{
    fs_mount_fresh();
    uint32_t length = build_note("link: Ghost.txt\n", NULL, 0u);
    fs_make_file("a.txt", file_content, length);
    with_service();
    collect_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK,
        yan_relation_backlinks(&context, "ghost.txt", collect_rows, NULL,
                               &summary),
        "relation_backlinks_case_ok");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, rows_seen_count,
                                     "relation_backlinks_case_sensitive");
}

static void relation_backlinks_late_bad_header_reserves_everything(void)
{
    fs_mount_fresh();
    uint32_t length = build_note("link: ghost.txt\n", NULL, 0u);
    fs_make_file("a.txt", file_content, length);
    fs_make_file("b.txt", file_content, length);
    length = build_note("kind: note\n", NULL, 0u);
    fs_make_file("z.txt", file_content, length);
    with_service();
    collect_reset();
    summary_mark();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_METADATA,
        yan_relation_backlinks(&context, "ghost.txt", collect_rows, NULL,
                               &summary),
        "relation_backlinks_late_bad_header_metadata");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, rows_seen_count,
                                     "relation_backlinks_late_bad_header_no_rows");
    TEST_ASSERT_TRUE_MESSAGE(summary_unpublished(),
                             "relation_backlinks_late_bad_header_keeps_summary");
}

static void relation_backlinks_covers_a_full_directory(void)
{
    fs_mount_fresh();
    uint32_t length = build_note("link: ghost.txt\n", NULL, 0u);
    char name[32];
    for (uint32_t i = 0u; i < YAN_FS_MAX_FILES; ++i) {
        (void)snprintf(name, sizeof name, "f%02u.txt", i);
        fs_make_file(name, file_content, length);
    }
    with_service();
    collect_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK,
        yan_relation_backlinks(&context, "ghost.txt", collect_rows, NULL,
                               &summary),
        "relation_backlinks_full_directory_ok");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(63u, rows_seen_count,
                                     "relation_backlinks_full_directory_rows");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("f00.txt", rows_seen[0].name,
                                     "relation_backlinks_full_directory_first");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("f62.txt", rows_seen[62].name,
                                     "relation_backlinks_full_directory_last");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(63u, summary.total,
                                     "relation_backlinks_full_directory_total");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(63u, summary.shown,
                                     "relation_backlinks_full_directory_shown");
    TEST_ASSERT_FALSE_MESSAGE(summary.target_exists,
                              "relation_backlinks_full_directory_target_false");
}

/* ---------------------------------------------------------- related: walk */

static void relation_related_missing_source_outranks_remote_bad_head(void)
{
    fs_mount_fresh();
    uint32_t length = build_note("kind: note\n", NULL, 0u);
    fs_make_file("remote.txt", file_content, length);
    with_service();
    collect_reset();
    summary_mark();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_NOT_FOUND,
        yan_relation_related(&context, "absent.txt", collect_rows, NULL,
                             &summary),
        "relation_related_missing_source_before_remote_bad_head");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, rows_seen_count,
                                     "relation_related_missing_source_no_rows");
    TEST_ASSERT_TRUE_MESSAGE(summary_unpublished(),
                             "relation_related_missing_source_keeps_summary");
}

static void relation_related_bidirectional_outranks_shared_tags(void)
{
    fs_mount_fresh();
    uint32_t length = build_note("link: src.txt\n", NULL, 0u);
    fs_make_file("b.txt", file_content, length);
    length = build_note("tag: os\ntag: riscv\ntag: x\n", NULL, 0u);
    fs_make_file("c.txt", file_content, length);
    length = build_note(
        "tag: os\ntag: riscv\ntag: x\nlink: b.txt\nlink: c.txt\n", NULL, 0u);
    fs_make_file("src.txt", file_content, length);
    with_service();
    collect_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK,
        yan_relation_related(&context, "src.txt", collect_rows, NULL, &summary),
        "relation_related_order_ok");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2u, rows_seen_count,
                                     "relation_related_order_count");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("b.txt", rows_seen[0].name,
                                     "relation_related_bidirectional_first");
    TEST_ASSERT_TRUE_MESSAGE(rows_seen[0].outgoing,
                             "relation_related_bidirectional_out");
    TEST_ASSERT_TRUE_MESSAGE(rows_seen[0].incoming,
                             "relation_related_bidirectional_in");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0u, rows_seen[0].shared_tags,
                                    "relation_related_bidirectional_tags");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("c.txt", rows_seen[1].name,
                                     "relation_related_one_way_second");
    TEST_ASSERT_TRUE_MESSAGE(rows_seen[1].outgoing,
                             "relation_related_one_way_out");
    TEST_ASSERT_FALSE_MESSAGE(rows_seen[1].incoming,
                              "relation_related_one_way_in");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(3u, rows_seen[1].shared_tags,
                                    "relation_related_one_way_tags");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2u, summary.total,
                                     "relation_related_order_total");
    TEST_ASSERT_TRUE_MESSAGE(summary.target_exists,
                             "relation_related_order_target_exists");
}

static void relation_related_breaks_ties_by_ascii_filename(void)
{
    fs_mount_fresh();
    uint32_t length = build_note("link: src.txt\n", NULL, 0u);
    fs_make_file("zebra.txt", file_content, length);
    fs_make_file("alpha.txt", file_content, length);
    length = build_note(NULL, NULL, 0u);
    fs_make_file("src.txt", file_content, length);
    with_service();
    collect_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK,
        yan_relation_related(&context, "src.txt", collect_rows, NULL, &summary),
        "relation_related_tiebreak_ok");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2u, rows_seen_count,
                                     "relation_related_tiebreak_count");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("alpha.txt", rows_seen[0].name,
                                     "relation_related_tiebreak_ascii_first");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("zebra.txt", rows_seen[1].name,
                                     "relation_related_tiebreak_ascii_second");
}

static void relation_related_shared_tags_are_exact_bytes(void)
{
    fs_mount_fresh();
    uint32_t length = build_note("tag: os\nlink: src.txt\n", NULL, 0u);
    fs_make_file("exact.txt", file_content, length);
    length = build_note("tag: OS\nlink: src.txt\n", NULL, 0u);
    fs_make_file("folded.txt", file_content, length);
    length = build_note("link: src.txt\n", NULL, 0u);
    fs_make_file("empty.txt", file_content, length);
    length = build_note("tag: os\n", NULL, 0u);
    fs_make_file("src.txt", file_content, length);
    with_service();
    collect_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK,
        yan_relation_related(&context, "src.txt", collect_rows, NULL, &summary),
        "relation_related_exact_tags_ok");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(3u, rows_seen_count,
                                     "relation_related_exact_tags_count");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("exact.txt", rows_seen[0].name,
                                     "relation_related_exact_tag_first");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1u, rows_seen[0].shared_tags,
                                    "relation_related_exact_tag_one");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("empty.txt", rows_seen[1].name,
                                     "relation_related_empty_tag_ascii_first");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0u, rows_seen[1].shared_tags,
                                    "relation_related_empty_tag_zero");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("folded.txt", rows_seen[2].name,
                                     "relation_related_case_folded_second");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0u, rows_seen[2].shared_tags,
                                    "relation_related_case_not_matched");
}

static void relation_related_counts_sixteen_shared_tags(void)
{
    fs_mount_fresh();
    char header[512];
    header[0] = '\0';
    for (uint32_t i = 0u; i < 16u; ++i) {
        append_tag(header, sizeof header, i);
    }
    append_text(header, sizeof header, "link: src.txt\n");
    uint32_t length = build_note(header, NULL, 0u);
    fs_make_file("cand.txt", file_content, length);
    header[0] = '\0';
    for (uint32_t i = 0u; i < 16u; ++i) {
        append_tag(header, sizeof header, i);
    }
    length = build_note(header, NULL, 0u);
    fs_make_file("src.txt", file_content, length);
    with_service();
    collect_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK,
        yan_relation_related(&context, "src.txt", collect_rows, NULL, &summary),
        "relation_related_sixteen_tags_ok");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, rows_seen_count,
                                     "relation_related_sixteen_tags_count");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(16u, rows_seen[0].shared_tags,
                                    "relation_related_sixteen_tags_value");
    TEST_ASSERT_TRUE_MESSAGE(rows_seen[0].incoming,
                             "relation_related_sixteen_tags_incoming");
    TEST_ASSERT_FALSE_MESSAGE(rows_seen[0].outgoing,
                              "relation_related_sixteen_tags_outgoing_zero");
}

static void relation_related_excludes_self_and_dangling(void)
{
    fs_mount_fresh();
    uint32_t length = build_note("link: src.txt\nlink: ghost.txt\n", NULL, 0u);
    fs_make_file("src.txt", file_content, length);
    with_service();
    collect_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK,
        yan_relation_related(&context, "src.txt", collect_rows, NULL, &summary),
        "relation_related_self_dangling_ok");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, rows_seen_count,
                                     "relation_related_self_dangling_no_rows");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, summary.total,
                                     "relation_related_self_dangling_total");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, summary.shown,
                                     "relation_related_self_dangling_shown");
}

static void relation_related_shows_twenty_with_the_full_total(void)
{
    fs_mount_fresh();
    uint32_t length = build_note("link: src.txt\n", NULL, 0u);
    char name[32];
    for (uint32_t i = 0u; i < 25u; ++i) {
        (void)snprintf(name, sizeof name, "c%02u.txt", i);
        fs_make_file(name, file_content, length);
    }
    length = build_note(NULL, NULL, 0u);
    fs_make_file("src.txt", file_content, length);
    with_service();
    collect_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK,
        yan_relation_related(&context, "src.txt", collect_rows, NULL, &summary),
        "relation_related_top20_ok");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(20u, rows_seen_count,
                                     "relation_related_top20_rows");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("c00.txt", rows_seen[0].name,
                                     "relation_related_top20_ascii_first");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("c19.txt", rows_seen[19].name,
                                     "relation_related_top20_ascii_last");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(25u, summary.total,
                                     "relation_related_top20_full_total");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(20u, summary.shown,
                                     "relation_related_top20_shown");
    TEST_ASSERT_TRUE_MESSAGE(summary.target_exists,
                             "relation_related_top20_target_exists");
}

static void relation_related_prechecks_a_noncandidate_bad_head(void)
{
    fs_mount_fresh();
    uint32_t length = build_note("link: src.txt\n", NULL, 0u);
    fs_make_file("cand.txt", file_content, length);
    length = build_note("kind: note\n", NULL, 0u);
    fs_make_file("bad.txt", file_content, length);
    length = build_note(NULL, NULL, 0u);
    fs_make_file("src.txt", file_content, length);
    with_service();
    collect_reset();
    summary_mark();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_METADATA,
        yan_relation_related(&context, "src.txt", collect_rows, NULL, &summary),
        "relation_related_noncandidate_bad_head_metadata");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, rows_seen_count,
                                     "relation_related_noncandidate_no_rows");
    TEST_ASSERT_TRUE_MESSAGE(summary_unpublished(),
                             "relation_related_noncandidate_keeps_summary");
}

static void relation_related_observes_old_and_large_binary_candidates(void)
{
    fs_mount_fresh();
    uint32_t length = build_note("link: old.txt\nlink: big.dat\n", NULL, 0u);
    fs_make_file("src.txt", file_content, length);
    fs_make_file("old.txt", (const uint8_t *)"plain old body", 14u);
    uint32_t body_length = fill_binary(20000u);
    length = build_note("title: big\n", binary_payload, body_length);
    fs_make_file("big.dat", file_content, length);
    with_service();
    collect_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK,
        yan_relation_related(&context, "src.txt", collect_rows, NULL, &summary),
        "relation_related_old_binary_ok");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2u, rows_seen_count,
                                     "relation_related_old_binary_count");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("big.dat", rows_seen[0].name,
                                     "relation_related_old_binary_ascii_first");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("old.txt", rows_seen[1].name,
                                     "relation_related_old_binary_old_second");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0u, rows_seen[0].shared_tags,
                                    "relation_related_big_zero_tags");
    TEST_ASSERT_FALSE_MESSAGE(rows_seen[1].incoming,
                              "relation_related_old_zero_incoming");
}

/* ----------------------------------------------- observation and mutation */

static void relation_query_is_zero_write_and_fresh_after_removal(void)
{
    fs_mount_fresh();
    uint32_t length = build_note("link: gone.txt\n", NULL, 0u);
    fs_make_file("src.txt", file_content, length);
    fs_make_file("gone.txt", (const uint8_t *)"g", 1u);
    with_service();
    collect_reset();
    device_writes = 0u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK,
        yan_relation_links(&context, "src.txt", collect_rows, NULL, &summary),
        "relation_freshness_first_ok");
    TEST_ASSERT_TRUE_MESSAGE(rows_seen[0].exists,
                             "relation_freshness_target_present");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, device_writes,
                                     "relation_query_zero_writes");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK, yan_fs_remove(&fs, "gone.txt"),
                                  "relation_freshness_remove_ok");
    collect_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK,
        yan_relation_links(&context, "src.txt", collect_rows, NULL, &summary),
        "relation_freshness_second_ok");
    TEST_ASSERT_FALSE_MESSAGE(rows_seen[0].exists,
                              "relation_freshness_after_removal_dangling");
}

static void relation_prefix_io_error_propagates_and_sticks(void)
{
    fs_mount_fresh();
    uint32_t length = build_note("link: a.txt\n", NULL, 0u);
    fs_make_file("src.txt", file_content, length);
    with_service();
    collect_reset();
    device_armed = true;
    device_read_fail_at = device_reads + 1u;
    device_read_fail_code = YAN_FS_IO_ERROR;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_IO,
        yan_relation_links(&context, "src.txt", collect_rows, NULL, &summary),
        "relation_prefix_io_propagates");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_FAULTED,
        yan_relation_links(&context, "src.txt", collect_rows, NULL, &summary),
        "relation_prefix_io_leaves_faulted");
}

static void relation_prefix_protocol_error_propagates(void)
{
    fs_mount_fresh();
    uint32_t length = build_note("link: a.txt\n", NULL, 0u);
    fs_make_file("src.txt", file_content, length);
    with_service();
    collect_reset();
    device_armed = true;
    device_read_fail_at = device_reads + 1u;
    device_read_fail_code = YAN_FS_IO_PROTOCOL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_PROTOCOL,
        yan_relation_links(&context, "src.txt", collect_rows, NULL, &summary),
        "relation_prefix_protocol_propagates");
    uint32_t before = device_reads;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_FAULTED,
        yan_relation_links(&context, "src.txt", collect_rows, NULL, &summary),
        "relation_protocol_sticky_faulted");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(before, device_reads, "relation_protocol_no_retry_reads");
}

/* ------------------------------------------------------ callback contract */

static void relation_sink_stop_reports_stopped_without_summary(void)
{
    fs_mount_fresh();
    uint32_t length = build_note("link: a.txt\nlink: b.txt\n", NULL, 0u);
    fs_make_file("src.txt", file_content, length);
    fs_make_file("a.txt", (const uint8_t *)"a", 1u);
    fs_make_file("b.txt", (const uint8_t *)"b", 1u);
    with_service();
    collect_reset();
    sink_stop_after_one = true;
    summary_mark();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_STOPPED,
        yan_relation_links(&context, "src.txt", collect_rows, NULL, &summary),
        "relation_links_stop_reports_stopped");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, rows_seen_count,
                                     "relation_links_stop_one_row");
    TEST_ASSERT_TRUE_MESSAGE(summary_unpublished(),
                             "relation_links_stop_no_summary_published");
}

static void relation_callback_reentry_is_busy_with_zero_io(void)
{
    fs_mount_fresh();
    uint32_t length = build_note("link: a.txt\n", NULL, 0u);
    fs_make_file("busy.txt", file_content, length);
    fs_make_file("a.txt", (const uint8_t *)"a", 1u);
    with_service();
    collect_reset();
    reenter_armed = true;
    reenter_context = &context;
    reenter_result = YAN_NOTE_OK;
    io_at_reentry = 0u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK,
        yan_relation_links(&context, "busy.txt", collect_rows, NULL, &summary),
        "relation_reentry_outer_ok");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_BUSY, reenter_result,
                                  "relation_reentry_inner_busy");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(io_at_reentry, device_reads,
                                     "relation_reentry_inner_zero_io");
}

/* ------------------------------------------------------------- registry */

static void relation_failed_init_preserves_an_existing_idle_context(void)
{
    fs_mount_fresh();
    with_service();
    static YanRelationContext before_context;
    memcpy(&before_context, &context, sizeof context);
    uint32_t before = device_reads;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_INVALID, test_relation_init(&context, NULL),
                                  "relation_init_failure_invalid");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&before_context, &context, sizeof context,
                                     "relation_init_failure_preserves_bytes");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK, yan_fs_unmount(&fs), "relation_init_failure_unmount");
    /* 0036-R3: the terminal initializes services between init_directory and
     * mount, so init accepts an initialized-but-unmounted filesystem and
     * publishes an empty service. Queries still report NOT_MOUNTED. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_OK, test_relation_init(&context, &fs),
                                  "relation_init_unmounted_accepted");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_NOT_MOUNTED,
                                  yan_relation_links(&context, "n", NULL, NULL, NULL),
                                  "relation_query_unmounted_rejected");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(before, device_reads, "relation_init_failure_zero_reads");
}

static void relation_related_uses_shared_tags_before_ascii_for_equal_edges(void)
{
    fs_mount_fresh();
    uint32_t length = build_note("tag: os\ntag: cpu\nlink: a\nlink: z\n", NULL, 0u);
    fs_make_file("n", file_content, length);
    length = build_note("tag: os\n", NULL, 0u);
    fs_make_file("a", file_content, length);
    length = build_note("tag: cpu\ntag: os\n", NULL, 0u);
    fs_make_file("z", file_content, length);
    with_service();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_OK,
        yan_relation_related(&context, "n", collect_rows, NULL, &summary),
        "relation_secondary_tags_ok");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2u, rows_seen_count, "relation_secondary_tags_count");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("z", rows_seen[0].name, "relation_secondary_more_tags_first");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(2u, rows_seen[0].shared_tags, "relation_secondary_shared_two");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("a", rows_seen[1].name, "relation_secondary_fewer_tags_second");
}

static void relation_alias_guards_preserve_summary_and_do_no_io(void)
{
    fs_mount_fresh();
    with_service();
    summary_mark();
    uint32_t before = device_reads;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_INVALID,
        yan_relation_related(&context, (const char *)&context, collect_rows, NULL, &summary),
        "relation_name_context_alias_invalid");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_INVALID,
        yan_relation_backlinks(&context, (const char *)&fs, collect_rows, NULL, &summary),
        "relation_name_fs_alias_invalid");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_INVALID,
        yan_relation_links(&context, "n", collect_rows, NULL, (YanRelationSummary *)(void *)&context),
        "relation_summary_context_alias_invalid");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_INVALID,
        yan_relation_links(&context, "n", collect_rows, NULL, (YanRelationSummary *)(void *)&fs),
        "relation_summary_fs_alias_invalid");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_INVALID,
        yan_relation_links(&context, "n", collect_rows, &context, &summary),
        "relation_sink_context_alias_invalid");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_INVALID,
        yan_relation_links(&context, "n", collect_rows, &fs, &summary),
        "relation_sink_fs_alias_invalid");
    TEST_ASSERT_TRUE_MESSAGE(summary_unpublished(), "relation_alias_preserves_summary");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(before, device_reads, "relation_alias_zero_reads");
}

static void relation_adjacent_short_name_and_null_summary_are_valid(void)
{
    fs_mount_fresh();
    with_service();
    struct { char name[2]; YanRelationSummary result; } adjacent;
    adjacent.name[0]='n'; adjacent.name[1]='\0';
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_OK,
        yan_relation_backlinks(&context, adjacent.name, collect_rows, NULL, &adjacent.result),
        "relation_short_adjacent_name_accepted");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, adjacent.result.total, "relation_adjacent_empty_total");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_OK,
        yan_relation_backlinks(&context, "n", collect_rows, NULL, NULL),
        "relation_optional_summary_null_accepted");
}

static void relation_late_io_failure_precedes_every_callback_and_releases_busy(void)
{
    fs_mount_fresh();
    uint32_t length = build_note("link: n\n", NULL, 0u);
    fs_make_file("a", file_content, length);
    fs_make_file("z", file_content, length);
    with_service();
    summary_mark();
    device_armed = true;
    device_read_fail_at = device_reads + 2u;
    device_read_fail_code = YAN_FS_IO_ERROR;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_IO,
        yan_relation_backlinks(&context, "n", collect_rows, NULL, &summary),
        "relation_late_io_propagates");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, rows_seen_count, "relation_late_io_no_rows");
    TEST_ASSERT_TRUE_MESSAGE(summary_unpublished(), "relation_late_io_no_summary");
    TEST_ASSERT_FALSE_MESSAGE(context.busy, "relation_late_io_releases_busy");
    uint32_t before = device_reads;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_FAULTED,
        yan_relation_related(&context, "a", collect_rows, NULL, &summary),
        "relation_late_io_faulted_next_call");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(before, device_reads, "relation_late_io_no_more_reads");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(relation_init_records_a_borrowed_fs_without_io);
    RUN_TEST(relation_init_rejects_bad_context_and_fs);
    RUN_TEST(relation_init_rejects_context_fs_overlap);
    RUN_TEST(relation_uninitialized_context_rejects_every_query);
    RUN_TEST(relation_query_rejects_bad_parameters_with_zero_io);
    RUN_TEST(relation_query_rejects_wrapping_context_and_name);
    RUN_TEST(relation_unmounted_state_precedes_parameter_checks);
    RUN_TEST(relation_faulted_state_precedes_parameter_checks);
    RUN_TEST(relation_links_declaration_order_dangling_and_self);
    RUN_TEST(relation_links_ignores_a_broken_target_head);
    RUN_TEST(relation_links_missing_source_is_not_created);
    RUN_TEST(relation_links_broken_source_is_metadata_without_rows);
    RUN_TEST(relation_links_old_headerless_source_has_zero_rows);
    RUN_TEST(relation_links_large_body_stays_in_the_prefix_budget);
    RUN_TEST(relation_backlinks_missing_name_is_zero_results);
    RUN_TEST(relation_backlinks_orders_ascii_and_keeps_self);
    RUN_TEST(relation_backlinks_match_exact_ascii_case);
    RUN_TEST(relation_backlinks_late_bad_header_reserves_everything);
    RUN_TEST(relation_backlinks_covers_a_full_directory);
    RUN_TEST(relation_related_missing_source_outranks_remote_bad_head);
    RUN_TEST(relation_related_bidirectional_outranks_shared_tags);
    RUN_TEST(relation_related_breaks_ties_by_ascii_filename);
    RUN_TEST(relation_related_shared_tags_are_exact_bytes);
    RUN_TEST(relation_related_counts_sixteen_shared_tags);
    RUN_TEST(relation_related_excludes_self_and_dangling);
    RUN_TEST(relation_related_shows_twenty_with_the_full_total);
    RUN_TEST(relation_related_prechecks_a_noncandidate_bad_head);
    RUN_TEST(relation_related_observes_old_and_large_binary_candidates);
    RUN_TEST(relation_query_is_zero_write_and_fresh_after_removal);
    RUN_TEST(relation_prefix_io_error_propagates_and_sticks);
    RUN_TEST(relation_prefix_protocol_error_propagates);
    RUN_TEST(relation_sink_stop_reports_stopped_without_summary);
    RUN_TEST(relation_callback_reentry_is_busy_with_zero_io);
    RUN_TEST(relation_alias_guards_preserve_summary_and_do_no_io);
    RUN_TEST(relation_adjacent_short_name_and_null_summary_are_valid);
    RUN_TEST(relation_late_io_failure_precedes_every_callback_and_releases_busy);
    RUN_TEST(relation_related_uses_shared_tags_before_ascii_for_equal_edges);
    RUN_TEST(relation_failed_init_preserves_an_existing_idle_context);
    return UNITY_END();
}
