#include "search_linear.h"
#include "search_terms_index.h"
#include "shell.h"
#include "unity.h"

#include <string.h>

/* 0026 shell/CLI front ends: search, rebuild and index status|clear.
 *
 * Two injected term configurations cover the contract:
 *
 *   real   the production in-memory index over a real YanFs, so the CLI is
 *          driven through the true build/lookup/fallback paths;
 *   fake   a scripted term backend, so exact output bytes, summary counts,
 *          truncation flags, reader faults and reentry can be pinned without
 *          depending on a source fixture.
 *
 * Every command goes through the public yan_shell_execute. The output is
 * captured byte for byte. The index context is a static object (it is over a
 * MiB and must never sit on a task stack). */

#define TERM_BLOCKS 64u
#define CAPTURE_CAPACITY 65536u
#define FAKE_MATCH_MAX 8u

typedef struct {
    uint8_t blocks[TERM_BLOCKS][YAN_FS_BLOCK_SIZE];
    uint32_t reads;
    uint32_t writes;
    uint32_t read_fail_at;
    YanFsIoResult read_fail_code;
} TermDevice;

static TermDevice device;
static YanFs fs;
static YanShell shell;
static YanSearchLinear literal_linear;
static YanSearch literal;
static YanSearchTermsIndex term_index;
static YanSearchTerms term_facade;
static uint8_t fake_source[16];

typedef struct {
    uint8_t bytes[CAPTURE_CAPACITY];
    uint32_t length;
    uint32_t calls;
    uint32_t fail_at;
    bool fail_seen;
} Capture;

static Capture capture;

typedef struct {
    const char *name;
    uint32_t line;
    uint16_t score;
    bool left;
    bool right;
    const uint8_t *snippet;
    uint32_t snippet_length;
} FakeMatch;

typedef struct {
    uint32_t query_calls;
    uint32_t read_calls;
    uint32_t status_calls;
    uint32_t rebuild_calls;
    uint32_t clear_calls;
    YanSearchTermsResult query_result;
    YanSearchTermsResult read_result;
    YanSearchTermsResult status_result;
    YanSearchTermsResult rebuild_result;
    YanSearchTermsResult clear_result;
    YanSearchTermsStatus status_value;
    YanSearchTermsSummary summary;
    uint32_t match_count;
    uint32_t current;
    FakeMatch matches[FAKE_MATCH_MAX];
    uint32_t query_seen_length;
    uint8_t query_seen[1024];
    uint32_t read_fail_at;
    YanSearchTermsResult read_fail_code;
    uint32_t chunk_cap;
} FakeTerms;

static FakeTerms fake;

/* A reentrant call driven from inside a borrowed callback. */
static bool reenter_armed;
static bool reenter_ran;
static YanShell *reenter_shell;
static const uint8_t *reenter_line;
static uint32_t reenter_length;
static YanShellResult reenter_result;
static uint32_t reenter_output_delta;

/* ---------------------------------------------------------------- output -- */

static bool capture_putc(void *context, uint8_t byte)
{
    Capture *self = (Capture *)context;
    ++self->calls;
    if (self->fail_at != 0u && self->calls == self->fail_at) {
        self->fail_seen = true;
        return false;
    }
    if (self->length < CAPTURE_CAPACITY - 1u) {
        self->bytes[self->length] = byte;
        ++self->length;
        self->bytes[self->length] = 0u;
    }
    return true;
}

static void reset_capture(void)
{
    capture.length = 0u;
    capture.calls = 0u;
    capture.fail_at = 0u;
    capture.fail_seen = false;
}

static void expect_output(const char *text)
{
    uint32_t length = (uint32_t)strlen(text);
    TEST_ASSERT_EQUAL_UINT32(length, capture.length);
    if (length > 0u) {
        TEST_ASSERT_EQUAL_MEMORY(text, capture.bytes, length);
    }
}

/* Same comparison, with a caller-supplied marker so the mutation gate can
 * require the specific assertion rather than any failure of the test. */
static void expect_output_message(const char *text, const char *message)
{
    uint32_t length = (uint32_t)strlen(text);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(length, capture.length, message);
    if (length > 0u) {
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(text, capture.bytes, length, message);
    }
}

static void assert_no_ok_in_capture(void)
{
    TEST_ASSERT_NULL_MESSAGE(strstr((const char *)capture.bytes, "OK"),
                             "a fatal command must not print an OK line");
}

/* ---------------------------------------------------------------- device -- */

static YanFsIoResult device_capacity(void *context, uint64_t *blocks)
{
    (void)context;
    *blocks = TERM_BLOCKS;
    return YAN_FS_IO_OK;
}

static YanFsIoResult device_read(void *context, uint32_t lba,
                                 uint8_t out[4096])
{
    TermDevice *self = (TermDevice *)context;
    ++self->reads;
    if (self->read_fail_at != 0u && self->reads == self->read_fail_at) {
        return self->read_fail_code != YAN_FS_IO_OK ? self->read_fail_code
                                                    : YAN_FS_IO_ERROR;
    }
    if (lba >= TERM_BLOCKS) {
        return YAN_FS_IO_ERROR;
    }
    memcpy(out, self->blocks[lba], YAN_FS_BLOCK_SIZE);
    return YAN_FS_IO_OK;
}

static YanFsIoResult device_write(void *context, uint32_t lba,
                                  const uint8_t data[4096])
{
    TermDevice *self = (TermDevice *)context;
    ++self->writes;
    if (lba >= TERM_BLOCKS) {
        return YAN_FS_IO_ERROR;
    }
    memcpy(self->blocks[lba], data, YAN_FS_BLOCK_SIZE);
    return YAN_FS_IO_OK;
}

/* ------------------------------------------------------ scripted backend -- */

static void run_reenter(void)
{
    if (!reenter_armed) {
        return;
    }
    reenter_armed = false;
    reenter_ran = true;
    uint32_t before = capture.length;
    reenter_result =
        yan_shell_execute(reenter_shell, reenter_line, reenter_length);
    reenter_output_delta = capture.length - before;
}

static YanSearchTermsResult fake_read_match(void *context, uint32_t offset,
                                            uint32_t capacity,
                                            const uint8_t **bytes,
                                            uint32_t *length)
{
    FakeTerms *self = (FakeTerms *)context;
    ++self->read_calls;
    run_reenter();
    if (self->read_fail_at != 0u && self->read_calls == self->read_fail_at) {
        return self->read_fail_code != YAN_SEARCH_TERMS_OK
                   ? self->read_fail_code
                   : YAN_SEARCH_TERMS_IO;
    }
    if (self->read_result != YAN_SEARCH_TERMS_OK) {
        return self->read_result;
    }
    const uint8_t *snippet = self->matches[self->current].snippet;
    uint32_t snippet_length = self->matches[self->current].snippet_length;
    if (offset >= snippet_length) {
        *bytes = NULL;
        *length = 0u;
        return YAN_SEARCH_TERMS_OK;
    }
    uint32_t get = snippet_length - offset;
    if (get > capacity) {
        get = capacity;
    }
    if (self->chunk_cap != 0u && get > self->chunk_cap) {
        get = self->chunk_cap;
    }
    *bytes = snippet + offset;
    *length = get;
    return YAN_SEARCH_TERMS_OK;
}

static YanSearchTermsResult fake_query(void *context, const uint8_t *query,
                                       uint32_t query_length,
                                       YanSearchTermsMatchFn match,
                                       void *match_context,
                                       YanSearchTermsSummary *summary)
{
    FakeTerms *self = (FakeTerms *)context;
    ++self->query_calls;
    if (query_length <= sizeof self->query_seen) {
        memcpy(self->query_seen, query, query_length);
        self->query_seen_length = query_length;
    }
    if (self->query_result != YAN_SEARCH_TERMS_OK) {
        return self->query_result;
    }
    for (uint32_t i = 0u; i < self->match_count; ++i) {
        self->current = i;
        YanSearchTermsMatch value;
        value.name = self->matches[i].name;
        value.line_number = self->matches[i].line;
        value.score = self->matches[i].score;
        value.snippet_length = self->matches[i].snippet_length;
        value.left_truncated = self->matches[i].left;
        value.right_truncated = self->matches[i].right;
        if (!match(match_context, &value)) {
            return YAN_SEARCH_TERMS_OK;
        }
    }
    *summary = self->summary;
    return YAN_SEARCH_TERMS_OK;
}

static YanSearchTermsResult fake_status(void *context,
                                        YanSearchTermsStatus *out)
{
    FakeTerms *self = (FakeTerms *)context;
    ++self->status_calls;
    if (self->status_result != YAN_SEARCH_TERMS_OK) {
        return self->status_result;
    }
    *out = self->status_value;
    return YAN_SEARCH_TERMS_OK;
}

static YanSearchTermsResult fake_rebuild(void *context)
{
    FakeTerms *self = (FakeTerms *)context;
    ++self->rebuild_calls;
    return self->rebuild_result;
}

static YanSearchTermsResult fake_clear(void *context)
{
    FakeTerms *self = (FakeTerms *)context;
    ++self->clear_calls;
    return self->clear_result;
}

static YanSearchTermsBackend fake_backend(void)
{
    YanSearchTermsBackend backend;
    backend.context = &fake;
    backend.context_size = sizeof fake;
    backend.source_context = fake_source;
    backend.source_size = sizeof fake_source;
    backend.query = fake_query;
    backend.read_match = fake_read_match;
    backend.status = fake_status;
    backend.rebuild = fake_rebuild;
    backend.clear = fake_clear;
    return backend;
}

/* -------------------------------------------------------------- fixtures -- */

static void attach_shell(void)
{
    YanShellOutput output;
    output.context = &capture;
    output.putc = capture_putc;
    TEST_ASSERT_EQUAL_INT(
        YAN_SHELL_OK,
        yan_shell_init(&shell, &fs, &literal, &term_facade, output));
}

static void fixture_base(void)
{
    memset(&device, 0, sizeof device);
    memset(&capture, 0, sizeof capture);
    memset(&fs, 0, sizeof fs);
    memset(&literal_linear, 0, sizeof literal_linear);
    memset(&literal, 0, sizeof literal);
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_OK, yan_fs_format_metadata(device.blocks[0], TERM_BLOCKS));
    YanFsBlockIo io;
    io.context = &device;
    io.capacity = device_capacity;
    io.read_block = device_read;
    io.write_block = device_write;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_init(&fs, io));
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_mount(&fs));
    TEST_ASSERT_EQUAL_INT(
        YAN_SEARCH_OK, yan_search_linear_init(&literal_linear, &fs));
    TEST_ASSERT_EQUAL_INT(
        YAN_SEARCH_OK,
        yan_search_init(&literal, yan_search_linear_backend(&literal_linear)));
}

static void attach_real_terms(void)
{
    memset(&term_index, 0, sizeof term_index);
    memset(&term_facade, 0, sizeof term_facade);
    TEST_ASSERT_EQUAL_INT(
        YAN_SEARCH_TERMS_OK, yan_search_terms_index_init(&term_index, &fs));
    TEST_ASSERT_EQUAL_INT(
        YAN_SEARCH_TERMS_OK,
        yan_search_terms_init(&term_facade,
                              yan_search_terms_index_backend(&term_index)));
    attach_shell();
}

static void attach_fake_terms(void)
{
    memset(&fake, 0, sizeof fake);
    memset(&term_facade, 0, sizeof term_facade);
    fake.status_value.state = YAN_SEARCH_INDEX_READY;
    fake.status_value.source = YAN_SEARCH_SOURCE_MOUNTED;
    fake.summary.mode = YAN_SEARCH_MODE_INDEX;
    TEST_ASSERT_EQUAL_INT(
        YAN_SEARCH_TERMS_OK, yan_search_terms_init(&term_facade,
                                                   fake_backend()));
    attach_shell();
}

void setUp(void)
{
    reenter_armed = false;
    reenter_ran = false;
    reenter_shell = NULL;
    reenter_line = NULL;
    reenter_length = 0u;
    reenter_result = YAN_SHELL_OK;
    reenter_output_delta = 0u;
    fixture_base();
    attach_real_terms();
}

void tearDown(void)
{
}

/* --------------------------------------------------------------- helpers -- */

static YanShellResult execute_bytes(const uint8_t *line, uint32_t length)
{
    return yan_shell_execute(&shell, line, length);
}

static YanShellResult execute_text(const char *text)
{
    return execute_bytes((const uint8_t *)text, (uint32_t)strlen(text));
}

static void seed_file(const char *name, const uint8_t *bytes, uint32_t length)
{
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_create(&fs, name, bytes, length));
}

/* ============================================ real index backend behaviour */

static void search_first_query_builds_ready_index(void)
{
    seed_file("a.md", (const uint8_t *)"interrupt here\n", 15u);
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("search interrupt"));
    expect_output("a.md:1:interrupt here\r\n"
                  "OK search total=1 shown=1 skipped=0 mode=index\r\n");

    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("index status"));
    expect_output("INDEX state=READY source=MOUNTED terms=2 postings=2\r\n"
                  "OK index\r\n");

    uint32_t first_reads = device.reads;
    device.reads = 0u;
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("search interrupt"));
    expect_output("a.md:1:interrupt here\r\n"
                  "OK search total=1 shown=1 skipped=0 mode=index\r\n");
    TEST_ASSERT_TRUE_MESSAGE(
        device.reads < first_reads,
        "a READY second query must only read the selected snippet");
}

static void search_zero_results_still_prints_the_summary(void)
{
    seed_file("a.md", (const uint8_t *)"interrupt here\n", 15u);
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("search ghost"));
    expect_output("OK search total=0 shown=0 skipped=0 mode=index\r\n");
}

static void index_status_and_clear_do_no_io_and_clear_drops_ready(void)
{
    seed_file("a.md", (const uint8_t *)"interrupt here\n", 15u);
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("search interrupt"));
    uint32_t reads = device.reads;
    uint32_t writes = device.writes;

    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("index status"));
    expect_output("INDEX state=READY source=MOUNTED terms=2 postings=2\r\n"
                  "OK index\r\n");
    TEST_ASSERT_EQUAL_UINT32(reads, device.reads);
    TEST_ASSERT_EQUAL_UINT32(writes, device.writes);

    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("index clear"));
    expect_output("OK index\r\n");
    TEST_ASSERT_EQUAL_UINT32(reads, device.reads);
    TEST_ASSERT_EQUAL_UINT32(writes, device.writes);

    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("index status"));
    expect_output_message(
        "INDEX state=EMPTY source=MOUNTED terms=0 postings=0\r\n" "OK index\r\n",
        "clear must drop the READY cache");
}

static void rebuild_reports_ok_then_capacity_is_index_limit(void)
{
    seed_file("a.md", (const uint8_t *)"interrupt here\n", 15u);
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("rebuild"));
    expect_output("OK rebuild\r\n");

    static uint8_t bad[300];
    memset(bad, (int)'a', 256u);
    bad[256] = (uint8_t)'\n';
    memcpy(bad + 257, "hit\n", 4u);
    seed_file("big.md", bad, 261u);
    reset_capture();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, execute_text("rebuild"),
                                  "INDEX_LIMIT is an ordinary error");
    expect_output("ERROR INDEX_LIMIT\r\n");

    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("search hit"));
    expect_output("big.md:2:hit\r\n"
                  "OK search total=1 shown=1 skipped=0 mode=scan\r\n");
    TEST_ASSERT_NULL(strstr((const char *)capture.bytes, "INDEX_LIMIT"));
}

static void search_and_grep_keep_different_semantics(void)
{
    static const uint8_t source[] =
        "interruption pending\ninterrupt here\n";
    seed_file("a.md", source, (uint32_t)(sizeof source - 1u));
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("grep interrupt"));
    expect_output("a.md:1:interruption pending\r\n"
                  "a.md:2:interrupt here\r\n"
                  "OK grep\r\n");

    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("search interrupt"));
    expect_output("a.md:2:interrupt here\r\n"
                  "OK search total=1 shown=1 skipped=0 mode=index\r\n");
}

static void search_skips_nul_and_invalid_files(void)
{
    seed_file("good.md", (const uint8_t *)"hit\n", 4u);
    seed_file("bad.md", (const uint8_t *)"h\x00it\n", 5u);
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("search hit"));
    expect_output("good.md:1:hit\r\n"
                  "OK search total=1 shown=1 skipped=1 mode=index\r\n");
}

static void index_management_survives_an_unavailable_source(void)
{
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_unmount(&fs));
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("index status"));
    expect_output("INDEX state=UNAVAILABLE source=UNMOUNTED terms=0 postings=0\r\n"
                  "OK index\r\n");
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("index clear"));
    expect_output("OK index\r\n");

    reset_capture();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_FATAL, execute_text("search hit"),
                                  "a non-management command keeps health priority");
    expect_output("ERROR NOT_MOUNTED\r\n");
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_FATAL, execute_text("grep hit"));
    expect_output("ERROR NOT_MOUNTED\r\n");
}

/* ================================================= scripted backend grammar */

static void search_grammar_is_usage_without_io(void)
{
    static const char *cases[] = {
        "search",          "search     ",  "search -x",  "search a\"b",
        "search \"a\"x",   "search \"\"",  "search \"  \"",
    };
    attach_fake_terms();
    device.reads = 0u;
    device.writes = 0u;
    for (uint32_t i = 0u; i < sizeof cases / sizeof cases[0]; ++i) {
        reset_capture();
        TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_OK, execute_text(cases[i]),
                                      cases[i]);
        expect_output_message("ERROR USAGE\r\n",
                              "invalid search grammar must print USAGE");
    }
    TEST_ASSERT_EQUAL_UINT32(0u, device.reads);
    TEST_ASSERT_EQUAL_UINT32(0u, device.writes);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, fake.query_calls,
        "an invalid query must not reach the term backend");
}

static void search_outer_quotes_preserve_internal_spaces(void)
{
    attach_fake_terms();
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK,
                          execute_text("search   \"interrupt PLIC\"   "));
    expect_output("OK search total=0 shown=0 skipped=0 mode=index\r\n");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        14u, fake.query_seen_length,
        "the outer quotes must be stripped and the interior preserved");
    TEST_ASSERT_EQUAL_MEMORY("interrupt PLIC", fake.query_seen, 14u);
}

static void search_rows_use_left_and_right_ellipsis(void)
{
    attach_fake_terms();
    static const uint8_t hello[] = "hello";
    static const uint8_t world[] = "world";
    fake.match_count = 2u;
    fake.matches[0].name = "a.md";
    fake.matches[0].line = 2u;
    fake.matches[0].score = 3u;
    fake.matches[0].left = true;
    fake.matches[0].snippet = hello;
    fake.matches[0].snippet_length = 5u;
    fake.matches[1].name = "b.md";
    fake.matches[1].line = 1u;
    fake.matches[1].score = 1u;
    fake.matches[1].right = true;
    fake.matches[1].snippet = world;
    fake.matches[1].snippet_length = 5u;
    fake.summary.total = 2u;
    fake.summary.shown = 2u;
    fake.summary.skipped = 0u;
    fake.summary.mode = YAN_SEARCH_MODE_INDEX;
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("search x"));
    expect_output("a.md:2:...hello\r\n"
                  "b.md:1:world...\r\n"
                  "OK search total=2 shown=2 skipped=0 mode=index\r\n");
}

static void search_escapes_control_and_invalid_utf8_snippet(void)
{
    attach_fake_terms();
    static const uint8_t raw[] = {'\r', '\t', 0x1Bu, 0x00u, 0xFFu};
    fake.match_count = 1u;
    fake.matches[0].name = "a.md";
    fake.matches[0].line = 1u;
    fake.matches[0].score = 1u;
    fake.matches[0].snippet = raw;
    fake.matches[0].snippet_length = sizeof raw;
    fake.summary.total = 1u;
    fake.summary.shown = 1u;
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("search x"));
    expect_output("a.md:1:\\x0D\\x09\\x1B\\x00\\xFF\r\n"
                  "OK search total=1 shown=1 skipped=0 mode=index\r\n");
}

static void search_splits_a_utf8_scalar_across_reads(void)
{
    attach_fake_terms();
    static const uint8_t raw[] = {0xE4u, 0xB8u, 0xADu, 'x'};
    fake.match_count = 1u;
    fake.matches[0].name = "a.md";
    fake.matches[0].line = 1u;
    fake.matches[0].score = 1u;
    fake.matches[0].snippet = raw;
    fake.matches[0].snippet_length = sizeof raw;
    fake.chunk_cap = 1u; /* one byte per read */
    fake.summary.total = 1u;
    fake.summary.shown = 1u;
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("search x"));
    expect_output("a.md:1:\xE4\xB8\xADx\r\n"
                  "OK search total=1 shown=1 skipped=0 mode=index\r\n");
}

static void search_summary_prints_a_uint64_total(void)
{
    attach_fake_terms();
    static const uint8_t hit[] = "hit";
    fake.match_count = 1u;
    fake.matches[0].name = "a.md";
    fake.matches[0].line = 1u;
    fake.matches[0].score = 1u;
    fake.matches[0].snippet = hit;
    fake.matches[0].snippet_length = 3u;
    fake.summary.total = 1234567890123ull;
    fake.summary.shown = 1u;
    fake.summary.skipped = 7u;
    fake.summary.mode = YAN_SEARCH_MODE_SCAN;
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("search x"));
    expect_output_message(
        "a.md:1:hit\r\n"
        "OK search total=1234567890123 shown=1 skipped=7 mode=scan\r\n",
        "the uint64 total must print exactly");
    fake.summary.total = UINT64_MAX;
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("search x"));
    expect_output("a.md:1:hit\r\n"
                  "OK search total=18446744073709551615 shown=1 skipped=7 mode=scan\r\n");
}

static void search_source_error_maps_to_the_existing_error_line(void)
{
    static const struct {
        YanSearchTermsResult result;
        const char *line;
        YanShellResult verdict;
    } cases[] = {
        {YAN_SEARCH_TERMS_IO, "ERROR IO\r\n", YAN_SHELL_FATAL},
        {YAN_SEARCH_TERMS_FAULTED, "ERROR FAULTED\r\n", YAN_SHELL_FATAL},
        {YAN_SEARCH_TERMS_NOT_MOUNTED, "ERROR NOT_MOUNTED\r\n", YAN_SHELL_FATAL},
        {YAN_SEARCH_TERMS_BUSY, "ERROR BUSY\r\n", YAN_SHELL_OK},
        {YAN_SEARCH_TERMS_CORRUPT, "ERROR CORRUPT\r\n", YAN_SHELL_FATAL},
    };
    attach_fake_terms();
    for (uint32_t i = 0u; i < sizeof cases / sizeof cases[0]; ++i) {
        fake.query_result = cases[i].result;
        reset_capture();
        TEST_ASSERT_EQUAL_INT(cases[i].verdict, execute_text("search x"));
        expect_output(cases[i].line);
        if (cases[i].verdict == YAN_SHELL_FATAL) {
            assert_no_ok_in_capture();
        }
    }
}

static void search_reader_fault_is_sticky_fatal_without_ok(void)
{
    attach_fake_terms();
    static const uint8_t hello[] = "hello";
    fake.match_count = 1u;
    fake.matches[0].name = "a.md";
    fake.matches[0].line = 1u;
    fake.matches[0].score = 1u;
    fake.matches[0].snippet = hello;
    fake.matches[0].snippet_length = 5u;
    fake.read_fail_at = 1u;
    fake.read_fail_code = YAN_SEARCH_TERMS_IO;
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_FATAL, execute_text("search x"));
    expect_output("a.md:1:\r\nERROR IO\r\n");
    assert_no_ok_in_capture();
}

static void index_status_unknown_enum_is_protocol_fatal(void)
{
    attach_fake_terms();
    fake.status_value.state = (YanSearchTermsIndexState)99;
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_FATAL, execute_text("index status"));
    expect_output("ERROR PROTOCOL\r\n");
    assert_no_ok_in_capture();

    fake.status_value.state = YAN_SEARCH_INDEX_READY;
    fake.status_result = (YanSearchTermsResult)99;
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_FATAL, execute_text("index status"));
    expect_output("ERROR PROTOCOL\r\n");
    assert_no_ok_in_capture();
}

static void rebuild_and_clear_map_their_errors(void)
{
    attach_fake_terms();
    fake.rebuild_result = YAN_SEARCH_TERMS_INDEX_LIMIT;
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("rebuild"));
    expect_output("ERROR INDEX_LIMIT\r\n");

    fake.rebuild_result = YAN_SEARCH_TERMS_IO;
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_FATAL, execute_text("rebuild"));
    expect_output("ERROR IO\r\n");

    fake.rebuild_result = YAN_SEARCH_TERMS_OK;
    fake.clear_result = YAN_SEARCH_TERMS_BUSY;
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("index clear"));
    expect_output("ERROR BUSY\r\n");

    fake.clear_result = YAN_SEARCH_TERMS_FAULTED;
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_FATAL, execute_text("index clear"));
    expect_output("ERROR FAULTED\r\n");
}

static void index_grammar_usage_and_spacing(void)
{
    attach_fake_terms();
    static const char *bad[] = {"index", "index foo", "index status extra",
                                "index clear extra"};
    for (uint32_t i = 0u; i < sizeof bad / sizeof bad[0]; ++i) {
        reset_capture();
        TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text(bad[i]));
        expect_output("ERROR USAGE\r\n");
    }
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("index   status"));
    expect_output("INDEX state=READY source=MOUNTED terms=0 postings=0\r\n"
                  "OK index\r\n");
    TEST_ASSERT_EQUAL_UINT32(1u, fake.status_calls);
}

static void rebuild_rejects_an_argument(void)
{
    attach_fake_terms();
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("rebuild x"));
    expect_output("ERROR USAGE\r\n");
    TEST_ASSERT_EQUAL_UINT32(0u, fake.rebuild_calls);

    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("rebuild   "));
    expect_output("OK rebuild\r\n");
    TEST_ASSERT_EQUAL_UINT32(1u, fake.rebuild_calls);
}

static void search_output_failure_first_middle_last_is_fatal(void)
{
    attach_fake_terms();
    static const uint8_t hello[] = "hello";
    fake.match_count = 1u;
    fake.matches[0].name = "a.md";
    fake.matches[0].line = 1u;
    fake.matches[0].score = 1u;
    fake.matches[0].snippet = hello;
    fake.matches[0].snippet_length = 5u;
    fake.summary.total = 1u;
    fake.summary.shown = 1u;
    const char *expected =
        "a.md:1:hello\r\nOK search total=1 shown=1 skipped=0 mode=index\r\n";
    uint32_t total = (uint32_t)strlen(expected);
    uint32_t positions[] = {1u, total / 2u, total};
    for (uint32_t i = 0u; i < sizeof positions / sizeof positions[0]; ++i) {
        reset_capture();
        capture.fail_at = positions[i];
        TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_FATAL, execute_text("search x"),
                                      "a refused byte is always fatal");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(
            positions[i] - 1u, capture.length,
            "no byte may be emitted after the first refused one");
    }
}

static void search_reentry_from_the_reader_is_busy(void)
{
    attach_fake_terms();
    static const uint8_t hello[] = "hello";
    fake.match_count = 1u;
    fake.matches[0].name = "a.md";
    fake.matches[0].line = 1u;
    fake.matches[0].score = 1u;
    fake.matches[0].snippet = hello;
    fake.matches[0].snippet_length = 5u;
    fake.summary.total = 1u;
    fake.summary.shown = 1u;
    reenter_armed = true;
    reenter_shell = &shell;
    reenter_line = (const uint8_t *)"index status";
    reenter_length = 12u;
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("search x"));
    TEST_ASSERT_TRUE_MESSAGE(reenter_ran, "the reader probe must have run");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_BUSY, reenter_result,
                                  "a nested call must answer BUSY");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, reenter_output_delta,
                                     "a BUSY nested call must emit nothing");
}

static void span_alias_guard_covers_the_term_facade(void)
{
    attach_fake_terms();
    YanShell local;
    YanSearchTerms forged;
    static uint8_t before[sizeof(YanShell)];
    memset(&local, 0, sizeof local);
    memcpy(before, &local, sizeof before);
    memset(&forged, 0, sizeof forged);
    forged.initialized = true;
    forged.backend.context = &local;
    forged.backend.context_size = sizeof local;
    YanShellOutput output;
    output.context = &capture;
    output.putc = capture_putc;
    reset_capture();
    TEST_ASSERT_EQUAL_INT(
        YAN_SHELL_INVALID,
        yan_shell_init(&local, &fs, &literal, &forged, output));
    TEST_ASSERT_EQUAL_MEMORY(before, &local, sizeof before);

    YanSearchTerms forged_source;
    memset(&forged_source, 0, sizeof forged_source);
    forged_source.initialized = true;
    forged_source.backend.source_context = &local;
    forged_source.backend.source_size = sizeof local;
    TEST_ASSERT_EQUAL_INT(
        YAN_SHELL_INVALID,
        yan_shell_init(&local, &fs, &literal, &forged_source, output));
    TEST_ASSERT_EQUAL_MEMORY(before, &local, sizeof before);
    TEST_ASSERT_EQUAL_UINT32(0u, capture.calls);
}

static void execute_rejects_a_line_that_aliases_the_term_facade(void)
{
    attach_fake_terms();
    reset_capture();
    TEST_ASSERT_EQUAL_INT(
        YAN_SHELL_INVALID,
        yan_shell_execute(&shell, (const uint8_t *)&term_facade, 8u));
    TEST_ASSERT_EQUAL_INT(
        YAN_SHELL_INVALID,
        yan_shell_execute(&shell, (const uint8_t *)&fake, 8u));
    TEST_ASSERT_EQUAL_INT(
        YAN_SHELL_INVALID,
        yan_shell_execute(&shell, fake_source, 8u));
    expect_output("");
}

/* Dummy literal vtable entries for a forged foreign source; the shell only
 * reads the span fields, never the function pointers. */
static YanSearchResult forged_literal_query(void *context, const uint8_t *pattern,
                                            uint32_t pattern_length,
                                            YanSearchMatchFn match,
                                            void *match_context)
{
    (void)context;
    (void)pattern;
    (void)pattern_length;
    (void)match;
    (void)match_context;
    return YAN_SEARCH_OK;
}

static YanSearchResult forged_literal_read(void *context, uint32_t offset,
                                           uint32_t capacity,
                                           const uint8_t **bytes,
                                           uint32_t *length)
{
    (void)context;
    (void)offset;
    (void)capacity;
    (void)bytes;
    (void)length;
    return YAN_SEARCH_OK;
}

/* REVIEW04 F1: a literal backend source crossing the term writable context
 * must be rejected, byte-identical and with no output or I/O. */
static void init_rejects_a_literal_source_crossing_the_term_context(void)
{
    attach_fake_terms(); /* the shell's term context is &fake */
    static uint8_t literal_context[8];
    YanShell local;
    YanSearch forged;
    static uint8_t before[sizeof(YanShell)];
    memset(&local, 0, sizeof local);
    memcpy(before, &local, sizeof before);
    memset(&forged, 0, sizeof forged);
    forged.initialized = true;
    forged.backend.context = literal_context;
    forged.backend.context_size = sizeof literal_context;
    forged.backend.source_context = &fake; /* crosses the term context */
    forged.backend.source_size = sizeof fake;
    forged.backend.query = forged_literal_query;
    forged.backend.read_match = forged_literal_read;
    YanShellOutput output;
    output.context = &capture;
    output.putc = capture_putc;
    device.reads = 0u;
    device.writes = 0u;
    reset_capture();
    TEST_ASSERT_EQUAL_INT(
        YAN_SHELL_INVALID,
        yan_shell_init(&local, &fs, &forged, &term_facade, output));
    TEST_ASSERT_EQUAL_MEMORY(before, &local, sizeof before);
    expect_output("");
    TEST_ASSERT_EQUAL_UINT32(0u, device.reads);
    TEST_ASSERT_EQUAL_UINT32(0u, device.writes);
}

/* REVIEW04 F1 reverse: a term source crossing the literal writable context must
 * also be rejected. The real literal facade's context is literal_linear. */
static void init_rejects_a_term_source_crossing_the_literal_context(void)
{
    static uint8_t term_context[8];
    YanShell local;
    YanSearchTerms forged;
    static uint8_t before[sizeof(YanShell)];
    memset(&local, 0, sizeof local);
    memcpy(before, &local, sizeof before);
    memset(&forged, 0, sizeof forged);
    forged.initialized = true;
    forged.backend.context = term_context;
    forged.backend.context_size = sizeof term_context;
    forged.backend.source_context = &literal_linear; /* crosses literal ctx */
    forged.backend.source_size = sizeof literal_linear;
    forged.backend.query = fake_query;
    forged.backend.read_match = fake_read_match;
    YanShellOutput output;
    output.context = &capture;
    output.putc = capture_putc;
    device.reads = 0u;
    device.writes = 0u;
    reset_capture();
    TEST_ASSERT_EQUAL_INT(
        YAN_SHELL_INVALID,
        yan_shell_init(&local, &fs, &literal, &forged, output));
    TEST_ASSERT_EQUAL_MEMORY(before, &local, sizeof before);
    expect_output("");
    TEST_ASSERT_EQUAL_UINT32(0u, device.reads);
    TEST_ASSERT_EQUAL_UINT32(0u, device.writes);
}

/* REVIEW04 F3: a busy backend status is the ordinary ERROR BUSY, not a
 * pseudo-zero result, and touches no disk. */
static void index_status_busy_is_an_ordinary_error(void)
{
    attach_fake_terms();
    fake.status_result = YAN_SEARCH_TERMS_BUSY;
    device.reads = 0u;
    device.writes = 0u;
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("index status"));
    expect_output("ERROR BUSY\r\n");
    TEST_ASSERT_EQUAL_UINT32(0u, device.reads);
    TEST_ASSERT_EQUAL_UINT32(0u, device.writes);
}

/* REVIEW04 F4: on an unavailable source, every non-management line keeps the
 * existing health priority, while the valid management lines stay 0-I/O. */
static void unavailable_source_keeps_health_priority(void)
{
    attach_fake_terms();
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_unmount(&fs));
    static const char *fatal_cases[] = {
        "index foo", "index status extra", "index clear extra", "rebuild extra",
        "rebuild   ", "", "exit",
    };
    for (uint32_t i = 0u; i < sizeof fatal_cases / sizeof fatal_cases[0]; ++i) {
        reset_capture();
        TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_FATAL,
                                      execute_text(fatal_cases[i]),
                                      fatal_cases[i]);
        expect_output("ERROR NOT_MOUNTED\r\n");
    }
    device.reads = 0u;
    device.writes = 0u;
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("index status"));
    expect_output("INDEX state=READY source=MOUNTED terms=0 postings=0\r\n"
                  "OK index\r\n");
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("index clear"));
    expect_output("OK index\r\n");
    TEST_ASSERT_EQUAL_UINT32(0u, device.reads);
    TEST_ASSERT_EQUAL_UINT32(0u, device.writes);
}

/* REVIEW04 F5: an unknown source class is PROTOCOL, never a fake state. */
static void index_status_unknown_source_is_protocol(void)
{
    attach_fake_terms();
    fake.status_value.source = (YanSearchTermsSourceState)99;
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_FATAL, execute_text("index status"));
    expect_output("ERROR PROTOCOL\r\n");
    assert_no_ok_in_capture();
}

/* REVIEW04 F5: a real faulted source still answers pure management with 0 I/O
 * and accurate class/counts, and is never recovered. */
static void faulted_source_management_is_zero_io(void)
{
    attach_real_terms();
    seed_file("a.md", (const uint8_t *)"hit\n", 4u);
    device.reads = 0u;
    device.read_fail_at = 1u;
    device.read_fail_code = YAN_FS_IO_ERROR;
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_FATAL, execute_text("search hit"));
    expect_output("ERROR IO\r\n");
    device.read_fail_at = 0u;

    uint32_t reads = device.reads;
    uint32_t writes = device.writes;
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("index clear"));
    expect_output("OK index\r\n");
    TEST_ASSERT_EQUAL_UINT32(reads, device.reads);
    TEST_ASSERT_EQUAL_UINT32(writes, device.writes);

    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("index status"));
    expect_output("INDEX state=UNAVAILABLE source=FAULTED terms=0 postings=0\r\n"
                  "OK index\r\n");
    TEST_ASSERT_EQUAL_UINT32(reads, device.reads);
    TEST_ASSERT_EQUAL_UINT32(writes, device.writes);
}

/* REVIEW04 F5: a stopped query is the ordinary ERROR STOPPED, with no summary. */
static void search_stopped_is_an_ordinary_error(void)
{
    attach_fake_terms();
    fake.query_result = YAN_SEARCH_TERMS_STOPPED;
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("search x"));
    expect_output("ERROR STOPPED\r\n");
    assert_no_ok_in_capture();
}

/* REVIEW04 F5: a backend read result outside the enum is latched as PROTOCOL
 * by the facade and stays fatal, with no OK line. */
static void search_reader_unknown_result_is_protocol(void)
{
    attach_fake_terms();
    static const uint8_t hit[] = "hit";
    fake.match_count = 1u;
    fake.matches[0].name = "a.md";
    fake.matches[0].line = 1u;
    fake.matches[0].score = 1u;
    fake.matches[0].snippet = hit;
    fake.matches[0].snippet_length = 3u;
    fake.read_result = (YanSearchTermsResult)99;
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_FATAL, execute_text("search x"));
    expect_output("a.md:1:\r\nERROR PROTOCOL\r\n");
    assert_no_ok_in_capture();
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(search_first_query_builds_ready_index);
    RUN_TEST(search_zero_results_still_prints_the_summary);
    RUN_TEST(index_status_and_clear_do_no_io_and_clear_drops_ready);
    RUN_TEST(rebuild_reports_ok_then_capacity_is_index_limit);
    RUN_TEST(search_and_grep_keep_different_semantics);
    RUN_TEST(search_skips_nul_and_invalid_files);
    RUN_TEST(index_management_survives_an_unavailable_source);
    RUN_TEST(search_grammar_is_usage_without_io);
    RUN_TEST(search_outer_quotes_preserve_internal_spaces);
    RUN_TEST(search_rows_use_left_and_right_ellipsis);
    RUN_TEST(search_escapes_control_and_invalid_utf8_snippet);
    RUN_TEST(search_splits_a_utf8_scalar_across_reads);
    RUN_TEST(search_summary_prints_a_uint64_total);
    RUN_TEST(search_source_error_maps_to_the_existing_error_line);
    RUN_TEST(search_reader_fault_is_sticky_fatal_without_ok);
    RUN_TEST(index_status_unknown_enum_is_protocol_fatal);
    RUN_TEST(rebuild_and_clear_map_their_errors);
    RUN_TEST(index_grammar_usage_and_spacing);
    RUN_TEST(rebuild_rejects_an_argument);
    RUN_TEST(search_output_failure_first_middle_last_is_fatal);
    RUN_TEST(search_reentry_from_the_reader_is_busy);
    RUN_TEST(span_alias_guard_covers_the_term_facade);
    RUN_TEST(execute_rejects_a_line_that_aliases_the_term_facade);
    RUN_TEST(init_rejects_a_literal_source_crossing_the_term_context);
    RUN_TEST(init_rejects_a_term_source_crossing_the_literal_context);
    RUN_TEST(index_status_busy_is_an_ordinary_error);
    RUN_TEST(unavailable_source_keeps_health_priority);
    RUN_TEST(index_status_unknown_source_is_protocol);
    RUN_TEST(faulted_source_management_is_zero_io);
    RUN_TEST(search_stopped_is_an_ordinary_error);
    RUN_TEST(search_reader_unknown_result_is_protocol);
    return UNITY_END();
}
