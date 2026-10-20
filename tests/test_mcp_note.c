#define _POSIX_C_SOURCE 200809L

#include "mcp_note.h"
#include "mcp_protocol.h"
#include "mcp_worker.h"
#include "unity.h"
#include <yyjson.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char directory[256];
static char missing[320];
static char lock_path[352];

void setUp(void)
{
    snprintf(directory, sizeof directory, "/tmp/yan-mcp-note-XXXXXX");
    TEST_ASSERT_NOT_NULL(mkdtemp(directory));
    snprintf(missing, sizeof missing, "%s/missing.img", directory);
    snprintf(lock_path, sizeof lock_path, "%s.lock", missing);
}

void tearDown(void)
{
    (void)unlink(lock_path);
    (void)rmdir(directory);
}

static void assert_invalid(bool dry_run, const char *name, const uint8_t *text,
                           uint32_t length)
{
    YanMcpNoteRequest request = {
        .replace = false,
        .dry_run = dry_run,
        .has_expected = !dry_run,
        .name = name,
        .text = text,
        .length = length,
    };
    char *payload = (char *)1;
    size_t payload_length = 1;
    TEST_ASSERT_EQUAL(YAN_MCP_NOTE_INVALID,
                      yan_mcp_note_execute(missing, &request, &payload,
                                           &payload_length, NULL));
    TEST_ASSERT_NULL(payload);
    TEST_ASSERT_EQUAL_UINT32(0, payload_length);
    TEST_ASSERT_EQUAL_INT(-1, access(missing, F_OK));
    TEST_ASSERT_EQUAL_INT(-1, access(lock_path, F_OK));
}

static void test_0035_R7_AC1_invalid_text_and_name_stop_before_io(void)
{
    const uint8_t nul[] = {'a', 0, 'b'};
    const uint8_t cr[] = "a\rb";
    const uint8_t utf8[] = {0xff};
    static uint8_t big[16385];
    memset(big, 'a', sizeof big);
    const uint8_t ok[] = "ok\n";
    for (unsigned dry_run = 0; dry_run < 2; ++dry_run) {
        assert_invalid(dry_run, "a.txt", nul, sizeof nul);
        assert_invalid(dry_run, "a.txt", cr, sizeof cr);
        assert_invalid(dry_run, "a.txt", utf8, sizeof utf8);
        assert_invalid(dry_run, "a.txt", big, sizeof big);
        assert_invalid(dry_run, "bad/name", ok, sizeof ok - 1);
        assert_invalid(dry_run, "", ok, sizeof ok - 1);
        assert_invalid(dry_run, "a.txt", NULL, 1);
    }
}

static void test_0035_R10_AC5_preview_uses_the_same_preflight(void)
{
    const uint8_t cr[] = "a\rb";
    assert_invalid(true, "a.txt", cr, sizeof cr);
}

static void test_0035_R4_AC4_commit_requires_expected_source(void)
{
    const uint8_t text[] = "a\n";
    YanMcpNoteRequest request = {
        .replace = false,
        .has_expected = false,
        .name = "a.txt",
        .text = text,
        .length = sizeof text - 1,
    };
    char *payload = (char *)1;
    size_t length = 1;
    TEST_ASSERT_EQUAL(YAN_MCP_NOTE_INVALID,
                      yan_mcp_note_execute(missing, &request, &payload, &length, NULL));
    TEST_ASSERT_NULL(payload);
    TEST_ASSERT_EQUAL_INT(-1, access(missing, F_OK));
    TEST_ASSERT_EQUAL_INT(-1, access(lock_path, F_OK));
}

static void test_0035_R6_AC3_mcp_reply_exposes_committed(void)
{
    YanMcpRequest request;
    memset(&request, 0, sizeof request);
    request.kind = YAN_MCP_TOOL_ERROR;
    request.id.valid = true;
    request.id.number = 1;
    request.committed = true;
    char *reply = NULL;
    size_t length = 0;
    TEST_ASSERT_EQUAL(YAN_MCP_REPLY_OK,
                      yan_mcp_reply_ex(&request, NULL, 0, "IO", &reply, &length,
                                       NULL));
    TEST_ASSERT_NOT_NULL(reply);
    yyjson_doc *doc = yyjson_read(reply, length, 0);
    TEST_ASSERT_NOT_NULL(doc);
    yyjson_val *result = yyjson_obj_get(yyjson_doc_get_root(doc), "result");
    TEST_ASSERT_TRUE(yyjson_get_bool(yyjson_obj_get(result, "isError")));
    yyjson_val *content = yyjson_arr_get(yyjson_obj_get(result, "content"), 0);
    yyjson_doc *text = yyjson_read(yyjson_get_str(yyjson_obj_get(content, "text")),
                                   yyjson_get_len(yyjson_obj_get(content, "text")), 0);
    TEST_ASSERT_NOT_NULL(text);
    yyjson_val *value = yyjson_doc_get_root(text);
    TEST_ASSERT_EQUAL_STRING("IO", yyjson_get_str(yyjson_obj_get(value, "code")));
    TEST_ASSERT_TRUE(yyjson_get_bool(yyjson_obj_get(value, "committed")));
    TEST_ASSERT_NULL(yyjson_obj_get(result, "structuredContent"));
    yyjson_doc_free(text);
    yyjson_doc_free(doc);
    free(reply);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_0035_R7_AC1_invalid_text_and_name_stop_before_io);
    RUN_TEST(test_0035_R10_AC5_preview_uses_the_same_preflight);
    RUN_TEST(test_0035_R4_AC4_commit_requires_expected_source);
    RUN_TEST(test_0035_R6_AC3_mcp_reply_exposes_committed);
    return UNITY_END();
}
