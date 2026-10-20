#include "sort.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX_SORT 4095u
static uint32_t keys[MAX_SORT];
static char names[MAX_SORT][9];
static uint16_t order[MAX_SORT + 16u], reference[MAX_SORT];
static uint64_t sort_cases;
static uint32_t state = UINT32_C(0x51a7f00d);

static uint32_t random_word(void)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

static bool name_before(const void *context, uint16_t a, uint16_t b)
{
    (void)context;
    for (unsigned at = 0; at < 8u; ++at) {
        if (names[a][at] != names[b][at]) return names[a][at] < names[b][at];
    }
    return false;
}

static void sort_case(uint32_t count, unsigned kind)
{
    static const char hex[] = "0123456789abcdef";
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t value = kind == 0u ? random_word() :
                         kind == 1u ? i : kind == 2u ? count - i :
                         kind == 3u ? 7u : i % 17u;
        keys[i] = value;
        for (unsigned j = 0; j < 8u; ++j)
            names[i][j] = hex[(value >> ((7u - j) * 4u)) & 15u];
        names[i][8] = '\0';
        order[i] = (uint16_t)i;
    }
    /* Unsorted index input too: the permutation, not only the names, varies. */
    if (kind == 0u && count > 1u) {
        for (uint32_t i = count - 1u; i > 0u; --i) {
            uint32_t j = random_word() % (i + 1u);
            uint16_t value = order[i]; order[i] = order[j]; order[j] = value;
        }
    }
    for (uint32_t i = 0; i < count; ++i) reference[i] = order[i];
    for (uint32_t i = count; i < MAX_SORT + 16u; ++i) order[i] = UINT16_C(0xa55a);
    /* Independent stable insertion reference compares numeric keys, whereas
     * the actual generic sorter receives fixed-width ASCII names. */
    for (uint32_t i = 1; i < count; ++i) {
        uint16_t value = reference[i]; uint32_t j = i;
        while (j > 0u && keys[value] < keys[reference[j - 1u]]) {
            reference[j] = reference[j - 1u]; --j;
        }
        reference[j] = value;
    }
    yan_sort_index(order, count, name_before, NULL);
    unsigned char seen[MAX_SORT] = {0};
    for (uint32_t i = 0; i < count; ++i) {
        if (order[i] >= count || seen[order[i]] ||
            keys[order[i]] != keys[reference[i]] ||
            (kind < 3u && order[i] != reference[i])) {
            fprintf(stderr, "FAIL sort count=%" PRIu32 " kind=%u index=%" PRIu32 "\n", count, kind, i);
            exit(1);
        }
        seen[order[i]] = 1;
    }
    for (uint32_t i = count; i < MAX_SORT + 16u; ++i) {
        if (order[i] != UINT16_C(0xa55a)) { fputs("FAIL sort guard\n", stderr); exit(1); }
    }
    ++sort_cases;
}

int main(void)
{
    for (uint32_t n = 0; n <= 2047u; ++n)
        for (unsigned kind = 0; kind < 5u; ++kind) sort_case(n, kind);
    for (unsigned kind = 0; kind < 5u; ++kind) sort_case(MAX_SORT, kind);
    printf("PASS sort_index: %" PRIu64 " fixed-seed/reference/guard cases\n", sort_cases);
    return 0;
}
