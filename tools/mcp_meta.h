#ifndef YAN_TOOLS_MCP_META_H
#define YAN_TOOLS_MCP_META_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 0027 note-metadata over MCP: an optional read-only tool that publishes the
 * parsed header (title/created/modified/tags/links) that the six 0033 block
 * tools never expose.
 *
 * The tool is gated by --allow-meta for ONE reason, and it is not that a new
 * read capability is wanted: 0035-R1-AC1 and 0035-R12-AC1 require tools/list
 * to stay byte-identical to the 0033 six-tool list when no extension flag is
 * given. An always-on seventh tool would break that pinned contract, so the
 * tool only appears when the operator opts in. The flag changes no image
 * binding: --disk-image still names the single image this process serves, and
 * --allow-meta takes no value and no bit in the required-argument bitmap
 * (0035-R11). */
typedef enum {
    YAN_MCP_META_OK = 0,
    YAN_MCP_META_INVALID,
    YAN_MCP_META_BUSY,
    YAN_MCP_META_STALE,
    YAN_MCP_META_METADATA,
    YAN_MCP_META_NOT_FOUND,
    YAN_MCP_META_CORRUPT,
    YAN_MCP_META_UNSUPPORTED,
    YAN_MCP_META_IO
} YanMcpMetaResult;

/* Gated tool JSON, appended to tools/list only when --allow-meta is set. */
const char *yan_mcp_meta_tool_json(void);

/* Reads the bounded 0027 header of one note and encodes one note_meta JSON
 * object. Reuses the 0027 service, so the classifications are the spec's own:
 * a header-less file is a valid old note (has_header false, empty metadata,
 * 0027:87), a matched prefix with a broken header is METADATA (0027:151), and
 * a missing file is NOT_FOUND. Links are resolved by a directory stat only
 * (0027:81 "LINK state only looks at whether the target file exists and does
 * not read its body"), so a missing target is an ordinary dangling link and
 * not an error; every other stat error fails the whole call instead of
 * silently becoming MISSING. expected is optional (NULL for none) and its 32
 * bytes are copied before any I/O; a mismatch after the read is STALE with no
 * payload. *payload is malloc-owned. */
YanMcpMetaResult yan_mcp_meta_execute(const char *image, const char *name,
                                      const uint8_t *expected,
                                      char **payload, size_t *length);

/* Stable tool-error token; NULL for OK. */
const char *yan_mcp_meta_result_name(YanMcpMetaResult);
#endif
