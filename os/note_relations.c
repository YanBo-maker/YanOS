/* 0028 knowledge-relations service.
 *
 * The behavioural contract is os/note_relations.h and
 * docs/specs/0028-knowledge-relations.md, "公共服务与接口责任" and "输入、预检与
 * 可见状态". This translation unit owns the independent, read-only relation
 * service. It borrows a caller-owned YanFs, reuses the pure
 * os/note_metadata_format.h parser and adds no disk format:
 *
 *   - links: one bounded header read of an existing source, then one directory
 *     existence lookup per declared link, in declaration order. The whole
 *     source header is validated and every target existence is observed before
 *     the first row, so no filesystem call happens once callbacks begin. A
 *     target's own broken header is irrelevant to this command.
 *   - backlinks: every existing file's bounded head is read and validated, and
 *     each declaring file is collected, before the first row. Rows are then
 *     emitted in ASCII filename order; the cited name need not exist and its
 *     own self-declaration is kept.
 *   - related: the source head is validated first (a missing source is
 *     NOT_FOUND before any remote head), then every other existing file's head
 *     is validated and its outgoing/incoming/shared-tag explanation computed
 *     before any row. Candidates are ranked (out+in) desc, shared_tags desc,
 *     filename ASCII asc and capped at 20 rows while the summary keeps the full
 *     total, so every current header including a non-candidate or a slot past
 *     the 20th row is parsed before callbacks begin.
 *
 * Layering: this file includes the pure format and os/yanfs.h only. It performs
 * no Host I/O, no allocation, and keeps every large object in the caller-owned
 * YanRelationContext. The interpreter frame is a handful of scalars plus a
 * 32-byte name copy, so no 4 KiB prefix or ~2 KiB parser workspace is automatic.
 * The shared guard chain runs before any device byte, the busy flag is held
 * across the whole synchronous call (including callbacks) and released on every
 * return path, and every query is zero-write. A real filesystem I/O or protocol
 * error propagates through the exact result mapping and leaves the borrowed
 * filesystem in its own sticky fault state for the next call. */

#include "note_relations.h"

#include "sort.h"

#include "memory.h"

/* ------------------------------------------------------------------ guards */

/* A span that would leave uintptr_t is reported as overlapping. Nothing is read
 * or written through either range. Zero-length spans are absent and never
 * overlap. The same pure range rule the other facade layers use. */
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

/* Borrowed-source state, in the order 0021 fixes: not initialized first, then
 * BUSY, then the fault record, then the mount state. Reading the state field is
 * only safe once `initialized` is known true, so the caller passes that value
 * in and no op reads a raw state field off an uninitialized object. */
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

/* Reads one caller name incrementally: every byte's address is checked against
 * the context and the filesystem before it is read, and the scan stops after 32
 * bytes so a name without a terminator cannot walk out of its object. A legal
 * 1..31 byte YanFS name is accepted; an empty name or one with no terminator
 * inside the 32-byte cap is rejected. */
static bool read_name(const YanRelationContext *context, const char *name,
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

/* The shared entry guard for all three queries. The order is the one 0028
 * fixes: the whole context extent (so a wrapping pointer never reaches a bool
 * load), then initialized, then BUSY, then the borrowed filesystem span and
 * state, then the other parameters, their spans, alias protection against the
 * context/borrowed filesystem and finally the full per-byte name. Nothing is
 * written and no device byte is touched before this returns OK. */
static YanNoteResult relation_guard(YanRelationContext *context, const char *name,
                                    YanRelationRowFn sink,
                                    const void *sink_context,
                                    YanRelationSummary *summary)
{
    if (!span_valid(context, sizeof *context)) {
        return YAN_NOTE_INVALID;
    }
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
    if (sink == NULL) {
        return YAN_NOTE_INVALID;
    }
    size_t summary_length = summary != NULL ? sizeof *summary : 0u;
    if (!span_valid(name, 1u) || !span_valid(summary, summary_length)) {
        return YAN_NOTE_INVALID;
    }
    if (ranges_overlap(name, 1u, summary, summary_length) ||
        ranges_overlap(name, 1u, context, sizeof *context) ||
        ranges_overlap(name, 1u, fs, sizeof *fs) ||
        ranges_overlap(summary, summary_length, context, sizeof *context) ||
        ranges_overlap(summary, summary_length, fs, sizeof *fs)) {
        return YAN_NOTE_INVALID;
    }
    /* The callback context is opaque: the service never dereferences it, but a
     * pointer into the service objects would let a callback corrupt state the
     * outer call still reads, so it is rejected conservatively. */
    if (sink_context != NULL &&
        (ranges_overlap(sink_context, 1u, context, sizeof *context) ||
         ranges_overlap(sink_context, 1u, fs, sizeof *fs))) {
        return YAN_NOTE_INVALID;
    }
    char name_copy[32];
    if (!read_name(context, name, name_copy)) {
        return YAN_NOTE_INVALID;
    }
    uint32_t name_length = 0u;
    while (name_copy[name_length] != '\0') {
        name_length += 1u;
    }
    if (ranges_overlap(name, (size_t)name_length + 1u, summary, summary_length)) {
        return YAN_NOTE_INVALID;
    }
    return YAN_NOTE_OK;
}

/* --------------------------------------------------------------- mapping */

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

/* ------------------------------------------------------- bounded prefix I/O */

/* Reads at most one bounded prefix of the named file into context->prefix,
 * parses the whole header and publishes into *out only on OK. The request is
 * clamped to YAN_RELATION_PREFIX_BYTES, so a multi-MiB binary body costs one
 * block read and is never preflighted. A short read of the size the directory
 * reported is a broken source and maps to PROTOCOL; a missing file maps to
 * NOT_FOUND; every other filesystem result is propagated exactly. */
static YanNoteResult read_head(YanRelationContext *context, const char *name,
                               YanNoteInfo *out)
{
    YanFsInfo info;
    YanFsResult stat_result = yan_fs_stat(context->fs, name, &info);
    if (stat_result != YAN_FS_OK) {
        return note_map_fs(stat_result);
    }
    uint32_t want = info.size_bytes < YAN_RELATION_PREFIX_BYTES
                        ? info.size_bytes
                        : YAN_RELATION_PREFIX_BYTES;
    uint32_t got = 0u;
    YanFsResult read_result = yan_fs_read(context->fs, name, 0u,
                                          context->prefix, want, &got);
    if (read_result != YAN_FS_OK) {
        return note_map_fs(read_result);
    }
    if (got != want) {
        return YAN_NOTE_PROTOCOL; /* a short read is a broken source */
    }
    return yan_note_parse_header(context->prefix, want, out,
                                 &context->parse_workspace);
}

/* --------------------------------------------------------- relation helpers */

/* ASCII byte order over two NUL-padded 32-byte names. */
static int name_compare(const char *left, const char *right)
{
    for (uint32_t i = 0u; i < 32u; ++i) {
        unsigned char a = (unsigned char)left[i], b = (unsigned char)right[i];
        if (a != b) return a < b ? -1 : 1;
        if (a == 0u) return 0;
    }
    return 0;
}

/* True when `target` is one of info's stored link names, exact ASCII bytes and
 * case, no folding and no directory lookup. */
static bool link_matches(const YanNoteInfo *info, const char *target)
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

/* Exact-byte common tag count, 0..16. Duplicate declarations do not inflate the
 * count and the intersection is computed over the source's distinct tags, so
 * the result is the set-intersection cardinality the interface promises. Empty
 * sets never match and no case folding or normalisation is applied. */
static uint8_t shared_tag_count(const YanNoteInfo *source,
                                const YanNoteInfo *candidate)
{
    uint8_t shared = 0u;
    for (uint16_t i = 0u; i < source->tag_count; ++i) {
        bool seen = false;
        for (uint16_t k = 0u; k < i; ++k) {
            if (source->tag_length[k] == source->tag_length[i] &&
                memcmp(source->tag[k], source->tag[i],
                       (size_t)source->tag_length[i]) == 0) {
                seen = true;
                break;
            }
        }
        if (seen) {
            continue;
        }
        for (uint16_t j = 0u; j < candidate->tag_count; ++j) {
            if (candidate->tag_length[j] == source->tag_length[i] &&
                memcmp(candidate->tag[j], source->tag[i],
                       (size_t)source->tag_length[i]) == 0) {
                shared = (uint8_t)(shared + 1u);
                break;
            }
        }
    }
    return shared;
}

/* True when left must be ranked after right: (out+in) desc, then shared tags
 * desc, then filename ASCII asc. All-zero rows (links) and name-only rows
 * (backlinks) collapse to the ASCII rule. */
static bool row_ranks_after(const YanRelationRow *left,
                            const YanRelationRow *right)
{
    uint32_t left_score = (uint32_t)left->outgoing + (uint32_t)left->incoming;
    uint32_t right_score = (uint32_t)right->outgoing + (uint32_t)right->incoming;
    if (left_score != right_score) {
        return left_score < right_score;
    }
    if (left->shared_tags != right->shared_tags) {
        return left->shared_tags < right->shared_tags;
    }
    return name_compare(left->name, right->name) > 0;
}

/* 0036-R8: the shared heapsort orders indices, so a 36-byte row is never moved
 * by the ranking pass. */
static bool relation_row_before(const void *context, uint16_t left,
                                uint16_t right)
{
    const YanRelationContext *relation = context;
    return row_ranks_after(&relation->rows[right], &relation->rows[left]);
}

static void sort_rows(YanRelationContext *context)
{
    for (uint32_t i = 0u; i < context->row_count; ++i) {
        context->sort_index[i] = (uint16_t)i;
    }
    yan_sort_index(context->sort_index, context->row_count,
                   relation_row_before, context);
}

/* -------------------------------------------------------------- public API */

uint32_t yan_relation_rows_storage_bytes(uint32_t capacity)
{
    return capacity * (uint32_t)sizeof(YanRelationRow);
}

uint32_t yan_relation_sort_index_storage_bytes(uint32_t capacity)
{
    return capacity * (uint32_t)sizeof(uint16_t);
}

YanNoteResult yan_relation_init(YanRelationContext *context, YanFs *fs,
                                YanRelationRow *rows, uint16_t *sort_index)
{
    if (context == NULL || fs == NULL || rows == NULL || sort_index == NULL) {
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
    /* 0036-R3: init runs before mount in the terminal, so UNMOUNTED is legal;
     * queries still go through relation_guard, which requires MOUNTED. */
    if (!fs->initialized) return YAN_NOTE_INVALID;
    if (fs->busy) return YAN_NOTE_BUSY;
    if (fs->state == YAN_FS_STATE_FAULTED) return YAN_NOTE_FAULTED;
    if (fs->entry_capacity == 0u) return YAN_NOTE_INVALID;
    uint32_t capacity = fs->entry_capacity;
    memset(context, 0, sizeof *context);
    context->fs = fs;
    context->rows = rows;
    context->sort_index = sort_index;
    context->row_capacity = capacity;
    context->initialized = true;
    context->busy = false;
    return YAN_NOTE_OK;
}

YanNoteResult yan_relation_links(YanRelationContext *context, const char *name,
                                 YanRelationRowFn sink, void *sink_context,
                                 YanRelationSummary *summary)
{
    YanNoteResult guard =
        relation_guard(context, name, sink, sink_context, summary);
    if (guard != YAN_NOTE_OK) {
        return guard;
    }
    char name_copy[32];
    if (!read_name(context, name, name_copy)) {
        return YAN_NOTE_INVALID;
    }

    context->busy = true;
    /* The source header is validated, and every declared target observed
     * through the directory, before the first row: no filesystem call happens
     * once callbacks begin. A target's own head is never parsed. */
    YanNoteResult result = read_head(context, name_copy, &context->source_info);
    if (result == YAN_NOTE_OK) {
        for (uint16_t i = 0u; i < context->source_info.link_count; ++i) {
            YanFsInfo target;
            YanFsResult looked_up = yan_fs_stat(
                context->fs, (const char *)context->source_info.link[i],
                &target);
            if (looked_up == YAN_FS_OK) {
                context->source_info.link_exists[i] = true;
            } else if (looked_up == YAN_FS_NOT_FOUND) {
                context->source_info.link_exists[i] = false; /* DANGLING */
            } else {
                result = note_map_fs(looked_up);
                break;
            }
        }
    }
    uint32_t shown = 0u;
    if (result == YAN_NOTE_OK) {
        for (uint16_t i = 0u; i < context->source_info.link_count; ++i) {
            YanRelationRow row;
            memset(&row, 0, sizeof row);
            uint32_t length = (uint32_t)strlen(
                (const char *)context->source_info.link[i]);
            memcpy(row.name, context->source_info.link[i], (size_t)length);
            row.name[length] = '\0';
            row.exists = context->source_info.link_exists[i];
            if (!sink(sink_context, &row)) {
                result = YAN_NOTE_STOPPED;
                break;
            }
            shown += 1u;
        }
    }
    if (result == YAN_NOTE_OK && summary != NULL) {
        summary->total = context->source_info.link_count;
        summary->shown = shown;
        summary->target_exists = true;
    }
    context->busy = false;
    return result;
}

YanNoteResult yan_relation_backlinks(YanRelationContext *context,
                                     const char *name, YanRelationRowFn sink,
                                     void *sink_context,
                                     YanRelationSummary *summary)
{
    YanNoteResult guard =
        relation_guard(context, name, sink, sink_context, summary);
    if (guard != YAN_NOTE_OK) {
        return guard;
    }
    char name_copy[32];
    if (!read_name(context, name, name_copy)) {
        return YAN_NOTE_INVALID;
    }

    context->busy = true;
    context->row_count = 0u;
    bool target_exists = false;
    /* The whole directory is prechecked: every existing file's bounded head is
     * parsed, collecting the declaring files and the cited-name existence,
     * before any callback. One broken header anywhere is METADATA with no
     * partial list. */
    YanNoteResult result = YAN_NOTE_OK;
    uint32_t cursor = 0u;
    YanFsInfo info;
    for (;;) {
        YanFsResult listed = yan_fs_list(context->fs, &cursor, &info);
        if (listed == YAN_FS_END) {
            break;
        }
        if (listed != YAN_FS_OK) {
            result = note_map_fs(listed);
            break;
        }
        if (name_compare(info.name, name_copy) == 0) {
            target_exists = true;
        }
        result = read_head(context, info.name, &context->candidate_info);
        if (result != YAN_NOTE_OK) {
            break;
        }
        if (link_matches(&context->candidate_info, name_copy)) {
            if (context->row_count >= context->row_capacity) {
                result = YAN_NOTE_PROTOCOL;
                break;
            }
            YanRelationRow *row = &context->rows[context->row_count];
            memset(row, 0, sizeof *row);
            memcpy(row->name, info.name, sizeof info.name);
            row->name[31] = '\0';
            row->exists = true;
            row->incoming = true;
            context->row_count += 1u;
        }
    }
    uint32_t shown = 0u;
    if (result == YAN_NOTE_OK) {
        sort_rows(context);
        const uint32_t limit = context->row_count < YAN_RELATION_BACKLINKS_SHOWN
                                   ? context->row_count : YAN_RELATION_BACKLINKS_SHOWN;
        for (uint32_t i = 0u; i < limit; ++i) {
            if (!sink(sink_context,
                      &context->rows[context->sort_index[i]])) {
                result = YAN_NOTE_STOPPED;
                break;
            }
            shown += 1u;
        }
    }
    if (result == YAN_NOTE_OK && summary != NULL) {
        summary->total = context->row_count;
        summary->shown = shown;
        summary->target_exists = target_exists;
    }
    context->busy = false;
    return result;
}

YanNoteResult yan_relation_related(YanRelationContext *context, const char *name,
                                   YanRelationRowFn sink, void *sink_context,
                                   YanRelationSummary *summary)
{
    YanNoteResult guard =
        relation_guard(context, name, sink, sink_context, summary);
    if (guard != YAN_NOTE_OK) {
        return guard;
    }
    char name_copy[32];
    if (!read_name(context, name, name_copy)) {
        return YAN_NOTE_INVALID;
    }

    context->busy = true;
    context->row_count = 0u;
    /* Source existence has priority: a missing source is NOT_FOUND before any
     * remote head can fail, and a broken source head is METADATA. */
    YanNoteResult result = read_head(context, name_copy, &context->source_info);
    if (result == YAN_NOTE_OK) {
        uint32_t cursor = 0u;
        YanFsInfo info;
        for (;;) {
            YanFsResult listed = yan_fs_list(context->fs, &cursor, &info);
            if (listed == YAN_FS_END) {
                break;
            }
            if (listed != YAN_FS_OK) {
                result = note_map_fs(listed);
                break;
            }
            if (name_compare(info.name, name_copy) == 0) {
                continue; /* the source is excluded and already validated */
            }
            result = read_head(context, info.name, &context->candidate_info);
            if (result != YAN_NOTE_OK) {
                break;
            }
            bool outgoing = link_matches(&context->source_info, info.name);
            bool incoming = link_matches(&context->candidate_info, name_copy);
            uint8_t shared = shared_tag_count(&context->source_info,
                                              &context->candidate_info);
            if (!outgoing && !incoming && shared == 0u) {
                continue; /* prechecked, but not a candidate */
            }
            if (context->row_count >= context->row_capacity) {
                result = YAN_NOTE_PROTOCOL;
                break;
            }
            YanRelationRow *row = &context->rows[context->row_count];
            memset(row, 0, sizeof *row);
            memcpy(row->name, info.name, sizeof info.name);
            row->name[31] = '\0';
            row->exists = true;
            row->outgoing = outgoing;
            row->incoming = incoming;
            row->shared_tags = shared;
            context->row_count += 1u;
        }
    }
    uint32_t shown = 0u;
    if (result == YAN_NOTE_OK) {
        sort_rows(context);
        uint32_t limit = context->row_count < YAN_RELATION_RELATED_SHOWN
                             ? context->row_count
                             : YAN_RELATION_RELATED_SHOWN;
        for (uint32_t i = 0u; i < limit; ++i) {
            if (!sink(sink_context,
                      &context->rows[context->sort_index[i]])) {
                result = YAN_NOTE_STOPPED;
                break;
            }
            shown += 1u;
        }
    }
    if (result == YAN_NOTE_OK && summary != NULL) {
        summary->total = context->row_count;
        summary->shown = shown;
        summary->target_exists = true;
    }
    context->busy = false;
    return result;
}
