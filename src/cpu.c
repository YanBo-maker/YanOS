#include "yan/cpu.h"

#include <stddef.h>

YanStatus yan_cpu_reset(YanCpu *cpu, uint32_t entry)
{
    if (cpu == NULL) {
        return YAN_INVALID_ARGUMENT;
    }
    if (entry % 4 != 0) {
        return YAN_UNALIGNED;
    }
    /* Register clearing is the Yan platform's deterministic reset policy. */
    *cpu = (YanCpu){0};
    cpu->pc = entry;
    return YAN_OK;
}

YanStatus yan_cpu_read_reg(const YanCpu *cpu, uint32_t index, uint32_t *value)
{
    if (cpu == NULL || value == NULL) {
        return YAN_INVALID_ARGUMENT;
    }
    if (index >= YAN_REGISTER_COUNT) {
        return YAN_OUT_OF_BOUNDS;
    }
    *value = index == 0 ? 0 : cpu->regs[index];
    return YAN_OK;
}

YanStatus yan_cpu_write_reg(YanCpu *cpu, uint32_t index, uint32_t value)
{
    if (cpu == NULL) {
        return YAN_INVALID_ARGUMENT;
    }
    if (index >= YAN_REGISTER_COUNT) {
        return YAN_OUT_OF_BOUNDS;
    }
    /* RV32I x0 discards writes, including writes of nonzero values. */
    if (index != 0) {
        cpu->regs[index] = value;
    }
    return YAN_OK;
}
