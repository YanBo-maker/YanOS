#include "note_blocks.h"
#include "memory.h"
#include "sort.h"
#include <stddef.h>

/* All names and IDs have the same fixed, ASCII, NUL-terminated 32-byte shape.
 * Keep hosted string routines out of the freestanding RV32 service. */
static int name_compare(const char left[32], const char right[32])
{
    for (uint32_t i = 0; i < 32u; ++i) {
        uint8_t a = (uint8_t)left[i], b = (uint8_t)right[i];
        if (a != b) return a < b ? -1 : 1;
        if (a == 0u) return 0;
    }
    return 0;
}

/* 0036-R8: order the directory indices by the borrowed name table. */
static bool block_name_before(const void *context, uint16_t left, uint16_t right)
{
    const YanBlockContext *blocks = context;
    return name_compare(blocks->names[left], blocks->names[right]) < 0;
}

static bool span(const void *p, size_t n)
{ return (p != NULL || n == 0u) && n <= UINTPTR_MAX - (uintptr_t)p; }
static bool overlap(const void *a, size_t an, const void *b, size_t bn)
{
    return an != 0u && bn != 0u && (uintptr_t)a < (uintptr_t)b + bn &&
           (uintptr_t)b < (uintptr_t)a + an;
}
static bool protected_span(const YanBlockContext *c, const void *p, size_t n)
{
    return !span(p, n) || overlap(p, n, c, sizeof *c) ||
           overlap(p, n, c->fs, sizeof *c->fs);
}
static YanBlockResult map_fs(YanFsResult r)
{
    switch (r) {
    case YAN_FS_OK: return YAN_BLOCK_OK;
    case YAN_FS_INVALID: return YAN_BLOCK_INVALID;
    case YAN_FS_BUSY: return YAN_BLOCK_BUSY;
    case YAN_FS_NOT_MOUNTED: return YAN_BLOCK_NOT_MOUNTED;
    case YAN_FS_FAULTED: return YAN_BLOCK_FAULTED;
    case YAN_FS_NOT_FOUND: return YAN_BLOCK_NOT_FOUND;
    case YAN_FS_IO: return YAN_BLOCK_IO;
    case YAN_FS_PROTOCOL: return YAN_BLOCK_PROTOCOL;
    case YAN_FS_CORRUPT: return YAN_BLOCK_CORRUPT;
    case YAN_FS_UNSUPPORTED: return YAN_BLOCK_UNSUPPORTED;
    /* END belongs only to iteration; write outcomes cannot mean read success. */
    case YAN_FS_END: case YAN_FS_EXISTS: case YAN_FS_NOSPACE:
    case YAN_FS_DIRECTORY_FULL: return YAN_BLOCK_PROTOCOL;
    }
    return YAN_BLOCK_PROTOCOL;
}
static YanBlockResult fs_state(const YanFs *fs)
{
    if (!fs->initialized) return YAN_BLOCK_INVALID;
    if (fs->busy) return YAN_BLOCK_BUSY;
    if (fs->state == YAN_FS_STATE_FAULTED) return YAN_BLOCK_FAULTED;
    if (fs->state != YAN_FS_MOUNTED) return YAN_BLOCK_NOT_MOUNTED;
    return YAN_BLOCK_OK;
}
uint32_t yan_blocks_names_storage_bytes(uint32_t capacity)
{ return capacity * 32u; }
uint32_t yan_blocks_sort_index_storage_bytes(uint32_t capacity)
{ return capacity * (uint32_t)sizeof(uint16_t); }

YanBlockResult yan_blocks_init(YanBlockContext *c, YanFs *fs,
                               char (*names)[32], uint16_t *sort_index)
{
    if (!span(c, sizeof *c) || !span(fs, sizeof *fs) || names == NULL ||
        sort_index == NULL ||
        overlap(c, sizeof *c, fs, sizeof *fs)) return YAN_BLOCK_INVALID;
    if (c->busy) return YAN_BLOCK_BUSY;
    /* 0036-R3: init runs before mount; a query still requires MOUNTED. */
    if (!fs->initialized) return YAN_BLOCK_INVALID;
    if (fs->busy) return YAN_BLOCK_BUSY;
    if (fs->state == YAN_FS_STATE_FAULTED) return YAN_BLOCK_FAULTED;
    if (fs->entry_capacity == 0u) return YAN_BLOCK_INVALID;
    c->fs = fs; c->names = names; c->sort_index = sort_index;
    c->name_capacity = fs->entry_capacity;
    c->initialized = true; c->busy = false;
    return YAN_BLOCK_OK;
}
static bool address_valid(const YanBlockAddress *a)
{
    uint32_t nn = 0, ni = 0;
    while (nn < 32u && a->name[nn] != '\0') ++nn;
    while (ni < 32u && a->id[ni] != '\0') ++ni;
    return nn < 32u && ni < 32u &&
        yan_note_name_valid((const uint8_t *)a->name, nn) &&
        yan_block_id_valid((const uint8_t *)a->id, ni);
}
/* Names include their NUL. Check each address before a load, and copy inputs
 * before modifying the context. The caller owns all other object lifetimes. */
static YanBlockResult guard(YanBlockContext *c, const void *input,
                            bool is_address, YanBlockRowFn sink, void *opaque,
                            YanBlockSummary *summary, YanBlockAddress *key)
{
    if (!span(c, sizeof *c)) return YAN_BLOCK_INVALID;
    if (!c->initialized) return YAN_BLOCK_INVALID;
    if (c->busy) return YAN_BLOCK_BUSY;
    if (!span(c->fs, sizeof *c->fs) || overlap(c, sizeof *c, c->fs, sizeof *c->fs))
        return YAN_BLOCK_INVALID;
    YanBlockResult r = fs_state(c->fs);
    if (r != YAN_BLOCK_OK) return r;
    size_t sn = summary != NULL ? sizeof *summary : 0u;
    if (sink == NULL || protected_span(c, summary, sn) ||
        (opaque != NULL && protected_span(c, opaque, 1u))) return YAN_BLOCK_INVALID;
    if (is_address) {
        if (protected_span(c, input, sizeof *key) || overlap(input, sizeof *key, summary, sn))
            return YAN_BLOCK_INVALID;
        const YanBlockAddress *a = input;
        if (!address_valid(a)) return YAN_BLOCK_INVALID;
        *key = *a;
    } else {
        const char *name = input;
        uint32_t n;
        for (n = 0; n < 32u; ++n) {
            if ((uintptr_t)name > UINTPTR_MAX - n) return YAN_BLOCK_INVALID;
            const void *at = (const void *)((uintptr_t)name + n);
            if (protected_span(c, at, 1u) || overlap(at, 1u, summary, sn)) return YAN_BLOCK_INVALID;
            key->name[n] = name[n];
            if (name[n] == '\0') break;
        }
        if (n == 32u || !yan_note_name_valid((const uint8_t *)key->name, n)) return YAN_BLOCK_INVALID;
    }
    return YAN_BLOCK_OK;
}
static void begin(YanBlockContext *c)
{
    c->busy = true; c->row_count = 0; c->total = 0;
    c->skipped = 0; c->filtering = false;
}
static YanBlockResult finish(YanBlockContext *c, YanBlockResult r,
                             YanBlockSummary *summary, YanBlockTargetState state)
{
    if (r == YAN_BLOCK_OK && summary != NULL) {
        YanBlockSummary value;
        memset(&value, 0, sizeof value);
        value.total = c->total; value.shown = c->row_count;
        value.skipped = c->skipped; value.target_state = state;
        *summary = value;
    }
    c->busy = false;
    return r;
}
static YanBlockResult read_exact(YanBlockContext *c, const char *name,
                                 uint32_t offset, uint32_t length)
{
    uint32_t got = 0;
    YanBlockResult r = map_fs(yan_fs_read(c->fs, name, offset, c->buffer, length, &got));
    if (r != YAN_BLOCK_OK) return r;
    return got == length ? YAN_BLOCK_OK : YAN_BLOCK_PROTOCOL;
}
/* Complete eligibility before interpreting syntax: late NUL/invalid UTF-8
 * cannot publish an earlier reference, even if a binary prefix looks textual.
 * Each I/O request consumes a bounded chunk, never one whole block per byte. */
static YanBlockResult file_scan(YanBlockContext *c, const char *name,
                                YanBlockReferenceSink sink)
{
    YanFsInfo file;
    YanBlockResult r = map_fs(yan_fs_stat(c->fs, name, &file));
    if (r != YAN_BLOCK_OK) return r;
    bool buffered = file.size_bytes <= sizeof c->buffer;
    r = yan_block_text_init(&c->text);
    for (uint32_t offset = 0; r == YAN_BLOCK_OK && offset < file.size_bytes;) {
        uint32_t amount = file.size_bytes - offset;
        if (amount > sizeof c->buffer) amount = sizeof c->buffer;
        r = read_exact(c, name, offset, amount);
        if (r == YAN_BLOCK_OK) r = yan_block_text_feed(&c->text, c->buffer, amount);
        offset += amount;
    }
    if (r == YAN_BLOCK_OK) r = yan_block_text_finish(&c->text);
    if (r != YAN_BLOCK_OK) return r;
    uint32_t prefix = file.size_bytes;
    if (prefix > sizeof c->buffer) prefix = sizeof c->buffer;
    /* A complete small file remains in the existing buffer after eligibility
     * validation. The header parser borrows it without modifying its bytes. */
    if (!buffered) r = read_exact(c, name, 0, prefix);
    if (r != YAN_BLOCK_OK) return r;
    YanNoteResult note = yan_note_parse_header(c->buffer, prefix, &c->metadata, &c->metadata_workspace);
    if (note != YAN_NOTE_OK) return note == YAN_NOTE_METADATA ? YAN_BLOCK_METADATA : YAN_BLOCK_PROTOCOL;
    uint32_t first_line = 1;
    for (uint32_t i = 0; i < c->metadata.header_bytes; ++i) if (c->buffer[i] == '\n') ++first_line;
    r = yan_block_parser_init(&c->parser, c->metadata.header_bytes, first_line, sink, sink != NULL ? c : NULL);
    if (buffered && r == YAN_BLOCK_OK) {
        r = yan_block_parser_feed(&c->parser,
            c->buffer + c->metadata.header_bytes,
            file.size_bytes - c->metadata.header_bytes);
    }
    for (uint32_t offset = c->metadata.header_bytes;
         !buffered && r == YAN_BLOCK_OK && offset < file.size_bytes;) {
        uint32_t amount = file.size_bytes - offset;
        if (amount > sizeof c->buffer) amount = sizeof c->buffer;
        r = read_exact(c, name, offset, amount);
        if (r == YAN_BLOCK_OK) r = yan_block_parser_feed(&c->parser, c->buffer, amount);
        offset += amount;
    }
    if (r == YAN_BLOCK_OK) r = yan_block_parser_finish(&c->parser);
    return r;
}
static bool collect(void *opaque, const YanBlockReference *reference)
{
    YanBlockContext *c = opaque;
    if (c->filtering && (name_compare(c->filter.name, reference->target.name) != 0 ||
                         name_compare(c->filter.id, reference->target.id) != 0)) return true;
    ++c->total;
    if (c->row_count < YAN_BLOCK_REFS_SHOWN) {
        YanBlockRow *row = &c->rows[c->row_count++];
        memset(row, 0, sizeof *row);
        memcpy(row->source, c->current_file, sizeof row->source);
        row->reference = *reference;
    }
    return true; /* cap storage, never parsing or the full occurrence count */
}
static YanBlockResult target(YanBlockContext *c, const YanBlockAddress *a, YanBlockTargetState *state)
{
    YanBlockResult r = file_scan(c, a->name, NULL);
    if (r == YAN_BLOCK_NOT_FOUND) { *state = YAN_BLOCK_DANGLING_FILE; return YAN_BLOCK_OK; }
    if (r != YAN_BLOCK_OK) return r;
    *state = YAN_BLOCK_DANGLING_BLOCK;
    for (uint32_t i = 0; i < c->parser.block_count; ++i)
        if (name_compare(c->parser.blocks[i].id, a->id) == 0) { *state = YAN_BLOCK_TARGET_OK; break; }
    return YAN_BLOCK_OK;
}
static YanBlockResult emit(YanBlockContext *c, YanBlockRowFn sink, void *opaque)
{
    for (uint32_t i = 0; i < c->row_count; ++i) if (!sink(opaque, &c->rows[i])) return YAN_BLOCK_STOPPED;
    return YAN_BLOCK_OK;
}
YanBlockResult yan_blocks_list(YanBlockContext *c, const char *name, YanBlockRowFn sink,
                               void *opaque, YanBlockSummary *summary)
{
    YanBlockAddress key = {{0}, {0}};
    YanBlockResult r = guard(c, name, false, sink, opaque, summary, &key);
    if (r != YAN_BLOCK_OK) return r;
    begin(c);
    r = file_scan(c, key.name, NULL);
    if (r == YAN_BLOCK_OK) {
        c->total = c->row_count = c->parser.block_count;
        for (uint32_t i = 0; i < c->row_count; ++i) {
            memset(&c->rows[i], 0, sizeof c->rows[i]);
            memcpy(c->rows[i].source, key.name, sizeof key.name);
            c->rows[i].block = c->parser.blocks[i];
        }
        r = emit(c, sink, opaque);
    }
    return finish(c, r, summary, YAN_BLOCK_TARGET_OK);
}
YanBlockResult yan_blocks_refs(YanBlockContext *c, const char *name, YanBlockRowFn sink,
                               void *opaque, YanBlockSummary *summary)
{
    YanBlockAddress key = {{0}, {0}};
    YanBlockResult r = guard(c, name, false, sink, opaque, summary, &key);
    if (r != YAN_BLOCK_OK) return r;
    begin(c);
    memcpy(c->current_file, key.name, sizeof c->current_file);
    r = file_scan(c, key.name, collect);
    for (uint32_t i = 0; r == YAN_BLOCK_OK && i < c->row_count; ++i)
        r = target(c, &c->rows[i].reference.target, &c->rows[i].state);
    if (r == YAN_BLOCK_OK) r = emit(c, sink, opaque);
    return finish(c, r, summary, YAN_BLOCK_TARGET_OK);
}
static YanBlockResult directory(YanBlockContext *c)
{
    uint32_t cursor = 0;
    c->name_count = 0;
    for (;;) {
        YanFsInfo file;
        YanFsResult r = yan_fs_list(c->fs, &cursor, &file);
        if (r == YAN_FS_END) break;
        if (r != YAN_FS_OK) return map_fs(r);
        if (c->name_count == c->name_capacity) return YAN_BLOCK_PROTOCOL;
        memcpy(c->names[c->name_count++], file.name, sizeof file.name);
    }
    for (uint32_t i = 0; i < c->name_count; ++i) c->sort_index[i] = (uint16_t)i;
    yan_sort_index(c->sort_index, c->name_count, block_name_before, c);
    return YAN_BLOCK_OK;
}
YanBlockResult yan_blocks_backlinks(YanBlockContext *c, const YanBlockAddress *a, YanBlockRowFn sink,
                                    void *opaque, YanBlockSummary *summary)
{
    YanBlockAddress key = {{0}, {0}};
    YanBlockResult r = guard(c, a, true, sink, opaque, summary, &key);
    if (r != YAN_BLOCK_OK) return r;
    begin(c);
    YanBlockTargetState state = YAN_BLOCK_TARGET_OK;
    r = target(c, &key, &state);
    if (r == YAN_BLOCK_OK) r = directory(c);
    c->filtering = true; c->filter = key;
    for (uint32_t i = 0; r == YAN_BLOCK_OK && i < c->name_count; ++i) {
        const char *name = c->names[c->sort_index[i]];
        memcpy(c->current_file, name, sizeof c->current_file);
        r = file_scan(c, name, collect);
        if (r == YAN_BLOCK_INVALID_TEXT) { ++c->skipped; r = YAN_BLOCK_OK; }
    }
    if (r == YAN_BLOCK_OK) r = emit(c, sink, opaque);
    return finish(c, r, summary, state);
}
YanBlockResult yan_blocks_show(YanBlockContext *c, const YanBlockAddress *a, YanBlockRowFn sink,
                               YanBlockChunkFn chunk, void *opaque, YanBlockSummary *summary)
{
    YanBlockAddress key = {{0}, {0}};
    YanBlockResult r = guard(c, a, true, sink, opaque, summary, &key);
    if (r != YAN_BLOCK_OK) return r;
    if (chunk == NULL) return YAN_BLOCK_INVALID;
    begin(c);
    r = file_scan(c, key.name, NULL);
    const YanBlockDecl *decl = NULL;
    if (r == YAN_BLOCK_OK) {
        for (uint32_t i = 0; i < c->parser.block_count; ++i)
            if (name_compare(c->parser.blocks[i].id, key.id) == 0) { decl = &c->parser.blocks[i]; break; }
        if (decl == NULL) r = YAN_BLOCK_NOT_FOUND;
    }
    if (r == YAN_BLOCK_OK) {
        YanBlockRow *row = &c->rows[0];
        memset(row, 0, sizeof *row);
        memcpy(row->source, key.name, sizeof row->source); row->block = *decl;
        uint32_t length = row->block.end - row->block.start;
        c->total = length == 0u ? 0u : row->block.last_line - row->block.first_line + 1u;
        c->row_count = (uint32_t)c->total;
        if (!sink(opaque, row)) r = YAN_BLOCK_STOPPED;
        for (uint32_t offset = 0; r == YAN_BLOCK_OK && offset < length;) {
            uint32_t amount = length - offset;
            if (amount > sizeof c->buffer) amount = sizeof c->buffer;
            r = read_exact(c, key.name, row->block.start + offset, amount);
            if (r == YAN_BLOCK_OK && !chunk(opaque, offset, c->buffer, amount)) r = YAN_BLOCK_STOPPED;
            offset += amount;
        }
    }
    return finish(c, r, summary, YAN_BLOCK_TARGET_OK);
}
const char *yan_block_target_name(YanBlockTargetState state)
{
    switch (state) {
    case YAN_BLOCK_TARGET_OK: return "OK";
    case YAN_BLOCK_DANGLING_FILE: return "DANGLING_FILE";
    case YAN_BLOCK_DANGLING_BLOCK: return "DANGLING_BLOCK";
    }
    return "INVALID";
}
