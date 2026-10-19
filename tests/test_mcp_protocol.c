#include "mcp_protocol.h"
#include "mcp_worker.h"
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
static void test_six_tools_schema_order_and_version(void)
{
    modern("tools/list", "");
    size_t length=0; char *reply=yan_mcp_reply(&request,NULL,0,NULL,&length,NULL);
    TEST_ASSERT_NOT_NULL(reply); yyjson_doc *doc=yyjson_read(reply,length,0);
    TEST_ASSERT_NOT_NULL(doc);
    yyjson_val *result=yyjson_obj_get(yyjson_doc_get_root(doc),"result");
    yyjson_val *tools=yyjson_obj_get(result,"tools");
    TEST_ASSERT_TRUE(yyjson_is_arr(tools));
    TEST_ASSERT_EQUAL_UINT64(6,yyjson_arr_size(tools));
    const char *names[]={"yanos_search","yanos_index_status","yanos_block_list",
        "yanos_block_read","yanos_block_refs","yanos_block_backlinks"};
    for (size_t i=0;i<6;++i) {
        yyjson_val *tool=yyjson_arr_get(tools,i);
        TEST_ASSERT_EQUAL_STRING(names[i],yyjson_get_str(yyjson_obj_get(tool,"name")));
        TEST_ASSERT_TRUE(yyjson_is_obj(yyjson_obj_get(tool,"inputSchema")));
        TEST_ASSERT_FALSE(yyjson_get_bool(yyjson_obj_get(yyjson_obj_get(tool,"inputSchema"),"additionalProperties")));
        TEST_ASSERT_TRUE(yyjson_is_obj(yyjson_obj_get(tool,"outputSchema")));
        yyjson_val *annotations=yyjson_obj_get(tool,"annotations");
        TEST_ASSERT_TRUE(yyjson_get_bool(yyjson_obj_get(annotations,"readOnlyHint")));
        TEST_ASSERT_FALSE(yyjson_get_bool(yyjson_obj_get(annotations,"destructiveHint")));
        TEST_ASSERT_TRUE(yyjson_get_bool(yyjson_obj_get(annotations,"idempotentHint")));
        TEST_ASSERT_FALSE(yyjson_get_bool(yyjson_obj_get(annotations,"openWorldHint")));
    }
    yyjson_val *server=yyjson_obj_get(yyjson_obj_get(result,"_meta"),
        "io.modelcontextprotocol/serverInfo");
    TEST_ASSERT_EQUAL_STRING("0033",yyjson_get_str(yyjson_obj_get(server,"version")));
    yyjson_doc_free(doc); free(reply);
}
static yyjson_val *tool_named(yyjson_val *tools,const char *name)
{
    size_t i,max; yyjson_val *tool;
    yyjson_arr_foreach(tools,i,max,tool)
        if (!strcmp(yyjson_get_str(yyjson_obj_get(tool,"name")),name)) return tool;
    return NULL;
}
static void assert_required(yyjson_val *schema,const char *const *names,size_t count)
{
    yyjson_val *required=yyjson_obj_get(schema,"required");
    TEST_ASSERT_TRUE(yyjson_is_arr(required));
    TEST_ASSERT_EQUAL_UINT64(count,yyjson_arr_size(required));
    for (size_t i=0;i<count;++i)
        TEST_ASSERT_EQUAL_STRING(names[i],yyjson_get_str(yyjson_arr_get(required,i)));
}
static void assert_properties(yyjson_val *schema,const char *const *names,size_t count)
{
    TEST_ASSERT_FALSE(yyjson_get_bool(yyjson_obj_get(schema,"additionalProperties")));
    yyjson_val *properties=yyjson_obj_get(schema,"properties");
    TEST_ASSERT_TRUE(yyjson_is_obj(properties));
    TEST_ASSERT_EQUAL_UINT64(count,yyjson_obj_size(properties));
    for (size_t i=0;i<count;++i) TEST_ASSERT_NOT_NULL(yyjson_obj_get(properties,names[i]));
}
static void assert_type(yyjson_val *schema,const char *type)
{
    TEST_ASSERT_EQUAL_STRING(type,yyjson_get_str(yyjson_obj_get(schema,"type")));
}
static void assert_integer(yyjson_val *schema)
{
    assert_type(schema,"integer");
    TEST_ASSERT_EQUAL_UINT64(0,yyjson_get_uint(yyjson_obj_get(schema,"minimum")));
}
static void assert_pattern(yyjson_val *schema,const char *pattern)
{
    TEST_ASSERT_EQUAL_STRING(pattern,yyjson_get_str(yyjson_obj_get(schema,"pattern")));
}
static void assert_common_output(yyjson_val *properties)
{
    yyjson_val *version=yyjson_obj_get(properties,"schema_version");
    yyjson_val *source=yyjson_obj_get(properties,"source_sha256");
    TEST_ASSERT_EQUAL_UINT64(1,yyjson_get_uint(yyjson_obj_get(version,"const")));
    assert_pattern(source,"^[0-9a-f]{64}$");
}
static void assert_nullable_integer(yyjson_val *schema)
{
    yyjson_val *type=yyjson_obj_get(schema,"type");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(2,yyjson_arr_size(type),"block_response_schema");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("integer",yyjson_get_str(yyjson_arr_get(type,0)),
                                     "block_response_schema");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("null",yyjson_get_str(yyjson_arr_get(type,1)),
                                     "block_response_schema");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(0,yyjson_get_uint(yyjson_obj_get(schema,"minimum")),
                                     "block_response_schema");
}
static void assert_nullable_string(yyjson_val *schema)
{
    yyjson_val *type=yyjson_obj_get(schema,"type");
    TEST_ASSERT_TRUE(yyjson_is_arr(type));
    TEST_ASSERT_EQUAL_UINT64(2,yyjson_arr_size(type));
    TEST_ASSERT_EQUAL_STRING("string",yyjson_get_str(yyjson_arr_get(type,0)));
    TEST_ASSERT_EQUAL_STRING("null",yyjson_get_str(yyjson_arr_get(type,1)));
}
static void assert_enum(yyjson_val *schema,const char *const *values,size_t count)
{
    yyjson_val *enumeration=yyjson_obj_get(schema,"enum");
    TEST_ASSERT_TRUE(yyjson_is_arr(enumeration));
    TEST_ASSERT_EQUAL_UINT64(count,yyjson_arr_size(enumeration));
    for (size_t i=0;i<count;++i)
        TEST_ASSERT_EQUAL_STRING(values[i],yyjson_get_str(yyjson_arr_get(enumeration,i)));
}
static void assert_top_closed(yyjson_val *schema)
{
    TEST_ASSERT_FALSE(yyjson_get_bool(yyjson_obj_get(schema,"additionalProperties")));
}
static void assert_schema_equal_named(yyjson_val *actual,const char *literal,
                                     const char *marker)
{
    yyjson_doc *expected=yyjson_read(literal,strlen(literal),0);
    TEST_ASSERT_NOT_NULL(expected);
    TEST_ASSERT_TRUE_MESSAGE(yyjson_equals(actual,yyjson_doc_get_root(expected)),marker);
    yyjson_doc_free(expected);
}
static void assert_schema_equal(yyjson_val *actual,const char *literal)
{
    yyjson_doc *expected=yyjson_read(literal,strlen(literal),0);
    TEST_ASSERT_NOT_NULL(expected);
    TEST_ASSERT_TRUE(yyjson_equals(actual,yyjson_doc_get_root(expected)));
    yyjson_doc_free(expected);
}
static void test_six_tools_deep_schemas(void)
{
    modern("tools/list","");
    size_t length=0; char *reply=yan_mcp_reply(&request,NULL,0,NULL,&length,NULL);
    TEST_ASSERT_NOT_NULL(reply);
    yyjson_doc *doc=yyjson_read(reply,length,0); TEST_ASSERT_NOT_NULL(doc);
    yyjson_val *tools=yyjson_obj_get(yyjson_obj_get(yyjson_doc_get_root(doc),"result"),"tools");
    TEST_ASSERT_TRUE(yyjson_is_arr(tools)); TEST_ASSERT_EQUAL_UINT64(6,yyjson_arr_size(tools));
    static const char *const search_in[]={"query"};
    static const char *const search_out[]={"schema_version","query","source_sha256","model_sha256",
        "runner_sha256","config_sha256","segments","skipped","shown","results"};
    static const char *const search_item[]={"file","first_line","last_line","start","end","score","text"};
    static const char *const list_in[]={"name","expected_source"};
    static const char *const list_out[]={"schema_version","operation","source_sha256","name",
        "total","shown","blocks"};
    static const char *const list_item[]={"id","marker_line","first_line","last_line","start","end"};
    static const char *const read_in[]={"name","block_id","offset","expected_source"};
    static const char *const read_out[]={"schema_version","operation","source_sha256","name",
        "block_id","block","page","text","complete","next_offset"};
    static const char *const range[]={"start","end","first_line","last_line"};
    static const char *const refs_out[]={"schema_version","operation","source_sha256","name",
        "total","shown","refs"};
    static const char *const refs_item[]={"source","block_id","line","start","end","target","state"};
    static const char *const target[]={"name","id"};
    static const char *const backlinks_out[]={"schema_version","operation","source_sha256","name",
        "block_id","total","shown","skipped","target_state","backlinks"};
    static const char *const backlinks_item[]={"source","block_id","line","start","end"};

    yyjson_val *search=tool_named(tools,"yanos_search");
    TEST_ASSERT_NOT_NULL(search);
    assert_schema_equal(yyjson_obj_get(search,"inputSchema"),
        "{\"type\":\"object\",\"additionalProperties\":false,\"required\":[\"query\"],"
        "\"properties\":{\"query\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":1023}}}");
    assert_properties(yyjson_obj_get(search,"inputSchema"),search_in,1);
    assert_required(yyjson_obj_get(search,"inputSchema"),search_in,1);
    yyjson_val *search_output=yyjson_obj_get(search,"outputSchema");
    static const char search_output_json[]="{\"type\":\"object\",\"additionalProperties\":false,\"required\":[\"schema_version\",\"query\",\"source_sha256\",\"model_sha256\",\"runner_sha256\",\"config_sha256\",\"segments\",\"skipped\",\"shown\",\"results\"],\"properties\":{\"schema_version\":{\"const\":1},\"query\":{\"type\":\"string\"},\"source_sha256\":{\"type\":\"string\",\"pattern\":\"^[0-9a-f]{64}$\"},\"model_sha256\":{\"type\":\"string\",\"pattern\":\"^[0-9a-f]{64}$\"},\"runner_sha256\":{\"type\":\"string\",\"pattern\":\"^[0-9a-f]{64}$\"},\"config_sha256\":{\"type\":\"string\",\"pattern\":\"^[0-9a-f]{64}$\"},\"segments\":{\"type\":\"integer\",\"minimum\":0},\"skipped\":{\"type\":\"integer\",\"minimum\":0},\"shown\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":20},\"results\":{\"type\":\"array\",\"maxItems\":20,\"items\":{\"type\":\"object\",\"additionalProperties\":false,\"required\":[\"file\",\"first_line\",\"last_line\",\"start\",\"end\",\"score\",\"text\"],\"properties\":{\"file\":{\"type\":\"string\"},\"first_line\":{\"type\":\"integer\",\"minimum\":1},\"last_line\":{\"type\":\"integer\",\"minimum\":1},\"start\":{\"type\":\"integer\",\"minimum\":0},\"end\":{\"type\":\"integer\",\"minimum\":0},\"score\":{\"type\":\"number\"},\"text\":{\"type\":\"string\"}}}}}}";
    assert_schema_equal_named(search_output,search_output_json,"block_response_schema");
    assert_top_closed(search_output);
    assert_required(search_output,search_out,sizeof search_out/sizeof *search_out);
    assert_common_output(yyjson_obj_get(search_output,"properties"));
    yyjson_val *results=yyjson_obj_get(yyjson_obj_get(search_output,"properties"),"results");
    TEST_ASSERT_EQUAL_UINT64(20,yyjson_get_uint(yyjson_obj_get(results,"maxItems")));
    assert_required(yyjson_obj_get(results,"items"),search_item,sizeof search_item/sizeof *search_item);
    assert_properties(yyjson_obj_get(results,"items"),search_item,sizeof search_item/sizeof *search_item);
    yyjson_val *search_item_props=yyjson_obj_get(yyjson_obj_get(results,"items"),"properties");
    assert_type(yyjson_obj_get(search_item_props,"file"),"string");
    assert_type(yyjson_obj_get(search_item_props,"score"),"number");
    assert_type(yyjson_obj_get(search_item_props,"text"),"string");
    assert_integer(yyjson_obj_get(search_item_props,"start"));
    assert_integer(yyjson_obj_get(search_item_props,"end"));
    TEST_ASSERT_EQUAL_UINT64(1,yyjson_get_uint(yyjson_obj_get(yyjson_obj_get(search_item_props,"first_line"),"minimum")));

    yyjson_val *status=tool_named(tools,"yanos_index_status");
    TEST_ASSERT_NOT_NULL(status);
    yyjson_val *status_in=yyjson_obj_get(status,"inputSchema");
    assert_schema_equal(status_in,
        "{\"type\":\"object\",\"additionalProperties\":false}");
    TEST_ASSERT_NULL(yyjson_obj_get(status_in,"required"));
    TEST_ASSERT_NULL(yyjson_obj_get(status_in,"properties"));
    TEST_ASSERT_EQUAL_UINT64(2,yyjson_obj_size(status_in));
    TEST_ASSERT_FALSE(yyjson_get_bool(yyjson_obj_get(status_in,"additionalProperties")));
    yyjson_val *status_out=yyjson_obj_get(status,"outputSchema");
    assert_schema_equal(status_out,
        "{\"type\":\"object\",\"oneOf\":["
        "{\"additionalProperties\":false,\"required\":[\"state\",\"segments\",\"skipped\"],"
        "\"properties\":{\"state\":{\"const\":\"READY\"},\"segments\":{\"type\":\"integer\",\"minimum\":0},"
        "\"skipped\":{\"type\":\"integer\",\"minimum\":0}}},"
        "{\"additionalProperties\":false,\"required\":[\"state\"],"
        "\"properties\":{\"state\":{\"enum\":[\"MISSING\",\"STALE\",\"CORRUPT\"]}}}]}");
    TEST_ASSERT_NULL(yyjson_obj_get(status_out,"additionalProperties"));
    yyjson_val *one_of=yyjson_obj_get(status_out,"oneOf");
    TEST_ASSERT_TRUE(yyjson_is_arr(one_of)); TEST_ASSERT_EQUAL_UINT64(2,yyjson_arr_size(one_of));
    static const char *const ready_required[]={"state","segments","skipped"};
    static const char *const missing_required[]={"state"};
    static const char *const status_states[]={"MISSING","STALE","CORRUPT"};
    assert_required(yyjson_arr_get(one_of,0),ready_required,3);
    assert_required(yyjson_arr_get(one_of,1),missing_required,1);
    TEST_ASSERT_NULL(yyjson_obj_get(yyjson_arr_get(one_of,0),"type"));
    TEST_ASSERT_NULL(yyjson_obj_get(yyjson_arr_get(one_of,1),"type"));
    TEST_ASSERT_FALSE(yyjson_get_bool(yyjson_obj_get(yyjson_arr_get(one_of,0),"additionalProperties")));
    TEST_ASSERT_FALSE(yyjson_get_bool(yyjson_obj_get(yyjson_arr_get(one_of,1),"additionalProperties")));
    assert_enum(yyjson_obj_get(yyjson_obj_get(yyjson_arr_get(one_of,1),"properties"),"state"),
                status_states,3);
    assert_integer(yyjson_obj_get(yyjson_obj_get(yyjson_arr_get(one_of,0),"properties"),"segments"));
    assert_integer(yyjson_obj_get(yyjson_obj_get(yyjson_arr_get(one_of,0),"properties"),"skipped"));
    TEST_ASSERT_EQUAL_STRING("READY",yyjson_get_str(yyjson_obj_get(
        yyjson_obj_get(yyjson_obj_get(yyjson_arr_get(one_of,0),"properties"),"state"),"const")));

    yyjson_val *list=tool_named(tools,"yanos_block_list");
    assert_properties(yyjson_obj_get(list,"inputSchema"),list_in,2);
    assert_required(yyjson_obj_get(list,"inputSchema"),list_in,1);
    assert_top_closed(yyjson_obj_get(list,"outputSchema"));
    assert_required(yyjson_obj_get(list,"outputSchema"),list_out,sizeof list_out/sizeof *list_out);
    yyjson_val *list_props=yyjson_obj_get(yyjson_obj_get(list,"outputSchema"),"properties");
    assert_common_output(list_props);
    yyjson_val *list_input_props=yyjson_obj_get(yyjson_obj_get(list,"inputSchema"),"properties");
    assert_type(yyjson_obj_get(list_input_props,"name"),"string");
    assert_pattern(yyjson_obj_get(list_input_props,"expected_source"),"^[0-9a-f]{64}$");
    yyjson_val *list_blocks=yyjson_obj_get(list_props,"blocks");
    TEST_ASSERT_EQUAL_UINT64(64,yyjson_get_uint(yyjson_obj_get(list_blocks,"maxItems")));
    assert_required(yyjson_obj_get(list_blocks,"items"),list_item,sizeof list_item/sizeof *list_item);
    assert_properties(yyjson_obj_get(list_blocks,"items"),list_item,sizeof list_item/sizeof *list_item);
    yyjson_val *list_item_props=yyjson_obj_get(yyjson_obj_get(list_blocks,"items"),"properties");
    assert_type(yyjson_obj_get(list_item_props,"id"),"string");
    assert_integer(yyjson_obj_get(list_item_props,"marker_line"));
    assert_integer(yyjson_obj_get(list_item_props,"first_line"));
    assert_integer(yyjson_obj_get(list_item_props,"last_line"));
    assert_integer(yyjson_obj_get(list_item_props,"start"));
    assert_integer(yyjson_obj_get(list_item_props,"end"));
    assert_integer(yyjson_obj_get(list_props,"total"));
    assert_integer(yyjson_obj_get(list_props,"shown"));
    yyjson_val *list_output=yyjson_obj_get(list,"outputSchema");
    yyjson_val *list_properties=yyjson_obj_get(list_output,"properties");
    TEST_ASSERT_NOT_NULL(list_output); TEST_ASSERT_NOT_NULL(list_properties);
    yyjson_val *list_operation=yyjson_obj_get(list_properties,"operation");
    TEST_ASSERT_NOT_NULL(list_operation);
    TEST_ASSERT_EQUAL_STRING("block_list",yyjson_get_str(yyjson_obj_get(list_operation,"const")));

    yyjson_val *read=tool_named(tools,"yanos_block_read");
    assert_properties(yyjson_obj_get(read,"inputSchema"),read_in,4);
    assert_required(yyjson_obj_get(read,"inputSchema"),read_in,2);
    yyjson_val *read_offset=yyjson_obj_get(yyjson_obj_get(read,"inputSchema"),"properties");
    assert_type(yyjson_obj_get(read_offset,"name"),"string");
    assert_type(yyjson_obj_get(read_offset,"block_id"),"string");
    assert_integer(yyjson_obj_get(read_offset,"offset"));
    TEST_ASSERT_EQUAL_UINT64(4294967295,yyjson_get_uint(yyjson_obj_get(yyjson_obj_get(read_offset,"offset"),"maximum")));
    yyjson_val *read_output=yyjson_obj_get(read,"outputSchema");
    assert_top_closed(read_output);
    assert_required(read_output,read_out,sizeof read_out/sizeof *read_out);
    yyjson_val *read_props=yyjson_obj_get(read_output,"properties");
    assert_common_output(read_props);
    assert_pattern(yyjson_obj_get(yyjson_obj_get(yyjson_obj_get(read,"inputSchema"),"properties"),"expected_source"),"^[0-9a-f]{64}$");
    TEST_ASSERT_EQUAL_STRING("block_read",yyjson_get_str(yyjson_obj_get(yyjson_obj_get(read_props,"operation"),"const")));
    assert_required(yyjson_obj_get(read_props,"block"),range,4);
    assert_properties(yyjson_obj_get(read_props,"block"),range,4);
    assert_required(yyjson_obj_get(read_props,"page"),range,4);
    assert_properties(yyjson_obj_get(read_props,"page"),range,4);
    assert_type(yyjson_obj_get(read_props,"text"),"string");
    assert_type(yyjson_obj_get(read_props,"complete"),"boolean");
    assert_type(yyjson_obj_get(read_props,"name"),"string");
    assert_type(yyjson_obj_get(read_props,"block_id"),"string");
    yyjson_val *next_offset=yyjson_obj_get(read_props,"next_offset");
    assert_nullable_integer(next_offset);
    for (unsigned i=0;i<4;++i) assert_integer(yyjson_obj_get(
        yyjson_obj_get(yyjson_obj_get(read_props,"block"),"properties"),range[i]));
    for (unsigned i=0;i<4;++i) assert_integer(yyjson_obj_get(
        yyjson_obj_get(yyjson_obj_get(read_props,"page"),"properties"),range[i]));

    yyjson_val *refs=tool_named(tools,"yanos_block_refs");
    assert_properties(yyjson_obj_get(refs,"inputSchema"),list_in,2);
    assert_required(yyjson_obj_get(refs,"inputSchema"),list_in,1);
    yyjson_val *refs_input_props=yyjson_obj_get(yyjson_obj_get(refs,"inputSchema"),"properties");
    assert_type(yyjson_obj_get(refs_input_props,"name"),"string");
    assert_pattern(yyjson_obj_get(refs_input_props,"expected_source"),"^[0-9a-f]{64}$");
    yyjson_val *refs_output=yyjson_obj_get(refs,"outputSchema");
    assert_top_closed(refs_output);
    assert_required(refs_output,refs_out,sizeof refs_out/sizeof *refs_out);
    yyjson_val *refs_props_out=yyjson_obj_get(refs_output,"properties");
    assert_common_output(refs_props_out);
    yyjson_val *refs_array=yyjson_obj_get(refs_props_out,"refs");
    TEST_ASSERT_EQUAL_UINT64(20,yyjson_get_uint(yyjson_obj_get(refs_array,"maxItems")));
    yyjson_val *refs_items=yyjson_obj_get(refs_array,"items");
    assert_required(refs_items,refs_item,sizeof refs_item/sizeof *refs_item);
    assert_properties(refs_items,refs_item,sizeof refs_item/sizeof *refs_item);
    yyjson_val *refs_props=yyjson_obj_get(refs_items,"properties");
    assert_nullable_string(yyjson_obj_get(refs_props,"block_id"));
    assert_required(yyjson_obj_get(refs_props,"target"),target,2);
    assert_properties(yyjson_obj_get(refs_props,"target"),target,2);
    assert_type(yyjson_obj_get(refs_props,"source"),"string");
    for (unsigned i=0;i<3;++i) assert_integer(yyjson_obj_get(refs_props, (const char *[]){"line","start","end"}[i]));
    static const char *const target_states[]={"OK","DANGLING_FILE","DANGLING_BLOCK"};
    assert_enum(yyjson_obj_get(refs_props,"state"),target_states,3);
    assert_type(yyjson_obj_get(yyjson_obj_get(yyjson_obj_get(refs_props,"target"),"properties"),"name"),"string");
    TEST_ASSERT_EQUAL_STRING("block_refs",yyjson_get_str(yyjson_obj_get(yyjson_obj_get(yyjson_obj_get(refs_output,"properties"),"operation"),"const")));

    yyjson_val *backlinks=tool_named(tools,"yanos_block_backlinks");
    static const char *const backlinks_in[]={"name","block_id","expected_source"};
    assert_properties(yyjson_obj_get(backlinks,"inputSchema"),backlinks_in,3);
    assert_required(yyjson_obj_get(backlinks,"inputSchema"),backlinks_in,2);
    yyjson_val *backlinks_input=yyjson_obj_get(yyjson_obj_get(backlinks,"inputSchema"),"properties");
    assert_type(yyjson_obj_get(backlinks_input,"name"),"string");
    assert_type(yyjson_obj_get(backlinks_input,"block_id"),"string");
    assert_pattern(yyjson_obj_get(backlinks_input,"expected_source"),"^[0-9a-f]{64}$");
    TEST_ASSERT_NULL(yyjson_obj_get(backlinks_input,"offset"));
    yyjson_val *backlinks_output=yyjson_obj_get(backlinks,"outputSchema");
    assert_top_closed(backlinks_output);
    assert_required(backlinks_output,backlinks_out,sizeof backlinks_out/sizeof *backlinks_out);
    yyjson_val *backlinks_props=yyjson_obj_get(backlinks_output,"properties");
    assert_common_output(backlinks_props);
    TEST_ASSERT_EQUAL_STRING("block_backlinks",yyjson_get_str(yyjson_obj_get(yyjson_obj_get(backlinks_props,"operation"),"const")));
    yyjson_val *backlinks_array=yyjson_obj_get(backlinks_props,"backlinks");
    TEST_ASSERT_EQUAL_UINT64(20,yyjson_get_uint(yyjson_obj_get(backlinks_array,"maxItems")));
    yyjson_val *backlinks_items=yyjson_obj_get(backlinks_array,"items");
    assert_required(backlinks_items,backlinks_item,sizeof backlinks_item/sizeof *backlinks_item);
    assert_properties(backlinks_items,backlinks_item,sizeof backlinks_item/sizeof *backlinks_item);
    yyjson_val *backlinks_item_props=yyjson_obj_get(backlinks_items,"properties");
    assert_nullable_string(yyjson_obj_get(backlinks_item_props,"block_id"));
    assert_type(yyjson_obj_get(backlinks_item_props,"source"),"string");
    assert_integer(yyjson_obj_get(backlinks_item_props,"line"));
    assert_integer(yyjson_obj_get(backlinks_item_props,"start"));
    assert_integer(yyjson_obj_get(backlinks_item_props,"end"));
    static const char *const backlinks_states[]={"OK","DANGLING_FILE","DANGLING_BLOCK"};
    assert_enum(yyjson_obj_get(backlinks_props,"target_state"),backlinks_states,3);
    assert_integer(yyjson_obj_get(backlinks_props,"total"));
    assert_integer(yyjson_obj_get(backlinks_props,"shown"));
    assert_integer(yyjson_obj_get(backlinks_props,"skipped"));
    yyjson_doc_free(doc); free(reply);
}
static void test_block_argument_parsing(void)
{
    const char *hex="000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
    char extra[512];
    snprintf(extra,sizeof extra,
        ",\"name\":\"yanos_block_list\",\"arguments\":{\"name\":\"note\",\"expected_source\":\"%s\"}",hex);
    modern("tools/call",extra);
    TEST_ASSERT_EQUAL_INT(YAN_MCP_BLOCK_LIST,request.kind);
    TEST_ASSERT_EQUAL_STRING("note",request.name);
    TEST_ASSERT_TRUE(request.has_expected_source);
    for (unsigned i=0;i<32;++i) TEST_ASSERT_EQUAL_UINT8(i,request.expected_source[i]);

    modern("tools/call",",\"name\":\"yanos_block_refs\",\"arguments\":{\"name\":\"note\"}");
    TEST_ASSERT_EQUAL_INT(YAN_MCP_BLOCK_REFS,request.kind);
    snprintf(extra,sizeof extra,
        ",\"name\":\"yanos_block_read\",\"arguments\":{\"name\":\"note\",\"block_id\":\"b\",\"offset\":17,\"expected_source\":\"%s\"}",hex);
    modern("tools/call",extra);
    TEST_ASSERT_EQUAL_INT(YAN_MCP_BLOCK_READ,request.kind);
    TEST_ASSERT_TRUE(request.has_offset);
    TEST_ASSERT_EQUAL_UINT32(17,request.offset);
    TEST_ASSERT_EQUAL_STRING("b",request.block_id);
    modern("tools/call",",\"name\":\"yanos_block_backlinks\",\"arguments\":{\"name\":\"note\",\"block_id\":\"b\"}");
    TEST_ASSERT_EQUAL_INT(YAN_MCP_BLOCK_BACKLINKS,request.kind);

    const char *bad[]={
        ",\"name\":\"yanos_block_list\"",
        ",\"name\":\"yanos_block_list\",\"arguments\":{}",
        ",\"name\":\"yanos_block_list\",\"arguments\":{\"name\":\"note\",\"extra\":1}",
        ",\"name\":\"yanos_block_list\",\"arguments\":{\"name\":\"a/b\"}",
        ",\"name\":\"yanos_block_list\",\"arguments\":{\"name\":\"a\\u0000b\"}",
        ",\"name\":\"yanos_block_list\",\"arguments\":{\"name\":\"abcdefghijklmnopqrstuvwxyz123456\"}",
        ",\"name\":\"yanos_block_read\",\"arguments\":{\"name\":\"note\"}",
        ",\"name\":\"yanos_block_read\",\"arguments\":{\"name\":\"note\",\"block_id\":\"a/b\"}",
        ",\"name\":\"yanos_block_read\",\"arguments\":{\"name\":\"note\",\"block_id\":\"a\",\"offset\":true}",
        ",\"name\":\"yanos_block_read\",\"arguments\":{\"name\":\"note\",\"block_id\":\"a\",\"offset\":1.5}",
        ",\"name\":\"yanos_block_read\",\"arguments\":{\"name\":\"note\",\"block_id\":\"a\",\"offset\":-1}",
        ",\"name\":\"yanos_block_read\",\"arguments\":{\"name\":\"note\",\"block_id\":\"a\",\"offset\":4294967296}",
        ",\"name\":\"yanos_block_read\",\"arguments\":{\"name\":\"note\",\"block_id\":\"a\",\"offset\":1}",
        ",\"name\":\"yanos_block_backlinks\",\"arguments\":{\"name\":\"note\",\"block_id\":\"a\",\"offset\":0}",
        ",\"name\":\"yanos_block_list\",\"arguments\":{\"name\":\"note\",\"expected_source\":\"ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789\"}",
        ",\"name\":\"yanos_block_list\",\"arguments\":{\"name\":\"note\",\"expected_source\":\"00\"}",
        ",\"name\":\"yanos_block_list\",\"arguments\":{\"name\":\"note\",\"_meta\":{}}",
        ",\"name\":\"yanos_block_read\",\"arguments\":{\"name\":\"note\",\"block_id\":\"a\",\"_meta\":{}}",
        ",\"name\":\"yanos_block_refs\",\"arguments\":{\"name\":\"note\",\"_meta\":{}}",
        ",\"name\":\"yanos_block_backlinks\",\"arguments\":{\"name\":\"note\",\"block_id\":\"a\",\"_meta\":{}}"
    };
    for (unsigned i=0;i<sizeof bad/sizeof *bad;++i) {
        modern("tools/call",bad[i]);
        TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_MCP_TOOL_ERROR,request.kind,
                                      "mcp_block_invalid_arguments");
        TEST_ASSERT_EQUAL_INT(0,request.error);
    }
    modern("tools/call",",\"name\":\"yanos_block_read\",\"arguments\":{\"name\":\"note\",\"block_id\":\"a\",\"offset\":0}");
    TEST_ASSERT_EQUAL_INT(YAN_MCP_BLOCK_READ,request.kind);
    modern("tools/call",",\"name\":\"yanos_block_list\",\"arguments\":null");
    TEST_ASSERT_EQUAL_INT(-32602,request.error);
}
static void test_reply_status_limit_and_io(void)
{
    modern("tools/call",",\"name\":\"yanos_index_status\"");
    char *out=(char *)1; size_t length=99;
    char *payload=malloc(YAN_MCP_OUTPUT_MAX+1);
    TEST_ASSERT_NOT_NULL(payload); memset(payload,'x',YAN_MCP_OUTPUT_MAX+1);
    YanMcpReplyStatus status=yan_mcp_reply_ex(&request,payload,
        YAN_MCP_OUTPUT_MAX+1,NULL,&out,&length,NULL);
    TEST_ASSERT_EQUAL_INT(YAN_MCP_REPLY_LIMIT,status);
    TEST_ASSERT_NULL(out); TEST_ASSERT_EQUAL_UINT32(0,length); free(payload);
    out=NULL; length=0;
    char *encoded=malloc(YAN_MCP_OUTPUT_MAX-64);
    TEST_ASSERT_NOT_NULL(encoded);
    memcpy(encoded,"{\"text\":\"",9);
    memset(encoded+9,'x',YAN_MCP_OUTPUT_MAX-64-11);
    memcpy(encoded+YAN_MCP_OUTPUT_MAX-64-2,"\"}",2);
    status=yan_mcp_reply_ex(&request,encoded,YAN_MCP_OUTPUT_MAX-64,NULL,&out,&length,NULL);
    TEST_ASSERT_EQUAL_INT(YAN_MCP_REPLY_LIMIT,status);
    TEST_ASSERT_NULL(out); TEST_ASSERT_EQUAL_UINT32(0,length); free(encoded);
    out=NULL; length=0;
    status=yan_mcp_reply_ex(&request,"not-an-object",12,NULL,&out,&length,NULL);
    TEST_ASSERT_EQUAL_INT(YAN_MCP_REPLY_IO,status);
    TEST_ASSERT_NULL(out); TEST_ASSERT_EQUAL_UINT32(0,length);
    out=NULL; length=0;
    const char *ok_payload="{\"state\":\"READY\",\"segments\":0,\"skipped\":0}";
    status=yan_mcp_reply_ex(&request,ok_payload,strlen(ok_payload),NULL,&out,&length,NULL);
    TEST_ASSERT_EQUAL_INT(YAN_MCP_REPLY_OK,status);
    TEST_ASSERT_NOT_NULL(out); TEST_ASSERT_GREATER_THAN_UINT32(0,length); free(out);
}
static YanMcpReplyStatus encode_filler(size_t filler,char **out,size_t *length)
{
    size_t payload_length=9+filler+2;
    char *payload=malloc(payload_length);
    if (!payload) return YAN_MCP_REPLY_IO;
    memcpy(payload,"{\"text\":\"",9);
    memset(payload+9,'x',filler);
    memcpy(payload+9+filler,"\"}",2);
    YanMcpReplyStatus status=yan_mcp_reply_ex(&request,payload,payload_length,
                                              NULL,out,length,NULL);
    free(payload);
    return status;
}
static void test_reply_exact_wire_boundary(void)
{
    modern("tools/call",",\"name\":\"yanos_index_status\"");
    /* One-extra-byte numeric id makes the MAX boundary reachable despite the
     * success payload being encoded twice (structuredContent and text). */
    request.id.number=10;
    size_t low=0,high=YAN_MCP_OUTPUT_MAX;
    while (low<high) {
        size_t middle=low+(high-low)/2;
        char *out=NULL; size_t length=0;
        YanMcpReplyStatus status=encode_filler(middle,&out,&length);
        if (status==YAN_MCP_REPLY_LIMIT) {
            TEST_ASSERT_NULL(out); TEST_ASSERT_EQUAL_UINT32(0,length);
            high=middle;
        } else {
            TEST_ASSERT_EQUAL_INT(YAN_MCP_REPLY_OK,status);
            TEST_ASSERT_NOT_NULL(out); TEST_ASSERT_GREATER_THAN_UINT32(0,length);
            free(out);
            low=middle+1;
        }
    }
    TEST_ASSERT_TRUE(low>0);
    char *out=NULL; size_t length=0;
    TEST_ASSERT_EQUAL_INT(YAN_MCP_REPLY_OK,encode_filler(low-1,&out,&length));
    /* The 1 MiB cap applies to the encoded JSON; the framing LF is outside it. */
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(YAN_MCP_OUTPUT_MAX+1,length,
        "block_response_limit");
    free(out); out=NULL; length=0;
    TEST_ASSERT_EQUAL_INT(YAN_MCP_REPLY_LIMIT,encode_filler(low,&out,&length));
    TEST_ASSERT_NULL(out); TEST_ASSERT_EQUAL_UINT32(0,length);
}
static void test_worker_frame_validation_and_tokens(void)
{
    YanMcpWorkerHeader header={YAN_MCP_DOMAIN_SEMANTIC,YAN_SEMANTIC_OK,1};
    TEST_ASSERT_TRUE(yan_mcp_worker_frame_valid(&header,sizeof header+1));
    TEST_ASSERT_NULL(yan_mcp_worker_token(&header));
    header.length=0;
    TEST_ASSERT_FALSE(yan_mcp_worker_frame_valid(&header,sizeof header));
    header.result=YAN_SEMANTIC_STOPPED;
    TEST_ASSERT_TRUE(yan_mcp_worker_frame_valid(&header,sizeof header));
    TEST_ASSERT_EQUAL_STRING("OUTPUT",yan_mcp_worker_token(&header));
    header.domain=YAN_MCP_DOMAIN_BLOCK; header.result=YAN_BLOCK_QUERY_STOPPED;
    TEST_ASSERT_TRUE(yan_mcp_worker_frame_valid(&header,sizeof header));
    TEST_ASSERT_EQUAL_STRING("STOPPED",yan_mcp_worker_token(&header));
    header.result=YAN_BLOCK_QUERY_LIMIT;
    TEST_ASSERT_EQUAL_STRING("LIMIT",yan_mcp_worker_token(&header));
    header.result=YAN_BLOCK_QUERY_OK; header.length=1;
    TEST_ASSERT_TRUE(yan_mcp_worker_frame_valid(&header,sizeof header+1));
    header.domain=2;
    TEST_ASSERT_FALSE(yan_mcp_worker_frame_valid(&header,sizeof header+1));
    header.domain=YAN_MCP_DOMAIN_SEMANTIC; header.result=YAN_SEMANTIC_NOT_FOUND+1;
    TEST_ASSERT_FALSE(yan_mcp_worker_frame_valid(&header,sizeof header));
    header.result=YAN_SEMANTIC_OK; header.length=1;
    TEST_ASSERT_FALSE(yan_mcp_worker_frame_valid(&header,sizeof header));
    TEST_ASSERT_FALSE(yan_mcp_worker_frame_valid(&header,sizeof header+2));
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
    RUN_TEST(test_six_tools_schema_order_and_version);
    RUN_TEST(test_six_tools_deep_schemas);
    RUN_TEST(test_block_argument_parsing);
    RUN_TEST(test_reply_status_limit_and_io);
    RUN_TEST(test_reply_exact_wire_boundary);
    RUN_TEST(test_worker_frame_validation_and_tokens);
    return UNITY_END();
}
