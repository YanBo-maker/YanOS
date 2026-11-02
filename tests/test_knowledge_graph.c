#include "knowledge_graph.h"
#include "unity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 0044 R1 core suite. The source is an in-memory library that implements the
 * 0029/0033 callback seam, so the graph core is driven exactly as the R2 POSIX
 * adapter will drive it, without a disk image, a model or a toolchain. Every
 * fixture is a real note text; the expected provenance values are the literal
 * byte offsets and physical lines of that text. */

#define FIXTURE_FILES 136u
/* Long enough to cross the 16 KiB editor draft limit several times over and to
 * span more than one 4096-byte read chunk. */
#define LONG_BODY (40000u)

typedef struct {
    const char *name;
    uint32_t slot;
    const char *bytes;
    uint32_t size;
    uint8_t *owned;
} FakeFile;

typedef struct {
    FakeFile files[FIXTURE_FILES];
    uint32_t count;
    uint32_t reads;
    bool read_fail;
    uint32_t capacity;
} FakeSource;

static FakeSource source_state;
static YanSemanticSource source;
static YanKg graph;

static void fake_add(const char *name, uint32_t slot, const char *bytes)
{
    FakeFile *file = &source_state.files[source_state.count++];
    file->name = name;
    file->slot = slot;
    file->bytes = bytes;
    file->size = (uint32_t)strlen(bytes);
    file->owned = NULL;
}

/* One owned fixture for generated text: `parts` is concatenated. */
static void fake_add_owned(const char *name, uint32_t slot, char *bytes)
{
    FakeFile *file = &source_state.files[source_state.count++];
    file->name = name;
    file->slot = slot;
    file->bytes = bytes;
    file->size = (uint32_t)strlen(bytes);
    file->owned = (uint8_t *)bytes;
}

/* Drops every fixture and the buffers it owns, so a mid-test rebuild can reuse
 * the same source object without leaking the previous library. */
static void fake_reset(void)
{
    for (uint32_t index = 0u; index < source_state.count; ++index) {
        free(source_state.files[index].owned);
        source_state.files[index].owned = NULL;
    }
    source_state.count = 0u;
}

static YanSemanticResult fake_next(void *opaque, uint32_t *cursor,
                                   YanSemanticFile *out)
{
    FakeSource *state = opaque;
    if (*cursor >= state->count) {
        return YAN_SEMANTIC_MISSING;
    }
    const FakeFile *file = &state->files[*cursor];
    memset(out, 0, sizeof *out);
    memcpy(out->name, file->name, strlen(file->name));
    out->slot = file->slot;
    out->size = file->size;
    ++*cursor;
    return YAN_SEMANTIC_OK;
}

static YanSemanticResult fake_read(void *opaque, const char *name,
                                   uint32_t offset, uint8_t *out,
                                   uint32_t length)
{
    FakeSource *state = opaque;
    ++state->reads;
    if (state->read_fail) {
        return YAN_SEMANTIC_IO;
    }
    for (uint32_t index = 0u; index < state->count; ++index) {
        const FakeFile *file = &state->files[index];
        if (strcmp(name, file->name) != 0) {
            continue;
        }
        if (offset > file->size || length > file->size - offset) {
            return YAN_SEMANTIC_IO;
        }
        memcpy(out, file->bytes + offset, length);
        return YAN_SEMANTIC_OK;
    }
    return YAN_SEMANTIC_IO;
}

/* --------------------------------------------------------------- utilities */

static const YanKgConcept *concept(const char *id)
{
    YanKgMatch match;
    memset(&match, 0, sizeof match);
    if (yan_kg_lookup(&graph, (const uint8_t *)id, (uint32_t)strlen(id),
                      &match) != YAN_KG_OK) {
        return NULL;
    }
    return yan_kg_concept_at(&graph, match.concept);
}

static uint32_t ordinal_of(const char *id)
{
    YanKgMatch match;
    memset(&match, 0, sizeof match);
    YanKgResult result = yan_kg_lookup(&graph, (const uint8_t *)id,
                                      (uint32_t)strlen(id), &match);
    TEST_ASSERT_EQUAL(YAN_KG_OK, result);
    TEST_ASSERT_EQUAL_UINT32(1u, match.candidate_count);
    TEST_ASSERT_EQUAL_INT(YAN_KG_MATCH_ID, match.kind);
    return match.concept;
}

static void expect_result(YanKgResult expected)
{
    YanKgSummary summary;
    memset(&summary, 0, sizeof summary);
    YanKgResult result = yan_kg_build(&graph, &source, &summary);
    if (result != expected) {
        char message[128];
        snprintf(message, sizeof message, "expected %s, got %s",
                 yan_kg_result_name(expected), yan_kg_result_name(result));
        TEST_FAIL_MESSAGE(message);
    }
}

static uint32_t count_rows(const YanKgRelation *const *rows, uint32_t count)
{
    uint32_t live = 0u;
    for (uint32_t index = 0u; index < count; ++index) {
        if (rows[index] != NULL) {
            ++live;
        }
    }
    return live;
}

void setUp(void)
{
    memset(&source_state, 0, sizeof source_state);
    source_state.capacity = 63u;
    memset(&graph, 0, sizeof graph);
    yan_kg_destroy(&graph);
    source = (YanSemanticSource){&source_state, sizeof source_state, fake_next,
                                 fake_read, 63u};
}

void tearDown(void)
{
    fake_reset();
    yan_kg_destroy(&graph);
}

/* ------------------------------------------------------- merging and duplicates */

static void test_empty_library_and_old_notes(void)
{
    YanKgSummary summary;
    memset(&summary, 0, sizeof summary);
    TEST_ASSERT_EQUAL(YAN_KG_OK, yan_kg_build(&graph, &source, &summary));
    TEST_ASSERT_EQUAL_UINT32(0u, summary.concepts);
    TEST_ASSERT_EQUAL_UINT32(0u, summary.relations);
    TEST_ASSERT_EQUAL_UINT32(0u, summary.files);

    /* A note with no directive at all still counts as a visited file. */
    fake_add("old-notes", 0u, "这份笔记没有图声明，正文照常。\n@blockx 不算\n");
    TEST_ASSERT_EQUAL(YAN_KG_OK, yan_kg_build(&graph, &source, &summary));
    TEST_ASSERT_EQUAL_UINT32(0u, summary.concepts);
    TEST_ASSERT_EQUAL_UINT32(1u, summary.files);
    TEST_ASSERT_EQUAL_UINT32(0u, summary.skipped);
    TEST_ASSERT_EQUAL_UINT32(0u, yan_kg_concept_count(&graph));
}

/* Two files declare the same concept with the same label: one concept, two
 * retained declaration provenance entries in ASCII file-name order. */
static void test_same_concept_in_two_files_merges_and_keeps_both(void)
{
    fake_add("b-note", 1u, "@block defs\n@kg concept square 正方形\n");
    fake_add("a-note", 0u, "@block defs\n@kg concept square 正方形\n");
    YanKgSummary summary;
    TEST_ASSERT_EQUAL(YAN_KG_OK, yan_kg_build(&graph, &source, &summary));
    TEST_ASSERT_EQUAL_UINT32(1u, summary.concepts);
    TEST_ASSERT_EQUAL_UINT32(2u, summary.declarations);
    const YanKgConcept *square = concept("square");
    TEST_ASSERT_NOT_NULL(square);
    TEST_ASSERT_EQUAL_STRING("正方形", square->name);
    TEST_ASSERT_EQUAL_UINT32(2u, square->declaration_count);
    /* ASCII name order decides which declaration comes first. */
    TEST_ASSERT_EQUAL_STRING("a-note", square->declarations[0].file);
    TEST_ASSERT_EQUAL_STRING("b-note", square->declarations[1].file);
}

/* The same ID with two different labels is CONFLICT and publishes nothing. */
static void test_same_id_different_label_is_conflict(void)
{
    fake_add("note", 0u,
             "@block a\n@kg concept square 正方形\n@kg concept square 方形\n");
    expect_result(YAN_KG_CONFLICT);
    TEST_ASSERT_EQUAL_UINT32(0u, yan_kg_concept_count(&graph));
}

/* A byte-identical repeated concept declaration is not a duplicate concept, but
 * it is a second provenance entry: every declaration keeps its location. */
static void test_repeated_identical_declaration_keeps_both_origins(void)
{
    fake_add("note", 0u,
             "@block a\n@kg concept square ASCII\n@kg concept square ASCII\n");
    YanKgSummary summary;
    TEST_ASSERT_EQUAL(YAN_KG_OK, yan_kg_build(&graph, &source, &summary));
    TEST_ASSERT_EQUAL_UINT32(1u, summary.concepts);
    const YanKgConcept *square = concept("square");
    TEST_ASSERT_NOT_NULL(square);
    TEST_ASSERT_EQUAL_UINT32(2u, square->declaration_count);
    TEST_ASSERT_TRUE(square->declarations[0].declaration_start <
                     square->declarations[1].declaration_start);
}

/* ------------------------------------------------------------ references */

/* An alias and a relation may precede their concept declarations. */
static void test_forward_references_resolve(void)
{
    fake_add("note", 0u,
             "@block a\n"
             "@kg relation is_a square rectangle\n"
             "@kg alias square 等边矩形\n"
             "@kg concept rectangle 矩形\n"
             "@kg concept square 正方形\n");
    YanKgSummary summary;
    TEST_ASSERT_EQUAL(YAN_KG_OK, yan_kg_build(&graph, &source, &summary));
    TEST_ASSERT_EQUAL_UINT32(2u, summary.concepts);
    TEST_ASSERT_EQUAL_UINT32(1u, summary.relations);
    TEST_ASSERT_EQUAL_UINT32(1u, summary.aliases);
    const YanKgConcept *square = concept("square");
    TEST_ASSERT_NOT_NULL(square);
    TEST_ASSERT_EQUAL_UINT32(1u, square->alias_count);
    TEST_ASSERT_EQUAL_STRING("等边矩形", square->aliases[0]);
    TEST_ASSERT_EQUAL_UINT32(1u, square->out_count);
    TEST_ASSERT_EQUAL_UINT32(0u, square->in_count);
}

/* A relation whose object was never declared is UNRESOLVED, even though the
 * subject exists. */
static void test_unresolved_object(void)
{
    fake_add("note", 0u,
             "@block a\n@kg concept square 正方形\n"
             "@kg relation is_a square rectangle\n");
    expect_result(YAN_KG_UNRESOLVED);
    TEST_ASSERT_EQUAL_UINT32(0u, yan_kg_concept_count(&graph));
}

/* An alias for an undeclared concept is UNRESOLVED as well. */
static void test_unresolved_alias(void)
{
    fake_add("note", 0u, "@block a\n@kg alias ghost 幽灵\n");
    expect_result(YAN_KG_UNRESOLVED);
    TEST_ASSERT_EQUAL_UINT32(0u, yan_kg_concept_count(&graph));
}

/* --------------------------------------------------------- lookup precedence */

static void test_exact_id_beats_label(void)
{
    /* Concept `b` has the label `a`, which is also concept `a`'s ID. */
    fake_add("note", 0u,
             "@block d\n@kg concept a A名\n@kg concept b a\n");
    YanKgSummary summary;
    TEST_ASSERT_EQUAL(YAN_KG_OK, yan_kg_build(&graph, &source, &summary));
    YanKgMatch match;
    memset(&match, 0, sizeof match);
    TEST_ASSERT_EQUAL(YAN_KG_OK,
                      yan_kg_lookup(&graph, (const uint8_t *)"a", 1u, &match));
    TEST_ASSERT_EQUAL_INT(YAN_KG_MATCH_ID, match.kind);
    TEST_ASSERT_EQUAL_UINT32(ordinal_of("a"), match.concept);
    TEST_ASSERT_EQUAL_UINT32(1u, match.candidate_count);
}

static void test_label_and_alias_unique_match(void)
{
    fake_add("note", 0u,
             "@block d\n@kg concept square 正方形\n"
             "@kg alias square 等边矩形\n");
    YanKgSummary summary;
    TEST_ASSERT_EQUAL(YAN_KG_OK, yan_kg_build(&graph, &source, &summary));
    YanKgMatch match;
    memset(&match, 0, sizeof match);
    TEST_ASSERT_EQUAL(YAN_KG_OK, yan_kg_lookup(&graph, (const uint8_t *)"正方形",
                                              (uint32_t)strlen("正方形"), &match));
    TEST_ASSERT_EQUAL_INT(YAN_KG_MATCH_LABEL, match.kind);
    TEST_ASSERT_EQUAL_UINT32(ordinal_of("square"), match.concept);
    TEST_ASSERT_EQUAL(YAN_KG_OK, yan_kg_lookup(&graph, (const uint8_t *)"等边矩形",
                                              (uint32_t)strlen("等边矩形"), &match));
    TEST_ASSERT_EQUAL_UINT32(ordinal_of("square"), match.concept);
}

/* One shared Chinese label behind two concepts is AMBIGUOUS and never merges
 * them; the same label as an alias of both behaves the same way. */
static void test_shared_label_is_ambiguous(void)
{
    fake_add("note", 0u,
             "@block d\n@kg concept a 相同\n@kg concept b 相同\n"
             "@kg alias a 共同\n@kg alias b 共同\n");
    YanKgSummary summary;
    TEST_ASSERT_EQUAL(YAN_KG_OK, yan_kg_build(&graph, &source, &summary));
    TEST_ASSERT_EQUAL_UINT32(2u, summary.concepts);
    YanKgMatch match;
    memset(&match, 0, sizeof match);
    TEST_ASSERT_EQUAL(YAN_KG_AMBIGUOUS,
                      yan_kg_lookup(&graph, (const uint8_t *)"相同",
                                    (uint32_t)strlen("相同"), &match));
    TEST_ASSERT_EQUAL_UINT32(2u, match.candidate_count);
    TEST_ASSERT_EQUAL(YAN_KG_AMBIGUOUS,
                      yan_kg_lookup(&graph, (const uint8_t *)"共同",
                                    (uint32_t)strlen("共同"), &match));
    /* The exact ID still wins over the ambiguous label. */
    TEST_ASSERT_EQUAL(YAN_KG_OK,
                      yan_kg_lookup(&graph, (const uint8_t *)"a", 1u, &match));
    TEST_ASSERT_EQUAL_INT(YAN_KG_MATCH_ID, match.kind);
    TEST_ASSERT_EQUAL_UINT32(2u, yan_kg_concept_count(&graph));
}

/* A concept's own name plus an alias with the same bytes is still one concept:
 * the candidate count is per concept, not per table entry. */
static void test_name_equals_alias_of_same_concept(void)
{
    fake_add("note", 0u,
             "@block d\n@kg concept a 同名\n@kg alias a 同名\n");
    YanKgSummary summary;
    TEST_ASSERT_EQUAL(YAN_KG_OK, yan_kg_build(&graph, &source, &summary));
    YanKgMatch match;
    memset(&match, 0, sizeof match);
    TEST_ASSERT_EQUAL(YAN_KG_OK, yan_kg_lookup(&graph, (const uint8_t *)"同名",
                                              (uint32_t)strlen("同名"), &match));
    TEST_ASSERT_EQUAL_UINT32(1u, match.candidate_count);
    TEST_ASSERT_EQUAL_UINT32(ordinal_of("a"), match.concept);
}

static void test_missing_label_is_not_found(void)
{
    fake_add("note", 0u, "@block d\n@kg concept a A\n");
    YanKgSummary summary;
    TEST_ASSERT_EQUAL(YAN_KG_OK, yan_kg_build(&graph, &source, &summary));
    YanKgMatch match;
    memset(&match, 0, sizeof match);
    TEST_ASSERT_EQUAL(YAN_KG_NOT_FOUND,
                      yan_kg_lookup(&graph, (const uint8_t *)"absent",
                                    (uint32_t)strlen("absent"), &match));
    TEST_ASSERT_EQUAL_UINT32(0u, match.candidate_count);
}


/* More than two concepts behind one label must report the real count. A lookup
 * that stopped at the first ambiguity would clamp every case to 2. */
static void test_many_concepts_share_one_label(void)
{
    fake_add("note", 0u,
             "@block p\n"
             "@kg concept a Shared\n@kg concept b Shared\n"
             "@kg concept c Shared\n@kg concept d Shared\n"
             "@kg concept e Shared\n@kg alias a Shared\n"
             "@kg alias e Shared\n");
    YanKgSummary summary;
    TEST_ASSERT_EQUAL(YAN_KG_OK, yan_kg_build(&graph, &source, &summary));
    TEST_ASSERT_EQUAL_UINT32(5u, summary.concepts);
    YanKgMatch match;
    memset(&match, 0, sizeof match);
    TEST_ASSERT_EQUAL(YAN_KG_AMBIGUOUS,
                      yan_kg_lookup(&graph, (const uint8_t *)"Shared",
                                    (uint32_t)strlen("Shared"), &match));
    TEST_ASSERT_EQUAL_UINT32(5u, match.candidate_count);

    /* A concept's own name plus its own identical alias is still one
     * candidate, so the count is per concept and not per table entry. */
    fake_reset();
    fake_add("note", 0u,
             "@block p\n@kg concept only Shared\n@kg alias only Shared\n");
    TEST_ASSERT_EQUAL(YAN_KG_OK, yan_kg_build(&graph, &source, &summary));
    TEST_ASSERT_EQUAL(YAN_KG_OK,
                      yan_kg_lookup(&graph, (const uint8_t *)"Shared",
                                    (uint32_t)strlen("Shared"), &match));
    TEST_ASSERT_EQUAL_UINT32(1u, match.candidate_count);
}

/* Three distinct aliases with the same bytes on three concepts behave like
 * three names; two of them identical stays two. */
static void test_aliases_only_shared_by_three_concepts(void)
{
    fake_add("note", 0u,
             "@block p\n"
             "@kg concept a A\n@kg concept b B\n@kg concept c C\n"
             "@kg alias a 共同\n@kg alias b 共同\n@kg alias c 共同\n");
    YanKgSummary summary;
    TEST_ASSERT_EQUAL(YAN_KG_OK, yan_kg_build(&graph, &source, &summary));
    YanKgMatch match;
    memset(&match, 0, sizeof match);
    TEST_ASSERT_EQUAL(YAN_KG_AMBIGUOUS,
                      yan_kg_lookup(&graph, (const uint8_t *)"共同",
                                    (uint32_t)strlen("共同"), &match));
    TEST_ASSERT_EQUAL_UINT32(3u, match.candidate_count);
}

/* ------------------------------------------------------------- multi-graph */

/* Every occurrence is its own proposition: byte-identical duplicates, the same
 * endpoints under another type, a two-node cycle and a self loop all survive. */
static void test_duplicates_types_cycles_and_self_loops(void)
{
    fake_add("a-note", 0u,
             "@block d\n"
             "@kg concept square 正方形\n@kg concept rectangle 矩形\n"
             "@kg relation is_a square rectangle\n"
             "@kg relation is_a square rectangle\n"
             "@kg relation part_of rectangle square\n"
             "@kg relation equivalent square square\n");
    YanKgSummary summary;
    TEST_ASSERT_EQUAL(YAN_KG_OK, yan_kg_build(&graph, &source, &summary));
    TEST_ASSERT_EQUAL_UINT32(2u, summary.concepts);
    TEST_ASSERT_EQUAL_UINT32(4u, summary.relations);
    const YanKgConcept *square = concept("square");
    const YanKgConcept *rectangle = concept("rectangle");
    TEST_ASSERT_NOT_NULL(square);
    TEST_ASSERT_NOT_NULL(rectangle);
    /* Two is_a plus the self loop going out; part_of arrives. */
    TEST_ASSERT_EQUAL_UINT32(2u, square->out_count);
    TEST_ASSERT_EQUAL_UINT32(1u, square->in_count);
    TEST_ASSERT_EQUAL_UINT32(1u, square->self_count);
    TEST_ASSERT_EQUAL_UINT32(0u, rectangle->self_count);
    TEST_ASSERT_EQUAL_UINT32(2u, rectangle->in_count);
    TEST_ASSERT_EQUAL_UINT32(1u, rectangle->out_count);
    /* The self loop is listed exactly once, only in `self`. */
    TEST_ASSERT_EQUAL_UINT32(1u, count_rows(square->self, square->self_count));
    TEST_ASSERT_EQUAL_STRING("equivalent", square->self[0]->type);
    TEST_ASSERT_EQUAL_UINT32(square->self[0]->subject,
                             square->self[0]->object);
    TEST_ASSERT_EQUAL_STRING("is_a", square->out[0]->type);
    TEST_ASSERT_EQUAL_STRING("is_a", square->out[1]->type);
    TEST_ASSERT_EQUAL_STRING("part_of", square->in[0]->type);
    /* Relation order is the declaration order inside one file. */
    TEST_ASSERT_TRUE(yan_kg_relation_at(&graph, 0u)->where.declaration_start <
                     yan_kg_relation_at(&graph, 1u)->where.declaration_start);
}

/* -------------------------------------------------------- byte/line provenance */

/* The declaration range is the line without its terminator, and the block range
 * is the whole block content with its first and last physical lines. */
static void test_provenance_bytes_and_lines_lf(void)
{
    static const char *text =
        "@yan-note 1\n"
        "title: 几何\n"
        "@end\n"
        "@block definitions\n"
        "@kg concept square 正方形\n"
        "正方形满足矩形定义。\n";
    fake_add("note", 0u, text);
    YanKgSummary summary;
    TEST_ASSERT_EQUAL(YAN_KG_OK, yan_kg_build(&graph, &source, &summary));
    const YanKgConcept *square = concept("square");
    TEST_ASSERT_NOT_NULL(square);
    TEST_ASSERT_EQUAL_UINT32(1u, square->declaration_count);
    const YanKgProvenance *where = &square->declarations[0];
    TEST_ASSERT_EQUAL_STRING("note", where->file);
    TEST_ASSERT_EQUAL_STRING("definitions", where->block_id);
    /* The marker is physical line 4, the content starts on line 5. */
    TEST_ASSERT_EQUAL_UINT32(4u, where->block_marker_line);
    TEST_ASSERT_EQUAL_UINT32(5u, where->block_first_line);
    TEST_ASSERT_EQUAL_UINT32(6u, where->block_last_line);
    TEST_ASSERT_EQUAL_UINT32(5u, where->declaration_line);
    /* The block runs to EOF; the declaration range is exactly the directive
     * text without its LF. Both are re-derived from the fixture bytes. */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)strlen(text), where->block_end);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)strlen("@kg concept square 正方形"),
                             where->declaration_end - where->declaration_start);
    TEST_ASSERT_EQUAL_CHAR('\n', text[where->declaration_end]);
    TEST_ASSERT_EQUAL_MEMORY("@kg concept square 正方形",
                             text + where->declaration_start,
                             where->declaration_end - where->declaration_start);
    TEST_ASSERT_EQUAL_MEMORY("@kg concept square 正方形\n正方形满足矩形定义。\n",
                             text + where->block_start,
                             where->block_end - where->block_start);
}

/* CRLF files keep physical line counting and exclude CR and LF from the range. */
static void test_provenance_bytes_and_lines_crlf(void)
{
    static const char *text =
        "@block b\r\n"
        "@kg concept square 正方形\r\n"
        "\xd0\x9e\xd0\xba\r\n";
    fake_add("crlf-note", 0u, text);
    YanKgSummary summary;
    TEST_ASSERT_EQUAL(YAN_KG_OK, yan_kg_build(&graph, &source, &summary));
    const YanKgConcept *square = concept("square");
    TEST_ASSERT_NOT_NULL(square);
    const YanKgProvenance *where = &square->declarations[0];
    TEST_ASSERT_EQUAL_UINT32(1u, where->block_marker_line);
    TEST_ASSERT_EQUAL_UINT32(2u, where->declaration_line);
    TEST_ASSERT_EQUAL_UINT32(2u, where->block_first_line);
    TEST_ASSERT_EQUAL_UINT32(3u, where->block_last_line);
    TEST_ASSERT_EQUAL_MEMORY("@kg concept square 正方形",
                             text + where->declaration_start,
                             where->declaration_end - where->declaration_start);
    TEST_ASSERT_EQUAL_UINT32(where->block_end, (uint32_t)strlen(text));
}

/* A final line without a newline is legal and its range ends at EOF. */
static void test_final_line_without_newline(void)
{
    static const char *text = "@block b\n@kg concept square 正方形";
    fake_add("note", 0u, text);
    YanKgSummary summary;
    TEST_ASSERT_EQUAL(YAN_KG_OK, yan_kg_build(&graph, &source, &summary));
    const YanKgConcept *square = concept("square");
    TEST_ASSERT_NOT_NULL(square);
    const YanKgProvenance *where = &square->declarations[0];
    TEST_ASSERT_EQUAL_UINT32(where->declaration_start +
                                 (uint32_t)strlen("@kg concept square 正方形"),
                             where->declaration_end);
    TEST_ASSERT_EQUAL_UINT32(where->declaration_end, where->block_end);
    TEST_ASSERT_EQUAL_UINT32(2u, where->declaration_line);
}

/* An ordinary prose line far past the editor's 16 KiB draft limit is legal text
 * and does not disturb block or directive scanning. */
static void test_long_ordinary_line_beyond_editor_limit(void)
{
    /* Built from fixed byte arrays with known lengths: the marker prefix, the
     * long prose run, then the directive suffix. calloc supplies the final NUL,
     * and no byte or offset differs from the spelled-out fixture. */
    static const char prefix[] = "@block big\n";
    static const char suffix[] = "\n@kg concept square 正方形\n";
    const uint32_t prefix_length = (uint32_t)sizeof prefix - 1u;
    const uint32_t suffix_length = (uint32_t)sizeof suffix - 1u;
    char *text = calloc((size_t)prefix_length + LONG_BODY + suffix_length + 1u, 1u);
    TEST_ASSERT_NOT_NULL(text);
    memcpy(text, prefix, prefix_length);
    memset(text + prefix_length, 'L', LONG_BODY);
    memcpy(text + prefix_length + LONG_BODY, suffix, suffix_length);
    fake_add_owned("long", 0u, text);
    YanKgSummary summary;
    TEST_ASSERT_EQUAL(YAN_KG_OK, yan_kg_build(&graph, &source, &summary));
    const YanKgConcept *square = concept("square");
    TEST_ASSERT_NOT_NULL(square);
    TEST_ASSERT_EQUAL_UINT32(3u, square->declarations[0].declaration_line);
    TEST_ASSERT_EQUAL_UINT32(2u, square->declarations[0].block_first_line);
    TEST_ASSERT_EQUAL_UINT32(3u, square->declarations[0].block_last_line);
    /* "@block big\n" + LONG_BODY bytes + "\n" precede the directive. */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)strlen("@block big\n") + LONG_BODY + 1u,
                             square->declarations[0].declaration_start);
    /* The block content starts right after the marker line, not at the
     * directive, which is separated from it by the long prose line. */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)strlen("@block big\n"),
                             square->declarations[0].block_start);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)strlen("@block big\n") + LONG_BODY + 1u
                                 + (uint32_t)strlen("@kg concept square 正方形") + 1u,
                             square->declarations[0].block_end);
}

/* ------------------------------------------------------------------ grammar */

/* Leading space, `@kgx` and `@ kg` are ordinary prose; an unknown command word,
 * a missing block and a malformed field are GRAPH_FORMAT. */
static void test_non_reserved_lines_are_prose(void)
{
    fake_add("note", 0u,
             "@block a\n"
             " @kg concept square 正方形\n"
             "@kgx concept square 正方形\n"
             "@ kg concept square 正方形\n"
             "@kgconcept\n"
             "正文 @kg concept square 正方形\n");
    YanKgSummary summary;
    TEST_ASSERT_EQUAL(YAN_KG_OK, yan_kg_build(&graph, &source, &summary));
    TEST_ASSERT_EQUAL_UINT32(0u, summary.concepts);
}

static void test_unknown_command_is_graph_format(void)
{
    fake_add("note", 0u, "@block a\n@kg property square 正方形\n");
    expect_result(YAN_KG_GRAPH_FORMAT);
}

static void test_reserved_line_without_block_is_graph_format(void)
{
    fake_add("note", 0u, "@kg concept square 正方形\n");
    expect_result(YAN_KG_GRAPH_FORMAT);
    /* The same directive inside a block is fine, so the failure is the missing
     * block, not the directive. */
    fake_reset();
    fake_add("note", 0u, "@block a\n@kg concept square 正方形\n");
    expect_result(YAN_KG_OK);
}

static void test_bare_marker_is_graph_format(void)
{
    fake_add("note", 0u, "@block a\n@kg\n");
    expect_result(YAN_KG_GRAPH_FORMAT);
    fake_reset();
    fake_add("note", 0u, "@block a\n@kg \n");
    expect_result(YAN_KG_GRAPH_FORMAT);
    fake_reset();
    fake_add("note", 0u, "@block a\n@kg\tconcept square 正方形\n");
    expect_result(YAN_KG_GRAPH_FORMAT);
}

static void test_field_separator_rules(void)
{
    /* Two spaces after the command word leaves an empty ID field. */
    fake_add("note", 0u, "@block a\n@kg concept  square 正方形\n");
    expect_result(YAN_KG_GRAPH_FORMAT);
    fake_reset();
    /* A trailing space on a relation is a fourth empty field. */
    fake_add("note", 0u,
             "@block a\n@kg concept a A\n@kg concept b B\n"
             "@kg relation is_a a b \n");
    expect_result(YAN_KG_GRAPH_FORMAT);
    fake_reset();
    /* A relation needs exactly three fields. */
    fake_add("note", 0u,
             "@block a\n@kg concept a A\n@kg relation is_a a\n");
    expect_result(YAN_KG_GRAPH_FORMAT);
    fake_reset();
    /* An over-long ID is refused. */
    fake_add("note", 0u,
             "@block a\n@kg concept abcdefghijabcdefghijabcdefghijab A\n");
    expect_result(YAN_KG_GRAPH_FORMAT);
}

static void test_label_boundaries(void)
{
    /* A 256-byte label is legal, 257 is not. */
    char text[512];
    char label[302];
    memset(label, 'x', 256u);
    label[256] = '\0';
    snprintf(text, sizeof text, "@block a\n@kg concept square %s\n", label);
    fake_add("note", 0u, text);
    YanKgSummary summary;
    TEST_ASSERT_EQUAL(YAN_KG_OK, yan_kg_build(&graph, &source, &summary));
    TEST_ASSERT_EQUAL_UINT32(256u, (uint32_t)strlen(concept("square")->name));

    memset(label, 'y', 257u);
    label[257] = '\0';
    snprintf(text, sizeof text, "@block a\n@kg concept other %s\n", label);
    fake_reset();
    fake_add("note", 0u, text);
    expect_result(YAN_KG_GRAPH_FORMAT);

    /* A label may contain interior spaces but not a leading or trailing one. */
    fake_reset();
    fake_add("note", 0u, "@block a\n@kg concept square 正方形 与 矩形\n");
    TEST_ASSERT_EQUAL(YAN_KG_OK, yan_kg_build(&graph, &source, &summary));
    TEST_ASSERT_EQUAL_STRING("正方形 与 矩形", concept("square")->name);
}

/* --------------------------------------------------- text and file preflight */

/* A binary byte anywhere in the file skips the whole file: no concept, no
 * error, and the skipped count reports it. */
static void test_binary_file_is_skipped_whole(void)
{
    static const char binary[] = "@block a\n@kg concept square 正方形\n\0 tail";
    fake_add("binary", 0u, binary);
    /* strlen() stops at the NUL, so the size is set explicitly here. */
    source_state.files[0].size = (uint32_t)sizeof binary - 1u;
    fake_add("good", 1u, "@block a\n@kg concept rectangle 矩形\n");
    YanKgSummary summary;
    TEST_ASSERT_EQUAL(YAN_KG_OK, yan_kg_build(&graph, &source, &summary));
    TEST_ASSERT_EQUAL_UINT32(1u, summary.concepts);
    TEST_ASSERT_EQUAL_UINT32(1u, summary.skipped);
    TEST_ASSERT_EQUAL_UINT32(2u, summary.files);
    TEST_ASSERT_NULL(concept("square"));
    TEST_ASSERT_NOT_NULL(concept("rectangle"));
}

/* A bare CR, a truncated scalar and a DEL byte are all text violations, so the
 * whole file is skipped rather than partially parsed. */
static void test_text_violations_skip_the_file(void)
{
    fake_add("barecr", 0u, "@block a\n@kg concept a A\rx\n");
    fake_add("truncated", 1u, "@block a\n@kg concept b B\n\xf0\x9f");
    fake_add("del", 2u, "@block a\n@kg concept c C\n\x7f\n");
    YanKgSummary summary;
    TEST_ASSERT_EQUAL(YAN_KG_OK, yan_kg_build(&graph, &source, &summary));
    TEST_ASSERT_EQUAL_UINT32(0u, summary.concepts);
    TEST_ASSERT_EQUAL_UINT32(3u, summary.skipped);
}

/* A broken header on legal text is METADATA: the build fails rather than
 * guessing an empty header. */
static void test_broken_header_is_metadata(void)
{
    fake_add("bad", 0u, "@yan-note 1\n@block a\n@kg concept a A\n@end\n");
    expect_result(YAN_KG_METADATA);
}

/* A broken block marker is BLOCK_FORMAT even when no directive is present. */
static void test_broken_block_marker_is_block_format(void)
{
    fake_add("bad", 0u, "@block\n");
    expect_result(YAN_KG_BLOCK_FORMAT);
    fake_reset();
    fake_add("bad", 0u, "@block\ttab\n");
    expect_result(YAN_KG_BLOCK_FORMAT);
}

/* A duplicate block ID inside one file is BLOCK_AMBIGUOUS. */
static void test_duplicate_block_id_is_block_ambiguous(void)
{
    fake_add("bad", 0u, "@block a\n@block a\n");
    expect_result(YAN_KG_BLOCK_AMBIGUOUS);
}

/* The graph header is not scanned: a `@kg`-looking line inside the 0027 header
 * is a header field violation, not a directive. */
static void test_header_is_not_scanned_for_directives(void)
{
    fake_add("note", 0u,
             "@yan-note 1\ntitle: 标题\n@end\n@block a\n"
             "@kg concept square 正方形\n");
    YanKgSummary summary;
    TEST_ASSERT_EQUAL(YAN_KG_OK, yan_kg_build(&graph, &source, &summary));
    TEST_ASSERT_EQUAL_UINT32(1u, summary.concepts);
    /* The header occupies lines 1..3, so the block body starts on line 5. */
    TEST_ASSERT_EQUAL_UINT32(4u, concept("square")->declarations[0].block_marker_line);
}

/* ---------------------------------------------------------------- caps */

/* Each cap is checked at its boundary and one past it: the boundary build must
 * succeed with the exact documented count and the over-cap build must be LIMIT
 * with the previous graph left serving. Generated fixtures set the file size
 * explicitly because they contain no NUL terminator. */

typedef struct {
    char *bytes;
    uint32_t size;
    uint32_t capacity;
} Builder;

static void builder_begin(Builder *builder, size_t capacity)
{
    builder->bytes = calloc(capacity, 1u);
    TEST_ASSERT_NOT_NULL(builder->bytes);
    memset(builder->bytes, 0, capacity);
    builder->size = 0u;
    builder->capacity = (uint32_t)capacity;
}

static void builder_add(Builder *builder, const char *text)
{
    size_t length = strlen(text);
    TEST_ASSERT_TRUE((size_t)builder->size + length < builder->capacity);
    memcpy(builder->bytes + builder->size, text, length);
    builder->size += (uint32_t)length;
}

/* strdup is POSIX, and the suite is built as strict C17 on both platforms, so
 * the owned copy is spelled out. */
static char *builder_detach(Builder *builder)
{
    size_t length = (size_t)builder->size + 1u;
    char *copy = malloc(length);
    TEST_ASSERT_NOT_NULL(copy);
    memcpy(copy, builder->bytes, length);
    return copy;
}

static void builder_add_block(Builder *builder, uint32_t ordinal)
{
    char line[64];
    snprintf(line, sizeof line, "@block b%02u\n", ordinal % 64u);
    builder_add(builder, line);
}

/* Adds `count` distinct concepts, one per block line, with distinct block IDs
 * inside each file. */
static void builder_add_concepts(Builder *builder, uint32_t first,
                                 uint32_t count)
{
    for (uint32_t index = 0u; index < count; ++index) {
        if (index % 64u == 0u) {
            builder_add_block(builder, index / 64u);
        }
        char line[64];
        snprintf(line, sizeof line, "@kg concept c%05u 名称%05u\n",
                 first + index, first + index);
        builder_add(builder, line);
    }
}

static void test_concept_cap_boundary(void)
{
    /* 8192 concepts is 128 files of 64 blocks. */
    source_state.capacity = 4095u;
    source.file_capacity = 4095u;
    static char names[129][32];
    Builder builder;
    /* 64 blocks + 64 concepts per file, 64 bytes each with room to spare. */
    builder_begin(&builder, 64u * 64u + 256u);
    uint32_t total = 0u;
    for (uint32_t file = 0u; file < 128u; ++file) {
        builder.size = 0u;
        builder_add_concepts(&builder, total, 64u);
                snprintf(names[file], sizeof names[file], "f%03u", file);
        fake_add_owned(names[file], file, builder_detach(&builder));
        total += 64u;
    }
    free(builder.bytes);
    TEST_ASSERT_EQUAL_UINT32(YAN_KG_CONCEPT_MAX, total);
    YanKgSummary summary;
    TEST_ASSERT_EQUAL(YAN_KG_OK, yan_kg_build(&graph, &source, &summary));
    TEST_ASSERT_EQUAL_UINT32(YAN_KG_CONCEPT_MAX, summary.concepts);
    TEST_ASSERT_EQUAL_UINT32(YAN_KG_CONCEPT_MAX, summary.declarations);
    TEST_ASSERT_EQUAL_UINT32(YAN_KG_CONCEPT_MAX, yan_kg_concept_count(&graph));
    TEST_ASSERT_EQUAL_STRING("名称00000", concept("c00000")->name);

    /* One more concept in one more file is LIMIT; the 8192-concept graph keeps
     * serving unchanged. */
    builder_begin(&builder, 256u);
    builder_add_concepts(&builder, 99999u, 1u);
        fake_add_owned("zzz", 128u, builder_detach(&builder));
    free(builder.bytes);
    expect_result(YAN_KG_LIMIT);
    TEST_ASSERT_EQUAL_UINT32(YAN_KG_CONCEPT_MAX, yan_kg_concept_count(&graph));
    TEST_ASSERT_NOT_NULL(concept("c00000"));
}

static void test_declaration_cap_boundary(void)
{
    /* 16384 provenance entries for ONE concept span 4 files of 64 blocks. */
    source_state.capacity = 4095u;
    source.file_capacity = 4095u;
    static char names[8][32];
    Builder builder;
    builder_begin(&builder, 4096u * 24u + 4096u);
    for (uint32_t file = 0u; file < 4u; ++file) {
        builder.size = 0u;
        for (uint32_t index = 0u; index < 4096u; ++index) {
            if (index % 64u == 0u) {
                builder_add_block(&builder, index / 64u);
            }
            builder_add(&builder, "@kg concept same 同一\n");
        }
                snprintf(names[file], sizeof names[file], "d%u", file);
        fake_add_owned(names[file], file, builder_detach(&builder));
    }
    free(builder.bytes);
    YanKgSummary summary;
    TEST_ASSERT_EQUAL(YAN_KG_OK, yan_kg_build(&graph, &source, &summary));
    TEST_ASSERT_EQUAL_UINT32(1u, summary.concepts);
    TEST_ASSERT_EQUAL_UINT32(0u, summary.relations);
    TEST_ASSERT_EQUAL_UINT32(YAN_KG_DECLARATION_MAX, summary.declarations);
    TEST_ASSERT_EQUAL_UINT32(YAN_KG_DECLARATION_MAX,
                             concept("same")->declaration_count);

    /* One more declaration of the same concept is LIMIT, not a truncation. */
    builder_begin(&builder, 512u);
    builder_add(&builder, "@block extra\n@kg concept same 同一\n");
        fake_add_owned("extra", 4u, builder_detach(&builder));
    free(builder.bytes);
    expect_result(YAN_KG_LIMIT);
    TEST_ASSERT_EQUAL_UINT32(YAN_KG_DECLARATION_MAX,
                             concept("same")->declaration_count);
}

static void test_alias_cap_boundary(void)
{
    source_state.capacity = 4095u;
    source.file_capacity = 4095u;
    Builder builder;
    /* "@kg alias base 别名NNNNN\n" is 15 + 14 + 1 = 30 bytes. */
    builder_begin(&builder, (size_t)16385u * 32u + 4096u);
    builder_add(&builder, "@block a\n@kg concept base 基础\n");
    for (uint32_t index = 0u; index < YAN_KG_ALIAS_MAX; ++index) {
        char line[48];
        snprintf(line, sizeof line, "@kg alias base 别名%05u\n", index);
        builder_add(&builder, line);
    }
    fake_add_owned("aliases", 0u, builder_detach(&builder));
    free(builder.bytes);
    YanKgSummary summary;
    TEST_ASSERT_EQUAL(YAN_KG_OK, yan_kg_build(&graph, &source, &summary));
    TEST_ASSERT_EQUAL_UINT32(YAN_KG_ALIAS_MAX, summary.aliases);
    TEST_ASSERT_EQUAL_UINT32(YAN_KG_ALIAS_MAX, concept("base")->alias_count);
    /* Aliases are published in label order, so the first one is 别名00000. */
    TEST_ASSERT_EQUAL_STRING("别名00000", concept("base")->aliases[0]);

    /* A byte-identical repeat of an existing pair consumes no cap. */
    fake_reset();
    builder_begin(&builder, (size_t)16385u * 32u + 4096u);
    builder_add(&builder, "@block a\n@kg concept base 基础\n");
    builder_add(&builder, "@kg alias base 别名00000\n");
    for (uint32_t index = 1u; index < YAN_KG_ALIAS_MAX; ++index) {
        char line[48];
        snprintf(line, sizeof line, "@kg alias base 别名%05u\n", index);
        builder_add(&builder, line);
    }
    fake_add_owned("aliases", 0u, builder_detach(&builder));
    free(builder.bytes);
    TEST_ASSERT_EQUAL(YAN_KG_OK, yan_kg_build(&graph, &source, &summary));
    TEST_ASSERT_EQUAL_UINT32(YAN_KG_ALIAS_MAX, summary.aliases);

    /* One more DISTINCT alias is LIMIT and the previous graph keeps serving. */
    fake_reset();
    builder_begin(&builder, (size_t)16386u * 32u + 4096u);
    builder_add(&builder, "@block a\n@kg concept base 基础\n");
    for (uint32_t index = 0u; index <= YAN_KG_ALIAS_MAX; ++index) {
        char line[48];
        snprintf(line, sizeof line, "@kg alias base 别名%05u\n", index);
        builder_add(&builder, line);
    }
    fake_add_owned("aliases", 0u, builder_detach(&builder));
    free(builder.bytes);
    expect_result(YAN_KG_LIMIT);
    TEST_ASSERT_EQUAL_UINT32(YAN_KG_ALIAS_MAX, concept("base")->alias_count);
    TEST_ASSERT_EQUAL_STRING("别名00000", concept("base")->aliases[0]);
}

static void test_relation_cap_boundary(void)
{
    /* 16384 propositions over 4 files; they all share the same two endpoints so
     * the adjacency lists carry exactly the cap. */
    source_state.capacity = 4095u;
    source.file_capacity = 4095u;
    static char names[8][32];
    Builder builder;
    builder_begin(&builder, 4096u * 24u + 4096u);
    for (uint32_t file = 0u; file < 4u; ++file) {
        builder.size = 0u;
        builder_add(&builder, "@block defs\n@kg concept a A\n@kg concept b B\n");
        for (uint32_t index = 0u; index < 4096u; ++index) {
            /* 4096 propositions per file at 128 per block is 32 blocks, well
             * under the 0032 cap of 64 blocks for one file. */
            if (index % 128u == 0u && index != 0u) {
                builder_add_block(&builder, index / 128u);
            }
            builder_add(&builder, "@kg relation t a b\n");
        }
                snprintf(names[file], sizeof names[file], "r%u", file);
        fake_add_owned(names[file], file, builder_detach(&builder));
    }
    free(builder.bytes);
    YanKgSummary summary;
    TEST_ASSERT_EQUAL(YAN_KG_OK, yan_kg_build(&graph, &source, &summary));
    TEST_ASSERT_EQUAL_UINT32(2u, summary.concepts);
    TEST_ASSERT_EQUAL_UINT32(YAN_KG_RELATION_MAX, summary.relations);
    TEST_ASSERT_EQUAL_UINT32(YAN_KG_RELATION_MAX, yan_kg_relation_count(&graph));
    const YanKgConcept *a = concept("a");
    const YanKgConcept *b = concept("b");
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_UINT32(YAN_KG_RELATION_MAX, a->out_count);
    TEST_ASSERT_EQUAL_UINT32(0u, a->in_count);
    TEST_ASSERT_EQUAL_UINT32(YAN_KG_RELATION_MAX, b->in_count);

    /* One more proposition is LIMIT; the graph is preserved. */
    builder_begin(&builder, 512u);
    builder_add(&builder, "@block extra\n@kg relation t a b\n");
        fake_add_owned("extra", 4u, builder_detach(&builder));
    free(builder.bytes);
    expect_result(YAN_KG_LIMIT);
    TEST_ASSERT_EQUAL_UINT32(YAN_KG_RELATION_MAX, yan_kg_relation_count(&graph));
    TEST_ASSERT_EQUAL_UINT32(YAN_KG_RELATION_MAX, concept("a")->out_count);
}

/* --------------------------------------------------------------- lifecycle */

/* A failed build leaves the previously published graph byte identical and the
 * summary untouched, and yan_kg_destroy is safe twice. */
static void test_failure_preserves_previous_graph(void)
{
    fake_add("good", 0u,
             "@block a\n@kg concept square 正方形\n"
             "@kg relation is_a square square\n");
    YanKgSummary first;
    TEST_ASSERT_EQUAL(YAN_KG_OK, yan_kg_build(&graph, &source, &first));
    TEST_ASSERT_EQUAL_UINT32(1u, first.concepts);
    const YanKgConcept *before = concept("square");
    TEST_ASSERT_NOT_NULL(before);
    uint32_t declarations = before->declaration_count;
    uint32_t relations = yan_kg_relation_count(&graph);

    /* Now make the library unbuildable. */
    fake_add("bad", 1u, "@block a\n@kg relation is_a square missing\n");
    YanKgSummary second;
    memset(&second, 0, sizeof second);
    second.concepts = 777u;
    TEST_ASSERT_EQUAL(YAN_KG_UNRESOLVED,
                      yan_kg_build(&graph, &source, &second));
    TEST_ASSERT_EQUAL_UINT32(777u, second.concepts); /* untouched */

    const YanKgConcept *after = concept("square");
    TEST_ASSERT_EQUAL_PTR(before, after);
    TEST_ASSERT_EQUAL_UINT32(declarations, after->declaration_count);
    TEST_ASSERT_EQUAL_UINT32(relations, yan_kg_relation_count(&graph));
    TEST_ASSERT_EQUAL_STRING("正方形", after->name);

    YanKgSummary observed;
    memset(&observed, 0, sizeof observed);
    yan_kg_summary(&graph, &observed);
    TEST_ASSERT_EQUAL_UINT32(1u, observed.concepts);

    yan_kg_destroy(&graph);
    yan_kg_destroy(&graph); /* idempotent */
    TEST_ASSERT_EQUAL_UINT32(0u, yan_kg_concept_count(&graph));
    TEST_ASSERT_NULL(yan_kg_concept_at(&graph, 0u));
}

/* A source read failure is IO and publishes nothing. */
static void test_source_failure_is_io(void)
{
    fake_add("note", 0u, "@block a\n@kg concept a A\n");
    source_state.read_fail = true;
    expect_result(YAN_KG_IO);
    TEST_ASSERT_EQUAL_UINT32(0u, yan_kg_concept_count(&graph));
}

/* An invalid lookup argument is INVALID; a legal but absent one is NOT_FOUND. */
static void test_lookup_argument_rules(void)
{
    fake_add("note", 0u, "@block a\n@kg concept a A\n");
    YanKgSummary summary;
    TEST_ASSERT_EQUAL(YAN_KG_OK, yan_kg_build(&graph, &source, &summary));
    YanKgMatch match;
    memset(&match, 0, sizeof match);
    TEST_ASSERT_EQUAL(YAN_KG_INVALID,
                      yan_kg_lookup(&graph, NULL, 0u, &match));
    TEST_ASSERT_EQUAL(YAN_KG_INVALID,
                      yan_kg_lookup(&graph, (const uint8_t *)"a", 0u, &match));
    TEST_ASSERT_EQUAL(YAN_KG_INVALID,
                      yan_kg_lookup(&graph, (const uint8_t *)"a\n", 2u, &match));
    TEST_ASSERT_EQUAL(YAN_KG_INVALID,
                      yan_kg_lookup(&graph, (const uint8_t *)"   ", 3u, &match));
    TEST_ASSERT_EQUAL(YAN_KG_INVALID, yan_kg_lookup(&graph, (const uint8_t *)"a",
                                                    1u, NULL));
    /* A 257-byte query is over the R1/R2 bound. */
    uint8_t long_query[258];
    memset(long_query, 'q', sizeof long_query);
    TEST_ASSERT_EQUAL(YAN_KG_INVALID,
                      yan_kg_lookup(&graph, long_query, 257u, &match));
}

/* Concepts are published in ASCII ID order, not declaration order. */
static void test_concept_order_is_id_order(void)
{
    fake_add("note", 0u,
             "@block a\n@kg concept zulu Z\n@kg concept alpha A\n"
             "@kg concept Mike M\n@kg concept beta B\n");
    YanKgSummary summary;
    TEST_ASSERT_EQUAL(YAN_KG_OK, yan_kg_build(&graph, &source, &summary));
    TEST_ASSERT_EQUAL_UINT32(4u, yan_kg_concept_count(&graph));
    TEST_ASSERT_EQUAL_STRING("Mike", yan_kg_concept_at(&graph, 0u)->id);
    TEST_ASSERT_EQUAL_STRING("alpha", yan_kg_concept_at(&graph, 1u)->id);
    TEST_ASSERT_EQUAL_STRING("beta", yan_kg_concept_at(&graph, 2u)->id);
    TEST_ASSERT_EQUAL_STRING("zulu", yan_kg_concept_at(&graph, 3u)->id);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_empty_library_and_old_notes);
    RUN_TEST(test_same_concept_in_two_files_merges_and_keeps_both);
    RUN_TEST(test_same_id_different_label_is_conflict);
    RUN_TEST(test_repeated_identical_declaration_keeps_both_origins);
    RUN_TEST(test_forward_references_resolve);
    RUN_TEST(test_unresolved_object);
    RUN_TEST(test_unresolved_alias);
    RUN_TEST(test_exact_id_beats_label);
    RUN_TEST(test_label_and_alias_unique_match);
    RUN_TEST(test_shared_label_is_ambiguous);
    RUN_TEST(test_name_equals_alias_of_same_concept);
    RUN_TEST(test_missing_label_is_not_found);
    RUN_TEST(test_many_concepts_share_one_label);
    RUN_TEST(test_aliases_only_shared_by_three_concepts);
    RUN_TEST(test_duplicates_types_cycles_and_self_loops);
    RUN_TEST(test_provenance_bytes_and_lines_lf);
    RUN_TEST(test_provenance_bytes_and_lines_crlf);
    RUN_TEST(test_final_line_without_newline);
    RUN_TEST(test_long_ordinary_line_beyond_editor_limit);
    RUN_TEST(test_non_reserved_lines_are_prose);
    RUN_TEST(test_unknown_command_is_graph_format);
    RUN_TEST(test_reserved_line_without_block_is_graph_format);
    RUN_TEST(test_bare_marker_is_graph_format);
    RUN_TEST(test_field_separator_rules);
    RUN_TEST(test_label_boundaries);
    RUN_TEST(test_binary_file_is_skipped_whole);
    RUN_TEST(test_text_violations_skip_the_file);
    RUN_TEST(test_broken_header_is_metadata);
    RUN_TEST(test_broken_block_marker_is_block_format);
    RUN_TEST(test_duplicate_block_id_is_block_ambiguous);
    RUN_TEST(test_header_is_not_scanned_for_directives);
    RUN_TEST(test_concept_cap_boundary);
    RUN_TEST(test_declaration_cap_boundary);
    RUN_TEST(test_alias_cap_boundary);
    RUN_TEST(test_relation_cap_boundary);
    RUN_TEST(test_failure_preserves_previous_graph);
    RUN_TEST(test_source_failure_is_io);
    RUN_TEST(test_lookup_argument_rules);
    RUN_TEST(test_concept_order_is_id_order);
    return UNITY_END();
}
