#ifndef YAN_TOOLS_MCP_WORKER_H
#define YAN_TOOLS_MCP_WORKER_H
#include "block_query.h"
#include "mcp_protocol.h"
#include "semantic_result.h"
#include <signal.h>
typedef struct {
    const char *image,*index,*model,*runner;
    unsigned startup,request;
} YanMcpOptions;
/* Private fork pipe, never an external wire format; same host and executable.
 * Domain + result code + bounded complete JSON, followed by EOF and a normal
 * worker exit. */
/* Domain 3, not 2: the committed worker-frame tests pin domain 2 as the
 * invalid value (tests/test_mcp_protocol.c "header.domain=2" must be refused
 * and tests/mcp_fault_hooks.c injects domain=2 as FRAME_BAD_DOMAIN), so reusing
 * 2 would silently disable both guards. */
typedef enum { YAN_MCP_DOMAIN_SEMANTIC = 0, YAN_MCP_DOMAIN_BLOCK = 1,
               YAN_MCP_DOMAIN_META = 3 } YanMcpDomain;
typedef struct { uint32_t domain,result,length; } YanMcpWorkerHeader;
typedef struct { YanMcpDomain domain; uint32_t result; } YanMcpWorkerResult;
YanMcpWorkerResult yan_mcp_execute(const YanMcpOptions *,const YanMcpRequest *,
    const volatile sig_atomic_t *,char **payload,size_t *length);
/* Parent-side validation of one complete worker pipe body. OK requires a
 * non-empty payload; any error requires an empty one. */
bool yan_mcp_worker_frame_valid(const YanMcpWorkerHeader *, size_t received_bytes);
/* NULL for OK, otherwise the stable tool-error token for this domain. */
const char *yan_mcp_worker_token(const YanMcpWorkerHeader *);
#endif
