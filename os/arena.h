#ifndef YAN_OS_ARENA_H
#define YAN_OS_ARENA_H

#include <stdint.h>

/* 0036-R3: the Guest's one-shot bump arena.
 *
 * The terminal reads the image's directory header, learns how many files the
 * medium can hold, and then allocates every file-count-dependent buffer once.
 * There is no free and no second allocation phase: after startup the arena is
 * a read-only record of what was handed out. A failed allocation leaves `next`
 * unchanged, so the caller can report the failure without a half-consumed
 * arena.
 *
 * The same source compiles for the Host, where 0036-R3's acceptance suite
 * exercises it directly. */
typedef struct {
    uintptr_t next;
    uintptr_t end;
} YanArena;

/* Places the cursor at `start` with `end` as the first byte past the arena.
 * No validation is performed: a range that is empty or inverted simply makes
 * every allocation fail. A NULL `arena` returns immediately without touching
 * anything (the return type is void). */
void yan_arena_init(YanArena *arena, void *start, void *end);

/* Allocates `bytes` at the first 16-byte-aligned address at or after the
 * cursor. Returns NULL without moving the cursor when the aligned address or
 * the addition overflows uintptr_t, or when the result would pass `end`. A
 * zero-byte request returns the aligned cursor without moving it. A NULL
 * `arena` returns NULL. */
void *yan_arena_alloc(YanArena *arena, uint32_t bytes);

/* Bytes consumed from `start` to the cursor, including alignment padding.
 * A NULL `arena` returns 0. */
uintptr_t yan_arena_used(const YanArena *arena, const void *start);

/* The three entry points share one NULL rule: a NULL `arena` is never
 * dereferenced, and each returns its neutral value (init: no-op, alloc: NULL,
 * used: 0). This is a consistency contract inside the module, not an observed
 * production path -- the only production caller is the Guest startup sequence
 * in `apps/yanfs_terminal/main.c`, which passes the address of a stack object
 * on every call, so no NULL can reach these functions there. The rule exists so
 * that a future caller that does pass NULL fails closed instead of crashing.
 * The Host test suite drives the NULL cases directly (tests/test_arena.c). */

#endif