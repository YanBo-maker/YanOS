#include <string.h>

#include "yan/cpu.h"
#include "test.h"

static int reset_and_registers(void)
{
    YanCpu cpu = {0};
    YanCpu other = {0};
    uint32_t value = UINT32_MAX;
    CHECK(yan_cpu_reset(&cpu, UINT32_C(0x80000000)) == YAN_OK);
    CHECK(cpu.pc == UINT32_C(0x80000000));
    for (uint32_t index = 0; index < YAN_REGISTER_COUNT; ++index) {
        CHECK(yan_cpu_read_reg(&cpu, index, &value) == YAN_OK && value == 0);
        CHECK(yan_cpu_write_reg(&cpu, index, UINT32_MAX - index) == YAN_OK);
    }
    CHECK(yan_cpu_read_reg(&cpu, 0, &value) == YAN_OK && value == 0);
    CHECK(cpu.regs[0] == 0);
    for (uint32_t index = 1; index < YAN_REGISTER_COUNT; ++index) {
        CHECK(yan_cpu_read_reg(&cpu, index, &value) == YAN_OK);
        CHECK(value == UINT32_MAX - index);
    }
    CHECK(yan_cpu_write_reg(&cpu, 1, UINT32_C(0x80000000)) == YAN_OK);
    CHECK(yan_cpu_read_reg(&cpu, 1, &value) == YAN_OK && value == UINT32_C(0x80000000));
    CHECK(yan_cpu_reset(&other, 0) == YAN_OK);
    CHECK(yan_cpu_write_reg(&other, 31, 42) == YAN_OK);
    CHECK(yan_cpu_reset(&cpu, UINT32_C(0xfffffffc)) == YAN_OK);
    CHECK(cpu.pc == UINT32_C(0xfffffffc));
    for (uint32_t index = 0; index < YAN_REGISTER_COUNT; ++index) {
        CHECK(yan_cpu_read_reg(&cpu, index, &value) == YAN_OK && value == 0);
    }
    CHECK(yan_cpu_read_reg(&other, 31, &value) == YAN_OK && value == 42);
    CHECK(other.pc == 0);
    return EXIT_SUCCESS;
}

static int register_errors_preserve_state(void)
{
    YanCpu cpu = {0};
    uint32_t value = UINT32_C(0xaabbccdd);
    CHECK(yan_cpu_reset(&cpu, UINT32_C(0x80000000)) == YAN_OK);
    CHECK(yan_cpu_write_reg(&cpu, 31, 123) == YAN_OK);
    YanCpu before = cpu;
    CHECK(yan_cpu_read_reg(&cpu, 32, &value) == YAN_OUT_OF_BOUNDS);
    CHECK(yan_cpu_read_reg(&cpu, UINT32_MAX, &value) == YAN_OUT_OF_BOUNDS);
    CHECK(yan_cpu_write_reg(&cpu, 32, 0) == YAN_OUT_OF_BOUNDS);
    CHECK(yan_cpu_write_reg(&cpu, UINT32_MAX, 0) == YAN_OUT_OF_BOUNDS);
    CHECK(yan_cpu_read_reg(NULL, 0, &value) == YAN_INVALID_ARGUMENT);
    CHECK(yan_cpu_read_reg(&cpu, 0, NULL) == YAN_INVALID_ARGUMENT);
    CHECK(yan_cpu_write_reg(NULL, 0, 0) == YAN_INVALID_ARGUMENT);
    CHECK(value == UINT32_C(0xaabbccdd));
    CHECK(cpu.pc == before.pc && memcmp(cpu.regs, before.regs, sizeof cpu.regs) == 0);
    return EXIT_SUCCESS;
}

static int reset_errors_preserve_state(void)
{
    YanCpu cpu = {0};
    CHECK(yan_cpu_reset(&cpu, UINT32_C(0x80000000)) == YAN_OK);
    CHECK(yan_cpu_write_reg(&cpu, 5, 77) == YAN_OK);
    YanCpu before = cpu;
    for (uint32_t offset = 1; offset <= 3; ++offset) {
        CHECK(yan_cpu_reset(&cpu, before.pc + offset) == YAN_UNALIGNED);
        CHECK(cpu.pc == before.pc && memcmp(cpu.regs, before.regs, sizeof cpu.regs) == 0);
    }
    CHECK(yan_cpu_reset(NULL, 0) == YAN_INVALID_ARGUMENT);
    return EXIT_SUCCESS;
}

int main(void)
{
    RUN_TEST(reset_and_registers);
    RUN_TEST(register_errors_preserve_state);
    RUN_TEST(reset_errors_preserve_state);
    return EXIT_SUCCESS;
}
