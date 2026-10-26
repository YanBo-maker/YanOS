/* 0032 pure grammar. The text pass and the structural pass are deliberately
 * separate: a late binary byte must outrank any earlier apparent block syntax.
 * The service completes qualification before it calls this body scanner.
 * Bounded line prefixes recognize markers without buffering long body lines;
 * one ASCII candidate recognizes references without retaining an occurrence
 * list or imposing an artificial 256-reference limit. */
#include "note_blocks_format.h"

#include "memory.h"
#include "note_metadata_format.h"
#include "search_text.h"

#define BLOCK_TEXT_READY UINT32_C(0x424c5458)
#define BLOCK_PARSER_READY UINT32_C(0x424c5052)

static bool span_valid(const void *pointer, uint64_t length)
{
    return (pointer != NULL || length == 0u) &&
           length <= (uint64_t)(UINTPTR_MAX - (uintptr_t)pointer);
}

static bool overlap(const void *left, uint64_t left_length,
                    const void *right, uint64_t right_length)
{
    if (left_length == 0u || right_length == 0u) {
        return false;
    }
    uintptr_t left_begin = (uintptr_t)left;
    uintptr_t right_begin = (uintptr_t)right;
    return left_begin < right_begin + (uintptr_t)right_length &&
           right_begin < left_begin + (uintptr_t)left_length;
}

static bool id_byte(uint8_t byte)
{
    return yan_search_text_is_ascii_word(byte) || byte == (uint8_t)'-';
}

bool yan_block_id_valid(const uint8_t *bytes, uint32_t length)
{
    if (length == 0u || length > YAN_BLOCK_ID_MAX ||
        !span_valid(bytes, length)) {
        return false;
    }
    for (uint32_t i = 0u; i < length; ++i) {
        if (!id_byte(bytes[i])) {
            return false;
        }
    }
    return true;
}

YanBlockResult yan_block_address_parse(const uint8_t *bytes, uint32_t length,
                                       YanBlockAddress *out)
{
    if (!span_valid(out, sizeof *out) || !span_valid(bytes, length) ||
        overlap(bytes, length, out, sizeof *out) ||
        length < 3u || length > 63u) {
        return YAN_BLOCK_INVALID;
    }
    uint32_t separator = length;
    for (uint32_t i = 0u; i < length; ++i) {
        if (bytes[i] == (uint8_t)'#') {
            if (separator != length) {
                return YAN_BLOCK_INVALID;
            }
            separator = i;
        }
    }
    if (separator == length ||
        !yan_note_name_valid(bytes, separator) ||
        !yan_block_id_valid(bytes + separator + 1u, length - separator - 1u)) {
        return YAN_BLOCK_INVALID;
    }
    memset(out, 0, sizeof *out);
    memcpy(out->name, bytes, separator);
    memcpy(out->id, bytes + separator + 1u, length - separator - 1u);
    return YAN_BLOCK_OK;
}

const char *yan_block_result_name(YanBlockResult result)
{
    switch (result) {
    case YAN_BLOCK_OK: return "OK";
    case YAN_BLOCK_INVALID: return "INVALID";
    case YAN_BLOCK_BUSY: return "BUSY";
    case YAN_BLOCK_STOPPED: return "STOPPED";
    case YAN_BLOCK_METADATA: return "METADATA";
    case YAN_BLOCK_FORMAT: return "BLOCK_FORMAT";
    case YAN_BLOCK_AMBIGUOUS: return "AMBIGUOUS";
    case YAN_BLOCK_LIMIT: return "LIMIT";
    case YAN_BLOCK_INVALID_TEXT: return "INVALID_TEXT";
    case YAN_BLOCK_NOT_MOUNTED: return "NOT_MOUNTED";
    case YAN_BLOCK_FAULTED: return "FAULTED";
    case YAN_BLOCK_NOT_FOUND: return "NOT_FOUND";
    case YAN_BLOCK_IO: return "IO";
    case YAN_BLOCK_PROTOCOL: return "PROTOCOL";
    case YAN_BLOCK_CORRUPT: return "CORRUPT";
    case YAN_BLOCK_UNSUPPORTED: return "UNSUPPORTED";
    default: return "INVALID";
    }
}

YanBlockResult yan_block_text_init(YanBlockTextValidator *validator)
{
    if (!span_valid(validator, sizeof *validator)) {
        return YAN_BLOCK_INVALID;
    }
    memset(validator, 0, sizeof *validator);
    validator->initialized = BLOCK_TEXT_READY;
    validator->result = YAN_BLOCK_OK;
    return YAN_BLOCK_OK;
}

static YanBlockResult text_state(const YanBlockTextValidator *validator)
{
    if (!span_valid(validator, sizeof *validator) ||
        validator->initialized != BLOCK_TEXT_READY) {
        return YAN_BLOCK_INVALID;
    }
    if (validator->result != YAN_BLOCK_OK) {
        return validator->result;
    }
    return validator->finished ? YAN_BLOCK_INVALID : YAN_BLOCK_OK;
}

YanBlockResult yan_block_text_feed(YanBlockTextValidator *validator,
                                   const uint8_t *bytes, uint32_t length)
{
    YanBlockResult state = text_state(validator);
    if (state != YAN_BLOCK_OK) {
        return state;
    }
    if (!span_valid(bytes, length) ||
        overlap(bytes, length, validator, sizeof *validator)) {
        return YAN_BLOCK_INVALID;
    }
    for (uint32_t i = 0u; i < length; ++i) {
        uint8_t byte = bytes[i];
        if (validator->pending_cr) {
            if (byte != (uint8_t)'\n') {
                validator->result = YAN_BLOCK_INVALID_TEXT;
                break;
            }
            validator->pending_cr = false;
            continue;
        }
        if (validator->utf8_length == 0u) {
            /* No CR or partial scalar is pending. Consume only this run of
             * legal ASCII; the first other byte still takes the old path. */
            while ((byte >= 0x20u && byte < 0x7fu) ||
                   byte == (uint8_t)'\t' || byte == (uint8_t)'\n') {
                if (++i == length) {
                    return validator->result;
                }
                byte = bytes[i];
            }
        }
        if (validator->utf8_length == 0u && byte == (uint8_t)'\r') {
            validator->pending_cr = true;
            continue;
        }
        validator->utf8[validator->utf8_length++] = byte;
        uint32_t need = yan_search_text_utf8_expected(validator->utf8,
                                                      validator->utf8_length);
        if (need == 0u) {
            validator->result = YAN_BLOCK_INVALID_TEXT;
            break;
        }
        if (validator->utf8_length == need) {
            uint32_t scalar = 0u;
            (void)yan_search_text_utf8_scalar(validator->utf8, need, &scalar);
            if ((scalar < 0x20u && scalar != '\t' && scalar != '\n') ||
                (scalar >= 0x7fu && scalar <= 0x9fu)) {
                validator->result = YAN_BLOCK_INVALID_TEXT;
                break;
            }
            validator->utf8_length = 0u;
        }
    }
    return validator->result;
}

YanBlockResult yan_block_text_finish(YanBlockTextValidator *validator)
{
    YanBlockResult state = text_state(validator);
    if (state != YAN_BLOCK_OK) {
        return state;
    }
    if (validator->utf8_length != 0u || validator->pending_cr) {
        validator->result = YAN_BLOCK_INVALID_TEXT;
    } else {
        validator->finished = true;
    }
    return validator->result;
}

YanBlockResult yan_block_parser_init(YanBlockParser *parser,
                                     uint32_t body_offset,
                                     uint32_t first_line,
                                     YanBlockReferenceSink sink,
                                     void *sink_context)
{
    if (!span_valid(parser, sizeof *parser)) {
        return YAN_BLOCK_INVALID;
    }
    if (parser->initialized == BLOCK_PARSER_READY && parser->busy) {
        return YAN_BLOCK_BUSY;
    }
    if (first_line == 0u) {
        return YAN_BLOCK_INVALID;
    }
    /* Only the count-bounded active prefix is a result. Retain unused slots;
     * each new declaration is fully initialized before increasing the count. */
    memset(&parser->block_count, 0,
           sizeof *parser - offsetof(YanBlockParser, block_count));
    parser->initialized = BLOCK_PARSER_READY;
    parser->result = YAN_BLOCK_OK;
    parser->offset = body_offset;
    parser->line_start = body_offset;
    parser->line = first_line;
    parser->ref_sink = sink;
    parser->ref_context = sink_context;
    return YAN_BLOCK_OK;
}

static YanBlockResult parser_state(const YanBlockParser *parser)
{
    if (!span_valid(parser, sizeof *parser) ||
        parser->initialized != BLOCK_PARSER_READY) {
        return YAN_BLOCK_INVALID;
    }
    if (parser->busy) {
        return YAN_BLOCK_BUSY;
    }
    if (parser->result != YAN_BLOCK_OK) {
        return parser->result;
    }
    return parser->finished ? YAN_BLOCK_INVALID : YAN_BLOCK_OK;
}

static void close_block(YanBlockParser *parser, uint32_t end,
                         uint64_t last_line)
{
    if (parser->block_count == 0u) {
        return;
    }
    YanBlockDecl *block = &parser->blocks[parser->block_count - 1u];
    if (block->start != end &&
        ((uint64_t)block->marker_line + 1u > UINT32_MAX ||
         last_line > UINT32_MAX)) {
        parser->result = YAN_BLOCK_INVALID;
        return;
    }
    block->end = end;
    block->first_line = block->start == end ? 0u : block->marker_line + 1u;
    block->last_line = block->start == end ? 0u : (uint32_t)last_line;
}

/* Only a fixed prefix is retained. A 100 KiB ordinary line has the same
 * memory cost as a one-byte line; a reserved marker beyond 38 bytes is bad. */
static void classify_line(YanBlockParser *parser, uint32_t after_eol)
{
    static const uint8_t marker[6] = {'@', 'b', 'l', 'o', 'c', 'k'};
    uint32_t length = parser->offset - parser->line_start;
    if (length != 0u && parser->last_byte == (uint8_t)'\r') {
        --length;
    }
    if (length < sizeof marker ||
        memcmp(parser->line_prefix, marker, sizeof marker) != 0 ||
        (length > sizeof marker && parser->line_prefix[6] != (uint8_t)' ' &&
         parser->line_prefix[6] != (uint8_t)'\t')) {
        return;
    }
    if (length < 8u || length > 38u ||
        parser->line_prefix[6] != (uint8_t)' ' ||
        !yan_block_id_valid(parser->line_prefix + 7u, length - 7u)) {
        parser->result = YAN_BLOCK_FORMAT;
        return;
    }
    uint32_t id_length = length - 7u;
    for (uint32_t i = 0u; i < parser->block_count; ++i) {
        if (parser->blocks[i].id[id_length] == '\0' &&
            memcmp(parser->blocks[i].id, parser->line_prefix + 7u,
                   id_length) == 0) {
            parser->result = YAN_BLOCK_AMBIGUOUS;
            return;
        }
    }
    if (parser->block_count == YAN_BLOCK_MAX_COUNT) {
        parser->result = YAN_BLOCK_LIMIT;
        return;
    }
    if (parser->line > UINT32_MAX) {
        parser->result = YAN_BLOCK_INVALID;
        return;
    }
    close_block(parser, parser->line_start, parser->line - 1u);
    if (parser->result != YAN_BLOCK_OK) {
        return;
    }
    YanBlockDecl *block = &parser->blocks[parser->block_count];
    memset(block, 0, sizeof *block);
    memcpy(block->id, parser->line_prefix + 7u, id_length);
    block->id[id_length] = '\0';
    block->marker_line = (uint32_t)parser->line;
    block->start = after_eol;
    block->end = after_eol;
    ++parser->block_count;
}

enum { REF_SEEK, REF_OPEN, REF_NAME, REF_ID, REF_CLOSE };

/* Invalid starts advance one byte rather than swallowing their contents.
 * Retaining the previous '[' is necessary for [[[a#b]]: the legal reference
 * begins at the second '[', even though the outer candidate was malformed. */
static void reference_restart(YanBlockParser *parser, uint8_t byte)
{
    parser->ref_state = REF_SEEK;
    if (byte != (uint8_t)'[') {
        return;
    }
    parser->reference.start = parser->offset;
    parser->ref_state = REF_OPEN;
    if (parser->last_byte == (uint8_t)'[') {
        parser->reference.start = parser->offset - 1u;
        parser->ref_state = REF_NAME;
    }
    parser->name_length = 0u;
    parser->id_length = 0u;
}

static bool name_byte(uint8_t byte)
{
    return id_byte(byte) || byte == (uint8_t)'.';
}

static void reference_byte(YanBlockParser *parser, uint8_t byte)
{
    switch (parser->ref_state) {
    case REF_SEEK:
        if (byte == (uint8_t)'[') {
            parser->reference.start = parser->offset;
            parser->ref_state = REF_OPEN;
        }
        break;
    case REF_OPEN:
        if (byte == (uint8_t)'[') {
            parser->name_length = 0u;
            parser->id_length = 0u;
            parser->ref_state = REF_NAME;
        } else {
            reference_restart(parser, byte);
        }
        break;
    case REF_NAME:
        if (name_byte(byte) && parser->name_length < YAN_BLOCK_NAME_MAX) {
            parser->reference.target.name[parser->name_length++] = (char)byte;
        } else if (byte == (uint8_t)'#' &&
                   yan_note_name_valid((const uint8_t *)parser->reference.target.name,
                                       parser->name_length)) {
            parser->reference.target.name[parser->name_length] = '\0';
            parser->ref_state = REF_ID;
        } else {
            reference_restart(parser, byte);
        }
        break;
    case REF_ID:
        if (id_byte(byte) && parser->id_length < YAN_BLOCK_ID_MAX) {
            parser->reference.target.id[parser->id_length++] = (char)byte;
        } else if (byte == (uint8_t)']' && parser->id_length != 0u) {
            parser->reference.target.id[parser->id_length] = '\0';
            parser->ref_state = REF_CLOSE;
        } else {
            reference_restart(parser, byte);
        }
        break;
    case REF_CLOSE:
        if (byte == (uint8_t)']') {
            parser->ref_state = REF_SEEK;
            if (parser->line > UINT32_MAX) {
                parser->result = YAN_BLOCK_INVALID;
                return;
            }
            parser->reference.line = (uint32_t)parser->line;
            parser->reference.end = parser->offset + 1u;
            parser->reference.has_source_id = parser->block_count != 0u;
            memset(parser->reference.source_id, 0,
                   sizeof parser->reference.source_id);
            if (parser->reference.has_source_id) {
                memcpy(parser->reference.source_id,
                       parser->blocks[parser->block_count - 1u].id, 32u);
            }
            if (parser->ref_sink != NULL &&
                !parser->ref_sink(parser->ref_context, &parser->reference)) {
                parser->result = YAN_BLOCK_STOPPED;
            }
        } else {
            reference_restart(parser, byte);
        }
        break;
    default:
        parser->result = YAN_BLOCK_INVALID;
        break;
    }
}

YanBlockResult yan_block_parser_feed(YanBlockParser *parser,
                                     const uint8_t *bytes, uint32_t length)
{
    YanBlockResult state = parser_state(parser);
    if (state != YAN_BLOCK_OK) {
        return state;
    }
    if (!span_valid(bytes, length) ||
        overlap(bytes, length, parser, sizeof *parser) ||
        length > UINT32_MAX - parser->offset) {
        return YAN_BLOCK_INVALID;
    }
    parser->busy = true;
    for (uint32_t i = 0u; i < length; ++i) {
        uint8_t byte = bytes[i];
        if (byte == (uint8_t)'\n') {
            classify_line(parser, parser->offset + 1u);
            if (parser->result != YAN_BLOCK_OK) {
                break;
            }
            ++parser->line;
            parser->line_start = parser->offset + 1u;
            parser->prefix_length = 0u;
            parser->ref_state = REF_SEEK;
        } else {
            if (parser->prefix_length < sizeof parser->line_prefix) {
                parser->line_prefix[parser->prefix_length++] = byte;
            }
            reference_byte(parser, byte);
        }
        parser->last_byte = byte;
        ++parser->offset;
        if (parser->result != YAN_BLOCK_OK) {
            break;
        }
    }
    parser->busy = false;
    return parser->result;
}

YanBlockResult yan_block_parser_finish(YanBlockParser *parser)
{
    YanBlockResult state = parser_state(parser);
    if (state != YAN_BLOCK_OK) {
        return state;
    }
    parser->busy = true;
    bool has_final_line = parser->offset != parser->line_start;
    if (has_final_line) {
        classify_line(parser, parser->offset);
    }
    if (parser->result == YAN_BLOCK_OK) {
        close_block(parser, parser->offset,
                    has_final_line ? parser->line : parser->line - 1u);
    }
    if (parser->result == YAN_BLOCK_OK) {
        parser->finished = true;
    }
    parser->busy = false;
    return parser->result;
}
