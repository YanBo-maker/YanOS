/* 0028 shell relation binding: native tests for the implemented binding and the
 * three routed commands.
 *
 * The contract is os/shell.h (the 0028 binding paragraph) and
 * docs/specs/0028-knowledge-relations.md ("公共服务与接口责任"). The relation
 * guard chain and the independent help fragment are implemented, and a valid
 * relation context is published: the bind routes the three commands through the
 * service. The fixtures are the real static YanFs,
 * YanNoteContext, relation service, literal/term facades and the real shell from
 * test_shell_metadata.c, so a failure here is a genuine behavioural failure, not
 * a compile, link or fixture abort.
 *
 * What this file pins, clause by clause:
 *
 *   - an unbound shell keeps the old `ERROR UNKNOWN_COMMAND` for the three
 *     relation commands and the byte-identical old help;
 *   - the bind guard chain rejects a null/wrapping shell, a null form unbinds
 *     with YAN_SHELL_OK, a wrapping, uninitialized, busy or foreign-filesystem
 *     relation context is refused, and the relation span may not overlap a
 *     bound metadata context (and vice versa);
 *   - a valid bind publishes the pointer, and the relations-only and combined
 *     help goldens pin the exact three-line fragment;
 *   - `links`, `backlinks` and `related` exact CRLF goldens, declaration order,
 *     dangling targets, a broken target head being irrelevant, a broken source
 *     head plus a global late bad head reporting METADATA with no partial list,
 *     missing-source NOT_FOUND priority, normal NOT_FOUND staying nonfatal and
 *     IO/PROTOCOL becoming fatal;
 *   - USAGE for missing, extra and invalid parameters with zero device I/O;
 *   - a first-byte, middle-byte and summary-byte output failure is fatal, emits
 *     no OK receipt and performs no further device read once callbacks began;
 *   - `total>20` shows exactly 20 rows while the summary keeps the full total;
 *   - a command line aliasing a bound relation context is rejected before the
 *     parse, and re-entering the shell or the bind from the output callback is
 *     YAN_SHELL_BUSY.
 *
 * Every assertion carries a stable, unique message tag for the later
 * owner-specific mutation gate. */

#include "shell.h"
#include "note_metadata.h"
#include "note_relations.h"
#include "search.h"
#include "search_linear.h"
#include "search_terms.h"
#include "search_terms_linear.h"
#include "unity.h"
#include "yanfs.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------- fixtures */

#define SHELL_BLOCKS 2048u /* 8 MiB */
#define OUTPUT_MAX 8192u

static uint8_t medium[SHELL_BLOCKS][YAN_FS_BLOCK_SIZE];
static uint32_t device_reads;
static uint32_t device_writes;
static uint32_t device_read_fail_at;
static YanFsIoResult device_read_fail_code;
static bool device_armed;
static YanFs fs;
static YanFs other_fs;
static YanNoteContext note;
static YanRelationContext relations;
static YanRelationContext uninit_relations;
static YanRelationContext other_relations;
static YanSearchLinear linear;
static YanSearch search;
static YanSearchTermsLinear terms_linear;
static YanSearchTerms terms;
static YanShell shell;

static uint8_t output_buffer[OUTPUT_MAX];
static uint32_t output_length;
static uint32_t output_fail_at; /* 0 = never */
static bool callback_seen;
static uint32_t io_at_first_callback;

static bool reenter_execute_armed;
static YanShellResult reenter_execute_result;
static bool reenter_bind_armed;
static YanShellResult reenter_bind_result;

static uint8_t file_content[8192];
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
    if (device_armed && device_read_fail_at != 0u &&
        device_reads == device_read_fail_at) {
        return device_read_fail_code;
    }
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
    if (!callback_seen) {
        callback_seen = true;
        io_at_first_callback = device_reads;
    }
    if (reenter_execute_armed) {
        reenter_execute_armed = false;
        reenter_execute_result = yan_shell_execute(&shell, line_buffer, 0u);
    }
    if (reenter_bind_armed) {
        reenter_bind_armed = false;
        reenter_bind_result = yan_shell_bind_relations(&shell, &relations);
    }
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
    callback_seen = false;
    io_at_first_callback = 0u;
}

static void assert_output_tagged(const char *expected, const char *tag)
{
    uint32_t expected_length = (uint32_t)strlen(expected);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(expected_length, output_length, tag);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(expected, output_buffer, expected_length,
                                     tag);
}

/* --------------------------------------------------------------- helpers */

static void shell_fixture(void)
{
    memset(&fs, 0, sizeof fs);
    memset(&note, 0, sizeof note);
    memset(&relations, 0, sizeof relations);
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
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_OK, yan_relation_init(&relations, &fs),
                                  "fixture_relation_init");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SHELL_OK,
        yan_shell_init(&shell, &fs, &search, &terms, shell_output()),
        "fixture_shell_init");
    output_reset();
}

/* The metadata bind is implemented; this helper is therefore green. */
static void shell_bind_meta(void)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SHELL_OK, yan_shell_bind_metadata(&shell, &note),
        "fixture_bind_metadata_ok");
}

/* Binds the relation context; every relation case starts from this successful
 * binding. */
static void shell_bind_rels(void)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SHELL_OK, yan_shell_bind_relations(&shell, &relations),
        "fixture_bind_relations_ok");
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

/* Marker + the caller's field lines + close. */
static uint32_t build_note(const char *lines)
{
    const char *marker = "@yan-note 1\n";
    uint32_t length = (uint32_t)strlen(marker);
    memcpy(file_content, marker, length);
    if (lines != NULL) {
        uint32_t count = (uint32_t)strlen(lines);
        memcpy(file_content + length, lines, count);
        length += count;
    }
    memcpy(file_content + length, "@end\n", 5u);
    return length + 5u;
}

/* ------------------------------------------------------------ help bytes */

static const char HELP_BODY_UNBOUND[] =
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
    "cat: printable ASCII as is, a backslash as two backslashes, C0 and DEL as "
    "\\xHH\r\n"
    "cat: valid UTF-8 as is except C1 U+0080 to U+009F, escaped byte by byte\r\n";

static const char HELP_BODY_METADATA[] =
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
    "cat: printable ASCII as is, a backslash as two backslashes, C0 and DEL as "
    "\\xHH\r\n"
    "cat: valid UTF-8 as is except C1 U+0080 to U+009F, escaped byte by byte\r\n";

static const char HELP_RELATIONS_LINES[] =
    "links NAME: list a note's declared outgoing links\r\n"
    "backlinks NAME: list files that declare NAME\r\n"
    "related NAME: list files related by a link or a shared tag\r\n";

static void expect_help(const char *body, bool relations_bound,
                        const char *tag)
{
    uint32_t used = 0u;
    uint32_t body_length = (uint32_t)strlen(body);
    memcpy(expected_buffer + used, body, body_length);
    used += body_length;
    if (relations_bound) {
        uint32_t lines_length = (uint32_t)strlen(HELP_RELATIONS_LINES);
        memcpy(expected_buffer + used, HELP_RELATIONS_LINES, lines_length);
        used += lines_length;
    }
    memcpy(expected_buffer + used, "OK help\r\n", 9u);
    used += 9u;
    expected_buffer[used] = '\0';
    assert_output_tagged((const char *)expected_buffer, tag);
}

void setUp(void)
{
    memset(medium, 0, sizeof medium);
    device_reads = 0u;
    device_writes = 0u;
    device_read_fail_at = 0u;
    device_read_fail_code = YAN_FS_IO_ERROR;
    device_armed = false;
    memset(&fs, 0, sizeof fs);
    memset(&other_fs, 0, sizeof other_fs);
    memset(&note, 0, sizeof note);
    memset(&relations, 0, sizeof relations);
    memset(&uninit_relations, 0, sizeof uninit_relations);
    memset(&other_relations, 0, sizeof other_relations);
    memset(&linear, 0, sizeof linear);
    memset(&search, 0, sizeof search);
    memset(&terms_linear, 0, sizeof terms_linear);
    memset(&terms, 0, sizeof terms);
    memset(&shell, 0, sizeof shell);
    memset(file_content, 0, sizeof file_content);
    memset(line_buffer, 0, sizeof line_buffer);
    memset(expected_buffer, 0, sizeof expected_buffer);
    output_reset();
    reenter_execute_armed = false;
    reenter_execute_result = YAN_SHELL_OK;
    reenter_bind_armed = false;
    reenter_bind_result = YAN_SHELL_OK;
}

void tearDown(void)
{
}

/* ------------------------------------------------------------ unbound path */

static void shell_unbound_relations_unknown_and_old_help(void)
{
    shell_fixture();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("links a.txt"),
                                  "unbound_links_status");
    assert_output_tagged("ERROR UNKNOWN_COMMAND\r\n",
                         "unbound_links_unknown_command");
    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("backlinks a.txt"),
                                  "unbound_backlinks_status");
    assert_output_tagged("ERROR UNKNOWN_COMMAND\r\n",
                         "unbound_backlinks_unknown_command");
    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("related a.txt"),
                                  "unbound_related_status");
    assert_output_tagged("ERROR UNKNOWN_COMMAND\r\n",
                         "unbound_related_unknown_command");
    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("help"),
                                  "unbound_help_status");
    expect_help(HELP_BODY_UNBOUND, false, "unbound_help_old_bytes");
    TEST_ASSERT_NULL_MESSAGE(shell.relations,
                             "unbound_shell_relations_is_null");
}

/* ------------------------------------------------------------ bind guards */

static void shell_bind_relations_guards(void)
{
    shell_fixture();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_INVALID,
                                  yan_shell_bind_relations(NULL, &relations),
                                  "bind_relations_null_shell_invalid");
    YanShell *wrapping_shell = (YanShell *)(uintptr_t)UINTPTR_MAX;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SHELL_INVALID,
        yan_shell_bind_relations(wrapping_shell, &relations),
        "bind_relations_wrapping_shell_invalid");
    YanRelationContext *wrapping =
        (YanRelationContext *)(uintptr_t)UINTPTR_MAX;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SHELL_INVALID, yan_shell_bind_relations(&shell, wrapping),
        "bind_relations_wrapping_context_invalid");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SHELL_INVALID, yan_shell_bind_relations(&shell, &uninit_relations),
        "bind_relations_uninitialized_context_invalid");

    /* A second filesystem sharing the same formatted medium is enough to make
     * the borrowed-source equality observable without touching the fixture. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK,
                                  yan_fs_init(&other_fs, fs_backend()),
                                  "bind_relations_other_fs_init");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK, yan_fs_mount(&other_fs),
                                  "bind_relations_other_fs_mount");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_NOTE_OK, yan_relation_init(&other_relations, &other_fs),
        "bind_relations_other_context_init");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SHELL_INVALID,
        yan_shell_bind_relations(&shell, &other_relations),
        "bind_relations_foreign_filesystem_invalid");

    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK,
                                  yan_shell_bind_relations(&shell, NULL),
                                  "bind_relations_null_unbind_ok");
    TEST_ASSERT_NULL_MESSAGE(shell.relations,
                             "bind_relations_null_unbind_clears");
}

static void shell_bind_relations_overlapping_metadata_invalid(void)
{
    shell_fixture();
    shell_bind_meta();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SHELL_INVALID,
        yan_shell_bind_relations(&shell, (YanRelationContext *)(void *)&note),
        "bind_relations_overlapping_metadata_invalid");
}

static void shell_bind_metadata_overlapping_relations_invalid(void)
{
    shell_fixture();
    shell_bind_rels();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SHELL_INVALID,
        yan_shell_bind_metadata(&shell, (YanNoteContext *)(void *)&relations),
        "bind_metadata_overlapping_relations_invalid");
}

static void shell_bind_relations_valid_publishes_pointer(void)
{
    shell_fixture();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SHELL_OK, yan_shell_bind_relations(&shell, &relations),
        "bind_relations_valid_bind_ok");
    TEST_ASSERT_TRUE_MESSAGE(shell.relations == &relations,
                             "bind_relations_valid_publishes_pointer");
}

/* --------------------------------------------------------------- help grid */

static void shell_relations_only_help_exact(void)
{
    shell_fixture();
    shell_bind_rels();
    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("help"),
                                  "relations_only_help_status");
    expect_help(HELP_BODY_UNBOUND, true, "relations_only_help_bytes");
}

static void shell_metadata_only_help_unchanged(void)
{
    shell_fixture();
    shell_bind_meta();
    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("help"),
                                  "metadata_only_help_legacy_status");
    expect_help(HELP_BODY_METADATA, false, "metadata_only_help_legacy_bytes");
}

static void shell_metadata_and_relations_help_exact(void)
{
    shell_fixture();
    shell_bind_meta();
    shell_bind_rels();
    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("help"),
                                  "both_bound_help_status");
    expect_help(HELP_BODY_METADATA, true, "both_bound_help_bytes");
}

/* --------------------------------------------------------------- commands */

static void shell_links_exact_golden(void)
{
    shell_fixture();
    fs_make_file("z.txt", (const uint8_t *)"z", 1u);
    uint32_t length =
        build_note("link: z.txt\nlink: missing.txt\nlink: src.txt\n");
    fs_make_file("src.txt", file_content, length);
    shell_bind_rels();
    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("links src.txt"),
                                  "links_golden_status");
    assert_output_tagged(
        "LINK z.txt OK\r\n"
        "LINK missing.txt DANGLING\r\n"
        "LINK src.txt OK\r\n"
        "OK links shown=3\r\n",
        "links_golden_bytes");
}

static void shell_links_bad_target_allowed_bad_source_metadata(void)
{
    shell_fixture();
    uint32_t length = build_note("kind: note\n");
    fs_make_file("bad.txt", file_content, length);
    length = build_note("link: bad.txt\nlink: gone.txt\n");
    fs_make_file("src.txt", file_content, length);
    shell_bind_rels();
    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("links src.txt"),
                                  "links_bad_target_status");
    assert_output_tagged(
        "LINK bad.txt OK\r\n"
        "LINK gone.txt DANGLING\r\n"
        "OK links shown=2\r\n",
        "links_bad_target_bytes");
    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("links bad.txt"),
                                  "links_bad_source_status");
    assert_output_tagged("ERROR METADATA\r\n", "links_bad_source_metadata");
}

static void shell_backlinks_exact_golden(void)
{
    shell_fixture();
    uint32_t length = build_note("link: ghost.txt\n");
    fs_make_file("zeta.txt", file_content, length);
    fs_make_file("alpha.txt", file_content, length);
    fs_make_file("ghost.txt", file_content, length);
    shell_bind_rels();
    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("backlinks ghost.txt"),
                                  "backlinks_golden_status");
    assert_output_tagged(
        "BACKLINK alpha.txt\r\n"
        "BACKLINK ghost.txt\r\n"
        "BACKLINK zeta.txt\r\n"
        "OK backlinks shown=3 target=OK\r\n",
        "backlinks_golden_bytes");
    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("backlinks absent.txt"),
                                  "backlinks_dangling_status");
    assert_output_tagged("OK backlinks shown=0 target=DANGLING\r\n",
                         "backlinks_dangling_receipt");
}

static void shell_related_exact_golden(void)
{
    shell_fixture();
    uint32_t length = build_note("link: src.txt\n");
    fs_make_file("b.txt", file_content, length);
    length = build_note("tag: os\ntag: riscv\ntag: x\n");
    fs_make_file("c.txt", file_content, length);
    length = build_note(
        "tag: os\ntag: riscv\ntag: x\nlink: b.txt\nlink: c.txt\n");
    fs_make_file("src.txt", file_content, length);
    shell_bind_rels();
    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("related src.txt"),
                                  "related_golden_status");
    assert_output_tagged(
        "RELATED b.txt out=1 in=1 tags=0\r\n"
        "RELATED c.txt out=1 in=0 tags=3\r\n"
        "OK related total=2 shown=2\r\n",
        "related_golden_bytes");
}

static void shell_related_missing_source_priority(void)
{
    shell_fixture();
    uint32_t length = build_note("kind: note\n");
    fs_make_file("remote.txt", file_content, length);
    shell_bind_rels();
    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("related absent.txt"),
                                  "related_missing_status");
    assert_output_tagged("ERROR NOT_FOUND\r\n",
                         "related_missing_source_not_found");
}

static void shell_backlinks_global_bad_head_no_partial(void)
{
    shell_fixture();
    uint32_t length = build_note("link: ghost.txt\n");
    fs_make_file("a.txt", file_content, length);
    fs_make_file("b.txt", file_content, length);
    length = build_note("kind: note\n");
    fs_make_file("z.txt", file_content, length);
    shell_bind_rels();
    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("backlinks ghost.txt"),
                                  "backlinks_late_bad_head_status");
    assert_output_tagged("ERROR METADATA\r\n",
                         "backlinks_late_bad_head_no_partial");
}

static void shell_related_top20_full_total(void)
{
    shell_fixture();
    uint32_t length = build_note("link: src.txt\n");
    char name[16];
    for (uint32_t i = 0u; i < 25u; ++i) {
        (void)snprintf(name, sizeof name, "c%02u.txt", i);
        fs_make_file(name, file_content, length);
    }
    length = build_note(NULL);
    fs_make_file("src.txt", file_content, length);
    shell_bind_rels();
    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("related src.txt"),
                                  "related_top20_status");
    uint32_t used = 0u;
    for (uint32_t i = 0u; i < 20u; ++i) {
        used += (uint32_t)snprintf((char *)expected_buffer + used,
                                   sizeof expected_buffer - used,
                                   "RELATED c%02u.txt out=0 in=1 tags=0\r\n", i);
    }
    used += (uint32_t)snprintf((char *)expected_buffer + used,
                               sizeof expected_buffer - used,
                               "OK related total=25 shown=20\r\n");
    expected_buffer[used] = '\0';
    assert_output_tagged((const char *)expected_buffer,
                         "related_top20_bytes");
}

static void shell_relations_usage_zero_io(void)
{
    shell_fixture();
    shell_bind_rels();
    device_reads = 0u;
    device_writes = 0u;
    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("links"),
                                  "usage_links_missing_status");
    assert_output_tagged("ERROR USAGE\r\n", "usage_links_missing");
    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("links a b"),
                                  "usage_links_extra_status");
    assert_output_tagged("ERROR USAGE\r\n", "usage_links_extra");
    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("links bad!name"),
                                  "usage_links_invalid_status");
    assert_output_tagged("ERROR USAGE\r\n", "usage_links_invalid");
    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("backlinks"),
                                  "usage_backlinks_missing_status");
    assert_output_tagged("ERROR USAGE\r\n", "usage_backlinks_missing");
    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("related src.txt extra"),
                                  "usage_related_extra_status");
    assert_output_tagged("ERROR USAGE\r\n", "usage_related_extra");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, device_reads,
                                     "usage_relations_zero_reads");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, device_writes,
                                     "usage_relations_zero_writes");
}

static void shell_relations_normal_not_found_is_nonfatal(void)
{
    shell_fixture();
    shell_bind_rels();
    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("links absent.txt"),
                                  "normal_not_found_status");
    assert_output_tagged("ERROR NOT_FOUND\r\n", "normal_not_found_error");
    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("ls"),
                                  "normal_not_found_session_continues");
}

static void shell_relations_io_error_is_fatal(void)
{
    shell_fixture();
    uint32_t length = build_note("link: z.txt\n");
    fs_make_file("src.txt", file_content, length);
    fs_make_file("z.txt", (const uint8_t *)"z", 1u);
    shell_bind_rels();
    output_reset();
    device_armed = true;
    device_read_fail_at = device_reads + 1u;
    device_read_fail_code = YAN_FS_IO_ERROR;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_FATAL, run("links src.txt"),
                                  "io_error_fatal_status");
    assert_output_tagged("ERROR IO\r\n", "io_error_error_line");
}

static void shell_relations_protocol_error_is_fatal(void)
{
    shell_fixture();
    uint32_t length = build_note("link: z.txt\n");
    fs_make_file("src.txt", file_content, length);
    fs_make_file("z.txt", (const uint8_t *)"z", 1u);
    shell_bind_rels();
    output_reset();
    device_armed = true;
    device_read_fail_at = device_reads + 1u;
    device_read_fail_code = YAN_FS_IO_PROTOCOL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_FATAL, run("links src.txt"),
                                  "protocol_error_fatal_status");
    assert_output_tagged("ERROR PROTOCOL\r\n", "protocol_error_error_line");
}

static void shell_relations_output_failure_is_fatal(void)
{
    shell_fixture();
    fs_make_file("z.txt", (const uint8_t *)"z", 1u);
    uint32_t length = build_note("link: z.txt\nlink: missing.txt\n");
    fs_make_file("src.txt", file_content, length);
    shell_bind_rels();

    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("links src.txt"),
                                  "output_failure_baseline_status");
    uint32_t full = output_length;
    static uint8_t healthy_output[OUTPUT_MAX];
    memcpy(healthy_output, output_buffer, full);
    TEST_ASSERT_TRUE_MESSAGE(full > 0u, "output_failure_baseline_nonempty");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(io_at_first_callback, device_reads,
                                     "output_failure_baseline_no_io_in_callbacks");

    static const uint32_t positions[3] = {1u, 0u, 0u};
    uint32_t middle = full / 2u;
    uint32_t cases[3];
    cases[0] = 1u;      /* first byte */
    cases[1] = middle;  /* middle byte */
    cases[2] = full;    /* summary byte */
    (void)positions;
    static const char *tags[3] = {"output_failure_first", "output_failure_middle",
                                  "output_failure_summary"};
    for (uint32_t i = 0u; i < 3u; ++i) {
        output_reset();
        output_fail_at = cases[i];
        TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_FATAL, run("links src.txt"),
                                      tags[i]);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(cases[i], output_length, tags[i]);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(io_at_first_callback, device_reads,
                                         tags[i]);
        if (cases[i] > 1u) {
            TEST_ASSERT_EQUAL_MEMORY_MESSAGE(healthy_output, output_buffer, cases[i] - 1u,
                                             tags[i]);
        }
        /* A receipt may already be partially delivered. The refused byte and
         * every later byte must be absent; accepted bytes cannot be retracted. */
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(0u, output_buffer[cases[i] - 1u], tags[i]);
    }
}

/* --------------------------------------------------- aliases and re-entry */

static void shell_execute_rejects_line_aliasing_bound_relations(void)
{
    shell_fixture();
    shell_bind_rels();
    output_reset();
    const uint8_t *alias = (const uint8_t *)(void *)&relations;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_INVALID,
                                  yan_shell_execute(&shell, alias, 1u),
                                  "execute_line_aliasing_relations_invalid");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, output_length,
                                     "execute_line_aliasing_relations_no_output");
}

static void shell_bind_busy_from_output_callback(void)
{
    shell_fixture();
    output_reset();
    reenter_bind_armed = true;
    reenter_bind_result = YAN_SHELL_OK;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("help"),
                                  "bind_from_callback_outer_status");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_BUSY, reenter_bind_result,
                                  "bind_relations_from_callback_busy");
    TEST_ASSERT_NULL_MESSAGE(shell.relations,
                             "bind_relations_from_callback_unbound");
}

static void shell_execute_reentry_from_output_callback_busy(void)
{
    shell_fixture();
    output_reset();
    reenter_execute_armed = true;
    reenter_execute_result = YAN_SHELL_OK;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("help"),
                                  "execute_reentry_outer_status");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_BUSY, reenter_execute_result,
                                  "execute_reentry_inner_busy");
}

/* ------------------------------------------------------------- registry */

static bool bind_active_relation(void *opaque, const YanRelationRow *row)
{
    (void)opaque;
    (void)row;
    uint32_t before = device_reads;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_BUSY,
        yan_shell_bind_relations(&shell, &relations), "bind_active_relation_busy");
    TEST_ASSERT_NULL_MESSAGE(shell.relations, "bind_active_relation_not_published");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(before, device_reads, "bind_active_relation_zero_reads");
    return true;
}

static void shell_bind_busy_relation_and_backend_aliases_are_rejected(void)
{
    shell_fixture();
    uint32_t length = build_note("link: n\n");
    fs_make_file("n", file_content, length);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_NOTE_OK,
        yan_relation_links(&relations, "n", bind_active_relation, NULL, NULL),
        "bind_active_relation_outer_ok");
    uint32_t before = device_reads;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_INVALID,
        yan_shell_bind_relations(&shell, (YanRelationContext *)(void *)&linear),
        "bind_literal_backend_alias_invalid");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_INVALID,
        yan_shell_bind_relations(&shell, (YanRelationContext *)(void *)&terms_linear),
        "bind_term_backend_alias_invalid");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_INVALID,
        yan_shell_bind_relations(&shell, (YanRelationContext *)(void *)&fs),
        "bind_shared_fs_source_alias_invalid");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(before, device_reads, "bind_backend_alias_zero_reads");
}

static void shell_relation_unbind_restores_help_and_unknown_commands(void)
{
    shell_fixture();
    shell_bind_rels();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, yan_shell_bind_relations(&shell, NULL),
                                  "relation_unbind_ok");
    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("help"), "relation_unbind_help_status");
    expect_help(HELP_BODY_UNBOUND, false, "relation_unbind_restores_help");
    output_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, run("links n"), "relation_unbind_command_status");
    assert_output_tagged("ERROR UNKNOWN_COMMAND\r\n", "relation_unbind_unknown_command");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(shell_unbound_relations_unknown_and_old_help);
    RUN_TEST(shell_bind_relations_guards);
    RUN_TEST(shell_bind_relations_overlapping_metadata_invalid);
    RUN_TEST(shell_bind_metadata_overlapping_relations_invalid);
    RUN_TEST(shell_bind_relations_valid_publishes_pointer);
    RUN_TEST(shell_relations_only_help_exact);
    RUN_TEST(shell_metadata_only_help_unchanged);
    RUN_TEST(shell_metadata_and_relations_help_exact);
    RUN_TEST(shell_links_exact_golden);
    RUN_TEST(shell_links_bad_target_allowed_bad_source_metadata);
    RUN_TEST(shell_backlinks_exact_golden);
    RUN_TEST(shell_related_exact_golden);
    RUN_TEST(shell_related_missing_source_priority);
    RUN_TEST(shell_backlinks_global_bad_head_no_partial);
    RUN_TEST(shell_related_top20_full_total);
    RUN_TEST(shell_relations_usage_zero_io);
    RUN_TEST(shell_relations_normal_not_found_is_nonfatal);
    RUN_TEST(shell_relations_io_error_is_fatal);
    RUN_TEST(shell_relations_protocol_error_is_fatal);
    RUN_TEST(shell_relations_output_failure_is_fatal);
    RUN_TEST(shell_execute_rejects_line_aliasing_bound_relations);
    RUN_TEST(shell_bind_busy_from_output_callback);
    RUN_TEST(shell_execute_reentry_from_output_callback_busy);
    RUN_TEST(shell_bind_busy_relation_and_backend_aliases_are_rejected);
    RUN_TEST(shell_relation_unbind_restores_help_and_unknown_commands);
    return UNITY_END();
}
