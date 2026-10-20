#ifndef YAN_TOOLS_MCP_PROTOCOL_H
#define YAN_TOOLS_MCP_PROTOCOL_H
#include "semantic_text.h"
#include <yyjson.h>
#include <stdint.h>

#define YAN_MCP_INPUT_MAX 16384u
#define YAN_MCP_OUTPUT_MAX 1048576u
#define YAN_MCP_QUEUE_MAX 2097152u
typedef struct { bool valid, string; int64_t number; size_t length; char text[129]; } YanMcpId;
typedef struct { bool legacy_initialized, legacy_ready, allow_write; } YanMcpSession;
typedef enum {
    YAN_MCP_IGNORE, YAN_MCP_PROTOCOL_ERROR, YAN_MCP_INITIALIZE,
    YAN_MCP_DISCOVER, YAN_MCP_LIST, YAN_MCP_PING, YAN_MCP_SEARCH,
    YAN_MCP_STATUS, YAN_MCP_BLOCK_LIST, YAN_MCP_BLOCK_READ,
    YAN_MCP_BLOCK_REFS, YAN_MCP_BLOCK_BACKLINKS,
    YAN_MCP_NOTE_CREATE, YAN_MCP_NOTE_REPLACE,
    YAN_MCP_TOOL_ERROR, YAN_MCP_CANCEL
} YanMcpKind;
/* No pointers into the decoded document survive parse. Session and active ID
 * are parent-owned; the protocol layer performs no file/process operations. */
typedef struct {
    YanMcpKind kind; YanMcpId id, target; bool modern;
    bool allow_write;
    int error; size_t requested_version_length;
    char requested_version[YAN_MCP_INPUT_MAX+1]; char query[1024];
    char name[32], block_id[32];
    uint8_t expected_source[32];
    bool has_expected_source, has_offset;
    uint32_t offset;
} YanMcpRequest;
bool yan_mcp_id_equal(const YanMcpId *, const YanMcpId *);
void yan_mcp_parse(const char *, size_t, YanMcpSession *, const YanMcpId *,
                   YanMcpRequest *, const yyjson_alc *);
/* Owned allocator buffer, including final LF. The 1 MiB cap is the encoded
 * JSON payload before that framing LF: exactly YAN_MCP_OUTPUT_MAX JSON bytes
 * plus LF is legal (length YAN_MCP_OUTPUT_MAX+1); one more JSON byte is LIMIT.
 * NULL on encoding/allocation failure or LIMIT. Caller releases with the same
 * allocator. */
typedef enum {
    YAN_MCP_REPLY_OK = 0,
    YAN_MCP_REPLY_LIMIT,
    YAN_MCP_REPLY_IO
} YanMcpReplyStatus;
/* Same encoding contract as yan_mcp_reply, but distinguishes the complete
 * response exceeding 1MiB (LIMIT) from allocation/encoding failure (IO).
 * *out is NULL and *length is 0 unless OK. */
YanMcpReplyStatus yan_mcp_reply_ex(const YanMcpRequest *, const char *payload,
    size_t, const char *error_token, char **out, size_t *,
    const yyjson_alc *);
char *yan_mcp_reply(const YanMcpRequest *, const char *payload, size_t,
                    const char *error_token, size_t *, const yyjson_alc *);
#endif
