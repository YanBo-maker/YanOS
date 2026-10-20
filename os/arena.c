#include "arena.h"

#include <stddef.h>

#define YAN_ARENA_ALIGN ((uintptr_t)16)

void yan_arena_init(YanArena *arena, void *start, void *end)
{
    if (arena == NULL) {
        return;
    }
    arena->next = (uintptr_t)start;
    arena->end = (uintptr_t)end;
}

void *yan_arena_alloc(YanArena *arena, uint32_t bytes)
{
    if (arena == NULL) {
        return NULL;
    }
    uintptr_t current = arena->next;
    /* Round up before adding the request. The subtraction form detects the
     * wrap without forming an out-of-range pointer. */
    uintptr_t aligned = current;
    if ((current % YAN_ARENA_ALIGN) != 0u) {
        uintptr_t padding = YAN_ARENA_ALIGN - (current % YAN_ARENA_ALIGN);
        if (padding > UINTPTR_MAX - current) {
            return NULL;
        }
        aligned = current + padding;
    }
    if (aligned > arena->end ||
        (uintptr_t)bytes > arena->end - aligned) {
        return NULL;
    }
    arena->next = aligned + (uintptr_t)bytes;
    return (void *)aligned;
}

uintptr_t yan_arena_used(const YanArena *arena, const void *start)
{
    if (arena == NULL) {
        return 0u;
    }
    return arena->next - (uintptr_t)start;
}
