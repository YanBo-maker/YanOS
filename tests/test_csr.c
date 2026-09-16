#include "yan/cpu.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void reset_and_masks(void)
{
    YanCpu cpu = {0};
    uint32_t value = 0;
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_reset(&cpu, 0));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_read_csr(&cpu, 0x300, &value));
    TEST_ASSERT_EQUAL_HEX32(0x1800, value);
    const uint32_t addresses[] = {0x300, 0x305, 0x340, 0x341, 0x342, 0x343};
    const uint32_t expected[] = {0x1888, 0xfffffffc, 0xffffffff, 0xfffffffc, 0xffffffff, 0xffffffff};
    for (size_t i = 0; i < 6; ++i) {
        TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_csr(&cpu, addresses[i], UINT32_MAX));
        TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_read_csr(&cpu, addresses[i], &value));
        TEST_ASSERT_EQUAL_HEX32(expected[i], value);
    }
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_reset(&cpu, 4));
    for (size_t i = 0; i < 6; ++i) {
        TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_read_csr(&cpu, addresses[i], &value));
        TEST_ASSERT_EQUAL_HEX32(i == 0 ? 0x1800 : 0, value);
    }
}

static void fixed_and_readonly_csrs(void)
{
    YanCpu cpu = {0};
    uint32_t value = 1;
    const uint32_t fixed[] = {0x301, 0x304, 0x310, 0x344};
    for (size_t i = 0; i < 4; ++i) {
        TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_csr(&cpu, fixed[i], UINT32_MAX));
        TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_read_csr(&cpu, fixed[i], &value));
        TEST_ASSERT_EQUAL_HEX32(0, value);
    }
    for (uint32_t address = 0xf11; address <= 0xf14; ++address) {
        TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_read_csr(&cpu, address, &value));
        TEST_ASSERT_EQUAL_HEX32(0, value);
        TEST_ASSERT_EQUAL_INT(YAN_UNSUPPORTED_INSTRUCTION, yan_cpu_write_csr(&cpu, address, 0));
    }
}

static void invalid_access_preserves_state(void)
{
    YanCpu cpu = {0};
    uint32_t value = 0x1234;
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_reset(&cpu, 4));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_csr(&cpu, 0x340, 42));
    YanCpu before = cpu;
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT, yan_cpu_read_csr(NULL, 0x300, &value));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT, yan_cpu_read_csr(&cpu, 0x300, NULL));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT, yan_cpu_write_csr(NULL, 0x300, 0));
    TEST_ASSERT_EQUAL_INT(YAN_UNSUPPORTED_INSTRUCTION, yan_cpu_read_csr(&cpu, 0x100, &value));
    TEST_ASSERT_EQUAL_INT(YAN_UNSUPPORTED_INSTRUCTION, yan_cpu_write_csr(&cpu, UINT32_MAX, 0));
    TEST_ASSERT_EQUAL_HEX32(0x1234, value);
    TEST_ASSERT_EQUAL_HEX32(before.pc, cpu.pc);
    TEST_ASSERT_EQUAL_MEMORY(&before.csr, &cpu.csr, sizeof cpu.csr);
    TEST_ASSERT_EQUAL_INT(YAN_UNALIGNED, yan_cpu_reset(&cpu, 1));
    TEST_ASSERT_EQUAL_MEMORY(&before.csr, &cpu.csr, sizeof cpu.csr);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(reset_and_masks);
    RUN_TEST(fixed_and_readonly_csrs);
    RUN_TEST(invalid_access_preserves_state);
    return UNITY_END();
}
