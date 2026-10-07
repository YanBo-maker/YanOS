/* Shared term-search core for 0026.
 *
 * The contract is os/search_terms_core.h. Everything here is pure or driven
 * through the caller's read callback: no filesystem, no console, no libc and no
 * allocation. The shape is:
 *
 *   parse                  one pass over the raw query validates the byte
 *   grammar, folds ASCII case, de-duplicates canonical groups and records the
 *   word-boundary flags. Repeated canonical groups cost one slot; more than 16
 *   distinct groups is INVALID.
 *
 *   pattern build          folds each group into one shared buffer and builds
 *   one KMP prefix table per group in the caller's buffers.
 *
 *   streaming scan         whole-file UTF-8/NUL preflight carries an incomplete
 *   scalar across a chunk boundary, so a legal sequence split by a read is not
 *   mistaken for damage. The scan then walks the bytes once with a one-byte
 *   lookahead (held across chunk boundaries) so a full match can check the
 *   right word boundary, while a 1024-byte history ring records whether the
 *   byte before a candidate start was an ASCII word character. Scores are
 *   counted per line, capped per group at 255, and only the best 20 lines are
 *   kept; the whole-database total is independent of that cap.
 *
 *   snippet                after ranking, re-reads a fixed window around the
 *   stored anchor, takes at most 40 scalars left and 160 in total, and stops at
 *   the first line break. It never rescans from the file head and never keeps a
 *   whole line.
 *
 * Position arithmetic is uint32 (the on-disk size field is uint32); the ring
 * masks with 1023 so it also behaves if a position wrapped. The line number is
 * uint32 and only overflows on a file with four billion lines, which the format
 * cannot describe. */

#include "search_terms_core.h"

#include "search_text.h"

#include <stddef.h>

/* A byte that separates query groups: TAB, or any ASCII byte that is not an
 * ASCII word character. Bytes >= 0x80 are content. */
static bool query_separator(uint8_t byte)
{
    if (byte == (uint8_t)'\t') {
        return true;
    }
    if (byte >= 0x80u) {
        return false;
    }
    return !yan_search_text_is_ascii_word(byte);
}

static bool group_equal(const uint8_t *query, uint32_t first_offset,
                        uint32_t first_length, uint32_t second_offset,
                        uint32_t second_length)
{
    if (first_length != second_length) {
        return false;
    }
    for (uint32_t i = 0; i < first_length; ++i) {
        if (yan_search_text_ascii_fold(query[first_offset + i]) !=
            yan_search_text_ascii_fold(query[second_offset + i])) {
            return false;
        }
    }
    return true;
}

YanSearchTermsResult yan_search_terms_parse(const uint8_t *query,
                                            uint32_t query_length,
                                            YanSearchTermsQuery *out)
{
    if (query == NULL || out == NULL) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    if (query_length == 0u || query_length > YAN_SEARCH_TERMS_QUERY_MAX) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    for (uint32_t i = 0; i < query_length; ++i) {
        uint8_t byte = query[i];
        if (byte == 0u) {
            return YAN_SEARCH_TERMS_INVALID;
        }
        if (byte < 0x20u && byte != (uint8_t)'\t') {
            return YAN_SEARCH_TERMS_INVALID; /* LF, CR and the other controls */
        }
        if (byte == 0x7fu) {
            return YAN_SEARCH_TERMS_INVALID; /* DEL */
        }
    }
    if (!yan_search_text_utf8_valid(query, query_length)) {
        return YAN_SEARCH_TERMS_INVALID;
    }

    out->count = 0u;
    uint32_t at = 0u;
    while (at < query_length) {
        while (at < query_length && query_separator(query[at])) {
            ++at;
        }
        if (at >= query_length) {
            break;
        }
        uint32_t start = at;
        uint32_t word = 0u;
        while (at < query_length && !query_separator(query[at])) {
            if (yan_search_text_is_ascii_word(query[at])) {
                ++word;
                if (word > YAN_SEARCH_TERMS_WORD_MAX) {
                    return YAN_SEARCH_TERMS_INVALID;
                }
            } else {
                word = 0u;
            }
            ++at;
        }
        uint32_t length = at - start;
        bool duplicate = false;
        for (uint32_t g = 0u; g < out->count; ++g) {
            if (group_equal(query, start, length, out->offset[g],
                            out->length[g])) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        if (out->count >= YAN_SEARCH_TERMS_GROUP_MAX) {
            return YAN_SEARCH_TERMS_INVALID;
        }
        uint32_t g = out->count;
        out->offset[g] = (uint16_t)start;
        out->length[g] = (uint16_t)length;
        out->left_word[g] = yan_search_text_is_ascii_word(query[start]);
        out->right_word[g] =
            yan_search_text_is_ascii_word(query[start + length - 1u]);
        ++out->count;
    }
    if (out->count == 0u) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    return YAN_SEARCH_TERMS_OK;
}

YanSearchTermsResult yan_search_terms_pattern_build(
    const uint8_t *query, const YanSearchTermsQuery *parsed,
    uint8_t *folded, uint32_t folded_capacity,
    uint16_t *prefix, uint32_t prefix_capacity,
    YanSearchTermsPattern *out)
{
    if (query == NULL || parsed == NULL || folded == NULL || prefix == NULL ||
        out == NULL || parsed->count == 0u) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    uint32_t total = 0u;
    for (uint32_t g = 0u; g < parsed->count; ++g) {
        total += parsed->length[g];
    }
    if (total > folded_capacity || total > prefix_capacity) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    out->count = parsed->count;
    uint32_t at = 0u;
    for (uint32_t g = 0u; g < parsed->count; ++g) {
        uint32_t length = parsed->length[g];
        out->length[g] = (uint16_t)length;
        out->fold_start[g] = (uint16_t)at;
        out->left_word[g] = parsed->left_word[g];
        out->right_word[g] = parsed->right_word[g];
        for (uint32_t j = 0u; j < length; ++j) {
            folded[at + j] =
                yan_search_text_ascii_fold(query[parsed->offset[g] + j]);
        }
        prefix[at] = 0u;
        uint32_t k = 0u;
        for (uint32_t j = 1u; j < length; ++j) {
            while (k > 0u && folded[at + j] != folded[at + k]) {
                k = prefix[at + k - 1u];
            }
            if (folded[at + j] == folded[at + k]) {
                ++k;
            }
            prefix[at + j] = (uint16_t)k;
        }
        at += length;
    }
    out->folded = folded;
    out->prefix = prefix;
    return YAN_SEARCH_TERMS_OK;
}

void yan_search_terms_rank_init(YanSearchTermsRank *rank)
{
    if (rank == NULL) {
        return;
    }
    rank->count = 0u;
    rank->total = 0u;
    rank->skipped = 0u;
}

static bool rank_better(const YanSearchTermsCandidate *candidate,
                        const YanSearchTermsCandidate *existing)
{
    if (candidate->score != existing->score) {
        return candidate->score > existing->score;
    }
    if (candidate->slot != existing->slot) {
        return candidate->slot < existing->slot;
    }
    return candidate->line_number < existing->line_number;
}

void yan_search_terms_rank_add(YanSearchTermsRank *rank, const char *name,
                               uint32_t slot, uint32_t line_number,
                               uint32_t file_size, uint32_t anchor,
                               uint16_t score)
{
    if (rank == NULL) {
        return;
    }
    ++rank->total;
    YanSearchTermsCandidate candidate;
    uint32_t name_at = 0u;
    while (name_at < 31u && name[name_at] != '\0') {
        candidate.name[name_at] = name[name_at];
        ++name_at;
    }
    candidate.name[name_at] = '\0';
    candidate.slot = slot;
    candidate.line_number = line_number;
    candidate.file_size = file_size;
    candidate.anchor = anchor;
    candidate.score = score;

    uint32_t position = rank->count;
    for (uint32_t i = 0u; i < rank->count; ++i) {
        if (rank_better(&candidate, &rank->candidate[i])) {
            position = i;
            break;
        }
    }
    if (position >= YAN_SEARCH_TERMS_SHOWN_MAX) {
        return; /* worse than every stored candidate */
    }
    uint32_t last = rank->count < YAN_SEARCH_TERMS_SHOWN_MAX
                        ? rank->count
                        : YAN_SEARCH_TERMS_SHOWN_MAX - 1u;
    for (uint32_t i = last; i > position; --i) {
        rank->candidate[i] = rank->candidate[i - 1u];
    }
    rank->candidate[position] = candidate;
    if (rank->count < YAN_SEARCH_TERMS_SHOWN_MAX) {
        ++rank->count;
    }
}

/* Whole-file UTF-8/NUL preflight. It reads in fixed chunks bounded by the
 * declared file size and carries an unfinished scalar, so a legal sequence that
 * straddles two reads is accepted while a bad byte or an unfinished tail makes
 * the whole file skip once. A callback that overruns the request or makes no
 * progress is a protocol error, not a silent early end. Exported so the index
 * build shares this exact check with the linear scan. */
YanSearchTermsResult yan_search_terms_core_preflight(
    YanSearchTermsSourceReadFn read, void *read_context, uint8_t *chunk,
    uint32_t capacity, uint32_t file_size, bool *skip)
{
    *skip = false;
    uint8_t carry[4];
    uint32_t carry_length = 0u;
    uint32_t offset = 0u;
    while (offset < file_size) {
        uint32_t remaining = file_size - offset;
        uint32_t request = remaining < capacity ? remaining : capacity;
        uint32_t got = 0u;
        YanSearchTermsResult result =
            read(read_context, offset, request, chunk, &got);
        if (result != YAN_SEARCH_TERMS_OK) {
            return result;
        }
        if (got > request || got == 0u) {
            return YAN_SEARCH_TERMS_PROTOCOL;
        }
        for (uint32_t i = 0u; i < got; ++i) {
            uint8_t byte = chunk[i];
            if (byte == 0u) {
                *skip = true;
                return YAN_SEARCH_TERMS_OK;
            }
            carry[carry_length] = byte;
            ++carry_length;
            uint32_t need = yan_search_text_utf8_expected(carry, carry_length);
            if (need == 0u) {
                *skip = true;
                return YAN_SEARCH_TERMS_OK;
            }
            if (carry_length >= need) {
                uint32_t rest = carry_length - need;
                for (uint32_t j = 0u; j < rest; ++j) {
                    carry[j] = carry[need + j];
                }
                carry_length = rest;
            }
        }
        offset += got;
    }
    if (carry_length != 0u) {
        *skip = true; /* an unfinished sequence at end of file is damage */
    }
    return YAN_SEARCH_TERMS_OK;
}

typedef struct {
    uint32_t pos;
    uint32_t line_number;
    uint16_t counts[YAN_SEARCH_TERMS_GROUP_MAX];
    uint16_t state[YAN_SEARCH_TERMS_GROUP_MAX];
    uint32_t anchor;
    bool have_anchor;
    bool line_active;
    uint8_t *ring;
    const YanSearchTermsPattern *pattern;
    const char *name;
    uint32_t slot;
    uint32_t file_size;
    YanSearchTermsRank *rank;
} ScanState;

static void scan_finalize(ScanState *state)
{
    const YanSearchTermsPattern *pattern = state->pattern;
    uint32_t score = 0u;
    bool complete = pattern->count > 0u;
    for (uint32_t g = 0u; g < pattern->count; ++g) {
        if (state->counts[g] == 0u) {
            complete = false;
            break;
        }
        score += state->counts[g];
    }
    if (complete) {
        yan_search_terms_rank_add(state->rank, state->name, state->slot,
                                  state->line_number, state->file_size,
                                  state->anchor, (uint16_t)score);
    }
}

static void scan_byte(ScanState *state, uint8_t byte, uint8_t next_byte,
                      bool next_eof)
{
    const YanSearchTermsPattern *pattern = state->pattern;
    uint32_t mask = YAN_SEARCH_TERMS_RING - 1u;
    state->ring[state->pos & mask] =
        yan_search_text_is_ascii_word(byte) ? 1u : 0u;
    uint8_t folded = yan_search_text_ascii_fold(byte);

    for (uint32_t g = 0u; g < pattern->count; ++g) {
        uint32_t base = pattern->fold_start[g];
        uint32_t length = pattern->length[g];
        uint32_t kmp = state->state[g];
        while (kmp > 0u && pattern->folded[base + kmp] != folded) {
            kmp = pattern->prefix[base + kmp - 1u];
        }
        if (pattern->folded[base + kmp] == folded) {
            ++kmp;
        }
        if (kmp == length) {
            uint32_t start = state->pos - length + 1u;
            bool accepted = true;
            if (pattern->left_word[g] && start > 0u &&
                state->ring[(start - 1u) & mask] != 0u) {
                accepted = false;
            }
            if (accepted && pattern->right_word[g] && !next_eof &&
                yan_search_text_is_ascii_word(next_byte)) {
                accepted = false;
            }
            if (accepted) {
                if (state->counts[g] < 255u) {
                    ++state->counts[g];
                }
                if (!state->have_anchor || start < state->anchor) {
                    state->anchor = start;
                    state->have_anchor = true;
                }
            }
            kmp = pattern->prefix[base + length - 1u];
        }
        state->state[g] = (uint16_t)kmp;
    }

    /* Only LF ends a line. The CR of a CRLF is an ordinary content byte here;
     * it never matches a legal group and the word-boundary checks already treat
     * it as a separator, so a bare CR simply stays in the line. */
    if (byte == (uint8_t)'\n') {
        scan_finalize(state);
        ++state->line_number;
        for (uint32_t g = 0u; g < pattern->count; ++g) {
            state->counts[g] = 0u;
        }
        state->have_anchor = false;
        state->anchor = 0u;
        state->line_active = false;
    } else {
        state->line_active = true;
    }
    ++state->pos;
}

YanSearchTermsResult yan_search_terms_core_scan_file(
    const char *name, uint32_t slot, uint32_t file_size,
    YanSearchTermsSourceReadFn read, void *read_context,
    const YanSearchTermsPattern *pattern, YanSearchTermsRank *rank,
    YanSearchTermsScratch *scratch)
{
    if (name == NULL || read == NULL || pattern == NULL || rank == NULL ||
        scratch == NULL) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    bool skip = false;
    YanSearchTermsResult result =
        yan_search_terms_core_preflight(read, read_context, scratch->chunk,
                                        YAN_SEARCH_TERMS_CHUNK, file_size,
                                        &skip);
    if (result != YAN_SEARCH_TERMS_OK) {
        return result;
    }
    if (skip) {
        ++rank->skipped;
        return YAN_SEARCH_TERMS_OK;
    }

    for (uint32_t i = 0u; i < YAN_SEARCH_TERMS_RING; ++i) {
        scratch->ring[i] = 0u;
    }
    ScanState state;
    state.pos = 0u;
    state.line_number = 1u;
    for (uint32_t g = 0u; g < pattern->count; ++g) {
        state.counts[g] = 0u;
        state.state[g] = 0u;
    }
    state.anchor = 0u;
    state.have_anchor = false;
    state.line_active = false;
    state.ring = scratch->ring;
    state.pattern = pattern;
    state.name = name;
    state.slot = slot;
    state.file_size = file_size;
    state.rank = rank;

    /* Bounded reads: never ask for bytes past file_size and stop exactly at the
     * declared end, so a normal file never triggers a read at end of file. A
     * callback that overruns the request or stalls before the end is a protocol
     * error, never an infinite loop. */
    uint32_t offset = 0u;
    bool have_pending = false;
    uint8_t pending = 0u;
    while (offset < file_size) {
        uint32_t remaining = file_size - offset;
        uint32_t request =
            remaining < YAN_SEARCH_TERMS_CHUNK ? remaining
                                               : YAN_SEARCH_TERMS_CHUNK;
        uint32_t got = 0u;
        result = read(read_context, offset, request, scratch->chunk, &got);
        if (result != YAN_SEARCH_TERMS_OK) {
            return result;
        }
        if (got > request || got == 0u) {
            return YAN_SEARCH_TERMS_PROTOCOL;
        }
        if (have_pending) {
            scan_byte(&state, pending, scratch->chunk[0], false);
            have_pending = false;
        }
        for (uint32_t i = 0u; i + 1u < got; ++i) {
            scan_byte(&state, scratch->chunk[i], scratch->chunk[i + 1u], false);
        }
        pending = scratch->chunk[got - 1u];
        have_pending = true;
        offset += got;
    }
    if (have_pending) {
        scan_byte(&state, pending, 0u, true);
    }
    if (state.line_active) {
        scan_finalize(&state);
    }
    return YAN_SEARCH_TERMS_OK;
}

static uint32_t snippet_align(const uint8_t *window, uint32_t got,
                              uint32_t anchor_relative)
{
    uint32_t base = 0u;
    while (base < got && base < anchor_relative &&
           (window[base] & 0xc0u) == 0x80u) {
        ++base;
    }
    return base;
}

YanSearchTermsResult yan_search_terms_core_snippet(
    uint32_t file_size, uint32_t anchor, YanSearchTermsSourceReadFn read,
    void *read_context, const uint8_t **body, uint32_t *body_length,
    bool *left_truncated, bool *right_truncated, uint8_t *scratch,
    uint32_t scratch_capacity)
{
    if (read == NULL || body == NULL || body_length == NULL ||
        left_truncated == NULL || right_truncated == NULL || scratch == NULL) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    /* A real hit occupies at least one byte, so its anchor is strictly inside
     * the file. Anything else is an impossible request. */
    if (anchor >= file_size) {
        return YAN_SEARCH_TERMS_INVALID;
    }
    /* Bounded window: at most 255 bytes to the left and 704 to the right, but
     * never past file_size and never with a wrapping addition. The remaining
     * subtraction happens first, so anchor + tail cannot overflow. */
    uint32_t head = anchor < 255u ? anchor : 255u;
    uint32_t win_start = anchor - head;
    uint32_t tail = file_size - anchor;
    if (tail > 704u) {
        tail = 704u;
    }
    uint32_t win_end = anchor + tail;
    uint32_t want = win_end - win_start;
    if (want > scratch_capacity) {
        want = scratch_capacity;
    }
    if (want <= head) {
        /* The clipped read would not even cover the anchor byte. */
        return YAN_SEARCH_TERMS_PROTOCOL;
    }
    uint32_t got = 0u;
    YanSearchTermsResult result =
        read(read_context, win_start, want, scratch, &got);
    if (result != YAN_SEARCH_TERMS_OK) {
        return result;
    }
    if (got > want || got == 0u) {
        return YAN_SEARCH_TERMS_PROTOCOL;
    }
    uint32_t anchor_relative = anchor - win_start;
    if (anchor_relative >= got) {
        return YAN_SEARCH_TERMS_PROTOCOL; /* the anchor was not read */
    }
    uint32_t base = snippet_align(scratch, got, anchor_relative);

    /* Walk the scalars before the anchor, resetting only at LF. The CR of a
     * CRLF belongs to the terminator and is skipped, while a bare CR is a
     * content scalar that stays in the snippet. Keep the last 41 starts so the
     * 40th-back can become the left edge. */
    uint32_t starts[YAN_SEARCH_TERMS_SNIPPET_LEFT + 1u];
    uint32_t ring_pos = 0u;
    uint32_t since_break = 0u;
    uint32_t i = base;
    while (i < anchor_relative) {
        uint8_t byte = scratch[i];
        if (byte == (uint8_t)'\n') {
            ++i;
            ring_pos = 0u;
            since_break = 0u;
            continue;
        }
        if (byte == (uint8_t)'\r') {
            if (i + 1u < got && scratch[i + 1u] == (uint8_t)'\n') {
                i += 2u; /* the CR of a CRLF is part of the terminator */
                ring_pos = 0u;
                since_break = 0u;
                continue;
            }
            starts[ring_pos] = i; /* bare CR: one content scalar */
            ring_pos = (ring_pos + 1u) % (YAN_SEARCH_TERMS_SNIPPET_LEFT + 1u);
            ++since_break;
            ++i;
            continue;
        }
        uint32_t need = yan_search_text_utf8_expected(scratch + i, got - i);
        if (need == 0u || i + need > got || i + need > anchor_relative) {
            need = 1u;
        }
        starts[ring_pos] = i;
        ring_pos = (ring_pos + 1u) % (YAN_SEARCH_TERMS_SNIPPET_LEFT + 1u);
        ++since_break;
        i += need;
    }
    uint32_t left_count = since_break < YAN_SEARCH_TERMS_SNIPPET_LEFT
                              ? since_break
                              : YAN_SEARCH_TERMS_SNIPPET_LEFT;
    *left_truncated = since_break > YAN_SEARCH_TERMS_SNIPPET_LEFT;
    uint32_t left_edge = anchor_relative;
    if (left_count > 0u) {
        uint32_t index =
            (ring_pos + (YAN_SEARCH_TERMS_SNIPPET_LEFT + 1u) - left_count) %
            (YAN_SEARCH_TERMS_SNIPPET_LEFT + 1u);
        left_edge = starts[index];
    }

    /* Walk forward from the anchor to the scalar cap or the line end. LF ends
     * the line; a CR ends it only when the next byte is LF, and any other CR is
     * content that stays in the snippet. */
    uint32_t right_end = anchor_relative;
    uint32_t right_count = 0u;
    i = anchor_relative;
    while (left_count + right_count < YAN_SEARCH_TERMS_SNIPPET_TOTAL) {
        if (i >= got) {
            break;
        }
        uint8_t byte = scratch[i];
        if (byte == (uint8_t)'\n') {
            break;
        }
        if (byte == (uint8_t)'\r') {
            if (i + 1u < got && scratch[i + 1u] == (uint8_t)'\n') {
                break; /* exclude the CR of a CRLF */
            }
            i += 1u;
            right_end = i;
            ++right_count;
            continue;
        }
        uint32_t need = yan_search_text_utf8_expected(scratch + i, got - i);
        if (need == 0u || i + need > got) {
            break;
        }
        i += need;
        right_end = i;
        ++right_count;
    }
    *right_truncated = false;
    if (left_count + right_count == YAN_SEARCH_TERMS_SNIPPET_TOTAL) {
        if (right_end < got) {
            uint8_t byte = scratch[right_end];
            bool at_line_end =
                byte == (uint8_t)'\n' ||
                (byte == (uint8_t)'\r' && right_end + 1u < got &&
                 scratch[right_end + 1u] == (uint8_t)'\n');
            if (!at_line_end) {
                *right_truncated = true;
            }
        } else if (win_start + right_end < file_size) {
            *right_truncated = true;
        }
    } else if (i >= got && win_start + i < file_size) {
        *right_truncated = true;
    }

    *body = scratch + left_edge;
    *body_length = right_end - left_edge;
    return YAN_SEARCH_TERMS_OK;
}
