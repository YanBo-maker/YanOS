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
YanSemanticResult yan_mcp_execute(const YanMcpOptions *o,const YanMcpRequest *request,
    const volatile sig_atomic_t *cancel,char **payload,size_t *length)
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
