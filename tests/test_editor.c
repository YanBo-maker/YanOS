#include "editor.h"
#include "unity.h"

#include <stddef.h>
#include <string.h>

/* Native tests for the 0023 multiline editor core (os/editor.c).
 *
 * The filesystem under test is the real os/yanfs.c over an in-memory block
 * device borrowed from the shell suite's pattern: 16 blocks, injected read and
 * write faults, and call counters. The test does not re-implement the on-disk
 * format as an oracle; it seeds files through yan_fs_create and checks the
 * editor's own draft, the exact output bytes, and the file read back through
 * yan_fs_read. The block callbacks also count calls so "p" and "q" can be held
 * to making none.
 *
 * Each test owns one rule of the 0023 contract: terminators, blank and empty
 * lines, the last-line special cases, the capacity boundary with an atomic
 * refusal, UTF-8 and C1 rejection, the incomplete-load rule, the fixed
 * create/replace split, ordinary-vs-fatal errors, alias/BUSY/range checks and
 * output failures before and after a commit. */

#define TEST_DEVICE_BLOCKS 16u
#define CAPTURE_CAPACITY 131072u

typedef struct {
    uint8_t blocks[TEST_DEVICE_BLOCKS][YAN_FS_BLOCK_SIZE];
    uint64_t capacity_blocks;
    YanFsIoResult capacity_status;
    YanFsIoResult read_status;
    YanFsIoResult write_status;
    uint32_t reads;
    uint32_t writes;
    /* Fail the N-th call after the counters are reset (0 = never). */
    uint32_t read_fail_at;
    YanFsIoResult read_fail_code;
    /* Fail every read of this LBA (0xffffffff = never). */
    uint32_t read_fail_lba;
    uint32_t write_fail_at;
    YanFsIoResult write_fail_code;
} TestDevice;

static TestDevice device;
static YanFs fs;
static YanEditor editor;

typedef struct {
    uint8_t bytes[CAPTURE_CAPACITY];
    uint32_t length;
    uint32_t calls;
    /* Return false on this 1-based putc call (0 = never). */
    uint32_t fail_at;
    bool fail_seen;
    /* On this 1-based putc call, run one nested yan_editor_execute. */
    uint32_t reentry_at;
    YanEditor *reentry_editor;
    const uint8_t *reentry_line;
    uint32_t reentry_length;
    YanEditorResult reentry_result;
    bool reentry_done;
    uint32_t reentry_output_delta;
} Capture;

static Capture capture;

/* A reentrant call placed inside a block callback, to prove busy spans the
 * filesystem call as well as the output call. */
static bool io_probe_enabled;
static YanEditor *io_probe_editor;
static const uint8_t *io_probe_line;
static uint32_t io_probe_length;
static uint32_t io_probe_count;
static YanEditorResult io_probe_result;
static uint32_t io_probe_output;

static uint8_t medium_before[TEST_DEVICE_BLOCKS][YAN_FS_BLOCK_SIZE];

/* ---------------------------------------------------------------- utilities */

static bool capture_putc(void *context, uint8_t byte)
{
    Capture *self = (Capture *)context;
    ++self->calls;
    if (self->reentry_at != 0u && self->calls == self->reentry_at &&
        !self->reentry_done) {
        self->reentry_done = true;
        uint32_t before = self->length;
        self->reentry_result =
            yan_editor_execute(self->reentry_editor, self->reentry_line,
                               self->reentry_length);
        self->reentry_output_delta = self->length - before;
    }
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

static void run_io_probe(void)
{
    if (!io_probe_enabled) {
        return;
    }
    ++io_probe_count;
    uint32_t before = capture.length;
    io_probe_result =
        yan_editor_execute(io_probe_editor, io_probe_line, io_probe_length);
    io_probe_output = capture.length - before;
}

static YanFsIoResult device_capacity(void *context, uint64_t *blocks)
{
    TestDevice *self = (TestDevice *)context;
    if (self->capacity_status != YAN_FS_IO_OK) {
        return self->capacity_status;
    }
    *blocks = self->capacity_blocks;
    return YAN_FS_IO_OK;
}

static YanFsIoResult device_read(void *context, uint32_t lba, uint8_t out[4096])
{
    TestDevice *self = (TestDevice *)context;
    run_io_probe();
    ++self->reads;
    if (self->read_status != YAN_FS_IO_OK) {
        return self->read_status;
    }
    if (self->read_fail_at != 0u && self->reads == self->read_fail_at) {
        return self->read_fail_code != YAN_FS_IO_OK ? self->read_fail_code
                                                    : YAN_FS_IO_ERROR;
    }
    if (self->read_fail_lba != 0xffffffffu && lba == self->read_fail_lba) {
        return self->read_fail_code != YAN_FS_IO_OK ? self->read_fail_code
                                                    : YAN_FS_IO_ERROR;
    }
    if (lba >= TEST_DEVICE_BLOCKS) {
        return YAN_FS_IO_ERROR;
    }
    memcpy(out, self->blocks[lba], YAN_FS_BLOCK_SIZE);
    return YAN_FS_IO_OK;
}

static YanFsIoResult device_write(void *context, uint32_t lba,
                                  const uint8_t data[4096])
{
    TestDevice *self = (TestDevice *)context;
    run_io_probe();
    ++self->writes;
    if (self->write_status != YAN_FS_IO_OK) {
        return self->write_status;
    }
    if (self->write_fail_at != 0u && self->writes == self->write_fail_at) {
        return self->write_fail_code != YAN_FS_IO_OK ? self->write_fail_code
                                                     : YAN_FS_IO_ERROR;
    }
    if (lba >= TEST_DEVICE_BLOCKS) {
        return YAN_FS_IO_ERROR;
    }
    memcpy(self->blocks[lba], data, YAN_FS_BLOCK_SIZE);
    return YAN_FS_IO_OK;
}

static void reset_capture(void)
{
    capture.length = 0u;
    capture.calls = 0u;
    capture.fail_at = 0u;
    capture.fail_seen = false;
    capture.reentry_at = 0u;
    capture.reentry_editor = NULL;
    capture.reentry_line = NULL;
    capture.reentry_length = 0u;
    capture.reentry_result = YAN_EDITOR_OK;
    capture.reentry_done = false;
    capture.reentry_output_delta = 0u;
}

static void reset_counters(void)
{
    device.reads = 0u;
    device.writes = 0u;
    device.capacity_status = YAN_FS_IO_OK;
    device.read_status = YAN_FS_IO_OK;
    device.write_status = YAN_FS_IO_OK;
    device.read_fail_at = 0u;
    device.read_fail_code = YAN_FS_IO_OK;
    device.read_fail_lba = 0xffffffffu;
    device.write_fail_at = 0u;
    device.write_fail_code = YAN_FS_IO_OK;
}

static void snapshot_medium(void)
{
    memcpy(medium_before, device.blocks, sizeof device.blocks);
}

static bool medium_unchanged(void)
{
    return memcmp(medium_before, device.blocks, sizeof device.blocks) == 0;
}

static void fixture(void)
{
    memset(&device, 0, sizeof device);
    device.capacity_blocks = TEST_DEVICE_BLOCKS;
    device.read_fail_lba = 0xffffffffu;
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_OK, yan_fs_format_metadata(device.blocks[0], TEST_DEVICE_BLOCKS));
    memset(&capture, 0, sizeof capture);
    memset(&fs, 0, sizeof fs);
    YanFsBlockIo io;
    io.context = &device;
    io.capacity = device_capacity;
    io.read_block = device_read;
    io.write_block = device_write;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_init(&fs, io));
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_mount(&fs));
    memset(&editor, 0, sizeof editor);
    YanShellOutput output;
    output.context = &capture;
    output.putc = capture_putc;
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, yan_editor_init(&editor, &fs, output));
}

void setUp(void)
{
    io_probe_enabled = false;
    io_probe_editor = NULL;
    io_probe_line = NULL;
    io_probe_length = 0u;
    io_probe_count = 0u;
    io_probe_result = YAN_EDITOR_OK;
    io_probe_output = 0u;
    memset(medium_before, 0, sizeof medium_before);
    fixture();
}

void tearDown(void)
{
}

/* ------------------------------------------------------------ test helpers */

static YanEditorResult execute_bytes(const uint8_t *line, uint32_t length)
{
    return yan_editor_execute(&editor, line, length);
}

static YanEditorResult execute_text(const char *text)
{
    return execute_bytes((const uint8_t *)text, (uint32_t)strlen(text));
}

static void expect_output(const char *text)
{
    uint32_t length = (uint32_t)strlen(text);
    TEST_ASSERT_EQUAL_UINT32(length, capture.length);
    if (length > 0u) {
        TEST_ASSERT_EQUAL_MEMORY(text, capture.bytes, length);
    }
}

static void expect_bytes(const uint8_t *bytes, uint32_t length)
{
    TEST_ASSERT_EQUAL_UINT32(length, capture.length);
    if (length > 0u) {
        TEST_ASSERT_EQUAL_MEMORY(bytes, capture.bytes, length);
    }
}

static void expect_draft(const uint8_t *bytes, uint32_t length)
{
    TEST_ASSERT_EQUAL_UINT32(length, editor.length);
    if (length > 0u) {
        TEST_ASSERT_EQUAL_MEMORY(bytes, editor.draft, length);
    }
}

static void seed_file(const char *name, const uint8_t *bytes, uint32_t length)
{
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_create(&fs, name, bytes, length));
}

static void expect_file(const char *name, const uint8_t *bytes, uint32_t length)
{
    YanFsInfo info;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_stat(&fs, name, &info));
    TEST_ASSERT_EQUAL_UINT32(length, info.size_bytes);
    static uint8_t readback[YAN_EDITOR_CAPACITY + 1u];
    uint32_t read_bytes = 0u;
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_OK, yan_fs_read(&fs, name, 0u, readback, length, &read_bytes));
    TEST_ASSERT_EQUAL_UINT32(length, read_bytes);
    if (length > 0u) {
        TEST_ASSERT_EQUAL_MEMORY(bytes, readback, length);
    }
}

/* Enters editing for `name` and checks the exact confirmation. */
static void enter(const char *name)
{
    reset_capture();
    TEST_ASSERT_EQUAL_INT(
        YAN_EDITOR_OK,
        yan_editor_start(&editor, (const uint8_t *)name,
                         (uint32_t)strlen(name)));
    expect_output("OK edit\r\n");
    TEST_ASSERT_TRUE(yan_editor_active(&editor));
}

static void make_indexed_name(char out[8], char prefix, uint32_t index)
{
    out[0] = prefix;
    if (index < 10u) {
        out[1] = (char)('0' + index);
        out[2] = '\0';
    } else {
        out[1] = (char)('0' + (index / 10u));
        out[2] = (char)('0' + (index % 10u));
        out[3] = '\0';
    }
}

/* ----------------------------------------------------------- start / load */

static void start_new_file_enters_editing_with_an_empty_draft(void)
{
    reset_capture();
    TEST_ASSERT_EQUAL_INT(
        YAN_EDITOR_OK,
        yan_editor_start(&editor, (const uint8_t *)"note.txt", 8u));
    expect_output("OK edit\r\n");
    TEST_ASSERT_TRUE(yan_editor_active(&editor));
    TEST_ASSERT_FALSE(editor.existing);
    TEST_ASSERT_EQUAL_UINT32(0u, editor.length);
    YanFsInfo info;
    TEST_ASSERT_EQUAL_INT(YAN_FS_NOT_FOUND, yan_fs_stat(&fs, "note.txt", &info));
}

static void start_loads_existing_bytes_without_normalizing(void)
{
    static const uint8_t content[] = {'A', 0x0du, 0x0au, 'B'};
    seed_file("note.txt", content, sizeof content);
    reset_capture();
    TEST_ASSERT_EQUAL_INT(
        YAN_EDITOR_OK,
        yan_editor_start(&editor, (const uint8_t *)"note.txt", 8u));
    expect_output("OK edit\r\n");
    TEST_ASSERT_TRUE(yan_editor_active(&editor));
    TEST_ASSERT_TRUE(editor.existing);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)sizeof content, editor.length,
                                     "EDT load normalized CRLF");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(content, editor.draft, sizeof content,
                                     "EDT load normalized CRLF");
}

static void start_rejects_oversize_file_without_publishing_a_prefix(void)
{
    static uint8_t big[YAN_EDITOR_CAPACITY + 1u];
    memset(big, 'A', sizeof big);
    seed_file("big", big, sizeof big);
    snapshot_medium();
    reset_counters();
    reset_capture();
    TEST_ASSERT_EQUAL_INT(
        YAN_EDITOR_OK, yan_editor_start(&editor, (const uint8_t *)"big", 3u));
    TEST_ASSERT_FALSE_MESSAGE(yan_editor_active(&editor),
                              "EDT oversize load truncated to capacity");
    expect_output("ERROR TOO_LARGE\r\n");
    TEST_ASSERT_FALSE(yan_editor_active(&editor));
    TEST_ASSERT_EQUAL_UINT32(0u, editor.length);
    TEST_ASSERT_TRUE(medium_unchanged());
    TEST_ASSERT_EQUAL_UINT32(0u, device.writes);
}

static void start_rejects_invalid_text_without_publishing_a_prefix(void)
{
    static const uint8_t nul[] = {'A', 0x00u, 'B'};
    static const uint8_t c0[] = {'A', 0x01u, 'B'};
    static const uint8_t del[] = {'A', 0x7fu, 'B'};
    static const uint8_t bare_cr[] = {'A', 0x0du, 'B'};
    static const uint8_t c1[] = {0xc2u, 0x80u};
    static const uint8_t lone_continuation[] = {0x80u};
    static const uint8_t overlong[] = {0xc0u, 0x80u};
    static const uint8_t surrogate[] = {0xedu, 0xa0u, 0x80u};
    static const uint8_t past_max[] = {0xf4u, 0x90u, 0x80u, 0x80u};
    static const uint8_t truncated[] = {0xe4u, 0xb8u};
    struct Case {
        const char *name;
        const uint8_t *bytes;
        uint32_t length;
    };
    static const struct Case cases[] = {
        {"b0", nul, sizeof nul},
        {"b1", c0, sizeof c0},
        {"b2", del, sizeof del},
        {"b3", bare_cr, sizeof bare_cr},
        {"b4", c1, sizeof c1},
        {"b5", lone_continuation, sizeof lone_continuation},
        {"b6", overlong, sizeof overlong},
        {"b7", surrogate, sizeof surrogate},
        {"b8", past_max, sizeof past_max},
        {"b9", truncated, sizeof truncated},
    };
    for (uint32_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        seed_file(cases[i].name, cases[i].bytes, cases[i].length);
        reset_capture();
        TEST_ASSERT_EQUAL_INT(
            YAN_EDITOR_OK,
            yan_editor_start(&editor, (const uint8_t *)cases[i].name,
                             (uint32_t)strlen(cases[i].name)));
        expect_output("ERROR INVALID_TEXT\r\n");
        TEST_ASSERT_FALSE(yan_editor_active(&editor));
        TEST_ASSERT_EQUAL_UINT32(0u, editor.length);
    }
}

static void start_rejects_an_incomplete_load_and_leaves_nothing_savable(void)
{
    seed_file("d", (const uint8_t *)"ABCD", 4u);
    uint32_t writes_before = device.writes;
    device.read_fail_lba = 1u;
    device.read_fail_code = YAN_FS_IO_ERROR;
    reset_capture();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_EDITOR_FATAL,
        yan_editor_start(&editor, (const uint8_t *)"d", 1u),
        "EDT failed load published a draft prefix");
    expect_output("ERROR IO\r\n");
    TEST_ASSERT_FALSE(yan_editor_active(&editor));
    TEST_ASSERT_EQUAL_UINT32(0u, editor.length);
    TEST_ASSERT_EQUAL_INT(YAN_FS_STATE_FAULTED, fs.state);

    /* A save can only be reached through an active session; this one is not,
     * so the failed load produces no write call at all. */
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_INVALID,
                          execute_bytes((const uint8_t *)"w", 1u));
    expect_output("");
    TEST_ASSERT_EQUAL_UINT32(writes_before, device.writes);
}

static void start_reports_ordinary_errors_and_stays_inactive(void)
{
    reset_capture();
    TEST_ASSERT_EQUAL_INT(
        YAN_EDITOR_OK,
        yan_editor_start(&editor, (const uint8_t *)"bad/name", 8u));
    expect_output("ERROR INVALID\r\n");
    TEST_ASSERT_FALSE(yan_editor_active(&editor));

    static const uint8_t long_name[32] = {
        'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a',
        'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a',
        'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a'
    };
    reset_capture();
    TEST_ASSERT_EQUAL_INT(
        YAN_EDITOR_OK,
        yan_editor_start(&editor, long_name, sizeof long_name));
    expect_output("ERROR INVALID\r\n");
    TEST_ASSERT_FALSE(yan_editor_active(&editor));

    fs.busy = true;
    reset_capture();
    TEST_ASSERT_EQUAL_INT(
        YAN_EDITOR_OK,
        yan_editor_start(&editor, (const uint8_t *)"busy.txt", 8u));
    expect_output("ERROR BUSY\r\n");
    TEST_ASSERT_FALSE(yan_editor_active(&editor));
    fs.busy = false;
}

static void start_validates_the_entire_length_delimited_name(void)
{
    static const uint8_t embedded[] = {'a', 0u, 'b'};
    static const uint8_t long_embedded[YAN_FS_NAME_MAX + 2u] = {'a', 0u, 'b'};
    const uint8_t *bad[] = {embedded, long_embedded};
    const uint32_t lengths[] = {sizeof embedded, sizeof long_embedded};
    reset_counters();
    for (uint32_t i = 0u; i < 2u; ++i) {
        reset_capture();
        TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK,
                              yan_editor_start(&editor, bad[i], lengths[i]));
        TEST_ASSERT_FALSE_MESSAGE(yan_editor_active(&editor),
                                  "EDT full name cannot truncate at NUL");
        expect_output("ERROR INVALID\r\n");
        TEST_ASSERT_EQUAL_UINT32(0u, editor.length);
        TEST_ASSERT_EQUAL_UINT32(0u, device.reads);
        TEST_ASSERT_EQUAL_UINT32(0u, device.writes);
    }
    /* Exactly two addressable bytes, with no NUL terminator to scan for. */
    static const uint8_t valid[] = {'x', 'y'};
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK,
                          yan_editor_start(&editor, valid, sizeof valid));
    TEST_ASSERT_TRUE(yan_editor_active(&editor));
    TEST_ASSERT_EQUAL_STRING("xy", editor.name);
    expect_output("OK edit\r\n");
}

static void start_on_unmounted_filesystem_is_fatal(void)
{
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_unmount(&fs));
    reset_capture();
    TEST_ASSERT_EQUAL_INT(
        YAN_EDITOR_FATAL, yan_editor_start(&editor, (const uint8_t *)"a", 1u));
    expect_output("ERROR NOT_MOUNTED\r\n");
    TEST_ASSERT_FALSE(yan_editor_active(&editor));
}

static void start_on_uninitialized_filesystem_is_ordinary_invalid(void)
{
    static YanFs fresh;
    static YanEditor local;
    memset(&fresh, 0, sizeof fresh);
    memset(&local, 0, sizeof local);
    YanShellOutput output;
    output.context = &capture;
    output.putc = capture_putc;
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK,
                          yan_editor_init(&local, &fresh, output));
    reset_capture();
    TEST_ASSERT_EQUAL_INT(
        YAN_EDITOR_OK, yan_editor_start(&local, (const uint8_t *)"a", 1u));
    expect_output("ERROR INVALID\r\n");
    TEST_ASSERT_FALSE(yan_editor_active(&local));
}

/* --------------------------------------------------------------- append */

static void append_creates_lf_terminated_lines(void)
{
    enter("note");
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("a first"));
    expect_output("OK a\r\n");
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("a second"));
    expect_output("OK a\r\n");
    static const uint8_t expected[] = "first\nsecond\n";
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)(sizeof expected - 1u),
                                     editor.length,
                                     "EDT append closing LF dropped");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(expected, editor.draft,
                                     sizeof expected - 1u,
                                     "EDT append closing LF dropped");
}

static void append_separates_an_unterminated_last_line(void)
{
    seed_file("u", (const uint8_t *)"A\nB", 3u);
    enter("u");
    reset_counters();
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("a C"));
    expect_output("OK a\r\n");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, device.writes, "EDT append performed filesystem IO");
    static const uint8_t expected[] = "A\nB\nC\n";
    expect_draft(expected, sizeof expected - 1u);
}

static void append_after_a_terminated_line_adds_no_separator(void)
{
    seed_file("t", (const uint8_t *)"A\n", 2u);
    enter("t");
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("a B"));
    expect_output("OK a\r\n");
    static const uint8_t expected[] = "A\nB\n";
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        (uint32_t)(sizeof expected - 1u), editor.length,
        "EDT append added a separator after a terminated draft");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(
        expected, editor.draft, sizeof expected - 1u,
        "EDT append added a separator after a terminated draft");
}

static void append_empty_text_makes_an_empty_line(void)
{
    enter("blank");
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("a"));
    expect_output("OK a\r\n");
    static const uint8_t expected[] = "\n";
    expect_draft(expected, sizeof expected - 1u);

    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("p"));
    expect_output("1 \r\nOK p\r\n");
}

static void append_preserves_extra_and_trailing_spaces(void)
{
    enter("sp");
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("a  x "));
    expect_output("OK a\r\n");
    static const uint8_t expected[] = " x \n";
    expect_draft(expected, sizeof expected - 1u);
}

static void append_keeps_raw_utf8_bytes(void)
{
    static const uint8_t text[] = {0xc3u, 0xa9u, 0xe4u, 0xb8u, 0xadu};
    uint8_t line[8];
    line[0] = (uint8_t)'a';
    line[1] = (uint8_t)' ';
    memcpy(line + 2u, text, sizeof text);
    enter("utf");
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK,
                          execute_bytes(line, 2u + (uint32_t)sizeof text));
    expect_output("OK a\r\n");
    uint8_t expected[8];
    memcpy(expected, text, sizeof text);
    expected[sizeof text] = (uint8_t)'\n';
    expect_draft(expected, sizeof text + 1u);
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_EXIT, execute_text("w"));
    expect_output("OK w\r\n");
    expect_file("utf", expected, sizeof text + 1u);
}

/* `a` always appends LF: it never rewrites an existing CRLF and it never makes
 * the new terminator CRLF. For "A\r\nB\r\n" plus `a C` the result is
 * "A\r\nB\r\nC\n", and for the unterminated "A\r\nB" the separator is LF and
 * the new terminator is LF too, giving "A\r\nB\nC\n". */
static void append_keeps_existing_crlf_and_writes_lf(void)
{
    seed_file("crlf1", (const uint8_t *)"A\r\nB\r\n", 6u);
    enter("crlf1");
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("a C"));
    static const uint8_t after1[] = "A\r\nB\r\nC\n";
    expect_draft(after1, sizeof after1 - 1u);
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_EXIT, execute_text("q"));

    seed_file("crlf2", (const uint8_t *)"A\r\nB", 4u);
    enter("crlf2");
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("a C"));
    static const uint8_t after2[] = "A\r\nB\nC\n";
    expect_draft(after2, sizeof after2 - 1u);
}

/* -------------------------------------------------------------- replace */

static void replace_preserves_lf_crlf_and_missing_eol(void)
{
    seed_file("mix", (const uint8_t *)"A\r\nB\nC", 6u);
    enter("mix");
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("r 1 Z"));
    static const uint8_t after1[] = "Z\r\nB\nC";
    expect_draft(after1, sizeof after1 - 1u);
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("r 2 Y"));
    static const uint8_t after2[] = "Z\r\nY\nC";
    expect_draft(after2, sizeof after2 - 1u);
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("r 3 W"));
    static const uint8_t after3[] = "Z\r\nY\nW";
    expect_draft(after3, sizeof after3 - 1u);
}

static void replace_grows_and_shrinks_the_line(void)
{
    seed_file("g", (const uint8_t *)"ab\ncd\n", 6u);
    enter("g");
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("r 1 abcdef"));
    static const uint8_t grown[] = "abcdef\ncd\n";
    expect_draft(grown, sizeof grown - 1u);
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("r 1 x"));
    static const uint8_t shrunk[] = "x\ncd\n";
    expect_draft(shrunk, sizeof shrunk - 1u);
}

/* Growing a replaced line makes the tail move backward; shrinking it makes the
 * tail move forward. Both directions must carry the old CRLF and the bytes of
 * the next line intact. This drives the same static draft-move helper that the
 * freestanding Guest links, so the two builds share one overlap implementation. */
static void replace_grow_and_shrink_move_crlf_and_tail(void)
{
    seed_file("mv", (const uint8_t *)"A\r\nBB\r\nC\n", 9u);
    enter("mv");
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("r 1 XXXX"));
    static const uint8_t grown[] = "XXXX\r\nBB\r\nC\n";
    expect_draft(grown, sizeof grown - 1u);
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("r 1 Z"));
    static const uint8_t shrunk[] = "Z\r\nBB\r\nC\n";
    expect_draft(shrunk, sizeof shrunk - 1u);
}

static void replace_empty_text_keeps_the_eol(void)
{
    seed_file("e", (const uint8_t *)"A\r\nB\nC", 6u);
    enter("e");
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("r 2"));
    static const uint8_t expected[] = "A\r\n\nC";
    expect_draft(expected, sizeof expected - 1u);
}

/* --------------------------------------------------------------- delete */

static void delete_removes_line_and_terminator(void)
{
    seed_file("d1", (const uint8_t *)"A\nB\r\nC", 6u);
    enter("d1");
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("d 1"));
    static const uint8_t after1[] = "B\r\nC";
    expect_draft(after1, sizeof after1 - 1u);
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("d 2"));
    static const uint8_t after2[] = "B\r\n";
    expect_draft(after2, sizeof after2 - 1u);
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("d 1"));
    expect_draft(NULL, 0u);
}

static void delete_last_line_without_eol_removes_only_content(void)
{
    seed_file("d2", (const uint8_t *)"A\nB", 3u);
    enter("d2");
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("d 2"));
    static const uint8_t expected[] = "A\n";
    expect_draft(expected, sizeof expected - 1u);
}

static void delete_every_line_leaves_an_empty_draft(void)
{
    seed_file("d3", (const uint8_t *)"A\nB\n", 4u);
    enter("d3");
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("d 1"));
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("d 1"));
    expect_draft(NULL, 0u);
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("p"));
    expect_output("OK p\r\n");
}

/* ---------------------------------------------------------------- print */

static void print_numbers_lines_and_ends_with_crlf(void)
{
    seed_file("p1", (const uint8_t *)"A\nB\r\n\n", 6u);
    enter("p1");
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("p"));
    expect_output("1 A\r\n2 B\r\n3 \r\nOK p\r\n");

    /* Leave the first session before starting the next one. */
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_EXIT, execute_text("q"));
    TEST_ASSERT_FALSE(yan_editor_active(&editor));
    seed_file("p2", (const uint8_t *)"A\nB", 3u);
    enter("p2");
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("p"));
    expect_output("1 A\r\n2 B\r\nOK p\r\n");
}

static void print_of_an_empty_draft_only_confirms(void)
{
    enter("p3");
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("p"));
    expect_output("OK p\r\n");
}

/* A draft whose only line is an empty CRLF-terminated line prints one number,
 * one space and CRLF, and the CRLF is not normalized away. */
static void print_of_one_empty_crlf_line(void)
{
    seed_file("ecrlf", (const uint8_t *)"\r\n", 2u);
    enter("ecrlf");
    static const uint8_t draft[] = "\r\n";
    expect_draft(draft, sizeof draft - 1u);
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("p"));
    expect_output("1 \r\nOK p\r\n");
}

static void print_escapes_tab_and_backslash(void)
{
    static const uint8_t content[] = {'a', 0x09u, 'b', '\\', 'c'};
    seed_file("tab", content, sizeof content);
    enter("tab");
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("p"));
    expect_output("1 a\\x09b\\\\c\r\nOK p\r\n");
}

/* Legal UTF-8 has no display escape: a 2-byte sequence, a 3-byte sequence and
 * a 4-byte sequence come out byte-for-byte, unlike C1 and invalid sequences,
 * which the load rule refuses before they could ever appear here. */
static void print_passes_valid_utf8_through(void)
{
    static const uint8_t content[] = {0xc3u, 0xa9u, 0xe4u, 0xb8u, 0xadu,
                                      0xf0u, 0x9fu, 0x98u, 0x80u};
    static const uint8_t expected[] = {
        '1', ' ',       0xc3u, 0xa9u, 0xe4u, 0xb8u, 0xadu,
        0xf0u, 0x9fu,   0x98u, 0x80u, '\r',  '\n',  'O',
        'K',   ' ',     'p',   '\r',  '\n'
    };
    seed_file("u8", content, sizeof content);
    enter("u8");
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("p"));
    expect_bytes(expected, sizeof expected);
}

static void print_does_not_touch_the_medium_or_the_draft(void)
{
    seed_file("keep", (const uint8_t *)"A\nB", 3u);
    enter("keep");
    static uint8_t before[4];
    memcpy(before, editor.draft, editor.length);
    uint32_t before_length = editor.length;
    snapshot_medium();
    reset_counters();
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("p"));
    expect_output("1 A\r\n2 B\r\nOK p\r\n");
    TEST_ASSERT_EQUAL_UINT32(0u, device.reads);
    TEST_ASSERT_EQUAL_UINT32(0u, device.writes);
    TEST_ASSERT_TRUE(medium_unchanged());
    expect_draft(before, before_length);
}

static void print_output_failure_stops_before_the_ok_tail(void)
{
    seed_file("pf", (const uint8_t *)"AAAA", 4u);
    enter("pf");
    uint32_t writes_before = device.writes;
    reset_capture();
    capture.fail_at = 1u;
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_FATAL, execute_text("p"));
    TEST_ASSERT_TRUE(capture.fail_seen);
    TEST_ASSERT_EQUAL_UINT32(0u, capture.length);
    TEST_ASSERT_EQUAL_UINT32(writes_before, device.writes);
}

/* A failure in the middle of a multi-line `p` keeps exactly the bytes already
 * sent, never prints the OK tail, and makes no filesystem call. */
static void print_output_failure_keeps_the_prefix_without_ok(void)
{
    seed_file("pm", (const uint8_t *)"A\nB\nC\n", 6u);
    enter("pm");
    uint32_t writes_before = device.writes;
    reset_capture();
    capture.fail_at = 8u; /* the 'B' of line two, after "1 A\r\n2 " */
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_FATAL, execute_text("p"));
    TEST_ASSERT_TRUE(capture.fail_seen);
    TEST_ASSERT_EQUAL_UINT32(7u, capture.length);
    TEST_ASSERT_EQUAL_MEMORY("1 A\r\n2 ", capture.bytes, 7u);
    TEST_ASSERT_NULL(strstr((const char *)capture.bytes, "OK"));
    TEST_ASSERT_EQUAL_UINT32(writes_before, device.writes);
}

/* ------------------------------------------------------------- capacity */

static void append_at_exact_capacity_succeeds_and_one_more_is_refused(void)
{
    static uint8_t near_full[YAN_EDITOR_CAPACITY - 3u];
    memset(near_full, 'A', sizeof near_full);
    seed_file("cap", near_full, sizeof near_full);
    enter("cap");
    TEST_ASSERT_EQUAL_UINT32((uint32_t)sizeof near_full, editor.length);

    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("a x"));
    expect_output("OK a\r\n");
    TEST_ASSERT_EQUAL_UINT32(YAN_EDITOR_CAPACITY, editor.length);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)'\n',
                            editor.draft[YAN_EDITOR_CAPACITY - 3u]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)'x',
                            editor.draft[YAN_EDITOR_CAPACITY - 2u]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)'\n',
                            editor.draft[YAN_EDITOR_CAPACITY - 1u]);

    static uint8_t before[YAN_EDITOR_CAPACITY];
    memcpy(before, editor.draft, sizeof before);
    snapshot_medium();
    reset_counters();
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("a y"));
    expect_output("ERROR TOO_LARGE\r\n");
    TEST_ASSERT_EQUAL_UINT32(YAN_EDITOR_CAPACITY, editor.length);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(before, editor.draft, sizeof before,
                                     "EDT capacity refusal mutated the draft");
    TEST_ASSERT_EQUAL_UINT32(0u, device.writes);
    TEST_ASSERT_TRUE(medium_unchanged());
    TEST_ASSERT_TRUE(yan_editor_active(&editor));
}

static void replace_that_would_exceed_capacity_is_refused_atomically(void)
{
    static uint8_t full[YAN_EDITOR_CAPACITY];
    full[0] = (uint8_t)'x';
    full[1] = (uint8_t)'\n';
    memset(full + 2u, 'A', sizeof full - 2u);
    seed_file("full", full, sizeof full);
    enter("full");
    TEST_ASSERT_EQUAL_UINT32(YAN_EDITOR_CAPACITY, editor.length);

    static uint8_t line[1024];
    memcpy(line, "r 1 ", 4u);
    memset(line + 4u, 'B', YAN_EDITOR_LINE_MAX - 4u);
    uint32_t line_length = YAN_EDITOR_LINE_MAX;

    static uint8_t before[YAN_EDITOR_CAPACITY];
    memcpy(before, editor.draft, sizeof before);
    snapshot_medium();
    reset_counters();
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK,
                          execute_bytes(line, line_length));
    expect_output("ERROR TOO_LARGE\r\n");
    TEST_ASSERT_EQUAL_UINT32(YAN_EDITOR_CAPACITY, editor.length);
    TEST_ASSERT_EQUAL_MEMORY(before, editor.draft, sizeof before);
    TEST_ASSERT_EQUAL_UINT32(0u, device.writes);
    TEST_ASSERT_TRUE(medium_unchanged());
}

/* -------------------------------------------------------- text validity */

static void append_rejects_invalid_utf8_and_keeps_the_draft(void)
{
    enter("badtext");
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("a ok"));
    static const uint8_t before[] = "ok\n";
    expect_draft(before, sizeof before - 1u);

    static const uint8_t lone[] = {0x80u};
    static const uint8_t overlong[] = {0xc0u, 0x80u};
    static const uint8_t surrogate[] = {0xedu, 0xa0u, 0x80u};
    static const uint8_t truncated[] = {0xe4u, 0xb8u};
    static const uint8_t past_max[] = {0xf4u, 0x90u, 0x80u, 0x80u};
    struct Case {
        const uint8_t *bytes;
        uint32_t length;
    };
    static const struct Case cases[] = {
        {lone, sizeof lone},
        {overlong, sizeof overlong},
        {surrogate, sizeof surrogate},
        {truncated, sizeof truncated},
        {past_max, sizeof past_max},
    };
    for (uint32_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        uint8_t line[8];
        line[0] = (uint8_t)'a';
        line[1] = (uint8_t)' ';
        memcpy(line + 2u, cases[i].bytes, cases[i].length);
        reset_capture();
        TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK,
                              execute_bytes(line, 2u + cases[i].length));
        expect_output("ERROR INVALID_TEXT\r\n");
        expect_draft(before, sizeof before - 1u);
        TEST_ASSERT_TRUE(yan_editor_active(&editor));
    }
}

static void append_rejects_c1_control_text_and_keeps_the_draft(void)
{
    enter("c1text");
    uint8_t line[4] = {(uint8_t)'a', (uint8_t)' ', 0xc2u, 0x80u};
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_bytes(line, sizeof line));
    expect_output("ERROR INVALID_TEXT\r\n");
    expect_draft(NULL, 0u);
    TEST_ASSERT_TRUE(yan_editor_active(&editor));
}

/* A truncated 2-, 3- or 4-byte sequence in typed TEXT rejects the edit as
 * INVALID_TEXT and leaves the whole draft byte-identical. */
static void append_rejects_truncated_utf8_of_each_length(void)
{
    enter("trunc");
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("a keep"));
    static const uint8_t before[] = "keep\n";
    expect_draft(before, sizeof before - 1u);

    static const uint8_t two[] = {0xc3u};
    static const uint8_t three[] = {0xe4u, 0xb8u};
    static const uint8_t four_one[] = {0xf0u};
    static const uint8_t four_two[] = {0xf0u, 0x9fu};
    static const uint8_t four_three[] = {0xf0u, 0x9fu, 0x92u};
    struct Case {
        const uint8_t *bytes;
        uint32_t length;
    };
    static const struct Case cases[] = {
        {two, sizeof two},
        {three, sizeof three},
        {four_one, sizeof four_one},
        {four_two, sizeof four_two},
        {four_three, sizeof four_three},
    };
    for (uint32_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        uint8_t line[8];
        line[0] = (uint8_t)'a';
        line[1] = (uint8_t)' ';
        memcpy(line + 2u, cases[i].bytes, cases[i].length);
        reset_capture();
        TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK,
                              execute_bytes(line, 2u + cases[i].length));
        expect_output("ERROR INVALID_TEXT\r\n");
        expect_draft(before, sizeof before - 1u);
        TEST_ASSERT_TRUE(yan_editor_active(&editor));
    }
}

static void replace_rejects_invalid_text_and_keeps_the_draft(void)
{
    seed_file("rt", (const uint8_t *)"A\nB\n", 4u);
    enter("rt");
    static const uint8_t before[] = "A\nB\n";
    uint8_t line[8] = {(uint8_t)'r', (uint8_t)' ', (uint8_t)'1', (uint8_t)' ',
                       0xe4u, 0xb8u};
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_bytes(line, 6u));
    expect_output("ERROR INVALID_TEXT\r\n");
    expect_draft(before, sizeof before - 1u);
}

/* An empty file has no line: both `r 1` and `d 1` are OUT_OF_RANGE, and the
 * draft stays empty and active. */
static void replace_and_delete_on_an_empty_draft_are_out_of_range(void)
{
    enter("emptydraft");
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("r 1 x"));
    expect_output("ERROR OUT_OF_RANGE\r\n");
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("d 1"));
    expect_output("ERROR OUT_OF_RANGE\r\n");
    expect_draft(NULL, 0u);
    TEST_ASSERT_TRUE(yan_editor_active(&editor));
}

/* ------------------------------------------------------------ save/quit */

static void save_new_draft_creates_the_file(void)
{
    enter("new.txt");
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("a hello"));
    reset_capture();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_EDITOR_EXIT, execute_text("w"),
        "EDT save used the wrong operation for the existing flag");
    expect_output("OK w\r\n");
    TEST_ASSERT_FALSE(yan_editor_active(&editor));
    static const uint8_t expected[] = "hello\n";
    expect_file("new.txt", expected, sizeof expected - 1u);
}

static void save_existing_draft_replaces_the_file(void)
{
    seed_file("old.txt", (const uint8_t *)"A\r\nB", 4u);
    enter("old.txt");
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("r 2 C"));
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_EXIT, execute_text("w"));
    expect_output("OK w\r\n");
    static const uint8_t expected[] = "A\r\nC";
    expect_file("old.txt", expected, sizeof expected - 1u);
}

static void save_empty_draft_creates_an_empty_file(void)
{
    enter("empty.txt");
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_EXIT, execute_text("w"));
    expect_output("OK w\r\n");
    YanFsInfo info;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_stat(&fs, "empty.txt", &info));
    TEST_ASSERT_EQUAL_UINT32(0u, info.size_bytes);
}

static void save_uses_create_for_a_new_name_even_if_it_appeared(void)
{
    enter("race.txt");
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("a mine"));
    /* The name did not exist at start, so the save must still use create. */
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK,
                          yan_fs_create(&fs, "race.txt",
                                        (const uint8_t *)"x", 1u));
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("w"));
    expect_output("ERROR EXISTS\r\n");
    TEST_ASSERT_TRUE(yan_editor_active(&editor));
    expect_file("race.txt", (const uint8_t *)"x", 1u);
}

static void save_uses_replace_for_an_existing_name_even_if_it_vanished(void)
{
    seed_file("vanish.txt", (const uint8_t *)"seed", 4u);
    enter("vanish.txt");
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("r 1 mine"));
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_remove(&fs, "vanish.txt"));
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("w"));
    expect_output("ERROR NOT_FOUND\r\n");
    TEST_ASSERT_TRUE(yan_editor_active(&editor));
    YanFsInfo info;
    TEST_ASSERT_EQUAL_INT(YAN_FS_NOT_FOUND,
                          yan_fs_stat(&fs, "vanish.txt", &info));
}

static void save_output_failure_after_commit_keeps_the_file(void)
{
    enter("made.txt");
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("a hi"));
    reset_capture();
    capture.fail_at = 1u; /* the first byte of the OK line, after the commit */
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_EDITOR_FATAL, execute_text("w"),
        "EDT failed save output returned exit");
    TEST_ASSERT_TRUE(capture.fail_seen);
    TEST_ASSERT_FALSE(yan_editor_active(&editor));
    static const uint8_t expected[] = "hi\n";
    expect_file("made.txt", expected, sizeof expected - 1u);
}

static void save_normal_error_keeps_the_draft_and_the_session(void)
{
    enter("full.txt");
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("a x"));
    /* Consume every data block so the create must fail with NOSPACE. */
    for (uint32_t i = 0; i < TEST_DEVICE_BLOCKS - 1u; ++i) {
        char name[8];
        make_indexed_name(name, 'f', i);
        seed_file(name, (const uint8_t *)"y", 1u);
    }
    static const uint8_t before[] = "x\n";
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("w"));
    expect_output("ERROR NOSPACE\r\n");
    TEST_ASSERT_TRUE(yan_editor_active(&editor));
    expect_draft(before, sizeof before - 1u);
}

static void save_io_error_is_fatal_and_leaves_the_filesystem_faulted(void)
{
    enter("io.txt");
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("a x"));
    device.write_status = YAN_FS_IO_ERROR;
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_FATAL, execute_text("w"));
    expect_output("ERROR IO\r\n");
    TEST_ASSERT_EQUAL_INT(YAN_FS_STATE_FAULTED, fs.state);
    TEST_ASSERT_TRUE(yan_editor_active(&editor));
    static const uint8_t before[] = "x\n";
    expect_draft(before, sizeof before - 1u);
}

static void quit_discards_without_any_io(void)
{
    enter("discard.txt");
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("a hello"));
    snapshot_medium();
    reset_counters();
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_EXIT, execute_text("q"));
    expect_output("OK q\r\n");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, device.writes,
                                     "EDT quit performed filesystem IO");
    TEST_ASSERT_FALSE(yan_editor_active(&editor));
    TEST_ASSERT_EQUAL_UINT32(0u, device.reads);
    TEST_ASSERT_EQUAL_UINT32(0u, device.writes);
    TEST_ASSERT_TRUE(medium_unchanged());
    YanFsInfo info;
    TEST_ASSERT_EQUAL_INT(YAN_FS_NOT_FOUND,
                          yan_fs_stat(&fs, "discard.txt", &info));
}

static void quit_leaves_an_existing_file_untouched(void)
{
    seed_file("keep.txt", (const uint8_t *)"old", 3u);
    enter("keep.txt");
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("r 1 changed"));
    snapshot_medium();
    reset_counters();
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_EXIT, execute_text("q"));
    expect_output("OK q\r\n");
    TEST_ASSERT_EQUAL_UINT32(0u, device.writes);
    TEST_ASSERT_TRUE(medium_unchanged());
    expect_file("keep.txt", (const uint8_t *)"old", 3u);
}

/* --------------------------------------------------------- command edges */

static void empty_and_all_space_editing_lines_emit_nothing(void)
{
    enter("quiet");
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_bytes(NULL, 0u));
    expect_output("");
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("     "));
    expect_output("");
    TEST_ASSERT_EQUAL_UINT32(0u, device.writes);
}

static void line_longer_than_1023_is_refused(void)
{
    enter("longline");
    static uint8_t line[YAN_EDITOR_LINE_MAX + 1u];
    memset(line, (int)'a', sizeof line);
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_bytes(line, sizeof line));
    expect_output("ERROR LINE_TOO_LONG\r\n");
    expect_draft(NULL, 0u);
}

static void typed_control_bytes_reject_the_whole_line(void)
{
    enter("control");
    static const uint8_t tab[] = {'a', ' ', 0x09u, 'x'};
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_bytes(tab, sizeof tab));
    expect_output("ERROR INVALID_INPUT\r\n");
    expect_draft(NULL, 0u);

    static const uint8_t spliced[] = {'a', 0x01u, 'x'};
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_bytes(spliced, sizeof spliced));
    expect_output("ERROR INVALID_INPUT\r\n");
    expect_draft(NULL, 0u);
}

static void unknown_editing_command_reports_unknown_command(void)
{
    enter("unknown");
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("z"));
    expect_output("ERROR UNKNOWN_COMMAND\r\n");
    TEST_ASSERT_TRUE(yan_editor_active(&editor));
}

static void bad_line_numbers_report_usage_or_out_of_range(void)
{
    seed_file("n", (const uint8_t *)"A\nB\n", 4u);
    enter("n");
    struct Case {
        const char *command;
        const char *output;
    };
    static const struct Case cases[] = {
        {"r", "ERROR USAGE\r\n"},
        {"r x y", "ERROR USAGE\r\n"},
        {"r 99999999999999 x", "ERROR USAGE\r\n"},
        {"r 0 x", "ERROR OUT_OF_RANGE\r\n"},
        {"r 3 x", "ERROR OUT_OF_RANGE\r\n"},
        {"d", "ERROR USAGE\r\n"},
        {"d 0", "ERROR OUT_OF_RANGE\r\n"},
        {"d 3", "ERROR OUT_OF_RANGE\r\n"},
        {"d 1 2", "ERROR USAGE\r\n"},
        {"p x", "ERROR USAGE\r\n"},
        {"w x", "ERROR USAGE\r\n"},
        {"q x", "ERROR USAGE\r\n"},
    };
    static const uint8_t before[] = "A\nB\n";
    for (uint32_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        reset_capture();
        TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text(cases[i].command));
        expect_output(cases[i].output);
        expect_draft(before, sizeof before - 1u);
        TEST_ASSERT_TRUE(yan_editor_active(&editor));
    }
}

/* UINT32_MAX parses and is simply past the last line (OUT_OF_RANGE);
 * UINT32_MAX + 1 overflows the parser and is a syntax error (USAGE). */
static void line_number_at_uint32_max_is_range_and_one_more_is_usage(void)
{
    seed_file("num", (const uint8_t *)"A\n", 2u);
    enter("num");
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("r 4294967295 x"));
    expect_output("ERROR OUT_OF_RANGE\r\n");
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("r 4294967296 x"));
    expect_output("ERROR USAGE\r\n");
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("d 4294967295"));
    expect_output("ERROR OUT_OF_RANGE\r\n");
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("d 4294967296"));
    expect_output("ERROR USAGE\r\n");
    static const uint8_t before[] = "A\n";
    expect_draft(before, sizeof before - 1u);
    TEST_ASSERT_TRUE(yan_editor_active(&editor));
}

static void editing_commands_on_a_faulted_filesystem_are_fatal(void)
{
    seed_file("fault", (const uint8_t *)"hello", 5u);
    enter("fault");
    device.write_status = YAN_FS_IO_ERROR;
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_FATAL, execute_text("w"));
    expect_output("ERROR IO\r\n");
    TEST_ASSERT_EQUAL_INT(YAN_FS_STATE_FAULTED, fs.state);

    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_FATAL, execute_text("p"));
    expect_output("ERROR FAULTED\r\n");
}

static void editing_commands_on_an_unmounted_filesystem_are_fatal(void)
{
    enter("um");
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_unmount(&fs));
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_FATAL, execute_text("a x"));
    expect_output("ERROR NOT_MOUNTED\r\n");
}

static void execute_requires_an_active_session(void)
{
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_INVALID, execute_text("p"));
    expect_output("");
    TEST_ASSERT_EQUAL_UINT32(0u, device.writes);
}

/* Whole-line validation comes first and touches no filesystem state, so a
 * rejected line on a gone filesystem reports the line, not the filesystem.
 * A line that passes validation then sees the fatal health state. */
static void line_validation_precedes_the_filesystem_health_guard(void)
{
    enter("prec");
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_unmount(&fs));

    static uint8_t long_line[YAN_EDITOR_LINE_MAX + 1u];
    memset(long_line, 'a', sizeof long_line);
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK,
                          execute_bytes(long_line, sizeof long_line));
    expect_output("ERROR LINE_TOO_LONG\r\n");

    static const uint8_t control[] = {'p', 0x01u};
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_bytes(control, sizeof control));
    expect_output("ERROR INVALID_INPUT\r\n");

    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_FATAL, execute_text("p"));
    expect_output("ERROR NOT_MOUNTED\r\n");
}

/* ----------------------------------------------------- API / alias / BUSY */

static void init_validates_arguments_and_does_no_io(void)
{
    static YanEditor local;
    memset(&local, 0, sizeof local);
    YanShellOutput output;
    output.context = &capture;
    output.putc = capture_putc;
    reset_counters();
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_INVALID,
                          yan_editor_init(NULL, &fs, output));
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_INVALID,
                          yan_editor_init(&local, NULL, output));
    YanShellOutput bad;
    bad.context = &capture;
    bad.putc = NULL;
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_INVALID,
                          yan_editor_init(&local, &fs, bad));
    TEST_ASSERT_EQUAL_UINT32(0u, device.reads);
    TEST_ASSERT_EQUAL_UINT32(0u, device.writes);
    TEST_ASSERT_EQUAL_UINT32(0u, capture.calls);

    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, yan_editor_init(&local, &fs, output));
    TEST_ASSERT_TRUE(local.initialized);
    TEST_ASSERT_FALSE(local.busy);
    TEST_ASSERT_FALSE(local.active);
    /* An idle instance may be re-initialized; one that is inside a call may
     * not, and the state below is exactly what the entry points set. */
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, yan_editor_init(&local, &fs, output));
    local.busy = true;
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_BUSY,
                          yan_editor_init(&local, &fs, output));
}

static void init_rejects_an_overlapping_filesystem(void)
{
    static union {
        YanEditor editor;
        YanFs fs;
    } overlap;
    static uint8_t before[sizeof overlap];
    memset(&overlap, 0, sizeof overlap);
    memcpy(before, &overlap, sizeof overlap);
    YanShellOutput output;
    output.context = &capture;
    output.putc = capture_putc;
    reset_capture();
    TEST_ASSERT_EQUAL_INT(
        YAN_EDITOR_INVALID,
        yan_editor_init(&overlap.editor, &overlap.fs, output));
    TEST_ASSERT_EQUAL_MEMORY(before, &overlap, sizeof overlap);
    TEST_ASSERT_EQUAL_UINT32(0u, capture.calls);
}

static void execute_rejects_aliases_and_ranges(void)
{
    enter("alias");
    reset_capture();
    TEST_ASSERT_EQUAL_INT(
        YAN_EDITOR_INVALID,
        yan_editor_execute(&editor, (const uint8_t *)&editor, 4u));
    TEST_ASSERT_EQUAL_INT(
        YAN_EDITOR_INVALID,
        yan_editor_execute(&editor, (const uint8_t *)&fs, 4u));
    TEST_ASSERT_EQUAL_INT(
        YAN_EDITOR_INVALID,
        yan_editor_execute(&editor, editor.draft, 4u));
    const uint8_t *bad =
        (const uint8_t *)(uintptr_t)(UINTPTR_MAX - (uintptr_t)3u);
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_INVALID,
                          yan_editor_execute(&editor, bad, 8u));
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_INVALID,
                          yan_editor_execute(&editor, NULL, 1u));
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_INVALID, yan_editor_execute(NULL, bad, 8u));
    expect_output("");
}

static void start_rejects_aliased_names(void)
{
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_INVALID,
                          yan_editor_start(&editor, editor.draft, 4u));
    TEST_ASSERT_EQUAL_INT(
        YAN_EDITOR_INVALID,
        yan_editor_start(&editor, (const uint8_t *)&fs, 4u));
    const uint8_t *bad =
        (const uint8_t *)(uintptr_t)(UINTPTR_MAX - (uintptr_t)3u);
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_INVALID,
                          yan_editor_start(&editor, bad, 8u));
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_INVALID,
                          yan_editor_start(&editor, NULL, 1u));
    expect_output("");
}

static void reentrant_output_callback_gets_busy(void)
{
    enter("reout");
    reset_counters();
    reset_capture();
    capture.reentry_at = 1u;
    capture.reentry_editor = &editor;
    capture.reentry_line = (const uint8_t *)"p";
    capture.reentry_length = 1u;
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("a hi"));
    TEST_ASSERT_TRUE(capture.reentry_done);
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_BUSY, capture.reentry_result);
    TEST_ASSERT_EQUAL_UINT32(0u, capture.reentry_output_delta);
    expect_output("OK a\r\n");
    TEST_ASSERT_EQUAL_UINT32(0u, device.reads);
    TEST_ASSERT_EQUAL_UINT32(0u, device.writes);
}

static void reentrant_block_callback_gets_busy(void)
{
    enter("reio");
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_OK, execute_text("a hi"));
    reset_capture();
    io_probe_enabled = true;
    io_probe_editor = &editor;
    io_probe_line = (const uint8_t *)"p";
    io_probe_length = 1u;
    io_probe_count = 0u;
    io_probe_result = YAN_EDITOR_OK;
    io_probe_output = 1u;
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_EXIT, execute_text("w"));
    io_probe_enabled = false;
    TEST_ASSERT_TRUE(io_probe_count > 0u);
    TEST_ASSERT_EQUAL_INT(YAN_EDITOR_BUSY, io_probe_result);
    TEST_ASSERT_EQUAL_UINT32(0u, io_probe_output);
    expect_output("OK w\r\n");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(start_new_file_enters_editing_with_an_empty_draft);
    RUN_TEST(start_loads_existing_bytes_without_normalizing);
    RUN_TEST(start_rejects_oversize_file_without_publishing_a_prefix);
    RUN_TEST(start_rejects_invalid_text_without_publishing_a_prefix);
    RUN_TEST(start_rejects_an_incomplete_load_and_leaves_nothing_savable);
    RUN_TEST(start_reports_ordinary_errors_and_stays_inactive);
    RUN_TEST(start_validates_the_entire_length_delimited_name);
    RUN_TEST(start_on_unmounted_filesystem_is_fatal);
    RUN_TEST(start_on_uninitialized_filesystem_is_ordinary_invalid);
    RUN_TEST(append_creates_lf_terminated_lines);
    RUN_TEST(append_separates_an_unterminated_last_line);
    RUN_TEST(append_after_a_terminated_line_adds_no_separator);
    RUN_TEST(append_empty_text_makes_an_empty_line);
    RUN_TEST(append_preserves_extra_and_trailing_spaces);
    RUN_TEST(append_keeps_raw_utf8_bytes);
    RUN_TEST(append_keeps_existing_crlf_and_writes_lf);
    RUN_TEST(replace_preserves_lf_crlf_and_missing_eol);
    RUN_TEST(replace_grows_and_shrinks_the_line);
    RUN_TEST(replace_grow_and_shrink_move_crlf_and_tail);
    RUN_TEST(replace_empty_text_keeps_the_eol);
    RUN_TEST(delete_removes_line_and_terminator);
    RUN_TEST(delete_last_line_without_eol_removes_only_content);
    RUN_TEST(delete_every_line_leaves_an_empty_draft);
    RUN_TEST(print_numbers_lines_and_ends_with_crlf);
    RUN_TEST(print_of_an_empty_draft_only_confirms);
    RUN_TEST(print_of_one_empty_crlf_line);
    RUN_TEST(print_escapes_tab_and_backslash);
    RUN_TEST(print_passes_valid_utf8_through);
    RUN_TEST(print_does_not_touch_the_medium_or_the_draft);
    RUN_TEST(print_output_failure_stops_before_the_ok_tail);
    RUN_TEST(print_output_failure_keeps_the_prefix_without_ok);
    RUN_TEST(append_at_exact_capacity_succeeds_and_one_more_is_refused);
    RUN_TEST(replace_that_would_exceed_capacity_is_refused_atomically);
    RUN_TEST(append_rejects_invalid_utf8_and_keeps_the_draft);
    RUN_TEST(append_rejects_truncated_utf8_of_each_length);
    RUN_TEST(append_rejects_c1_control_text_and_keeps_the_draft);
    RUN_TEST(replace_rejects_invalid_text_and_keeps_the_draft);
    RUN_TEST(replace_and_delete_on_an_empty_draft_are_out_of_range);
    RUN_TEST(save_new_draft_creates_the_file);
    RUN_TEST(save_existing_draft_replaces_the_file);
    RUN_TEST(save_empty_draft_creates_an_empty_file);
    RUN_TEST(save_uses_create_for_a_new_name_even_if_it_appeared);
    RUN_TEST(save_uses_replace_for_an_existing_name_even_if_it_vanished);
    RUN_TEST(save_output_failure_after_commit_keeps_the_file);
    RUN_TEST(save_normal_error_keeps_the_draft_and_the_session);
    RUN_TEST(save_io_error_is_fatal_and_leaves_the_filesystem_faulted);
    RUN_TEST(quit_discards_without_any_io);
    RUN_TEST(quit_leaves_an_existing_file_untouched);
    RUN_TEST(empty_and_all_space_editing_lines_emit_nothing);
    RUN_TEST(line_longer_than_1023_is_refused);
    RUN_TEST(typed_control_bytes_reject_the_whole_line);
    RUN_TEST(unknown_editing_command_reports_unknown_command);
    RUN_TEST(bad_line_numbers_report_usage_or_out_of_range);
    RUN_TEST(line_number_at_uint32_max_is_range_and_one_more_is_usage);
    RUN_TEST(editing_commands_on_a_faulted_filesystem_are_fatal);
    RUN_TEST(editing_commands_on_an_unmounted_filesystem_are_fatal);
    RUN_TEST(execute_requires_an_active_session);
    RUN_TEST(line_validation_precedes_the_filesystem_health_guard);
    RUN_TEST(init_validates_arguments_and_does_no_io);
    RUN_TEST(init_rejects_an_overlapping_filesystem);
    RUN_TEST(execute_rejects_aliases_and_ranges);
    RUN_TEST(start_rejects_aliased_names);
    RUN_TEST(reentrant_output_callback_gets_busy);
    RUN_TEST(reentrant_block_callback_gets_busy);
    return UNITY_END();
}
