#ifndef YAN_TOOLS_MCP_NOTE_H
#define YAN_TOOLS_MCP_NOTE_H
#include <stddef.h>
/* 0035 write-tool adapter. This stage only owns the default-off schemas and
 * name dispatch; commit/preview I/O arrives with the write implementation. */
typedef enum {
    YAN_NOTE_TOOL_NONE = 0,
    YAN_NOTE_TOOL_CREATE,
    YAN_NOTE_TOOL_REPLACE
} YanMcpNoteTool;
YanMcpNoteTool yan_mcp_note_tool(const char *name);
unsigned yan_mcp_note_tool_count(void);
const char *yan_mcp_note_tool_json(unsigned index);
#endif
