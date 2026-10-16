#include "semantic_json.h"
#include "unity.h"
#include <stdlib.h>
#include <string.h>

typedef struct { size_t calls, fail_at, live; bool hit; } Arena;
static YanSemanticJsonRows rows;
static YanSemanticEvidence evidence;
static char query[1024];
static void *allocate(void *opaque, size_t size)
{
    Arena *a = opaque;
    if (++a->calls == a->fail_at) { a->hit = true; return NULL; }
    void *p = malloc(size); if (p != NULL) ++a->live; return p;
}
static void *resize(void *opaque, void *old, size_t old_size, size_t size)
{
    (void)old_size; Arena *a = opaque;
    if (++a->calls == a->fail_at) { a->hit = true; return NULL; }
    bool fresh = old == NULL;
    void *p = realloc(old, size); if (p != NULL && fresh) ++a->live; return p;
}
static void release(void *opaque, void *p)
{
    Arena *a = opaque; if (p != NULL) { --a->live; free(p); }
}
void setUp(void)
{
    memset(&rows, 0, sizeof rows); memset(&evidence, 0, sizeof evidence);
    memset(query, '\t', sizeof query-1); query[sizeof query-1] = 0;
    rows.count = 20; evidence.summary.shown = evidence.summary.segments = 20;
    for (unsigned i = 0; i < 20; ++i) {
        YanSemanticJsonRow *r = &rows.rows[i];
        memcpy(r->row.provenance.name, "note", 5);
        r->row.provenance.end = r->row.raw_length = 512;
        r->row.provenance.first_line = r->row.provenance.last_line = 1;
        r->row.raw = r->raw; memset(r->raw, '\t', 512); r->row.score = 0.4472135954999579;
    }
}
void tearDown(void) {}
static void test_every_json_allocation_failure_releases_owned_memory(void)
{
    Arena baseline = {0}; yyjson_alc alc = {allocate, resize, release, &baseline};
    size_t length = 0;
    char *encoded = yan_semantic_json_encode(query, &rows, &evidence, &length, &alc);
    TEST_ASSERT_NOT_NULL(encoded);
    TEST_ASSERT_GREATER_THAN_UINT32(0, length);
    TEST_ASSERT_EQUAL_CHAR('\n', encoded[length-1]);
    release(&baseline, encoded);
    TEST_ASSERT_EQUAL_UINT32(0, baseline.live);
    size_t points = baseline.calls, rejected = 0;
    for (size_t i = 1; i <= points; ++i) {
        Arena a = {0}; a.fail_at = i; alc.ctx = &a;
        encoded = yan_semantic_json_encode(query, &rows, &evidence, &length, &alc);
        TEST_ASSERT_TRUE_MESSAGE(a.hit, "json_allocation_injection_reached");
        if (encoded == NULL) ++rejected;
        else release(&a, encoded); /* Optional shrink can recover from failure. */
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, a.live, "json_failure_owned_memory_released");
    }
    TEST_ASSERT_GREATER_THAN_UINT32(0, rejected);
    printf("JSON allocation failures: %zu points, %zu rejected safely\n", points, rejected);
}
int main(void)
{
    UNITY_BEGIN(); RUN_TEST(test_every_json_allocation_failure_releases_owned_memory);
    return UNITY_END();
}
