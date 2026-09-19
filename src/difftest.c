#include "yan/difftest.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>

/* Appending reports failure out of band: a written length can legitimately be
 * zero only for an empty format, which this module never uses. */
#define REPORT_FAILED SIZE_MAX

static uint32_t observable(const YanCpuState *state, uint32_t index)
{
    return index == 0 ? 0 : state->regs[index];
}

YanStatus yan_difftest_compare(const YanCpuState *dut, const YanCpuState *ref,
                               YanDiffResult *result)
{
    if (dut == NULL || ref == NULL || result == NULL) {
        return YAN_INVALID_ARGUMENT;
    }
    *result = (YanDiffResult){0};
    result->pc_dut = dut->pc;
    result->pc_ref = ref->pc;
    result->pc_equal = dut->pc == ref->pc;
    result->first_differing_reg = YAN_REGISTER_COUNT;
    for (uint32_t index = 0; index < YAN_REGISTER_COUNT; ++index) {
        const uint32_t left = observable(dut, index);
        const uint32_t right = observable(ref, index);
        if (left == right) {
            continue;
        }
        result->differing |= UINT32_C(1) << index;
        if (result->first_differing_reg == YAN_REGISTER_COUNT) {
            result->first_differing_reg = index;
            result->reg_dut = left;
            result->reg_ref = right;
        }
    }
    result->registers_equal = result->differing == 0;
    result->equal = result->pc_equal && result->registers_equal;
    return YAN_OK;
}

static size_t append(char *buffer, size_t size, size_t used,
                     const char *format, ...)
{
    if (used >= size) {
        return REPORT_FAILED;
    }
    va_list arguments;
    va_start(arguments, format);
    const int written = vsnprintf(buffer + used, size - used, format, arguments);
    va_end(arguments);
    if (written < 0 || (size_t)written >= size - used) {
        return REPORT_FAILED;
    }
    return used + (size_t)written;
}

static size_t append_registers(char *buffer, size_t size, size_t used,
                               const YanCpuState *state)
{
    used = append(buffer, size, used, "[");
    for (uint32_t index = 0; used != REPORT_FAILED && index < YAN_REGISTER_COUNT;
         ++index) {
        used = append(buffer, size, used, index == 0 ? "%" PRIu32 : ",%" PRIu32,
                      observable(state, index));
    }
    return used == REPORT_FAILED ? REPORT_FAILED : append(buffer, size, used, "]");
}

size_t yan_difftest_report(const YanDiffResult *result, const YanCpuState *dut,
                           const YanCpuState *ref, uint64_t step,
                           uint32_t executed_pc, uint32_t instruction,
                           char *buffer, size_t size)
{
    if (result == NULL || dut == NULL || ref == NULL || buffer == NULL ||
        size == 0) {
        return 0;
    }
    size_t used = append(buffer, size, 0,
                         "{\"step\":%" PRIu64 ",\"pc\":%" PRIu32
                         ",\"insn\":%" PRIu32 ",\"pc_next\":%" PRIu32
                         ",\"pc_ref_next\":%" PRIu32
                         ",\"pc_differ\":%d,\"first_differing_reg\":%" PRIu32
                         ",\"reg_dut\":%" PRIu32 ",\"reg_ref\":%" PRIu32
                         ",\"dut_regs\":",
                         step, executed_pc, instruction, result->pc_dut,
                         result->pc_ref, !result->pc_equal,
                         result->first_differing_reg, result->reg_dut,
                         result->reg_ref);
    if (used != REPORT_FAILED) {
        used = append_registers(buffer, size, used, dut);
    }
    if (used != REPORT_FAILED) {
        used = append(buffer, size, used, ",\"ref_regs\":");
    }
    if (used != REPORT_FAILED) {
        used = append_registers(buffer, size, used, ref);
    }
    if (used != REPORT_FAILED) {
        used = append(buffer, size, used, "}\n");
    }
    return used == REPORT_FAILED ? 0 : used;
}
