/* 0036-R3-AC1: the one-shot Guest bump arena.
 *
 * The arena itself is Host-testable because it is plain C over uintptr_t. The
 * cases below pin the two properties the spec fixes: every allocation starts
 * 16-byte aligned, and a failed allocation leaves next unchanged. */
#include "arena.h"
#include "unity.h"

#include <stdint.h>

static uint8_t bytes[256];

void setUp(void) {}
void tearDown(void) {}

static void test_init_and_used(void)
{
    uintptr_t aligned = ((uintptr_t)bytes + 15u) & ~(uintptr_t)15u;
    uint8_t *start = (uint8_t *)aligned;
    YanArena arena;
    yan_arena_init(&arena, start, start + 64u);
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)yan_arena_used(&arena, start));
    void *first = yan_arena_alloc(&arena, 1u);
    TEST_ASSERT_EQUAL_PTR(start, first);
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)yan_arena_used(&arena, start));
    void *second = yan_arena_alloc(&arena, 1u);
    TEST_ASSERT_EQUAL_PTR(start + 16, second);
    TEST_ASSERT_EQUAL_UINT32(17u, (uint32_t)yan_arena_used(&arena, start));
}

static void test_alignment_and_exact_fit(void)
{
    uintptr_t aligned = ((uintptr_t)bytes + 15u) & ~(uintptr_t)15u;
    uint8_t *start = (uint8_t *)aligned + 3;
    YanArena arena;
    yan_arena_init(&arena, start, start + 48u);
    void *first = yan_arena_alloc(&arena, 1u);
    TEST_ASSERT_EQUAL_PTR((uint8_t *)aligned + 16, first);
    TEST_ASSERT_EQUAL_UINT32(14u, (uint32_t)yan_arena_used(&arena, start));
    void *exact = yan_arena_alloc(&arena, 19u);
    TEST_ASSERT_EQUAL_PTR((uint8_t *)aligned + 32, exact);
    TEST_ASSERT_EQUAL_UINT32(48u, (uint32_t)yan_arena_used(&arena, start));
    /* The arena is exhausted; a failed allocation must not move next. */
    TEST_ASSERT_NULL(yan_arena_alloc(&arena, 1u));
    TEST_ASSERT_EQUAL_UINT32(48u, (uint32_t)yan_arena_used(&arena, start));
}

static void test_zero_length_allocation_does_not_move(void)
{
    YanArena arena;
    yan_arena_init(&arena, bytes, bytes + sizeof bytes);
    void *first = yan_arena_alloc(&arena, 0u);
    TEST_ASSERT_EQUAL_PTR(bytes, first);
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)yan_arena_used(&arena, bytes));
}

static void test_overflow_and_out_of_range_do_not_move(void)
{
    YanArena arena;
    yan_arena_init(&arena, (void *)(UINTPTR_MAX - 3u), (void *)UINTPTR_MAX);
    TEST_ASSERT_NULL(yan_arena_alloc(&arena, 1u));
    TEST_ASSERT_EQUAL_UINT32(0u,
        (uint32_t)yan_arena_used(&arena, (void *)(UINTPTR_MAX - 3u)));

    yan_arena_init(&arena, bytes, bytes + sizeof bytes);
    TEST_ASSERT_NULL(yan_arena_alloc(&arena, UINT32_MAX));
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)yan_arena_used(&arena, bytes));

    /* end below start: every allocation is out of range. */
    yan_arena_init(&arena, bytes + 64, bytes);
    TEST_ASSERT_NULL(yan_arena_alloc(&arena, 1u));
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)yan_arena_used(&arena, bytes + 64));
}

/* Issue #107: all three entry points accept a NULL arena, but only alloc() was
 * asserted before, so the claim "the NULL checks are locked by tests" held for
 * one of three functions.
 *
 * COVERAGE LEVEL -- read this before trusting the test:
 *   Every one of the three checks is CRASH level, not assertion level.
 *   Removing any single guard makes the next statement dereference NULL and
 *   the process dies with SIGSEGV (measured per function: rc=-11). Unity
 *   therefore never prints a `:FAIL:` line for these; a regression shows up as
 *   a crashing test binary.
 *
 * Because all three crash rather than assert, one function can only ever
 * observe the FIRST broken call: with two guards removed the run stops at
 * whichever comes first and the later call is never reached. The order below
 * puts used() first so its guard is the one probed when several are broken at
 * once. The init() call is still reached whenever the used() guard is intact,
 * which is the single-mutation case a mutation harness actually produces (each
 * mutant removes exactly one guard).
 *
 * The buffer is named `scratch` rather than `bytes`: the file already has a
 * file-scope `bytes` fixture, and MSVC rejects the shadowing under C4459
 * (treated as an error here). GCC does not warn, so only the Windows job
 * catches that.
 *
 * The NULL-vs-empty distinction is pinned too, so an implementation that
 * clamped a NULL arena into a plausible-looking empty one would fail here
 * instead of passing on a technicality. */
static void test_null_arena_is_rejected(void)
{
    /* Address must be well-defined; the guards never read the contents. */
    unsigned char scratch[64] = {0};

    /* Crash level: probed first, so a broken used() guard is reached before
     * init() could take the process down. */
    TEST_ASSERT_EQUAL_UINT32(0u,
        (uint32_t)yan_arena_used(NULL, scratch));

    /* Crash level: reached whenever the used() guard above is intact. */
    yan_arena_init(NULL, scratch, scratch + sizeof scratch);

    /* Crash level: this one dereferences arena->next right after the guard. */
    TEST_ASSERT_NULL(yan_arena_alloc(NULL, 1u));

    /* Distinctness check. What it DOES catch: an implementation that clamps a
     * NULL arena into a usable/NON-EMPTY one. Measured: with the two NULL
     * assertions above removed, this block alone still fails
     * (`Expected 4096 Was 0`) against a mutant whose used(NULL) returns a
     * non-zero cursor. So it is not redundant with them.
     *
     * What it does NOT catch: an implementation that clamps NULL into a
     * genuinely EMPTY arena (cursor == start). Both sides then report 0 and the
     * comparison passes.
     *
     * And the honest part: nothing in this test catches that mutant either.
     * Measured on a clamp whose used(NULL) returns
     * `(uintptr_t)start - (uintptr_t)start`, the whole suite is green --
     * `used(NULL) == 0` is exactly what a clamped empty arena reports, and
     * `alloc(NULL, 1u)` still returns NULL because an empty arena cannot
     * satisfy a 1-byte request. Detecting that mutant would need an assertion
     * on state the API does not expose (e.g. that the NULL path never touches
     * the caller's arena object), which a header-only contract cannot offer.
     *
     * So this block and the NULL assertions above are complementary, not
     * substitutes -- but the coverage has a hole at "NULL behaves like an empty
     * arena", and the hole is documented here rather than papered over. */
    {
        YanArena empty;
        yan_arena_init(&empty, scratch, scratch);
        TEST_ASSERT_EQUAL_UINT32((uint32_t)yan_arena_used(NULL, scratch),
                                 (uint32_t)yan_arena_used(&empty, scratch));
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_and_used);
    RUN_TEST(test_alignment_and_exact_fit);
    RUN_TEST(test_zero_length_allocation_does_not_move);
    RUN_TEST(test_overflow_and_out_of_range_do_not_move);
    RUN_TEST(test_null_arena_is_rejected);
    return UNITY_END();
}
