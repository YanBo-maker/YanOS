#ifndef YAN_CPU_H
#define YAN_CPU_H

#include <stdint.h>

#include "yan/bus.h"

#define YAN_REGISTER_COUNT 32
#define YAN_MSTATUS_MIE UINT32_C(0x8)
#define YAN_MSTATUS_MPIE UINT32_C(0x80)
#define YAN_MSTATUS_MPP UINT32_C(0x1800)

typedef struct {
    uint32_t mstatus, mtvec, mscratch, mepc, mcause, mtval;
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
/* Reads the word at PC; preserves CPU state, RAM, and outputs on failure. */
YanBusResult yan_cpu_fetch(const YanCpu *cpu, const YanBus *bus,
                           uint32_t *instruction);
/* YAN_TRAP enters the Guest handler; Host errors preserve the whole state. */
YanStatus yan_cpu_step(YanCpu *cpu, YanBus *bus);

#endif
