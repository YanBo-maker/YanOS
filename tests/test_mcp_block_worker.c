/* D: yan_mcp_execute block domain must not need model/index/runner. */
#define _POSIX_C_SOURCE 200809L
#include "mcp_worker.h"
#include "unity.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BLOCKS 64u
static uint8_t medium[BLOCKS][4096];
static YanFsIoResult capacity(void *opaque, uint64_t *blocks)
{ (void)opaque; *blocks = BLOCKS; return YAN_FS_IO_OK; }
static YanFsIoResult read_block(void *opaque, uint32_t lba, uint8_t out[4096])
{
    (void)opaque;
    if (lba >= BLOCKS) return YAN_FS_IO_ERROR;
    memcpy(out, medium[lba], 4096);
    return YAN_FS_IO_OK;
}
static YanFsIoResult write_block(void *opaque, uint32_t lba, const uint8_t in[4096])
{
    (void)opaque;
    if (lba >= BLOCKS) return YAN_FS_IO_ERROR;
    memcpy(medium[lba], in, 4096);
    return YAN_FS_IO_OK;
}
static void make_image(char path[64])
{
    memset(medium, 0, sizeof medium);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_format_metadata(medium[0], BLOCKS));
    YanFs fs;
    YanFsBlockIo io = {NULL, capacity, read_block, write_block};
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_init(&fs, io));
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_mount(&fs));
    static const uint8_t data[] = "@block a\nhello\n";
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_create(&fs, "blocks.txt", data, sizeof data - 1));
    strcpy(path, "/tmp/yan-mcp-block-XXXXXX");
    int fd = mkstemp(path);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
    for (uint32_t i = 0; i < BLOCKS; ++i)
        TEST_ASSERT_EQUAL_INT(4096, (int)write(fd, medium[i], 4096));
    TEST_ASSERT_EQUAL_INT(0, close(fd));
}
void setUp(void) {}
void tearDown(void) {}
static void test_block_worker_no_model(void)
{
    char path[64]; make_image(path);
    YanMcpOptions options = {path, "/missing/index", "/missing/model",
                             "/missing/runner", 3, 3};
    YanMcpRequest request;
    memset(&request, 0, sizeof request);
    request.kind = YAN_MCP_BLOCK_LIST;
    strcpy(request.name, "blocks.txt");
    volatile sig_atomic_t cancel = 0;
    char *payload = NULL; size_t length = 0;
    YanMcpWorkerResult result = yan_mcp_execute(&options, &request, &cancel,
                                                &payload, &length);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_MCP_DOMAIN_BLOCK, result.domain,
                                  "block_worker_no_model_success");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_BLOCK_QUERY_OK, result.result,
                                  "block_worker_no_model_success");
    TEST_ASSERT_NOT_NULL_MESSAGE(payload, "block_worker_no_model_success");
    TEST_ASSERT_TRUE_MESSAGE(length > 0, "block_worker_no_model_success");
    yyjson_doc *doc = yyjson_read(payload, length, 0);
    TEST_ASSERT_NOT_NULL(doc);
    yyjson_val *root = yyjson_doc_get_root(doc);
    TEST_ASSERT_EQUAL_STRING("block_list",
        yyjson_get_str(yyjson_obj_get(root, "operation")));
    TEST_ASSERT_EQUAL_UINT64(1, yyjson_arr_size(yyjson_obj_get(root, "blocks")));
    yyjson_doc_free(doc);
    free(payload);
    unlink(path);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_block_worker_no_model);
    return UNITY_END();
}
