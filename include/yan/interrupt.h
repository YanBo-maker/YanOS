#ifndef YAN_INTERRUPT_H
#define YAN_INTERRUPT_H

#include <stdbool.h>
#include <stdint.h>

#include "yan/status.h"

/* CLINT (Core Local Interruptor) for a single hart, standard SiFive layout.
 * mtimecmp and mtime are 64-bit and are accessed as two 32-bit half words. */
#define YAN_CLINT_BASE UINT32_C(0x02000000)
#define YAN_CLINT_SIZE UINT32_C(0x00010000)
#define YAN_CLINT_MSIP UINT32_C(0x0000)
#define YAN_CLINT_MTIMECMP UINT32_C(0x4000)
#define YAN_CLINT_MTIME UINT32_C(0xbff8)

/* PLIC (Platform-Level Interrupt Controller) with one M-mode context.
 * Sources 1..YAN_PLIC_SOURCE_MAX are usable; source 0 is reserved. */
#define YAN_PLIC_BASE UINT32_C(0x0c000000)
#define YAN_PLIC_SIZE UINT32_C(0x00400000)
#define YAN_PLIC_PRIORITY UINT32_C(0x0000)
#define YAN_PLIC_PENDING UINT32_C(0x1000)
#define YAN_PLIC_ENABLE_M UINT32_C(0x2000)
#define YAN_PLIC_THRESHOLD_M UINT32_C(0x200000)
#define YAN_PLIC_CLAIM_M UINT32_C(0x200004)
#define YAN_PLIC_SOURCE_COUNT 32
#define YAN_PLIC_SOURCE_MAX 31

/* mip / mie bit positions. Each position is the interrupt cause code it
 * reports, so these constants also name the machine-level interrupt classes. */
#define YAN_INTERRUPT_MSIP UINT32_C(0x00000008) /* cause 3  */
#define YAN_INTERRUPT_MTIP UINT32_C(0x00000080) /* cause 7  */
#define YAN_INTERRUPT_MEIP UINT32_C(0x00000800) /* cause 11 */

typedef struct {
    uint32_t msip;
    uint64_t mtime;
    uint64_t mtimecmp;
} YanClint;

typedef struct {
    uint32_t priority[YAN_PLIC_SOURCE_COUNT];
    /* Latched service requests produced by the gateways. */
    uint32_t pending;
    /* Last level driven onto each source line. A device reports a level and
     * nothing else; the platform drives it here and the gateway decides. */
    uint32_t level;
    uint32_t enable_m;
    uint32_t threshold_m;
    /* Claimed but not yet completed sources. A source in service cannot become
     * pending again until it is completed. */
    uint32_t in_service_m;
} YanPlic;

void yan_clint_reset(YanClint *clint);
/* Discrete tick interface: the counter never reads the host wall clock. */
void yan_clint_tick(YanClint *clint, uint64_t ticks);
YanStatus yan_clint_read(const YanClint *clint, uint32_t offset, uint32_t *value);
YanStatus yan_clint_write(YanClint *clint, uint32_t offset, uint32_t value);
uint32_t yan_clint_pending(const YanClint *clint);

void yan_plic_reset(YanPlic *plic);
/* A claim read has side effects, so it takes a mutable controller. */
YanStatus yan_plic_read(YanPlic *plic, uint32_t offset, uint32_t *value);
YanStatus yan_plic_write(YanPlic *plic, uint32_t offset, uint32_t value);
/* Drive one source line. Level semantics: asserted means the device requests
 * service now; deasserted withdraws a request that has not been claimed. A
 * source that is already in service is held closed until it is completed, so
 * driving an asserted level again does not re-pend it. Source 0 is ignored.
 * This is the only external source interface; platform device lines reach the
 * controller through it. See docs/specs/0016-plic-gateway-and-irq-lines.md. */
void yan_plic_set_level(YanPlic *plic, uint32_t source, bool asserted);
uint32_t yan_plic_pending(const YanPlic *plic);

#endif
