#include "knowledge_graph.h"

#include "note_blocks_format.h"
#include "note_metadata_format.h"
#include "search_text.h"
#include "semantic_text.h"

#include <stdlib.h>
#include <string.h>

/* 0044 R1 core. See host/knowledge_graph.h for the contract. Per file the pass
 * order is text -> header -> blocks -> directives, and files are visited in
 * ASCII name order, so the first violation wins and a binary byte anywhere in a
 * file outranks any earlier-looking `@kg` line in the same file. The complete
 * block scan of a file runs before its directive pass, so every recorded block
 * range is that block's final extent.
 *
 * Publication builds one candidate YanKg: every allocation is attached to the
 * candidate as soon as it exists, so a failure has exactly one cleanup path
 * (yan_kg_destroy) and the caller's published graph is untouched until the whole
 * candidate validated. */

/* One legal declaration is at most 4 + 8 + 1 + 31 + 1 + 31 + 1 + 31 = 108 bytes
 * (relation) or 4 + 7 + 1 + 31 + 1 + 256 = 300 (concept/alias). The retained
 * line prefix is larger, so an overflowing reserved line is already illegal and
 * the remainder of the line is never buffered. */
#define KG_LINE_PREFIX UINT32_C(384)
#define KG_CHUNK UINT32_C(4096)
/* Open addressing. 16384 buckets index at most 8192 concept IDs and 32768
 * buckets at most 16384 distinct (ID, alias) pairs, so a free slot always
 * exists; slots store ordinal + 1 so 0 keeps meaning empty. */
#define KG_ID_BUCKETS UINT32_C(16384)
#define KG_ALIAS_BUCKETS UINT32_C(32768)
#define KG_NONE UINT32_MAX

static bool kg_span(const void *pointer, size_t length)
{
    return (pointer != NULL || length == 0u) &&
           length <= UINTPTR_MAX - (uintptr_t)pointer;
}

static bool kg_overlaps(const void *left, size_t left_length,
                        const void *right, size_t right_length)
{
    return left_length != 0u && right_length != 0u &&
           (uintptr_t)left < (uintptr_t)right + right_length &&
           (uintptr_t)right < (uintptr_t)left + left_length;
}

static uint32_t kg_hash(const uint8_t *bytes, uint32_t length)
{
    uint32_t hash = UINT32_C(2166136261);
    for (uint32_t index = 0u; index < length; ++index) {
        hash ^= (uint32_t)bytes[index];
        hash *= UINT32_C(16777619);
    }
    return hash;
}

/* Bounded length of a NUL-terminated label in a fixed-width field. */
static uint32_t kg_text_length(const char *text)
{
    uint32_t length = 0u;
    while (length < YAN_KG_LABEL_MAX && text[length] != '\0') {
        ++length;
    }
    return length;
}

static int kg_compare_bytes(const uint8_t *left, uint32_t left_length,
                            const uint8_t *right, uint32_t right_length)
{
    uint32_t shared = left_length < right_length ? left_length : right_length;
    for (uint32_t index = 0u; index < shared; ++index) {
        if (left[index] != right[index]) {
            return left[index] < right[index] ? -1 : 1;
        }
    }
    if (left_length == right_length) {
        return 0;
    }
    return left_length < right_length ? -1 : 1;
}

static int kg_compare_id(const char *left, const char *right)
{
    return kg_compare_bytes((const uint8_t *)left, kg_text_length(left),
                            (const uint8_t *)right, kg_text_length(right));
}

/* ------------------------------------------------------------ build records */

typedef struct {
    char id[32];
    char name[YAN_KG_LABEL_MAX + 1u];
    uint32_t name_length;
    uint32_t declaration_head;
    uint32_t declaration_tail;
    uint32_t declaration_count;
} KgBuildConcept;

/* Declarations of one concept are chained in insertion order, which is already
 * the canonical (file name, declaration byte) order. */
typedef struct {
    uint32_t concept;
    uint32_t next;
    YanKgProvenance where;
} KgBuildDeclaration;

typedef struct {
    char id[32];
    char label[YAN_KG_LABEL_MAX + 1u];
    uint32_t label_length;
    uint32_t concept;
} KgBuildAlias;

typedef struct {
    char type[32];
    char subject[32];
    char object[32];
    uint32_t subject_concept;
    uint32_t object_concept;
    YanKgProvenance where;
} KgBuildRelation;

typedef struct {
    const uint8_t *bytes;
    uint32_t length;
} KgSlice;

typedef struct {
    const YanSemanticSource *source;
    YanSemanticFile *files;
    uint32_t file_count;
    YanBlockParser parser;
    YanBlockTextValidator text;
    uint8_t chunk[KG_CHUNK];

    KgBuildConcept *concepts;
    KgBuildDeclaration *declarations;
    KgBuildAlias *aliases;
    KgBuildRelation *relations;
    uint32_t *id_slots;
    uint32_t *alias_slots;
    uint32_t concept_count;
    uint32_t declaration_count;
    uint32_t alias_count;
    uint32_t relation_count;

    /* Streaming line scanner state for the directive pass. */
    uint8_t prefix[KG_LINE_PREFIX];
    uint32_t prefix_length;
    bool prefix_overflow;
    uint32_t line_start;
    uint32_t line_number;
    char file[32];
} KgBuild;

/* -------------------------------------------------------------- id indexing */

static uint32_t kg_id_find(const KgBuild *build, const char id[32])
{
    uint32_t slot = kg_hash((const uint8_t *)id, kg_text_length(id)) &
                    (KG_ID_BUCKETS - 1u);
    for (uint32_t probe = 0u; probe < KG_ID_BUCKETS; ++probe) {
        uint32_t stored = build->id_slots[slot];
        if (stored == 0u) {
            return KG_NONE;
        }
        if (kg_compare_id(build->concepts[stored - 1u].id, id) == 0) {
            return stored - 1u;
        }
        slot = (slot + 1u) & (KG_ID_BUCKETS - 1u);
    }
    return KG_NONE;
}

static bool kg_id_insert(KgBuild *build, const char id[32], uint32_t ordinal)
{
    uint32_t slot = kg_hash((const uint8_t *)id, kg_text_length(id)) &
                    (KG_ID_BUCKETS - 1u);
    for (uint32_t probe = 0u; probe < KG_ID_BUCKETS; ++probe) {
        if (build->id_slots[slot] == 0u) {
            build->id_slots[slot] = ordinal + 1u;
            return true;
        }
        slot = (slot + 1u) & (KG_ID_BUCKETS - 1u);
    }
    return false;
}

static uint32_t kg_alias_hash(const char id[32], const KgSlice label)
{
    uint32_t hash = kg_hash((const uint8_t *)id, kg_text_length(id));
    return (hash * UINT32_C(16777619)) ^ kg_hash(label.bytes, label.length);
}

/* KG_NONE when the (ID, alias) pair is new, the existing ordinal when the exact
 * pair was already recorded, so a duplicate never consumes cap. */
static uint32_t kg_alias_find(const KgBuild *build, const char id[32],
                              const KgSlice label)
{
    uint32_t slot = kg_alias_hash(id, label) & (KG_ALIAS_BUCKETS - 1u);
    for (uint32_t probe = 0u; probe < KG_ALIAS_BUCKETS; ++probe) {
        uint32_t stored = build->alias_slots[slot];
        if (stored == 0u) {
            return KG_NONE;
        }
        const KgBuildAlias *alias = &build->aliases[stored - 1u];
        if (kg_compare_id(alias->id, id) == 0 &&
            kg_compare_bytes((const uint8_t *)alias->label, alias->label_length,
                             label.bytes, label.length) == 0) {
            return stored - 1u;
        }
        slot = (slot + 1u) & (KG_ALIAS_BUCKETS - 1u);
    }
    return KG_NONE;
}

static bool kg_alias_insert(KgBuild *build, uint32_t ordinal)
{
    const KgBuildAlias *alias = &build->aliases[ordinal];
    KgSlice label = {(const uint8_t *)alias->label, alias->label_length};
    uint32_t slot = kg_alias_hash(alias->id, label) & (KG_ALIAS_BUCKETS - 1u);
    for (uint32_t probe = 0u; probe < KG_ALIAS_BUCKETS; ++probe) {
        if (build->alias_slots[slot] == 0u) {
            build->alias_slots[slot] = ordinal + 1u;
            return true;
        }
        slot = (slot + 1u) & (KG_ALIAS_BUCKETS - 1u);
    }
    return false;
}

/* ------------------------------------------------------------- field checks */

/* The whole file already passed the 0032 text preflight, so this routine only
 * adds the label rules: 1..256 bytes, no control scalar at all (TAB included),
 * no leading or trailing ASCII space or TAB, exact bytes and case. UTF-8
 * validity and the CR/LF rules come from the shared primitives. */
static bool kg_label_valid(const uint8_t *bytes, uint32_t length)
{
    if (bytes == NULL || length < 1u || length > YAN_KG_LABEL_MAX) {
        return false;
    }
    if (bytes[0] == (uint8_t)' ' || bytes[0] == (uint8_t)'\t' ||
        bytes[length - 1u] == (uint8_t)' ' ||
        bytes[length - 1u] == (uint8_t)'\t') {
        return false;
    }
    for (uint32_t offset = 0u; offset < length;) {
        uint32_t scalar = 0u;
        uint32_t width = yan_search_text_utf8_scalar(bytes + offset,
                                                     length - offset, &scalar);
        if (width == 0u) {
            return false;
        }
        if (scalar < 0x20u || (scalar >= 0x7Fu && scalar <= 0x9Fu)) {
            return false;
        }
        offset += width;
    }
    return true;
}

/* --------------------------------------------------------- candidate growth */

static YanKgResult kg_add_declaration(KgBuild *build, uint32_t concept,
                                      const YanKgProvenance *where)
{
    if (build->declaration_count == YAN_KG_DECLARATION_MAX) {
        return YAN_KG_LIMIT;
    }
    KgBuildDeclaration *entry = &build->declarations[build->declaration_count];
    memset(entry, 0, sizeof *entry);
    entry->concept = concept;
    entry->next = KG_NONE;
    entry->where = *where;
    KgBuildConcept *owner = &build->concepts[concept];
    if (owner->declaration_tail == KG_NONE) {
        owner->declaration_head = build->declaration_count;
    } else {
        build->declarations[owner->declaration_tail].next =
            build->declaration_count;
    }
    owner->declaration_tail = build->declaration_count;
    ++owner->declaration_count;
    ++build->declaration_count;
    return YAN_KG_OK;
}

static YanKgResult kg_record_concept(KgBuild *build, const KgSlice id,
                                    const KgSlice label,
                                    const YanKgProvenance *where)
{
    char key[32];
    memset(key, 0, sizeof key);
    memcpy(key, id.bytes, id.length);
    uint32_t ordinal = kg_id_find(build, key);
    if (ordinal != KG_NONE) {
        const KgBuildConcept *existing = &build->concepts[ordinal];
        if (existing->name_length != label.length ||
            memcmp(existing->name, label.bytes, label.length) != 0) {
            return YAN_KG_CONFLICT;
        }
        return kg_add_declaration(build, ordinal, where);
    }
    if (build->concept_count == YAN_KG_CONCEPT_MAX) {
        return YAN_KG_LIMIT;
    }
    ordinal = build->concept_count;
    KgBuildConcept *concept = &build->concepts[ordinal];
    memset(concept, 0, sizeof *concept);
    memcpy(concept->id, key, sizeof key);
    memcpy(concept->name, label.bytes, label.length);
    concept->name[label.length] = '\0';
    concept->name_length = label.length;
    concept->declaration_head = KG_NONE;
    concept->declaration_tail = KG_NONE;
    if (!kg_id_insert(build, concept->id, ordinal)) {
        return YAN_KG_NOSPACE;
    }
    ++build->concept_count;
    return kg_add_declaration(build, ordinal, where);
}

static YanKgResult kg_record_alias(KgBuild *build, const KgSlice id,
                                   const KgSlice label)
{
    char key[32];
    memset(key, 0, sizeof key);
    memcpy(key, id.bytes, id.length);
    if (kg_alias_find(build, key, label) != KG_NONE) {
        return YAN_KG_OK; /* identical (ID, alias) bytes are one pair */
    }
    if (build->alias_count == YAN_KG_ALIAS_MAX) {
        return YAN_KG_LIMIT;
    }
    KgBuildAlias *alias = &build->aliases[build->alias_count];
    memset(alias, 0, sizeof *alias);
    memcpy(alias->id, key, sizeof key);
    memcpy(alias->label, label.bytes, label.length);
    alias->label[label.length] = '\0';
    alias->label_length = label.length;
    alias->concept = KG_NONE;
    if (!kg_alias_insert(build, build->alias_count)) {
        return YAN_KG_NOSPACE;
    }
    ++build->alias_count;
    return YAN_KG_OK;
}

static YanKgResult kg_record_relation(KgBuild *build, const KgSlice type,
                                      const KgSlice subject,
                                      const KgSlice object,
                                      const YanKgProvenance *where)
{
    if (build->relation_count == YAN_KG_RELATION_MAX) {
        return YAN_KG_LIMIT;
    }
    KgBuildRelation *relation = &build->relations[build->relation_count];
    memset(relation, 0, sizeof *relation);
    memcpy(relation->type, type.bytes, type.length);
    memcpy(relation->subject, subject.bytes, subject.length);
    memcpy(relation->object, object.bytes, object.length);
    relation->subject_concept = KG_NONE;
    relation->object_concept = KG_NONE;
    relation->where = *where;
    ++build->relation_count;
    return YAN_KG_OK;
}

/* -------------------------------------------------------- directive grammar */

/* The enclosing block of a declaration, from its absolute byte offset. Blocks
 * are disjoint and ordered, so this is a binary search. */
static uint32_t kg_block_for(const YanBlockDecl *blocks, uint32_t count,
                             uint32_t offset)
{
    uint32_t low = 0u, high = count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2u;
        if (blocks[middle].start <= offset) {
            low = middle + 1u;
        } else {
            high = middle;
        }
    }
    if (low == 0u) {
        return KG_NONE;
    }
    uint32_t index = low - 1u;
    return offset < blocks[index].end ? index : KG_NONE;
}

static void kg_fill_block(YanKgProvenance *where, const YanBlockDecl *owner,
                          const char file[32], uint32_t line_number,
                          uint32_t line_start, uint32_t length)
{
    memset(where, 0, sizeof *where);
    memcpy(where->file, file, sizeof where->file);
    memcpy(where->block_id, owner->id, sizeof where->block_id);
    where->declaration_line = line_number;
    where->declaration_start = line_start;
    where->declaration_end = line_start + length;
    where->block_marker_line = owner->marker_line;
    where->block_first_line = owner->first_line;
    where->block_last_line = owner->last_line;
    where->block_start = owner->start;
    where->block_end = owner->end;
}

static bool kg_word_is(const KgSlice word, const char *expected)
{
    uint32_t length = (uint32_t)strlen(expected);
    return word.length == length &&
           memcmp(word.bytes, expected, length) == 0;
}

/* Parses one reserved line, which is the whole line without its CR/LF. A TAB
 * right after `@kg` is reserved and invalid, exactly as 0032 refuses
 * `@block<TAB>id`; every separator between the command word and a fixed field
 * must be one ASCII space. */
static YanKgResult kg_directive(KgBuild *build, const uint8_t *line,
                                uint32_t length, uint32_t line_number,
                                uint32_t line_start)
{
    if (length <= 3u || line[3] != (uint8_t)' ') {
        return YAN_KG_GRAPH_FORMAT;
    }
    uint32_t block = kg_block_for(build->parser.blocks,
                                  build->parser.block_count, line_start);
    if (block == KG_NONE) {
        return YAN_KG_GRAPH_FORMAT;
    }
    const YanBlockDecl *owner = &build->parser.blocks[block];

    uint32_t word_end = 4u;
    while (word_end < length && line[word_end] != (uint8_t)' ') {
        ++word_end;
    }
    KgSlice word = {line + 4u, word_end - 4u};
    bool is_concept = kg_word_is(word, "concept");
    bool is_alias = kg_word_is(word, "alias");
    bool is_relation = kg_word_is(word, "relation");
    if (!is_concept && !is_alias && !is_relation) {
        return YAN_KG_GRAPH_FORMAT;
    }
    if (word_end == length) {
        return YAN_KG_GRAPH_FORMAT; /* command word with no field at all */
    }
    uint32_t id_begin = word_end + 1u;
    uint32_t id_end = id_begin;
    while (id_end < length && line[id_end] != (uint8_t)' ') {
        ++id_end;
    }
    if (id_end == length) {
        /* No second field: a concept/alias label, or the relation's SUBJECT and
         * OBJECT, are missing. */
        return YAN_KG_GRAPH_FORMAT;
    }
    KgSlice id = {line + id_begin, id_end - id_begin};
    if (!yan_block_id_valid(id.bytes, id.length)) {
        return YAN_KG_GRAPH_FORMAT;
    }
    if (is_concept || is_alias) {
        KgSlice label = {line + id_end + 1u, length - (id_end + 1u)};
        if (!kg_label_valid(label.bytes, label.length)) {
            return YAN_KG_GRAPH_FORMAT;
        }
        YanKgProvenance where;
        kg_fill_block(&where, owner, build->file, line_number, line_start,
                      length);
        return is_concept ? kg_record_concept(build, id, label, &where)
                          : kg_record_alias(build, id, label);
    }
    /* relation TYPE SUBJECT OBJECT: exactly three fields, no trailing space.
     * yan_block_id_valid also rejects an empty field. */
    uint32_t subject_end = id_end + 1u;
    while (subject_end < length && line[subject_end] != (uint8_t)' ') {
        ++subject_end;
    }
    if (subject_end == length) {
        return YAN_KG_GRAPH_FORMAT;
    }
    KgSlice subject = {line + id_end + 1u, subject_end - (id_end + 1u)};
    KgSlice object = {line + subject_end + 1u, length - (subject_end + 1u)};
    if (!yan_block_id_valid(subject.bytes, subject.length) ||
        !yan_block_id_valid(object.bytes, object.length)) {
        return YAN_KG_GRAPH_FORMAT;
    }
    YanKgProvenance where;
    kg_fill_block(&where, owner, build->file, line_number, line_start, length);
    return kg_record_relation(build, id, subject, object, &where);
}

/* Reserved prefix: `@kg` at column 0 followed by one ASCII space, one TAB or the
 * end of the line. Leading space, `@kgx` and `@ kg` are ordinary prose. */
static bool kg_reserved(const uint8_t *line, uint32_t length)
{
    if (length < 3u || line[0] != (uint8_t)'@' || line[1] != (uint8_t)'k' ||
        line[2] != (uint8_t)'g') {
        return false;
    }
    return length == 3u || line[3] == (uint8_t)' ' || line[3] == (uint8_t)'\t';
}

/* ---------------------------------------------------------------- scanning */

static YanKgResult kg_read(KgBuild *build, const char *name, uint32_t offset,
                           uint32_t length, uint8_t *out)
{
    if (length == 0u) {
        return YAN_KG_OK;
    }
    YanSemanticResult result = build->source->read(build->source->context, name,
                                                   offset, out, length);
    return result == YAN_SEMANTIC_OK ? YAN_KG_OK : YAN_KG_IO;
}

/* The shared 0033 logical view: strictly increasing cursor, slot below the
 * runtime capacity, a valid NUL-terminated name, no duplicate slot or name, and
 * at most `capacity` files. Files are then ordered by ASCII name, the canonical
 * order every provenance array uses. */
static int kg_compare_files(const void *left, const void *right)
{
    return kg_compare_id(((const YanSemanticFile *)left)->name,
                         ((const YanSemanticFile *)right)->name);
}

static YanKgResult kg_collect_files(KgBuild *build)
{
    const uint32_t capacity = build->source->file_capacity;
    uint32_t used = 0u;
    uint32_t cursor = 0u;
    for (;;) {
        YanSemanticFile file;
        uint32_t previous = cursor;
        memset(&file, 0, sizeof file);
        YanSemanticResult result = build->source->next(build->source->context,
                                                      &cursor, &file);
        if (result == YAN_SEMANTIC_MISSING) {
            break;
        }
        if (result != YAN_SEMANTIC_OK) {
            return YAN_KG_IO;
        }
        if (cursor <= previous || file.slot >= capacity || used >= capacity) {
            return YAN_KG_CORRUPT;
        }
        const char *terminator = memchr(file.name, 0, sizeof file.name);
        if (terminator == NULL ||
            !yan_note_name_valid((const uint8_t *)file.name,
                                 (uint32_t)(terminator - file.name))) {
            return YAN_KG_CORRUPT;
        }
        for (uint32_t index = 0u; index < used; ++index) {
            if (build->files[index].slot == file.slot ||
                strcmp(build->files[index].name, file.name) == 0) {
                return YAN_KG_CORRUPT;
            }
        }
        build->files[used++] = file;
    }
    build->file_count = used;
    if (used > 1u) {
        qsort(build->files, used, sizeof build->files[0], kg_compare_files);
    }
    return YAN_KG_OK;
}

/* Pass 1: complete 0032 text qualification of the whole file. A non-text file is
 * skipped whole, before any header or directive is read, so a binary note can
 * neither contribute a concept nor abort the build. */
static YanKgResult kg_preflight_text(KgBuild *build, const YanSemanticFile *file,
                                     bool *eligible)
{
    *eligible = false;
    if (yan_block_text_init(&build->text) != YAN_BLOCK_OK) {
        return YAN_KG_PROTOCOL;
    }
    for (uint32_t offset = 0u; offset < file->size;) {
        uint32_t amount = file->size - offset;
        if (amount > KG_CHUNK) {
            amount = KG_CHUNK;
        }
        YanKgResult result = kg_read(build, file->name, offset, amount,
                                     build->chunk);
        if (result != YAN_KG_OK) {
            return result;
        }
        YanBlockResult state = yan_block_text_feed(&build->text, build->chunk,
                                                  amount);
        if (state != YAN_BLOCK_OK && state != YAN_BLOCK_INVALID_TEXT) {
            return YAN_KG_PROTOCOL;
        }
        offset += amount;
    }
    YanBlockResult state = yan_block_text_finish(&build->text);
    if (state == YAN_BLOCK_INVALID_TEXT) {
        return YAN_KG_OK;
    }
    if (state != YAN_BLOCK_OK) {
        return YAN_KG_PROTOCOL;
    }
    *eligible = true;
    return YAN_KG_OK;
}

/* Pass 2: the 0027 header decides where the body starts. A broken header on
 * legal text is METADATA. */
static YanKgResult kg_read_header(KgBuild *build, const YanSemanticFile *file,
                                  uint32_t *body_offset, uint32_t *body_line)
{
    uint32_t prefix = file->size;
    if (prefix > YAN_NOTE_HEADER_MAX) {
        prefix = YAN_NOTE_HEADER_MAX;
    }
    YanKgResult result = kg_read(build, file->name, 0u, prefix, build->chunk);
    if (result != YAN_KG_OK) {
        return result;
    }
    YanNoteParseWorkspace workspace;
    YanNoteInfo info;
    memset(&info, 0, sizeof info);
    if (yan_note_parse_header(build->chunk, prefix, &info, &workspace) !=
        YAN_NOTE_OK) {
        return YAN_KG_METADATA;
    }
    *body_offset = info.header_bytes;
    *body_line = 1u;
    for (uint32_t index = 0u; index < info.header_bytes; ++index) {
        if (build->chunk[index] == (uint8_t)'\n') {
            ++*body_line;
        }
    }
    return YAN_KG_OK;
}

static YanKgResult kg_map_block(YanBlockResult state)
{
    switch (state) {
    case YAN_BLOCK_OK: return YAN_KG_OK;
    case YAN_BLOCK_FORMAT: return YAN_KG_BLOCK_FORMAT;
    case YAN_BLOCK_AMBIGUOUS: return YAN_KG_BLOCK_AMBIGUOUS;
    case YAN_BLOCK_LIMIT: return YAN_KG_BLOCK_LIMIT;
    default: return YAN_KG_PROTOCOL;
    }
}

/* Pass 3: the 0032 grammar validates every named block and leaves the final
 * extents in parser.blocks, which the directive pass then reads. */
static YanKgResult kg_scan_blocks(KgBuild *build, const YanSemanticFile *file,
                                 uint32_t body_offset, uint32_t body_line)
{
    YanBlockResult state = yan_block_parser_init(&build->parser, body_offset,
                                                body_line, NULL, NULL);
    if (state != YAN_BLOCK_OK) {
        return kg_map_block(state);
    }
    for (uint32_t offset = body_offset; offset < file->size;) {
        uint32_t amount = file->size - offset;
        if (amount > KG_CHUNK) {
            amount = KG_CHUNK;
        }
        YanKgResult result = kg_read(build, file->name, offset, amount,
                                      build->chunk);
        if (result != YAN_KG_OK) {
            return result;
        }
        state = yan_block_parser_feed(&build->parser, build->chunk, amount);
        if (state != YAN_BLOCK_OK) {
            return kg_map_block(state);
        }
        offset += amount;
    }
    return kg_map_block(yan_block_parser_finish(&build->parser));
}

/* Ends the buffered line. `terminator` is the absolute offset of the LF that
 * ended it, or KG_NONE for a final line without a newline. */
static YanKgResult kg_close_line(KgBuild *build, uint32_t terminator)
{
    uint32_t length = build->prefix_length;
    bool known = !build->prefix_overflow;
    if (known && terminator != KG_NONE && length != 0u &&
        build->prefix[length - 1u] == (uint8_t)'\r') {
        --length;
    }
    /* An overflowing line retains only its first KG_LINE_PREFIX bytes, which is
     * enough to classify the reserved prefix but never enough to hold a legal
     * declaration. */
    bool reserved = kg_reserved(build->prefix,
                                known ? length : build->prefix_length);
    YanKgResult result = YAN_KG_OK;
    if (reserved) {
        result = known ? kg_directive(build, build->prefix, length,
                                      build->line_number, build->line_start)
                       : YAN_KG_GRAPH_FORMAT;
    }
    build->prefix_length = 0u;
    build->prefix_overflow = false;
    if (terminator != KG_NONE) {
        build->line_start = terminator + 1u;
        if (build->line_number != UINT32_MAX) {
            ++build->line_number;
        }
    }
    return result;
}

/* Pass 4: the directive scan. Only a bounded line prefix is retained, so a
 * 100 KiB ordinary line costs exactly what a one-byte line costs. */
static YanKgResult kg_scan_directives(KgBuild *build,
                                      const YanSemanticFile *file,
                                      uint32_t body_offset, uint32_t body_line)
{
    build->prefix_length = 0u;
    build->prefix_overflow = false;
    build->line_start = body_offset;
    build->line_number = body_line;
    for (uint32_t offset = body_offset; offset < file->size;) {
        uint32_t amount = file->size - offset;
        if (amount > KG_CHUNK) {
            amount = KG_CHUNK;
        }
        YanKgResult result = kg_read(build, file->name, offset, amount,
                                      build->chunk);
        if (result != YAN_KG_OK) {
            return result;
        }
        for (uint32_t index = 0u; index < amount; ++index) {
            uint8_t byte = build->chunk[index];
            if (byte == (uint8_t)'\n') {
                result = kg_close_line(build, offset + index);
                if (result != YAN_KG_OK) {
                    return result;
                }
            } else if (build->prefix_length < KG_LINE_PREFIX) {
                build->prefix[build->prefix_length++] = byte;
            } else {
                build->prefix_overflow = true;
            }
        }
        offset += amount;
    }
    if (build->prefix_length != 0u || build->prefix_overflow) {
        return kg_close_line(build, KG_NONE);
    }
    return YAN_KG_OK;
}

/* ------------------------------------------------------------ publication */

/* Used only to derive the canonical ID order. */
typedef struct {
    const char *id;
    uint32_t ordinal;
} KgConceptKey;

static int kg_compare_keys(const void *left, const void *right)
{
    return kg_compare_id(((const KgConceptKey *)left)->id,
                         ((const KgConceptKey *)right)->id);
}

/* (label bytes, then concept): equal labels form one run whose concepts ascend,
 * which is what makes both lookup and the per-concept alias list one pass. */
static int kg_compare_labels(const void *left, const void *right)
{
    const YanKgLabelRef *a = left, *b = right;
    int order = kg_compare_bytes((const uint8_t *)a->text, a->length,
                                 (const uint8_t *)b->text, b->length);
    if (order != 0) {
        return order;
    }
    if (a->concept == b->concept) {
        return 0;
    }
    return a->concept < b->concept ? -1 : 1;
}

/* Copies one published string into the graph's arena. The arena is sized
 * exactly beforehand, so this never grows or moves. */
static const char *kg_arena_copy(char *arena, size_t *used, const char *text,
                                 uint32_t length)
{
    char *copy = arena + *used;
    memcpy(copy, text, length);
    copy[length] = '\0';
    *used += (size_t)length + 1u;
    return copy;
}

/* Resolves every reference, then materialises the published graph into `graph`,
 * attaching each allocation as soon as it exists so a failure needs only
 * yan_kg_destroy(graph). */
static YanKgResult kg_publish(KgBuild *build, YanKg *graph)
{
    const uint32_t concept_total = build->concept_count;
    const uint32_t relation_total = build->relation_count;
    const uint32_t alias_total = build->alias_count;

    /* Reference resolution. An undeclared concept ID is UNRESOLVED. */
    for (uint32_t index = 0u; index < alias_total; ++index) {
        uint32_t ordinal = kg_id_find(build, build->aliases[index].id);
        if (ordinal == KG_NONE) {
            return YAN_KG_UNRESOLVED;
        }
        build->aliases[index].concept = ordinal;
    }
    for (uint32_t index = 0u; index < relation_total; ++index) {
        KgBuildRelation *relation = &build->relations[index];
        relation->subject_concept = kg_id_find(build, relation->subject);
        relation->object_concept = kg_id_find(build, relation->object);
        if (relation->subject_concept == KG_NONE ||
            relation->object_concept == KG_NONE) {
            return YAN_KG_UNRESOLVED;
        }
    }

    /* Canonical concept order plus the build -> published ordinal inverse. */
    KgConceptKey *keys = NULL;
    uint32_t *inverse = NULL;
    size_t arena_size = 0u;
    if (concept_total != 0u) {
        keys = malloc((size_t)concept_total * sizeof *keys);
        inverse = malloc((size_t)concept_total * sizeof *inverse);
        if (keys == NULL || inverse == NULL) {
            free(keys);
            free(inverse);
            return YAN_KG_NOSPACE;
        }
    }
    for (uint32_t index = 0u; index < concept_total; ++index) {
        const KgBuildConcept *concept = &build->concepts[index];
        keys[index].id = concept->id;
        keys[index].ordinal = index;
        arena_size += (size_t)kg_text_length(concept->id) + 1u;
        arena_size += (size_t)concept->name_length + 1u;
    }
    for (uint32_t index = 0u; index < alias_total; ++index) {
        arena_size += (size_t)build->aliases[index].label_length + 1u;
    }
    for (uint32_t index = 0u; index < relation_total; ++index) {
        arena_size += (size_t)kg_text_length(build->relations[index].type) + 1u;
    }
    if (concept_total > 1u) {
        qsort(keys, concept_total, sizeof *keys, kg_compare_keys);
    }

    if (arena_size != 0u) {
        graph->arena = malloc(arena_size);
        if (graph->arena == NULL) {
            free(keys);
            free(inverse);
            return YAN_KG_NOSPACE;
        }
    }
    const uint32_t label_total = concept_total + alias_total;
    graph->concepts = calloc(concept_total == 0u ? 1u : concept_total,
                             sizeof *graph->concepts);
    graph->relations = calloc(relation_total == 0u ? 1u : relation_total,
                              sizeof *graph->relations);
    graph->declarations =
        calloc(build->declaration_count == 0u ? 1u : build->declaration_count,
               sizeof *graph->declarations);
    graph->alias_pool = calloc(alias_total == 0u ? 1u : alias_total,
                               sizeof *graph->alias_pool);
    graph->labels = calloc(label_total == 0u ? 1u : label_total,
                           sizeof *graph->labels);
    graph->out_offsets = calloc((size_t)concept_total + 1u,
                                sizeof *graph->out_offsets);
    graph->in_offsets = calloc((size_t)concept_total + 1u,
                               sizeof *graph->in_offsets);
    graph->self_offsets = calloc((size_t)concept_total + 1u,
                                 sizeof *graph->self_offsets);
    graph->out_items = calloc(relation_total == 0u ? 1u : relation_total,
                              sizeof *graph->out_items);
    graph->in_items = calloc(relation_total == 0u ? 1u : relation_total,
                             sizeof *graph->in_items);
    graph->self_items = calloc(relation_total == 0u ? 1u : relation_total,
                               sizeof *graph->self_items);
    if (graph->concepts == NULL || graph->relations == NULL ||
        graph->declarations == NULL || graph->alias_pool == NULL ||
        graph->labels == NULL || graph->out_offsets == NULL ||
        graph->in_offsets == NULL || graph->self_offsets == NULL ||
        graph->out_items == NULL || graph->in_items == NULL ||
        graph->self_items == NULL) {
        free(keys);
        free(inverse);
        return YAN_KG_NOSPACE;
    }

    size_t arena_used = 0u;
    /* Concepts in ID order, each with its declaration slice. */
    uint32_t declaration_cursor = 0u;
    for (uint32_t index = 0u; index < concept_total; ++index) {
        const KgBuildConcept *source = &build->concepts[keys[index].ordinal];
        YanKgConcept *target = &graph->concepts[index];
        inverse[keys[index].ordinal] = index;
        target->id = kg_arena_copy(graph->arena, &arena_used, source->id,
                                   kg_text_length(source->id));
        target->name = kg_arena_copy(graph->arena, &arena_used, source->name,
                                     source->name_length);
        target->declaration_count = source->declaration_count;
        target->declarations = &graph->declarations[declaration_cursor];
        for (uint32_t node = source->declaration_head; node != KG_NONE;
             node = build->declarations[node].next) {
            graph->declarations[declaration_cursor++] =
                build->declarations[node].where;
        }
    }
    free(keys);

    /* One label table: each concept's own name, then its aliases. Sorting it
     * yields label lookup and every per-concept alias list at once. */
    for (uint32_t index = 0u; index < concept_total; ++index) {
        YanKgLabelRef *label = &graph->labels[index];
        label->text = graph->concepts[index].name;
        label->length = kg_text_length(graph->concepts[index].name);
        label->concept = index;
        label->is_name = true;
    }
    for (uint32_t index = 0u; index < alias_total; ++index) {
        const KgBuildAlias *alias = &build->aliases[index];
        const char *text = kg_arena_copy(graph->arena, &arena_used,
                                         alias->label, alias->label_length);
        YanKgLabelRef *label = &graph->labels[concept_total + index];
        label->text = text;
        label->length = alias->label_length;
        label->concept = inverse[alias->concept];
        label->is_name = false;
    }
    if (label_total > 1u) {
        qsort(graph->labels, label_total, sizeof *graph->labels,
              kg_compare_labels);
    }

    /* Per-concept alias slices, taken from the sorted table so each list is in
     * ascending label order. */
    uint32_t *alias_offsets = calloc((size_t)concept_total + 1u,
                                     sizeof *alias_offsets);
    uint32_t *alias_fill = calloc(concept_total == 0u ? 1u : concept_total,
                                  sizeof *alias_fill);
    if (alias_offsets == NULL || alias_fill == NULL) {
        free(alias_offsets);
        free(alias_fill);
        free(inverse);
        return YAN_KG_NOSPACE;
    }
    for (uint32_t index = 0u; index < label_total; ++index) {
        if (!graph->labels[index].is_name) {
            ++graph->concepts[graph->labels[index].concept].alias_count;
        }
    }
    for (uint32_t index = 0u; index < concept_total; ++index) {
        alias_offsets[index + 1u] =
            alias_offsets[index] + graph->concepts[index].alias_count;
    }
    for (uint32_t index = 0u; index < label_total; ++index) {
        if (graph->labels[index].is_name) {
            continue;
        }
        uint32_t concept = graph->labels[index].concept;
        graph->alias_pool[alias_offsets[concept] + alias_fill[concept]++] =
            graph->labels[index].text;
    }
    for (uint32_t index = 0u; index < concept_total; ++index) {
        graph->concepts[index].aliases = &graph->alias_pool[alias_offsets[index]];
    }
    free(alias_fill);
    free(alias_offsets);

    /* Propositions keep insertion order, which is already the canonical (file
     * name, declaration byte) order. */
    for (uint32_t index = 0u; index < relation_total; ++index) {
        const KgBuildRelation *source = &build->relations[index];
        YanKgRelation *target = &graph->relations[index];
        target->type = kg_arena_copy(graph->arena, &arena_used, source->type,
                                     kg_text_length(source->type));
        target->subject = inverse[source->subject_concept];
        target->object = inverse[source->object_concept];
        target->subject_id = graph->concepts[target->subject].id;
        target->object_id = graph->concepts[target->object].id;
        target->where = source->where;
    }
    free(inverse);

    /* CSR adjacency in one O(E + V) counting pass. A self loop appears only in
     * `self`, so no proposition is listed twice for one concept. */
    uint32_t *out_fill = calloc(concept_total == 0u ? 1u : concept_total,
                                sizeof *out_fill);
    uint32_t *in_fill = calloc(concept_total == 0u ? 1u : concept_total,
                               sizeof *in_fill);
    uint32_t *self_fill = calloc(concept_total == 0u ? 1u : concept_total,
                                 sizeof *self_fill);
    if (out_fill == NULL || in_fill == NULL || self_fill == NULL) {
        free(out_fill);
        free(in_fill);
        free(self_fill);
        return YAN_KG_NOSPACE;
    }
    for (uint32_t index = 0u; index < relation_total; ++index) {
        const YanKgRelation *relation = &graph->relations[index];
        if (relation->subject == relation->object) {
            ++graph->concepts[relation->subject].self_count;
        } else {
            ++graph->concepts[relation->subject].out_count;
            ++graph->concepts[relation->object].in_count;
        }
    }
    for (uint32_t index = 0u; index < concept_total; ++index) {
        graph->out_offsets[index + 1u] =
            graph->out_offsets[index] + graph->concepts[index].out_count;
        graph->in_offsets[index + 1u] =
            graph->in_offsets[index] + graph->concepts[index].in_count;
        graph->self_offsets[index + 1u] =
            graph->self_offsets[index] + graph->concepts[index].self_count;
        graph->concepts[index].out =
            &graph->out_items[graph->out_offsets[index]];
        graph->concepts[index].in = &graph->in_items[graph->in_offsets[index]];
        graph->concepts[index].self =
            &graph->self_items[graph->self_offsets[index]];
    }
    for (uint32_t index = 0u; index < relation_total; ++index) {
        const YanKgRelation *relation = &graph->relations[index];
        if (relation->subject == relation->object) {
            uint32_t bucket = relation->subject;
            graph->self_items[graph->self_offsets[bucket] + self_fill[bucket]++] =
                relation;
        } else {
            uint32_t subject = relation->subject;
            uint32_t object = relation->object;
            graph->out_items[graph->out_offsets[subject] + out_fill[subject]++] =
                relation;
            graph->in_items[graph->in_offsets[object] + in_fill[object]++] =
                relation;
        }
    }
    free(out_fill);
    free(in_fill);
    free(self_fill);
    graph->concept_count = concept_total;
    graph->relation_count = relation_total;
    graph->declaration_count = build->declaration_count;
    graph->alias_count = alias_total;
    graph->label_count = label_total;
    graph->arena_size = (uint32_t)arena_used;
    return YAN_KG_OK;
}

/* ------------------------------------------------------------------- public */

uint32_t yan_kg_concept_count(const YanKg *graph)
{
    return graph != NULL ? graph->concept_count : 0u;
}

uint32_t yan_kg_relation_count(const YanKg *graph)
{
    return graph != NULL ? graph->relation_count : 0u;
}

const YanKgConcept *yan_kg_concept_at(const YanKg *graph, uint32_t ordinal)
{
    if (graph == NULL || graph->concepts == NULL ||
        ordinal >= graph->concept_count) {
        return NULL;
    }
    return &graph->concepts[ordinal];
}

const YanKgRelation *yan_kg_relation_at(const YanKg *graph, uint32_t ordinal)
{
    if (graph == NULL || graph->relations == NULL ||
        ordinal >= graph->relation_count) {
        return NULL;
    }
    return &graph->relations[ordinal];
}

void yan_kg_summary(const YanKg *graph, YanKgSummary *out)
{
    if (graph == NULL || out == NULL) {
        return;
    }
    *out = graph->summary;
}

/* Exact concept ID first, then a unique name or alias. The label table is sorted
 * by (label, concept), so one lower bound plus one run walk counts every
 * distinct matching concept: three or more candidates are reported as three or
 * more, never clamped. */
YanKgResult yan_kg_lookup(const YanKg *graph, const uint8_t *text,
                          uint32_t length, YanKgMatch *out)
{
    if (graph == NULL || !kg_span(graph, sizeof *graph) ||
        !kg_span(out, sizeof *out) ||
        kg_overlaps(out, sizeof *out, graph, sizeof *graph) || text == NULL ||
        length < 1u || length > YAN_KG_QUERY_MAX ||
        !yan_semantic_query_valid(text, length)) {
        return YAN_KG_INVALID;
    }
    YanKgMatch match;
    memset(&match, 0, sizeof match);
    match.concept = KG_NONE;
    match.kind = YAN_KG_MATCH_ID;

    /* Exact ID: concepts are published in ascending ID byte order. */
    if (graph->concepts != NULL && graph->concept_count != 0u) {
        uint32_t low = 0u, high = graph->concept_count;
        while (low < high) {
            uint32_t middle = low + (high - low) / 2u;
            const char *id = graph->concepts[middle].id;
            int order = kg_compare_bytes(text, length, (const uint8_t *)id,
                                         kg_text_length(id));
            if (order == 0) {
                match.concept = middle;
                match.candidate_count = 1u;
                *out = match;
                return YAN_KG_OK;
            }
            if (order < 0) {
                high = middle;
            } else {
                low = middle + 1u;
            }
        }
    }

    /* Lower bound of the label run, then count the distinct concepts in it. */
    match.kind = YAN_KG_MATCH_LABEL;
    if (graph->labels != NULL && graph->label_count != 0u) {
        uint32_t low = 0u, high = graph->label_count;
        while (low < high) {
            uint32_t middle = low + (high - low) / 2u;
            const YanKgLabelRef *label = &graph->labels[middle];
            int order = kg_compare_bytes(text, length,
                                         (const uint8_t *)label->text,
                                         label->length);
            if (order <= 0) {
                high = middle;
            } else {
                low = middle + 1u;
            }
        }
        uint32_t distinct = 0u, first = KG_NONE, previous = KG_NONE;
        for (uint32_t index = low; index < graph->label_count; ++index) {
            const YanKgLabelRef *label = &graph->labels[index];
            if (kg_compare_bytes(text, length, (const uint8_t *)label->text,
                                 label->length) != 0) {
                break;
            }
            /* The run ascends by concept, so one comparison detects each new
             * candidate. A concept's own name plus an identical alias counts
             * once, and no candidate is ever dropped. */
            if (label->concept != previous) {
                if (distinct == 0u) {
                    first = label->concept;
                }
                ++distinct;
                previous = label->concept;
            }
        }
        match.candidate_count = distinct;
        if (distinct == 1u) {
            match.concept = first;
            *out = match;
            return YAN_KG_OK;
        }
        if (distinct > 1u) {
            *out = match;
            return YAN_KG_AMBIGUOUS;
        }
    }
    match.candidate_count = 0u;
    *out = match;
    return YAN_KG_NOT_FOUND;
}

void yan_kg_destroy(YanKg *graph)
{
    if (graph == NULL) {
        return;
    }
    free(graph->concepts);
    free(graph->relations);
    free(graph->declarations);
    free(graph->labels);
    free(graph->alias_pool);
    free(graph->out_offsets);
    free(graph->in_offsets);
    free(graph->self_offsets);
    free(graph->out_items);
    free(graph->in_items);
    free(graph->self_items);
    free(graph->arena);
    memset(graph, 0, sizeof *graph);
}

/* Frees the transient build workspace. Every pointer is owned here. */
static void kg_release_build(KgBuild *build)
{
    free(build->files);
    free(build->concepts);
    free(build->declarations);
    free(build->aliases);
    free(build->relations);
    free(build->id_slots);
    free(build->alias_slots);
    memset(build, 0, sizeof *build);
}

YanKgResult yan_kg_build(YanKg *graph, const YanSemanticSource *source,
                         YanKgSummary *summary)
{
    if (graph == NULL || !kg_span(graph, sizeof *graph) ||
        !kg_span(source, sizeof *source) ||
        kg_overlaps(graph, sizeof *graph, source, sizeof *source) ||
        (summary != NULL &&
         (!kg_span(summary, sizeof *summary) ||
          kg_overlaps(summary, sizeof *summary, graph, sizeof *graph) ||
          kg_overlaps(summary, sizeof *summary, source, sizeof *source)))) {
        return YAN_KG_INVALID;
    }
    if (graph->building) {
        return YAN_KG_BUSY;
    }
    if (source->next == NULL || source->read == NULL ||
        !kg_span(source->context, source->context_size) ||
        (source->context_size != 0u &&
         kg_overlaps(source->context, source->context_size, graph,
                     sizeof *graph))) {
        return YAN_KG_INVALID;
    }
    const uint32_t capacity = source->file_capacity;
    if (capacity < 1u || capacity > YAN_SEMANTIC_CAPACITY_MAX) {
        return YAN_KG_INVALID;
    }

    /* Transient workspace is sized by the R1 caps once, so no growth step can
     * move a retained pointer. */
    KgBuild build;
    memset(&build, 0, sizeof build);
    build.source = source;
    build.files = calloc(capacity, sizeof *build.files);
    build.concepts = calloc(YAN_KG_CONCEPT_MAX, sizeof *build.concepts);
    build.declarations = calloc(YAN_KG_DECLARATION_MAX,
                                sizeof *build.declarations);
    build.aliases = calloc(YAN_KG_ALIAS_MAX, sizeof *build.aliases);
    build.relations = calloc(YAN_KG_RELATION_MAX, sizeof *build.relations);
    build.id_slots = calloc(KG_ID_BUCKETS, sizeof *build.id_slots);
    build.alias_slots = calloc(KG_ALIAS_BUCKETS, sizeof *build.alias_slots);
    if (build.files == NULL || build.concepts == NULL ||
        build.declarations == NULL || build.aliases == NULL ||
        build.relations == NULL || build.id_slots == NULL ||
        build.alias_slots == NULL) {
        kg_release_build(&build);
        return YAN_KG_NOSPACE;
    }

    /* The candidate owns everything it allocates, so every failure path is
     * exactly one destroy and the caller's graph is never touched. */
    YanKg candidate;
    memset(&candidate, 0, sizeof candidate);
    graph->building = 1;
    YanKgResult result = kg_collect_files(&build);
    uint32_t skipped = 0u;
    for (uint32_t index = 0u; result == YAN_KG_OK && index < build.file_count;
         ++index) {
        const YanSemanticFile *file = &build.files[index];
        memset(build.file, 0, sizeof build.file);
        memcpy(build.file, file->name, sizeof build.file);
        bool eligible = false;
        result = kg_preflight_text(&build, file, &eligible);
        if (result != YAN_KG_OK) {
            break;
        }
        if (!eligible) {
            ++skipped;
            continue;
        }
        uint32_t body_offset = 0u, body_line = 0u;
        result = kg_read_header(&build, file, &body_offset, &body_line);
        if (result == YAN_KG_OK) {
            result = kg_scan_blocks(&build, file, body_offset, body_line);
        }
        if (result == YAN_KG_OK) {
            result = kg_scan_directives(&build, file, body_offset, body_line);
        }
        if (result != YAN_KG_OK) {
            break;
        }
    }
    if (result == YAN_KG_OK) {
        result = kg_publish(&build, &candidate);
    }
    const uint32_t visited = build.file_count;
    graph->building = 0;
    kg_release_build(&build);

    if (result != YAN_KG_OK) {
        yan_kg_destroy(&candidate);
        return result;
    }
    candidate.summary.concepts = candidate.concept_count;
    candidate.summary.relations = candidate.relation_count;
    candidate.summary.declarations = candidate.declaration_count;
    candidate.summary.aliases = candidate.alias_count;
    candidate.summary.files = visited;
    candidate.summary.skipped = skipped;
    yan_kg_destroy(graph);
    *graph = candidate;
    if (summary != NULL) {
        *summary = graph->summary;
    }
    return YAN_KG_OK;
}

const char *yan_kg_result_name(YanKgResult result)
{
    switch (result) {
    case YAN_KG_OK: return "OK";
    case YAN_KG_INVALID: return "INVALID";
    case YAN_KG_BUSY: return "BUSY";
    case YAN_KG_NOSPACE: return "NOSPACE";
    case YAN_KG_UNSUPPORTED: return "UNSUPPORTED";
    case YAN_KG_METADATA: return "METADATA";
    case YAN_KG_BLOCK_FORMAT: return "BLOCK_FORMAT";
    case YAN_KG_BLOCK_AMBIGUOUS: return "BLOCK_AMBIGUOUS";
    case YAN_KG_BLOCK_LIMIT: return "BLOCK_LIMIT";
    case YAN_KG_GRAPH_FORMAT: return "GRAPH_FORMAT";
    case YAN_KG_LIMIT: return "LIMIT";
    case YAN_KG_CONFLICT: return "CONFLICT";
    case YAN_KG_UNRESOLVED: return "UNRESOLVED";
    case YAN_KG_AMBIGUOUS: return "AMBIGUOUS";
    case YAN_KG_NOT_FOUND: return "NOT_FOUND";
    case YAN_KG_STALE: return "STALE";
    case YAN_KG_CORRUPT: return "CORRUPT";
    case YAN_KG_IO: return "IO";
    case YAN_KG_PROTOCOL: return "PROTOCOL";
    }
    return "UNKNOWN";
}
