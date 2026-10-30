#include "mcp_meta.h"

#include "note_metadata.h"
#include "semantic_posix.h"

#include <stdlib.h>
#include <string.h>

#include <yyjson.h>

/* The gated 0027 metadata tool. It reuses the 0027 service for every
 * classification and adds only JSON encoding, so the header rules stay in one
 * place: "show reads only the bounded prefix needed to recognise and validate
 * the header and does not preflight the whole body; an old header-less file may
 * have any binary body and observation stays legal" (0027:87), and "a matched
 * prefix with a broken header is exactly METADATA" (0027:151).
 *
 * Gating rationale (stated here, in --allow-meta's own comment and in the tool
 * description because a read tool behind a flag otherwise looks arbitrary):
 * tools/list must remain byte-identical to the 0033 six-tool array whenever no
 * extension flag is given (0035-R1-AC1, 0035-R12-AC1). A seventh always-on tool
 * would break that pinned contract. The flag is not a new product toggle and
 * changes no image binding: it takes no value and no bit in the required
 * argument bitmap, so --disk-image alone still selects the one image served
 * (0035-R11). */
static const char tool_json[] =
"{\"name\":\"yanos_note_meta\",\"description\":\"只读返回一篇笔记的 0027 元数据头字段（title/created/modified/tags/links），不返回正文。头缺失的旧文件是合法的，此时 has_header 为 false 且各字段为空，不伪造。links 的 state 为 OK 或 MISSING：MISSING 对应 Guest 终端 meta show 的 DANGLING，两者语义相同，当且仅当目标文件不存在（LINK 状态只看目标文件是否存在，不读目标正文，0027:81）；目标存在的其它读取错误会让整个调用报错，不会变成 MISSING。本工具仅在带 --allow-meta 启动时出现，理由是保护默认启动态 tools/list 与 0033 六工具逐字节相同（0035-R1-AC1、0035-R12-AC1），不是新增产品读开关；该开关不改变镜像绑定方式。\",\"inputSchema\":{\"type\":\"object\",\"additionalProperties\":false,\"required\":[\"name\"],\"properties\":{\"name\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":31},\"expected_source\":{\"type\":\"string\",\"pattern\":\"^[0-9a-f]{64}$\"}}},\"outputSchema\":{\"type\":\"object\",\"additionalProperties\":false,\"required\":[\"schema_version\",\"operation\",\"source_sha256\",\"name\",\"has_header\",\"title\",\"created\",\"modified\",\"tags\",\"links\",\"complete\"],\"properties\":{\"schema_version\":{\"const\":1},\"operation\":{\"const\":\"note_meta\"},\"source_sha256\":{\"type\":\"string\",\"pattern\":\"^[0-9a-f]{64}$\"},\"name\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":31},\"has_header\":{\"type\":\"boolean\"},\"title\":{\"type\":[\"string\",\"null\"]},\"created\":{\"type\":[\"string\",\"null\"]},\"modified\":{\"type\":[\"string\",\"null\"]},\"tags\":{\"type\":\"array\",\"maxItems\":16,\"items\":{\"type\":\"string\"}},\"links\":{\"type\":\"array\",\"maxItems\":16,\"items\":{\"type\":\"object\",\"additionalProperties\":false,\"required\":[\"name\",\"state\"],\"properties\":{\"name\":{\"type\":\"string\"},\"state\":{\"enum\":[\"OK\",\"MISSING\"]}}}},\"complete\":{\"const\":true}}},\"annotations\":{\"readOnlyHint\":true,\"destructiveHint\":false,\"idempotentHint\":true,\"openWorldHint\":false}}";

const char *yan_mcp_meta_tool_json(void) { return tool_json; }

static YanMcpMetaResult map_note(YanNoteResult result)
{
    switch (result) {
    case YAN_NOTE_OK: return YAN_MCP_META_OK;
    case YAN_NOTE_INVALID: return YAN_MCP_META_INVALID;
    case YAN_NOTE_BUSY: return YAN_MCP_META_BUSY;
    case YAN_NOTE_METADATA: return YAN_MCP_META_METADATA;
    case YAN_NOTE_NOT_FOUND: return YAN_MCP_META_NOT_FOUND;
    case YAN_NOTE_CORRUPT: return YAN_MCP_META_CORRUPT;
    case YAN_NOTE_UNSUPPORTED: return YAN_MCP_META_UNSUPPORTED;
    case YAN_NOTE_TOO_LARGE:
    case YAN_NOTE_INVALID_TEXT:
    /* The observation path never preflights the body (0027:87), so these two
     * cannot arise from yan_note_show; they are mapped for exhaustiveness. */
    default: return YAN_MCP_META_IO;
    }
}

static YanMcpMetaResult map_open(YanImageOpenResult result)
{
    switch (result) {
    case YAN_IMAGE_OPEN_OK: return YAN_MCP_META_OK;
    case YAN_IMAGE_OPEN_INVALID: return YAN_MCP_META_INVALID;
    case YAN_IMAGE_OPEN_CORRUPT: return YAN_MCP_META_CORRUPT;
    case YAN_IMAGE_OPEN_UNSUPPORTED: return YAN_MCP_META_UNSUPPORTED;
    case YAN_IMAGE_OPEN_BUSY: return YAN_MCP_META_BUSY;
    default: return YAN_MCP_META_IO;
    }
}

static YanMcpMetaResult map_semantic(YanSemanticResult result)
{
    switch (result) {
    case YAN_SEMANTIC_OK: return YAN_MCP_META_OK;
    case YAN_SEMANTIC_INVALID: return YAN_MCP_META_INVALID;
    case YAN_SEMANTIC_BUSY: return YAN_MCP_META_BUSY;
    case YAN_SEMANTIC_CORRUPT: return YAN_MCP_META_CORRUPT;
    case YAN_SEMANTIC_UNSUPPORTED: return YAN_MCP_META_UNSUPPORTED;
    case YAN_SEMANTIC_NOT_FOUND: return YAN_MCP_META_NOT_FOUND;
    default: return YAN_MCP_META_IO;
    }
}

static bool add_sha256(yyjson_mut_doc *doc, yyjson_mut_val *object,
                       const char *key, const uint8_t digest[32])
{
    static const char hex[] = "0123456789abcdef";
    char text[65];
    for (unsigned i = 0; i < 32; ++i) {
        text[2 * i] = hex[digest[i] >> 4];
        text[2 * i + 1] = hex[digest[i] & 15u];
    }
    text[64] = 0;
    return yyjson_mut_obj_add_strcpy(doc, object, key, text);
}

/* Copies one parsed header field into the JSON object, or null when absent.
 * Length 0 with has_* false is the documented "absent" encoding (0027 uses
 * has_* flags rather than a sentinel), so an empty string is never used to
 * stand in for a missing field. */
static bool add_field(yyjson_mut_doc *doc, yyjson_mut_val *root,
                      const char *key, bool present, const uint8_t *bytes,
                      uint32_t length)
{
    if (!present) return yyjson_mut_obj_add_null(doc, root, key);
    return yyjson_mut_obj_add_strncpy(doc, root, key, (const char *)bytes, length);
}

static YanMcpMetaResult encode(const char *name, const uint8_t source[32],
                               const YanNoteInfo *info, char **payload,
                               size_t *length)
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return YAN_MCP_META_IO;
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    bool ok = root && yyjson_mut_obj_add_uint(doc, root, "schema_version", 1) &&
        yyjson_mut_obj_add_str(doc, root, "operation", "note_meta") &&
        add_sha256(doc, root, "source_sha256", source) &&
        yyjson_mut_obj_add_strcpy(doc, root, "name", name) &&
        yyjson_mut_obj_add_bool(doc, root, "has_header", info->has_header) &&
        add_field(doc, root, "title", info->has_title, info->title,
                  info->title_length) &&
        add_field(doc, root, "created", info->has_created, info->created,
                  info->created_length) &&
        add_field(doc, root, "modified", info->has_modified, info->modified,
                  info->modified_length);
    yyjson_mut_val *tags = ok ? yyjson_mut_arr(doc) : NULL;
    ok = ok && tags != NULL;
    for (uint16_t i = 0; ok && i < info->tag_count; ++i)
        ok = yyjson_mut_arr_add_strncpy(doc, tags, (const char *)info->tag[i],
                                        info->tag_length[i]);
    yyjson_mut_val *links = ok ? yyjson_mut_arr(doc) : NULL;
    ok = ok && links != NULL;
    for (uint16_t i = 0; ok && i < info->link_count; ++i) {
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        ok = item && yyjson_mut_obj_add_strcpy(doc, item, "name",
                                               (const char *)info->link[i]) &&
            yyjson_mut_obj_add_str(doc, item, "state",
                                   info->link_exists[i] ? "OK" : "MISSING") &&
            yyjson_mut_arr_add_val(links, item);
    }
    ok = ok && yyjson_mut_obj_add_val(doc, root, "tags", tags) &&
        yyjson_mut_obj_add_val(doc, root, "links", links) &&
        yyjson_mut_obj_add_bool(doc, root, "complete", true);
    if (ok) *payload = yyjson_mut_write_opts(doc, 0, NULL, length, NULL);
    yyjson_mut_doc_free(doc);
    return *payload ? YAN_MCP_META_OK : YAN_MCP_META_IO;
}

YanMcpMetaResult yan_mcp_meta_execute(const char *image, const char *name,
                                      const uint8_t *expected,
                                      char **payload, size_t *length)
{
    if (!payload || !length) return YAN_MCP_META_INVALID;
    *payload = NULL;
    *length = 0;
    if (!image || !*image || !name) return YAN_MCP_META_INVALID;
    size_t name_length = strlen(name);
    /* The 0027 service applies the same YanFS 1..31 byte rule; checking here
     * keeps a bad name from reaching any source I/O (0027's argument order). */
    if (name_length == 0 || name_length > YAN_FS_NAME_MAX ||
        !yan_note_name_valid((const uint8_t *)name, (uint32_t)name_length))
        return YAN_MCP_META_INVALID;
    uint8_t expect[32], before[32], after[32];
    bool has_expected = expected != NULL;
    if (has_expected) memcpy(expect, expected, sizeof expect);

    YanSemanticPosix adapter;
    memset(&adapter, 0, sizeof adapter);
    adapter.image_fd = adapter.index_fd = -1;
    YanMcpMetaResult result = map_open(
        yan_semantic_posix_open_image_ex(&adapter, image));
    if (result != YAN_MCP_META_OK) return result;

    uint32_t capacity = adapter.fs.entry_capacity;
    YanNoteEntry *entries = NULL;
    char (*sort_name)[32] = NULL;
    uint16_t *sort_index = NULL;
    YanNoteContext *note = NULL;
    if (capacity == 0u) {
        result = YAN_MCP_META_INVALID;
        goto out;
    }
    /* The tables are required by yan_note_init even though an observation uses
     * only the prefix/workspace windows, so they are sized from the mounted
     * image's own entry capacity rather than a hard-coded 63. */
    entries = calloc(capacity, sizeof *entries);
    sort_name = calloc(capacity, sizeof *sort_name);
    sort_index = calloc(capacity, sizeof *sort_index);
    note = calloc(1, sizeof *note);
    if (!entries || !sort_name || !sort_index || !note) {
        result = YAN_MCP_META_IO;
        goto out;
    }
    result = map_note(yan_note_init(note, &adapter.fs, entries, sort_name,
                                    sort_index));
    if (result != YAN_MCP_META_OK) goto out;

    YanSemanticSource source;
    yan_semantic_posix_bind_source(&adapter, &source);
    result = map_semantic(yan_semantic_source_digest(&source, before));
    if (result != YAN_MCP_META_OK) goto out;
    /* STALE is decided before any per-file check, matching the 0033 block
     * tools (host/block_query.c observe -> expected_differs -> service). One
     * server must not run two orders: a caller sending a stale identity for a
     * missing file has to see STALE here just as it does for
     * yanos_block_list. The comparison is against `before`, the identity this
     * call actually observed first. */
    if (has_expected && memcmp(expect, before, 32) != 0) {
        result = YAN_MCP_META_STALE;
        goto out;
    }

    YanNoteInfo info;
    memset(&info, 0, sizeof info);
    result = map_note(yan_note_show(note, name, &info));
    if (result != YAN_MCP_META_OK) goto out;

    result = map_semantic(yan_semantic_source_digest(&source, after));
    if (result != YAN_MCP_META_OK) goto out;
    /* The source must not have moved while the header was read; a change is
     * reported as STALE rather than as a snapshot. */
    if (memcmp(before, after, 32) != 0) {
        result = YAN_MCP_META_STALE;
        goto out;
    }
    result = encode(name, after, &info, payload, length);

out:
    free(note);
    free(sort_index);
    free(sort_name);
    free(entries);
    (void)yan_semantic_posix_cleanup(&adapter);
    if (result != YAN_MCP_META_OK) {
        free(*payload);
        *payload = NULL;
        *length = 0;
    }
    return result;
}

const char *yan_mcp_meta_result_name(YanMcpMetaResult result)
{
    switch (result) {
    case YAN_MCP_META_OK: return "OK";
    case YAN_MCP_META_INVALID: return "INVALID";
    case YAN_MCP_META_BUSY: return "BUSY";
    case YAN_MCP_META_STALE: return "STALE";
    case YAN_MCP_META_METADATA: return "METADATA";
    case YAN_MCP_META_NOT_FOUND: return "NOT_FOUND";
    case YAN_MCP_META_CORRUPT: return "CORRUPT";
    case YAN_MCP_META_UNSUPPORTED: return "UNSUPPORTED";
    case YAN_MCP_META_IO: return "IO";
    }
    return "IO";
}
