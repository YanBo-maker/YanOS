#include <string.h>

#include "yan/cpu.h"
#include "unity.h"

void setUp(void)
{
}

void tearDown(void)
{
}

static void reset_and_registers(void)
{
    YanCpu cpu = {0};
    YanCpu other = {0};
    uint32_t value = UINT32_MAX;
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_reset(&cpu, UINT32_C(0x80000000)));
    TEST_ASSERT_TRUE(cpu.pc == UINT32_C(0x80000000));
    for (uint32_t index = 0; index < YAN_REGISTER_COUNT; ++index) {
        TEST_ASSERT_TRUE(yan_cpu_read_reg(&cpu, index, &value) == YAN_OK && value == 0);
        TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, index, UINT32_MAX - index));
    }
    TEST_ASSERT_TRUE(yan_cpu_read_reg(&cpu, 0, &value) == YAN_OK && value == 0);
    TEST_ASSERT_TRUE(cpu.regs[0] == 0);
    for (uint32_t index = 1; index < YAN_REGISTER_COUNT; ++index) {
        TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_read_reg(&cpu, index, &value));
        TEST_ASSERT_TRUE(value == UINT32_MAX - index);
    }
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, 1, UINT32_C(0x80000000)));
    TEST_ASSERT_TRUE(yan_cpu_read_reg(&cpu, 1, &value) == YAN_OK && value == UINT32_C(0x80000000));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_reset(&other, 0));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&other, 31, 42));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_reset(&cpu, UINT32_C(0xfffffffc)));
    TEST_ASSERT_TRUE(cpu.pc == UINT32_C(0xfffffffc));
    for (uint32_t index = 0; index < YAN_REGISTER_COUNT; ++index) {
        TEST_ASSERT_TRUE(yan_cpu_read_reg(&cpu, index, &value) == YAN_OK && value == 0);
    }
    TEST_ASSERT_TRUE(yan_cpu_read_reg(&other, 31, &value) == YAN_OK && value == 42);
    TEST_ASSERT_TRUE(other.pc == 0);
}

static void register_errors_preserve_state(void)
{
    YanCpu cpu = {0};
    uint32_t value = UINT32_C(0xaabbccdd);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_reset(&cpu, UINT32_C(0x80000000)));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, 31, 123));
    YanCpu before = cpu;
    TEST_ASSERT_EQUAL_INT(YAN_OUT_OF_BOUNDS, yan_cpu_read_reg(&cpu, 32, &value));
    TEST_ASSERT_EQUAL_INT(YAN_OUT_OF_BOUNDS, yan_cpu_read_reg(&cpu, UINT32_MAX, &value));
    TEST_ASSERT_EQUAL_INT(YAN_OUT_OF_BOUNDS, yan_cpu_write_reg(&cpu, 32, 0));
    TEST_ASSERT_EQUAL_INT(YAN_OUT_OF_BOUNDS, yan_cpu_write_reg(&cpu, UINT32_MAX, 0));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT, yan_cpu_read_reg(NULL, 0, &value));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT, yan_cpu_read_reg(&cpu, 0, NULL));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT, yan_cpu_write_reg(NULL, 0, 0));
    TEST_ASSERT_TRUE(value == UINT32_C(0xaabbccdd));
    TEST_ASSERT_TRUE(cpu.pc == before.pc && memcmp(cpu.regs, before.regs, sizeof cpu.regs) == 0);
}

static void reset_errors_preserve_state(void)
{
    YanCpu cpu = {0};
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_reset(&cpu, UINT32_C(0x80000000)));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, 5, 77));
    YanCpu before = cpu;
    for (uint32_t offset = 1; offset <= 3; ++offset) {
        TEST_ASSERT_EQUAL_INT(YAN_UNALIGNED, yan_cpu_reset(&cpu, before.pc + offset));
        TEST_ASSERT_TRUE(cpu.pc == before.pc && memcmp(cpu.regs, before.regs, sizeof cpu.regs) == 0);
    }
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT, yan_cpu_reset(NULL, 0));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(reset_and_registers);
    RUN_TEST(register_errors_preserve_state);
    RUN_TEST(reset_errors_preserve_state);
    return UNITY_END();
}
