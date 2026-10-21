/* No-index linear term backend for 0026.
 *
 * The contract is os/search_terms_linear.h. This file is the filesystem edge:
 * it walks the single-root directory with yan_fs_list, reads each file through
 * yan_fs_read, and lets os/search_terms_core.c run the shared parse, streaming
 * scan and snippet window. It owns the shared folded/prefix/scan buffers so the
 * 4 KiB task stack is never involved.
 *
 * The status mapping is deliberately narrow: a known uninitialized borrowed
 * filesystem is the only INVALID that becomes UNINITIALIZED, a filesystem BUSY
 * propagates as TERMS_BUSY with the caller's status holder untouched, every
 * other filesystem result maps one-to-one, and a mounted but uncached source is
 * EMPTY while an unusable source is UNAVAILABLE. */
#include "search_terms_linear.h"

#include "memory.h"

#include <stddef.h>

/* The same pure range rule the facade uses; duplicated here so the backend
 * needs no shared private header (the linear backend is the only place a
 * filesystem span exists). */
static bool linear_ranges_overlap(const void *first, size_t first_length,
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

static YanSearchTermsResult linear_map_fs(YanFsResult result)
{
    switch (result) {
    case YAN_FS_OK:
        return YAN_SEARCH_TERMS_OK;
    case YAN_FS_END:
        return YAN_SEARCH_TERMS_OK;
    case YAN_FS_INVALID:
        return YAN_SEARCH_TERMS_INVALID;
    case YAN_FS_BUSY:
        return YAN_SEARCH_TERMS_BUSY;
    case YAN_FS_NOT_MOUNTED:
        return YAN_SEARCH_TERMS_NOT_MOUNTED;
    case YAN_FS_FAULTED:
        return YAN_SEARCH_TERMS_FAULTED;
    case YAN_FS_NOT_FOUND:
        return YAN_SEARCH_TERMS_NOT_FOUND;
    case YAN_FS_IO:
        return YAN_SEARCH_TERMS_IO;
    case YAN_FS_PROTOCOL:
        return YAN_SEARCH_TERMS_PROTOCOL;
    case YAN_FS_CORRUPT:
        return YAN_SEARCH_TERMS_CORRUPT;
    case YAN_FS_DIRECTORY_FULL:
        return YAN_SEARCH_TERMS_INVALID; /* not a searchable condition */
    case YAN_FS_UNSUPPORTED:
        return YAN_SEARCH_TERMS_UNSUPPORTED;
    default:
        return YAN_SEARCH_TERMS_PROTOCOL;
    }
}

typedef struct {
    YanFs *fs;
    const char *name;
    /* 0036-R8: for a file that fits in the context's header window, the one
     * medium read that fills this borrowed prefix serves both the whole-file
     * preflight and the scan (and the snippet when it is still the same file).
     * The pointer and name are valid only while `name` is unchanged; every new
     * file either replaces them through linear_reader_prefetch or clears them
     * before a snippet read. */
    const uint8_t *prefix;
    uint32_t prefix_length;
    char prefix_name[YAN_FS_NAME_MAX + 1u];
} LinearReader;

static void linear_reader_init(LinearReader *reader, YanFs *fs, const char *name)
{
    reader->fs = fs;
    reader->name = name;
    reader->prefix = NULL;
    reader->prefix_length = 0u;
    reader->prefix_name[0] = '\0';
}

static void linear_reader_forget_prefix(LinearReader *reader)
{
    reader->prefix = NULL;
    reader->prefix_length = 0u;
    reader->prefix_name[0] = '\0';
}

static bool linear_reader_prefix_is(const LinearReader *reader,
                                    const char *name)
{
    if (reader->prefix == NULL || name == NULL) {
        return false;
    }
    uint32_t at = 0u;
    while (reader->prefix_name[at] != '\0' && name[at] != '\0' &&
           reader->prefix_name[at] == name[at]) {
        ++at;
    }
    return reader->prefix_name[at] == '\0' && name[at] == '\0';
}

/* Reads a file that fits the context's header window into `prefix` exactly
 * once. A larger file leaves the prefix empty, so the shared streaming core
 * keeps its bounded two-read preflight/scan path. The caller must not have a
 * live prefix in this reader before the call. */
static YanSearchTermsResult linear_reader_prefetch(LinearReader *reader,
                                                   uint8_t *prefix,
                                                   uint32_t capacity,
                                                   uint32_t file_size)
{
    linear_reader_forget_prefix(reader);
    if (file_size == 0u || file_size > capacity) {
        return YAN_SEARCH_TERMS_OK;
    }
    uint32_t got = 0u;
    YanFsResult result =
        yan_fs_read(reader->fs, reader->name, 0u, prefix, file_size, &got);
    if (result != YAN_FS_OK) {
        return linear_map_fs(result);
    }
    if (got != file_size) {
        return YAN_SEARCH_TERMS_PROTOCOL;
    }
    /* The directory contract caps a name at 31 bytes, but a listing copied
     * from a malformed block may not be NUL-terminated. Bound the scan before
     * trusting the name, so this never reads past the 32-byte field. */
    uint32_t name_length = 0u;
    while (name_length <= YAN_FS_NAME_MAX &&
           reader->name[name_length] != '\0') {
        ++name_length;
    }
    if (name_length > YAN_FS_NAME_MAX) {
        return YAN_SEARCH_TERMS_PROTOCOL;
    }
    memcpy(reader->prefix_name, reader->name, name_length + 1u);
    reader->prefix = prefix;
    reader->prefix_length = file_size;
    return YAN_SEARCH_TERMS_OK;
}

static YanSearchTermsResult linear_read(void *context, uint32_t offset,
                                        uint32_t capacity, uint8_t *out,
                                        uint32_t *length)
{
    LinearReader *reader = (LinearReader *)context;
    /* The core bounds every request by file_size, and the prefix only covers
     * whole files that fit it, so a fully covered request is exactly the bytes
     * that would have come from the medium. A partially covered request falls
     * through to yan_fs_read instead of mixing the two sources. */
    if (reader->prefix != NULL && offset <= reader->prefix_length &&
        capacity <= reader->prefix_length - offset) {
        memcpy(out, reader->prefix + offset, capacity);
        *length = capacity;
        return YAN_SEARCH_TERMS_OK;
    }
    uint32_t got = 0u;
    YanFsResult result =
        yan_fs_read(reader->fs, reader->name, offset, out, capacity, &got);
    *length = got;
    return linear_map_fs(result);
}

uint32_t yan_search_terms_linear_storage_bytes(uint32_t capacity)
{
    return capacity;
}

YanSearchTermsResult yan_search_terms_linear_init(YanSearchTermsLinear *linear,
                                                  YanFs *fs,
                                                  uint8_t *metadata_eligible)
{
    if (linear == NULL || fs == NULL || metadata_eligible == NULL) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    if (linear_ranges_overlap(linear, sizeof *linear, fs, sizeof *fs)) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    if (linear->initialized && linear->busy) {
        return YAN_SEARCH_TERMS_BUSY;
    }
    linear->fs = fs;
    linear->metadata_eligible = metadata_eligible;
    /* An uninitialized source is a legal status input (UNAVAILABLE); it has no
     * directory capacity yet, so the flag array is simply never indexed. */
    linear->capacity = fs->initialized ? fs->entry_capacity : 0u;
    linear->initialized = true;
    linear->busy = false;
    linear->snippet = NULL;
    linear->snippet_length = 0u;
    linear->metadata_skipped = 0u;
    linear->metadata_filtered = false;
    return YAN_SEARCH_TERMS_OK;
}

/* Reads the bounded header prefix of one file and reports its qualification.
 * The whole body is preflighted first, so a binary/NUL file never reaches the
 * parser: `valid` false means INVALID (skip once). A valid file is read for at
 * most YAN_NOTE_HEADER_MAX bytes and parsed; a matched prefix with a broken
 * header is METADATA and stops the whole query. */
static YanSearchTermsResult linear_qualify_file(YanSearchTermsLinear *linear,
                                                const char *name,
                                                uint32_t file_size,
                                                bool *valid)
{
    LinearReader reader;
    linear_reader_init(&reader, linear->fs, name);
    YanSearchTermsResult result = linear_reader_prefetch(
        &reader, linear->metadata_prefix, YAN_NOTE_HEADER_MAX, file_size);
    if (result != YAN_SEARCH_TERMS_OK) {
        return result;
    }
    bool skip = false;
    result = yan_search_terms_core_preflight(
        linear_read, &reader, linear->scratch.chunk, YAN_SEARCH_TERMS_CHUNK,
        file_size, &skip);
    if (result != YAN_SEARCH_TERMS_OK) {
        return result;
    }
    if (skip) {
        *valid = false;
        return YAN_SEARCH_TERMS_OK;
    }
    *valid = true;
    uint32_t want = file_size < YAN_NOTE_HEADER_MAX ? file_size
                                                    : YAN_NOTE_HEADER_MAX;
    uint32_t got = want;
    if (reader.prefix_length < want) {
        got = 0u;
        YanFsResult read_result =
            yan_fs_read(linear->fs, name, 0u, linear->metadata_prefix, want,
                        &got);
        if (read_result != YAN_FS_OK) {
            return linear_map_fs(read_result);
        }
    }
    if (got != want) {
        return YAN_SEARCH_TERMS_PROTOCOL;
    }
    YanNoteResult parsed = yan_note_parse_header(linear->metadata_prefix, want,
                                                &linear->metadata_info,
                                                &linear->metadata_workspace);
    if (parsed == YAN_NOTE_OK) return YAN_SEARCH_TERMS_OK;
    if (parsed == YAN_NOTE_METADATA) return YAN_SEARCH_TERMS_METADATA;
    return YAN_SEARCH_TERMS_PROTOCOL;
}

static bool linear_tag_present(const YanNoteInfo *info, const uint8_t *value,
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

/* Defensive filter validation for a direct backend call: the facade has already
 * checked the caller's span and aliases, but an exported entry point must not
 * trust that. It rejects an absent, out-of-range or malformed selection. */
bool yan_search_terms_linear_filter_valid(const YanSearchTermsLinear *linear,
                                          const YanNoteTagFilter *filter)
{
    if (linear == NULL || filter == NULL ||
        (uint64_t)sizeof *linear > UINTPTR_MAX - (uintptr_t)linear) {
        return false;
    }
    if (linear_ranges_overlap(filter, sizeof *filter, linear, sizeof *linear) ||
        linear_ranges_overlap(filter, sizeof *filter, linear->fs,
                              sizeof *linear->fs)) {
        return false;
    }
    if (filter->count > YAN_NOTE_TAG_MAX_COUNT) return false;
    for (uint16_t i = 0u; i < filter->count; ++i) {
        if (filter->length[i] == 0u || filter->length[i] > YAN_NOTE_TAG_MAX ||
            !yan_note_tag_valid(filter->tag[i], filter->length[i])) {
            return false;
        }
    }
    return true;
}

YanSearchTermsResult yan_search_terms_linear_qualify(
    YanSearchTermsLinear *linear, const YanNoteTagFilter *filter)
{
    if (linear == NULL ||
        (uint64_t)sizeof *linear > UINTPTR_MAX - (uintptr_t)linear ||
        !linear->initialized) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    if (!yan_search_terms_linear_filter_valid(linear, filter)) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    for (uint32_t i = 0u; i < linear->capacity; ++i) {
        linear->metadata_eligible[i] = YAN_NOTE_META_TAG;
    }
    linear->metadata_skipped = 0u;

    uint32_t cursor = 0u;
    for (;;) {
        YanFsInfo info;
        YanFsResult listed = yan_fs_list(linear->fs, &cursor, &info);
        if (listed == YAN_FS_END) {
            break;
        }
        if (listed != YAN_FS_OK) {
            return linear_map_fs(listed);
        }
        /* The cursor is the next physical slot, so the entry just read is at
         * cursor - 1; the eligibility table is indexed by that same physical
         * slot, never by the entry's ordinal position. */
        uint32_t slot = cursor - 1u;
        if (slot >= linear->capacity) {
            return YAN_SEARCH_TERMS_PROTOCOL;
        }
        bool valid = true;
        YanSearchTermsResult result =
            linear_qualify_file(linear, info.name, info.size_bytes, &valid);
        if (result == YAN_SEARCH_TERMS_METADATA) {
            return result; /* a broken header is fatal for the whole query */
        }
        if (result != YAN_SEARCH_TERMS_OK) {
            return result;
        }
        if (!valid) {
            linear->metadata_eligible[slot] = YAN_NOTE_META_INVALID;
            ++linear->metadata_skipped;
            continue;
        }
        bool qualified = true;
        for (uint16_t t = 0u; t < filter->count; ++t) {
            if (!linear_tag_present(&linear->metadata_info, filter->tag[t],
                                    filter->length[t])) {
                qualified = false;
                break;
            }
        }
        if (qualified) {
            linear->metadata_eligible[slot] = YAN_NOTE_META_ELIGIBLE;
        } else {
            linear->metadata_eligible[slot] = YAN_NOTE_META_TAG;
        }
    }
    return YAN_SEARCH_TERMS_OK;
}

static YanSearchTermsResult linear_query_locked(
    YanSearchTermsLinear *linear, const uint8_t *query, uint32_t query_length,
    YanSearchTermsMatchFn match, void *match_context,
    YanSearchTermsSummary *summary)
{
    YanSearchTermsQuery parsed;
    YanSearchTermsResult result =
        yan_search_terms_parse(query, query_length, &parsed);
    if (result != YAN_SEARCH_TERMS_OK) {
        return result;
    }
    YanSearchTermsPattern pattern;
    result = yan_search_terms_pattern_build(
        query, &parsed, linear->folded, YAN_SEARCH_TERMS_QUERY_MAX,
        linear->prefix, YAN_SEARCH_TERMS_QUERY_MAX, &pattern);
    if (result != YAN_SEARCH_TERMS_OK) {
        return result;
    }

    YanSearchTermsRank *rank = &linear->rank;
    yan_search_terms_rank_init(rank);

    LinearReader reader;
    linear_reader_init(&reader, linear->fs, NULL);
    uint32_t cursor = 0u;
    for (;;) {
        YanFsInfo info;
        YanFsResult listed = yan_fs_list(linear->fs, &cursor, &info);
        if (listed == YAN_FS_END) {
            break;
        }
        if (listed != YAN_FS_OK) {
            return linear_map_fs(listed);
        }
        uint32_t slot = cursor - 1u;
        /* 0027 filtered mode: consult the precomputed eligibility set before
         * the file is read. A binary/NUL slot was already counted once by the
         * qualification pass; a valid file without the tags is skipped without
         * touching the skip count. A plain (unfiltered) query never consults
         * the table, so its warm path and read bound are unchanged. */
        if (linear->metadata_filtered) {
            if (slot >= linear->capacity) {
                return YAN_SEARCH_TERMS_PROTOCOL;
            }
            if (linear->metadata_eligible[slot] != YAN_NOTE_META_ELIGIBLE) {
                if (linear->metadata_eligible[slot] == YAN_NOTE_META_INVALID) {
                    ++rank->skipped;
                }
                continue;
            }
        }
        reader.name = info.name;
        result = linear_reader_prefetch(&reader, linear->metadata_prefix,
                                        YAN_NOTE_HEADER_MAX,
                                        info.size_bytes);
        if (result != YAN_SEARCH_TERMS_OK) {
            return result;
        }
        result = yan_search_terms_core_scan_file(
            info.name, slot, info.size_bytes, linear_read, &reader, &pattern,
            rank, &linear->scratch);
        if (result != YAN_SEARCH_TERMS_OK) {
            return result;
        }
    }

    uint32_t shown = rank->count < YAN_SEARCH_TERMS_SHOWN_MAX
                         ? rank->count
                         : YAN_SEARCH_TERMS_SHOWN_MAX;
    for (uint32_t i = 0u; i < shown; ++i) {
        YanSearchTermsCandidate *candidate = &rank->candidate[i];
        /* A snippet is only allowed to reuse the prefix of the file it names.
         * The scan normally leaves the last file cached; a candidate from any
         * other file drops that prefix before the read. */
        if (!linear_reader_prefix_is(&reader, candidate->name)) {
            linear_reader_forget_prefix(&reader);
        }
        reader.name = candidate->name;
        const uint8_t *body = NULL;
        uint32_t body_length = 0u;
        bool left = false;
        bool right = false;
        result = yan_search_terms_core_snippet(
            candidate->file_size, candidate->anchor, linear_read, &reader,
            &body, &body_length, &left, &right, linear->scratch.snippet,
            YAN_SEARCH_TERMS_SNIPPET_SCRATCH);
        if (result != YAN_SEARCH_TERMS_OK) {
            return result;
        }
        linear->snippet = body;
        linear->snippet_length = body_length;
        YanSearchTermsMatch value;
        value.name = candidate->name;
        value.line_number = candidate->line_number;
        value.score = candidate->score;
        value.snippet_length = body_length;
        value.left_truncated = left;
        value.right_truncated = right;
        if (!match(match_context, &value)) {
            /* A clean callback stop: no summary is published and the facade
             * maps the empty result to STOPPED. */
            return YAN_SEARCH_TERMS_OK;
        }
    }
    if (summary != NULL) {
        summary->total = rank->total;
        summary->shown = shown;
        summary->skipped = rank->skipped;
        summary->mode = YAN_SEARCH_MODE_SCAN;
    }
    return YAN_SEARCH_TERMS_OK;
}

YanSearchTermsResult yan_search_terms_linear_query(
    void *context, const uint8_t *query, uint32_t query_length,
    YanSearchTermsMatchFn match, void *match_context,
    YanSearchTermsSummary *summary)
{
    YanSearchTermsLinear *linear = (YanSearchTermsLinear *)context;
    if (linear == NULL || !linear->initialized) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    if (match == NULL) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    if (linear->busy) {
        return YAN_SEARCH_TERMS_BUSY;
    }
    linear->busy = true;
    linear->snippet = NULL;
    linear->snippet_length = 0u;
    YanSearchTermsResult result = linear_query_locked(
        linear, query, query_length, match, match_context, summary);
    linear->busy = false;
    return result;
}

YanSearchTermsResult yan_search_terms_linear_query_filtered(
    void *context, const uint8_t *query, uint32_t query_length,
    const YanNoteTagFilter *filter, YanSearchTermsMatchFn match,
    void *match_context, YanSearchTermsSummary *summary)
{
    YanSearchTermsLinear *linear = (YanSearchTermsLinear *)context;
    if (linear == NULL ||
        (uint64_t)sizeof *linear > UINTPTR_MAX - (uintptr_t)linear ||
        !linear->initialized) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    if (match == NULL) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    if (linear->busy) {
        return YAN_SEARCH_TERMS_BUSY;
    }
    if (!yan_search_terms_linear_filter_valid(linear, filter)) {
        return YAN_SEARCH_TERMS_INVALID;
    }

    /* The whole qualification runs first: every invalid binary is skipped once,
     * every valid header is parsed, and one broken header fails the query
     * before any ranking or callback happens. The mode flag is armed only
     * around the scan and is cleared on every exit so a later plain query takes
     * the original warm path. */
    linear->busy = true;
    linear->snippet = NULL;
    linear->snippet_length = 0u;
    linear->metadata_filtered = true;
    YanSearchTermsResult result =
        yan_search_terms_linear_qualify(linear, filter);
    if (result == YAN_SEARCH_TERMS_OK) {
        result = linear_query_locked(linear, query, query_length, match,
                                     match_context, summary);
    }
    linear->metadata_filtered = false;
    linear->busy = false;
    return result;
}

YanSearchTermsResult yan_search_terms_linear_read_match(
    void *context, uint32_t offset, uint32_t capacity,
    const uint8_t **bytes, uint32_t *length)
{
    YanSearchTermsLinear *linear = (YanSearchTermsLinear *)context;
    if (linear == NULL || !linear->initialized || bytes == NULL ||
        length == NULL) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    if (offset > linear->snippet_length) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    uint32_t remaining = linear->snippet_length - offset;
    uint32_t got = remaining < capacity ? remaining : capacity;
    *bytes = (got > 0u && linear->snippet != NULL)
                 ? linear->snippet + offset
                 : NULL;
    *length = got;
    return YAN_SEARCH_TERMS_OK;
}

YanSearchTermsResult yan_search_terms_linear_status(
    void *context, YanSearchTermsStatus *out)
{
    YanSearchTermsLinear *linear = (YanSearchTermsLinear *)context;
    if (linear == NULL || !linear->initialized || out == NULL) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    YanFsSource source;
    source.state = YAN_FS_UNMOUNTED;
    source.token = 0u;
    source.cacheable = false;
    YanFsResult observed = yan_fs_source(linear->fs, &source);
    if (observed == YAN_FS_OK) {
        out->terms = 0u;
        out->postings = 0u;
        switch (source.state) {
        case YAN_FS_MOUNTED:
            out->state = YAN_SEARCH_INDEX_EMPTY;
            out->source = YAN_SEARCH_SOURCE_MOUNTED;
            return YAN_SEARCH_TERMS_OK;
        case YAN_FS_UNMOUNTED:
            out->state = YAN_SEARCH_INDEX_UNAVAILABLE;
            out->source = YAN_SEARCH_SOURCE_UNMOUNTED;
            return YAN_SEARCH_TERMS_OK;
        case YAN_FS_STATE_FAULTED:
            out->state = YAN_SEARCH_INDEX_UNAVAILABLE;
            out->source = YAN_SEARCH_SOURCE_FAULTED;
            return YAN_SEARCH_TERMS_OK;
        default:
            out->state = YAN_SEARCH_INDEX_UNAVAILABLE;
            out->source = YAN_SEARCH_SOURCE_UNINITIALIZED;
            return YAN_SEARCH_TERMS_OK;
        }
    }
    if (observed == YAN_FS_INVALID) {
        /* The backend's own known holder is uninitialized: the only INVALID
         * that becomes an observation rather than an error. */
        out->state = YAN_SEARCH_INDEX_UNAVAILABLE;
        out->source = YAN_SEARCH_SOURCE_UNINITIALIZED;
        out->terms = 0u;
        out->postings = 0u;
        return YAN_SEARCH_TERMS_OK;
    }
    if (observed == YAN_FS_BUSY) {
        return YAN_SEARCH_TERMS_BUSY; /* out is left untouched */
    }
    return linear_map_fs(observed);
}

YanSearchTermsResult yan_search_terms_linear_clear(void *context)
{
    YanSearchTermsLinear *linear = (YanSearchTermsLinear *)context;
    if (linear == NULL || !linear->initialized) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    /* There is nothing to drop; discarding the empty cache is a successful
     * no-op, not a fabricated rebuild, and it never revives the source. */
    return YAN_SEARCH_TERMS_OK;
}

YanSearchTermsResult yan_search_terms_linear_rebuild(void *context)
{
    YanSearchTermsLinear *linear = (YanSearchTermsLinear *)context;
    if (linear == NULL || !linear->initialized) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    /* The linear backend owns no index, so it cannot rebuild one. The later
     * indexed backend registers its own management entry points. */
    return YAN_SEARCH_TERMS_UNSUPPORTED;
}

YanSearchTermsBackend yan_search_terms_linear_backend(
    YanSearchTermsLinear *linear)
{
    YanSearchTermsBackend backend;
    backend.context = linear;
    backend.context_size = sizeof(YanSearchTermsLinear);
    backend.source_context = (linear != NULL) ? (void *)linear->fs : NULL;
    backend.source_size =
        (linear != NULL && linear->fs != NULL) ? sizeof(YanFs) : 0u;
    backend.query = yan_search_terms_linear_query;
    backend.read_match = yan_search_terms_linear_read_match;
    backend.status = yan_search_terms_linear_status;
    backend.rebuild = yan_search_terms_linear_rebuild;
    backend.clear = yan_search_terms_linear_clear;
    backend.query_filtered = yan_search_terms_linear_query_filtered;
    return backend;
}
