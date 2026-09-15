#ifndef YAN_CPU_H
#define YAN_CPU_H

#include <stdint.h>

#include "yan/status.h"

#define YAN_REGISTER_COUNT 32

/* Observe fields directly; use register accessors to preserve x0 semantics. */
typedef struct {
    uint32_t regs[YAN_REGISTER_COUNT];
    uint32_t pc;
} YanCpu;

YanStatus yan_cpu_reset(YanCpu *cpu, uint32_t entry);
YanStatus yan_cpu_read_reg(const YanCpu *cpu, uint32_t index, uint32_t *value);
YanStatus yan_cpu_write_reg(YanCpu *cpu, uint32_t index, uint32_t value);

#endif
