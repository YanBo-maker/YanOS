#ifndef YAN_TOOLS_MCP_WORKER_H
#define YAN_TOOLS_MCP_WORKER_H
#include "mcp_protocol.h"
#include "semantic_result.h"
#include <signal.h>
typedef struct {
    const char *image,*index,*model,*runner;
    unsigned startup,request;
} YanMcpOptions;
/* Private fork pipe, never an external wire format; same host and executable.
 * Result + bounded complete JSON, followed by EOF and a normal worker exit. */
typedef struct { uint32_t result,length; } YanMcpWorkerHeader;
YanSemanticResult yan_mcp_execute(const YanMcpOptions *,const YanMcpRequest *,
    const volatile sig_atomic_t *,char **payload,size_t *length);
#endif
