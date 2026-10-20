#include "mcp_note.h"
#include <string.h>

/* Appended after the 0033 six tools only when --allow-write is set. */
static const char *const tool_json[] = {
"{\"name\":\"yanos_note_create\",\"description\":\"新建一个 YanFS 笔记文件；不删除、不改名、不追加，整文件写入，单次最多 16384 字节。提交（dry_run 缺省或 false）必须带预览返回的 expected_source；写入成功响应的 source_sha256 是写后身份，previous_source_sha256 是写前身份（create 时为 null）。学习笔记只放在学习镜像：正文用固定 ID 的五个具名块 criteria（学会标准，每条一行、可检查）、known（闫博确认已懂）、unknown（已知不懂）、untouched（还没碰过）、log（每次核对的日期、题目、回答要点、被指出的问题和 [[NAME#ID]] 原文出处）。Agent 可以出题、核对、写 `log` 和 `unknown`，把条目移入 `known` 必须在同一会话里得到闫博明确确认。提交前，把预览的 `text` 和 `diff` 展示给闫博，在同一会话里得到明确确认后，才用同一 name、同一 text、预览返回的身份提交。\",\"inputSchema\":{\"type\":\"object\",\"additionalProperties\":false,\"required\":[\"name\",\"text\"],\"properties\":{\"name\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":31},\"text\":{\"type\":\"string\"},\"expected_source\":{\"type\":\"string\",\"pattern\":\"^[0-9a-f]{64}$\"},\"dry_run\":{\"type\":\"boolean\"}}},\"outputSchema\":{\"type\":\"object\",\"oneOf\":[{\"additionalProperties\":false,\"required\":[\"schema_version\",\"operation\",\"name\",\"bytes\",\"source_sha256\",\"previous_source_sha256\"],\"properties\":{\"schema_version\":{\"const\":1},\"operation\":{\"const\":\"note_create\"},\"name\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":31},\"bytes\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":16384},\"source_sha256\":{\"type\":\"string\",\"pattern\":\"^[0-9a-f]{64}$\"},\"previous_source_sha256\":{\"type\":\"null\"},\"unchanged\":{\"type\":\"boolean\"}}},{\"additionalProperties\":false,\"required\":[\"schema_version\",\"operation\",\"name\",\"source_sha256\",\"text\",\"diff\"],\"properties\":{\"schema_version\":{\"const\":1},\"operation\":{\"const\":\"note_create_preview\"},\"name\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":31},\"source_sha256\":{\"type\":\"string\",\"pattern\":\"^[0-9a-f]{64}$\"},\"text\":{\"type\":\"string\"},\"diff\":{\"type\":\"object\",\"additionalProperties\":false,\"required\":[\"start_line\",\"removed\",\"added\"],\"properties\":{\"start_line\":{\"type\":\"integer\",\"minimum\":0},\"removed\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"added\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}}}}}]},\"annotations\":{\"readOnlyHint\":false,\"destructiveHint\":false,\"openWorldHint\":false}}",
"{\"name\":\"yanos_note_replace\",\"description\":\"整文件覆盖一个 YanFS 笔记文件；不删除、不改名、不追加，单次最多 16384 字节。提交（dry_run 缺省或 false）必须带预览返回的 expected_source；写入成功响应的 source_sha256 是写后身份，previous_source_sha256 是写前身份。学习笔记只放在学习镜像：正文用固定 ID 的五个具名块 criteria（学会标准，每条一行、可检查）、known（闫博确认已懂）、unknown（已知不懂）、untouched（还没碰过）、log（每次核对的日期、题目、回答要点、被指出的问题和 [[NAME#ID]] 原文出处）。Agent 可以出题、核对、写 `log` 和 `unknown`，把条目移入 `known` 必须在同一会话里得到闫博明确确认。提交前，把预览的 `text` 和 `diff` 展示给闫博，在同一会话里得到明确确认后，才用同一 name、同一 text、预览返回的身份提交。\",\"inputSchema\":{\"type\":\"object\",\"additionalProperties\":false,\"required\":[\"name\",\"text\"],\"properties\":{\"name\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":31},\"text\":{\"type\":\"string\"},\"expected_source\":{\"type\":\"string\",\"pattern\":\"^[0-9a-f]{64}$\"},\"dry_run\":{\"type\":\"boolean\"}}},\"outputSchema\":{\"type\":\"object\",\"oneOf\":[{\"additionalProperties\":false,\"required\":[\"schema_version\",\"operation\",\"name\",\"bytes\",\"source_sha256\",\"previous_source_sha256\"],\"properties\":{\"schema_version\":{\"const\":1},\"operation\":{\"const\":\"note_replace\"},\"name\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":31},\"bytes\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":16384},\"source_sha256\":{\"type\":\"string\",\"pattern\":\"^[0-9a-f]{64}$\"},\"previous_source_sha256\":{\"type\":\"string\",\"pattern\":\"^[0-9a-f]{64}$\"},\"unchanged\":{\"type\":\"boolean\"}}},{\"additionalProperties\":false,\"required\":[\"schema_version\",\"operation\",\"name\",\"source_sha256\",\"text\",\"diff\"],\"properties\":{\"schema_version\":{\"const\":1},\"operation\":{\"const\":\"note_replace_preview\"},\"name\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":31},\"source_sha256\":{\"type\":\"string\",\"pattern\":\"^[0-9a-f]{64}$\"},\"text\":{\"type\":\"string\"},\"diff\":{\"type\":\"object\",\"additionalProperties\":false,\"required\":[\"start_line\",\"removed\",\"added\"],\"properties\":{\"start_line\":{\"type\":\"integer\",\"minimum\":0},\"removed\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"added\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}}}}}]},\"annotations\":{\"readOnlyHint\":false,\"destructiveHint\":true,\"idempotentHint\":true,\"openWorldHint\":false}}",
};
YanMcpNoteTool yan_mcp_note_tool(const char *name) {
    if (!strcmp(name, "yanos_note_create")) return YAN_NOTE_TOOL_CREATE;
    if (!strcmp(name, "yanos_note_replace")) return YAN_NOTE_TOOL_REPLACE;
    return YAN_NOTE_TOOL_NONE;
}
unsigned yan_mcp_note_tool_count(void)
{ return (unsigned)(sizeof tool_json / sizeof *tool_json); }
const char *yan_mcp_note_tool_json(unsigned index)
{ return index < sizeof tool_json / sizeof *tool_json ? tool_json[index] : NULL; }

#include "note_blocks_format.h"
#include "note_write.h"
#include "semantic_posix.h"
#include <stdlib.h>
#include <sys/file.h>
#include <unistd.h>
#include <yyjson.h>
typedef struct { YanSemanticPosix adapter; int lock_fd; } NoteImage;
static YanMcpNoteResult map_fs(YanFsResult r) {
    switch (r) {
    case YAN_FS_OK: return YAN_MCP_NOTE_OK; case YAN_FS_INVALID: return YAN_MCP_NOTE_INVALID;
    case YAN_FS_BUSY: return YAN_MCP_NOTE_BUSY; case YAN_FS_EXISTS: return YAN_MCP_NOTE_EXISTS;
    case YAN_FS_NOT_FOUND: return YAN_MCP_NOTE_NOT_FOUND; case YAN_FS_DIRECTORY_FULL: return YAN_MCP_NOTE_DIRECTORY_FULL;
    case YAN_FS_NOSPACE: return YAN_MCP_NOTE_NOSPACE; case YAN_FS_CORRUPT: return YAN_MCP_NOTE_CORRUPT;
    case YAN_FS_UNSUPPORTED: return YAN_MCP_NOTE_UNSUPPORTED; default: return YAN_MCP_NOTE_IO;
    }
}
static YanMcpNoteResult map_open(YanImageOpenResult r) {
    switch (r) {
    case YAN_IMAGE_OPEN_OK: return YAN_MCP_NOTE_OK; case YAN_IMAGE_OPEN_INVALID: return YAN_MCP_NOTE_INVALID;
    case YAN_IMAGE_OPEN_CORRUPT: return YAN_MCP_NOTE_CORRUPT; case YAN_IMAGE_OPEN_UNSUPPORTED: return YAN_MCP_NOTE_UNSUPPORTED;
    case YAN_IMAGE_OPEN_BUSY: return YAN_MCP_NOTE_BUSY; default: return YAN_MCP_NOTE_IO;
    }
}
static YanMcpNoteResult map_semantic(YanSemanticResult r) {
    switch (r) {
    case YAN_SEMANTIC_OK: return YAN_MCP_NOTE_OK; case YAN_SEMANTIC_INVALID: return YAN_MCP_NOTE_INVALID;
    case YAN_SEMANTIC_BUSY: return YAN_MCP_NOTE_BUSY; case YAN_SEMANTIC_CORRUPT: return YAN_MCP_NOTE_CORRUPT;
    case YAN_SEMANTIC_UNSUPPORTED: return YAN_MCP_NOTE_UNSUPPORTED; case YAN_SEMANTIC_NOT_FOUND: return YAN_MCP_NOTE_NOT_FOUND;
    default: return YAN_MCP_NOTE_IO;
    }
}
static YanMcpNoteResult open_note(const char *image, int operation, NoteImage *state) {
    memset(state, 0, sizeof *state);
    state->adapter.image_fd = state->adapter.index_fd = -1;
    state->lock_fd = -1;
    YanFsResult locked = yan_image_lock(image, operation, &state->lock_fd);
    if (locked != YAN_FS_OK) return map_fs(locked);
    YanImageOpenResult opened = yan_semantic_posix_open_image_ex(&state->adapter, image);
    if (opened == YAN_IMAGE_OPEN_OK) return YAN_MCP_NOTE_OK;
    (void)yan_semantic_posix_cleanup(&state->adapter);
    (void)close(state->lock_fd); state->lock_fd = -1;
    return map_open(opened);
}
static void close_note(NoteImage *state) {
    (void)yan_semantic_posix_cleanup(&state->adapter);
    if (state->lock_fd >= 0) (void)close(state->lock_fd);
    state->lock_fd = -1;
}
static YanMcpNoteResult note_digest(NoteImage *state, uint8_t digest[32]) {
    YanSemanticSource source;
    yan_semantic_posix_bind_source(&state->adapter, &source);
    return map_semantic(yan_semantic_source_digest(&source, digest));
}
static YanMcpNoteResult note_old(NoteImage *state, const char *name, uint8_t **text,
                                 uint32_t *length, bool *exists) {
    *text = NULL; *length = 0; *exists = false;
    YanFsInfo info;
    YanFsResult result = yan_fs_stat(&state->adapter.fs, name, &info);
    if (result == YAN_FS_NOT_FOUND) return YAN_MCP_NOTE_OK;
    if (result != YAN_FS_OK) return map_fs(result);
    if (info.size_bytes != 0) {
        uint8_t *bytes = malloc(info.size_bytes);
        if (bytes == NULL) return YAN_MCP_NOTE_IO;
        uint32_t got = 0;
        result = yan_fs_read(&state->adapter.fs, name, 0, bytes, info.size_bytes, &got);
        if (result != YAN_FS_OK || got != info.size_bytes) {
            free(bytes);
            return result == YAN_FS_OK ? YAN_MCP_NOTE_IO : map_fs(result);
        }
        *text = bytes; *length = info.size_bytes;
    }
    *exists = true;
    return YAN_MCP_NOTE_OK;
}
static bool same_bytes(const uint8_t *a, uint32_t an, const uint8_t *b, uint32_t bn)
{ return an == bn && (an == 0 || memcmp(a, b, an) == 0); }
static bool text_ok(const uint8_t *text, uint32_t length) {
    YanBlockTextValidator validator;
    return yan_block_text_init(&validator) == YAN_BLOCK_OK &&
        (length == 0 || yan_block_text_feed(&validator, text, length) == YAN_BLOCK_OK) &&
        yan_block_text_finish(&validator) == YAN_BLOCK_OK;
}
static bool add_sha256(yyjson_mut_doc *doc, yyjson_mut_val *object, const char *key,
                       const uint8_t digest[32]) {
    static const char hex[] = "0123456789abcdef";
    char text[65];
    for (unsigned i = 0; i < 32; ++i) {
        text[2 * i] = hex[digest[i] >> 4]; text[2 * i + 1] = hex[digest[i] & 15u];
    }
    text[64] = 0;
    return yyjson_mut_obj_add_strcpy(doc, object, key, text);
}
typedef struct { const uint8_t *text; uint32_t length; } NoteLine;
static uint32_t line_count(const uint8_t *text, uint32_t length) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < length; ++i) if (text[i] == '\n') ++count;
    return length != 0 && text[length - 1] != '\n' ? count + 1 : count;
}
static void split_lines(const uint8_t *text, uint32_t length, NoteLine *lines) {
    uint32_t count = 0, start = 0;
    for (uint32_t i = 0; i < length; ++i) if (text[i] == '\n') {
        lines[count++] = (NoteLine){text + start, i - start + 1}; start = i + 1;
    }
    if (start < length) lines[count] = (NoteLine){text + start, length - start};
}
static bool same_line(NoteLine a, NoteLine b)
{ return a.length == b.length && (a.length == 0 || memcmp(a.text, b.text, a.length) == 0); }
static bool add_diff(yyjson_mut_doc *doc, yyjson_mut_val *root, const uint8_t *old_text,
    uint32_t old_length, const uint8_t *new_text, uint32_t new_length) {
    uint32_t old_count = line_count(old_text, old_length);
    uint32_t new_count = line_count(new_text, new_length);
    NoteLine *old = old_count ? malloc(old_count * sizeof *old) : NULL;
    NoteLine *new = new_count ? malloc(new_count * sizeof *new) : NULL;
    if ((old_count && !old) || (new_count && !new)) { free(old); free(new); return false; }
    split_lines(old_text, old_length, old);
    split_lines(new_text, new_length, new);
    uint32_t prefix = 0, suffix = 0, shortest = old_count < new_count ? old_count : new_count;
    while (prefix < shortest && same_line(old[prefix], new[prefix])) ++prefix;
    while (suffix < shortest - prefix &&
           same_line(old[old_count - 1 - suffix], new[new_count - 1 - suffix])) ++suffix;
    uint32_t removed = old_count - prefix - suffix;
    uint32_t added = new_count - prefix - suffix;
    yyjson_mut_val *diff = yyjson_mut_obj(doc), *removed_value = yyjson_mut_arr(doc);
    yyjson_mut_val *added_value = yyjson_mut_arr(doc);
    bool ok = diff && removed_value && added_value &&
        yyjson_mut_obj_add_uint(doc, diff, "start_line", removed || added ? prefix + 1 : 0);
    for (uint32_t i = 0; ok && i < removed; ++i)
        ok = yyjson_mut_arr_add_strncpy(doc, removed_value,
            (const char *)old[prefix + i].text, old[prefix + i].length);
    for (uint32_t i = 0; ok && i < added; ++i)
        ok = yyjson_mut_arr_add_strncpy(doc, added_value,
            (const char *)new[prefix + i].text, new[prefix + i].length);
    ok = ok && yyjson_mut_obj_add_val(doc, diff, "removed", removed_value) &&
        yyjson_mut_obj_add_val(doc, diff, "added", added_value) &&
        yyjson_mut_obj_add_val(doc, root, "diff", diff);
    free(old); free(new);
    return ok;
}
static YanMcpNoteResult encode_commit(const YanMcpNoteRequest *request,
    const uint8_t *before, const uint8_t *after, bool unchanged, char **payload, size_t *length) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return YAN_MCP_NOTE_IO;
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    bool ok = root && yyjson_mut_obj_add_uint(doc, root, "schema_version", 1) &&
        yyjson_mut_obj_add_str(doc, root, "operation", request->replace ? "note_replace" : "note_create") &&
        yyjson_mut_obj_add_str(doc, root, "name", request->name) &&
        yyjson_mut_obj_add_uint(doc, root, "bytes", request->length) &&
        add_sha256(doc, root, "source_sha256", after);
    if (ok && request->replace) ok = add_sha256(doc, root, "previous_source_sha256", before);
    else if (ok) ok = yyjson_mut_obj_add_null(doc, root, "previous_source_sha256");
    if (ok && unchanged) ok = yyjson_mut_obj_add_bool(doc, root, "unchanged", true);
    if (ok) *payload = yyjson_mut_write_opts(doc, 0, NULL, length, NULL);
    yyjson_mut_doc_free(doc);
    return *payload ? YAN_MCP_NOTE_OK : YAN_MCP_NOTE_IO;
}
static YanMcpNoteResult encode_preview(const YanMcpNoteRequest *request,
    const uint8_t *source, const uint8_t *old_text, uint32_t old_length,
    char **payload, size_t *length) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return YAN_MCP_NOTE_IO;
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    const char *text = (const char *)(request->text ? request->text : (const uint8_t *)"");
    bool ok = root && yyjson_mut_obj_add_uint(doc, root, "schema_version", 1) &&
        yyjson_mut_obj_add_str(doc, root, "operation",
            request->replace ? "note_replace_preview" : "note_create_preview") &&
        yyjson_mut_obj_add_str(doc, root, "name", request->name) &&
        add_sha256(doc, root, "source_sha256", source) &&
        yyjson_mut_obj_add_strncpy(doc, root, "text", text, request->length) &&
        add_diff(doc, root, old_text, old_length, request->text, request->length);
    if (ok) *payload = yyjson_mut_write_opts(doc, 0, NULL, length, NULL);
    yyjson_mut_doc_free(doc);
    return *payload ? YAN_MCP_NOTE_OK : YAN_MCP_NOTE_IO;
}
YanMcpNoteResult yan_mcp_note_execute(const char *image, const YanMcpNoteRequest *request,
                                      char **payload, size_t *length, bool *committed) {
    if (!payload || !length) return YAN_MCP_NOTE_INVALID;
    *payload = NULL; *length = 0;
    if (committed) *committed = false;
    if (!image || !*image || !request || !request->name) return YAN_MCP_NOTE_INVALID;
    if (yan_note_write_check(request->name, request->text, request->length) != YAN_FS_OK)
        return YAN_MCP_NOTE_INVALID;
    if (!request->dry_run && !request->has_expected) return YAN_MCP_NOTE_INVALID;
    NoteImage state;
    YanMcpNoteResult result = open_note(image, request->dry_run ? LOCK_SH : LOCK_EX, &state);
    if (result != YAN_MCP_NOTE_OK) return result;
    uint8_t before[32], after[32];
    uint8_t *old = NULL; uint32_t old_length = 0; bool exists = false;
    result = note_digest(&state, before);
    if (result == YAN_MCP_NOTE_OK) result = note_old(&state, request->name, &old, &old_length, &exists);
    if (result != YAN_MCP_NOTE_OK) goto out;
    if (!request->dry_run && exists && same_bytes(old, old_length, request->text, request->length)) {
        result = encode_commit(request, before, before, true, payload, length); goto out;
    }
    if (request->has_expected && memcmp(request->expected, before, 32) != 0) {
        result = YAN_MCP_NOTE_STALE; goto out;
    }
    if (request->dry_run) {
        if (!request->replace && exists) { result = YAN_MCP_NOTE_EXISTS; goto out; }
        if (request->replace && !exists) { result = YAN_MCP_NOTE_NOT_FOUND; goto out; }
        if (request->replace && !text_ok(old, old_length)) { result = YAN_MCP_NOTE_INVALID_TEXT; goto out; }
        result = encode_preview(request, before, old, old_length, payload, length); goto out;
    }
    if (!request->replace && exists) { result = YAN_MCP_NOTE_EXISTS; goto out; }
    if (request->replace && !exists) { result = YAN_MCP_NOTE_NOT_FOUND; goto out; }
    (void)yan_semantic_posix_cleanup(&state.adapter);
    state.adapter.image_fd = state.adapter.index_fd = -1;
    bool wrote = false;
    YanFsResult written = yan_note_write_locked(image, request->replace, request->name,
                                                request->text, request->length, &wrote);
    if (written != YAN_FS_OK) {
        if (committed) *committed = wrote;
        result = wrote ? YAN_MCP_NOTE_IO_COMMITTED : map_fs(written); goto out;
    }
    if (committed) *committed = true;
    YanImageOpenResult reopened = yan_semantic_posix_open_image_ex(&state.adapter, image);
    if (reopened != YAN_IMAGE_OPEN_OK) {
        (void)yan_semantic_posix_cleanup(&state.adapter);
        result = YAN_MCP_NOTE_IO_COMMITTED; goto out;
    }
    result = note_digest(&state, after);
    if (result != YAN_MCP_NOTE_OK) { result = YAN_MCP_NOTE_IO_COMMITTED; goto out; }
    result = encode_commit(request, before, after, false, payload, length);
    if (result != YAN_MCP_NOTE_OK) result = YAN_MCP_NOTE_IO_COMMITTED;
out:
    free(old);
    close_note(&state);
    if (result != YAN_MCP_NOTE_OK) { free(*payload); *payload = NULL; *length = 0; }
    return result;
}
const char *yan_mcp_note_result_name(YanMcpNoteResult result) {
    switch (result) {
    case YAN_MCP_NOTE_OK: return "OK"; case YAN_MCP_NOTE_INVALID: return "INVALID";
    case YAN_MCP_NOTE_BUSY: return "BUSY"; case YAN_MCP_NOTE_STALE: return "STALE";
    case YAN_MCP_NOTE_EXISTS: return "EXISTS"; case YAN_MCP_NOTE_NOT_FOUND: return "NOT_FOUND";
    case YAN_MCP_NOTE_INVALID_TEXT: return "INVALID_TEXT"; case YAN_MCP_NOTE_CORRUPT: return "CORRUPT";
    case YAN_MCP_NOTE_UNSUPPORTED: return "UNSUPPORTED"; case YAN_MCP_NOTE_DIRECTORY_FULL: return "DIRECTORY_FULL";
    case YAN_MCP_NOTE_NOSPACE: return "NOSPACE"; case YAN_MCP_NOTE_IO: return "IO";
    case YAN_MCP_NOTE_IO_COMMITTED: return "IO";
    }
    return "UNKNOWN";
}