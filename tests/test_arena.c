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

static void test_null_arena_is_rejected(void)
{
    TEST_ASSERT_NULL(yan_arena_alloc(NULL, 1u));
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
