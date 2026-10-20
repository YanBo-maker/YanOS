#ifndef YAN_TOOLS_MCP_NOTE_H
#define YAN_TOOLS_MCP_NOTE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* 0035 write-tool adapter: default-off schemas, name dispatch, and the
 * lock/read/compare/write/preview path shared by both tools. */
typedef enum {
    YAN_NOTE_TOOL_NONE = 0,
    YAN_NOTE_TOOL_CREATE,
    YAN_NOTE_TOOL_REPLACE
} YanMcpNoteTool;
typedef enum {
    YAN_MCP_NOTE_OK = 0, YAN_MCP_NOTE_INVALID, YAN_MCP_NOTE_BUSY, YAN_MCP_NOTE_STALE,
    YAN_MCP_NOTE_EXISTS, YAN_MCP_NOTE_NOT_FOUND, YAN_MCP_NOTE_INVALID_TEXT,
    YAN_MCP_NOTE_CORRUPT, YAN_MCP_NOTE_UNSUPPORTED, YAN_MCP_NOTE_DIRECTORY_FULL,
    YAN_MCP_NOTE_NOSPACE, YAN_MCP_NOTE_IO, YAN_MCP_NOTE_IO_COMMITTED
} YanMcpNoteResult;
typedef struct {
    bool replace, dry_run, has_expected;
    uint8_t expected[32];
    const char *name;
    const uint8_t *text;
    uint32_t length;
} YanMcpNoteRequest;
YanMcpNoteTool yan_mcp_note_tool(const char *name);
unsigned yan_mcp_note_tool_count(void);
const char *yan_mcp_note_tool_json(unsigned index);
/* Validates in memory first (zero source I/O), then takes LOCK_EX (commit) or
 * LOCK_SH (preview), reads the current identity and old bytes, and writes only
 * through yan_note_write_locked. *payload is owned by malloc. */
YanMcpNoteResult yan_mcp_note_execute(const char *image, const YanMcpNoteRequest *,
                                   char **payload, size_t *length, bool *committed);
const char *yan_mcp_note_result_name(YanMcpNoteResult);
#endif
