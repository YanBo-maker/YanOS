#include "mcp_worker.h"
#include "semantic_json.h"
#include "semantic_posix.h"
#include "semantic_llama.h"
#include <stdlib.h>
#include <string.h>

typedef struct { YanSemanticJsonRows rows; const volatile sig_atomic_t *cancel; } Rows;
static bool collect(void *opaque,const YanSemanticRow *row)
{
    Rows *saved=opaque;
    if (*saved->cancel || saved->rows.count>=YAN_SEMANTIC_TOP || row->raw_length>2048u) return false;
    YanSemanticJsonRow *item=&saved->rows.rows[saved->rows.count++];
    item->row=*row; memcpy(item->raw,row->raw,row->raw_length); item->row.raw=item->raw; return true;
}
static YanSemanticResult execute_semantic(const YanMcpOptions *o,
    const YanMcpRequest *request,const volatile sig_atomic_t *cancel,
    char **payload,size_t *length)
{
    *payload=NULL; *length=0;
    YanSemanticPosix *adapter=calloc(1,sizeof *adapter);
    Rows *saved=request->kind==YAN_MCP_SEARCH ? calloc(1,sizeof *saved) : NULL;
    if (!adapter || (request->kind==YAN_MCP_SEARCH && !saved)) { free(adapter); free(saved); return YAN_SEMANTIC_IO; }
    if (saved) saved->cancel=cancel;
    adapter->image_fd=adapter->index_fd=-1;
    YanSemanticIdentity identity={{0},{0},{0}};
    YanSemanticLlama llama={0}; llama.owned_pid=-1;
    YanSemanticResult result=yan_semantic_posix_open(adapter,o->image,o->index,o->model,o->runner,&identity);
    if (result==YAN_SEMANTIC_OK) result=yan_semantic_llama_init(&llama,o->model,o->runner,o->startup,o->request,cancel);
    if (result==YAN_SEMANTIC_OK) result=yan_semantic_llama_config(&llama,identity.config);
    YanSemanticSource source; YanSemanticStore store; YanSemanticProvider provider;
    YanSemantic context={0}; YanSemanticSummary summary={0}; YanSemanticEvidence evidence={0};
    yan_semantic_posix_bind(adapter,&source,&store); yan_semantic_llama_bind(&llama,&provider);
    if (result==YAN_SEMANTIC_OK) result=yan_semantic_init(&context,&source,&provider,&store,&identity,yan_semantic_posix_close_source);
    if (result==YAN_SEMANTIC_OK && *cancel) result=YAN_SEMANTIC_CANCELLED;
    if (result==YAN_SEMANTIC_OK) {
        if (request->kind==YAN_MCP_STATUS) result=yan_semantic_status(&context,&summary);
        else result=yan_semantic_search_evidence(&context,(const uint8_t *)request->query,strlen(request->query),collect,saved,&evidence);
    }
    YanSemanticResult closed=yan_semantic_posix_cleanup(adapter);
    if (result==YAN_SEMANTIC_OK) result=closed;
    YanSemanticResult stopped=yan_semantic_llama_stop(&llama);
    if (result==YAN_SEMANTIC_OK) result=stopped;
    free(adapter);
    if (*cancel) result=YAN_SEMANTIC_CANCELLED;
    if (result==YAN_SEMANTIC_OK && request->kind==YAN_MCP_SEARCH)
        *payload=yan_semantic_json_encode(request->query,&saved->rows,&evidence,length,NULL);
    else if (result==YAN_SEMANTIC_OK) {
        yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL);
        if (doc) {
            yyjson_mut_val *root=yyjson_mut_obj(doc); yyjson_mut_doc_set_root(doc,root);
            const char *state=summary.state==YAN_SEMANTIC_INDEX_READY ? "READY" :
                summary.state==YAN_SEMANTIC_INDEX_MISSING ? "MISSING" : summary.state==YAN_SEMANTIC_INDEX_STALE ? "STALE" : "CORRUPT";
            bool ok=root && yyjson_mut_obj_add_str(doc,root,"state",state);
            if (summary.state==YAN_SEMANTIC_INDEX_READY) ok=ok && yyjson_mut_obj_add_uint(doc,root,"segments",summary.segments) && yyjson_mut_obj_add_uint(doc,root,"skipped",summary.skipped);
            if (ok) *payload=yyjson_mut_write_opts(doc,0,NULL,length,NULL);
            yyjson_mut_doc_free(doc);
        }
    }
    free(saved);
    if (result==YAN_SEMANTIC_OK && !*payload) result=YAN_SEMANTIC_IO;
    if (result!=YAN_SEMANTIC_OK) { free(*payload); *payload=NULL; *length=0; }
    return result;
}

static YanBlockQueryResult map_image_open(YanImageOpenResult result)
{
    switch (result) {
    case YAN_IMAGE_OPEN_OK: return YAN_BLOCK_QUERY_OK;
    case YAN_IMAGE_OPEN_INVALID: return YAN_BLOCK_QUERY_INVALID;
    case YAN_IMAGE_OPEN_IO: return YAN_BLOCK_QUERY_IO;
    case YAN_IMAGE_OPEN_CORRUPT: return YAN_BLOCK_QUERY_CORRUPT;
    case YAN_IMAGE_OPEN_UNSUPPORTED: return YAN_BLOCK_QUERY_UNSUPPORTED;
    case YAN_IMAGE_OPEN_NOT_MOUNTED: return YAN_BLOCK_QUERY_NOT_MOUNTED;
    case YAN_IMAGE_OPEN_FAULTED: return YAN_BLOCK_QUERY_FAULTED;
    case YAN_IMAGE_OPEN_BUSY: return YAN_BLOCK_QUERY_BUSY;
    case YAN_IMAGE_OPEN_PROTOCOL: return YAN_BLOCK_QUERY_PROTOCOL;
    }
    return YAN_BLOCK_QUERY_PROTOCOL;
}
static bool add_sha256(yyjson_mut_doc *doc,yyjson_mut_val *object,
                       const char *key,const uint8_t digest[32])
{
    static const char hex[]="0123456789abcdef";
    char text[65];
    for (unsigned i=0;i<32;++i) {
        text[2*i]=hex[digest[i]>>4]; text[2*i+1]=hex[digest[i]&15u];
    }
    text[64]=0;
    return yyjson_mut_obj_add_strcpy(doc,object,key,text);
}
static yyjson_mut_val *range_object(yyjson_mut_doc *doc,uint32_t start,
    uint32_t end,uint32_t first_line,uint32_t last_line)
{
    yyjson_mut_val *object=yyjson_mut_obj(doc);
    bool ok=object && yyjson_mut_obj_add_uint(doc,object,"start",start) &&
        yyjson_mut_obj_add_uint(doc,object,"end",end) &&
        yyjson_mut_obj_add_uint(doc,object,"first_line",first_line) &&
        yyjson_mut_obj_add_uint(doc,object,"last_line",last_line);
    return ok ? object : NULL;
}
static bool add_block_id(yyjson_mut_doc *doc,yyjson_mut_val *object,
                         const YanBlockReference *reference)
{
    return reference->has_source_id ?
        yyjson_mut_obj_add_str(doc,object,"block_id",reference->source_id) :
        yyjson_mut_obj_add_null(doc,object,"block_id");
}
typedef struct {
    YanBlockQueryList list;
    YanBlockQueryRead read;
    YanBlockQueryRefs refs;
    YanBlockQueryBacklinks backlinks;
} BlockResults;
static bool encode_block(const YanMcpRequest *request,const YanBlockQuery *query,
                         const BlockResults *results,char **payload,size_t *length)
{
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL);
    if (!doc) return false;
    yyjson_mut_val *root=yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc,root);
    const char *operation=request->kind==YAN_MCP_BLOCK_LIST ? "block_list" :
        request->kind==YAN_MCP_BLOCK_READ ? "block_read" :
        request->kind==YAN_MCP_BLOCK_REFS ? "block_refs" : "block_backlinks";
    bool ok=root && yyjson_mut_obj_add_uint(doc,root,"schema_version",1) &&
        yyjson_mut_obj_add_str(doc,root,"operation",operation) &&
        add_sha256(doc,root,"source_sha256",query->source_sha256) &&
        yyjson_mut_obj_add_str(doc,root,"name",request->name);
    if (ok && (request->kind==YAN_MCP_BLOCK_READ ||
               request->kind==YAN_MCP_BLOCK_BACKLINKS))
        ok=yyjson_mut_obj_add_str(doc,root,"block_id",request->block_id);
    if (ok && request->kind==YAN_MCP_BLOCK_LIST) {
        const YanBlockQueryList *list=&results->list;
        ok=yyjson_mut_obj_add_uint(doc,root,"total",list->total) &&
            yyjson_mut_obj_add_uint(doc,root,"shown",list->shown);
        yyjson_mut_val *array=ok ? yyjson_mut_arr(doc) : NULL;
        ok=ok && array;
        for (uint32_t i=0;ok && i<list->shown;++i) {
            const YanBlockDecl *block=&list->blocks[i];
            yyjson_mut_val *item=yyjson_mut_obj(doc);
            ok=item && yyjson_mut_obj_add_str(doc,item,"id",block->id) &&
                yyjson_mut_obj_add_uint(doc,item,"marker_line",block->marker_line) &&
                yyjson_mut_obj_add_uint(doc,item,"first_line",block->first_line) &&
                yyjson_mut_obj_add_uint(doc,item,"last_line",block->last_line) &&
                yyjson_mut_obj_add_uint(doc,item,"start",block->start) &&
                yyjson_mut_obj_add_uint(doc,item,"end",block->end) &&
                yyjson_mut_arr_add_val(array,item);
        }
        ok=ok && yyjson_mut_obj_add_val(doc,root,"blocks",array);
    } else if (ok && request->kind==YAN_MCP_BLOCK_READ) {
        const YanBlockQueryRead *read=&results->read;
        yyjson_mut_val *block=range_object(doc,read->block_start,
            read->block_end,read->block_first_line,read->block_last_line);
        yyjson_mut_val *page=range_object(doc,read->page_start,read->page_end,
            read->page_first_line,read->page_last_line);
        ok=block && page && yyjson_mut_obj_add_val(doc,root,"block",block) &&
            yyjson_mut_obj_add_val(doc,root,"page",page) &&
            yyjson_mut_obj_add_strncpy(doc,root,"text",
                (const char *)read->text,read->text_length) &&
            yyjson_mut_obj_add_bool(doc,root,"complete",read->complete);
        if (ok) {
            ok=read->complete ? yyjson_mut_obj_add_null(doc,root,"next_offset") :
                yyjson_mut_obj_add_uint(doc,root,"next_offset",read->next_offset);
        }
    } else if (ok && request->kind==YAN_MCP_BLOCK_REFS) {
        const YanBlockQueryRefs *refs=&results->refs;
        ok=yyjson_mut_obj_add_uint(doc,root,"total",refs->total) &&
            yyjson_mut_obj_add_uint(doc,root,"shown",refs->shown);
        yyjson_mut_val *array=ok ? yyjson_mut_arr(doc) : NULL;
        ok=ok && array;
        for (uint32_t i=0;ok && i<refs->shown;++i) {
            const YanBlockRow *row=&refs->rows[i];
            const YanBlockReference *reference=&row->reference;
            yyjson_mut_val *item=yyjson_mut_obj(doc);
            yyjson_mut_val *target=yyjson_mut_obj(doc);
            ok=item && target && yyjson_mut_obj_add_str(doc,item,"source",row->source) &&
                add_block_id(doc,item,reference) &&
                yyjson_mut_obj_add_uint(doc,item,"line",reference->line) &&
                yyjson_mut_obj_add_uint(doc,item,"start",reference->start) &&
                yyjson_mut_obj_add_uint(doc,item,"end",reference->end) &&
                yyjson_mut_obj_add_str(doc,target,"name",reference->target.name) &&
                yyjson_mut_obj_add_str(doc,target,"id",reference->target.id) &&
                yyjson_mut_obj_add_val(doc,item,"target",target) &&
                yyjson_mut_obj_add_str(doc,item,"state",yan_block_target_name(row->state)) &&
                yyjson_mut_arr_add_val(array,item);
        }
        ok=ok && yyjson_mut_obj_add_val(doc,root,"refs",array);
    } else if (ok && request->kind==YAN_MCP_BLOCK_BACKLINKS) {
        const YanBlockQueryBacklinks *backlinks=&results->backlinks;
        ok=yyjson_mut_obj_add_uint(doc,root,"total",backlinks->total) &&
            yyjson_mut_obj_add_uint(doc,root,"shown",backlinks->shown) &&
            yyjson_mut_obj_add_uint(doc,root,"skipped",backlinks->skipped) &&
            yyjson_mut_obj_add_str(doc,root,"target_state",
                yan_block_target_name(backlinks->target_state));
        yyjson_mut_val *array=ok ? yyjson_mut_arr(doc) : NULL;
        ok=ok && array;
        for (uint32_t i=0;ok && i<backlinks->shown;++i) {
            const YanBlockRow *row=&backlinks->rows[i];
            const YanBlockReference *reference=&row->reference;
            yyjson_mut_val *item=yyjson_mut_obj(doc);
            ok=item && yyjson_mut_obj_add_str(doc,item,"source",row->source) &&
                add_block_id(doc,item,reference) &&
                yyjson_mut_obj_add_uint(doc,item,"line",reference->line) &&
                yyjson_mut_obj_add_uint(doc,item,"start",reference->start) &&
                yyjson_mut_obj_add_uint(doc,item,"end",reference->end) &&
                yyjson_mut_arr_add_val(array,item);
        }
        ok=ok && yyjson_mut_obj_add_val(doc,root,"backlinks",array);
    }
    if (ok) *payload=yyjson_mut_write_opts(doc,0,NULL,length,NULL);
    yyjson_mut_doc_free(doc);
    return ok && *payload;
}
static YanBlockQueryResult execute_block(const YanMcpOptions *options,
    const YanMcpRequest *request,const volatile sig_atomic_t *cancel,
    char **payload,size_t *length)
{
    *payload=NULL; *length=0;
    YanSemanticPosix *adapter=calloc(1,sizeof *adapter);
    if (!adapter) return YAN_BLOCK_QUERY_IO;
    adapter->image_fd=adapter->index_fd=-1;
    YanBlockQueryResult result=map_image_open(
        yan_semantic_posix_open_image_ex(adapter,options->image));
    YanBlockQuery *query=NULL;
    BlockResults *results=NULL;
    if (result==YAN_BLOCK_QUERY_OK) {
        query=calloc(1,sizeof *query);
        results=calloc(1,sizeof *results);
        if (!query || !results) result=YAN_BLOCK_QUERY_IO;
    }
    if (result==YAN_BLOCK_QUERY_OK)
        result=yan_block_query_init(query,adapter,cancel);
    const uint8_t *expected=request->has_expected_source ?
        request->expected_source : NULL;
    if (result==YAN_BLOCK_QUERY_OK) {
        switch (request->kind) {
        case YAN_MCP_BLOCK_LIST:
            result=yan_block_query_list(query,request->name,expected,&results->list);
            break;
        case YAN_MCP_BLOCK_READ:
            result=yan_block_query_read(query,request->name,request->block_id,
                request->offset,expected,&results->read);
            break;
        case YAN_MCP_BLOCK_REFS:
            result=yan_block_query_refs(query,request->name,expected,&results->refs);
            break;
        case YAN_MCP_BLOCK_BACKLINKS:
            result=yan_block_query_backlinks(query,request->name,request->block_id,
                expected,&results->backlinks);
            break;
        default:
            result=YAN_BLOCK_QUERY_INVALID;
            break;
        }
    }
    if (result==YAN_BLOCK_QUERY_OK && *cancel) result=YAN_BLOCK_QUERY_CANCELLED;
    if (result==YAN_BLOCK_QUERY_OK && !encode_block(request,query,results,payload,length))
        result=YAN_BLOCK_QUERY_IO;
    YanSemanticResult closed=yan_semantic_posix_cleanup(adapter);
    if (result==YAN_BLOCK_QUERY_OK && closed!=YAN_SEMANTIC_OK)
        result=YAN_BLOCK_QUERY_IO;
    free(results); free(query); free(adapter);
    if (result!=YAN_BLOCK_QUERY_OK) { free(*payload); *payload=NULL; *length=0; }
    return result;
}
bool yan_mcp_worker_frame_valid(const YanMcpWorkerHeader *header,size_t received_bytes)
{
    if (!header || header->length>YAN_MCP_OUTPUT_MAX) return false;
    if (received_bytes!=sizeof *header+header->length) return false;
    if (header->domain!=YAN_MCP_DOMAIN_SEMANTIC && header->domain!=YAN_MCP_DOMAIN_BLOCK)
        return false;
    if (header->result==0) return header->length>0;
    if (header->length!=0) return false;
    if (header->domain==YAN_MCP_DOMAIN_SEMANTIC)
        return header->result<=YAN_SEMANTIC_NOT_FOUND;
    return header->result<=YAN_BLOCK_QUERY_UNSUPPORTED;
}
const char *yan_mcp_worker_token(const YanMcpWorkerHeader *header)
{
    if (!header) return "IO";
    if (header->domain==YAN_MCP_DOMAIN_SEMANTIC) {
        if (header->result==0) return NULL;
        if (header->result==YAN_SEMANTIC_STOPPED) return "OUTPUT";
        if (header->result<=YAN_SEMANTIC_NOT_FOUND)
            return yan_semantic_result_name((YanSemanticResult)header->result);
        return "IO";
    }
    if (header->result==0) return NULL;
    if (header->result<=YAN_BLOCK_QUERY_UNSUPPORTED)
        return yan_block_query_result_name((YanBlockQueryResult)header->result);
    return "IO";
}
YanMcpWorkerResult yan_mcp_execute(const YanMcpOptions *options,
    const YanMcpRequest *request,const volatile sig_atomic_t *cancel,
    char **payload,size_t *length)
{
    YanMcpWorkerResult result;
    if (request->kind==YAN_MCP_BLOCK_LIST || request->kind==YAN_MCP_BLOCK_READ ||
        request->kind==YAN_MCP_BLOCK_REFS || request->kind==YAN_MCP_BLOCK_BACKLINKS) {
        result.domain=YAN_MCP_DOMAIN_BLOCK;
        result.result=(uint32_t)execute_block(options,request,cancel,payload,length);
    } else {
        result.domain=YAN_MCP_DOMAIN_SEMANTIC;
        result.result=(uint32_t)execute_semantic(options,request,cancel,payload,length);
    }
    return result;
}
