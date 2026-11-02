/* 0044 R1 allocation-failure proof.
 *
 * Contract: docs/specs/0044-evidence-knowledge-graph.md R1 — "内存分配失败为
 * NOSPACE" and "构建失败不发布候选图，已有成功图保持".
 *
 * The production core has no fault switch and none is added here: this file is
 * linked only into `test_knowledge_graph_alloc`, and the linker's
 * `--wrap=malloc` / `--wrap=calloc` lets the test fail exactly one allocation of
 * a rebuild while the real `host/knowledge_graph.c` runs unchanged.
 *
 * The proof is a sweep over every allocation ordinal one rebuild performs,
 * workspace and publication allocations alike:
 *
 *   1. build a valid graph once from a borrowed in-memory source;
 *   2. measure how many allocations one more successful rebuild performs (this
 *      is the sweep bound, not a guess) and capture every reference value from
 *      the graph that rebuild left published;
 *   3. for each ordinal 1..bound, fail only that allocation and require
 *      NOSPACE, an unchanged published graph (counts, summary, node pointers,
 *      lookup result and original provenance) and no crash;
 *   4. require that one allocation past the bound succeeds, which proves the
 *      bound was exact rather than an over-estimate hiding later ordinals.
 *
 * Injection is confined to the measured window, so the test's own fixture
 * buffers, the Unity framework and stdout can never be handed a NULL. All
 * assertions run after the window is closed and the fault cleared. */

#include "knowledge_graph.h"
#include "unity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ fault seam */

static bool counting;      /* true only inside a measured window */
static uint32_t calls;     /* allocation ordinal inside the window */
static uint32_t fault_at;  /* 0 disables injection, else the ordinal to fail */

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);

static bool should_fail(void)
{
    if (!counting) {
        return false;
    }
    ++calls;
    return fault_at != 0u && calls == fault_at;
}

void *__wrap_malloc(size_t size);
void *__wrap_malloc(size_t size)
{
    if (should_fail()) {
        return NULL;
    }
    return __real_malloc(size);
}

void *__wrap_calloc(size_t count, size_t size);
void *__wrap_calloc(size_t count, size_t size)
{
    if (should_fail()) {
        return NULL;
    }
    return __real_calloc(count, size);
}

/* ---------------------------------------------------------------------- fixture */

/* One note in one named block: two concepts, one alias and two propositions, so a
 * rebuild exercises the concept, alias, relation and adjacency publications. */
static const char note_text[] =
    "@block definitions\n"
    "@kg concept square 正方形\n"
    "@kg concept rectangle 矩形\n"
    "@kg alias square 等边矩形\n"
    "@kg relation is_a square rectangle\n"
    "@kg relation equivalent square square\n";

static const char note_name[] = "a-note";

static YanSemanticResult fake_next(void *opaque, uint32_t *cursor,
                                   YanSemanticFile *out)
{
    (void)opaque;
    if (*cursor != 0u) {
        return YAN_SEMANTIC_MISSING;
    }
    memset(out, 0, sizeof *out);
    memcpy(out->name, note_name, sizeof note_name);
    out->slot = 0u;
    out->size = (uint32_t)(sizeof note_text - 1u);
    ++*cursor;
    return YAN_SEMANTIC_OK;
}

static YanSemanticResult fake_read(void *opaque, const char *name,
                                   uint32_t offset, uint8_t *out,
                                   uint32_t length)
{
    (void)opaque;
    if (strcmp(name, note_name) != 0 ||
        offset > sizeof note_text - 1u ||
        length > sizeof note_text - 1u - offset) {
        return YAN_SEMANTIC_IO;
    }
    memcpy(out, note_text + offset, length);
    return YAN_SEMANTIC_OK;
}

static YanSemanticSource source;
static YanKg graph;

void setUp(void)
{
    memset(&graph, 0, sizeof graph);
    yan_kg_destroy(&graph);
    source = (YanSemanticSource){NULL, 0u, fake_next, fake_read, 63u};
    counting = false;
    calls = 0u;
    fault_at = 0u;
}

void tearDown(void)
{
    counting = false;
    fault_at = 0u;
    yan_kg_destroy(&graph);
}

/* One rebuild inside a measured window. The window closes before the caller
 * asserts anything, and the requested ordinal is reported through `injected` so
 * the caller can tell "the failure happened" from "the ordinal does not exist". */
static YanKgResult rebuild_with_fault(uint32_t ordinal, uint32_t *observed_calls,
                                      bool *injected)
{
    YanKgSummary summary;
    memset(&summary, 0, sizeof summary);
    calls = 0u;
    fault_at = ordinal;
    counting = true;
    YanKgResult result = yan_kg_build(&graph, &source, &summary);
    counting = false;
    fault_at = 0u;
    if (observed_calls != NULL) {
        *observed_calls = calls;
    }
    if (injected != NULL) {
        *injected = ordinal != 0u && calls >= ordinal;
    }
    return result;
}

/* Copies a bounded NUL-terminated string out of a published view. */
static void copy_text(const char *text, char *out, size_t capacity)
{
    size_t length = strlen(text);
    TEST_ASSERT_TRUE(length < capacity);
    memset(out, 0, capacity);
    memcpy(out, text, length);
}

/* ------------------------------------------------------------------------ proof */

static void test_every_allocation_failure_reports_nospace_and_keeps_graph(void)
{
    /* 1. A valid published graph. */
    YanKgSummary summary;
    memset(&summary, 0, sizeof summary);
    TEST_ASSERT_EQUAL(YAN_KG_OK, yan_kg_build(&graph, &source, &summary));
    TEST_ASSERT_EQUAL_UINT32(2u, summary.concepts);
    TEST_ASSERT_EQUAL_UINT32(2u, summary.relations);
    TEST_ASSERT_EQUAL_UINT32(1u, summary.aliases);
    TEST_ASSERT_EQUAL_UINT32(2u, summary.declarations);
    TEST_ASSERT_EQUAL_UINT32(1u, summary.files);
    TEST_ASSERT_EQUAL_UINT32(0u, summary.skipped);

    /* 2. Measure one rebuild's allocation count, then capture the reference
     * publication it left in place: a successful rebuild replaces the graph, so
     * every reference below is read AFTER this call, never before it. */
    uint32_t bound = 0u;
    bool injected = false;
    TEST_ASSERT_EQUAL(YAN_KG_OK, rebuild_with_fault(0u, &bound, &injected));
    TEST_ASSERT_FALSE(injected);
    /* A rebuild allocates both its workspace and the whole publication, so the
     * sweep must cover more than a handful of ordinals; the exact count is an
     * implementation detail and is deliberately not pinned. */
    TEST_ASSERT_GREATER_THAN_UINT32(8u, bound);

    const uint32_t old_concepts = yan_kg_concept_count(&graph);
    const uint32_t old_relations = yan_kg_relation_count(&graph);
    const YanKgConcept *old_first = yan_kg_concept_at(&graph, 0u);
    const YanKgConcept *old_second = yan_kg_concept_at(&graph, 1u);
    const YanKgRelation *old_relation = yan_kg_relation_at(&graph, 0u);
    TEST_ASSERT_NOT_NULL(old_first);
    TEST_ASSERT_NOT_NULL(old_second);
    TEST_ASSERT_NOT_NULL(old_relation);
    const uint32_t old_out = old_first->out_count;
    const uint32_t old_declaration_count = old_first->declaration_count;
    const uint32_t old_alias_count = old_first->alias_count;
    /* A VALUE copy: keeping the pointer would make the later comparison read the
     * very bytes it is meant to check. */
    const YanKgProvenance old_where = old_first->declarations[0];
    char old_id[32], old_name[YAN_KG_LABEL_MAX + 1u];
    copy_text(old_first->id, old_id, sizeof old_id);
    copy_text(old_first->name, old_name, sizeof old_name);

    YanKgMatch old_match;
    memset(&old_match, 0, sizeof old_match);
    TEST_ASSERT_EQUAL(YAN_KG_OK,
                      yan_kg_lookup(&graph, (const uint8_t *)"square", 6u,
                                    &old_match));
    const uint32_t old_ordinal = old_match.concept;
    const YanKgMatchKind old_kind = old_match.kind;
    YanKgSummary old_summary;
    memset(&old_summary, 0, sizeof old_summary);
    yan_kg_summary(&graph, &old_summary);

    /* 3. Fail each ordinal in turn. */
    for (uint32_t ordinal = 1u; ordinal <= bound; ++ordinal) {
        YanKgResult result = rebuild_with_fault(ordinal, NULL, &injected);
        if (result != YAN_KG_NOSPACE || !injected) {
            char message[128];
            snprintf(message, sizeof message, "ordinal %u of %u returned %s%s",
                     ordinal, bound, yan_kg_result_name(result),
                     injected ? "" : " (no allocation was injected)");
            TEST_FAIL_MESSAGE(message);
        }

        /* Old graph preserved: counts, summary, node pointers, lookup and the
         * original provenance all still describe the successful publication. */
        TEST_ASSERT_EQUAL_UINT32(old_concepts, yan_kg_concept_count(&graph));
        TEST_ASSERT_EQUAL_UINT32(old_relations, yan_kg_relation_count(&graph));
        YanKgSummary now;
        memset(&now, 0, sizeof now);
        yan_kg_summary(&graph, &now);
        TEST_ASSERT_EQUAL_MEMORY(&old_summary, &now, sizeof now);

        const YanKgConcept *first = yan_kg_concept_at(&graph, 0u);
        TEST_ASSERT_NOT_NULL(first);
        TEST_ASSERT_EQUAL_PTR(old_first, first);
        TEST_ASSERT_EQUAL_PTR(old_second, yan_kg_concept_at(&graph, 1u));
        TEST_ASSERT_EQUAL_PTR(old_relation, yan_kg_relation_at(&graph, 0u));
        TEST_ASSERT_EQUAL_UINT32(old_out, first->out_count);
        TEST_ASSERT_EQUAL_UINT32(old_declaration_count,
                                 first->declaration_count);
        TEST_ASSERT_EQUAL_UINT32(old_alias_count, first->alias_count);
        TEST_ASSERT_EQUAL_STRING(old_id, first->id);
        TEST_ASSERT_EQUAL_STRING(old_name, first->name);
        TEST_ASSERT_EQUAL_MEMORY(&old_where, &first->declarations[0],
                                 sizeof old_where);

        YanKgMatch match;
        memset(&match, 0, sizeof match);
        TEST_ASSERT_EQUAL(YAN_KG_OK,
                          yan_kg_lookup(&graph, (const uint8_t *)"square", 6u,
                                        &match));
        TEST_ASSERT_EQUAL_UINT32(old_ordinal, match.concept);
        TEST_ASSERT_EQUAL_INT(old_kind, match.kind);
    }

    /* 4. One ordinal past the bound must succeed, which proves the measured bound
     * was exact: no allocation was left unexercised by the sweep. */
    uint32_t beyond = 0u;
    TEST_ASSERT_EQUAL(YAN_KG_OK, rebuild_with_fault(bound + 1u, &beyond,
                                                   &injected));
    TEST_ASSERT_FALSE(injected);
    TEST_ASSERT_EQUAL_UINT32(bound, beyond);

    /* 5. The graph stays usable, can be destroyed safely, and rebuilds again. */
    TEST_ASSERT_EQUAL_UINT32(old_concepts, yan_kg_concept_count(&graph));
    yan_kg_destroy(&graph);
    TEST_ASSERT_EQUAL_UINT32(0u, yan_kg_concept_count(&graph));
    TEST_ASSERT_NULL(yan_kg_concept_at(&graph, 0u));
    TEST_ASSERT_EQUAL(YAN_KG_OK, yan_kg_build(&graph, &source, &summary));
    TEST_ASSERT_EQUAL_UINT32(2u, yan_kg_concept_count(&graph));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_every_allocation_failure_reports_nospace_and_keeps_graph);
    return UNITY_END();
}
