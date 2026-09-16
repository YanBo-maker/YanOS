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

static uint32_t arithmetic_shift_right(uint32_t value, uint32_t amount)
{
    if (amount == 0) {
        return value;
    }
    uint32_t result = value >> amount;
    if ((value & UINT32_C(0x80000000)) != 0) {
        result |= UINT32_MAX << (32 - amount);
    }
    return result;
}

/* funct3 is a validated three-bit field; all eight operations are defined. */
static uint32_t integer_operation(uint32_t funct3, uint32_t left,
                                  uint32_t right, int alternate)
{
    const uint32_t amount = right & UINT32_C(31);
    switch (funct3) {
    case 0: return alternate ? left - right : left + right;
    case 1: return left << amount;
    /* Flipping the sign bit orders two's-complement values as unsigned. */
    case 2: return (left ^ UINT32_C(0x80000000)) < (right ^ UINT32_C(0x80000000));
    case 3: return left < right;
    case 4: return left ^ right;
    case 5: return alternate ? arithmetic_shift_right(left, amount) : left >> amount;
    case 6: return left | right;
    default: return left & right;
    }
}

static YanStatus compute_integer_result(const YanCpu *cpu, uint32_t instruction,
                                        uint32_t *value)
{
    const uint32_t opcode = instruction & UINT32_C(0x7f);
    if (opcode == UINT32_C(0x37) || opcode == UINT32_C(0x17)) {
        const uint32_t immediate = instruction & UINT32_C(0xfffff000);
        *value = opcode == UINT32_C(0x37) ? immediate : cpu->pc + immediate;
        return YAN_OK;
    }
    const uint32_t funct3 = (instruction >> 12) & UINT32_C(7);
    if (opcode != UINT32_C(0x13) && opcode != UINT32_C(0x33)) {
        return YAN_UNSUPPORTED_INSTRUCTION;
    }
    const uint32_t upper = instruction >> 25;
    const int register_op = opcode == UINT32_C(0x33);
    if (register_op && upper != 0 &&
        !(upper == UINT32_C(0x20) && (funct3 == 0 || funct3 == 5))) {
        return YAN_UNSUPPORTED_INSTRUCTION;
    }
    if (!register_op && ((funct3 == 1 && upper != 0) ||
        (funct3 == 5 && upper != 0 && upper != UINT32_C(0x20)))) {
        return YAN_UNSUPPORTED_INSTRUCTION;
    }

    const uint32_t rs1 = (instruction >> 15) & UINT32_C(31);
    uint32_t immediate = instruction >> 20;
    if ((immediate & UINT32_C(0x800)) != 0) {
        immediate |= UINT32_C(0xfffff000);
    }

    uint32_t source = 0;
    /* Decoded register indices are in range; fetch validated the CPU pointer. */
    (void)yan_cpu_read_reg(cpu, rs1, &source);
    uint32_t operand = immediate;
    if (register_op) {
        const uint32_t rs2 = (instruction >> 20) & UINT32_C(31);
        (void)yan_cpu_read_reg(cpu, rs2, &operand);
    }
    const int alternate = upper == UINT32_C(0x20) && (register_op || funct3 == 5);
    *value = integer_operation(funct3, source, operand, alternate);
    return YAN_OK;
}

static uint32_t sign_extend(uint32_t value, uint32_t bits)
{
    const uint32_t sign = UINT32_C(1) << (bits - 1);
    return (value ^ sign) - sign;
}

static YanStatus compute_branch_target(const YanCpu *cpu, uint32_t instruction,
                                       uint32_t *target)
{
    uint32_t left = 0, right = 0;
    (void)yan_cpu_read_reg(cpu, (instruction >> 15) & UINT32_C(31), &left);
    (void)yan_cpu_read_reg(cpu, (instruction >> 20) & UINT32_C(31), &right);
    const int less = (left ^ UINT32_C(0x80000000)) < (right ^ UINT32_C(0x80000000));
    int taken;
    switch ((instruction >> 12) & UINT32_C(7)) {
    case 0: taken = left == right; break;
    case 1: taken = left != right; break;
    case 4: taken = less; break;
    case 5: taken = !less; break;
    case 6: taken = left < right; break;
    case 7: taken = left >= right; break;
    default: return YAN_UNSUPPORTED_INSTRUCTION;
    }
    *target = cpu->pc + UINT32_C(4);
    if (taken) {
        const uint32_t offset = ((instruction >> 31) << 12) |
            (((instruction >> 7) & UINT32_C(1)) << 11) |
            (((instruction >> 25) & UINT32_C(0x3f)) << 5) |
            (((instruction >> 8) & UINT32_C(0xf)) << 1);
        *target = cpu->pc + sign_extend(offset, 13);
    }
    return YAN_OK;
}

static YanStatus load_value(const YanCpu *cpu, const YanBus *bus,
                             uint32_t instruction, uint32_t *value)
{
    const uint32_t kind = (instruction >> 12) & UINT32_C(7);
    if (kind != 0 && kind != 1 && kind != 2 && kind != 4 && kind != 5) {
        return YAN_UNSUPPORTED_INSTRUCTION;
    }
    uint32_t base = 0;
    (void)yan_cpu_read_reg(cpu, (instruction >> 15) & UINT32_C(31), &base);
    const uint32_t address = base + sign_extend(instruction >> 20, 12);
    const size_t width = (size_t)1 << (kind & UINT32_C(3));
    YanBusResult result = yan_bus_read(bus, address, width, value);
    if (result.status != YAN_OK) {
        return result.status;
    }
    if (kind == 0 || kind == 1) {
        *value = sign_extend(*value, kind == 0 ? 8 : 16);
    }
    return YAN_OK;
}

YanStatus yan_cpu_step(YanCpu *cpu, const YanBus *bus)
{
    uint32_t instruction = 0;
    YanBusResult fetch = yan_cpu_fetch(cpu, bus, &instruction);
    if (fetch.status != YAN_OK) {
        return fetch.status;
    }
    uint32_t value = 0;
    uint32_t next_pc = cpu->pc + UINT32_C(4);
    const uint32_t opcode = instruction & UINT32_C(0x7f);
    const int branch = opcode == UINT32_C(0x63);
    YanStatus status = YAN_OK;
    if (opcode == UINT32_C(0x03)) {
        status = load_value(cpu, bus, instruction, &value);
    } else if (branch) {
        status = compute_branch_target(cpu, instruction, &next_pc);
    } else if (opcode == UINT32_C(0x6f)) {
        const uint32_t offset = ((instruction >> 31) << 20) |
            (instruction & UINT32_C(0xff000)) |
            (((instruction >> 20) & UINT32_C(1)) << 11) |
            (((instruction >> 21) & UINT32_C(0x3ff)) << 1);
        value = next_pc;
        next_pc = cpu->pc + sign_extend(offset, 21);
    } else if (opcode == UINT32_C(0x67)) {
        if (((instruction >> 12) & UINT32_C(7)) != 0) {
            return YAN_UNSUPPORTED_INSTRUCTION;
        }
        uint32_t source = 0;
        (void)yan_cpu_read_reg(cpu, (instruction >> 15) & UINT32_C(31), &source);
        value = next_pc;
        /* Read rs1 before writing rd, then clear bit zero before alignment. */
        next_pc = (source + sign_extend(instruction >> 20, 12)) & UINT32_C(0xfffffffe);
    } else {
        status = compute_integer_result(cpu, instruction, &value);
    }
    if (status != YAN_OK) {
        return status;
    }
    /* A non-taken branch keeps PC+4; it does not check its encoded target. */
    if (next_pc % 4 != 0) {
        return YAN_UNALIGNED;
    }
    const uint32_t rd = (instruction >> 7) & UINT32_C(31);
    /* Commit only after decoding and reading every source operand. */
    if (!branch) {
        (void)yan_cpu_write_reg(cpu, rd, value);
    }
    cpu->pc = next_pc;
    return YAN_OK;
}
