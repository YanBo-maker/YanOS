#include "yan/interrupt.h"

#include <stddef.h>

/* Both devices answer 32-bit word accesses only. */
#define YAN_DEVICE_ALIGNMENT UINT32_C(4)

void yan_clint_reset(YanClint *clint)
{
    if (clint != NULL) {
        clint->msip = 0;
        clint->mtime = 0;
        /* UINT64_MAX keeps MTIP low until software programs a deadline; a zero
         * comparator would assert the timer interrupt straight out of reset. */
        clint->mtimecmp = UINT64_MAX;
    }
}

void yan_clint_tick(YanClint *clint, uint64_t ticks)
{
    if (clint != NULL) {
        /* Unsigned wraparound is defined and deliberately not checked: mtime is
         * a free-running counter, exactly as on hardware. */
        clint->mtime += ticks;
    }
}

YanStatus yan_clint_read(const YanClint *clint, uint32_t offset, uint32_t *value)
{
    if (clint == NULL || value == NULL) {
        return YAN_INVALID_ARGUMENT;
    }
    if (offset % YAN_DEVICE_ALIGNMENT != 0) {
        return YAN_UNALIGNED;
    }
    switch (offset) {
    case YAN_CLINT_MSIP: *value = clint->msip & UINT32_C(1); return YAN_OK;
    case YAN_CLINT_MTIMECMP: *value = (uint32_t)clint->mtimecmp; return YAN_OK;
    case YAN_CLINT_MTIMECMP + 4:
        *value = (uint32_t)(clint->mtimecmp >> 32);
        return YAN_OK;
    case YAN_CLINT_MTIME: *value = (uint32_t)clint->mtime; return YAN_OK;
    case YAN_CLINT_MTIME + 4: *value = (uint32_t)(clint->mtime >> 32); return YAN_OK;
    default: return YAN_UNMAPPED;
    }
}

YanStatus yan_clint_write(YanClint *clint, uint32_t offset, uint32_t value)
{
    if (clint == NULL) {
        return YAN_INVALID_ARGUMENT;
    }
    if (offset % YAN_DEVICE_ALIGNMENT != 0) {
        return YAN_UNALIGNED;
    }
    /* A half-word write replaces only its half of the 64-bit register. */
    switch (offset) {
    case YAN_CLINT_MSIP:
        clint->msip = value & UINT32_C(1);
        return YAN_OK;
    case YAN_CLINT_MTIMECMP:
        clint->mtimecmp = (clint->mtimecmp & UINT64_C(0xffffffff00000000)) |
                          (uint64_t)value;
        return YAN_OK;
    case YAN_CLINT_MTIMECMP + 4:
        clint->mtimecmp = (clint->mtimecmp & UINT64_C(0x00000000ffffffff)) |
                          ((uint64_t)value << 32);
        return YAN_OK;
    case YAN_CLINT_MTIME:
        clint->mtime = (clint->mtime & UINT64_C(0xffffffff00000000)) |
                       (uint64_t)value;
        return YAN_OK;
    case YAN_CLINT_MTIME + 4:
        clint->mtime = (clint->mtime & UINT64_C(0x00000000ffffffff)) |
                       ((uint64_t)value << 32);
        return YAN_OK;
    default: return YAN_UNMAPPED;
    }
}

uint32_t yan_clint_pending(const YanClint *clint)
{
    if (clint == NULL) {
        return 0;
    }
    uint32_t pending = (clint->msip & UINT32_C(1)) != 0 ? YAN_INTERRUPT_MSIP : 0;
    /* MTIP is a level signal: it stays asserted until the deadline moves. */
    if (clint->mtime >= clint->mtimecmp) {
        pending |= YAN_INTERRUPT_MTIP;
    }
    return pending;
}

void yan_plic_reset(YanPlic *plic)
{
    if (plic != NULL) {
        *plic = (YanPlic){0};
    }
}

/* Highest-priority pending and enabled source above the threshold. Ties go to
 * the lowest source id, which is the arbitration rule the PLIC spec fixes. */
static uint32_t plic_select(const YanPlic *plic)
{
    const uint32_t active = plic->pending & plic->enable_m & ~UINT32_C(1);
    uint32_t selected = 0;
    uint32_t selected_priority = plic->threshold_m;
    for (uint32_t source = 1; source <= YAN_PLIC_SOURCE_MAX; ++source) {
        if ((active & (UINT32_C(1) << source)) == 0) {
            continue;
        }
        if (plic->priority[source] > selected_priority) {
            selected = source;
            selected_priority = plic->priority[source];
        }
    }
    return selected;
}

YanStatus yan_plic_read(YanPlic *plic, uint32_t offset, uint32_t *value)
{
    if (plic == NULL || value == NULL) {
        return YAN_INVALID_ARGUMENT;
    }
    if (offset % YAN_DEVICE_ALIGNMENT != 0) {
        return YAN_UNALIGNED;
    }
    if (offset < YAN_PLIC_PENDING) {
        const uint32_t source = offset / YAN_DEVICE_ALIGNMENT;
        if (source >= YAN_PLIC_SOURCE_COUNT) {
            return YAN_UNMAPPED;
        }
        *value = plic->priority[source];
        return YAN_OK;
    }
    switch (offset) {
    case YAN_PLIC_PENDING: *value = plic->pending & ~UINT32_C(1); return YAN_OK;
    case YAN_PLIC_ENABLE_M: *value = plic->enable_m & ~UINT32_C(1); return YAN_OK;
    case YAN_PLIC_THRESHOLD_M: *value = plic->threshold_m; return YAN_OK;
    case YAN_PLIC_CLAIM_M: {
        /* A claim read is a handshake: it returns the winning source, clears
         * its pending bit and marks it in service until completion. */
        const uint32_t source = plic_select(plic);
        if (source != 0) {
            plic->pending &= ~(UINT32_C(1) << source);
            plic->in_service_m |= UINT32_C(1) << source;
        }
        *value = source;
        return YAN_OK;
    }
    default: return YAN_UNMAPPED;
    }
}

YanStatus yan_plic_write(YanPlic *plic, uint32_t offset, uint32_t value)
{
    if (plic == NULL) {
        return YAN_INVALID_ARGUMENT;
    }
    if (offset % YAN_DEVICE_ALIGNMENT != 0) {
        return YAN_UNALIGNED;
    }
    if (offset < YAN_PLIC_PENDING) {
        const uint32_t source = offset / YAN_DEVICE_ALIGNMENT;
        if (source >= YAN_PLIC_SOURCE_COUNT) {
            return YAN_UNMAPPED;
        }
        /* Source 0 is reserved and keeps its priority slot at zero. */
        if (source != 0) {
            plic->priority[source] = value;
        }
        return YAN_OK;
    }
    switch (offset) {
    /* Pending is read-only: raise sets it and a claim clears it. */
    case YAN_PLIC_PENDING: return YAN_OK;
    case YAN_PLIC_ENABLE_M: plic->enable_m = value & ~UINT32_C(1); return YAN_OK;
    case YAN_PLIC_THRESHOLD_M: plic->threshold_m = value; return YAN_OK;
    case YAN_PLIC_CLAIM_M:
        /* Writes complete a previously claimed source. */
        if (value >= 1 && value <= YAN_PLIC_SOURCE_MAX) {
            plic->in_service_m &= ~(UINT32_C(1) << value);
        }
        return YAN_OK;
    default: return YAN_UNMAPPED;
    }
}

void yan_plic_raise(YanPlic *plic, uint32_t source)
{
    if (plic != NULL && source >= 1 && source <= YAN_PLIC_SOURCE_MAX) {
        plic->pending |= UINT32_C(1) << source;
    }
}

uint32_t yan_plic_pending(const YanPlic *plic)
{
    if (plic == NULL) {
        return 0;
    }
    /* MEIP is a summary line: it carries no source id, so only the existence of
     * a qualifying source matters here. */
    return plic_select(plic) != 0 ? YAN_INTERRUPT_MEIP : 0;
}
