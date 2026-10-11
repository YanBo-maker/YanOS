/* 0027 filtered term search: native tests over the unified term facade.
 *
 * The contract is os/search_terms.h, os/search_terms_linear.h,
 * os/search_terms_index.h, os/note_metadata_format.h and
 * docs/specs/0027-note-metadata.md, "标签搜索与既有查询的关系" and
 * "统一term接口与生命周期".
 *
 * Both backends qualify files before ranking. Every case drives a real mounted
 * YanFS on a caller-owned static device through the facade. These tests pin
 * successful behavior and errors; earlier RED results live in build logs.
 *
 * What this file pins, clause by clause:
 *
 *   - untagged files still rank by score with the plain query; a tag filter
 *     drops them (the "eligible after top 20" shape);
 *   - a late valid-text file with a broken header fails the whole filtered
 *     query with METADATA, zero callbacks and an unpublished summary;
 *   - a binary/invalid-UTF-8 file is a preflight skip, not METADATA, even when
 *     its bytes could not be parsed as a header;
 *   - an old header-less file carries empty tags: excluded by a filter, still
 *     visible to the plain query;
 *   - the header is physical content: line numbers count it, so a body match is
 *     reported on its real physical line;
 *   - tag identity is exact: case-folded tags do not match, multiple tags are
 *     ANDed, and a duplicated filter tag counts once;
 *   - scan, index-backed and repeated warm queries agree for the same filter;
 *   - a metadata change is visible to a later query (no stale metadata cache);
 *   - the facade's protected spans, the count bound and address wrapping are
 *     rejected as INVALID before any file is read;
 *   - a NULL filter and an empty filter delegate to the unchanged plain query;
 *   - a callback stop surfaces STOPPED without publishing a summary.
 *
 * Every assertion carries a stable, unique message tag for the later
 * owner-specific mutation gate. The owner-approved FEFF ban is asserted in
 * the pure format suite rather than this filtered-search batch.
 *
 * LIMIT is independently covered by search_metadata_oracle.py; actual source
 * exhaustion is covered by test_search_index_exhaustion.c. Neither fixture
 * writes private filesystem or index state to simulate these conditions. */

#include "note_metadata_format.h"
#include "search_terms.h"
#include "search_terms_index.h"
#include "search_terms_linear.h"
#include "yanfs.h"
#include "unity.h"

#include <string.h>

/* ------------------------------------------------------------- fixtures */

#define FILTER_BLOCKS 2048u /* 8 MiB, the device size the index suite uses */
#define CAPTURE_MAX 24u

static uint8_t medium[FILTER_BLOCKS][YAN_FS_BLOCK_SIZE];
static YanFs fs;
static YanSearchTermsLinear linear;
static YanSearchTermsIndex index_terms;
static YanSearchTerms facade;
static YanSearchTerms index_facade;

static const uint8_t tag_os[] = "os";
static const uint8_t tag_riscv[] = "riscv";
static const uint8_t keyword[] = "alpha";

static YanNoteTagFilter filter;
static YanNoteTagFilter filter_over;
static uint8_t file_content[8192];

static char capture_names[CAPTURE_MAX][32];
static uint32_t capture_lines[CAPTURE_MAX];
static uint16_t capture_scores[CAPTURE_MAX];
static uint32_t capture_count;
static bool capture_stop;

static YanSearchTermsSummary summary;
static uint32_t device_reads;

/* ------------------------------------------------------------ byte device */

static YanFsIoResult device_capacity(void *context_pointer, uint64_t *blocks)
{
    (void)context_pointer;
    *blocks = FILTER_BLOCKS;
    return YAN_FS_IO_OK;
}

static YanFsIoResult device_read(void *context_pointer, uint32_t lba,
                                 uint8_t out[4096])
{
    (void)context_pointer;
    ++device_reads;
    if (lba >= FILTER_BLOCKS) {
        return YAN_FS_IO_ERROR;
    }
    memcpy(out, medium[lba], YAN_FS_BLOCK_SIZE);
    return YAN_FS_IO_OK;
}

static YanFsIoResult device_write(void *context_pointer, uint32_t lba,
                                  const uint8_t data[4096])
{
    (void)context_pointer;
    if (lba >= FILTER_BLOCKS) {
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

static void device_mount(void)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_FS_OK, yan_fs_format_metadata(medium[0], FILTER_BLOCKS),
        "filter_fixture_format");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK, yan_fs_init(&fs, fs_backend()),
                                  "filter_fixture_init");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK, yan_fs_mount(&fs),
                                  "filter_fixture_mount");
}

static void fs_make_file(const char *name, const uint8_t *bytes,
                         uint32_t length)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK,
                                  yan_fs_create(&fs, name, bytes, length),
                                  "filter_fixture_create");
}

static void fs_replace_file(const char *name, const uint8_t *bytes,
                            uint32_t length)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK,
                                  yan_fs_replace(&fs, name, bytes, length),
                                  "filter_fixture_replace");
}

static void facade_init(void)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_TERMS_OK, yan_search_terms_linear_init(&linear, &fs),
        "filter_fixture_linear_init");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_TERMS_OK,
        yan_search_terms_init(&facade,
                              yan_search_terms_linear_backend(&linear)),
        "filter_fixture_facade_init");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_TERMS_OK, yan_search_terms_index_init(&index_terms, &fs),
        "filter_fixture_index_init");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_TERMS_OK,
        yan_search_terms_init(&index_facade,
                              yan_search_terms_index_backend(&index_terms)),
        "filter_fixture_index_facade_init");
}

/* Marker + the caller's field lines + close. The caller spells the exact field
 * bytes it wants; nothing is normalised or re-encoded. */
static uint32_t build_note(const char *lines)
{
    const char *marker = "@yan-note 1\n";
    uint32_t length = (uint32_t)strlen(marker);
    memcpy(file_content, marker, length);
    if (lines != NULL && lines[0] != '\0') {
        uint32_t lines_length = (uint32_t)strlen(lines);
        memcpy(file_content + length, lines, lines_length);
        length += lines_length;
    }
    memcpy(file_content + length, "@end\n", 5u);
    length += 5u;
    return length;
}

static uint32_t build_note_with_body(const char *lines, const char *body)
{
    uint32_t length = build_note(lines);
    uint32_t body_length = (uint32_t)strlen(body);
    memcpy(file_content + length, body, body_length);
    return length + body_length;
}

static void set_filter_one(const uint8_t *tag, uint16_t length)
{
    memset(&filter, 0, sizeof filter);
    filter.count = 1u;
    filter.length[0] = length;
    memcpy(filter.tag[0], tag, length);
}

static void set_filter_two(const uint8_t *first, uint16_t first_length,
                           const uint8_t *second, uint16_t second_length)
{
    memset(&filter, 0, sizeof filter);
    filter.count = 2u;
    filter.length[0] = first_length;
    memcpy(filter.tag[0], first, first_length);
    filter.length[1] = second_length;
    memcpy(filter.tag[1], second, second_length);
}

static void capture_reset(void)
{
    memset(capture_names, 0, sizeof capture_names);
    memset(capture_lines, 0, sizeof capture_lines);
    memset(capture_scores, 0, sizeof capture_scores);
    capture_count = 0u;
    capture_stop = false;
}

static bool capture_match(void *context, const YanSearchTermsMatch *match)
{
    (void)context;
    if (capture_stop) {
        return false;
    }
    if (capture_count < CAPTURE_MAX) {
        for (uint32_t i = 0u; i < 31u; ++i) {
            capture_names[capture_count][i] = match->name[i];
            if (match->name[i] == '\0') {
                break;
            }
        }
        capture_names[capture_count][31] = '\0';
        capture_lines[capture_count] = match->line_number;
        capture_scores[capture_count] = match->score;
        capture_count += 1u;
    }
    return true;
}

static YanSearchTermsSink capture_sink(void)
{
    YanSearchTermsSink sink;
    sink.context = NULL;
    sink.match = capture_match;
    sink.chunk = NULL;
    return sink;
}

static void summary_reset(void)
{
    summary.total = 0xdeadbeefu;
    summary.shown = 0xdeadbeefu;
    summary.skipped = 0xdeadbeefu;
    summary.mode = YAN_SEARCH_MODE_SCAN;
}

static uint32_t capture_count_name(const char *name)
{
    uint32_t hits = 0u;
    for (uint32_t i = 0u; i < capture_count; ++i) {
        if (strcmp(capture_names[i], name) == 0) {
            hits += 1u;
        }
    }
    return hits;
}

static YanSearchTermsResult run_filtered(YanSearchTerms *terms,
                                         const YanNoteTagFilter *which)
{
    capture_reset();
    summary_reset();
    return yan_search_terms_filtered(terms, keyword,
                                     (uint32_t)(sizeof keyword - 1u), which,
                                     capture_sink(), &summary);
}

void setUp(void)
{
    memset(medium, 0, sizeof medium);
    memset(&fs, 0, sizeof fs);
    memset(&linear, 0, sizeof linear);
    memset(&index_terms, 0, sizeof index_terms);
    memset(&facade, 0, sizeof facade);
    memset(&index_facade, 0, sizeof index_facade);
    memset(&filter, 0, sizeof filter);
    memset(&filter_over, 0, sizeof filter_over);
    memset(file_content, 0, sizeof file_content);
    capture_reset();
    summary_reset();
}

void tearDown(void)
{
}

/* --------------------------------------------------------- ranking rules */

static void filter_untagged_files_are_excluded_after_ranking(void)
{
    device_mount();
    uint32_t length = build_note_with_body(
        "title: untagged\n",
        "alpha\nalpha\nalpha\nalpha\nalpha\n"
        "alpha\nalpha\nalpha\nalpha\nalpha\n");
    fs_make_file("untagged.txt", file_content, length);
    facade_init();
    set_filter_one(tag_os, (uint16_t)(sizeof tag_os - 1u));
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_TERMS_OK, run_filtered(&facade, &filter),
        "untagged_filtered_status");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, capture_count,
                                     "untagged_filtered_no_matches");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(0u, summary.total,
                                     "untagged_filtered_zero_total");
}

static void filter_ranking_uses_score_within_the_eligible_set(void)
{
    device_mount();
    uint32_t length = build_note_with_body("tag: os\n",
                                           "alpha\nalpha alpha\nalpha\n");
    fs_make_file("rank.txt", file_content, length);
    length = build_note_with_body("tag: os\n", "alpha\n");
    fs_make_file("second.txt", file_content, length);
    facade_init();
    set_filter_one(tag_os, (uint16_t)(sizeof tag_os - 1u));
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_TERMS_OK, run_filtered(&facade, &filter),
        "ranking_filtered_status");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(4u, capture_count,
                                     "ranking_filtered_four_rows");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("rank.txt", capture_names[0],
                                     "ranking_filtered_best_file");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(5u, capture_lines[0],
                                     "ranking_filtered_best_line");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(2u, capture_scores[0],
                                     "ranking_filtered_best_score");
}

static void filter_late_valid_text_bad_header_is_metadata(void)
{
    device_mount();
    uint32_t length = build_note_with_body("title: good\ntag: os\n",
                                           "alpha\n");
    fs_make_file("a_good.txt", file_content, length);
    length = build_note_with_body("tag: os\nkind: note\n", "alpha\n");
    fs_make_file("z_bad.txt", file_content, length);
    facade_init();
    set_filter_one(tag_os, (uint16_t)(sizeof tag_os - 1u));
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_TERMS_METADATA, run_filtered(&facade, &filter),
        "late_bad_head_status_metadata");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, capture_count,
                                     "late_bad_head_no_callbacks");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(0xdeadbeefu, summary.total,
                                     "late_bad_head_summary_untouched");
}

static void filter_binary_file_is_a_preflight_skip_not_metadata(void)
{
    device_mount();
    static uint8_t binary[64];
    for (uint32_t i = 0u; i < sizeof binary; ++i) {
        binary[i] = (uint8_t)(0x80u + (i & 0x3Fu));
    }
    fs_make_file("binary.dat", binary, (uint32_t)sizeof binary);
    facade_init();
    set_filter_one(tag_os, (uint16_t)(sizeof tag_os - 1u));
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_TERMS_OK, run_filtered(&facade, &filter),
        "binary_filtered_status");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, summary.skipped,
                                     "binary_filtered_skipped_once");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, capture_count,
                                     "binary_filtered_no_callbacks");
}

static void filter_old_headerless_file_is_tag_empty(void)
{
    device_mount();
    static const char old_text[] = "alpha\nplain old note\n";
    fs_make_file("old.txt", (const uint8_t *)old_text,
                 (uint32_t)(sizeof old_text - 1u));
    facade_init();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_TERMS_OK, run_filtered(&facade, NULL),
        "old_plain_status");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, capture_count,
                                     "old_plain_finds_the_body");
    set_filter_one(tag_os, (uint16_t)(sizeof tag_os - 1u));
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_TERMS_OK, run_filtered(&facade, &filter),
        "old_filtered_status");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, capture_count,
                                     "old_filtered_excluded");
}

static void filter_body_match_reports_the_physical_line(void)
{
    device_mount();
    uint32_t length = build_note_with_body("title: physical\ntag: os\n",
                                           "alpha\n");
    fs_make_file("physical.txt", file_content, length);
    facade_init();
    set_filter_one(tag_os, (uint16_t)(sizeof tag_os - 1u));
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_TERMS_OK, run_filtered(&facade, &filter),
        "physical_line_status");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, capture_count,
                                     "physical_line_one_row");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(5u, capture_lines[0],
                                     "physical_line_is_five");
}

static void filter_is_exact_and_anded_with_duplicates_counted_once(void)
{
    device_mount();
    uint32_t length = build_note_with_body("tag: os\ntag: riscv\n", "alpha\n");
    fs_make_file("both.txt", file_content, length);
    length = build_note_with_body("tag: os\n", "alpha\n");
    fs_make_file("only_os.txt", file_content, length);
    length = build_note_with_body("tag: OS\n", "alpha\n");
    fs_make_file("upper.txt", file_content, length);
    facade_init();

    set_filter_two(tag_os, (uint16_t)(sizeof tag_os - 1u), tag_riscv,
                   (uint16_t)(sizeof tag_riscv - 1u));
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_TERMS_OK, run_filtered(&facade, &filter),
        "and_filter_status");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, capture_count, "and_filter_one_row");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("both.txt", capture_names[0],
                                     "and_filter_matches_both_tags");

    /* Case is identity: the OS-tagged file is not the os-tagged one. */
    set_filter_one(tag_os, (uint16_t)(sizeof tag_os - 1u));
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_TERMS_OK, run_filtered(&facade, &filter),
        "case_filter_status");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2u, capture_count,
                                     "case_filter_exact_two_rows");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, capture_count_name("upper.txt"),
                                     "case_filter_upper_not_folded");

    /* A duplicated tag is the same one tag, not two. */
    set_filter_two(tag_os, (uint16_t)(sizeof tag_os - 1u), tag_os,
                   (uint16_t)(sizeof tag_os - 1u));
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_TERMS_OK, run_filtered(&facade, &filter),
        "duplicate_filter_status");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2u, capture_count,
                                     "duplicate_filter_deduplicated");
}

static void filter_scan_index_and_warm_repeat_agree(void)
{
    device_mount();
    uint32_t length = build_note_with_body("tag: os\n", "alpha\n");
    fs_make_file("one.txt", file_content, length);
    facade_init();
    set_filter_one(tag_os, (uint16_t)(sizeof tag_os - 1u));

    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_TERMS_OK, run_filtered(&facade, &filter),
        "consistency_scan_status");
    uint32_t first_count = capture_count;
    uint64_t first_total = summary.total;
    YanSearchTermsMode first_mode = summary.mode;

    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_TERMS_OK, run_filtered(&facade, &filter),
        "consistency_repeat_status");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(first_count, capture_count,
                                     "consistency_repeat_rows");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(first_total, summary.total,
                                     "consistency_repeat_total");
    TEST_ASSERT_EQUAL_INT_MESSAGE(first_mode, summary.mode,
                                  "consistency_repeat_mode");

    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_TERMS_OK, run_filtered(&index_facade, &filter),
        "consistency_index_status");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(first_count, capture_count,
                                     "consistency_index_rows");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(first_total, summary.total,
                                     "consistency_index_total");
}

static void filter_metadata_change_is_visible_to_the_next_query(void)
{
    device_mount();
    uint32_t length = build_note_with_body("tag: os\n", "alpha\n");
    fs_make_file("mut.txt", file_content, length);
    facade_init();
    set_filter_one(tag_os, (uint16_t)(sizeof tag_os - 1u));
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_TERMS_OK, run_filtered(&facade, &filter),
        "change_before_status");
    uint32_t before = capture_count;

    /* Rewrite the file with a different tag; the later query must see it. */
    length = build_note_with_body("tag: riscv\n", "alpha\n");
    fs_replace_file("mut.txt", file_content, length);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_TERMS_OK, run_filtered(&facade, &filter),
        "change_after_status");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, before, "change_before_one_row");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, capture_count,
                                     "change_after_no_stale_match");
}

/* ---------------------------------------------------- facade protections */

static void filter_arguments_are_guarded_before_any_read(void)
{
    device_mount();
    uint32_t length = build_note_with_body("tag: os\n", "alpha\n");
    fs_make_file("guard.txt", file_content, length);
    facade_init();

    /* A filter whose span sits inside the facade itself. The alias is rejected
     * by address arithmetic before any filter field is read, so the bytes at
     * that address (the facade's own zeroed first field) are never used as a
     * count. */
    YanNoteTagFilter *alias = (YanNoteTagFilter *)(void *)&facade;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_TERMS_INVALID,
        yan_search_terms_filtered(&facade, keyword,
                                  (uint32_t)(sizeof keyword - 1u), alias,
                                  capture_sink(), &summary),
        "guard_alias_filter_invalid");

    YanNoteTagFilter *wrapping = (YanNoteTagFilter *)(uintptr_t)UINTPTR_MAX;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_TERMS_INVALID,
        yan_search_terms_filtered(&facade, keyword,
                                  (uint32_t)(sizeof keyword - 1u), wrapping,
                                  capture_sink(), &summary),
        "guard_wrapping_filter_invalid");

    filter_over.count = YAN_NOTE_TAG_MAX_COUNT + 1u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_TERMS_INVALID,
        yan_search_terms_filtered(&facade, keyword,
                                  (uint32_t)(sizeof keyword - 1u), &filter_over,
                                  capture_sink(), &summary),
        "guard_over_count_invalid");

    YanSearchTermsBackend direct = yan_search_terms_index_backend(&index_terms);
    void *wrapping_index = (void *)(uintptr_t)
        (UINTPTR_MAX - sizeof(YanSearchTermsIndex) + 1u);
    uint32_t before_reads = device_reads;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_TERMS_INVALID,
        direct.query_filtered(wrapping_index, keyword, sizeof keyword - 1u,
                              &filter, capture_match, NULL, &summary),
        "guard_direct_index_extent_before_bool");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(before_reads, device_reads,
        "guard_direct_index_extent_zero_reads");
}

static void filter_null_and_empty_delegate_to_the_plain_query(void)
{
    device_mount();
    uint32_t length = build_note_with_body("tag: os\n", "alpha\n");
    fs_make_file("plain.txt", file_content, length);
    facade_init();

    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_TERMS_OK, run_filtered(&facade, NULL),
        "null_filter_status");
    uint32_t null_count = capture_count;
    uint64_t null_total = summary.total;

    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_TERMS_OK, run_filtered(&facade, &filter),
        "empty_filter_status");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(null_count, capture_count,
                                     "empty_filter_matches_null");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(null_total, summary.total,
                                     "empty_filter_total_matches_null");

    capture_reset();
    summary_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_TERMS_OK,
        yan_search_terms(&facade, keyword, (uint32_t)(sizeof keyword - 1u),
                         capture_sink(), &summary),
        "plain_query_status");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(null_count, capture_count,
                                     "plain_query_matches_filtered_null");
}

static void filter_error_publishes_no_summary_and_surfaces_stopped(void)
{
    device_mount();
    uint32_t length = build_note_with_body("tag: os\n", "alpha\n");
    fs_make_file("stop.txt", file_content, length);
    facade_init();
    set_filter_one(tag_os, (uint16_t)(sizeof tag_os - 1u));

    capture_reset();
    summary_reset();
    capture_stop = true;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_TERMS_STOPPED,
        yan_search_terms_filtered(&facade, keyword,
                                  (uint32_t)(sizeof keyword - 1u), &filter,
                                  capture_sink(), &summary),
        "stop_filter_status_stopped");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(0xdeadbeefu, summary.total,
                                     "stop_filter_no_summary");
}

/* ------------------------------------------------------------- registry */

static void filter_eligible_beyond_plain_top20_is_found(void)
{
    device_mount();
    static uint8_t high[30u * 12u];
    for (uint32_t i = 0u; i < 30u; ++i) memcpy(high + i * 12u, "alpha alpha\n", 12u);
    fs_make_file("high.txt", high, sizeof high);
    uint32_t length = build_note_with_body("tag: os\n", "alpha\n");
    fs_make_file("eligible.txt", file_content, length);
    facade_init();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_TERMS_OK,
        run_filtered(&index_facade, NULL), "beyond20_plain_status");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(20u, capture_count, "beyond20_plain_shown20");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, capture_count_name("eligible.txt"), "beyond20_plain_excludes_eligible");
    set_filter_one(tag_os, sizeof tag_os - 1u);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_TERMS_OK,
        run_filtered(&index_facade, &filter), "beyond20_index_filter_status");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(1u, summary.total, "beyond20_index_total_one");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, capture_count_name("eligible.txt"), "beyond20_index_found_eligible");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_TERMS_OK,
        run_filtered(&facade, &filter), "beyond20_scan_filter_status");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(1u, summary.total, "beyond20_scan_total_one");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, capture_count_name("eligible.txt"), "beyond20_scan_found_eligible");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(filter_eligible_beyond_plain_top20_is_found);
    RUN_TEST(filter_untagged_files_are_excluded_after_ranking);
    RUN_TEST(filter_ranking_uses_score_within_the_eligible_set);
    RUN_TEST(filter_late_valid_text_bad_header_is_metadata);
    RUN_TEST(filter_binary_file_is_a_preflight_skip_not_metadata);
    RUN_TEST(filter_old_headerless_file_is_tag_empty);
    RUN_TEST(filter_body_match_reports_the_physical_line);
    RUN_TEST(filter_is_exact_and_anded_with_duplicates_counted_once);
    RUN_TEST(filter_scan_index_and_warm_repeat_agree);
    RUN_TEST(filter_metadata_change_is_visible_to_the_next_query);
    RUN_TEST(filter_arguments_are_guarded_before_any_read);
    RUN_TEST(filter_null_and_empty_delegate_to_the_plain_query);
    RUN_TEST(filter_error_publishes_no_summary_and_surfaces_stopped);
    return UNITY_END();
}
