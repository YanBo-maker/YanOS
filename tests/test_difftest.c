#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "yan/difftest.h"
#include "unity.h"

void setUp(void)
{
}

void tearDown(void)
{
}

static YanCpuState make_state(uint32_t pc, uint32_t seed)
{
    YanCpuState state = {0};
    state.pc = pc;
    for (uint32_t index = 0; index < YAN_REGISTER_COUNT; ++index) {
        state.regs[index] = seed + index;
    }
    return state;
}

static void equal_states_compare_equal(void)
{
    const YanCpuState dut = make_state(UINT32_C(0x80000010), 7);
    YanCpuState ref = dut;
    YanDiffResult result = {0};
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_difftest_compare(&dut, &ref, &result));
    TEST_ASSERT_TRUE(result.equal);
    TEST_ASSERT_TRUE(result.pc_equal);
    TEST_ASSERT_TRUE(result.registers_equal);
    TEST_ASSERT_EQUAL_HEX32(0, result.differing);
    TEST_ASSERT_EQUAL_UINT32(YAN_REGISTER_COUNT, result.first_differing_reg);
}

static void pc_only_difference_is_reported(void)
{
    YanCpuState dut = make_state(UINT32_C(0x80000010), 3);
    YanCpuState ref = dut;
    ref.pc = UINT32_C(0x80000020);
    YanDiffResult result = {0};
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_difftest_compare(&dut, &ref, &result));
    TEST_ASSERT_FALSE(result.equal);
    TEST_ASSERT_FALSE(result.pc_equal);
    TEST_ASSERT_TRUE(result.registers_equal);
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0x80000010), result.pc_dut);
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0x80000020), result.pc_ref);
}

static void first_differing_register_is_located(void)
{
    YanCpuState dut = make_state(UINT32_C(0x80000000), 1);
    YanCpuState ref = dut;
    ref.regs[9] = UINT32_C(0xdeadbeef);
    ref.regs[3] = UINT32_C(0x12345678);
    YanDiffResult result = {0};
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_difftest_compare(&dut, &ref, &result));
    TEST_ASSERT_FALSE(result.equal);
    TEST_ASSERT_TRUE(result.pc_equal);
    TEST_ASSERT_FALSE(result.registers_equal);
    TEST_ASSERT_EQUAL_UINT32(3, result.first_differing_reg);
    TEST_ASSERT_EQUAL_HEX32(dut.regs[3], result.reg_dut);
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0x12345678), result.reg_ref);
    TEST_ASSERT_TRUE((result.differing & (UINT32_C(1) << 3)) != 0);
    TEST_ASSERT_TRUE((result.differing & (UINT32_C(1) << 9)) != 0);
    TEST_ASSERT_EQUAL_HEX32((UINT32_C(1) << 3) | (UINT32_C(1) << 9),
                            result.differing);
}

static void zero_register_differences_are_ignored(void)
{
    YanCpuState dut = make_state(UINT32_C(0x80000000), 5);
    YanCpuState ref = dut;
    dut.regs[0] = UINT32_C(0xffffffff);
    ref.regs[0] = UINT32_C(0x00ff00ff);
    YanDiffResult result = {0};
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_difftest_compare(&dut, &ref, &result));
    TEST_ASSERT_TRUE(result.equal);
}

static void report_contains_both_register_files(void)
{
    YanCpuState dut = make_state(UINT32_C(0x80000018), 11);
    YanCpuState ref = dut;
    ref.regs[5] = UINT32_C(0x500);
    YanDiffResult result = {0};
    char line[1024];
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_difftest_compare(&dut, &ref, &result));
    const size_t length =
        yan_difftest_report(&result, &dut, &ref, 42, UINT32_C(0x80000018),
                            UINT32_C(0x0012a393), line, sizeof line);
    TEST_ASSERT_TRUE(length > 0);
    char expected[32];
    (void)snprintf(expected, sizeof expected, "\"insn\":%" PRIu32,
                   UINT32_C(0x0012a393));
    TEST_ASSERT_NOT_NULL(strstr(line, "\"step\":42"));
    TEST_ASSERT_NOT_NULL(strstr(line, "\"pc\":2147483672"));
    TEST_ASSERT_NOT_NULL(strstr(line, expected));
    TEST_ASSERT_NOT_NULL(strstr(line, "\"first_differing_reg\":5"));
    TEST_ASSERT_NOT_NULL(strstr(line, "\"dut_regs\":["));
    TEST_ASSERT_NOT_NULL(strstr(line, "\"ref_regs\":["));
    TEST_ASSERT_EQUAL_INT('\n', line[length - 1]);
}

static void report_rejects_small_buffers(void)
{
    YanCpuState dut = make_state(UINT32_C(0x80000000), 1);
    YanCpuState ref = dut;
    ref.regs[1] = 99;
    YanDiffResult result = {0};
    char line[16];
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_difftest_compare(&dut, &ref, &result));
    TEST_ASSERT_EQUAL_UINT(0, yan_difftest_report(&result, &dut, &ref, 1,
                                                  UINT32_C(0x80000000), 2, line,
                                                  sizeof line));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT,
                          yan_difftest_compare(NULL, &ref, &result));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT,
                          yan_difftest_compare(&dut, NULL, &result));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT,
                          yan_difftest_compare(&dut, &ref, NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(equal_states_compare_equal);
    RUN_TEST(pc_only_difference_is_reported);
    RUN_TEST(first_differing_register_is_located);
    RUN_TEST(zero_register_differences_are_ignored);
    RUN_TEST(report_contains_both_register_files);
    RUN_TEST(report_rejects_small_buffers);
    return UNITY_END();
}
