/* Exercise the real digest's allocation failure without replacing the
 * production library allocator or depending on host memory exhaustion. */
#include <stdlib.h>
#include "unity.h"
static void *failed_calloc(size_t count, size_t width)
{ (void)count; (void)width; return NULL; }
#define calloc failed_calloc
#include "semantic_corpus.c"
#undef calloc

static unsigned callbacks;
static YanSemanticResult next_empty(void *p, uint32_t *c, YanSemanticFile *f)
{ (void)p; (void)c; (void)f; ++callbacks; return YAN_SEMANTIC_MISSING; }
static YanSemanticResult read_empty(void *p, const char *name, uint32_t offset,
                                    uint8_t *out, uint32_t length)
{ (void)p; (void)name; (void)offset; (void)out; (void)length;
  ++callbacks; return YAN_SEMANTIC_OK; }
void setUp(void) { callbacks = 0; }
void tearDown(void) { }
static void digest_allocation_failure_is_io_without_callbacks_or_output(void)
{
    const uint32_t capacities[] = {63, 1023, 4095};
    for (size_t i = 0; i < sizeof capacities / sizeof capacities[0]; ++i) {
        YanSemanticSource source = {NULL, 0, next_empty, read_empty, capacities[i]};
        uint8_t out[32], original[32];
        memset(out, 0xA5, sizeof out); memcpy(original, out, sizeof out);
        TEST_ASSERT_EQUAL_INT(YAN_SEMANTIC_IO, yan_semantic_source_digest(&source, out));
        TEST_ASSERT_EQUAL_UINT(0, callbacks);
        TEST_ASSERT_EQUAL_MEMORY(original, out, sizeof out);
    }
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(digest_allocation_failure_is_io_without_callbacks_or_output);
    return UNITY_END();
}
