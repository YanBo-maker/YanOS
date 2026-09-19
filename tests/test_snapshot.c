#include <string.h>

#include "yan/cpu.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void snapshot_is_architectural_and_read_only(void)
{
    YanCpu cpu = {0};
    YanCpuState state = {0};
    cpu.pc = UINT32_C(0x80000004);
    cpu.regs[1] = UINT32_C(0x12345678);
    cpu.regs[0] = UINT32_MAX;
    cpu.csr.mcause = 11;
    YanCpu before = cpu;

    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_snapshot(&cpu, &state));
    TEST_ASSERT_EQUAL_MEMORY(&before, &cpu, sizeof cpu);
    TEST_ASSERT_EQUAL_HEX32(cpu.pc, state.pc);
    TEST_ASSERT_EQUAL_HEX32(cpu.regs[1], state.regs[1]);
    TEST_ASSERT_EQUAL_HEX32(0, state.regs[0]);
    TEST_ASSERT_EQUAL_HEX32(cpu.csr.mcause, state.csr.mcause);
}

static void snapshot_rejects_missing_arguments(void)
{
    YanCpu cpu = {0};
    YanCpuState state = {0};
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT, yan_cpu_snapshot(NULL, &state));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT, yan_cpu_snapshot(&cpu, NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(snapshot_is_architectural_and_read_only);
    RUN_TEST(snapshot_rejects_missing_arguments);
    return UNITY_END();
}
