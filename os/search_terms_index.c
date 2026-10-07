/* In-memory term index for 0026.
 *
 * The contract is os/search_terms_index.h and docs/specs/0026-term-search-
 * index.md. The object and its tables are caller-owned static storage; this
 * translation unit never allocates and never touches the medium except through
 * the borrowed YanFs reader.
 *
 * Build: clear the cache, snapshot the FS source identity, walk the directory
 * in physical slot order, whole-file UTF-8/NUL preflight each file (a bad file
 * is skipped once and not indexed), then tokenize the bytes into atoms. An
 * atom is a maximal ASCII word folded to lower case or one raw non-ASCII UTF-8
 * scalar. Each occurrence appends one posting to its key's linked list, in
 * physical slot then byte-offset order. A word longer than 255 bytes, more than
 * 8192 keys, more than 262144 key-pool bytes or more than 65536 postings fails
 * the whole build without publishing a partial index. After the walk the source
 * identity is observed again; only a complete, unchanged, MOUNTED and cacheable
 * identity is published READY.
 *
 * Query: observe the source; READY with a matching token uses the index, LIMIT
 * with a matching token or an uncacheable source falls back to a complete SCAN,
 * and an empty/stale/mismatched cache rebuilds first. The query is decomposed
 * into the same atoms. Complete-word keys make the ASCII boundaries implicit;
 * a group is matched by atom adjacency (prev_offset + prev_length ==
 * next_offset in the same slot/line). The shortest posting list is the anchor:
 * its postings enumerate candidate lines once each, and per-atom cursors count
 * every group's occurrences in linear time. Each group's per-line score is
 * capped at 255 (the stored key count is not), the earliest complete group
 * start is the snippet anchor, and only the best 20 lines keep metadata. The
 * selected snippet windows are re-read through the shared core snippet helper
 * and served by the embedded linear reader. */
#include "search_terms_index.h"

#include "search_text.h"
#include "memory.h"

#include <stddef.h>

#define INDEX_NO_KEY UINT32_MAX
#define INDEX_NO_POSTING UINT32_MAX

/* The same pure range rule the facade and the linear backend use; duplicated
 * so the index header pulls in no private helper. */
static bool index_ranges_overlap(const void *first, size_t first_length,
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

static YanSearchTermsResult index_map_fs(YanFsResult result)
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
    case YAN_FS_UNSUPPORTED:
        return YAN_SEARCH_TERMS_UNSUPPORTED;
    default:
        return YAN_SEARCH_TERMS_PROTOCOL;
    }
}

static YanSearchTermsSourceState index_source_class(YanFsState state)
{
    switch (state) {
    case YAN_FS_MOUNTED:
        return YAN_SEARCH_SOURCE_MOUNTED;
    case YAN_FS_UNMOUNTED:
        return YAN_SEARCH_SOURCE_UNMOUNTED;
    case YAN_FS_STATE_FAULTED:
        return YAN_SEARCH_SOURCE_FAULTED;
    default:
        return YAN_SEARCH_SOURCE_UNINITIALIZED;
    }
}

/* FNV-1a over the raw canonical key bytes. The test that forces a hash
 * collision uses the same published constants, never a production helper. */
static uint32_t index_hash_key(const uint8_t *key, uint32_t length)
{
    uint32_t hash = UINT32_C(2166136261);
    for (uint32_t i = 0u; i < length; ++i) {
        hash ^= key[i];
        hash *= UINT32_C(16777619);
    }
    return hash;
}

static void index_clear_cache(YanSearchTermsIndex *index)
{
    index->state = YAN_SEARCH_INDEX_EMPTY;
    index->token = 0u;
    index->token_valid = false;
    index->term_count = 0u;
    index->posting_count = 0u;
    index->key_pool_used = 0u;
    index->skipped = 0u;
}

/* Finds an existing key. Returns false when the atom is not in the index. */
static bool index_key_lookup(const YanSearchTermsIndex *index,
                             const uint8_t *key, uint32_t length,
                             uint32_t *out_key)
{
    uint32_t mask = YAN_SEARCH_TERMS_INDEX_HASH_SLOTS - 1u;
    uint32_t slot = index_hash_key(key, length) & mask;
    for (uint32_t probe = 0u; probe < YAN_SEARCH_TERMS_INDEX_HASH_SLOTS;
         ++probe) {
        uint32_t entry = index->hash[slot];
        if (entry == 0u) {
            return false;
        }
        uint32_t ki = entry - 1u;
        const YanSearchTermsIndexKey *k = &index->keys[ki];
        if (k->key_length == length &&
            memcmp(index->key_pool + k->key_offset, key, length) == 0) {
            *out_key = ki;
            return true;
        }
        slot = (slot + 1u) & mask;
    }
    return false;
}

/* Finds or creates a key. Capacity exhaustion is INDEX_LIMIT. */
static YanSearchTermsResult index_key_for(YanSearchTermsIndex *index,
                                          const uint8_t *key, uint32_t length,
                                          uint32_t *out_key)
{
    uint32_t mask = YAN_SEARCH_TERMS_INDEX_HASH_SLOTS - 1u;
    uint32_t slot = index_hash_key(key, length) & mask;
    for (uint32_t probe = 0u; probe < YAN_SEARCH_TERMS_INDEX_HASH_SLOTS;
         ++probe) {
        uint32_t entry = index->hash[slot];
        if (entry == 0u) {
            if (index->term_count >= YAN_SEARCH_TERMS_INDEX_KEYS) {
                return YAN_SEARCH_TERMS_INDEX_LIMIT;
            }
            if (index->key_pool_used + length >
                YAN_SEARCH_TERMS_INDEX_KEY_POOL) {
                return YAN_SEARCH_TERMS_INDEX_LIMIT;
            }
            uint32_t ki = index->term_count;
            YanSearchTermsIndexKey *k = &index->keys[ki];
            k->key_offset = index->key_pool_used;
            k->key_length = length;
            k->head = INDEX_NO_POSTING;
            k->tail = INDEX_NO_POSTING;
            k->count = 0u;
            memcpy(index->key_pool + index->key_pool_used, key, length);
            index->key_pool_used += length;
            index->hash[slot] = ki + 1u;
            ++index->term_count;
            *out_key = ki;
            return YAN_SEARCH_TERMS_OK;
        }
        uint32_t ki = entry - 1u;
        const YanSearchTermsIndexKey *k = &index->keys[ki];
        if (k->key_length == length &&
            memcmp(index->key_pool + k->key_offset, key, length) == 0) {
            *out_key = ki;
            return YAN_SEARCH_TERMS_OK;
        }
        slot = (slot + 1u) & mask;
    }
    return YAN_SEARCH_TERMS_INDEX_LIMIT;
}

static YanSearchTermsResult index_add_posting(YanSearchTermsIndex *index,
                                              uint32_t key, uint32_t slot,
                                              uint32_t line,
                                              uint32_t file_byte_offset)
{
    if (index->posting_count >= YAN_SEARCH_TERMS_INDEX_POSTINGS) {
        return YAN_SEARCH_TERMS_INDEX_LIMIT;
    }
    uint32_t p = index->posting_count;
    YanSearchTermsIndexPosting *posting = &index->postings[p];
    posting->next = INDEX_NO_POSTING;
    posting->slot = slot;
    posting->line = line;
    posting->file_byte_offset = file_byte_offset;
    YanSearchTermsIndexKey *k = &index->keys[key];
    if (k->tail == INDEX_NO_POSTING) {
        k->head = p;
    } else {
        index->postings[k->tail].next = p;
    }
    k->tail = p;
    ++k->count;
    ++index->posting_count;
    return YAN_SEARCH_TERMS_OK;
}

typedef struct {
    YanFs *fs;
    const char *name;
} IndexReader;

static YanSearchTermsResult index_read(void *context, uint32_t offset,
                                       uint32_t capacity, uint8_t *out,
                                       uint32_t *length)
{
    IndexReader *reader = (IndexReader *)context;
    uint32_t got = 0u;
    YanFsResult result =
        yan_fs_read(reader->fs, reader->name, offset, out, capacity, &got);
    *length = got;
    return index_map_fs(result);
}

/* Observes the source identity through the public FS getter. */
static YanSearchTermsResult index_observe(const YanSearchTermsIndex *index,
                                          YanFsSource *out)
{
    out->state = YAN_FS_UNMOUNTED;
    out->token = 0u;
    out->cacheable = false;
    YanFsResult result = yan_fs_source(index->fs, out);
    if (result == YAN_FS_OK) {
        return YAN_SEARCH_TERMS_OK;
    }
    if (result == YAN_FS_BUSY) {
        return YAN_SEARCH_TERMS_BUSY;
    }
    if (result == YAN_FS_INVALID) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    return index_map_fs(result);
}

static YanSearchTermsResult index_scan_file(YanSearchTermsIndex *index,
                                            uint32_t slot, const char *name,
                                            uint32_t file_size)
{
    IndexReader reader;
    reader.fs = index->fs;
    reader.name = name;
    uint8_t *chunk = index->linear.scratch.chunk;
    uint8_t word[YAN_SEARCH_TERMS_WORD_MAX + 1u];
    uint8_t scalar[4];
    uint32_t word_length = 0u;
    uint32_t word_start = 0u;
    uint32_t scalar_length = 0u;
    uint32_t scalar_start = 0u;
    uint32_t line = 1u;
    uint32_t offset = 0u;
    while (offset < file_size) {
        uint32_t remaining = file_size - offset;
        uint32_t request = remaining < YAN_SEARCH_TERMS_CHUNK
                               ? remaining
                               : YAN_SEARCH_TERMS_CHUNK;
        uint32_t got = 0u;
        YanSearchTermsResult result =
            index_read(&reader, offset, request, chunk, &got);
        if (result != YAN_SEARCH_TERMS_OK) {
            return result;
        }
        if (got > request || got == 0u) {
            return YAN_SEARCH_TERMS_PROTOCOL;
        }
        for (uint32_t i = 0u; i < got; ++i) {
            uint8_t byte = chunk[i];
            uint32_t position = offset + i;
            if (yan_search_text_is_ascii_word(byte)) {
                if (word_length == 0u) {
                    word_start = position;
                }
                if (word_length >= YAN_SEARCH_TERMS_WORD_MAX) {
                    return YAN_SEARCH_TERMS_INDEX_LIMIT; /* source word > 255 */
                }
                word[word_length++] = yan_search_text_ascii_fold(byte);
                continue;
            }
            if (word_length > 0u) {
                uint32_t key = 0u;
                result = index_key_for(index, word, word_length, &key);
                if (result != YAN_SEARCH_TERMS_OK) {
                    return result;
                }
                result = index_add_posting(index, key, slot, line, word_start);
                if (result != YAN_SEARCH_TERMS_OK) {
                    return result;
                }
                word_length = 0u;
            }
            if (byte >= 0x80u) {
                if (scalar_length == 0u) {
                    scalar_start = position;
                }
                scalar[scalar_length] = byte;
                ++scalar_length;
                uint32_t need =
                    yan_search_text_utf8_expected(scalar, scalar_length);
                if (need == 0u) {
                    return YAN_SEARCH_TERMS_PROTOCOL; /* preflight passed */
                }
                if (scalar_length >= need) {
                    uint32_t key = 0u;
                    result = index_key_for(index, scalar, need, &key);
                    if (result != YAN_SEARCH_TERMS_OK) {
                        return result;
                    }
                    result = index_add_posting(index, key, slot, line,
                                               scalar_start);
                    if (result != YAN_SEARCH_TERMS_OK) {
                        return result;
                    }
                    scalar_length = 0u;
                }
            } else if (byte == (uint8_t)'\n') {
                ++line;
            }
        }
        offset += got;
    }
    if (word_length > 0u) {
        uint32_t key = 0u;
        YanSearchTermsResult result =
            index_key_for(index, word, word_length, &key);
        if (result != YAN_SEARCH_TERMS_OK) {
            return result;
        }
        result = index_add_posting(index, key, slot, line, word_start);
        if (result != YAN_SEARCH_TERMS_OK) {
            return result;
        }
    }
    if (scalar_length != 0u) {
        return YAN_SEARCH_TERMS_PROTOCOL; /* preflight passed */
    }
    return YAN_SEARCH_TERMS_OK;
}

static YanSearchTermsResult index_build_locked(YanSearchTermsIndex *index)
{
    index_clear_cache(index);

    YanFsSource before;
    YanSearchTermsResult result = index_observe(index, &before);
    if (result != YAN_SEARCH_TERMS_OK) {
        if (result != YAN_SEARCH_TERMS_BUSY) {
            index->state = YAN_SEARCH_INDEX_UNAVAILABLE;
        }
        return result;
    }
    if (before.state != YAN_FS_MOUNTED) {
        index->state = YAN_SEARCH_INDEX_UNAVAILABLE;
        return before.state == YAN_FS_STATE_FAULTED
                   ? YAN_SEARCH_TERMS_FAULTED
                   : YAN_SEARCH_TERMS_NOT_MOUNTED;
    }
    if (!before.cacheable) {
        index->state = YAN_SEARCH_INDEX_UNCACHEABLE;
        return YAN_SEARCH_TERMS_OK;
    }

    memset(index->hash, 0, sizeof index->hash);

    uint32_t cursor = 0u;
    YanFsInfo info;
    YanFsResult listed;
    while ((listed = yan_fs_list(index->fs, &cursor, &info)) == YAN_FS_OK) {
        uint32_t slot = cursor - 1u;
        if (slot < YAN_FS_MAX_FILES) {
            index->entries[slot] = info;
        }
        IndexReader reader;
        reader.fs = index->fs;
        reader.name = info.name;
        bool skip = false;
        result = yan_search_terms_core_preflight(
            index_read, &reader, index->linear.scratch.chunk,
            YAN_SEARCH_TERMS_CHUNK, info.size_bytes, &skip);
        if (result != YAN_SEARCH_TERMS_OK) {
            index_clear_cache(index);
            return result;
        }
        if (skip) {
            ++index->skipped;
            continue;
        }
        result = index_scan_file(index, slot, info.name, info.size_bytes);
        if (result == YAN_SEARCH_TERMS_INDEX_LIMIT) {
            /* Remember the source so a same-identity query can scan without
             * rebuilding, but publish no counts and no partial index. */
            index->state = YAN_SEARCH_INDEX_LIMIT;
            index->token = before.token;
            index->token_valid = true;
            index->term_count = 0u;
            index->posting_count = 0u;
            index->key_pool_used = 0u;
            return YAN_SEARCH_TERMS_INDEX_LIMIT;
        }
        if (result != YAN_SEARCH_TERMS_OK) {
            index_clear_cache(index);
            return result;
        }
    }
    if (listed != YAN_FS_END) {
        index_clear_cache(index);
        return index_map_fs(listed);
    }

    YanFsSource after;
    result = index_observe(index, &after);
    if (result != YAN_SEARCH_TERMS_OK) {
        index_clear_cache(index);
        return result;
    }
    if (after.state != YAN_FS_MOUNTED || after.token != before.token ||
        !after.cacheable) {
        index_clear_cache(index);
        if (after.state != YAN_FS_MOUNTED) {
            index->state = YAN_SEARCH_INDEX_UNAVAILABLE;
        } else if (!after.cacheable) {
            index->state = YAN_SEARCH_INDEX_UNCACHEABLE;
        } else {
            index->state = YAN_SEARCH_INDEX_STALE;
        }
        return YAN_SEARCH_TERMS_OK;
    }

    index->state = YAN_SEARCH_INDEX_READY;
    index->token = before.token;
    index->token_valid = true;
    return YAN_SEARCH_TERMS_OK;
}

static bool index_posting_before_slot_line(const YanSearchTermsIndex *index,
                                           uint32_t posting, uint32_t slot,
                                           uint32_t line)
{
    const YanSearchTermsIndexPosting *p = &index->postings[posting];
    if (p->slot != slot) {
        return p->slot < slot;
    }
    return p->line < line;
}

static bool index_posting_same_slot_line(const YanSearchTermsIndex *index,
                                         uint32_t posting, uint32_t slot,
                                         uint32_t line)
{
    const YanSearchTermsIndexPosting *p = &index->postings[posting];
    return p->slot == slot && p->line == line;
}

static bool index_posting_before_offset(const YanSearchTermsIndex *index,
                                        uint32_t posting, uint32_t slot,
                                        uint32_t line, uint32_t offset)
{
    const YanSearchTermsIndexPosting *p = &index->postings[posting];
    if (p->slot != slot) {
        return p->slot < slot;
    }
    if (p->line != line) {
        return p->line < line;
    }
    return p->file_byte_offset < offset;
}

/* Counts every group in one candidate line. Each per-atom cursor only moves
 * forward, so the whole query visits each posting list linearly. A group's
 * first-atom occurrences drive the adjacency walk; a failed walk leaves the
 * inner cursors at the first posting not before the required offset, which is
 * exactly where the next (later-starting) occurrence resumes. */
static YanSearchTermsResult index_eval_line(YanSearchTermsIndex *index,
                                            const YanSearchTermsQuery *parsed,
                                            uint32_t slot, uint32_t line,
                                            YanSearchTermsRank *rank)
{
    uint32_t score = 0u;
    uint32_t anchor = UINT32_MAX;
    for (uint32_t g = 0u; g < parsed->count; ++g) {
        uint32_t base = index->group_atom_start[g];
        uint32_t atoms = index->group_atom_count[g];
        uint32_t count = 0u;
        uint32_t earliest = UINT32_MAX;
        uint32_t first = index->atom_cursor[base];
        while (first != INDEX_NO_POSTING &&
               index_posting_before_slot_line(index, first, slot, line)) {
            first = index->postings[first].next;
        }
        while (first != INDEX_NO_POSTING &&
               index_posting_same_slot_line(index, first, slot, line)) {
            uint32_t start = index->postings[first].file_byte_offset;
            uint32_t end =
                start + index->keys[index->atom_key[base]].key_length;
            bool complete = true;
            for (uint32_t i = 1u; i < atoms; ++i) {
                uint32_t ai = base + i;
                uint32_t c = index->atom_cursor[ai];
                while (c != INDEX_NO_POSTING &&
                       index_posting_before_offset(index, c, slot, line,
                                                   end)) {
                    c = index->postings[c].next;
                }
                index->atom_cursor[ai] = c;
                if (c == INDEX_NO_POSTING ||
                    !index_posting_same_slot_line(index, c, slot, line) ||
                    index->postings[c].file_byte_offset != end) {
                    complete = false;
                    break;
                }
                end += index->keys[index->atom_key[ai]].key_length;
            }
            if (complete) {
                if (count < 255u) {
                    ++count;
                }
                if (start < earliest) {
                    earliest = start;
                }
            }
            first = index->postings[first].next;
        }
        index->atom_cursor[base] = first;
        if (count == 0u) {
            return YAN_SEARCH_TERMS_OK; /* same-line AND fails */
        }
        score += count;
        if (earliest < anchor) {
            anchor = earliest;
        }
    }
    if (slot >= YAN_FS_MAX_FILES) {
        return YAN_SEARCH_TERMS_PROTOCOL;
    }
    yan_search_terms_rank_add(rank, index->entries[slot].name, slot, line,
                              index->entries[slot].size_bytes, anchor,
                              (uint16_t)score);
    return YAN_SEARCH_TERMS_OK;
}

static YanSearchTermsResult index_search_locked(
    YanSearchTermsIndex *index, const uint8_t *query, uint32_t query_length,
    YanSearchTermsMatchFn match, void *match_context,
    YanSearchTermsSummary *summary)
{
    YanSearchTermsQuery parsed;
    YanSearchTermsResult result =
        yan_search_terms_parse(query, query_length, &parsed);
    if (result != YAN_SEARCH_TERMS_OK) {
        return result;
    }

    uint32_t atom_count = 0u;
    for (uint32_t g = 0u; g < parsed.count; ++g) {
        uint32_t start = parsed.offset[g];
        uint32_t length = parsed.length[g];
        index->group_atom_start[g] = (uint16_t)atom_count;
        uint32_t i = 0u;
        while (i < length) {
            uint8_t byte = query[start + i];
            uint32_t key = INDEX_NO_KEY;
            if (yan_search_text_is_ascii_word(byte)) {
                uint32_t begin = i;
                while (i < length &&
                       yan_search_text_is_ascii_word(query[start + i])) {
                    ++i;
                }
                uint32_t word_length = i - begin;
                uint8_t folded[YAN_SEARCH_TERMS_WORD_MAX + 1u];
                for (uint32_t j = 0u; j < word_length; ++j) {
                    folded[j] =
                        yan_search_text_ascii_fold(query[start + begin + j]);
                }
                uint32_t found = INDEX_NO_KEY;
                if (index_key_lookup(index, folded, word_length, &found)) {
                    key = found;
                }
            } else {
                uint32_t need = yan_search_text_utf8_expected(
                    query + start + i, length - i);
                if (need == 0u || i + need > length) {
                    return YAN_SEARCH_TERMS_INVALID;
                }
                uint32_t found = INDEX_NO_KEY;
                if (index_key_lookup(index, query + start + i, need, &found)) {
                    key = found;
                }
                i += need;
            }
            if (atom_count >= YAN_SEARCH_TERMS_QUERY_MAX) {
                return YAN_SEARCH_TERMS_INVALID;
            }
            index->atom_key[atom_count] = key;
            ++atom_count;
        }
        index->group_atom_count[g] =
            (uint16_t)(atom_count - index->group_atom_start[g]);
    }

    /* A missing atom means at least one group cannot match at all. */
    for (uint32_t a = 0u; a < atom_count; ++a) {
        if (index->atom_key[a] == INDEX_NO_KEY) {
            if (summary != NULL) {
                summary->total = 0u;
                summary->shown = 0u;
                summary->skipped = index->skipped;
                summary->mode = YAN_SEARCH_MODE_INDEX;
            }
            return YAN_SEARCH_TERMS_OK;
        }
    }

    uint32_t anchor_atom = 0u;
    uint32_t anchor_count = index->keys[index->atom_key[0]].count;
    for (uint32_t a = 1u; a < atom_count; ++a) {
        uint32_t count = index->keys[index->atom_key[a]].count;
        if (count < anchor_count) {
            anchor_count = count;
            anchor_atom = a;
        }
    }
    for (uint32_t a = 0u; a < atom_count; ++a) {
        index->atom_cursor[a] = index->keys[index->atom_key[a]].head;
    }

    YanSearchTermsRank *rank = &index->linear.rank;
    yan_search_terms_rank_init(rank);
    uint32_t last_slot = UINT32_MAX;
    uint32_t last_line = UINT32_MAX;
    bool have_last = false;
    uint32_t posting =
        index->keys[index->atom_key[anchor_atom]].head;
    while (posting != INDEX_NO_POSTING) {
        uint32_t slot = index->postings[posting].slot;
        uint32_t line = index->postings[posting].line;
        if (!have_last || slot != last_slot || line != last_line) {
            have_last = true;
            last_slot = slot;
            last_line = line;
            result = index_eval_line(index, &parsed, slot, line, rank);
            if (result != YAN_SEARCH_TERMS_OK) {
                return result;
            }
        }
        posting = index->postings[posting].next;
    }

    uint32_t shown = rank->count < YAN_SEARCH_TERMS_SHOWN_MAX
                         ? rank->count
                         : YAN_SEARCH_TERMS_SHOWN_MAX;
    for (uint32_t i = 0u; i < shown; ++i) {
        YanSearchTermsCandidate *candidate = &rank->candidate[i];
        IndexReader reader;
        reader.fs = index->fs;
        reader.name = candidate->name;
        const uint8_t *body = NULL;
        uint32_t body_length = 0u;
        bool left = false;
        bool right = false;
        result = yan_search_terms_core_snippet(
            candidate->file_size, candidate->anchor, index_read, &reader,
            &body, &body_length, &left, &right, index->linear.scratch.snippet,
            YAN_SEARCH_TERMS_SNIPPET_SCRATCH);
        if (result != YAN_SEARCH_TERMS_OK) {
            return result;
        }
        index->linear.snippet = body;
        index->linear.snippet_length = body_length;
        YanSearchTermsMatch value;
        value.name = candidate->name;
        value.line_number = candidate->line_number;
        value.score = candidate->score;
        value.snippet_length = body_length;
        value.left_truncated = left;
        value.right_truncated = right;
        if (!match(match_context, &value)) {
            return YAN_SEARCH_TERMS_OK; /* facade maps this to STOPPED */
        }
    }
    if (summary != NULL) {
        summary->total = rank->total;
        summary->shown = shown;
        summary->skipped = index->skipped;
        summary->mode = YAN_SEARCH_MODE_INDEX;
    }
    return YAN_SEARCH_TERMS_OK;
}

static YanSearchTermsResult index_scan_locked(
    YanSearchTermsIndex *index, const uint8_t *query, uint32_t query_length,
    YanSearchTermsMatchFn match, void *match_context,
    YanSearchTermsSummary *summary)
{
    return yan_search_terms_linear_query(&index->linear, query, query_length,
                                         match, match_context, summary);
}

static YanSearchTermsResult index_query_locked(
    YanSearchTermsIndex *index, const uint8_t *query, uint32_t query_length,
    YanSearchTermsMatchFn match, void *match_context,
    YanSearchTermsSummary *summary)
{
    YanFsSource source;
    YanSearchTermsResult result = index_observe(index, &source);
    if (result == YAN_SEARCH_TERMS_BUSY) {
        return YAN_SEARCH_TERMS_BUSY;
    }
    if (result == YAN_SEARCH_TERMS_INVALID) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    if (result != YAN_SEARCH_TERMS_OK) {
        return result;
    }
    if (source.state != YAN_FS_MOUNTED) {
        return source.state == YAN_FS_STATE_FAULTED
                   ? YAN_SEARCH_TERMS_FAULTED
                   : YAN_SEARCH_TERMS_NOT_MOUNTED;
    }
    if (!source.cacheable) {
        index_clear_cache(index);
        index->state = YAN_SEARCH_INDEX_UNCACHEABLE;
        return index_scan_locked(index, query, query_length, match,
                                 match_context, summary);
    }
    bool token_match = index->token_valid && source.token == index->token;
    if (index->state == YAN_SEARCH_INDEX_UNCACHEABLE) {
        return index_scan_locked(index, query, query_length, match,
                                 match_context, summary);
    }
    if (index->state == YAN_SEARCH_INDEX_READY && token_match) {
        return index_search_locked(index, query, query_length, match,
                                   match_context, summary);
    }
    if (index->state == YAN_SEARCH_INDEX_LIMIT && token_match) {
        return index_scan_locked(index, query, query_length, match,
                                 match_context, summary);
    }
    /* EMPTY, STALE or a changed READY/LIMIT identity: rebuild once. */
    result = index_build_locked(index);
    if (result == YAN_SEARCH_TERMS_INDEX_LIMIT) {
        return index_scan_locked(index, query, query_length, match,
                                 match_context, summary);
    }
    if (result != YAN_SEARCH_TERMS_OK) {
        return result;
    }
    if (index->state == YAN_SEARCH_INDEX_READY) {
        return index_search_locked(index, query, query_length, match,
                                   match_context, summary);
    }
    return index_scan_locked(index, query, query_length, match, match_context,
                             summary);
}

static YanSearchTermsResult index_guard(const YanSearchTermsIndex *index)
{
    if (index == NULL || !index->initialized) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    if (index->busy) {
        return YAN_SEARCH_TERMS_BUSY;
    }
    return YAN_SEARCH_TERMS_OK;
}

static YanSearchTermsResult index_query(void *context, const uint8_t *query,
                                        uint32_t query_length,
                                        YanSearchTermsMatchFn match,
                                        void *match_context,
                                        YanSearchTermsSummary *summary)
{
    YanSearchTermsIndex *index = (YanSearchTermsIndex *)context;
    YanSearchTermsResult guard = index_guard(index);
    if (guard != YAN_SEARCH_TERMS_OK) {
        return guard;
    }
    if (match == NULL) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    index->busy = true;
    YanSearchTermsResult result = index_query_locked(
        index, query, query_length, match, match_context, summary);
    index->busy = false;
    return result;
}

static YanSearchTermsResult index_read_match(void *context, uint32_t offset,
                                             uint32_t capacity,
                                             const uint8_t **bytes,
                                             uint32_t *length)
{
    YanSearchTermsIndex *index = (YanSearchTermsIndex *)context;
    if (index == NULL || !index->initialized) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    return yan_search_terms_linear_read_match(&index->linear, offset, capacity,
                                              bytes, length);
}

static YanSearchTermsResult index_status(void *context,
                                         YanSearchTermsStatus *out)
{
    YanSearchTermsIndex *index = (YanSearchTermsIndex *)context;
    YanSearchTermsResult guard = index_guard(index);
    if (guard != YAN_SEARCH_TERMS_OK) {
        return guard;
    }
    if (out == NULL) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    YanFsSource source;
    YanSearchTermsResult result = index_observe(index, &source);
    if (result == YAN_SEARCH_TERMS_BUSY) {
        return YAN_SEARCH_TERMS_BUSY; /* out untouched */
    }
    out->terms = 0u;
    out->postings = 0u;
    if (result == YAN_SEARCH_TERMS_INVALID) {
        out->state = YAN_SEARCH_INDEX_UNAVAILABLE;
        out->source = YAN_SEARCH_SOURCE_UNINITIALIZED;
        return YAN_SEARCH_TERMS_OK;
    }
    if (result != YAN_SEARCH_TERMS_OK) {
        return result;
    }
    out->source = index_source_class(source.state);
    if (source.state != YAN_FS_MOUNTED) {
        out->state = YAN_SEARCH_INDEX_UNAVAILABLE;
        return YAN_SEARCH_TERMS_OK;
    }
    if (!source.cacheable) {
        out->state = YAN_SEARCH_INDEX_UNCACHEABLE;
        return YAN_SEARCH_TERMS_OK;
    }
    bool token_match = index->token_valid && source.token == index->token;
    if ((index->state == YAN_SEARCH_INDEX_READY ||
         index->state == YAN_SEARCH_INDEX_LIMIT) &&
        !token_match) {
        out->state = YAN_SEARCH_INDEX_STALE;
        return YAN_SEARCH_TERMS_OK;
    }
    out->state = index->state;
    if (index->state == YAN_SEARCH_INDEX_READY) {
        out->terms = index->term_count;
        out->postings = index->posting_count;
    }
    return YAN_SEARCH_TERMS_OK;
}

static YanSearchTermsResult index_clear(void *context)
{
    YanSearchTermsIndex *index = (YanSearchTermsIndex *)context;
    YanSearchTermsResult guard = index_guard(index);
    if (guard != YAN_SEARCH_TERMS_OK) {
        return guard;
    }
    index_clear_cache(index);
    return YAN_SEARCH_TERMS_OK;
}

static YanSearchTermsResult index_rebuild(void *context)
{
    YanSearchTermsIndex *index = (YanSearchTermsIndex *)context;
    YanSearchTermsResult guard = index_guard(index);
    if (guard != YAN_SEARCH_TERMS_OK) {
        return guard;
    }
    index->busy = true;
    YanSearchTermsResult result = index_build_locked(index);
    index->busy = false;
    return result;
}

YanSearchTermsResult yan_search_terms_index_init(YanSearchTermsIndex *index,
                                                 YanFs *fs)
{
    if (index == NULL || fs == NULL) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    if (index_ranges_overlap(index, sizeof *index, fs, sizeof *fs)) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    if (index->initialized && index->busy) {
        return YAN_SEARCH_TERMS_BUSY;
    }
    YanSearchTermsResult result =
        yan_search_terms_linear_init(&index->linear, fs);
    if (result != YAN_SEARCH_TERMS_OK) {
        return result;
    }
    index->fs = fs;
    index->initialized = true;
    index->busy = false;
    index_clear_cache(index);
    return YAN_SEARCH_TERMS_OK;
}

YanSearchTermsBackend yan_search_terms_index_backend(
    YanSearchTermsIndex *index)
{
    YanSearchTermsBackend backend;
    backend.context = index;
    backend.context_size = sizeof(YanSearchTermsIndex);
    backend.source_context = (index != NULL) ? (void *)index->fs : NULL;
    backend.source_size =
        (index != NULL && index->fs != NULL) ? sizeof(YanFs) : 0u;
    backend.query = index_query;
    backend.read_match = index_read_match;
    backend.status = index_status;
    backend.rebuild = index_rebuild;
    backend.clear = index_clear;
    return backend;
}
