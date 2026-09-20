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
    cpu->csr.mstatus = YAN_MSTATUS_MPP;
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

YanStatus yan_cpu_snapshot(const YanCpu *cpu, YanCpuState *state)
{
    if (cpu == NULL || state == NULL) {
        return YAN_INVALID_ARGUMENT;
    }
    *state = *cpu;
    state->regs[0] = 0;
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

YanStatus yan_cpu_poll_interrupts(YanCpu *cpu, const YanBus *bus)
{
    if (cpu == NULL || bus == NULL) {
        return YAN_INVALID_ARGUMENT;
    }
    /* mip is not storage: every instruction boundary re-reads the devices. */
    cpu->csr.mip = yan_bus_pending_interrupts(bus) & YAN_MIE_MASK;
    return YAN_OK;
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

static uint32_t m_extension_operation(uint32_t funct3, uint32_t left,
                                       uint32_t right)
{
    const int64_t signed_left = left <= INT32_MAX ? (int64_t)left :
        (int64_t)left - INT64_C(4294967296);
    const int64_t signed_right = right <= INT32_MAX ? (int64_t)right :
        (int64_t)right - INT64_C(4294967296);
    switch (funct3) {
    case 0: return (uint32_t)((uint64_t)left * right);
    case 1: return (uint32_t)(((uint64_t)(signed_left * signed_right)) >> 32);
    case 2: return (uint32_t)(((uint64_t)(signed_left * (int64_t)(uint64_t)right)) >> 32);
    case 3: return (uint32_t)(((uint64_t)left * right) >> 32);
    case 4:
        if (right == 0) return UINT32_MAX;
        if (left == UINT32_C(0x80000000) && right == UINT32_MAX) return left;
        return (uint32_t)(signed_left / signed_right);
    case 5: return right == 0 ? UINT32_MAX : left / right;
    case 6:
        if (right == 0) return left;
        if (left == UINT32_C(0x80000000) && right == UINT32_MAX) return 0;
        return (uint32_t)(signed_left % signed_right);
    default: return right == 0 ? left : left % right;
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
    if (register_op && upper == UINT32_C(0x01)) {
        const uint32_t rs1 = (instruction >> 15) & UINT32_C(31);
        const uint32_t rs2 = (instruction >> 20) & UINT32_C(31);
        uint32_t left = 0, right = 0;
        (void)yan_cpu_read_reg(cpu, rs1, &left);
        (void)yan_cpu_read_reg(cpu, rs2, &right);
        *value = m_extension_operation(funct3, left, right);
        return YAN_OK;
    }
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
                             uint32_t instruction, uint32_t *value, uint32_t *fault_address)
{
    const uint32_t kind = (instruction >> 12) & UINT32_C(7);
    if (kind != 0 && kind != 1 && kind != 2 && kind != 4 && kind != 5) {
        return YAN_UNSUPPORTED_INSTRUCTION;
    }
    uint32_t base = 0;
    (void)yan_cpu_read_reg(cpu, (instruction >> 15) & UINT32_C(31), &base);
    const uint32_t address = base + sign_extend(instruction >> 20, 12);
    *fault_address = address;
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

static YanStatus store_value(const YanCpu *cpu, YanBus *bus, uint32_t instruction,
                              uint32_t *fault_address)
{
    const uint32_t kind = (instruction >> 12) & UINT32_C(7);
    if (kind > 2) {
        return YAN_UNSUPPORTED_INSTRUCTION;
    }
    const uint32_t offset = ((instruction >> 25) << 5) |
                             ((instruction >> 7) & UINT32_C(31));
    uint32_t base = 0, value = 0;
    (void)yan_cpu_read_reg(cpu, (instruction >> 15) & UINT32_C(31), &base);
    (void)yan_cpu_read_reg(cpu, (instruction >> 20) & UINT32_C(31), &value);
    const uint32_t address = base + sign_extend(offset, 12);
    *fault_address = address;
    return yan_bus_write(bus, address, (size_t)1 << kind, value).status;
}

static YanStatus execute_csr(YanCpu *cpu, uint32_t instruction, uint32_t *value)
{
    const uint32_t kind = (instruction >> 12) & UINT32_C(7);
    if (kind == 0 || kind == 4) {
        return YAN_UNSUPPORTED_INSTRUCTION;
    }
    const uint32_t address = instruction >> 20;
    const uint32_t source_index = (instruction >> 15) & UINT32_C(31);
    uint32_t operand = source_index;
    if (kind < 4) {
        (void)yan_cpu_read_reg(cpu, source_index, &operand);
    }
    const uint32_t operation = kind & UINT32_C(3);
    const uint32_t rd = (instruction >> 7) & UINT32_C(31);
    uint32_t old = 0;
    YanStatus status;
    /* CSRRW[I] with rd=x0 suppresses the read; the write validates the CSR. */
    if (operation != 1 || rd != 0) {
        status = yan_cpu_read_csr(cpu, address, &old);
        if (status != YAN_OK) {
            return status;
        }
    }
    if (operation == 1 || source_index != 0) {
        uint32_t updated = operation == 1 ? operand :
            (operation == 2 ? old | operand : old & ~operand);
        status = yan_cpu_write_csr(cpu, address, updated);
        if (status != YAN_OK) {
            return status;
        }
    }
    *value = old;
    return YAN_OK;
}

static YanStatus enter_trap(YanCpu *cpu, uint32_t cause, uint32_t value)
{
    cpu->csr.mepc = cpu->pc & UINT32_C(0xfffffffc);
    cpu->csr.mcause = cause;
    cpu->csr.mtval = value;
    cpu->csr.mstatus = YAN_MSTATUS_MPP |
        ((cpu->csr.mstatus & YAN_MSTATUS_MIE) != 0 ? YAN_MSTATUS_MPIE : 0);
    cpu->pc = cpu->csr.mtvec & UINT32_C(0xfffffffc);
    return YAN_TRAP;
}

/* Machine-level interrupts are ordered by privilege of the source, not by
 * arrival: external (11) outranks timer (7), which outranks software (3). */
static uint32_t interrupt_cause(uint32_t pending_and_enabled)
{
    if ((pending_and_enabled & YAN_INTERRUPT_MEIP) != 0) {
        return YAN_MCAUSE_MEIP;
    }
    if ((pending_and_enabled & YAN_INTERRUPT_MTIP) != 0) {
        return YAN_MCAUSE_MTIP;
    }
    return YAN_MCAUSE_MSIP;
}

/* An interrupt differs from a synchronous exception: no instruction faulted, so
 * mtval is zero and mepc names the instruction that has not run yet. */
static YanStatus enter_interrupt(YanCpu *cpu)
{
    const uint32_t cause = interrupt_cause(cpu->csr.mip & cpu->csr.mie);
    return enter_trap(cpu, YAN_MCAUSE_INTERRUPT | cause, 0);
}

static YanStatus access_fault(YanCpu *cpu, YanStatus status, uint32_t cause, uint32_t address)
{
    if (status == YAN_UNALIGNED) {
        return enter_trap(cpu, cause, address);
    }
    if (status == YAN_UNMAPPED || status == YAN_OUT_OF_BOUNDS) {
        return enter_trap(cpu, cause + 1, address);
    }
    return status;
}

YanStatus yan_cpu_step(YanCpu *cpu, YanBus *bus)
{
    /* Interrupts are sampled at the instruction boundary and take precedence
     * over any exception of the instruction that would have been fetched. A
     * handler entered this way has MIE cleared, so this step cannot nest. */
    YanStatus polled = yan_cpu_poll_interrupts(cpu, bus);
    if (polled != YAN_OK) {
        return polled;
    }
    if ((cpu->csr.mstatus & YAN_MSTATUS_MIE) != 0 &&
        (cpu->csr.mip & cpu->csr.mie & YAN_MIE_MASK) != 0) {
        return enter_interrupt(cpu);
    }
    uint32_t instruction = 0;
    YanBusResult fetch = yan_cpu_fetch(cpu, bus, &instruction);
    if (fetch.status != YAN_OK) {
        return access_fault(cpu, fetch.status, 0, fetch.address);
    }
    uint32_t value = 0;
    uint32_t fault_address = 0;
    uint32_t next_pc = cpu->pc + UINT32_C(4);
    const uint32_t opcode = instruction & UINT32_C(0x7f);
    const int branch = opcode == UINT32_C(0x63);
    const int store = opcode == UINT32_C(0x23);
    /* Zifencei: FENCE (funct3=0) and FENCE.I (funct3=1) only advance the PC.
     * A store may overwrite the instruction already fetched and every step
     * re-reads RAM, so no instruction stream synchronisation is needed. */
    const int fence = opcode == UINT32_C(0x0f) && ((instruction >> 12) & UINT32_C(7)) <= 1;
    const int mret = instruction == UINT32_C(0x30200073);
    YanStatus status = YAN_OK;
    if (instruction == UINT32_C(0x00000073)) {
        return enter_trap(cpu, 11, 0);
    } else if (instruction == UINT32_C(0x00100073)) {
        return enter_trap(cpu, 3, cpu->pc);
    } else if (mret) {
        next_pc = cpu->csr.mepc & UINT32_C(0xfffffffc);
        cpu->csr.mstatus = YAN_MSTATUS_MPP | YAN_MSTATUS_MPIE |
            ((cpu->csr.mstatus & YAN_MSTATUS_MPIE) != 0 ? YAN_MSTATUS_MIE : 0);
    } else if (opcode == UINT32_C(0x73)) {
        status = execute_csr(cpu, instruction, &value);
    } else if (fence) {
        /* One hart, synchronous RAM accesses, no asynchronous devices. */
    } else if (opcode == UINT32_C(0x03)) {
        status = load_value(cpu, bus, instruction, &value, &fault_address);
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
            return enter_trap(cpu, 2, instruction);
        }
        uint32_t source = 0;
        (void)yan_cpu_read_reg(cpu, (instruction >> 15) & UINT32_C(31), &source);
        value = next_pc;
        /* Read rs1 before writing rd, then clear bit zero before alignment. */
        next_pc = (source + sign_extend(instruction >> 20, 12)) & UINT32_C(0xfffffffe);
    } else if (!store) {
        status = compute_integer_result(cpu, instruction, &value);
    }
    if (status != YAN_OK) {
        if (status == YAN_UNSUPPORTED_INSTRUCTION) {
            return enter_trap(cpu, 2, instruction);
        }
        return access_fault(cpu, status, 4, fault_address);
    }
    /* A non-taken branch keeps PC+4; it does not check its encoded target. */
    if (next_pc % 4 != 0) {
        return enter_trap(cpu, 0, next_pc);
    }
    const uint32_t rd = (instruction >> 7) & UINT32_C(31);
    /* Commit only after decoding and reading every source operand. */
    if (store) {
        /* Bus validates the entire write before modifying RAM. */
        status = store_value(cpu, bus, instruction, &fault_address);
        if (status != YAN_OK) {
            if (status == YAN_UNSUPPORTED_INSTRUCTION) {
                return enter_trap(cpu, 2, instruction);
            }
            return access_fault(cpu, status, 6, fault_address);
        }
    } else if (!branch && !fence && !mret) {
        (void)yan_cpu_write_reg(cpu, rd, value);
    }
    cpu->pc = next_pc;
    return YAN_OK;
}
