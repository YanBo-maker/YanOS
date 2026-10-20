/* 0027 note-metadata service.
 *
 * The behavioural contract is os/note_metadata.h, os/note_metadata_format.h and
 * docs/specs/0027-note-metadata.md, "命令及可观察输出", "观察与更新的读取范围" and
 * "公共接口、状态与内存". This translation unit owns the filesystem-observing half
 * of the feature:
 *
 *   - observation (yan_note_show): one bounded prefix read of the target file,
 *     the shared pure parser, then one directory lookup per declared link. The
 *     body is never preflighted and a header-less binary file is a valid old
 *     note.
 *   - update (set_title / set_date / tag / link): the source must exist and its
 *     header must be legal before any no-change decision. A request that would
 *     not change the stored metadata returns OK with no body read and no write.
 *     An actual change reads the whole old file, applies the editor text rules,
 *     rebuilds the canonical header plus the preserved body in the 16 KiB
 *     candidate area and publishes with exactly one yan_fs_replace.
 *   - listing (yan_note_list): every directory entry is read and its whole
 *     header validated first; only then are eligible names sorted and
 *     delivered. A broken header anywhere fails the call with no callback and
 *     no published count.
 *
 * Layering: this file includes the pure format and os/yanfs.h only. It performs
 * no Host I/O, allocates nothing, and keeps every large object in the
 * caller-owned YanNoteContext. The interpreter's own frame is a handful of
 * scalars plus small name copies; no 4 KiB or 16 KiB object is automatic.
 *
 * No on-disk format is added: a note is an ordinary YanFS file whose content
 * starts with the pure header, and every write goes through yan_fs_replace.
 *
 * FEFF boundary: the locked SPEC line 17 forbids U+FEFF anywhere in a
 * recognised header; the pure parser enforces that and this layer calls it
 * unchanged for the header. The owner confirmed on 2026-10-08 that the ban
 * includes field values. */

#include "note_metadata.h"

#include "memory.h"
#include "yanfs.h"

/* ------------------------------------------------------------------ guards */

/* The same pure range rule the facade layers use: a span that would leave
 * uintptr_t is reported as overlapping. Nothing is read or written through
 * either range. Zero-length spans are absent and never overlap. */
static bool ranges_overlap(const void *first, size_t first_length,
                           const void *second, size_t second_length)
{
    uintptr_t first_begin = (uintptr_t)first;
    uintptr_t second_begin = (uintptr_t)second;
    if (first_length == 0u || second_length == 0u) {
        return false;
    }
    if ((uint64_t)first_length > (uint64_t)(UINTPTR_MAX - first_begin) ||
        (uint64_t)second_length > (uint64_t)(UINTPTR_MAX - second_begin)) {
        return true;
    }
    uintptr_t first_end = first_begin + (uintptr_t)first_length;
    uintptr_t second_end = second_begin + (uintptr_t)second_length;
    return first_begin < second_end && second_begin < first_end;
}

/* A nonempty span needs a base and a range that stays inside uintptr_t. A
 * zero-length span is legal even with a NULL base and is never dereferenced. */
static bool span_valid(const void *base, size_t size)
{
    if (size == 0u) {
        return true;
    }
    if (base == NULL) {
        return false;
    }
    return (uint64_t)size <= (uint64_t)(UINTPTR_MAX - (uintptr_t)base);
}

/* Explicit, exhaustive mapping. Never a cast between the two enums: a compiler
 * that adds an enumerator forces a decision here, and the default is the
 * conservative protocol error rather than a silent misclassification. */
static YanNoteResult note_map_fs(YanFsResult result)
{
    switch (result) {
    case YAN_FS_OK: return YAN_NOTE_OK;
    case YAN_FS_END: return YAN_NOTE_OK;
    case YAN_FS_INVALID: return YAN_NOTE_INVALID;
    case YAN_FS_NOT_MOUNTED: return YAN_NOTE_NOT_MOUNTED;
    case YAN_FS_FAULTED: return YAN_NOTE_FAULTED;
    case YAN_FS_BUSY: return YAN_NOTE_BUSY;
    case YAN_FS_EXISTS: return YAN_NOTE_EXISTS;
    case YAN_FS_NOT_FOUND: return YAN_NOTE_NOT_FOUND;
    case YAN_FS_DIRECTORY_FULL: return YAN_NOTE_DIRECTORY_FULL;
    case YAN_FS_NOSPACE: return YAN_NOTE_NOSPACE;
    case YAN_FS_CORRUPT: return YAN_NOTE_CORRUPT;
    case YAN_FS_UNSUPPORTED: return YAN_NOTE_UNSUPPORTED;
    case YAN_FS_IO: return YAN_NOTE_IO;
    case YAN_FS_PROTOCOL: return YAN_NOTE_PROTOCOL;
    }
    return YAN_NOTE_PROTOCOL;
}

/* Borrowed-source state, in the order 0021 fixes: not initialized first (the
 * caller must already have proven the borrowed pointer itself), then BUSY, then
 * the fault record, then the mount state. Reading the state field is only safe
 * once `initialized` is known true, so the caller passes that value in. */
static YanNoteResult fs_state(const YanFs *fs, bool initialized)
{
    if (!initialized) {
        return YAN_NOTE_INVALID;
    }
    if (fs->busy) {
        return YAN_NOTE_BUSY;
    }
    if (fs->state == YAN_FS_STATE_FAULTED) {
        return YAN_NOTE_FAULTED;
    }
    if (fs->state != YAN_FS_MOUNTED) {
        return YAN_NOTE_NOT_MOUNTED;
    }
    return YAN_NOTE_OK;
}

/* The shared prefix of every public entry point. `bytes`/`bytes_length` is the
 * optional read-only input span (a value, a filter or a name); `holder`/
 * `holder_length` is the optional writable output span (an info or a shown
 * counter). Both are validated with pure address arithmetic before the first
 * input byte is read, and the context and the borrowed filesystem are both
 * protected ranges. */
static YanNoteResult service_guard(YanNoteContext *context,
                                   const void *bytes, size_t bytes_length,
                                   const void *holder, size_t holder_length)
{
    if (!span_valid(context, sizeof *context)) {
        return YAN_NOTE_INVALID;
    }
    /* A span that leaves uintptr_t is reported before any context bool is read,
     * so a wrapping input can never reach a field load. */
    if (!context->initialized) {
        return YAN_NOTE_INVALID;
    }
    if (context->busy) {
        return YAN_NOTE_BUSY;
    }
    const YanFs *fs = context->fs;
    if (!span_valid(fs, sizeof *fs) ||
        ranges_overlap(context, sizeof *context, fs, sizeof *fs)) {
        return YAN_NOTE_INVALID;
    }
    YanNoteResult state = fs_state(fs, fs->initialized);
    if (state != YAN_NOTE_OK) {
        return state;
    }
    if (!span_valid(bytes, bytes_length) || !span_valid(holder, holder_length)) {
        return YAN_NOTE_INVALID;
    }
    if (ranges_overlap(bytes, bytes_length, context, sizeof *context) ||
        ranges_overlap(holder, holder_length, context, sizeof *context) ||
        ranges_overlap(bytes, bytes_length, fs, sizeof *fs) ||
        ranges_overlap(holder, holder_length, fs, sizeof *fs)) {
        return YAN_NOTE_INVALID;
    }
    return YAN_NOTE_OK;
}

/* ------------------------------------------------------------ name reading */

/* Reads one caller name incrementally: every byte's address is checked against
 * the context and the filesystem before it is read, and the scan stops after 32
 * bytes so a name without a terminator cannot walk out of its object. */
static bool read_name(const YanNoteContext *context, const char *name,
                      char out[32])
{
    if (name == NULL) {
        return false;
    }
    const YanFs *fs = context->fs;
    uint32_t length = 0u;
    while (length < 32u) {
        if (length > UINTPTR_MAX - (uintptr_t)name) return false;
        const void *byte_address = (const void *)((uintptr_t)name + length);
        if (!span_valid(byte_address, 1u) ||
            ranges_overlap(byte_address, 1u, context, sizeof *context) ||
            ranges_overlap(byte_address, 1u, fs, sizeof *fs)) {
            return false;
        }
        char byte = name[length];
        if (byte == '\0') {
            break;
        }
        out[length] = byte;
        ++length;
    }
    if (length == 0u || length == 32u) {
        return false; /* empty, or no terminator inside the name area */
    }
    out[length] = '\0';
    return yan_note_name_valid((const uint8_t *)out, length);
}

/* --------------------------------------------------------- directory table */

static YanNoteResult load_snapshot(YanNoteContext *context)
{
    context->entry_count = 0u;
    uint32_t cursor = 0u;
    YanFsInfo info;
    for (;;) {
        YanFsResult listed = yan_fs_list(context->fs, &cursor, &info);
        if (listed == YAN_FS_END) return YAN_NOTE_OK;
        if (listed != YAN_FS_OK) return note_map_fs(listed);
        if (context->entry_count >= context->capacity) return YAN_NOTE_PROTOCOL;
        memcpy(context->entries[context->entry_count].name, info.name,
               sizeof info.name);
        context->entries[context->entry_count].size_bytes = info.size_bytes;
        context->entry_count += 1u;
    }
}

/* ------------------------------------------------------- bounded prefix I/O */

/* Reads at most one bounded prefix of the named file into context->prefix and
 * parses it into context->current. `size_out` receives the file's real size as
 * the directory records it. It never reads a body byte: the request is clamped
 * to the prefix area, so a multi-MiB file costs one block read. */
static YanNoteResult read_head(YanNoteContext *context, const char *name,
                               uint32_t *size_out)
{
    YanFsInfo info;
    YanFsResult stat_result = yan_fs_stat(context->fs, name, &info);
    if (stat_result != YAN_FS_OK) {
        return note_map_fs(stat_result);
    }
    uint32_t want = info.size_bytes < YAN_NOTE_PREFIX_BYTES
                        ? info.size_bytes
                        : YAN_NOTE_PREFIX_BYTES;
    uint32_t got = 0u;
    YanFsResult read_result = yan_fs_read(context->fs, name, 0u,
                                          context->prefix, want, &got);
    if (read_result != YAN_FS_OK) {
        return note_map_fs(read_result);
    }
    if (got != want) {
        return YAN_NOTE_PROTOCOL; /* a short read is a broken source */
    }
    YanNoteResult parsed = yan_note_parse_header(context->prefix, want,
                                                 &context->current,
                                                 &context->parse_workspace);
    if (parsed != YAN_NOTE_OK) {
        return parsed;
    }
    *size_out = info.size_bytes;
    return YAN_NOTE_OK;
}

/* ------------------------------------------------------------ editor text */

/* One UTF-8 scalar, enforcing the shortest legal encoding: rejects a
 * continuation lead, an overlong form, a UTF-16 surrogate and any scalar above
 * U+10FFFF. Returns the byte length, or 0 on a violation. */
static uint32_t editor_scalar(const uint8_t *bytes, uint32_t length,
                              uint32_t *code_point)
{
    if (length == 0u) {
        return 0u;
    }
    uint8_t lead = bytes[0];
    uint32_t need;
    uint32_t value;
    uint32_t minimum;
    if (lead < 0x80u) {
        need = 1u;
        value = lead;
        minimum = 0u;
    } else if (lead >= 0xC2u && lead <= 0xDFu) {
        need = 2u;
        value = (uint32_t)(lead & 0x1Fu);
        minimum = UINT32_C(0x80);
    } else if (lead >= 0xE0u && lead <= 0xEFu) {
        need = 3u;
        value = (uint32_t)(lead & 0x0Fu);
        minimum = UINT32_C(0x800);
    } else if (lead >= 0xF0u && lead <= 0xF4u) {
        need = 4u;
        value = (uint32_t)(lead & 0x07u);
        minimum = UINT32_C(0x10000);
    } else {
        return 0u;
    }
    if (length < need) {
        return 0u;
    }
    for (uint32_t i = 1u; i < need; ++i) {
        uint8_t continuation = bytes[i];
        if (continuation < 0x80u || continuation > 0xBFu) {
            return 0u;
        }
        value = (value << 6) | (uint32_t)(continuation & 0x3Fu);
    }
    if (value < minimum || value > UINT32_C(0x10FFFF) ||
        (value >= UINT32_C(0xD800) && value <= UINT32_C(0xDFFF))) {
        return 0u;
    }
    *code_point = value;
    return need;
}

/* True when the whole buffer is text the editor rules accept: legal UTF-8 that
 * may contain TAB, LF and CRLF, and no bare CR, no other C0, no DEL and no C1.
 * This is the same set os/editor.c fixes for a saved draft; it is implemented
 * here rather than called, because editor.c works on its own draft object. */
static bool editor_text_valid(const uint8_t *bytes, uint32_t length)
{
    uint32_t offset = 0u;
    while (offset < length) {
        uint8_t byte = bytes[offset];
        if (byte < 0x80u) {
            if (byte == (uint8_t)'\r') {
                if (offset + 1u >= length ||
                    bytes[offset + 1u] != (uint8_t)'\n') {
                    return false; /* bare CR */
                }
                offset += 2u; /* CRLF is one terminator */
                continue;
            }
            if (byte == (uint8_t)'\n' || byte == (uint8_t)'\t') {
                offset += 1u;
                continue;
            }
            if (byte < 0x20u || byte == 0x7Fu) {
                return false; /* other C0, or DEL */
            }
            offset += 1u;
            continue;
        }
        uint32_t code_point = 0u;
        uint32_t need = editor_scalar(bytes + offset, length - offset,
                                      &code_point);
        if (need == 0u) {
            return false;
        }
        if (code_point >= UINT32_C(0x80) && code_point <= UINT32_C(0x9F)) {
            return false; /* C1 */
        }
        offset += need;
    }
    return true;
}

static int name_compare(const char *left, const char *right)
{
    for (uint32_t i = 0u; i < 32u; ++i) {
        unsigned char a = (unsigned char)left[i], b = (unsigned char)right[i];
        if (a != b) return a < b ? -1 : 1;
        if (a == 0u) return 0;
    }
    return 0;
}

/* ------------------------------------------------------------- tag helpers */

static bool tag_present(const YanNoteInfo *info, const uint8_t *value,
                        uint32_t length)
{
    for (uint16_t i = 0u; i < info->tag_count; ++i) {
        if (info->tag_length[i] == (uint16_t)length &&
            memcmp(info->tag[i], value, (size_t)length) == 0) {
            return true;
        }
    }
    return false;
}

static bool link_present(const YanNoteInfo *info, const char *target)
{
    uint32_t length = (uint32_t)strlen(target);
    for (uint16_t i = 0u; i < info->link_count; ++i) {
        if (info->link_length[i] == (uint16_t)length &&
            memcmp(info->link[i], target, (size_t)length) == 0) {
            return true;
        }
    }
    return false;
}

static bool tag_append(YanNoteInfo *info, const uint8_t *value, uint32_t length)
{
    if (info->tag_count >= YAN_NOTE_TAG_MAX_COUNT) {
        return false;
    }
    uint16_t index = info->tag_count;
    info->tag_length[index] = (uint16_t)length;
    memcpy(info->tag[index], value, (size_t)length);
    info->tag[index][length] = 0u;
    info->tag_count = (uint16_t)(index + 1u);
    return true;
}

static void tag_remove(YanNoteInfo *info, const uint8_t *value, uint32_t length)
{
    for (uint16_t i = 0u; i < info->tag_count; ++i) {
        if (info->tag_length[i] == (uint16_t)length &&
            memcmp(info->tag[i], value, (size_t)length) == 0) {
            for (uint16_t j = i; j + 1u < info->tag_count; ++j) {
                info->tag_length[j] = info->tag_length[j + 1u];
                memcpy(info->tag[j], info->tag[j + 1u], sizeof info->tag[j]);
            }
            info->tag_count -= 1u;
            return;
        }
    }
}

static bool link_append(YanNoteInfo *info, const char *target, uint32_t length)
{
    if (info->link_count >= YAN_NOTE_LINK_MAX_COUNT) {
        return false;
    }
    uint16_t index = info->link_count;
    info->link_length[index] = (uint16_t)length;
    memcpy(info->link[index], target, (size_t)length);
    info->link[index][length] = 0u;
    info->link_count = (uint16_t)(index + 1u);
    return true;
}

static void link_remove(YanNoteInfo *info, const char *target, uint32_t length)
{
    for (uint16_t i = 0u; i < info->link_count; ++i) {
        if (info->link_length[i] == (uint16_t)length &&
            memcmp(info->link[i], target, (size_t)length) == 0) {
            for (uint16_t j = i; j + 1u < info->link_count; ++j) {
                info->link_length[j] = info->link_length[j + 1u];
                memcpy(info->link[j], info->link[j + 1u], sizeof info->link[j]);
            }
            info->link_count -= 1u;
            return;
        }
    }
}

/* ---------------------------------------------------------- actual change */

/* Builds the candidate file in context->candidate and publishes it with exactly
 * one yan_fs_replace. The old file is read completely here, which is why the
 * no-change path must have returned before reaching this function. The encoded
 * header goes into context->prefix so the candidate's body staging area is not
 * overwritten, and the body is moved byte by byte because there is no memmove.
 *
 * The old body offset remains in context->current; pending metadata lives in
 * context->working and is published only after a successful replace. */
static YanNoteResult publish_change(YanNoteContext *context, const char *name,
                                    uint32_t old_size)
{
    if (old_size > YAN_NOTE_CANDIDATE_BYTES) {
        return YAN_NOTE_TOO_LARGE;
    }
    uint32_t old_bytes = 0u;
    YanFsResult read_result = yan_fs_read(context->fs, name, 0u,
                                          context->candidate, old_size,
                                          &old_bytes);
    if (read_result != YAN_FS_OK) {
        return note_map_fs(read_result);
    }
    if (old_bytes != old_size) {
        return YAN_NOTE_PROTOCOL;
    }
    if (!editor_text_valid(context->candidate, old_size)) {
        return YAN_NOTE_INVALID_TEXT;
    }

    uint32_t encoded = 0u;
    YanNoteResult encoded_result =
        yan_note_encode_header(&context->working, context->prefix,
                               YAN_NOTE_PREFIX_BYTES, &encoded);
    if (encoded_result != YAN_NOTE_OK) {
        return encoded_result;
    }
    if (encoded > YAN_NOTE_CANDIDATE_BYTES) {
        return YAN_NOTE_TOO_LARGE;
    }

    uint32_t old_header = context->current.header_bytes;
    if (old_header > old_size) {
        return YAN_NOTE_PROTOCOL;
    }
    uint32_t body_length = old_size - old_header;
    if (body_length > YAN_NOTE_CANDIDATE_BYTES - encoded) {
        return YAN_NOTE_TOO_LARGE;
    }

    if (encoded > old_header) {
        for (uint32_t i = body_length; i > 0u; --i) {
            context->candidate[encoded + i - 1u] =
                context->candidate[old_header + i - 1u];
        }
    } else if (encoded < old_header) {
        for (uint32_t i = 0u; i < body_length; ++i) {
            context->candidate[encoded + i] =
                context->candidate[old_header + i];
        }
    }
    memcpy(context->candidate, context->prefix, (size_t)encoded);

    YanFsResult replace_result =
        yan_fs_replace(context->fs, name, context->candidate,
                       encoded + body_length);
    if (replace_result != YAN_FS_OK) {
        return note_map_fs(replace_result);
    }
    context->working.has_header = true;
    context->working.header_bytes = encoded;
    context->current = context->working;
    return YAN_NOTE_OK;
}

/* -------------------------------------------------------------- public API */

uint32_t yan_note_entries_storage_bytes(uint32_t capacity)
{
    return capacity * (uint32_t)sizeof(YanNoteEntry);
}

uint32_t yan_note_sort_name_storage_bytes(uint32_t capacity)
{
    return capacity * 32u;
}

YanNoteResult yan_note_init(YanNoteContext *context, YanFs *fs,
                            YanNoteEntry *entries, char (*sort_name)[32])
{
    if (context == NULL || fs == NULL || entries == NULL || sort_name == NULL) {
        return YAN_NOTE_INVALID;
    }
    if (!span_valid(context, sizeof *context) || !span_valid(fs, sizeof *fs)) {
        return YAN_NOTE_INVALID;
    }
    if (ranges_overlap(context, sizeof *context, fs, sizeof *fs)) {
        return YAN_NOTE_INVALID;
    }
    if (context->initialized && context->busy) {
        return YAN_NOTE_BUSY; /* an active operation owns the object */
    }
    /* 0036-R3: the terminal initializes the services between init_directory
     * and mount, so an initialized but still UNMOUNTED filesystem is legal
     * here. A faulted one is not, and queries still require MOUNTED. */
    if (!fs->initialized) return YAN_NOTE_INVALID;
    if (fs->busy) return YAN_NOTE_BUSY;
    if (fs->state == YAN_FS_STATE_FAULTED) return YAN_NOTE_FAULTED;
    if (fs->entry_capacity == 0u) return YAN_NOTE_INVALID;
    uint32_t capacity = fs->entry_capacity;
    memset(context, 0, sizeof *context);
    context->fs = fs;
    context->entries = entries;
    context->sort_name = sort_name;
    context->capacity = capacity;
    context->initialized = true;
    context->busy = false;
    return YAN_NOTE_OK;
}

YanNoteResult yan_note_show(YanNoteContext *context, const char *name,
                            YanNoteInfo *info_out)
{
    YanNoteResult guard =
        service_guard(context, name, 1u, info_out, sizeof *info_out);
    if (guard != YAN_NOTE_OK) {
        return guard;
    }
    if (info_out == NULL) {
        return YAN_NOTE_INVALID;
    }
    if (ranges_overlap(name, 1u, info_out, sizeof *info_out)) {
        return YAN_NOTE_INVALID;
    }
    char name_copy[32];
    if (!read_name(context, name, name_copy)) {
        return YAN_NOTE_INVALID;
    }
    if (ranges_overlap(name, strlen(name_copy) + 1u, info_out, sizeof *info_out)) {
        return YAN_NOTE_INVALID;
    }

    context->busy = true;
    uint32_t size = 0u;
    YanNoteResult result = read_head(context, name_copy, &size);
    if (result == YAN_NOTE_OK) {
        /* The whole header is validated before a single holder byte is
         * written, so a failure anywhere leaves *info_out untouched. */
        for (uint16_t i = 0u; i < context->current.link_count; ++i) {
            YanFsInfo target;
            YanFsResult looked_up = yan_fs_stat(
                context->fs, (const char *)context->current.link[i], &target);
            if (looked_up == YAN_FS_OK) {
                context->current.link_exists[i] = true;
            } else if (looked_up == YAN_FS_NOT_FOUND) {
                context->current.link_exists[i] = false; /* DANGLING */
            } else {
                result = note_map_fs(looked_up);
                break;
            }
        }
    }
    if (result == YAN_NOTE_OK) {
        *info_out = context->current;
    }
    context->busy = false;
    return result;
}

YanNoteResult yan_note_set_title(YanNoteContext *context, const char *name,
                                 const uint8_t *bytes, uint32_t length)
{
    bool clearing = length == 0u;
    YanNoteResult guard = service_guard(context, name, 1u, bytes,
                                        (size_t)length);
    if (guard != YAN_NOTE_OK) {
        return guard;
    }
    if (!clearing && !yan_note_title_valid(bytes, length)) {
        return YAN_NOTE_INVALID;
    }
    char name_copy[32];
    if (!read_name(context, name, name_copy)) {
        return YAN_NOTE_INVALID;
    }

    context->busy = true;
    uint32_t old_size = 0u;
    YanNoteResult result = read_head(context, name_copy, &old_size);
    if (result != YAN_NOTE_OK) {
        context->busy = false;
        return result;
    }
    context->working = context->current;
    bool changed = false;
    if (clearing) {
        changed = context->working.has_title;
        context->working.has_title = false;
        context->working.title_length = 0u;
    } else if (!context->working.has_title ||
               context->working.title_length != length ||
               memcmp(context->working.title, bytes, (size_t)length) != 0) {
        changed = true;
        context->working.has_title = true;
        context->working.title_length = (uint16_t)length;
        memcpy(context->working.title, bytes, (size_t)length);
        context->working.title[length] = 0u;
    }
    if (!changed) {
        context->busy = false;
        return YAN_NOTE_OK;
    }

    result = publish_change(context, name_copy, old_size);
    context->busy = false;
    return result;
}

YanNoteResult yan_note_set_date(YanNoteContext *context, const char *name,
                                YanNoteDateField field, const uint8_t *bytes,
                                uint32_t length)
{
    bool clearing = length == 0u;
    YanNoteResult guard = service_guard(context, name, 1u, bytes,
                                        (size_t)length);
    if (guard != YAN_NOTE_OK) {
        return guard;
    }
    if (field != YAN_NOTE_DATE_CREATED && field != YAN_NOTE_DATE_MODIFIED) {
        return YAN_NOTE_INVALID;
    }
    if (!clearing && !yan_note_date_valid(bytes, length)) {
        return YAN_NOTE_INVALID;
    }
    char name_copy[32];
    if (!read_name(context, name, name_copy)) {
        return YAN_NOTE_INVALID;
    }

    context->busy = true;
    uint32_t old_size = 0u;
    YanNoteResult result = read_head(context, name_copy, &old_size);
    if (result != YAN_NOTE_OK) {
        context->busy = false;
        return result;
    }
    context->working = context->current;
    bool created = field == YAN_NOTE_DATE_CREATED;
    bool *flag = created ? &context->working.has_created
                         : &context->working.has_modified;
    uint16_t *stored_length = created ? &context->working.created_length
                                      : &context->working.modified_length;
    uint8_t *stored = created ? context->working.created
                              : context->working.modified;
    bool changed = false;
    if (clearing) {
        if (*flag) {
            changed = true;
            *flag = false;
            *stored_length = 0u;
            memcpy(stored, "0000-00-00", YAN_NOTE_DATE_LENGTH);
        }
    } else if (!*flag || *stored_length != length ||
               memcmp(stored, bytes, (size_t)length) != 0) {
        changed = true;
        *flag = true;
        *stored_length = (uint16_t)length;
        memcpy(stored, bytes, (size_t)length);
        stored[length] = 0u;
    }
    if (!changed) {
        context->busy = false;
        return YAN_NOTE_OK;
    }

    result = publish_change(context, name_copy, old_size);
    context->busy = false;
    return result;
}

YanNoteResult yan_note_tag(YanNoteContext *context, const char *name,
                           YanNoteManageMode mode, const uint8_t *bytes,
                           uint32_t length)
{
    YanNoteResult guard = service_guard(context, name, 1u, bytes,
                                        (size_t)length);
    if (guard != YAN_NOTE_OK) {
        return guard;
    }
    if (mode != YAN_NOTE_MANAGE_ADD && mode != YAN_NOTE_MANAGE_DELETE) {
        return YAN_NOTE_INVALID;
    }
    if (!yan_note_tag_valid(bytes, length)) {
        return YAN_NOTE_INVALID;
    }
    char name_copy[32];
    if (!read_name(context, name, name_copy)) {
        return YAN_NOTE_INVALID;
    }

    context->busy = true;
    uint32_t old_size = 0u;
    YanNoteResult result = read_head(context, name_copy, &old_size);
    if (result != YAN_NOTE_OK) {
        context->busy = false;
        return result;
    }
    context->working = context->current;
    bool changed = false;
    if (mode == YAN_NOTE_MANAGE_ADD) {
        if (!tag_present(&context->working, bytes, length)) {
            if (!tag_append(&context->working, bytes, length)) {
                context->busy = false;
                return YAN_NOTE_INVALID; /* already 16: no write */
            }
            changed = true;
        }
    } else if (tag_present(&context->working, bytes, length)) {
        tag_remove(&context->working, bytes, length);
        changed = true;
    }
    if (!changed) {
        context->busy = false;
        return YAN_NOTE_OK;
    }
    result = publish_change(context, name_copy, old_size);
    context->busy = false;
    return result;
}

YanNoteResult yan_note_link(YanNoteContext *context, const char *name,
                            YanNoteManageMode mode, const char *target)
{
    YanNoteResult guard = service_guard(context, name, 1u, target, 1u);
    if (guard != YAN_NOTE_OK) {
        return guard;
    }
    if (mode != YAN_NOTE_MANAGE_ADD && mode != YAN_NOTE_MANAGE_DELETE) {
        return YAN_NOTE_INVALID;
    }
    char name_copy[32];
    if (!read_name(context, name, name_copy)) {
        return YAN_NOTE_INVALID;
    }
    char target_copy[32];
    if (!read_name(context, target, target_copy)) {
        return YAN_NOTE_INVALID;
    }
    uint32_t target_length = (uint32_t)strlen(target_copy);
    if (!yan_note_name_valid((const uint8_t *)target_copy, target_length)) {
        return YAN_NOTE_INVALID;
    }

    context->busy = true;
    uint32_t old_size = 0u;
    YanNoteResult result = read_head(context, name_copy, &old_size);
    if (result != YAN_NOTE_OK) {
        context->busy = false;
        return result;
    }
    context->working = context->current;
    bool changed = false;
    if (mode == YAN_NOTE_MANAGE_ADD) {
        if (!link_present(&context->working, target_copy)) {
            if (!link_append(&context->working, target_copy, target_length)) {
                context->busy = false;
                return YAN_NOTE_INVALID; /* already 16: no write */
            }
            changed = true;
        }
    } else if (link_present(&context->working, target_copy)) {
        link_remove(&context->working, target_copy, target_length);
        changed = true;
    }
    if (!changed) {
        context->busy = false;
        return YAN_NOTE_OK;
    }
    result = publish_change(context, name_copy, old_size);
    context->busy = false;
    return result;
}

YanNoteResult yan_note_list(YanNoteContext *context,
                            const YanNoteTagFilter *filter,
                            YanNoteListFn callback, void *callback_context,
                            uint32_t *shown_out)
{
    size_t filter_length = filter != NULL ? sizeof *filter : 0u;
    size_t shown_length = shown_out != NULL ? sizeof *shown_out : 0u;
    YanNoteResult guard =
        service_guard(context, filter, filter_length, shown_out, shown_length);
    if (guard != YAN_NOTE_OK) {
        return guard;
    }
    if (ranges_overlap(filter, filter_length, shown_out, shown_length)) {
        return YAN_NOTE_INVALID;
    }
    if (callback == NULL) {
        return YAN_NOTE_INVALID;
    }
    /* The callback context is opaque: the service never dereferences it, but a
     * pointer into the service objects would let a callback corrupt state that
     * the outer call still reads, so it is rejected conservatively. */
    if (callback_context != NULL &&
        (ranges_overlap(callback_context, 1u, context, sizeof *context) ||
         ranges_overlap(callback_context, 1u, context->fs,
                        sizeof *context->fs))) {
        return YAN_NOTE_INVALID;
    }
    uint16_t filter_count = 0u;
    if (filter != NULL) {
        if (filter->count > YAN_NOTE_TAG_MAX_COUNT) {
            return YAN_NOTE_INVALID;
        }
        for (uint16_t i = 0u; i < filter->count; ++i) {
            if (!yan_note_tag_valid(filter->tag[i], filter->length[i])) {
                return YAN_NOTE_INVALID;
            }
        }
        filter_count = filter->count;
    }

    context->busy = true;
    /* Reserve the whole directory before any callback: one broken header
     * anywhere means no name is delivered and no count is published. */
    YanNoteResult result = load_snapshot(context);
    uint32_t eligible = 0u;
    uint32_t shown = 0u;
    for (uint32_t i = 0u; result == YAN_NOTE_OK && i < context->entry_count; ++i) {
        uint32_t size = 0u;
        result = read_head(context, context->entries[i].name, &size);
        if (result != YAN_NOTE_OK) {
            break;
        }
        bool qualified = true;
        for (uint16_t t = 0u; t < filter_count; ++t) {
            if (!tag_present(&context->current, filter->tag[t],
                             filter->length[t])) {
                qualified = false;
                break;
            }
        }
        if (!qualified) {
            continue;
        }
        if (eligible >= context->capacity) {
            result = YAN_NOTE_PROTOCOL;
            break;
        }
        memcpy(context->sort_name[eligible], context->entries[i].name, 32u);
        eligible += 1u;
    }
    if (result == YAN_NOTE_OK) {
        /* ASCII byte order, exactly the order the callback observes. */
        for (uint32_t i = 1u; i < eligible; ++i) {
            char key[32];
            memcpy(key, context->sort_name[i], 32u);
            uint32_t j = i;
            while (j > 0u && name_compare(context->sort_name[j - 1u], key) > 0) {
                memcpy(context->sort_name[j], context->sort_name[j - 1u], 32u);
                --j;
            }
            memcpy(context->sort_name[j], key, 32u);
        }
        for (uint32_t i = 0u; i < eligible; ++i) {
            if (!callback(callback_context, context->sort_name[i])) {
                result = YAN_NOTE_STOPPED;
                break;
            }
            shown += 1u;
        }
    }
    if (result == YAN_NOTE_OK && shown_out != NULL) {
        *shown_out = shown;
    }
    context->busy = false;
    return result;
}
