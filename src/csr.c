#include "yan/cpu.h"

YanStatus yan_cpu_read_csr(const YanCpu *cpu, uint32_t address, uint32_t *value)
{
    if (cpu == NULL || value == NULL) {
        return YAN_INVALID_ARGUMENT;
    }
    switch (address) {
    case 0x300: *value = (cpu->csr.mstatus & UINT32_C(0x88)) | YAN_MSTATUS_MPP; break;
    case 0x305: *value = cpu->csr.mtvec & UINT32_C(0xfffffffc); break;
    case 0x340: *value = cpu->csr.mscratch; break;
    case 0x341: *value = cpu->csr.mepc & UINT32_C(0xfffffffc); break;
    case 0x342: *value = cpu->csr.mcause; break;
    case 0x343: *value = cpu->csr.mtval; break;
    case 0x301: case 0x304: case 0x310: case 0x344:
    case 0xf11: case 0xf12: case 0xf13: case 0xf14: *value = 0; break;
    default: return YAN_UNSUPPORTED_INSTRUCTION;
    }
    return YAN_OK;
}

YanStatus yan_cpu_write_csr(YanCpu *cpu, uint32_t address, uint32_t value)
{
    if (cpu == NULL) {
        return YAN_INVALID_ARGUMENT;
    }
    switch (address) {
    case 0x300: cpu->csr.mstatus = (value & UINT32_C(0x88)) | YAN_MSTATUS_MPP; break;
    case 0x305: cpu->csr.mtvec = value & UINT32_C(0xfffffffc); break;
    case 0x340: cpu->csr.mscratch = value; break;
    case 0x341: cpu->csr.mepc = value & UINT32_C(0xfffffffc); break;
    case 0x342: cpu->csr.mcause = value; break;
    case 0x343: cpu->csr.mtval = value; break;
    case 0x301: case 0x304: case 0x310: case 0x344: break;
    default: return YAN_UNSUPPORTED_INSTRUCTION;
    }
    return YAN_OK;
}
