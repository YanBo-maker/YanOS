/* Pure note-metadata format for 0027.
 *
 * The behavioural contract is os/note_metadata_format.h and
 * docs/specs/0027-note-metadata.md, "文件头及实际状态变化" and
 * "公共接口、状态与内存". This translation unit owns the bounded header
 * parser, the canonical encoder and the four field validators. It performs no
 * I/O, allocates nothing, includes no Host header, no YanFS header and no
 * shell header, and compiles unchanged for the Guest: the only external
 * dependency is os/memory.h for the byte copies, which the native CRT
 * satisfies for Host tests.
 *
 * Shape of the parse, in the order the checks run:
 *
 *   1. argument rules: null input (except a zero length), null out, null
 *      workspace, every span inside uintptr_t, and the pairwise alias
 *      rejection of bytes/out/workspace - all pure address arithmetic, before
 *      a single byte of input is read;
 *   2. the length cap: an offer above YAN_NOTE_HEADER_MAX is TOO_LARGE, so a
 *      caller cannot smuggle an unterminated 4097-byte prefix into METADATA;
 *   3. recognition at byte 0: without the complete 9-byte `@yan-note` prefix
 *      the file is an old no-header file (OK, empty metadata); with the
 *      prefix but without the full 12-byte marker, or with an unknown
 *      version, it is METADATA;
 *   4. the bounded line walk: only LF ends a line, the walk stops at `@end`
 *      or at the bounded prefix end, and the parser never reads a body byte.
 *      A missing close, an unknown key, a duplicate, a non-canonical field
 *      spelling or an invalid value is METADATA;
 *   5. publication: *out is written only after the whole header validated, so
 *      every error leaves the caller's holder byte-identical. A no-header
 *      success publishes a fully zeroed info.
 *
 * The caller's workspace is the accumulator: it holds the candidate info while
 * the header is being read and is copied out only on success. That keeps this
 * translation unit's frame small (no 2 KiB automatic object) while the large
 * object stays in the caller-owned static storage the header documents.
 *
 * FEFF boundary: the owner confirmed on 2026-10-08 that a recognised header
 * contains no U+FEFF, including field values. The parser rejects it in keys
 * and validated values; body bytes remain opaque. */

#include "note_metadata_format_internal.h"

#include "memory.h"

const uint8_t yan_note_marker[YAN_NOTE_MARKER_SIZE] = {
    (uint8_t)'@', (uint8_t)'y', (uint8_t)'a', (uint8_t)'n', (uint8_t)'-',
    (uint8_t)'n', (uint8_t)'o', (uint8_t)'t', (uint8_t)'e', (uint8_t)' ',
    (uint8_t)'1', (uint8_t)'\n'
};

/* The recognition prefix is exactly the first nine marker bytes, `@yan-note`.
 * It deliberately excludes the space, the version digit and the LF: a shorter
 * or otherwise different prefix is an old no-header file, while a matching
 * prefix that is not followed by the exact marker is METADATA. */
const uint8_t yan_note_marker_prefix[YAN_NOTE_PREFIX_SIZE] = {
    (uint8_t)'@', (uint8_t)'y', (uint8_t)'a', (uint8_t)'n', (uint8_t)'-',
    (uint8_t)'n', (uint8_t)'o', (uint8_t)'t', (uint8_t)'e'
};

static bool prefix_matches(const uint8_t *bytes, uint32_t length)
{
    if (length < YAN_NOTE_PREFIX_SIZE) {
        return false;
    }
    for (uint32_t i = 0u; i < YAN_NOTE_PREFIX_SIZE; ++i) {
        if (bytes[i] != yan_note_marker_prefix[i]) {
            return false;
        }
    }
    return true;
}

static bool marker_matches(const uint8_t *bytes, uint32_t length)
{
    if (length < YAN_NOTE_MARKER_SIZE) {
        return false;
    }
    for (uint32_t i = 0u; i < YAN_NOTE_MARKER_SIZE; ++i) {
        if (bytes[i] != yan_note_marker[i]) {
            return false;
        }
    }
    return true;
}

/* True when two caller spans intersect. A range that would leave uintptr_t is
 * reported as overlapping, matching the 0021/0025/0026 alias rule, and a
 * zero-length span is absent because it holds no byte. Nothing is read or
 * written through either range. */
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

/* The isolated U+FEFF decision described in the file comment. */
static bool scalar_forbidden_in_header(uint32_t code_point)
{
    return code_point == UINT32_C(0xFEFF);
}

/* Decodes exactly one UTF-8 scalar at bytes[0..length). It enforces the
 * shortest legal encoding, so it rejects continuation bytes without a lead,
 * overlong forms (C0/C1 and their kin), UTF-16 surrogates and every scalar
 * above U+10FFFF. It returns the byte length on success and 0 on any
 * violation. */
static uint32_t decode_scalar(const uint8_t *bytes, uint32_t length,
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
        return 0u; /* continuation lead, C0/C1, or an impossible F5..FF */
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

/* Rejects a scalar that a title may not carry: C0, DEL and the C1 block
 * (U+0080..U+009F), which includes TAB and every other separator byte. */
static bool scalar_forbidden_in_title(uint32_t code_point)
{
    return code_point < UINT32_C(0x20) ||
           (code_point >= UINT32_C(0x7F) && code_point <= UINT32_C(0x9F));
}

/* Rejects a scalar that a tag may not carry: the same control range plus the
 * fixed Unicode White_Space set from the spec. The set is hard-coded, never
 * taken from the Host locale. */
static bool scalar_forbidden_in_tag(uint32_t code_point)
{
    if (code_point < UINT32_C(0x20) ||
        (code_point >= UINT32_C(0x7F) && code_point <= UINT32_C(0x9F))) {
        return true;
    }
    switch (code_point) {
    case UINT32_C(0x20):
    case UINT32_C(0x85):
    case UINT32_C(0xA0):
    case UINT32_C(0x1680):
    case UINT32_C(0x2028):
    case UINT32_C(0x2029):
    case UINT32_C(0x202F):
    case UINT32_C(0x205F):
    case UINT32_C(0x3000):
        return true;
    default:
        return code_point >= UINT32_C(0x2000) &&
               code_point <= UINT32_C(0x200A);
    }
}

static bool text_is_valid(const uint8_t *bytes, uint32_t length, bool for_tag)
{
    uint32_t offset = 0u;
    while (offset < length) {
        uint32_t code_point = 0u;
        uint32_t used = decode_scalar(bytes + offset, length - offset,
                                      &code_point);
        if (used == 0u) {
            return false;
        }
        if (for_tag ? scalar_forbidden_in_tag(code_point)
                    : scalar_forbidden_in_title(code_point)) {
            return false;
        }
        if (scalar_forbidden_in_header(code_point)) {
            return false;
        }
        offset += used;
    }
    return true;
}

bool yan_note_title_valid(const uint8_t *bytes, uint32_t length)
{
    if (bytes == NULL || length < 1u || length > YAN_NOTE_TITLE_MAX ||
        (uint64_t)length > (uint64_t)(UINTPTR_MAX - (uintptr_t)bytes)) {
        return false;
    }
    return text_is_valid(bytes, length, false);
}

bool yan_note_tag_valid(const uint8_t *bytes, uint32_t length)
{
    if (bytes == NULL || length < 1u || length > YAN_NOTE_TAG_MAX ||
        (uint64_t)length > (uint64_t)(UINTPTR_MAX - (uintptr_t)bytes)) {
        return false;
    }
    return text_is_valid(bytes, length, true);
}

static bool is_leap_year(uint32_t year)
{
    return (year % 4u == 0u && year % 100u != 0u) || year % 400u == 0u;
}

static uint32_t days_in_month(uint32_t year, uint32_t month)
{
    static const uint8_t days[12] = { 31u, 28u, 31u, 30u, 31u, 30u,
                                      31u, 31u, 30u, 31u, 30u, 31u };
    if (month == 2u && is_leap_year(year)) {
        return 29u;
    }
    return (uint32_t)days[month - 1u];
}

/* Strictly ten bytes, ASCII digits in YYYY-MM-DD, year 0001..9999 and a real
 * Gregorian month/day. 2000-02-29 is legal; 1900-02-29 and 0000-01-01 are not. */
bool yan_note_date_valid(const uint8_t *bytes, uint32_t length)
{
    static const uint8_t positions[8] = { 0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u };
    uint32_t digits[8];
    if (bytes == NULL || length != YAN_NOTE_DATE_LENGTH ||
        (uint64_t)length > (uint64_t)(UINTPTR_MAX - (uintptr_t)bytes)) {
        return false;
    }
    if (bytes[4] != (uint8_t)'-' || bytes[7] != (uint8_t)'-') {
        return false;
    }
    for (uint32_t i = 0u; i < 8u; ++i) {
        uint8_t byte = bytes[positions[i]];
        if (byte < (uint8_t)'0' || byte > (uint8_t)'9') {
            return false;
        }
        digits[i] = (uint32_t)(byte - (uint8_t)'0');
    }
    uint32_t year = digits[0] * 1000u + digits[1] * 100u + digits[2] * 10u +
                    digits[3];
    uint32_t month = digits[4] * 10u + digits[5];
    uint32_t day = digits[6] * 10u + digits[7];
    if (year < 1u || year > 9999u || month < 1u || month > 12u || day < 1u) {
        return false;
    }
    return day <= days_in_month(year, month);
}

/* The YanFS 1..31 byte ASCII name rule: [A-Za-z0-9._-], case sensitive, with
 * "." and ".." refused. */
bool yan_note_name_valid(const uint8_t *bytes, uint32_t length)
{
    if (bytes == NULL || length < 1u || length > YAN_NOTE_LINK_NAME_MAX ||
        (uint64_t)length > (uint64_t)(UINTPTR_MAX - (uintptr_t)bytes)) {
        return false;
    }
    if (length == 1u && bytes[0] == (uint8_t)'.') {
        return false;
    }
    if (length == 2u && bytes[0] == (uint8_t)'.' && bytes[1] == (uint8_t)'.') {
        return false;
    }
    for (uint32_t i = 0u; i < length; ++i) {
        uint8_t byte = bytes[i];
        bool allowed = (byte >= (uint8_t)'A' && byte <= (uint8_t)'Z') ||
                       (byte >= (uint8_t)'a' && byte <= (uint8_t)'z') ||
                       (byte >= (uint8_t)'0' && byte <= (uint8_t)'9') ||
                       byte == (uint8_t)'.' || byte == (uint8_t)'_' ||
                       byte == (uint8_t)'-';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

/* --------------------------------------------------------------- parser */

typedef enum {
    NOTE_LINE_END = 0,
    NOTE_LINE_TITLE = 1,
    NOTE_LINE_CREATED = 2,
    NOTE_LINE_MODIFIED = 3,
    NOTE_LINE_TAG = 4,
    NOTE_LINE_LINK = 5,
    NOTE_LINE_UNKNOWN = 6
} NoteLineKind;

static bool key_is(const uint8_t *key, uint32_t key_length, const char *text,
                   uint32_t text_length)
{
    if (key_length != text_length) {
        return false;
    }
    for (uint32_t i = 0u; i < text_length; ++i) {
        if (key[i] != (uint8_t)text[i]) {
            return false;
        }
    }
    return true;
}

/* Classifies one header line. The only legal field syntax is a lower-case key,
 * `: ` and the value, so an empty key, a missing separator or an unknown key
 * is UNKNOWN and becomes METADATA at the call site. */
static NoteLineKind classify_line(const uint8_t *line, uint32_t line_length,
                                  uint32_t *value_offset)
{
    if (line_length == 4u && line[0] == (uint8_t)'@' &&
        line[1] == (uint8_t)'e' && line[2] == (uint8_t)'n' &&
        line[3] == (uint8_t)'d') {
        return NOTE_LINE_END;
    }
    uint32_t separator = UINT32_MAX;
    for (uint32_t i = 0u; i + 1u < line_length; ++i) {
        if (line[i] == (uint8_t)':' && line[i + 1u] == (uint8_t)' ') {
            separator = i;
            break;
        }
    }
    if (separator == 0u || separator == UINT32_MAX) {
        return NOTE_LINE_UNKNOWN; /* empty key, or no canonical separator */
    }
    uint32_t value_start = separator + 2u;
    if (key_is(line, separator, "title", 5u)) {
        *value_offset = value_start;
        return NOTE_LINE_TITLE;
    }
    if (key_is(line, separator, "created", 7u)) {
        *value_offset = value_start;
        return NOTE_LINE_CREATED;
    }
    if (key_is(line, separator, "modified", 8u)) {
        *value_offset = value_start;
        return NOTE_LINE_MODIFIED;
    }
    if (key_is(line, separator, "tag", 3u)) {
        *value_offset = value_start;
        return NOTE_LINE_TAG;
    }
    if (key_is(line, separator, "link", 4u)) {
        *value_offset = value_start;
        return NOTE_LINE_LINK;
    }
    return NOTE_LINE_UNKNOWN;
}

static bool is_end_line(const uint8_t *line, uint32_t line_length)
{
    return line_length == 4u && line[0] == (uint8_t)'@' &&
           line[1] == (uint8_t)'e' && line[2] == (uint8_t)'n' &&
           line[3] == (uint8_t)'d';
}

/* Applies one non-terminator field line to the accumulator. Every failure is
 * METADATA: a broken header is a content error, not an I/O error. */
static YanNoteResult apply_field(YanNoteInfo *accumulator, NoteLineKind kind,
                                 const uint8_t *value, uint32_t value_length)
{
    switch (kind) {
    case NOTE_LINE_TITLE:
        if (accumulator->has_title) {
            return YAN_NOTE_METADATA;
        }
        if (!yan_note_title_valid(value, value_length)) {
            return YAN_NOTE_METADATA;
        }
        accumulator->has_title = true;
        accumulator->title_length = (uint16_t)value_length;
        memcpy(accumulator->title, value, (size_t)value_length);
        accumulator->title[value_length] = 0u;
        return YAN_NOTE_OK;
    case NOTE_LINE_CREATED:
    case NOTE_LINE_MODIFIED: {
        bool created = kind == NOTE_LINE_CREATED;
        bool *flag = created ? &accumulator->has_created
                             : &accumulator->has_modified;
        uint16_t *stored_length = created ? &accumulator->created_length
                                          : &accumulator->modified_length;
        uint8_t *stored = created ? accumulator->created
                                  : accumulator->modified;
        if (*flag) {
            return YAN_NOTE_METADATA;
        }
        if (!yan_note_date_valid(value, value_length)) {
            return YAN_NOTE_METADATA;
        }
        *flag = true;
        *stored_length = (uint16_t)value_length;
        memcpy(stored, value, (size_t)value_length);
        stored[value_length] = 0u;
        return YAN_NOTE_OK;
    }
    case NOTE_LINE_TAG:
        if (accumulator->tag_count >= YAN_NOTE_TAG_MAX_COUNT) {
            return YAN_NOTE_METADATA;
        }
        if (!yan_note_tag_valid(value, value_length)) {
            return YAN_NOTE_METADATA;
        }
        for (uint16_t i = 0u; i < accumulator->tag_count; ++i) {
            if (accumulator->tag_length[i] == (uint16_t)value_length &&
                memcmp(accumulator->tag[i], value, (size_t)value_length) == 0) {
                return YAN_NOTE_METADATA; /* exact byte identity, no folding */
            }
        }
        accumulator->tag_length[accumulator->tag_count] =
            (uint16_t)value_length;
        memcpy(accumulator->tag[accumulator->tag_count], value,
               (size_t)value_length);
        accumulator->tag[accumulator->tag_count][value_length] = 0u;
        accumulator->tag_count = (uint16_t)(accumulator->tag_count + 1u);
        return YAN_NOTE_OK;
    case NOTE_LINE_LINK:
        if (accumulator->link_count >= YAN_NOTE_LINK_MAX_COUNT) {
            return YAN_NOTE_METADATA;
        }
        if (!yan_note_name_valid(value, value_length)) {
            return YAN_NOTE_METADATA;
        }
        for (uint16_t i = 0u; i < accumulator->link_count; ++i) {
            if (accumulator->link_length[i] == (uint16_t)value_length &&
                memcmp(accumulator->link[i], value, (size_t)value_length) == 0) {
                return YAN_NOTE_METADATA;
            }
        }
        accumulator->link_length[accumulator->link_count] =
            (uint16_t)value_length;
        memcpy(accumulator->link[accumulator->link_count], value,
               (size_t)value_length);
        accumulator->link[accumulator->link_count][value_length] = 0u;
        accumulator->link_count = (uint16_t)(accumulator->link_count + 1u);
        return YAN_NOTE_OK;
    default:
        return YAN_NOTE_METADATA;
    }
}

static YanNoteResult parse_header_core(const uint8_t *bytes, uint32_t length,
                                        YanNoteParseWorkspace *workspace,
                                        uint32_t *offset, bool full_result)
{
    if (!prefix_matches(bytes, length)) {
        /* An old no-header file: publish empty metadata, read no byte. */
        *offset = 0u;
        return YAN_NOTE_OK;
    }
    if (!marker_matches(bytes, length)) {
        return YAN_NOTE_METADATA; /* prefix without the exact ` 1\n` marker */
    }

    YanNoteInfo *accumulator = &workspace->info;
    if (full_result) {
        memset(accumulator, 0, sizeof *accumulator);
    } else {
        /* The offset-only path publishes no info. Reset every flag, length
         * and count read by the shared grammar; list values are read only
         * below their freshly built counts. */
        accumulator->has_header = false;
        accumulator->header_bytes = 0u;
        accumulator->has_title = false;
        accumulator->title_length = 0u;
        accumulator->has_created = false;
        accumulator->created_length = 0u;
        accumulator->has_modified = false;
        accumulator->modified_length = 0u;
        accumulator->tag_count = 0u;
        accumulator->link_count = 0u;
    }
    uint32_t position = YAN_NOTE_MARKER_SIZE;
    while (position < length) {
        uint32_t line_start = position;
        uint32_t line_end = length;
        for (uint32_t i = position; i < length; ++i) {
            if (bytes[i] == (uint8_t)'\n') {
                line_end = i;
                break;
            }
        }
        if (line_end == length) {
            /* No closing LF inside the offered prefix: an unterminated or
             * oversized header, never a guess that it ended here. */
            return YAN_NOTE_METADATA;
        }
        uint32_t line_length = line_end - line_start;
        position = line_end + 1u;
        const uint8_t *line = bytes + line_start;
        if (is_end_line(line, line_length)) {
            accumulator->has_header = true;
            accumulator->header_bytes = position;
            *offset = position;
            return YAN_NOTE_OK;
        }
        uint32_t value_offset = 0u;
        NoteLineKind kind = classify_line(line, line_length, &value_offset);
        YanNoteResult result =
            apply_field(accumulator, kind, line + value_offset,
                        line_length - value_offset);
        if (result != YAN_NOTE_OK) {
            return result;
        }
    }
    /* The walk reached the end of a bounded prefix without `@end`. */
    return YAN_NOTE_METADATA;
}

YanNoteResult yan_note_parse_header(const uint8_t *bytes, uint32_t length,
                                    YanNoteInfo *out,
                                    YanNoteParseWorkspace *workspace)
{
    /* Argument and alias checks first: no input byte is read before both spans
     * have been proven addressable and disjoint. */
    if (out == NULL || workspace == NULL) {
        return YAN_NOTE_INVALID;
    }
    if (bytes == NULL && length != 0u) {
        return YAN_NOTE_INVALID;
    }
    if ((uint64_t)length > (uint64_t)(UINTPTR_MAX - (uintptr_t)bytes)) {
        return YAN_NOTE_INVALID;
    }
    if (ranges_overlap(workspace, sizeof *workspace, out, sizeof *out) ||
        ranges_overlap(bytes, (size_t)length, out, sizeof *out) ||
        ranges_overlap(bytes, (size_t)length, workspace, sizeof *workspace)) {
        return YAN_NOTE_INVALID;
    }
    if (length > YAN_NOTE_HEADER_MAX) {
        return YAN_NOTE_TOO_LARGE;
    }

    uint32_t offset = 0u;
    YanNoteResult result = parse_header_core(bytes, length, workspace, &offset, true);
    if (result == YAN_NOTE_OK) {
        if (offset == 0u) {
            memset(out, 0, sizeof *out);
        } else {
            memcpy(out, &workspace->info, sizeof *out);
        }
    }
    return result;
}

YanNoteResult yan_note_body_offset(const uint8_t *bytes, uint32_t length,
                                   uint32_t *offset,
                                   YanNoteParseWorkspace *workspace)
{
    if (offset == NULL || workspace == NULL || (bytes == NULL && length != 0u) ||
        (uint64_t)length > (uint64_t)(UINTPTR_MAX - (uintptr_t)bytes) ||
        ranges_overlap(workspace, sizeof *workspace, offset, sizeof *offset) ||
        ranges_overlap(bytes, length, offset, sizeof *offset) ||
        ranges_overlap(bytes, length, workspace, sizeof *workspace)) {
        return YAN_NOTE_INVALID;
    }
    if (length > YAN_NOTE_HEADER_MAX) {
        return YAN_NOTE_TOO_LARGE;
    }
    uint32_t candidate = 0u;
    YanNoteResult result = parse_header_core(bytes, length, workspace, &candidate, false);
    if (result == YAN_NOTE_OK) {
        *offset = candidate;
    }
    return result;
}

/* -------------------------------------------------------------- encoder */

/* Sum of the canonical header size. The arithmetic is uint32 with a ceiling of
 * 17 + 264 + 20 + 21 + 16*69 + 16*38 = 2034, so it cannot wrap, and
 * the value is known before the first byte is written. */
static uint32_t encoded_total(const YanNoteInfo *info)
{
    uint32_t total = YAN_NOTE_MARKER_SIZE + YAN_NOTE_END_SIZE;
    if (info->has_title) {
        total += 7u + (uint32_t)info->title_length + 1u;
    }
    if (info->has_created) {
        total += 9u + (uint32_t)info->created_length + 1u;
    }
    if (info->has_modified) {
        total += 10u + (uint32_t)info->modified_length + 1u;
    }
    for (uint16_t i = 0u; i < info->tag_count; ++i) {
        total += 5u + (uint32_t)info->tag_length[i] + 1u;
    }
    for (uint16_t i = 0u; i < info->link_count; ++i) {
        total += 6u + (uint32_t)info->link_length[i] + 1u;
    }
    return total;
}

/* Validates that an info is internally complete: a present field carries a
 * legal value, an absent field carries length 0 and no value, and list entries
 * are individually legal and mutually distinct. */
static bool info_is_complete(const YanNoteInfo *info)
{
    if (!info->has_title && info->title_length != 0u) {
        return false;
    }
    if (info->has_title) {
        if (info->title_length == 0u ||
            !yan_note_title_valid(info->title, info->title_length)) {
            return false;
        }
    }
    if (!info->has_created && info->created_length != 0u) {
        return false;
    }
    if (info->has_created) {
        if (info->created_length == 0u ||
            !yan_note_date_valid(info->created, info->created_length)) {
            return false;
        }
    }
    if (!info->has_modified && info->modified_length != 0u) {
        return false;
    }
    if (info->has_modified) {
        if (info->modified_length == 0u ||
            !yan_note_date_valid(info->modified, info->modified_length)) {
            return false;
        }
    }
    if (info->tag_count > YAN_NOTE_TAG_MAX_COUNT ||
        info->link_count > YAN_NOTE_LINK_MAX_COUNT) {
        return false;
    }
    for (uint16_t i = 0u; i < info->tag_count; ++i) {
        if (!yan_note_tag_valid(info->tag[i], info->tag_length[i])) {
            return false;
        }
        for (uint16_t j = 0u; j < i; ++j) {
            if (info->tag_length[i] == info->tag_length[j] &&
                memcmp(info->tag[i], info->tag[j],
                       (size_t)info->tag_length[i]) == 0) {
                return false;
            }
        }
    }
    for (uint16_t i = 0u; i < info->link_count; ++i) {
        if (!yan_note_name_valid(info->link[i], info->link_length[i])) {
            return false;
        }
        for (uint16_t j = 0u; j < i; ++j) {
            if (info->link_length[i] == info->link_length[j] &&
                memcmp(info->link[i], info->link[j],
                       (size_t)info->link_length[i]) == 0) {
                return false;
            }
        }
    }
    return true;
}

static uint32_t emit_literal(uint8_t *cursor, const char *text,
                             uint32_t text_length)
{
    for (uint32_t i = 0u; i < text_length; ++i) {
        cursor[i] = (uint8_t)text[i];
    }
    return text_length;
}

static uint32_t emit_bytes(uint8_t *cursor, const uint8_t *bytes,
                           uint32_t length)
{
    memcpy(cursor, bytes, (size_t)length);
    return length;
}

YanNoteResult yan_note_encode_header(const YanNoteInfo *info, uint8_t *out,
                                     uint32_t capacity, uint32_t *length_out)
{
    if (info == NULL || out == NULL || length_out == NULL) {
        return YAN_NOTE_INVALID;
    }
    if (ranges_overlap(out, (size_t)capacity, length_out, sizeof *length_out) ||
        ranges_overlap(out, (size_t)capacity, info, sizeof *info) ||
        ranges_overlap(length_out, sizeof *length_out, info, sizeof *info)) {
        return YAN_NOTE_INVALID;
    }
    if (!info_is_complete(info)) {
        return YAN_NOTE_INVALID;
    }
    uint32_t total = encoded_total(info);
    if (total > YAN_NOTE_HEADER_MAX || capacity < total) {
        return YAN_NOTE_TOO_LARGE;
    }

    /* The whole size is known before the first write, so an error can never
     * leave a partial or padded buffer behind. */
    uint8_t *cursor = out;
    cursor += emit_bytes(cursor, yan_note_marker, YAN_NOTE_MARKER_SIZE);
    if (info->has_title) {
        cursor += emit_literal(cursor, "title: ", 7u);
        cursor += emit_bytes(cursor, info->title, (uint32_t)info->title_length);
        *cursor = (uint8_t)'\n';
        ++cursor;
    }
    if (info->has_created) {
        cursor += emit_literal(cursor, "created: ", 9u);
        cursor +=
            emit_bytes(cursor, info->created, (uint32_t)info->created_length);
        *cursor = (uint8_t)'\n';
        ++cursor;
    }
    if (info->has_modified) {
        cursor += emit_literal(cursor, "modified: ", 10u);
        cursor +=
            emit_bytes(cursor, info->modified, (uint32_t)info->modified_length);
        *cursor = (uint8_t)'\n';
        ++cursor;
    }
    for (uint16_t i = 0u; i < info->tag_count; ++i) {
        cursor += emit_literal(cursor, "tag: ", 5u);
        cursor +=
            emit_bytes(cursor, info->tag[i], (uint32_t)info->tag_length[i]);
        *cursor = (uint8_t)'\n';
        ++cursor;
    }
    for (uint16_t i = 0u; i < info->link_count; ++i) {
        cursor += emit_literal(cursor, "link: ", 6u);
        cursor +=
            emit_bytes(cursor, info->link[i], (uint32_t)info->link_length[i]);
        *cursor = (uint8_t)'\n';
        ++cursor;
    }
    cursor += emit_literal(cursor, "@end\n", 5u);
    *length_out = total;
    return YAN_NOTE_OK;
}

const char *yan_note_result_name(YanNoteResult result)
{
    switch (result) {
    case YAN_NOTE_OK: return "OK";
    case YAN_NOTE_INVALID: return "INVALID";
    case YAN_NOTE_BUSY: return "BUSY";
    case YAN_NOTE_STOPPED: return "STOPPED";
    case YAN_NOTE_METADATA: return "METADATA";
    case YAN_NOTE_TOO_LARGE: return "TOO_LARGE";
    case YAN_NOTE_INVALID_TEXT: return "INVALID_TEXT";
    case YAN_NOTE_NOT_MOUNTED: return "NOT_MOUNTED";
    case YAN_NOTE_FAULTED: return "FAULTED";
    case YAN_NOTE_NOT_FOUND: return "NOT_FOUND";
    case YAN_NOTE_EXISTS: return "EXISTS";
    case YAN_NOTE_NOSPACE: return "NOSPACE";
    case YAN_NOTE_CORRUPT: return "CORRUPT";
    case YAN_NOTE_DIRECTORY_FULL: return "DIRECTORY_FULL";
    case YAN_NOTE_IO: return "IO";
    case YAN_NOTE_PROTOCOL: return "PROTOCOL";
    case YAN_NOTE_UNSUPPORTED: return "UNSUPPORTED";
    }
    return "UNKNOWN";
}
