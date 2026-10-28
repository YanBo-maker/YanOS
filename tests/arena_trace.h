#ifndef YAN_TESTS_ARENA_TRACE_H
#define YAN_TESTS_ARENA_TRACE_H

#include <stdbool.h>
#include <stdint.h>

#include "yan/bus.h"
#include "yan/cpu.h"

/* 0036-R3 execution observation: record what `yan_arena_alloc` actually did
 * during the default production ELF run.
 *
 * The Guest publishes `yan_arena_used_bytes` only after the last startup
 * allocation, so an allocation moved ahead of the header probe would still
 * read 0 on a rejected image. This observer instead watches the real function
 * boundary: at the entry address it records the caller's return address and
 * the a0/a1 arguments; at that return address it reads the returned pointer and
 * the arena cursor word the function wrote. It therefore reports calls,
 * successes, failures and the actual high-water cursor independent of the
 * Guest snapshot.
 *
 * Scope: the selected default RV32 ELF, built at -O2 without LTO. The traced
 * call shape is the ABI call/return pair (`jal`/`ret`), not an inlined or
 * tail-called variant. The selected `yan_arena_alloc` is non-recursive; a
 * revisit of its entry while a call is pending is treated as interrupt
 * resampling (the entry instruction has not executed yet), not as a second
 * call. Recursion is outside this selected shape. A missing symbol, a bus read
 * failure or a call still active when the run ends is an observation error,
 * never zero. */
typedef struct {
    bool available;
    bool requested;
    bool active;
    bool error;
    bool incomplete;
    uint32_t entry;
    uint32_t start;
    uint32_t return_pc;
    uint32_t request_bytes;
    uint32_t arena_address;
    uint32_t cursor;
    uint64_t calls;
    uint64_t successes;
    uint64_t failures;
    uint32_t highwater;
} YanArenaTrace;

/* `available` is true only when both the function and arena-start symbols were
 * found. A requested but unavailable trace reports state "unavailable". */
void yan_arena_trace_configure(YanArenaTrace *trace, bool requested,
                               bool available, uint32_t entry, uint32_t start);

/* Call once at every instruction boundary, before the CPU executes the
 * instruction at cpu->pc. */
void yan_arena_trace_step(YanArenaTrace *trace, const YanCpu *cpu,
                          const YanBus *bus);

/* Marks an entry that never returned as an incomplete observation. */
void yan_arena_trace_finish(YanArenaTrace *trace);

/* "ok", "unavailable", "error" or "incomplete". */
const char *yan_arena_trace_state(const YanArenaTrace *trace);

#endif
