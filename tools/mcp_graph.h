#ifndef YAN_TOOLS_MCP_GRAPH_H
#define YAN_TOOLS_MCP_GRAPH_H
#include "knowledge_graph_query.h"

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 0045 R2: the two read-only knowledge-graph tools over the existing MCP.
 *
 * Contract: docs/specs/0045-graph-mcp-tools.md R2 and
 * docs/decisions/0009-graph-agent-access.md.
 *
 * These tools publish what the 0044 graph contains and nothing more: the
 * explicit concepts, aliases and provenance-carrying propositions the notes
 * declare. A concept that is absent is absent, never a claim about what the
 * user does or does not know, and nothing here infers, ranks or generates.
 *
 * The module owns only the graph-specific parts: the two gated tool schemas and
 * the execution adapter that calls the shared 0045 R1 service. The protocol,
 * worker and server layers keep their existing shape and only route to it, so
 * there is one JSON implementation (in host/knowledge_graph_query.c) and one
 * error vocabulary (the 0044 YanKgResult) rather than a second copy of either.
 *
 * Both tools are gated by --allow-graph for the same reason --allow-meta
 * exists: 0035-R1-AC1 and 0035-R12-AC1 pin tools/list to the 0033 six-tool
 * array byte for byte when no extension flag is given, so an always-on seventh
 * tool would break a committed contract. The flag takes no value and adds no
 * bit to the required-argument bitmap, so --disk-image still names the one
 * image this process serves and the graph branch opens only that image. */

/* Schema for yanos_graph_list, appended after every previously enabled tool. */
const char *yan_mcp_graph_list_tool_json(void);
/* Schema for yanos_graph_query, appended directly after yanos_graph_list. */
const char *yan_mcp_graph_query_tool_json(void);

/* The 0045 R2 payload bound for one encoded graph object. It is the existing
 * 1 MiB MCP payload ceiling, not a new graph-specific limit: the response that
 * carries the object (and its second copy inside text content) is measured
 * against the same bound by yan_mcp_reply_ex, which reports LIMIT rather than a
 * truncated success. */
#define YAN_MCP_GRAPH_PAYLOAD_MAX 1048576u

/* Executes one graph request against one image and encodes the complete 0044
 * JSON object. `expected` is optional (NULL for none) and is copied into the
 * service call before any I/O; a mismatch after the initial observation is
 * STALE. `cancel` is the parent-owned flag, observed between source callbacks.
 *
 * The call is the shared 0045 R1 service directly -- no subprocess, no model, no
 * runner, no index -- so it works on an image whose model/index/runner are
 * absent and starts no extra process. *payload is malloc-owned on success,
 * including its NUL terminator, and is NULL with *length 0 on every failure, so
 * no caller can publish half an object. */
YanKgResult yan_mcp_graph_execute(const char *image,
                                  YanKgQueryOperation operation,
                                  const char *query, uint32_t query_length,
                                  const uint8_t *expected,
                                  const volatile sig_atomic_t *cancel,
                                  char **payload, size_t *length);

/* Stable ASCII token for a graph execution result, using the 0044 vocabulary
 * (NOT_FOUND, AMBIGUOUS, GRAPH_FORMAT, UNRESOLVED, STALE, LIMIT, NOSPACE,
 * IO, ...). NULL means success. This is an explicit mapping of the shared
 * enumerator; it never casts a number from another domain. */
const char *yan_mcp_graph_result_name(YanKgResult result);

#endif
