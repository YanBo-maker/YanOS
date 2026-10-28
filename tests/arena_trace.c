#include "arena_trace.h"

#include <stddef.h>
#include <string.h>

/* The traced function follows the standard RV32 ABI: its first argument (the
 * arena pointer) is in x10, the byte count in x11, the return value back in x10
 * and the caller's continuation in x1. YanArena stores `next` at offset 0 and
 * `end` at offset 4. Both facts are part of the selected non-LTO production
 * build, not a Guest API. */
enum {
    ARG_ARENA = 10,
    ARG_BYTES = 11,
    ARG_RETURN = 10,
    RETURN_ADDRESS = 1,
    ARENA_ALIGN = 16
};

void yan_arena_trace_configure(YanArenaTrace *trace, bool requested,
                               bool available, uint32_t entry, uint32_t start)
{
    if (trace == NULL) {
        return;
    }
    memset(trace, 0, sizeof *trace);
    trace->requested = requested;
    trace->available = available;
    trace->entry = entry;
    trace->start = start;
    trace->cursor = start;
}

void yan_arena_trace_step(YanArenaTrace *trace, const YanCpu *cpu,
                          const YanBus *bus)
{
    if (trace == NULL || cpu == NULL || bus == NULL ||
        !trace->requested || !trace->available || trace->error) {
        return;
    }
    const uint32_t pc = cpu->pc;
    if (!trace->active) {
        if (pc != trace->entry) {
            return;
        }
        trace->active = true;
        trace->return_pc = cpu->regs[RETURN_ADDRESS];
        trace->arena_address = cpu->regs[ARG_ARENA];
        trace->request_bytes = cpu->regs[ARG_BYTES];
        ++trace->calls;
        return;
    }
    /* A trap can be sampled at the entry boundary before the first
     * instruction executes. `mret` then returns to the same address; the call
     * is still pending, so neither a second call nor a return has happened. */
    if (pc == trace->entry) {
        return;
    }
    if (pc != trace->return_pc) {
        return;
    }
    uint32_t next = 0;
    if (yan_bus_read(bus, trace->arena_address, 4, &next).status != YAN_OK) {
        trace->error = true;
        return;
    }
    const uint32_t returned = cpu->regs[ARG_RETURN];
    if (returned == 0u) {
        ++trace->failures;
        /* A refused allocation must leave the cursor untouched. */
        if (next != trace->cursor) {
            trace->error = true;
            return;
        }
        trace->active = false;
        return;
    }
    /* A successful allocation returns the 16-aligned cursor and advances it by
     * the requested bytes. Padding is therefore part of the high-water mark. */
    if ((returned % ARENA_ALIGN) != 0u || returned < trace->start ||
        next < returned || next - returned != trace->request_bytes) {
        trace->error = true;
        return;
    }
    ++trace->successes;
    trace->cursor = next;
    if (next - trace->start > trace->highwater) {
        trace->highwater = next - trace->start;
    }
    trace->active = false;
}

void yan_arena_trace_finish(YanArenaTrace *trace)
{
    if (trace == NULL) {
        return;
    }
    if (trace->requested && trace->available && trace->active &&
        !trace->error) {
        trace->incomplete = true;
    }
}

const char *yan_arena_trace_state(const YanArenaTrace *trace)
{
    if (trace == NULL || !trace->requested) {
        return "disabled";
    }
    if (!trace->available) {
        return "unavailable";
    }
    if (trace->incomplete) {
        return "incomplete";
    }
    if (trace->error) {
        return "error";
    }
    return "ok";
}
