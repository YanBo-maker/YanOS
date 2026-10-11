/* 0027 shell metadata binding: native tests, written before the implementation.
 *
 * The contract is os/shell.h, os/note_metadata.h, os/note_metadata_format.h and
 * docs/specs/0027-note-metadata.md, "命令及可观察输出". The optional binding
 * is implemented; these tests pin its behavior and the unchanged unbound path.
 * Historical RED evidence is retained separately in build logs.
 *
 * The fixture is platform independent: a real mounted YanFS on a caller-owned
 * static 8 MiB byte device, a real note-metadata context, the real literal and
 * term facades, and a static shell output buffer. No fake filesystem and no
 * fake service are used, so a passing assertion means the real stack behaved.
 *
 * What this file pins, clause by clause:
 *
 *   - `meta show` prints the fixed field order, UNKNOWN for absent dates,
 *     `TAG NONE`/`LINK NONE` for empty lists, LINK OK/DANGLING from a target
 *     lookup, and never rejects a binary body;
 *   - `meta set/tag/link` rewrite the canonical file with the body bytes
 *     preserved exactly (CRLF included) and print one `OK meta ...` receipt per
 *     command; the title keeps everything after the first separator space and
 *     `created -` clears the field;
 *   - a broken header makes show and list report `ERROR METADATA` with no field
 *     lines, no partial list and no OK;
 *   - `meta list` sorts names in ASCII order and ANDs the tags, with case
 *     distinct;
 *   - grammar/value failures print ERROR USAGE with zero filesystem I/O;
 *   - a bound shell supports the tag operator in a bare query, keeps the whole
 *     outer-quoted form as the old 0026 grammar, rejects a keyword-inner quote,
 *     and enforces 16 distinct tags after de-duplication;
 *   - an output failure stops the command with no retry and no OK; a metadata
 *     change that already published is not rolled back by a later output
 *     failure;
 *   - the binding rejects span, alias and filesystem mismatches; a metadata
 *     command line that aliases the context is rejected before a byte is
 *     emitted;
 *   - an unbound shell keeps the old help, plain search and meta behaviour.
 *
 * Every assertion carries a stable, unique message tag for the later
 * owner-specific mutation gate. All fixtures are static; no test builds a large
 * automatic object and no variable-length array is used. */

#include "note_metadata.h"
#include "note_metadata_format.h"
#include "search.h"
#include "search_linear.h"
#include "search_terms.h"
#include "search_terms_linear.h"
#include "shell.h"
#include "yanfs.h"
#include "unity.h"
#include <string.h>

#include "memory.h"

/* ------------------------------------------------------------- fixtures */

#define SHELL_BLOCKS 2048u /* 8 MiB */
#define OUTPUT_MAX 8192u

static uint8_t medium[SHELL_BLOCKS][YAN_FS_BLOCK_SIZE];
static uint32_t device_reads;
static uint32_t device_writes;
static YanFs fs;
static YanNoteContext note;
static YanSearchLinear linear;
static YanSearch search;
static YanSearchTermsLinear terms_linear;
static YanSearchTerms terms;
static YanShell shell;

static uint8_t output_buffer[OUTPUT_MAX];
static uint32_t output_length;
static uint32_t output_fail_at; /* 0 = never */

static uint8_t file_content[8192];
static uint8_t readback[8192];
static uint8_t line_buffer[1024];
static uint8_t expected_buffer[4096];

/* ------------------------------------------------------------ byte device */

static YanFsIoResult device_capacity(void *context_pointer, uint64_t *blocks)
{
    (void)context_pointer;
    *blocks = SHELL_BLOCKS;
    return YAN_FS_IO_OK;
}

static YanFsIoResult device_read(void *context_pointer, uint32_t lba,
                                 uint8_t out[4096])
{
    (void)context_pointer;
    ++device_reads;
    if (lba >= SHELL_BLOCKS) {
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
    if (lba >= SHELL_BLOCKS) {
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

/* ------------------------------------------------------------- output sink */

static bool sink_putc(void *context, uint8_t byte)
{
    (void)context;
    output_length += 1u;
    if (output_fail_at != 0u && output_length == output_fail_at) {
        return false;
    }
    if (output_length <= OUTPUT_MAX) {
        output_buffer[output_length - 1u] = byte;
    }
    return true;
}

static YanShellOutput shell_output(void)
{
    YanShellOutput output;
    output.context = NULL;
    output.putc = sink_putc;
    return output;
}

static void output_reset(void)
{
    memset(output_buffer, 0, sizeof output_buffer);
    output_length = 0u;
    output_fail_at = 0u;
}

/* Exact-output helper: the expected literal is spelled by the caller, so a
 * wrong byte, a wrong order or a missing receipt is caught. */
static void assert_output(const char *expected)
{
    uint32_t expected_length = (uint32_t)strlen(expected);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(expected_length, output_length,
                                     "output_length_exact");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(expected, output_buffer, expected_length,
                                     "output_bytes_exact");
}

/* --------------------------------------------------------------- helpers */

static void shell_fixture(void)
{
    memset(&fs, 0, sizeof fs);
    memset(&note, 0, sizeof note);
    memset(&linear, 0, sizeof linear);
    memset(&search, 0, sizeof search);
    memset(&terms_linear, 0, sizeof terms_linear);
    memset(&terms, 0, sizeof terms);
    memset(&shell, 0, sizeof shell);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_FS_OK, yan_fs_format_metadata(medium[0], SHELL_BLOCKS),
        "fixture_format");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK, yan_fs_init(&fs, fs_backend()),
                                  "fixture_fs_init");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK, yan_fs_mount(&fs),
                                  "fixture_mount");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_OK,
                                  yan_search_linear_init(&linear, &fs),
                                  "fixture_literal_init");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK,
        yan_search_init(&search, yan_search_linear_backend(&linear)),
        "fixture_literal_facade");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_TERMS_OK, yan_search_terms_linear_init(&terms_linear, &fs),
        "fixture_term_init");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_TERMS_OK,
        yan_search_terms_init(&terms,
                              yan_search_terms_linear_backend(&terms_linear)),
        "fixture_term_facade");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_OK, yan_note_init(&note, &fs),
                                  "fixture_note_init");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SHELL_OK,
        yan_shell_init(&shell, &fs, &search, &terms, shell_output()),
        "fixture_shell_init");
    output_reset();
}

/* Binds the metadata context. The stub returns INVALID, so this assertion is
 * the first RED failure of every metadata case. */
static void shell_bind(void)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SHELL_OK, yan_shell_bind_metadata(&shell, &note),
        "fixture_bind_metadata_ok");
}

static YanShellResult run(const char *text)
{
    uint32_t length = (uint32_t)strlen(text);
    TEST_ASSERT_TRUE_MESSAGE(length < sizeof line_buffer, "run_line_fits");
    memcpy(line_buffer, text, length);
    return yan_shell_execute(&shell, line_buffer, length);
}

static void fs_make_file(const char *name, const uint8_t *bytes,
                         uint32_t length)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK,
                                  yan_fs_create(&fs, name, bytes, length),
                                  "fixture_create");
}

/* Marker + the caller's field lines + close. The caller spells the field bytes
 * exactly; nothing is normalised. */
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

static uint32_t build_note_with_body(const char *lines, const uint8_t *body,
                                     uint32_t body_length)
{
    uint32_t length = build_note(lines);
    if (body != NULL && body_length != 0u) {
        memcpy(file_content + length, body, body_length);
        length += body_length;
    }
    return length;
}

static uint32_t read_file(const char *name)
{
    uint32_t got = 0u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_FS_OK,
        yan_fs_read(&fs, name, 0u, readback, (uint32_t)sizeof readback, &got),
        "fixture_read");
    return got;
}

void setUp(void)
{
    memset(medium, 0, sizeof medium);
    device_reads = 0u;
    device_writes = 0u;
    memset(file_content, 0, sizeof file_content);
    memset(readback, 0, sizeof readback);
    memset(line_buffer, 0, sizeof line_buffer);
    memset(expected_buffer, 0, sizeof expected_buffer);
    output_reset();
}

void tearDown(void)
{
}

/* ---------------------------------------------------------- meta show */

static void shell_meta_show_unknown_and_none_for_binary(void)
{
    shell_fixture();
    uint8_t binary[32];
    for (uint32_t i = 0u; i < sizeof binary; ++i) {
        binary[i] = (uint8_t)(0x80u + (i & 0x3fu));
    }
    fs_make_file("bin.dat", binary, (uint32_t)sizeof binary);
    shell_bind();
    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("meta show bin.dat"),
                                  "show_unknown_status");
    assert_output("META bin.dat\r\n"
                  "TITLE UNKNOWN\r\n"
                  "CREATED UNKNOWN\r\n"
                  "MODIFIED UNKNOWN\r\n"
                  "TAG NONE\r\n"
                  "LINK NONE\r\n"
                  "OK meta show\r\n");
}

static void shell_meta_show_all_fields_and_dangling_link(void)
{
    shell_fixture();
    uint8_t body[64];
    for (uint32_t i = 0u; i < sizeof body; ++i) {
        body[i] = (uint8_t)(0x80u + (i & 0x3fu));
    }
    uint32_t length = build_note_with_body(
        "title: note\ncreated: 2026-10-07\ntag: os\n"
        "link: irq.txt\nlink: missing.txt\n",
        body, (uint32_t)sizeof body);
    fs_make_file("note.txt", file_content, length);
    fs_make_file("irq.txt", body, 4u);
    shell_bind();
    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("meta show note.txt"),
                                  "show_fields_status");
    assert_output("META note.txt\r\n"
                  "TITLE note\r\n"
                  "CREATED 2026-10-07\r\n"
                  "MODIFIED UNKNOWN\r\n"
                  "TAG os\r\n"
                  "LINK irq.txt OK\r\n"
                  "LINK missing.txt DANGLING\r\n"
                  "OK meta show\r\n");
}

/* ------------------------------------------------------- meta updates */

static void shell_meta_update_receipts_rewrite_canonical_file(void)
{
    static const uint8_t body[] = "line1\r\nline2";
    static const char expected_header[] =
        "@yan-note 1\n"
        "title:  new title \n"
        "tag: os\n"
        "link: irq.txt\n"
        "@end\n";
    shell_fixture();
    uint32_t body_length = (uint32_t)(sizeof body - 1u);
    uint32_t length = build_note_with_body("title: old\ncreated: 2026-10-08\n"
                                           "tag: keep\n",
                                           body, body_length);
    fs_make_file("note.txt", file_content, length);
    shell_bind();

    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK,
                                  run("meta set note.txt title  new title "),
                                  "set_title_status");
    assert_output("OK meta set\r\n");
    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SHELL_OK, run("meta set note.txt created 2026-10-07"),
        "set_created_status");
    assert_output("OK meta set\r\n");
    /* `-` clears the date, so the canonical header drops the created line. */
    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK,
                                  run("meta set note.txt created -"),
                                  "clear_created_status");
    assert_output("OK meta set\r\n");
    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK,
                                  run("meta tag note.txt add os"),
                                  "tag_add_status");
    assert_output("OK meta tag\r\n");
    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK,
                                  run("meta tag note.txt del keep"),
                                  "tag_del_status");
    assert_output("OK meta tag\r\n");
    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK,
                                  run("meta link note.txt add irq.txt"),
                                  "link_add_status");
    assert_output("OK meta link\r\n");

    uint32_t header_length = (uint32_t)(sizeof expected_header - 1u);
    uint32_t got = read_file("note.txt");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(header_length + body_length, got,
                                     "update_total_length");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(expected_header, readback, header_length,
                                     "update_canonical_header");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(body, readback + header_length, body_length,
                                     "update_body_bytes_preserved");
}

/* ------------------------------------------------------- bad headers */

static void shell_meta_bad_header_metadata_error_for_show_and_list(void)
{
    shell_fixture();
    uint32_t length = build_note_with_body("title: ok\n", (const uint8_t *)"x",
                                           1u);
    fs_make_file("a.txt", file_content, length);
    length = build_note_with_body("kind: note\n", (const uint8_t *)"x", 1u);
    fs_make_file("z.txt", file_content, length);
    shell_bind();

    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("meta show z.txt"),
                                  "bad_show_status");
    assert_output("ERROR METADATA\r\n");

    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("meta list"),
                                  "bad_list_status");
    assert_output("ERROR METADATA\r\n");
}

/* ------------------------------------------------------------- list */

static void shell_meta_list_sorts_and_ands_with_case_distinct(void)
{
    shell_fixture();
    uint32_t length = build_note("tag: os\ntag: riscv\n");
    fs_make_file("beta.txt", file_content, length);
    length = build_note("tag: os\n");
    fs_make_file("alpha.txt", file_content, length);
    length = build_note("tag: OS\n");
    fs_make_file("upper.txt", file_content, length);
    shell_bind();

    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("meta list"),
                                  "list_status");
    assert_output("alpha.txt\r\n"
                  "beta.txt\r\n"
                  "upper.txt\r\n"
                  "OK meta list shown=3\r\n");

    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("meta list tag:os"),
                                  "list_one_tag_status");
    assert_output("alpha.txt\r\n"
                  "beta.txt\r\n"
                  "OK meta list shown=2\r\n");

    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK,
                                  run("meta list tag:os tag:riscv"),
                                  "list_and_status");
    assert_output("beta.txt\r\n"
                  "OK meta list shown=1\r\n");

    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK,
                                  run("meta list tag:nonexistent"),
                                  "list_empty_status");
    assert_output("OK meta list shown=0\r\n");
}

/* ----------------------------------------------------------- usage */

static void shell_meta_usage_errors_do_no_io(void)
{
    shell_fixture();
    uint32_t length = build_note("title: note\n");
    fs_make_file("note.txt", file_content, length);
    shell_bind();

    uint32_t reads = device_reads;
    uint32_t writes = device_writes;
    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("meta show"),
                                  "usage_missing_name_status");
    assert_output("ERROR USAGE\r\n");

    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK,
                                  run("meta set note.txt created yesterday"),
                                  "usage_bad_date_status");
    assert_output("ERROR USAGE\r\n");

    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK,
                                  run("meta tag note.txt add"),
                                  "usage_missing_tag_status");
    assert_output("ERROR USAGE\r\n");

    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK,
                                  run("meta set note.txt created 2026-10-07 extra"),
                                  "usage_extra_value_status");
    assert_output("ERROR USAGE\r\n");

    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("meta link note.txt add ."),
                                  "usage_bad_link_status");
    assert_output("ERROR USAGE\r\n");

    TEST_ASSERT_EQUAL_UINT32_MESSAGE(reads, device_reads, "usage_no_reads");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(writes, device_writes, "usage_no_writes");
}

/* -------------------------------------------------------- tag search */

static void shell_search_tag_operator_and_plain_untagged(void)
{
    shell_fixture();
    static uint8_t high[30u * 12u];
    for (uint32_t i = 0u; i < 30u; ++i) {
        memcpy(high + i * 12u, "alpha alpha\n", 12u);
    }
    fs_make_file("high.txt", high, (uint32_t)sizeof high);
    uint32_t length = build_note_with_body("tag: os\n",
                                           (const uint8_t *)"alpha\n", 6u);
    fs_make_file("eligible.txt", file_content, length);
    shell_bind();

    /* The header is physical content, so the eligible note's body match is
     * line 4. */
    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("search tag:os alpha"),
                                  "tag_search_status");
    assert_output("eligible.txt:4:alpha\r\n"
                  "OK search total=1 shown=1 skipped=0 mode=scan\r\n");

    /* Without the tag, the old top-20 ranking keeps the late eligible file out
     * of the shown rows. */
    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("search alpha"),
                                  "plain_search_status");
    TEST_ASSERT_TRUE_MESSAGE(
        strstr((const char *)output_buffer, "eligible.txt") == NULL,
        "plain_search_top20_excludes_late_eligible");
    TEST_ASSERT_TRUE_MESSAGE(
        strstr((const char *)output_buffer, "OK search") != NULL,
        "plain_search_receipt");
}

static void shell_metadata_change_invalidates_search_eligibility(void)
{
    shell_fixture();
    uint32_t length = build_note_with_body("tag: os\n",
                                           (const uint8_t *)"alpha\n", 6u);
    fs_make_file("mut.txt", file_content, length);
    shell_bind();
    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("search tag:os alpha"),
                                  "change_before_status");
    assert_output("mut.txt:4:alpha\r\n"
                  "OK search total=1 shown=1 skipped=0 mode=scan\r\n");

    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("meta tag mut.txt del os"),
                                  "change_delete_status");
    assert_output("OK meta tag\r\n");
    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("search tag:os alpha"),
                                  "change_after_status");
    assert_output("OK search total=0 shown=0 skipped=0 mode=scan\r\n");
}

static void shell_tag_and_quote_grammar_rules(void)
{
    shell_fixture();
    uint32_t length = build_note_with_body("tag: os\n",
                                           (const uint8_t *)"alpha\n", 6u);
    fs_make_file("note.txt", file_content, length);
    shell_bind();

    /* tag-only is a usage error: `meta list` is the file-level tag query. */
    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("search tag:os"),
                                  "tag_only_status");
    assert_output("ERROR USAGE\r\n");

    /* The whole outer-quoted form keeps the old 0026 keyword grammar. */
    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("search \"alpha\""),
                                  "quoted_status");
    assert_output("note.txt:4:alpha\r\n"
                  "OK search total=1 shown=1 skipped=0 mode=scan\r\n");

    /* A quote inside the keyword part stays invalid. */
    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("search al\"pha"),
                                  "inner_quote_status");
    assert_output("ERROR USAGE\r\n");

    /* Seventeen distinct tags exceed the 16-slot bound. */
    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SHELL_OK,
        run("search tag:a tag:b tag:c tag:d tag:e tag:f tag:g tag:h tag:i "
            "tag:j tag:k tag:l tag:m tag:n tag:o tag:p tag:q alpha"),
        "seventeen_tags_status");
    assert_output("ERROR USAGE\r\n");

    /* A duplicated tag is still one tag, so it fits the bound. */
    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SHELL_OK, run("search tag:os tag:os alpha"), "duplicate_tag_status");
    assert_output("note.txt:4:alpha\r\n"
                  "OK search total=1 shown=1 skipped=0 mode=scan\r\n");
}

/* --------------------------------------------------------- output faults */

static void shell_output_failure_stops_show_without_retry(void)
{
    shell_fixture();
    uint32_t length = build_note("title: note\ntag: os\n");
    fs_make_file("note.txt", file_content, length);
    shell_bind();
    output_reset();
    output_fail_at = 5u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_FATAL, run("meta show note.txt"),
                                  "show_output_fatal");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(5u, output_length,
                                     "show_output_stops_at_failure");
    TEST_ASSERT_TRUE_MESSAGE(
        strstr((const char *)output_buffer, "OK meta show") == NULL,
        "show_output_no_ok_after_failure");
}

static void shell_published_change_is_not_rolled_back_by_output_failure(void)
{
    shell_fixture();
    uint32_t length = build_note_with_body("title: old\n",
                                           (const uint8_t *)"body", 4u);
    fs_make_file("note.txt", file_content, length);
    shell_bind();
    output_reset();
    output_fail_at = 3u; /* the command publishes, then the receipt fails */
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SHELL_FATAL, run("meta set note.txt title new"),
        "set_output_fatal");
    uint32_t got = read_file("note.txt");
    TEST_ASSERT_TRUE_MESSAGE(got > 0u,
                             "set_output_failure_keeps_published_file");
    TEST_ASSERT_TRUE_MESSAGE(
        strstr((const char *)readback, "title: new") != NULL,
        "set_output_failure_does_not_roll_back");
}

/* ------------------------------------------------------------ binding */

static void shell_bind_metadata_guards(void)
{
    shell_fixture();
    uint32_t reads = device_reads;
    uint32_t writes = device_writes;
    /* A context inside the shell is an alias. */
    YanNoteContext *inside = (YanNoteContext *)(void *)&shell;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SHELL_INVALID, yan_shell_bind_metadata(&shell, inside),
        "bind_context_inside_shell_invalid");
    /* A context whose span leaves uintptr_t is rejected by arithmetic. */
    YanNoteContext *wrapping = (YanNoteContext *)(uintptr_t)UINTPTR_MAX;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SHELL_INVALID, yan_shell_bind_metadata(&shell, wrapping),
        "bind_wrapping_context_invalid");
    /* A context that was never initialized cannot serve commands. */
    YanNoteContext uninitialized;
    memset(&uninitialized, 0, sizeof uninitialized);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SHELL_INVALID, yan_shell_bind_metadata(&shell, &uninitialized),
        "bind_uninitialized_context_invalid");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SHELL_INVALID, yan_shell_bind_metadata(NULL, &note),
        "bind_null_shell_invalid");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(reads, device_reads, "bind_no_reads");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(writes, device_writes, "bind_no_writes");
}

static void shell_execute_rejects_a_line_aliasing_the_metadata_context(void)
{
    shell_fixture();
    shell_bind();
    uint32_t reads = device_reads;
    output_reset();
    /* A "line" that lives inside the bound context must be refused before a
     * single output byte is produced. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SHELL_INVALID,
        yan_shell_execute(&shell, (const uint8_t *)(void *)&note,
                          (uint32_t)sizeof note),
        "execute_metadata_span_alias_invalid");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, output_length,
                                     "execute_alias_no_output");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(reads, device_reads,
                                     "execute_alias_no_io");
}

/* --------------------------------------------------------- unbound */

static void shell_unbound_keeps_old_behaviour(void)
{
    shell_fixture();
    uint32_t length = build_note_with_body("tag: os\n",
                                           (const uint8_t *)"alpha\n", 6u);
    fs_make_file("note.txt", file_content, length);
    /* No bind: the metadata commands are not part of this shell. */
    TEST_ASSERT_NULL_MESSAGE(shell.metadata,
                             "unbound_shell_metadata_is_null");

    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("meta show note.txt"),
                                  "unbound_show_status");
    assert_output("ERROR UNKNOWN_COMMAND\r\n");

    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("search alpha"),
                                  "unbound_search_status");
    assert_output("note.txt:4:alpha\r\n"
                  "OK search total=1 shown=1 skipped=0 mode=scan\r\n");

    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("help"),
                                  "unbound_help_status");
    TEST_ASSERT_TRUE_MESSAGE(
        strstr((const char *)output_buffer, "OK help") != NULL,
        "unbound_help_receipt");
}

/* ------------------------------------------------------------- registry */


static void shell_tag_quotes_backslash_and_interleaved_keywords(void)
{
    shell_fixture();
    uint32_t length = build_note_with_body("tag: os\ntag: a\"b\ntag: a\\b\n",
        (const uint8_t *)"alpha beta\n", 11u);
    fs_make_file("note.txt", file_content, length);
    shell_bind();
    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("search tag:a\"b alpha"), "raw_quote_status");
    assert_output("note.txt:6:alpha beta\r\nOK search total=1 shown=1 skipped=0 mode=scan\r\n");
    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("search alpha tag:os beta"), "interleaved_tag_status");
    assert_output("note.txt:6:alpha beta\r\nOK search total=1 shown=1 skipped=0 mode=scan\r\n");
    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("meta show note.txt"), "escaped_tag_show_status");
    assert_output("META note.txt\r\nTITLE UNKNOWN\r\nCREATED UNKNOWN\r\nMODIFIED UNKNOWN\r\n"
        "TAG os\r\nTAG a\"b\r\nTAG a\\\\b\r\nLINK NONE\r\nOK meta show\r\n");
    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("search tag:os tag:os tag:os tag:os tag:os tag:os tag:os tag:os tag:os tag:os tag:os tag:os tag:os tag:os tag:os tag:os tag:os alpha"), "seventeen_duplicate_tags_status");
    assert_output("note.txt:6:alpha beta\r\nOK search total=1 shown=1 skipped=0 mode=scan\r\n");
}
static void shell_invalid_title_and_names_are_usage_before_io(void)
{
    shell_fixture(); shell_bind();
    uint32_t reads = device_reads, writes = device_writes;
    char command[300];
    memcpy(command, "meta set note.txt title ", 24u);
    memset(command + 24u, 'a', 257u); command[281u] = 0;
    output_reset();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, run(command));
    assert_output("ERROR USAGE\r\n");
    const char *bad[] = {"meta show bad/name", "meta set bad/name title x",
        "meta tag bad/name add os", "meta link bad/name add target",
        "meta set note.txt title \xC2\x85", "meta set note.txt title \xFF"};
    for(uint32_t i=0; i<sizeof bad/sizeof bad[0]; ++i) {
        output_reset(); TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK,run(bad[i]));
        assert_output("ERROR USAGE\r\n");
    }
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(reads,device_reads,"invalid_declarations_zero_reads");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(writes,device_writes,"invalid_declarations_zero_writes");
}
static void shell_binding_extent_lifecycle_and_unbind(void)
{
    shell_fixture();
    static YanShell empty_shell;
    memset(&empty_shell,0,sizeof empty_shell);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_INVALID,yan_shell_bind_metadata(&empty_shell,&note),"bind_empty_shell_invalid_before_backend");
    YanShell *wrap=(YanShell *)(uintptr_t)(UINTPTR_MAX-sizeof(YanShell)+1u);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_INVALID,yan_shell_bind_metadata(wrap,NULL),"unbind_wrapping_shell_invalid");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_INVALID,yan_shell_bind_metadata(wrap,&note),"bind_wrapping_shell_invalid");
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_INVALID,yan_shell_execute(wrap,NULL,0u));
    shell_bind();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK,yan_shell_bind_metadata(&shell,NULL));
    output_reset();TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK,run("meta show n"));
    assert_output("ERROR UNKNOWN_COMMAND\r\n");
}


static void shell_bound_search_keeps_leading_minus_grammar(void)
{
    shell_fixture();
    uint32_t n=build_note_with_body("tag: os\n",(const uint8_t *)"alpha beta\n",11u);
    fs_make_file("n",file_content,n);shell_bind();
    const char *bad[]={"search -alpha", "search tag:os -alpha", "search -alpha tag:os"};
    uint32_t reads=device_reads;
    for(uint32_t i=0;i<sizeof bad/sizeof bad[0];++i) {
        output_reset();TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK,run(bad[i]));
        assert_output("ERROR USAGE\r\n");
    }
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(reads,device_reads,"leading_minus_usage_zero_reads");
    output_reset();TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK,run("search alpha tag:os -beta"));
    assert_output("n:4:alpha beta\r\nOK search total=1 shown=1 skipped=0 mode=scan\r\n");
    output_reset();TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK,run("search \"-alpha\""));
    assert_output("n:4:alpha beta\r\nOK search total=1 shown=1 skipped=0 mode=scan\r\n");
}
static void shell_title_256_and_empty_separator_boundary(void)
{
    shell_fixture();fs_make_file("n",(const uint8_t *)"body",4u);shell_bind();
    char command[300]; static const char prefix[]="meta set n title ";
    uint32_t prefix_length=sizeof prefix-1u;
    memcpy(command,prefix,prefix_length);memset(command+prefix_length,'a',256u);command[prefix_length+256u]=0;
    output_reset();TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK,run(command));assert_output("OK meta set\r\n");
    TEST_ASSERT_EQUAL_INT(YAN_NOTE_OK,yan_note_show(&note,"n",&shell.metadata_info));
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(256u,shell.metadata_info.title_length,"shell_title_256_preserved");
    command[prefix_length+256u]='a';command[prefix_length+257u]=0;
    output_reset();TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK,run(command));assert_output("ERROR USAGE\r\n");
    output_reset();TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK,run("meta set n title  "));assert_output("OK meta set\r\n");
    TEST_ASSERT_EQUAL_INT(YAN_NOTE_OK,yan_note_show(&note,"n",&shell.metadata_info));
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(1u,shell.metadata_info.title_length,"second_separator_is_space_value");
    output_reset();TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK,run("meta set n title "));assert_output("OK meta set\r\n");
    TEST_ASSERT_EQUAL_INT(YAN_NOTE_OK,yan_note_show(&note,"n",&shell.metadata_info));
    TEST_ASSERT_FALSE_MESSAGE(shell.metadata_info.has_title,"single_separator_clears_title");
}

static const char HELP_UNBOUND_EXPECTED[] =
    "help: show commands and limits\r\n"
    "ls: list files as <size> <name>\r\n"
    "stat NAME: show a file's size\r\n"
    "cat NAME: print a file\r\n"
    "create NAME [TEXT]: create a new file\r\n"
    "write NAME [TEXT]: replace an existing file\r\n"
    "rm NAME: remove a file\r\n"
    "mv OLD NEW: rename a file\r\n"
    "cp SRC DEST: copy a file\r\n"
    "grep TOKEN: print matching lines as FILE:LINE:CONTENT\r\n"
    "search QUERY: print term matches as FILE:LINE:SNIPPET\r\n"
    "rebuild: rebuild the term index\r\n"
    "index status|clear: show or drop the term index\r\n"
    "exit: end the session\r\n"
    "line: at most 1023 bytes, ended by CR or LF\r\n"
    "separators: ASCII spaces; commands and names are case sensitive\r\n"
    "names: 1 to 31 bytes of A-Z a-z 0-9 . _ - and never . or ..\r\n"
    "create creates only; write replaces only; no LF is added to text\r\n"
    "cat: printable ASCII as is, a backslash as two backslashes, C0 and DEL as \\xHH\r\n"
    "cat: valid UTF-8 as is except C1 U+0080 to U+009F, escaped byte by byte\r\n"
    "OK help\r\n";
static const char HELP_BOUND_EXPECTED[] =
    "help: show commands and limits\r\n"
    "ls: list files as <size> <name>\r\n"
    "stat NAME: show a file's size\r\n"
    "cat NAME: print a file\r\n"
    "create NAME [TEXT]: create a new file\r\n"
    "write NAME [TEXT]: replace an existing file\r\n"
    "rm NAME: remove a file\r\n"
    "mv OLD NEW: rename a file\r\n"
    "cp SRC DEST: copy a file\r\n"
    "grep TOKEN: print matching lines as FILE:LINE:CONTENT\r\n"
    "search QUERY: print term matches as FILE:LINE:SNIPPET\r\n"
    "rebuild: rebuild the term index\r\n"
    "index status|clear: show or drop the term index\r\n"
    "meta show NAME: print the note's declared metadata\r\n"
    "meta set NAME title|created|modified VALUE: declare or clear a field\r\n"
    "meta tag NAME add|del TAG: change one tag\r\n"
    "meta link NAME add|del TARGET: change one link\r\n"
    "meta list [tag:TAG ...]: list notes whose tags match\r\n"
    "search tag:TAG QUERY: search only notes carrying every tag\r\n"
    "exit: end the session\r\n"
    "line: at most 1023 bytes, ended by CR or LF\r\n"
    "separators: ASCII spaces; commands and names are case sensitive\r\n"
    "names: 1 to 31 bytes of A-Z a-z 0-9 . _ - and never . or ..\r\n"
    "create creates only; write replaces only; no LF is added to text\r\n"
    "cat: printable ASCII as is, a backslash as two backslashes, C0 and DEL as \\xHH\r\n"
    "cat: valid UTF-8 as is except C1 U+0080 to U+009F, escaped byte by byte\r\n"
    "OK help\r\n";
static void shell_bound_and_unbound_help_exact(void)
{
    shell_fixture(); output_reset();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK,run("help")); assert_output(HELP_UNBOUND_EXPECTED);
    shell_bind(); output_reset();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK,run("help")); assert_output(HELP_BOUND_EXPECTED);
}
static YanShellResult bind_from_service_callback_result;
static bool bind_from_service_callback(void *opaque, const char *name)
{
    (void)opaque; (void)name;
    bind_from_service_callback_result=yan_shell_bind_metadata(&shell,&note);
    return false;
}
static void shell_binding_active_service_is_busy(void)
{
    shell_fixture();fs_make_file("n",NULL,0u);
    uint32_t shown=0xdeadbeefu;
    TEST_ASSERT_EQUAL_INT(YAN_NOTE_STOPPED,yan_note_list(&note,NULL,bind_from_service_callback,NULL,&shown));
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_BUSY,bind_from_service_callback_result,"bind_active_note_busy");
    TEST_ASSERT_NULL(shell.metadata);
    TEST_ASSERT_EQUAL_UINT32(0xdeadbeefu,shown);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(shell_bound_and_unbound_help_exact);
    RUN_TEST(shell_binding_active_service_is_busy);
    RUN_TEST(shell_bound_search_keeps_leading_minus_grammar);
    RUN_TEST(shell_title_256_and_empty_separator_boundary);
    RUN_TEST(shell_tag_quotes_backslash_and_interleaved_keywords);
    RUN_TEST(shell_invalid_title_and_names_are_usage_before_io);
    RUN_TEST(shell_binding_extent_lifecycle_and_unbind);
    RUN_TEST(shell_meta_show_unknown_and_none_for_binary);
    RUN_TEST(shell_meta_show_all_fields_and_dangling_link);
    RUN_TEST(shell_meta_update_receipts_rewrite_canonical_file);
    RUN_TEST(shell_meta_bad_header_metadata_error_for_show_and_list);
    RUN_TEST(shell_meta_list_sorts_and_ands_with_case_distinct);
    RUN_TEST(shell_meta_usage_errors_do_no_io);
    RUN_TEST(shell_search_tag_operator_and_plain_untagged);
    RUN_TEST(shell_metadata_change_invalidates_search_eligibility);
    RUN_TEST(shell_tag_and_quote_grammar_rules);
    RUN_TEST(shell_output_failure_stops_show_without_retry);
    RUN_TEST(shell_published_change_is_not_rolled_back_by_output_failure);
    RUN_TEST(shell_bind_metadata_guards);
    RUN_TEST(shell_execute_rejects_a_line_aliasing_the_metadata_context);
    RUN_TEST(shell_unbound_keeps_old_behaviour);
    return UNITY_END();
}
