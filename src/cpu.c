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

YanBusResult yan_cpu_fetch(const YanCpu *cpu, const YanBus *bus,
                           uint32_t *instruction)
{
    if (cpu == NULL) {
        YanBusResult result = {YAN_INVALID_ARGUMENT, 0, 4, YAN_ACCESS_FETCH};
        return result;
    }
    /* Fetch observes PC. Instruction execution will decide the next PC. */
    return yan_bus_fetch32(bus, cpu->pc, instruction);
}

YanStatus yan_cpu_step(YanCpu *cpu, const YanBus *bus)
{
    uint32_t instruction = 0;
    YanBusResult fetch = yan_cpu_fetch(cpu, bus, &instruction);
    if (fetch.status != YAN_OK) {
        return fetch.status;
    }
    const uint32_t opcode = instruction & UINT32_C(0x7f);
    const uint32_t funct3 = (instruction >> 12) & UINT32_C(7);
    if (opcode != UINT32_C(0x13) || funct3 != 0) {
        return YAN_UNSUPPORTED_INSTRUCTION;
    }

    const uint32_t rd = (instruction >> 7) & UINT32_C(31);
    const uint32_t rs1 = (instruction >> 15) & UINT32_C(31);
    uint32_t immediate = instruction >> 20;
    if ((immediate & UINT32_C(0x800)) != 0) {
        immediate |= UINT32_C(0xfffff000);
    }

    uint32_t source = 0;
    /* Decoded register indices are in range; fetch validated the CPU pointer. */
    (void)yan_cpu_read_reg(cpu, rs1, &source);
    const uint32_t value = source + immediate;
    const uint32_t next_pc = cpu->pc + UINT32_C(4);
    /* Unsigned arithmetic keeps the low 32 bits, including negative immediates. */
    (void)yan_cpu_write_reg(cpu, rd, value);
    cpu->pc = next_pc;
    return YAN_OK;
}
