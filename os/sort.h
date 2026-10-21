#ifndef YAN_OS_SORT_H
#define YAN_OS_SORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 0036-R8: the one order relation shared by the directory index, the mount
 * checks and the three services. Sorting indices rather than whole rows keeps
 * the moved bytes at two per swap no matter how wide the row becomes.
 *
 * `before(context, left, right)` returns true when `left` must be ordered
 * before `right`; equal keys may be ordered either way because heapsort is not
 * stable. The caller owns `order` and fills it with indices in [0, count).
 * The sort is an in-place heapsort with an iterative sift-down, so the frame
 * holds only a few integers and never grows with count. */
typedef bool (*YanSortBefore)(const void *context, uint16_t left, uint16_t right);

static inline void yan_sort_sift_down(uint16_t *order, uint32_t root,
                                      uint32_t count, YanSortBefore before,
                                      const void *context)
{
    for (;;) {
        uint32_t child = root * 2u + 1u;
        if (child >= count) {
            return;
        }
        if (child + 1u < count &&
            before(context, order[child], order[child + 1u])) {
            ++child;
        }
        if (!before(context, order[root], order[child])) {
            return;
        }
        uint16_t swap = order[root];
        order[root] = order[child];
        order[child] = swap;
        root = child;
    }
}

static inline void yan_sort_index(uint16_t *order, uint32_t count,
                                  YanSortBefore before, const void *context)
{
    if (order == NULL || before == NULL || count < 2u) {
        return;
    }
    for (uint32_t first = count / 2u; first > 0u; --first) {
        yan_sort_sift_down(order, first - 1u, count, before, context);
    }
    for (uint32_t end = count; end > 1u; --end) {
        uint16_t swap = order[0];
        order[0] = order[end - 1u];
        order[end - 1u] = swap;
        yan_sort_sift_down(order, 0u, end - 1u, before, context);
    }
}

#endif
