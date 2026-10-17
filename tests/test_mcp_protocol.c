#include "mcp_protocol.h"
#include "unity.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static YanMcpSession session;
static YanMcpRequest request;
static const char meta[] = "\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\",\"io.modelcontextprotocol/clientCapabilities\":{}}";
void setUp(void) { memset(&session, 0, sizeof session); }
void tearDown(void) {}
static void read_request(const char *json, const YanMcpId *active)
{ yan_mcp_parse(json, strlen(json), &session, active, &request, NULL); }
static void modern(const char *method, const char *extra)
{
    char input[4096];
    int n = snprintf(input, sizeof input,
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"%s\",\"params\":{%s%s}}", method, meta, extra);
    TEST_ASSERT_TRUE(n > 0 && (size_t)n < sizeof input);
    read_request(input, NULL);
}
static void test_modern_discovery_needs_no_initialize_or_client_info(void)
{
    modern("server/discover", "");
    TEST_ASSERT_EQUAL_INT(YAN_MCP_DISCOVER, request.kind);
    size_t length = 0;
    char *reply = yan_mcp_reply(&request, NULL, 0, NULL, &length, NULL);
    TEST_ASSERT_NOT_NULL(reply);
    yyjson_doc *doc = yyjson_read(reply, length, 0);
    TEST_ASSERT_NOT_NULL(doc);
    yyjson_val *result = yyjson_obj_get(yyjson_doc_get_root(doc), "result");
    TEST_ASSERT_EQUAL_STRING("complete", yyjson_get_str(yyjson_obj_get(result, "resultType")));
    TEST_ASSERT_EQUAL_STRING("2026-07-28", yyjson_get_str(yyjson_arr_get(yyjson_obj_get(result, "supportedVersions"), 0)));
    TEST_ASSERT_EQUAL_UINT64(0, yyjson_get_uint(yyjson_obj_get(result, "ttlMs")));
    TEST_ASSERT_EQUAL_STRING("private", yyjson_get_str(yyjson_obj_get(result, "cacheScope")));
    TEST_ASSERT_NOT_NULL(yyjson_obj_get(yyjson_obj_get(result, "_meta"), "io.modelcontextprotocol/serverInfo"));
    yyjson_doc_free(doc); free(reply);
}
static void test_modern_required_metadata_and_version_errors(void)
{
    read_request("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}", NULL);
    TEST_ASSERT_EQUAL_INT(-32602, request.error);
    read_request("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\"}}}",NULL);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-32602,request.error,"mcp_modern_required_caps");
    read_request("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":\"other\",\"io.modelcontextprotocol/clientCapabilities\":{}}}}", NULL);
    TEST_ASSERT_EQUAL_INT(-32022, request.error);
    modern("ping", ""); TEST_ASSERT_EQUAL_INT(-32601, request.error);
}
static void test_legacy_requires_initialized_and_does_not_inherit_modern(void)
{
    read_request("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{},\"clientInfo\":{\"name\":\"test\",\"version\":\"1\"}}}", NULL);
    TEST_ASSERT_EQUAL_INT(YAN_MCP_INITIALIZE, request.kind);
    read_request("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}", NULL);
    TEST_ASSERT_EQUAL_INT(-32602, request.error);
    read_request("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}", NULL);
    TEST_ASSERT_TRUE(session.legacy_ready);
    read_request("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"ping\"}", NULL);
    TEST_ASSERT_EQUAL_INT(YAN_MCP_PING, request.kind);
    TEST_ASSERT_FALSE(request.modern);
    modern("tools/list", ""); TEST_ASSERT_EQUAL_INT(YAN_MCP_LIST, request.kind);
    modern("ping", ""); TEST_ASSERT_EQUAL_INT(-32601, request.error);
}
static void test_ids_are_typed_and_current_id_cannot_be_reused(void)
{
    YanMcpId active = {.valid=true, .number=7};
    read_request("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\",\"params\":{\"requestId\":\"7\"}}", &active);
    TEST_ASSERT_EQUAL_INT(YAN_MCP_CANCEL, request.kind);
    TEST_ASSERT_FALSE_MESSAGE(yan_mcp_id_equal(&active, &request.target),"mcp_typed_id_cancel");
    read_request("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/list\"}", &active);
    TEST_ASSERT_EQUAL_INT(-32600, request.error);
    read_request("{\"jsonrpc\":\"2.0\",\"id\":9007199254740992,\"method\":\"ping\"}", NULL);
    TEST_ASSERT_EQUAL_INT(-32600, request.error);
    read_request("{\"jsonrpc\":\"2.0\",\"id\":null,\"method\":\"ping\"}", NULL);
    TEST_ASSERT_EQUAL_INT(-32600, request.error);
}
static void test_duplicate_keys_are_compared_after_unescaping(void)
{
    modern("tools/call", ",\"name\":\"yanos_search\",\"arguments\":{\"query\":\"cpu\",\"qu\\u0065ry\":\"disk\"}");
    TEST_ASSERT_EQUAL_INT_MESSAGE(-32700, request.error,"mcp_duplicate_decoded_key");
    read_request("{\"jsonrpc\":\"2.0\",\"id\":1,\"id\":2,\"method\":\"ping\"}", NULL);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-32700, request.error,"mcp_duplicate_request_id");
}
static void test_depth_is_bounded_before_json_decode(void)
{
    char input[256]; size_t n = 0;
    for (unsigned i=0; i<17; ++i) input[n++]='[';
    input[n++]='0'; for (unsigned i=0; i<17; ++i) input[n++]=']';
    yan_mcp_parse(input, n, &session, NULL, &request, NULL);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-32700, request.error,"mcp_depth_limit");
}
static void test_business_arguments_fail_without_dispatch(void)
{
    modern("tools/call", ",\"name\":\"yanos_search\",\"arguments\":{\"query\":\"cpu\\n\"}");
    TEST_ASSERT_EQUAL_INT(YAN_MCP_TOOL_ERROR, request.kind);
    modern("tools/call", ",\"name\":\"yanos_search\",\"arguments\":{\"query\":\"cpu\",\"path\":\"/tmp\"}");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_MCP_TOOL_ERROR, request.kind,"mcp_extra_business_args");
    modern("tools/call", ",\"name\":\"yanos_index_status\"");
    TEST_ASSERT_EQUAL_INT(YAN_MCP_STATUS, request.kind);
    modern("tools/call", ",\"name\":\"yanos_index_status\",\"arguments\":null");
    TEST_ASSERT_EQUAL_INT(-32602, request.error);
    modern("tools/call", ",\"name\":\"unknown\",\"arguments\":{}");
    TEST_ASSERT_EQUAL_INT(-32602, request.error);
}
static void test_utf8_query_limit_is_bytes_and_nul_cannot_bypass_it(void)
{
    char extra[3500]; strcpy(extra, ",\"name\":\"yanos_search\",\"arguments\":{\"query\":\"");
    for (unsigned i=0; i<341; ++i) strcat(extra, "中");
    strcat(extra, "\"}"); modern("tools/call", extra);
    TEST_ASSERT_EQUAL_INT(YAN_MCP_SEARCH, request.kind);
    TEST_ASSERT_EQUAL_UINT32(1023, strlen(request.query));
    extra[strlen(extra)-2]=0; strcat(extra, "a\"}"); modern("tools/call", extra);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_MCP_TOOL_ERROR, request.kind,"mcp_query_utf8_byte_limit");
    modern("tools/call", ",\"name\":\"yanos_search\",\"arguments\":{\"query\":\"cpu\\u0000tail\"}");
    TEST_ASSERT_EQUAL_INT(YAN_MCP_TOOL_ERROR, request.kind);
}
static void test_notifications_never_get_replies(void)
{
    read_request("{\"jsonrpc\":\"2.0\",\"method\":\"unknown\"}", NULL);
    TEST_ASSERT_EQUAL_INT(YAN_MCP_IGNORE, request.kind);
    read_request("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\",\"params\":{\"requestId\":null}}", NULL);
    TEST_ASSERT_EQUAL_INT(YAN_MCP_IGNORE, request.kind);
}
static void test_known_metadata_fields_cannot_hide_wrong_types(void)
{
    const char *bad[]={
        "{\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\",\"io.modelcontextprotocol/clientCapabilities\":{\"sampling\":false}}",
        "{\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\",\"io.modelcontextprotocol/clientCapabilities\":{},\"io.modelcontextprotocol/clientInfo\":{\"name\":\"x\",\"version\":\"1\",\"title\":7}}",
        "{\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\",\"io.modelcontextprotocol/clientCapabilities\":{\"extensions\":{\"vendor\":false}}}"
    };
    for (unsigned i=0;i<sizeof bad/sizeof *bad;++i) {
        char input[2048]; snprintf(input,sizeof input,"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"server/discover\",\"params\":{\"_meta\":%s}}",bad[i]);
        read_request(input,NULL); TEST_ASSERT_EQUAL_INT_MESSAGE(-32602,request.error,"mcp_known_metadata_type");
    }
    read_request("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{},\"clientInfo\":{\"name\":\"x\",\"version\":\"1\"},\"_meta\":7}}",NULL);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-32602,request.error,"mcp_legacy_metadata_type");
}
static void test_id_utf8_byte_boundary_and_container_depth_boundary(void)
{
    char input[4096],id[132];
    for (unsigned i=0;i<42;++i) memcpy(id+3*i,"中",3);
    memcpy(id+126,"ab",3);
    snprintf(input,sizeof input,"{\"jsonrpc\":\"2.0\",\"id\":\"%s\",\"method\":\"server/discover\",\"params\":{%s}}",id,meta);
    read_request(input,NULL); TEST_ASSERT_EQUAL_INT(YAN_MCP_DISCOVER,request.kind); TEST_ASSERT_EQUAL_UINT32(128,request.id.length);
    strcpy(id+128,"c");
    snprintf(input,sizeof input,"{\"jsonrpc\":\"2.0\",\"id\":\"%s\",\"method\":\"server/discover\",\"params\":{%s}}",id,meta);
    read_request(input,NULL); TEST_ASSERT_EQUAL_INT(-32600,request.error); TEST_ASSERT_FALSE(request.id.valid);
    char nested[64]; size_t at=0;
    for (unsigned i=0;i<13;++i) nested[at++]='[';
    nested[at++]='0'; for (unsigned i=0;i<13;++i) nested[at++]=']'; nested[at]=0;
    snprintf(input,sizeof input,"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"server/discover\",\"params\":{\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\",\"io.modelcontextprotocol/clientCapabilities\":{},\"vendor\":%s}}}",nested);
    read_request(input,NULL); TEST_ASSERT_EQUAL_INT(YAN_MCP_DISCOVER,request.kind);
}
static void test_tool_payload_is_unchanged_and_text_is_same_json(void)
{
    modern("tools/call", ",\"name\":\"yanos_search\",\"arguments\":{\"query\":\"cpu\"}");
    const char payload[]="{\"schema_version\":1,\"query\":\"cpu\",\"shown\":0,\"results\":[]}";
    size_t n=0; char *out=yan_mcp_reply(&request,payload,strlen(payload),NULL,&n,NULL);
    TEST_ASSERT_NOT_NULL(out); yyjson_doc *doc=yyjson_read(out,n,0);
    TEST_ASSERT_NOT_NULL(doc); yyjson_val *r=yyjson_obj_get(yyjson_doc_get_root(doc),"result");
    TEST_ASSERT_FALSE(yyjson_get_bool(yyjson_obj_get(r,"isError")));
    TEST_ASSERT_EQUAL_STRING(payload,yyjson_get_str(yyjson_obj_get(yyjson_arr_get(yyjson_obj_get(r,"content"),0),"text")));
    TEST_ASSERT_NULL(yyjson_obj_get(yyjson_obj_get(r,"structuredContent"),"resultType"));
    yyjson_doc_free(doc); free(out);
    out=yan_mcp_reply(&request,NULL,0,"STALE",&n,NULL);
    TEST_ASSERT_NOT_NULL(out); doc=yyjson_read(out,n,0); r=yyjson_obj_get(yyjson_doc_get_root(doc),"result");
    TEST_ASSERT_TRUE(yyjson_get_bool(yyjson_obj_get(r,"isError")));
    TEST_ASSERT_NULL(yyjson_obj_get(r,"structuredContent")); yyjson_doc_free(doc); free(out);
}
typedef struct { size_t calls,fail_at,live; bool hit; } Arena;
static void *allocate(void *opaque,size_t size)
{
    Arena *a=opaque;
    if (++a->calls==a->fail_at) { a->hit=true; return NULL; }
    void *p=malloc(size); if (p) ++a->live; return p;
}
static void *resize(void *opaque,void *old,size_t old_size,size_t size)
{
    (void)old_size; Arena *a=opaque;
    if (++a->calls==a->fail_at) { a->hit=true; return NULL; }
    bool fresh=old==NULL; void *p=realloc(old,size); if (p && fresh) ++a->live; return p;
}
static void release(void *opaque,void *p)
{ Arena *a=opaque; if (p) { --a->live; free(p); } }
static void test_each_reply_allocation_failure_is_closed_and_leak_free(void)
{
    const char payload[]="{\"state\":\"READY\",\"segments\":20,\"skipped\":0}";
    size_t total=0;
    for (unsigned mode=0;mode<3;++mode) {
        modern(mode==0?"tools/list":"tools/call",mode==0?"":",\"name\":\"yanos_index_status\"");
        Arena baseline={0}; yyjson_alc alc={allocate,resize,release,&baseline}; size_t n=0;
        char *out=yan_mcp_reply(&request,payload,sizeof payload-1,mode==2?"STALE":NULL,&n,&alc);
        TEST_ASSERT_NOT_NULL(out); release(&baseline,out); TEST_ASSERT_EQUAL_UINT32(0,baseline.live);
        total+=baseline.calls;
        for (size_t fail=1;fail<=baseline.calls;++fail) {
            Arena injected={.fail_at=fail}; alc.ctx=&injected;
            out=yan_mcp_reply(&request,payload,sizeof payload-1,mode==2?"STALE":NULL,&n,&alc);
            TEST_ASSERT_TRUE(injected.hit); TEST_ASSERT_NULL(out); TEST_ASSERT_EQUAL_UINT32(0,injected.live);
        }
    }
    printf("MCP reply allocation points=%zu (list/success/error)\n",total);
}
static void test_tool_result_exceeding_wire_limit_cannot_publish(void)
{
    modern("tools/call",",\"name\":\"yanos_index_status\"");
    char *payload=malloc(600012); TEST_ASSERT_NOT_NULL(payload);
    memcpy(payload,"{\"text\":\"",9); memset(payload+9,'x',600000); memcpy(payload+600009,"\"}",2);
    size_t length=0; char *out=yan_mcp_reply(&request,payload,600011,NULL,&length,NULL);
    TEST_ASSERT_NULL(out); TEST_ASSERT_EQUAL_UINT32(0,length); free(payload);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_modern_discovery_needs_no_initialize_or_client_info);
    RUN_TEST(test_modern_required_metadata_and_version_errors);
    RUN_TEST(test_legacy_requires_initialized_and_does_not_inherit_modern);
    RUN_TEST(test_ids_are_typed_and_current_id_cannot_be_reused);
    RUN_TEST(test_duplicate_keys_are_compared_after_unescaping);
    RUN_TEST(test_depth_is_bounded_before_json_decode);
    RUN_TEST(test_business_arguments_fail_without_dispatch);
    RUN_TEST(test_utf8_query_limit_is_bytes_and_nul_cannot_bypass_it);
    RUN_TEST(test_notifications_never_get_replies);
    RUN_TEST(test_known_metadata_fields_cannot_hide_wrong_types);
    RUN_TEST(test_id_utf8_byte_boundary_and_container_depth_boundary);
    RUN_TEST(test_tool_payload_is_unchanged_and_text_is_same_json);
    RUN_TEST(test_each_reply_allocation_failure_is_closed_and_leak_free);
    RUN_TEST(test_tool_result_exceeding_wire_limit_cannot_publish);
    return UNITY_END();
}
