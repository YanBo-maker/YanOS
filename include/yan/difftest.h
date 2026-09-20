#ifndef YAN_DIFFTEST_H
#define YAN_DIFFTEST_H

#include <stddef.h>
#include <stdint.h>

#include "yan/cpu.h"
#include "yan/status.h"

/* Architectural state that an external reference model can also provide.
 * A NEMU riscv32 reference carries 32 general registers and pc and nothing
 * else, so a DiffTest can only observe those fields: CSR, privilege level,
 * traps and memory ordering stay outside its scope. */
typedef struct {
    int pc_equal;
    int registers_equal;
    int equal;
    uint32_t pc_dut;
    uint32_t pc_ref;
    uint32_t differing;          /* one bit per differing register */
    uint32_t first_differing_reg; /* YAN_REGISTER_COUNT when none differs */
    uint32_t reg_dut;
    uint32_t reg_ref;
} YanDiffResult;

/* Compares only the fields a reference model is able to mirror. x0 is
 * normalized to zero on both sides, matching its architectural meaning. */
YanStatus yan_difftest_compare(const YanCpuState *dut, const YanCpuState *ref,
                               YanDiffResult *result);

/* Writes one JSON line describing a mismatch, including both register files
 * and the instruction that produced them. `executed_pc` is the address of that
 * instruction; the DUT's and reference's post-instruction pc follow it as
 * `pc_next` and `pc_ref_next`. Returns the number of bytes written, or zero
 * when the buffer is too small. */
size_t yan_difftest_report(const YanDiffResult *result, const YanCpuState *dut,
                           const YanCpuState *ref, uint64_t step,
                           uint32_t executed_pc, uint32_t instruction,
                           char *buffer, size_t size);

#endif
