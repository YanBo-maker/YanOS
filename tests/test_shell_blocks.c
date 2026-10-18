#include "shell.h"
#include "search_linear.h"
#include "search_terms_linear.h"
#include "unity.h"
#include <string.h>

static uint8_t disk[128][4096], output[200000];
static YanFs fs;
static YanBlockContext blocks, other;
static YanSearchLinear linear;
static YanSearch search;
static YanSearchTermsLinear term_linear;
static YanSearchTerms terms;
static YanShell shell;
static uint32_t reads, writes, length, fail_at, failure_reads, read_fail;
static bool accept_failed_byte, reenter;
static YanShellResult nested, nested_bind;

static YanFsIoResult capacity(void *p, uint64_t *n)
{ (void)p; *n = 128; return YAN_FS_IO_OK; }
static YanFsIoResult read_block(void *p, uint32_t lba, uint8_t *out)
{
    (void)p; ++reads;
    if (reads == read_fail || lba >= 128) return YAN_FS_IO_ERROR;
    memcpy(out, disk[lba], 4096); return YAN_FS_IO_OK;
}
static YanFsIoResult write_block(void *p, uint32_t lba, const uint8_t *in)
{
    (void)p; ++writes;
    if (lba >= 128) return YAN_FS_IO_ERROR;
    memcpy(disk[lba], in, 4096); return YAN_FS_IO_OK;
}
static bool putc_sink(void *p, uint8_t byte)
{
    (void)p;
    if (reenter) {
        nested = yan_shell_execute(&shell, (const uint8_t *)"ls", 2);
        nested_bind = yan_shell_bind_blocks(&shell, NULL);
        reenter = false;
    }
    if (fail_at != 0u && length + 1u == fail_at) {
        failure_reads = reads;
        if (accept_failed_byte) output[length++] = byte;
        output[length] = 0;
        return false;
    }
    if (length >= sizeof output - 1u) return false;
    output[length++] = byte; output[length] = 0;
    return true;
}
static void make(const char *name, const char *data)
{ TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_create(&fs, name, (const uint8_t *)data, (uint32_t)strlen(data))); }
static YanShellResult run(const char *command)
{
    length = 0; output[0] = 0;
    return yan_shell_execute(&shell, (const uint8_t *)command, (uint32_t)strlen(command));
}
void setUp(void)
{
    memset(disk, 0, sizeof disk); memset(&fs, 0, sizeof fs);
    memset(&blocks, 0, sizeof blocks); memset(&other, 0, sizeof other);
    memset(&shell, 0, sizeof shell); memset(&search, 0, sizeof search);
    memset(&terms, 0, sizeof terms); memset(&linear, 0, sizeof linear);
    memset(&term_linear, 0, sizeof term_linear);
    reads = writes = length = fail_at = failure_reads = read_fail = 0;
    accept_failed_byte = reenter = false;
    YanFsBlockIo io = {NULL, capacity, read_block, write_block};
    YanShellOutput sink = {NULL, putc_sink};
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_format_metadata(disk[0], 128));
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_init(&fs, io));
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_mount(&fs));
    TEST_ASSERT_EQUAL_INT(YAN_BLOCK_OK, yan_blocks_init(&blocks, &fs));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_OK, yan_search_linear_init(&linear, &fs));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_OK, yan_search_init(&search, yan_search_linear_backend(&linear)));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK, yan_search_terms_linear_init(&term_linear, &fs));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK, yan_search_terms_init(&terms, yan_search_terms_linear_backend(&term_linear)));
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, yan_shell_init(&shell, &fs, &search, &terms, sink));
}
void tearDown(void) { }
static void bind(void) { TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, yan_shell_bind_blocks(&shell, &blocks)); }

static void shell_blocks_unbound_keeps_old_commands(void)
{
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, run("block list n"));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("ERROR UNKNOWN_COMMAND\r\n", output, "blocks_unbound_unknown");
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, run("help"));
    TEST_ASSERT_NULL(strstr((const char *)output, "block list"));
}
static void shell_blocks_list_and_show_golden(void)
{
    bind(); make("n", "@block a\nX\tY\r\nZ\n@block empty");
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, run("block list n"));
    TEST_ASSERT_EQUAL_STRING_MESSAGE(
        "BLOCK n#a marker=1 first=2 last=3 bytes=9:16\r\n"
        "BLOCK n#empty marker=4 first=0 last=0 bytes=28:28\r\n"
        "OK block list total=2 shown=2\r\n", output, "blocks_list_golden");
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, run("block show n#a"));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("2 X\\x09Y\r\n3 Z\r\nOK block show total=2 shown=2\r\n",
                                     output, "blocks_show_golden");
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, run("block show n#empty"));
    TEST_ASSERT_EQUAL_STRING("OK block show total=0 shown=0\r\n", output);
}
static void shell_blocks_refs_and_backlinks_golden(void)
{
    bind(); make("n", "@block a\nx"); make("s", "[[n#a]][[missing#x]][[n#gone]]");
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, run("block refs s"));
    TEST_ASSERT_EQUAL_STRING_MESSAGE(
        "REF source=s block=NONE line=1 bytes=0:7 target=n#a state=OK\r\n"
        "REF source=s block=NONE line=1 bytes=7:20 target=missing#x state=DANGLING_FILE\r\n"
        "REF source=s block=NONE line=1 bytes=20:30 target=n#gone state=DANGLING_BLOCK\r\n"
        "OK block refs total=3 shown=3\r\n", output, "blocks_refs_golden");
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, run("block backlinks missing#x"));
    TEST_ASSERT_EQUAL_STRING_MESSAGE(
        "BACKLINK source=s block=NONE line=1 bytes=7:20 target=missing#x\r\n"
        "OK block backlinks total=1 shown=1 skipped=0 target=DANGLING_FILE\r\n",
        output, "blocks_backlinks_golden");
}
static void shell_blocks_usage_zero_io(void)
{
    bind(); const char *bad[] = {"block", "block list", "block list n x", "block other n",
        "block show n", "block refs n#id", "block backlinks n#", "block show n#a#b"};
    uint32_t old = reads;
    for (uint32_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, run(bad[i]));
        TEST_ASSERT_EQUAL_STRING_MESSAGE("ERROR USAGE\r\n", output, "blocks_usage_error");
    }
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(old, reads, "blocks_usage_no_io");
}
static void shell_blocks_content_error_nonfatal_and_io_fatal(void)
{
    bind(); make("bad", "@block\n"); make("n", "@block a\nx");
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, run("block list bad"));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("ERROR BLOCK_FORMAT\r\n", output, "blocks_content_error");
    read_fail = reads + 1u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_FATAL, run("block list n"), "blocks_io_fatal");
    TEST_ASSERT_EQUAL_STRING("ERROR IO\r\n", output);
}
static void shell_blocks_output_failures_stop_without_io(void)
{
    bind(); make("n", "@block a\nx");
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, run("block list n"));
    uint32_t complete = length;
    const uint32_t points[] = {1, 12, complete};
    for (uint32_t i = 0; i < 3; ++i) {
        fail_at = points[i]; accept_failed_byte = i == 2;
        TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_FATAL, run("block list n"), "blocks_output_fatal");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(failure_reads, reads, "blocks_output_no_more_io");
        TEST_ASSERT_EQUAL_UINT32(points[i] - (accept_failed_byte ? 0u : 1u), length);
    }
}
static void shell_blocks_binding_and_line_aliases(void)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_INVALID, yan_shell_bind_blocks(&shell, &other), "blocks_uninitialized_bind");
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_INVALID, yan_shell_bind_blocks(&shell, (YanBlockContext *)&shell));
    bind(); uint32_t old = reads;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_INVALID,
        yan_shell_execute(&shell, blocks.buffer, 2), "blocks_line_alias_rejected");
    TEST_ASSERT_EQUAL_UINT32(old, reads);
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, yan_shell_bind_blocks(&shell, NULL));
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, run("block list n"));
    TEST_ASSERT_EQUAL_STRING("ERROR UNKNOWN_COMMAND\r\n", output);
}
static void shell_blocks_reentry_is_busy(void)
{
    bind(); make("n", "@block a\nx"); reenter = true;
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, run("block list n"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_BUSY, nested, "blocks_shell_reentry_busy");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SHELL_BUSY, nested_bind, "blocks_bind_reentry_busy");
}
static void shell_blocks_refs_top20_total(void)
{
    static char data[300 * 7 + 1];
    bind(); make("n", "@block a\nx");
    for (uint32_t i = 0; i < 300; ++i) memcpy(data + i * 7u, "[[n#a]]", 7);
    data[300 * 7] = 0; make("s", data);
    TEST_ASSERT_EQUAL_INT(YAN_SHELL_OK, run("block refs s"));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr((const char *)output, "OK block refs total=300 shown=20\r\n"),
                                 "blocks_shell_full_total");
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(shell_blocks_unbound_keeps_old_commands);
    RUN_TEST(shell_blocks_list_and_show_golden);
    RUN_TEST(shell_blocks_refs_and_backlinks_golden);
    RUN_TEST(shell_blocks_usage_zero_io);
    RUN_TEST(shell_blocks_content_error_nonfatal_and_io_fatal);
    RUN_TEST(shell_blocks_output_failures_stop_without_io);
    RUN_TEST(shell_blocks_binding_and_line_aliases);
    RUN_TEST(shell_blocks_reentry_is_busy);
    RUN_TEST(shell_blocks_refs_top20_total);
    return UNITY_END();
}
