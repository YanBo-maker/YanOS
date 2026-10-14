#include "semantic_corpus.h"
#include "note_metadata_format.h"
#include <stdlib.h>
#include <string.h>

static bool span(const void *p, size_t n)
{
    return (p != NULL || n == 0) && n <= UINTPTR_MAX - (uintptr_t)p;
}
static bool overlaps(const void *a, size_t an, const void *b, size_t bn)
{
    return an && bn && (uintptr_t)a < (uintptr_t)b + bn &&
           (uintptr_t)b < (uintptr_t)a + an;
}
static int compare_files(const void *a, const void *b)
{
    const YanSemanticCorpusFile *fa = a, *fb = b;
    return strcmp(fa->file.name, fb->file.name);
}
static YanSemanticResult hash_u32(YanSemanticSha256 *hash, uint32_t value)
{
    uint8_t bytes[4];
    for (unsigned i = 0; i < 4; ++i) bytes[i] = (uint8_t)(value >> (8 * i));
    return yan_semantic_sha256_update(hash, bytes, 4);
}
typedef struct {
    YanSemanticCorpus *corpus;
    const char *name;
    bool limit;
} Collector;
static bool collect(void *opaque, const YanSemanticChunk *chunk)
{
    Collector *collector = opaque;
    YanSemanticCorpus *c = collector->corpus;
    if (c->segment_count >= YAN_SEMANTIC_SEGMENT_MAX) {
        collector->limit = true;
        return false;
    }
    YanSemanticProvenance *p = &c->segments[c->segment_count++];
    memset(p, 0, sizeof *p);
    memcpy(p->name, collector->name, strlen(collector->name));
    p->start = chunk->start; p->end = chunk->end;
    p->first_line = chunk->first_line; p->last_line = chunk->last_line;
    p->scalar_count = chunk->scalar_count;
    return true;
}

YanSemanticResult yan_semantic_corpus_scan(YanSemanticCorpus *corpus,
                                           const YanSemanticSource *source)
{
    if (!span(corpus, sizeof *corpus) || corpus == NULL ||
        !span(source, sizeof *source) || source == NULL ||
        overlaps(corpus, sizeof *corpus, source, sizeof *source)) {
        return YAN_SEMANTIC_INVALID;
    }
    if (source->next == NULL || source->read == NULL ||
        !span(source->context, source->context_size) ||
        overlaps(corpus, sizeof *corpus, source->context, source->context_size))
        return YAN_SEMANTIC_INVALID;
    corpus->file_count = corpus->segment_count = corpus->skipped = 0;
    uint32_t cursor = 0;
    for (;;) {
        YanSemanticFile file;
        uint32_t previous = cursor;
        YanSemanticResult r = source->next(source->context, &cursor, &file);
        if (r == YAN_SEMANTIC_MISSING) break;
        if (r != YAN_SEMANTIC_OK) return r;
        if (cursor <= previous || file.slot >= 63 || corpus->file_count >= 63)
            return YAN_SEMANTIC_CORRUPT;
        const char *end = memchr(file.name, 0, sizeof file.name);
        if (end == NULL || !yan_note_name_valid((const uint8_t *)file.name,
                                               (uint32_t)(end - file.name)))
            return YAN_SEMANTIC_CORRUPT;
        for (uint32_t i = 0; i < corpus->file_count; ++i)
            if (corpus->files[i].file.slot == file.slot ||
                strcmp(corpus->files[i].file.name, file.name) == 0)
                return YAN_SEMANTIC_CORRUPT;
        YanSemanticCorpusFile *entry = &corpus->files[corpus->file_count++];
        memset(entry, 0, sizeof *entry);
        entry->file = file;
    }
    qsort(corpus->files, corpus->file_count, sizeof corpus->files[0], compare_files);
    YanSemanticResult r = yan_semantic_sha256_init(&corpus->hasher);
    if (r != YAN_SEMANTIC_OK) return r;
    /* Complete classification of every file before metadata/chunks. Hash the
     * remainder even after a text violation: skipped bytes still name source. */
    for (uint32_t i = 0; i < corpus->file_count; ++i) {
        YanSemanticCorpusFile *entry = &corpus->files[i];
        uint32_t n = (uint32_t)strlen(entry->file.name);
        r = hash_u32(&corpus->hasher, entry->file.slot);
        if (r == YAN_SEMANTIC_OK) r = hash_u32(&corpus->hasher, n);
        if (r == YAN_SEMANTIC_OK) r = yan_semantic_sha256_update(&corpus->hasher,
                                         (const uint8_t *)entry->file.name, n);
        if (r == YAN_SEMANTIC_OK) r = hash_u32(&corpus->hasher, entry->file.size);
        if (r == YAN_SEMANTIC_OK) r = yan_semantic_text_init(&corpus->validator);
        if (r != YAN_SEMANTIC_OK) return r;
        for (uint32_t offset = 0; offset < entry->file.size;) {
            uint32_t amount = entry->file.size - offset;
            if (amount > sizeof corpus->bytes) amount = sizeof corpus->bytes;
            r = source->read(source->context, entry->file.name, offset,
                              corpus->bytes, amount);
            if (r == YAN_SEMANTIC_OK) r = yan_semantic_sha256_update(
                                        &corpus->hasher, corpus->bytes, amount);
            if (r == YAN_SEMANTIC_OK) r = yan_semantic_text_update(
                                        &corpus->validator, corpus->bytes, amount);
            if (r != YAN_SEMANTIC_OK) return r;
            offset += amount;
        }
        r = yan_semantic_text_finish(&corpus->validator);
        if (r != YAN_SEMANTIC_OK && r != YAN_SEMANTIC_INVALID) return r;
        entry->eligible = r == YAN_SEMANTIC_OK;
        if (!entry->eligible) ++corpus->skipped;
    }
    r = yan_semantic_sha256_final(&corpus->hasher, corpus->source_hash);
    if (r != YAN_SEMANTIC_OK) return r;
    /* Bounded metadata is parsed only after the whole file passed preflight. */
    YanNoteParseWorkspace workspace;
    YanNoteInfo info;
    for (uint32_t i = 0; i < corpus->file_count; ++i) {
        YanSemanticCorpusFile *entry = &corpus->files[i];
        if (!entry->eligible) continue;
        uint32_t prefix = entry->file.size;
        if (prefix > sizeof corpus->bytes) prefix = sizeof corpus->bytes;
        r = source->read(source->context, entry->file.name, 0, corpus->bytes, prefix);
        if (r != YAN_SEMANTIC_OK) return r;
        YanNoteResult parsed = yan_note_parse_header(corpus->bytes, prefix, &info, &workspace);
        if (parsed != YAN_NOTE_OK) return YAN_SEMANTIC_METADATA;
        entry->body_offset = info.header_bytes;
        entry->body_line = 1;
        for (uint32_t b = 0; b < info.header_bytes; ++b)
            if (corpus->bytes[b] == '\n') ++entry->body_line;
        if (info.has_title) {
            entry->title_length = info.title_length;
            memcpy(entry->title, info.title, info.title_length);
        }
        r = yan_semantic_chunker_init(&corpus->chunker, entry->body_offset,
                                       entry->body_line);
        if (r != YAN_SEMANTIC_OK) return r;
        Collector collector = {corpus, entry->file.name, false};
        YanSemanticChunkSink sink = {&collector, collect};
        for (uint32_t offset = entry->body_offset; offset < entry->file.size;) {
            uint32_t amount = entry->file.size - offset;
            if (amount > sizeof corpus->bytes) amount = sizeof corpus->bytes;
            r = source->read(source->context, entry->file.name, offset,
                              corpus->bytes, amount);
            if (r == YAN_SEMANTIC_OK) r = yan_semantic_chunker_feed(&corpus->chunker,
                                                  corpus->bytes, amount, &sink);
            if (r != YAN_SEMANTIC_OK)
                return collector.limit ? YAN_SEMANTIC_LIMIT : r;
            offset += amount;
        }
        r = yan_semantic_chunker_finish(&corpus->chunker, &sink);
        if (r != YAN_SEMANTIC_OK) return collector.limit ? YAN_SEMANTIC_LIMIT : r;
    }
    return YAN_SEMANTIC_OK;
}
