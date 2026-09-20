#ifndef YAN_CPU_H
#define YAN_CPU_H

#include <stdint.h>

#include "yan/bus.h"

#define YAN_REGISTER_COUNT 32
#define YAN_MSTATUS_MIE UINT32_C(0x8)
#define YAN_MSTATUS_MPIE UINT32_C(0x80)
#define YAN_MSTATUS_MPP UINT32_C(0x1800)
/* mie and mip implement the same three machine-level interrupt bits. */
#define YAN_MIE_MASK (YAN_INTERRUPT_MSIP | YAN_INTERRUPT_MTIP | YAN_INTERRUPT_MEIP)
/* An interrupt cause carries the highest bit set; see the privileged spec. */
#define YAN_MCAUSE_INTERRUPT UINT32_C(0x80000000)
#define YAN_MCAUSE_MSIP UINT32_C(3)
#define YAN_MCAUSE_MTIP UINT32_C(7)
#define YAN_MCAUSE_MEIP UINT32_C(11)

typedef struct {
    uint32_t mstatus, mtvec, mscratch, mepc, mcause, mtval, mie, mip;
} YanCsr;

/* Observe fields directly; use register accessors to preserve x0 semantics. */
typedef struct {
    uint32_t regs[YAN_REGISTER_COUNT];
    uint32_t pc;
    YanCsr csr;
} YanCpu;

/* Stable architectural state exchanged with external validation models. */
typedef YanCpu YanCpuState;

YanStatus yan_cpu_reset(YanCpu *cpu, uint32_t entry);
YanStatus yan_cpu_read_reg(const YanCpu *cpu, uint32_t index, uint32_t *value);
YanStatus yan_cpu_write_reg(YanCpu *cpu, uint32_t index, uint32_t value);
YanStatus yan_cpu_read_csr(const YanCpu *cpu, uint32_t address, uint32_t *value);
YanStatus yan_cpu_write_csr(YanCpu *cpu, uint32_t address, uint32_t value);
YanStatus yan_cpu_snapshot(const YanCpu *cpu, YanCpuState *state);
/* Latches the device interrupt lines into mip without touching PC or mstatus.
 * yan_cpu_step samples at every instruction boundary; the host uses this to
 * observe the same value outside a step. */
YanStatus yan_cpu_poll_interrupts(YanCpu *cpu, const YanBus *bus);
/* Reads the word at PC; preserves CPU state, RAM, and outputs on failure. */
YanBusResult yan_cpu_fetch(const YanCpu *cpu, const YanBus *bus,
                           uint32_t *instruction);
/* YAN_TRAP enters the Guest handler; Host errors preserve the whole state. */
YanStatus yan_cpu_step(YanCpu *cpu, YanBus *bus);

#endif
