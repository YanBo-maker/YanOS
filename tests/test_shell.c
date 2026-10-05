#include "shell.h"
#include "unity.h"

#include <stddef.h>
#include <string.h>

/* Native tests for the 0022 terminal command dispatcher (os/shell.c).
 *
 * The filesystem under test is the real os/yanfs.c over an in-memory block
 * device, so every command exercises the true name validation, allocation and
 * fault behaviour. The terminal is a capture sink, so the expected output is
 * compared byte for byte.
 *
 * The tests below deliberately own one behaviour each. The names are the
 * mutant-attribution map: a future mutation of any rule should make exactly
 * the test named for that rule fail, and the no-effect controls (empty lines,
 * read-only commands, spaces around tokens) stay green under mutations that
 * do not change behaviour.
 *
 * Coverage boundary: the shell never mounts, formats or repairs a device, so
 * the CORRUPT and UNSUPPORTED results cannot be produced by any command (the
 * filesystem returns them only from mount/format). The dispatcher classifies
 * them as fatal in code, but this file cannot drive them through the public
 * path; everything reachable is pinned below. */

#define TEST_DEVICE_BLOCKS 16u
#define CAPTURE_CAPACITY 262144u

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
static YanShell shell;

typedef struct {
    uint8_t bytes[CAPTURE_CAPACITY];
    uint32_t length;
    uint32_t calls;
    /* Return false on this 1-based putc call (0 = never). */
    uint32_t fail_at;
    bool fail_seen;
    /* On this 1-based putc call, run one nested yan_shell_execute. */
    uint32_t reentry_at;
    YanShell *reentry_shell;
    const uint8_t *reentry_line;
    uint32_t reentry_length;
    YanShellResult reentry_result;
    bool reentry_done;
    uint32_t reentry_output_delta;
} Capture;

static Capture capture;

/* A reentrant call placed inside a block callback, to prove that busy spans
 * the filesystem call as well as the output call. */
static bool io_probe_enabled;
static YanShell *io_probe_shell;
static const uint8_t *io_probe_line;
static uint32_t io_probe_length;
static uint32_t io_probe_count;
static YanShellResult io_probe_result;
static uint32_t io_probe_output;

static uint8_t medium_before[TEST_DEVICE_BLOCKS][YAN_FS_BLOCK_SIZE];

static const char HELP_EXPECTED[] =
    "help: show commands and limits\r\n"
    "ls: list files as <size> <name>\r\n"
    "stat NAME: show a file's size\r\n"
    "cat NAME: print a file\r\n"
    "create NAME [TEXT]: create a new file\r\n"
    "write NAME [TEXT]: replace an existing file\r\n"
    "rm NAME: remove a file\r\n"
    "exit: end the session\r\n"
    "line: at most 1023 bytes, ended by CR or LF\r\n"
    "separators: ASCII spaces; commands and names are case sensitive\r\n"
    "names: 1 to 31 bytes of A-Z a-z 0-9 . _ - and never . or ..\r\n"
    "create creates only; write replaces only; no LF is added to text\r\n"
    "cat: printable ASCII as is, a backslash as two backslashes, C0 and DEL as \\xHH\r\n"
    "cat: valid UTF-8 as is except C1 U+0080 to U+009F, escaped byte by byte\r\n"
    "OK help\r\n";

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
            yan_shell_execute(self->reentry_shell, self->reentry_line,
                              self->reentry_length);
        self->reentry_output_delta = self->length - before;
    }
    if (self->fail_at != 0u && self->calls == self->fail_at) {
        self->fail_seen = true;
        return false;
    }
    /* One byte stays reserved for the terminator used by the strstr checks. */
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
        yan_shell_execute(io_probe_shell, io_probe_line, io_probe_length);
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
    capture.reentry_shell = NULL;
    capture.reentry_line = NULL;
    capture.reentry_length = 0u;
    capture.reentry_result = YAN_SHELL_OK;
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
    memset(&shell, 0, sizeof shell);
    YanShellOutput output;
    output.context = &capture;
    output.putc = capture_putc;
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, yan_shell_init(&shell, &fs, output));
}

void setUp(void)
{
    io_probe_enabled = false;
    io_probe_shell = NULL;
    io_probe_line = NULL;
    io_probe_length = 0u;
    io_probe_count = 0u;
    io_probe_result = YAN_SHELL_OK;
    io_probe_output = 0u;
    memset(medium_before, 0, sizeof medium_before);
    fixture();
}

void tearDown(void)
{
}

/* ------------------------------------------------------------ test helpers */

static YanShellResult execute_bytes(const uint8_t *line, uint32_t length)
{
    return yan_shell_execute(&shell, line, length);
}

static YanShellResult execute_text(const char *text)
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

static void seed_file(const char *name, const uint8_t *bytes, uint32_t length)
{
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_create(&fs, name, bytes, length));
}

static uint32_t append_escape(uint8_t *out, uint32_t position, uint8_t byte)
{
    static const char hex[] = "0123456789ABCDEF";
    out[position] = (uint8_t)'\\';
    out[position + 1u] = (uint8_t)'x';
    out[position + 2u] = (uint8_t)hex[byte >> 4];
    out[position + 3u] = (uint8_t)hex[byte & 0x0Fu];
    return position + 4u;
}

static void append_text(uint8_t *out, uint32_t *position, const char *text)
{
    uint32_t length = (uint32_t)strlen(text);
    memcpy(out + *position, text, length);
    *position += length;
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

/* ------------------------------------------------------------- command set */

static void help_lists_all_commands_and_limits(void)
{
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("help"));
    expect_output(HELP_EXPECTED);
    TEST_ASSERT_NOT_NULL(strstr((const char *)capture.bytes, "stat NAME"));
    TEST_ASSERT_NOT_NULL(strstr((const char *)capture.bytes, "rm NAME"));
    TEST_ASSERT_NOT_NULL(strstr((const char *)capture.bytes, "1023"));
    TEST_ASSERT_NOT_NULL(strstr((const char *)capture.bytes, "exit"));
    TEST_ASSERT_NOT_NULL(strstr((const char *)capture.bytes, "UTF-8"));
    TEST_ASSERT_NOT_NULL(strstr((const char *)capture.bytes, "\\xHH"));
}

static void no_argument_commands_allow_leading_and_trailing_spaces(void)
{
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("   help   "));
    expect_output(HELP_EXPECTED);
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("  ls  "));
    expect_output("OK ls\r\n");
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_EXIT, execute_text("  exit  "));
    expect_output("OK exit\r\n");
}

static void ls_lists_entries_in_slot_order_with_sizes(void)
{
    seed_file("a", (const uint8_t *)"abc", 3u);
    seed_file("bb", NULL, 0u);
    seed_file("ccc", (const uint8_t *)"12345", 5u);
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("ls"));
    expect_output("3 a\r\n0 bb\r\n5 ccc\r\nOK ls\r\n");
}

static void ls_of_an_empty_directory_prints_only_ok(void)
{
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("ls"));
    expect_output("OK ls\r\n");
}

static void stat_prints_size_name_and_ok(void)
{
    seed_file("hello.txt", (const uint8_t *)"hello", 5u);
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("stat hello.txt"));
    expect_output("5 hello.txt\r\nOK stat\r\n");
}

static void stat_missing_file_reports_not_found(void)
{
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("stat nope.txt"));
    expect_output("ERROR NOT_FOUND\r\n");
}

static void cat_prints_content_then_the_completion_tail(void)
{
    seed_file("hello.txt", (const uint8_t *)"hello", 5u);
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("cat hello.txt"));
    expect_output("hello\r\nOK cat\r\n");
}

static void cat_of_an_empty_file_prints_only_the_tail(void)
{
    seed_file("empty.txt", NULL, 0u);
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("cat empty.txt"));
    expect_output("\r\nOK cat\r\n");
}

static void cat_missing_file_reports_not_found(void)
{
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("cat nope.txt"));
    expect_output("ERROR NOT_FOUND\r\n");
}

static void create_makes_a_new_file_with_the_exact_text(void)
{
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("create hello.txt hello"));
    expect_output("OK create\r\n");
    YanFsInfo info;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_stat(&fs, "hello.txt", &info));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(5u, info.size_bytes,
                                     "TML text_gets_implicit_lf");
    uint8_t readback[8];
    uint32_t read_bytes = 0u;
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_OK, yan_fs_read(&fs, "hello.txt", 0u, readback, 8u, &read_bytes));
    TEST_ASSERT_EQUAL_UINT32(5u, read_bytes);
    TEST_ASSERT_EQUAL_MEMORY("hello", readback, 5u);
}

static void create_without_text_makes_a_zero_byte_file(void)
{
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("create empty.txt"));
    expect_output("OK create\r\n");
    YanFsInfo info;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_stat(&fs, "empty.txt", &info));
    TEST_ASSERT_EQUAL_UINT32(0u, info.size_bytes);

    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("create blank.txt "));
    expect_output("OK create\r\n");
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_stat(&fs, "blank.txt", &info));
    TEST_ASSERT_EQUAL_UINT32(0u, info.size_bytes);
}

/* The 0022 example: `write hello.txt  world ` stores one leading and one
 * trailing space around five letters, and no line feed is appended. */
static void write_preserves_extra_leading_and_trailing_spaces(void)
{
    seed_file("hello.txt", NULL, 0u);
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK,
                          execute_text("write hello.txt  world "));
    expect_output("OK write\r\n");
    YanFsInfo info;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_stat(&fs, "hello.txt", &info));
    TEST_ASSERT_EQUAL_UINT32(7u, info.size_bytes);
    uint8_t readback[8];
    uint32_t read_bytes = 0u;
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_OK, yan_fs_read(&fs, "hello.txt", 0u, readback, 8u, &read_bytes));
    TEST_ASSERT_EQUAL_UINT32(7u, read_bytes);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(" world ", readback, 7u,
                                     "TML text_swallows_leading_space");
}

static void create_allows_spaces_between_command_and_name(void)
{
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK,
                          execute_text("  create  spaced.txt x  "));
    expect_output("OK create\r\n");
    YanFsInfo info;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_stat(&fs, "spaced.txt", &info));
    TEST_ASSERT_EQUAL_UINT32(3u, info.size_bytes);
    uint8_t readback[8];
    uint32_t read_bytes = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_read(&fs, "spaced.txt", 0u, readback,
                                                 8u, &read_bytes));
    TEST_ASSERT_EQUAL_UINT32(3u, read_bytes);
    TEST_ASSERT_EQUAL_MEMORY("x  ", readback, 3u);
}

static void create_duplicate_reports_exists_and_keeps_the_original(void)
{
    seed_file("dup", (const uint8_t *)"original", 8u);
    snapshot_medium();
    reset_counters();
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("create dup other"));
    static const char expected_exists[] = "ERROR EXISTS\r\n";
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(sizeof expected_exists - 1u, capture.length,
                                     "TML create_replace_swapped");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(expected_exists, capture.bytes,
                                     sizeof expected_exists - 1u,
                                     "TML bytes create_replace_swapped");
    TEST_ASSERT_EQUAL_UINT32(0u, device.writes);
    TEST_ASSERT_TRUE(medium_unchanged());
    uint8_t readback[16];
    uint32_t read_bytes = 0u;
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_OK, yan_fs_read(&fs, "dup", 0u, readback, 16u, &read_bytes));
    TEST_ASSERT_EQUAL_UINT32(8u, read_bytes);
    TEST_ASSERT_EQUAL_MEMORY("original", readback, 8u);
}

static void write_replaces_an_existing_file(void)
{
    seed_file("replace.txt", (const uint8_t *)"old", 3u);
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("write replace.txt new"));
    expect_output("OK write\r\n");
    YanFsInfo info;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_stat(&fs, "replace.txt", &info));
    TEST_ASSERT_EQUAL_UINT32(3u, info.size_bytes);
    uint8_t readback[8];
    uint32_t read_bytes = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_read(&fs, "replace.txt", 0u, readback,
                                                 8u, &read_bytes));
    TEST_ASSERT_EQUAL_UINT32(3u, read_bytes);
    TEST_ASSERT_EQUAL_MEMORY("new", readback, 3u);
}

static void write_missing_file_reports_not_found_and_does_not_create(void)
{
    snapshot_medium();
    reset_counters();
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("write nope.txt text"));
    expect_output("ERROR NOT_FOUND\r\n");
    TEST_ASSERT_EQUAL_UINT32(0u, device.writes);
    TEST_ASSERT_TRUE(medium_unchanged());
    YanFsInfo info;
    TEST_ASSERT_EQUAL_INT(YAN_FS_NOT_FOUND, yan_fs_stat(&fs, "nope.txt", &info));
}

static void write_shorter_replacement_frees_the_old_extent(void)
{
    uint8_t big[5000];
    memset(big, 'B', sizeof big);
    seed_file("big.txt", big, sizeof big);
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("write big.txt abc"));
    expect_output("OK write\r\n");
    YanFsInfo info;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_stat(&fs, "big.txt", &info));
    TEST_ASSERT_EQUAL_UINT32(3u, info.size_bytes);
    uint8_t readback[4];
    uint32_t read_bytes = 0u;
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_OK, yan_fs_read(&fs, "big.txt", 0u, readback, 4u, &read_bytes));
    TEST_ASSERT_EQUAL_UINT32(3u, read_bytes);
    TEST_ASSERT_EQUAL_MEMORY("abc", readback, 3u);
    /* The freed two-block extent is available again. */
    uint8_t second[4999];
    memset(second, 'C', sizeof second);
    seed_file("second.txt", second, sizeof second);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_stat(&fs, "second.txt", &info));
    TEST_ASSERT_EQUAL_UINT32(4999u, info.size_bytes);
}

static void rm_removes_a_file_and_reports_ok(void)
{
    seed_file("gone.txt", (const uint8_t *)"x", 1u);
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("rm gone.txt"));
    expect_output("OK rm\r\n");
    YanFsInfo info;
    TEST_ASSERT_EQUAL_INT(YAN_FS_NOT_FOUND, yan_fs_stat(&fs, "gone.txt", &info));
}

static void rm_missing_file_reports_not_found(void)
{
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("rm nope.txt"));
    expect_output("ERROR NOT_FOUND\r\n");
}

static void exit_writes_ok_and_returns_exit(void)
{
    reset_capture();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_EXIT, execute_text("exit"),
                                  "TML exit_reported_ok");
    expect_output("OK exit\r\n");
}

static void unknown_commands_report_unknown_command(void)
{
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("frobnicate"));
    expect_output("ERROR UNKNOWN_COMMAND\r\n");
}

static void commands_and_names_are_case_sensitive(void)
{
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("HELP"));
    expect_output("ERROR UNKNOWN_COMMAND\r\n");
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("Ls"));
    expect_output("ERROR UNKNOWN_COMMAND\r\n");
    seed_file("case.txt", (const uint8_t *)"x", 1u);
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("stat CASE.TXT"));
    expect_output("ERROR NOT_FOUND\r\n");
}

static void wrong_arity_reports_usage(void)
{
    static const char *cases[] = {
        "help now", "ls now", "exit now",
        "stat",     "stat a b",
        "cat",      "cat a b",
        "rm",       "rm a b",
        "create",   "write",
    };
    for (uint32_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        reset_capture();
        TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text(cases[i]));
        expect_output("ERROR USAGE\r\n");
    }
}

static void empty_and_all_space_lines_emit_nothing(void)
{
    reset_counters();
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_bytes((const uint8_t *)"", 0u));
    expect_output("");
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("       "));
    expect_output("");
    TEST_ASSERT_EQUAL_UINT32(0u, device.reads);
    TEST_ASSERT_EQUAL_UINT32(0u, device.writes);
}

/* ------------------------------------------------------------- line limits */

static void line_of_1023_bytes_is_processed_and_1024_is_rejected(void)
{
    static uint8_t long_line[1024];
    memset(long_line, (int)'a', sizeof long_line);
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_bytes(long_line, 1023u));
    expect_output("ERROR UNKNOWN_COMMAND\r\n");
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_bytes(long_line, 1024u));
    expect_output("ERROR LINE_TOO_LONG\r\n");
    TEST_ASSERT_EQUAL_UINT32(0u, device.writes);
}

static void maximum_length_create_saves_1008_bytes(void)
{
    static uint8_t line[1024];
    uint32_t position = 0u;
    append_text(line, &position, "create ");
    append_text(line, &position, "big.txt");
    line[position] = (uint8_t)' ';
    ++position;
    uint32_t text_length = 1023u - position;
    for (uint32_t i = 0; i < text_length; ++i) {
        line[position + i] = (uint8_t)('A' + (i % 26u));
    }
    position += text_length;
    TEST_ASSERT_EQUAL_UINT32(1023u, position);

    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_bytes(line, position));
    expect_output("OK create\r\n");
    YanFsInfo info;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_stat(&fs, "big.txt", &info));
    TEST_ASSERT_EQUAL_UINT32(text_length, info.size_bytes);
    static uint8_t readback[1008];
    uint32_t read_bytes = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_read(&fs, "big.txt", 0u, readback,
                                                 text_length, &read_bytes));
    TEST_ASSERT_EQUAL_UINT32(text_length, read_bytes);
    TEST_ASSERT_EQUAL_MEMORY(line + 15u, readback, text_length);
}

/* --------------------------------------------------------- control bytes */

static void control_bytes_reject_the_whole_line_before_any_io(void)
{
    static const uint8_t controls[] = {
        0x00u, 0x01u, 0x09u, 0x0au, 0x0du, 0x1bu, 0x1fu, 0x7fu
    };
    for (uint32_t i = 0; i < sizeof controls; ++i) {
        uint8_t line[12];
        memcpy(line, "create a", 8u);
        line[8] = controls[i];
        line[9] = (uint8_t)'x';
        reset_capture();
        snapshot_medium();
        reset_counters();
        TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_bytes(line, 10u));
        expect_output("ERROR INVALID_INPUT\r\n");
        TEST_ASSERT_EQUAL_UINT32(0u, device.reads);
        TEST_ASSERT_EQUAL_UINT32(0u, device.writes);
        TEST_ASSERT_TRUE(medium_unchanged());
        YanFsInfo info;
        TEST_ASSERT_EQUAL_INT(YAN_FS_NOT_FOUND, yan_fs_stat(&fs, "a", &info));
    }
}

static void control_bytes_inside_text_are_also_rejected(void)
{
    uint8_t line[16];
    memcpy(line, "create a ", 9u);
    line[9] = 0x09u; /* TAB inside TEXT */
    line[10] = (uint8_t)'b';
    reset_capture();
    reset_counters();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_bytes(line, 11u));
    expect_output("ERROR INVALID_INPUT\r\n");
    TEST_ASSERT_EQUAL_UINT32(0u, device.writes);
    YanFsInfo info;
    TEST_ASSERT_EQUAL_INT(YAN_FS_NOT_FOUND, yan_fs_stat(&fs, "a", &info));
}

static void control_bytes_cannot_be_spliced_into_a_command(void)
{
    uint8_t line[16];
    memcpy(line, "cre", 3u);
    line[3] = 0x1bu; /* ESC: without the scan this line would read `create a`. */
    memcpy(line + 4u, "ate a", 5u);
    reset_capture();
    reset_counters();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_bytes(line, 9u));
    expect_output("ERROR INVALID_INPUT\r\n");
    TEST_ASSERT_EQUAL_UINT32(0u, device.writes);
    YanFsInfo info;
    TEST_ASSERT_EQUAL_INT(YAN_FS_NOT_FOUND, yan_fs_stat(&fs, "a", &info));
}

/* -------------------------------------------------------------- raw bytes */

static void text_keeps_high_bytes_unchanged(void)
{
    static const uint8_t line[] = {
        (uint8_t)'c', (uint8_t)'r', (uint8_t)'e', (uint8_t)'a', (uint8_t)'t',
        (uint8_t)'e', (uint8_t)' ', (uint8_t)'h', (uint8_t)' ', 0xc3u, 0xa9u
    };
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_bytes(line, sizeof line));
    expect_output("OK create\r\n");
    YanFsInfo info;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_stat(&fs, "h", &info));
    TEST_ASSERT_EQUAL_UINT32(2u, info.size_bytes);
    uint8_t readback[4];
    uint32_t read_bytes = 0u;
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_OK, yan_fs_read(&fs, "h", 0u, readback, 4u, &read_bytes));
    TEST_ASSERT_EQUAL_UINT32(2u, read_bytes);
    TEST_ASSERT_EQUAL_UINT8(0xc3u, readback[0]);
    TEST_ASSERT_EQUAL_UINT8(0xa9u, readback[1]);
}

static void invalid_names_reach_the_filesystem_as_invalid(void)
{
    static const char *cases[] = {
        "create . x", "create .. x", "create a/b x", "create a*b x",
        "create a\\b x",
    };
    for (uint32_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        reset_capture();
        snapshot_medium();
        reset_counters();
        TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text(cases[i]));
        expect_output("ERROR INVALID\r\n");
        TEST_ASSERT_EQUAL_UINT32(0u, device.writes);
        TEST_ASSERT_TRUE(medium_unchanged());
    }
}

static void name_of_31_bytes_is_accepted_and_32_is_invalid(void)
{
    char name[40];
    char line[64];
    memset(name, 'a', 31u);
    name[31] = '\0';
    memcpy(line, "create ", 7u);
    memcpy(line + 7u, name, 31u);
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_bytes((const uint8_t *)line, 38u));
    expect_output("OK create\r\n");
    YanFsInfo info;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_stat(&fs, name, &info));
    TEST_ASSERT_EQUAL_UINT32(0u, info.size_bytes);

    memset(name, 'b', 32u);
    name[32] = '\0';
    memcpy(line, "create ", 7u);
    memcpy(line + 7u, name, 32u);
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_bytes((const uint8_t *)line, 39u));
    expect_output("ERROR INVALID\r\n");
}

static void read_only_commands_never_write_the_medium(void)
{
    seed_file("f.txt", (const uint8_t *)"hello", 5u);
    snapshot_medium();
    reset_counters();
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("ls"));
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("stat f.txt"));
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("cat f.txt"));
    TEST_ASSERT_EQUAL_UINT32(0u, device.writes);
    TEST_ASSERT_TRUE(medium_unchanged());
}

/* ------------------------------------------------------- filesystem errors */

static void directory_full_reports_and_continues(void)
{
    for (uint32_t i = 0; i < 63u; ++i) {
        char name[8];
        make_indexed_name(name, 'f', i);
        seed_file(name, NULL, 0u);
    }
    snapshot_medium();
    reset_counters();
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("create overflow x"));
    expect_output("ERROR DIRECTORY_FULL\r\n");
    TEST_ASSERT_EQUAL_UINT32(0u, device.writes);
    TEST_ASSERT_TRUE(medium_unchanged());
}

static void nospace_reports_and_continues(void)
{
    for (uint32_t i = 0; i < 15u; ++i) {
        char name[8];
        make_indexed_name(name, 'd', i);
        seed_file(name, (const uint8_t *)"x", 1u);
    }
    snapshot_medium();
    reset_counters();
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("create extra x"));
    expect_output("ERROR NOSPACE\r\n");
    TEST_ASSERT_EQUAL_UINT32(0u, device.writes);
    TEST_ASSERT_TRUE(medium_unchanged());
}

static void filesystem_busy_reports_busy_and_continues(void)
{
    /* Simulates the state an outer operation owns: this layer must map the
     * result to a normal ERROR line instead of pretending the command ran. */
    fs.busy = true;
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("ls"));
    expect_output("ERROR BUSY\r\n");
    fs.busy = false;
}

static void unmounted_filesystem_is_fatal(void)
{
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_unmount(&fs));
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_FATAL, execute_text("ls"));
    expect_output("ERROR NOT_MOUNTED\r\n");
}

static void write_io_error_reports_io_and_faults(void)
{
    device.write_status = YAN_FS_IO_ERROR;
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_FATAL, execute_text("create a hi"));
    static const char expected_io[] = "ERROR IO\r\n";
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(sizeof expected_io - 1u, capture.length,
                                     "TML io_named_as_protocol");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(expected_io, capture.bytes,
                                     sizeof expected_io - 1u,
                                     "TML bytes io_named_as_protocol");
    TEST_ASSERT_EQUAL_INT(YAN_FS_STATE_FAULTED, fs.state);
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_FATAL, execute_text("stat a"));
    expect_output("ERROR FAULTED\r\n");
}

static void write_protocol_error_reports_protocol_and_faults(void)
{
    device.write_status = YAN_FS_IO_PROTOCOL;
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_FATAL, execute_text("create a hi"));
    expect_output("ERROR PROTOCOL\r\n");
    TEST_ASSERT_EQUAL_INT(YAN_FS_STATE_FAULTED, fs.state);
}

/* --------------------------------------------------------------- cat edges */

static void cat_read_failure_keeps_the_prefix_without_an_ok_tail(void)
{
    uint8_t content[5000];
    memset(content, (int)'A', sizeof content);
    seed_file("big", content, sizeof content);
    device.read_fail_lba = 2u;
    device.read_fail_code = YAN_FS_IO_ERROR;
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_FATAL, execute_text("cat big"));
    static uint8_t expected[4096 + 32];
    memset(expected, (int)'A', 4096u);
    /* The ERROR line starts on its own line: the shown bytes were only a
     * prefix, and the success tail must never appear. */
    memcpy(expected + 4096u, "\r\nERROR IO\r\n", 12u);
    expect_bytes(expected, 4096u + 12u);
    TEST_ASSERT_NULL(strstr((const char *)capture.bytes, "OK cat"));
    TEST_ASSERT_NULL(strstr((const char *)capture.bytes, "OK"));
}

/* 0022 cat: a multibyte sequence that begins in one 256-byte read and whose
 * next read fails must still be flushed byte by byte, then the ERROR line, and
 * never an OK tail. The 5000-byte file's first chunk ends with a 0xE4 lead at
 * offset 255 after 255 'A's; the read for the second chunk (offset 256, still
 * block 1) is failed. */
static void cat_read_failure_flushes_pending_utf8(void)
{
    static uint8_t content[5000];
    memset(content, (int)'A', sizeof content);
    content[255] = 0xe4u;
    seed_file("big", content, sizeof content);
    device.read_fail_at = device.reads + 2u; /* the read for the second chunk */
    device.read_fail_code = YAN_FS_IO_ERROR;
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_FATAL, execute_text("cat big"));
    static uint8_t expected[512];
    memset(expected, (int)'A', 255u);
    uint32_t position = 255u;
    position = append_escape(expected, position, 0xe4u);
    append_text(expected, &position, "\r\nERROR IO\r\n");
    expect_bytes(expected, position);
    TEST_ASSERT_NULL(strstr((const char *)capture.bytes, "OK"));
}

/* The same failure while the flushed byte itself is refused: the callback
 * returns false on the first byte of \xE4, so the reader stops at once - no
 * retry, and no ERROR line is attempted. */
static void cat_read_failure_stops_at_a_refused_flush_byte(void)
{
    static uint8_t content[5000];
    memset(content, (int)'A', sizeof content);
    content[255] = 0xe4u;
    seed_file("big", content, sizeof content);
    device.read_fail_at = device.reads + 2u;
    device.read_fail_code = YAN_FS_IO_ERROR;
    reset_capture();
    capture.fail_at = 256u; /* the '\' of the flushed \xE4 */
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_FATAL, execute_text("cat big"));
    TEST_ASSERT_TRUE(capture.fail_seen);
    TEST_ASSERT_EQUAL_UINT32(256u, capture.calls); /* 255 A plus one refusal */
    TEST_ASSERT_EQUAL_UINT32(255u, capture.length);
    TEST_ASSERT_NULL(strstr((const char *)capture.bytes, "ERROR"));
}

/* The three multibyte sequences straddle the 256-byte read chunk boundaries:
 * the 2-byte sequence at 255, the 3-byte one at 510, the 4-byte one at 765. */
static void cat_streams_multibyte_utf8_across_chunk_boundaries(void)
{
    static uint8_t content[1024];
    memset(content, (int)'A', sizeof content);
    content[255] = 0xc3u;
    content[256] = 0xa9u;
    content[510] = 0xe4u;
    content[511] = 0xb8u;
    content[512] = 0xadu;
    content[765] = 0xf0u;
    content[766] = 0x9fu;
    content[767] = 0x98u;
    content[768] = 0x80u;
    seed_file("utf8", content, sizeof content);
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("cat utf8"));
    static uint8_t expected[sizeof content + 16];
    memcpy(expected, content, sizeof content);
    memcpy(expected + sizeof content, "\r\nOK cat\r\n", 10u);
    expect_bytes(expected, sizeof content + 10u);
}

static void cat_escapes_backslash_and_c0_and_del(void)
{
    static const uint8_t content[] = {
        (uint8_t)'a', (uint8_t)'\\', 0x00u, 0x09u, 0x1fu, 0x7fu, (uint8_t)'b'
    };
    seed_file("esc", content, sizeof content);
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("cat esc"));
    uint8_t expected[64];
    uint32_t position = 0u;
    expected[position++] = (uint8_t)'a';
    expected[position++] = (uint8_t)'\\';
    expected[position++] = (uint8_t)'\\';
    position = append_escape(expected, position, 0x00u);
    position = append_escape(expected, position, 0x09u);
    position = append_escape(expected, position, 0x1fu);
    position = append_escape(expected, position, 0x7fu);
    expected[position++] = (uint8_t)'b';
    append_text(expected, &position, "\r\nOK cat\r\n");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(position, capture.length,
                                     "TML cat_ascii_controls_raw");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(expected, capture.bytes, position,
                                     "TML bytes cat_ascii_controls_raw");
}

static void cat_escapes_c1_controls_but_passes_other_utf8(void)
{
    static const uint8_t content[] = {
        0xc2u, 0x80u, /* U+0080 */
        0xc2u, 0x9fu, /* U+009F */
        0xc2u, 0xa0u, /* U+00A0: not C1, passes through */
        0xe2u, 0x82u, 0xacu /* U+20AC */
    };
    seed_file("c1", content, sizeof content);
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("cat c1"));
    uint8_t expected[64];
    uint32_t position = 0u;
    position = append_escape(expected, position, 0xc2u);
    position = append_escape(expected, position, 0x80u);
    position = append_escape(expected, position, 0xc2u);
    position = append_escape(expected, position, 0x9fu);
    expected[position++] = 0xc2u;
    expected[position++] = 0xa0u;
    expected[position++] = 0xe2u;
    expected[position++] = 0x82u;
    expected[position++] = 0xacu;
    append_text(expected, &position, "\r\nOK cat\r\n");
    expect_bytes(expected, position);
}

static void cat_escapes_invalid_utf8_byte_by_byte(void)
{
    static const uint8_t content[] = {
        0x80u,                         /* lone continuation */
        0xc0u, 0x80u,                  /* overlong */
        0xedu, 0xa0u, 0x80u,           /* surrogate */
        0xf4u, 0x90u, 0x80u, 0x80u,    /* above U+10FFFF */
        0xe4u, 0xb8u,                  /* a lead whose continuation is broken
                                        * by the next byte, mid-stream */
        0xe4u, 0x41u                   /* broken sequence, then ASCII */
    };
    seed_file("bad", content, sizeof content);
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("cat bad"));
    uint8_t expected[128];
    uint32_t position = 0u;
    position = append_escape(expected, position, 0x80u);
    position = append_escape(expected, position, 0xc0u);
    position = append_escape(expected, position, 0x80u);
    position = append_escape(expected, position, 0xedu);
    position = append_escape(expected, position, 0xa0u);
    position = append_escape(expected, position, 0x80u);
    position = append_escape(expected, position, 0xf4u);
    position = append_escape(expected, position, 0x90u);
    position = append_escape(expected, position, 0x80u);
    position = append_escape(expected, position, 0x80u);
    position = append_escape(expected, position, 0xe4u);
    position = append_escape(expected, position, 0xb8u);
    position = append_escape(expected, position, 0xe4u);
    expected[position++] = (uint8_t)'A';
    append_text(expected, &position, "\r\nOK cat\r\n");
    expect_bytes(expected, position);
}

/* A true end of file in the middle of a multibyte sequence: the pending bytes
 * have no continuation coming, so each is escaped and the OK tail still
 * follows. The "E4 B8 then E4 41" case above is a mid-stream break, not an EOF
 * one; these are the EOF cases, and they fail if a fix only adjusted comments. */
static void cat_escapes_incomplete_utf8_at_true_eof(void)
{
    static const struct {
        uint8_t bytes[4];
        uint32_t length;
    } cases[] = {
        {{0xc2u, 0u, 0u, 0u}, 1u},          /* 2-byte lead, 1 pending */
        {{0xe4u, 0u, 0u, 0u}, 1u},          /* 3-byte lead, 1 pending */
        {{0xe4u, 0xb8u, 0u, 0u}, 2u},       /* 3-byte sequence, 2 pending */
        {{0xf0u, 0u, 0u, 0u}, 1u},          /* 4-byte lead, 1 pending */
        {{0xf0u, 0x9fu, 0u, 0u}, 2u},       /* 4-byte sequence, 2 pending */
        {{0xf0u, 0x9fu, 0x92u, 0u}, 3u},    /* 4-byte sequence, 3 pending */
    };
    for (uint32_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        char name[3];
        name[0] = 'e';
        name[1] = (char)('0' + i);
        name[2] = '\0';
        seed_file(name, cases[i].bytes, cases[i].length);

        char command[8];
        uint32_t at = 0u;
        const char *prefix = "cat ";
        while (prefix[at] != '\0') {
            command[at] = prefix[at];
            ++at;
        }
        command[at] = name[0];
        ++at;
        command[at] = name[1];
        ++at;
        command[at] = '\0';

        reset_capture();
        TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text(command));
        uint8_t expected[64];
        uint32_t out = 0u;
        for (uint32_t j = 0; j < cases[i].length; ++j) {
            out = append_escape(expected, out, cases[i].bytes[j]);
        }
        append_text(expected, &out, "\r\nOK cat\r\n");
        expect_bytes(expected, out);
    }
}

/* Overlong sequences that are complete still fail the range rule, so every
 * byte is escaped: the check is not only about a missing continuation. */
static void cat_escapes_overlong_sequences_that_are_complete(void)
{
    static const uint8_t overlong3[] = {0xe0u, 0x80u, 0x80u};
    static const uint8_t overlong4[] = {0xf0u, 0x80u, 0x80u, 0x80u};
    uint8_t expected[64];

    seed_file("over3", overlong3, sizeof overlong3);
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("cat over3"));
    uint32_t at = 0u;
    for (uint32_t i = 0; i < sizeof overlong3; ++i) {
        at = append_escape(expected, at, overlong3[i]);
    }
    append_text(expected, &at, "\r\nOK cat\r\n");
    expect_bytes(expected, at);

    seed_file("over4", overlong4, sizeof overlong4);
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("cat over4"));
    at = 0u;
    for (uint32_t i = 0; i < sizeof overlong4; ++i) {
        at = append_escape(expected, at, overlong4[i]);
    }
    append_text(expected, &at, "\r\nOK cat\r\n");
    expect_bytes(expected, at);
}

static void cat_of_a_large_file_reads_every_chunk(void)
{
    static uint8_t content[5000];
    for (uint32_t i = 0; i < sizeof content; ++i) {
        content[i] = (uint8_t)('A' + (i % 26u));
    }
    seed_file("big", content, sizeof content);
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("cat big"));
    static uint8_t expected[sizeof content + 16];
    memcpy(expected, content, sizeof content);
    memcpy(expected + sizeof content, "\r\nOK cat\r\n", 10u);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(sizeof content + 10u, capture.length,
                                     "TML cat_skips_a_chunk_byte");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(expected, capture.bytes,
                                     sizeof content + 10u,
                                     "TML bytes cat_skips_a_chunk_byte");
    TEST_ASSERT_TRUE(device.reads > 1u);
}

/* ------------------------------------------------------- output failures */

static void output_failure_on_the_first_byte_is_fatal(void)
{
    capture.fail_at = 1u;
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_FATAL, execute_text("help"));
    TEST_ASSERT_TRUE(capture.fail_seen);
    TEST_ASSERT_EQUAL_UINT32(0u, capture.length);
    TEST_ASSERT_EQUAL_UINT32(1u, capture.calls);
}

static void output_failure_in_the_middle_stops_immediately(void)
{
    capture.fail_at = 10u;
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_FATAL, execute_text("help"));
    TEST_ASSERT_TRUE(capture.fail_seen);
    TEST_ASSERT_EQUAL_UINT32(9u, capture.length);
    TEST_ASSERT_EQUAL_MEMORY("help: sho", capture.bytes, 9u);
    /* No putc happens after the one that returned false. */
    TEST_ASSERT_EQUAL_UINT32(10u, capture.calls);
}

static void output_failure_on_the_exit_tail_is_fatal_not_exit(void)
{
    capture.fail_at = 9u; /* the final '\n' of "OK exit\r\n" */
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_FATAL, execute_text("exit"),
                                  "TML exit_ignores_output_failure");
    TEST_ASSERT_TRUE(capture.fail_seen);
    TEST_ASSERT_EQUAL_UINT32(8u, capture.length);
    TEST_ASSERT_EQUAL_MEMORY("OK exit\r", capture.bytes, 8u);
    TEST_ASSERT_EQUAL_UINT32(9u, capture.calls);
}

static void create_committed_before_output_failure_keeps_the_file(void)
{
    capture.fail_at = 1u; /* the 'O' of the OK line, after the commit */
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_FATAL, execute_text("create made.txt hi"));
    TEST_ASSERT_TRUE(capture.fail_seen);
    YanFsInfo info;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_stat(&fs, "made.txt", &info));
    TEST_ASSERT_EQUAL_UINT32(2u, info.size_bytes);
}

/* -------------------------------------------------------- reentrancy / API */

static void reentrant_output_callback_gets_busy_without_side_effects(void)
{
    seed_file("r.txt", (const uint8_t *)"x", 1u);
    reset_counters();
    reset_capture();
    capture.reentry_at = 1u;
    capture.reentry_shell = &shell;
    capture.reentry_line = (const uint8_t *)"ls";
    capture.reentry_length = 2u;
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("help"));
    TEST_ASSERT_TRUE(capture.reentry_done);
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_BUSY, capture.reentry_result);
    TEST_ASSERT_EQUAL_UINT32(0u, capture.reentry_output_delta);
    TEST_ASSERT_EQUAL_UINT32(0u, device.reads);
    TEST_ASSERT_EQUAL_UINT32(0u, device.writes);
    expect_output(HELP_EXPECTED);
}

static void reentrant_block_callback_gets_busy_without_side_effects(void)
{
    reset_capture();
    io_probe_enabled = true;
    io_probe_shell = &shell;
    io_probe_line = (const uint8_t *)"ls";
    io_probe_length = 2u;
    io_probe_count = 0u;
    io_probe_result = YAN_SHELL_OK;
    io_probe_output = 1u;
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text("create rr x"));
    io_probe_enabled = false;
    TEST_ASSERT_TRUE(io_probe_count > 0u);
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_BUSY, io_probe_result);
    TEST_ASSERT_EQUAL_UINT32(0u, io_probe_output);
    expect_output("OK create\r\n");
}

static void execute_validates_api_arguments(void)
{
    uint8_t byte = (uint8_t)'x';
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_INVALID, yan_shell_execute(NULL, &byte, 1u));
    YanShell fresh;
    memset(&fresh, 0, sizeof fresh);
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_INVALID, yan_shell_execute(&fresh, &byte, 1u));
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, yan_shell_execute(&shell, NULL, 0u));
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_INVALID, yan_shell_execute(&shell, NULL, 1u));
    expect_output("");
    TEST_ASSERT_EQUAL_UINT32(0u, capture.calls);
}

static void execute_rejects_a_line_that_aliases_shell_or_fs(void)
{
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_INVALID,
                          yan_shell_execute(&shell, (const uint8_t *)&shell, 4u));
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_INVALID,
                          yan_shell_execute(&shell, (const uint8_t *)&fs, 4u));
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_INVALID,
                          yan_shell_execute(&shell, shell.scratch, 4u));
    expect_output("");
}

static void execute_rejects_ranges_that_leave_uintptr(void)
{
    reset_capture();
    const uint8_t *bad =
        (const uint8_t *)(uintptr_t)(UINTPTR_MAX - (uintptr_t)3u);
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_INVALID, yan_shell_execute(&shell, bad, 8u));
    expect_output("");
}

static void init_validates_arguments_and_does_no_filesystem_io(void)
{
    YanShell local;
    memset(&local, 0, sizeof local);
    YanShellOutput output;
    output.context = &capture;
    output.putc = capture_putc;
    reset_counters();
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_INVALID, yan_shell_init(NULL, &fs, output));
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_INVALID, yan_shell_init(&local, NULL, output));
    YanShellOutput bad;
    bad.context = &capture;
    bad.putc = NULL;
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_INVALID, yan_shell_init(&local, &fs, bad));
    TEST_ASSERT_EQUAL_UINT32(0u, device.reads);
    TEST_ASSERT_EQUAL_UINT32(0u, device.writes);
    TEST_ASSERT_EQUAL_UINT32(0u, capture.calls);

    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, yan_shell_init(&local, &fs, output));
    TEST_ASSERT_TRUE(local.initialized);
    TEST_ASSERT_FALSE(local.busy);
    /* An idle instance may be re-initialized; one that is inside a call may
     * not, and the state below is exactly what yan_shell_execute sets. */
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, yan_shell_init(&local, &fs, output));
    local.busy = true;
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_BUSY, yan_shell_init(&local, &fs, output));
}

/* The borrowed filesystem and the shell must not overlap; init rejects that
 * pairing before writing either context. A union forces the overlap. */
static void init_rejects_a_filesystem_that_overlaps_the_shell(void)
{
    static union {
        YanShell shell;
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
        YAN_SHELL_INVALID, yan_shell_init(&overlap.shell, &overlap.fs, output));
    TEST_ASSERT_EQUAL_MEMORY(before, &overlap, sizeof overlap);
    TEST_ASSERT_EQUAL_UINT32(0u, capture.calls);
}

/* The rejected alias region must not be read as a _Bool. A shell pointer placed
 * at the borrowed filesystem's metadata, where a byte is deliberately not a
 * valid _Bool, must come back INVALID with the whole filesystem byte-identical:
 * the overlap test is address arithmetic and runs before any field load. */
static void init_rejects_an_alias_region_with_non_boolean_bytes(void)
{
    static YanFs probe;
    static uint8_t before[sizeof probe];
    memset(&probe, 0, sizeof probe);
    probe.metadata[offsetof(YanShell, initialized)] = 0x02u;
    memcpy(before, &probe, sizeof probe);
    YanShell *aliased = (YanShell *)(void *)probe.metadata;
    YanShellOutput output;
    output.context = &capture;
    output.putc = capture_putc;
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_INVALID,
                          yan_shell_init(aliased, &probe, output));
    TEST_ASSERT_EQUAL_MEMORY(before, &probe, sizeof probe);
    TEST_ASSERT_EQUAL_UINT32(0u, capture.calls);
}

/* A healthy exit is only healthy if the filesystem is still usable. */
static void healthy_exit_requires_a_mounted_filesystem(void)
{
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_unmount(&fs));
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_FATAL, execute_text("exit"));
    expect_output("ERROR NOT_MOUNTED\r\n");
}

/* The health guard ends the session on a faulted filesystem even for a command
 * that would not have touched it: help, or an empty line. */
static void faulted_filesystem_stops_output_only_commands(void)
{
    device.write_status = YAN_FS_IO_ERROR;
    reset_capture();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_FATAL, execute_text("create a hi"),
                                  "TML faulted_reported_ok");
    expect_output("ERROR IO\r\n");

    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_FATAL, execute_text("help"));
    expect_output("ERROR FAULTED\r\n");

    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_FATAL, execute_text(""));
    expect_output("ERROR FAULTED\r\n");
}

/* Whole-line validation comes first and touches no filesystem state, so a
 * rejected line on a gone filesystem reports the line, not the filesystem. */
static void line_rejection_precedes_the_filesystem_health_guard(void)
{
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_unmount(&fs));
    static uint8_t long_line[1024];
    memset(long_line, 'a', sizeof long_line);
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK,
                          yan_shell_execute(&shell, long_line, 1024u));
    expect_output("ERROR LINE_TOO_LONG\r\n");

    static const uint8_t control[] = {'x', 0x01u};
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, yan_shell_execute(&shell, control, 2u));
    expect_output("ERROR INVALID_INPUT\r\n");
}

/* A fatal filesystem ends the session even for an empty line, and a NULL
 * zero-length line is the same case: it must not skip the health look. */
static void fatal_filesystem_stops_null_and_non_null_empty_lines(void)
{
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_unmount(&fs));
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_FATAL, yan_shell_execute(&shell, NULL, 0u));
    expect_output("ERROR NOT_MOUNTED\r\n");

    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_FATAL, execute_text("   "));
    expect_output("ERROR NOT_MOUNTED\r\n");
}

/* The health guard reads the mounted state fields and does not make a
 * filesystem call: a busy-but-mounted instance is not turned into an ERROR by
 * the guard itself. A command that really calls the filesystem still reports
 * BUSY. */
static void health_guard_reads_mounted_state_without_a_filesystem_call(void)
{
    fs.busy = true;
    reset_capture();
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, execute_text(""));
    expect_output("");
    fs.busy = false;
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(help_lists_all_commands_and_limits);
    RUN_TEST(no_argument_commands_allow_leading_and_trailing_spaces);
    RUN_TEST(ls_lists_entries_in_slot_order_with_sizes);
    RUN_TEST(ls_of_an_empty_directory_prints_only_ok);
    RUN_TEST(stat_prints_size_name_and_ok);
    RUN_TEST(stat_missing_file_reports_not_found);
    RUN_TEST(cat_prints_content_then_the_completion_tail);
    RUN_TEST(cat_of_an_empty_file_prints_only_the_tail);
    RUN_TEST(cat_missing_file_reports_not_found);
    RUN_TEST(create_makes_a_new_file_with_the_exact_text);
    RUN_TEST(create_without_text_makes_a_zero_byte_file);
    RUN_TEST(write_preserves_extra_leading_and_trailing_spaces);
    RUN_TEST(create_allows_spaces_between_command_and_name);
    RUN_TEST(create_duplicate_reports_exists_and_keeps_the_original);
    RUN_TEST(write_replaces_an_existing_file);
    RUN_TEST(write_missing_file_reports_not_found_and_does_not_create);
    RUN_TEST(write_shorter_replacement_frees_the_old_extent);
    RUN_TEST(rm_removes_a_file_and_reports_ok);
    RUN_TEST(rm_missing_file_reports_not_found);
    RUN_TEST(exit_writes_ok_and_returns_exit);
    RUN_TEST(unknown_commands_report_unknown_command);
    RUN_TEST(commands_and_names_are_case_sensitive);
    RUN_TEST(wrong_arity_reports_usage);
    RUN_TEST(empty_and_all_space_lines_emit_nothing);
    RUN_TEST(line_of_1023_bytes_is_processed_and_1024_is_rejected);
    RUN_TEST(maximum_length_create_saves_1008_bytes);
    RUN_TEST(control_bytes_reject_the_whole_line_before_any_io);
    RUN_TEST(control_bytes_inside_text_are_also_rejected);
    RUN_TEST(control_bytes_cannot_be_spliced_into_a_command);
    RUN_TEST(text_keeps_high_bytes_unchanged);
    RUN_TEST(invalid_names_reach_the_filesystem_as_invalid);
    RUN_TEST(name_of_31_bytes_is_accepted_and_32_is_invalid);
    RUN_TEST(read_only_commands_never_write_the_medium);
    RUN_TEST(directory_full_reports_and_continues);
    RUN_TEST(nospace_reports_and_continues);
    RUN_TEST(filesystem_busy_reports_busy_and_continues);
    RUN_TEST(unmounted_filesystem_is_fatal);
    RUN_TEST(write_io_error_reports_io_and_faults);
    RUN_TEST(write_protocol_error_reports_protocol_and_faults);
    RUN_TEST(cat_read_failure_keeps_the_prefix_without_an_ok_tail);
    RUN_TEST(cat_read_failure_flushes_pending_utf8);
    RUN_TEST(cat_read_failure_stops_at_a_refused_flush_byte);
    RUN_TEST(cat_streams_multibyte_utf8_across_chunk_boundaries);
    RUN_TEST(cat_escapes_backslash_and_c0_and_del);
    RUN_TEST(cat_escapes_c1_controls_but_passes_other_utf8);
    RUN_TEST(cat_escapes_invalid_utf8_byte_by_byte);
    RUN_TEST(cat_escapes_incomplete_utf8_at_true_eof);
    RUN_TEST(cat_escapes_overlong_sequences_that_are_complete);
    RUN_TEST(cat_of_a_large_file_reads_every_chunk);
    RUN_TEST(output_failure_on_the_first_byte_is_fatal);
    RUN_TEST(output_failure_in_the_middle_stops_immediately);
    RUN_TEST(output_failure_on_the_exit_tail_is_fatal_not_exit);
    RUN_TEST(create_committed_before_output_failure_keeps_the_file);
    RUN_TEST(reentrant_output_callback_gets_busy_without_side_effects);
    RUN_TEST(reentrant_block_callback_gets_busy_without_side_effects);
    RUN_TEST(execute_validates_api_arguments);
    RUN_TEST(execute_rejects_a_line_that_aliases_shell_or_fs);
    RUN_TEST(execute_rejects_ranges_that_leave_uintptr);
    RUN_TEST(init_validates_arguments_and_does_no_filesystem_io);
    RUN_TEST(init_rejects_a_filesystem_that_overlaps_the_shell);
    RUN_TEST(init_rejects_an_alias_region_with_non_boolean_bytes);
    RUN_TEST(healthy_exit_requires_a_mounted_filesystem);
    RUN_TEST(faulted_filesystem_stops_output_only_commands);
    RUN_TEST(line_rejection_precedes_the_filesystem_health_guard);
    RUN_TEST(fatal_filesystem_stops_null_and_non_null_empty_lines);
    RUN_TEST(health_guard_reads_mounted_state_without_a_filesystem_call);
    return UNITY_END();
}
