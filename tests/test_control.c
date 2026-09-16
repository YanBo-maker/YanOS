#include <string.h>

#include "yan/cpu.h"
#include "unity.h"

static YanRam ram;
static YanBus bus;
static YanCpu cpu;

void setUp(void)
{
    ram = (YanRam){0};
    bus = (YanBus){0};
    cpu = (YanCpu){0};
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_init(&ram, 8));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_init(&bus, &ram, UINT32_C(0x80000000)));
    for (uint32_t index = 1; index < 32; ++index) {
        TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, index, index * 17));
    }
}

void tearDown(void)
{
    yan_ram_destroy(&ram);
}

static void check_control(uint32_t word, YanStatus status, uint32_t target, uint32_t rd)
{
    cpu.pc = bus.ram_base;
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_write(&ram, 0, 4, word));
    YanCpu expected = cpu;
    if (status == YAN_OK && rd != 0) {
        expected.regs[rd] = cpu.pc + UINT32_C(4);
    }
    uint8_t memory[8];
    memcpy(memory, ram.data, sizeof memory);
    TEST_ASSERT_EQUAL_INT(status, yan_cpu_step(&cpu, &bus));
    TEST_ASSERT_EQUAL_HEX32(status == YAN_OK ? target : expected.pc, cpu.pc);
    TEST_ASSERT_EQUAL_HEX32_ARRAY(expected.regs, cpu.regs, 32);
    TEST_ASSERT_EQUAL_MEMORY(memory, ram.data, sizeof memory);
}

/* Test input encoders are separate from the fixed machine-code vectors. */
static uint32_t branch_word(int32_t offset, uint32_t funct3, uint32_t rs1, uint32_t rs2)
{
    const uint32_t bits = (uint32_t)offset & UINT32_C(0x1fff);
    return ((bits & 0x1000) << 19) | ((bits & 0x800) >> 4) |
           ((bits & 0x7e0) << 20) | ((bits & 0x1e) << 7) |
           (rs1 << 15) | (rs2 << 20) | (funct3 << 12) | UINT32_C(0x63);
}

static int64_t signed_value(uint32_t value)
{
    return value <= INT32_MAX ? (int64_t)value : (int64_t)value - INT64_C(4294967296);
}

static void branch_fixed_vectors(void)
{
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, 6, UINT32_MAX));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, 7, 1));
    const struct { uint32_t word; int taken; } cases[] = {
        {0x00730463, 0}, {0x00731463, 1}, {0x00734463, 1},
        {0x00735463, 0}, {0x00736463, 0}, {0x00737463, 1}
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        check_control(cases[i].word, YAN_OK, bus.ram_base + (cases[i].taken ? 8 : 4), 0);
    }
    check_control(0xfe000ee3, YAN_OK, bus.ram_base - 4, 0); /* BEQ x0,x0,-4 */
    check_control(0x00000063, YAN_OK, bus.ram_base, 0);
    check_control(0x00000163, YAN_UNALIGNED, 0, 0);
    check_control(0x00001163, YAN_OK, bus.ram_base + 4, 0);
}

static void branch_condition_matrix(void)
{
    const uint32_t values[] = {0, 1, 0x7fffffff, 0x80000000, 0x80000001, 0xffffffff};
    const uint32_t conditions[] = {0, 1, 4, 5, 6, 7};
    for (size_t i = 0; i < sizeof values / sizeof values[0]; ++i) {
        for (size_t j = 0; j < sizeof values / sizeof values[0]; ++j) {
            const uint32_t left = values[i], right = values[j];
            const int taken[] = {left == right, left != right,
                signed_value(left) < signed_value(right), signed_value(left) >= signed_value(right),
                left < right, left >= right};
            TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, 6, left));
            TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, 7, right));
            for (size_t condition = 0; condition < 6; ++condition) {
                check_control(branch_word(8, conditions[condition], 6, 7), YAN_OK,
                              bus.ram_base + (taken[condition] ? 8 : 4), 0);
                check_control(branch_word(2, conditions[condition], 6, 7),
                              taken[condition] ? YAN_UNALIGNED : YAN_OK, bus.ram_base + 4, 0);
            }
        }
    }
}

static void branch_all_offsets(void)
{
    for (int32_t offset = -4096; offset <= 4094; offset += 2) {
        const uint32_t target = (uint32_t)((int64_t)bus.ram_base + offset);
        check_control(branch_word(offset, 0, 0, 0), offset % 4 == 0 ? YAN_OK : YAN_UNALIGNED,
                      target, 0);
        check_control(branch_word(offset, 1, 0, 0), YAN_OK, bus.ram_base + 4, 0);
    }
}

static void branch_register_fields(void)
{
    const uint32_t conditions[] = {0, 1, 4, 5, 6, 7};
    for (uint32_t rs1 = 0; rs1 < 32; ++rs1) {
        for (uint32_t rs2 = 0; rs2 < 32; ++rs2) {
            const int taken[] = {rs1 == rs2, rs1 != rs2, rs1 < rs2, rs1 >= rs2,
                                 rs1 < rs2, rs1 >= rs2};
            for (size_t condition = 0; condition < 6; ++condition) {
                check_control(branch_word(8, conditions[condition], rs1, rs2), YAN_OK,
                              bus.ram_base + (taken[condition] ? 8 : 4), 0);
            }
        }
    }
}

static void branch_wrap_and_deferred_fetch(void)
{
    check_control(branch_word(8, 0, 0, 0), YAN_OK, bus.ram_base + 8, 0);
    YanCpu before = cpu;
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED, yan_cpu_step(&cpu, &bus));
    TEST_ASSERT_EQUAL_HEX32(before.pc, cpu.pc);
    TEST_ASSERT_EQUAL_HEX32_ARRAY(before.regs, cpu.regs, 32);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_init(&bus, &ram, UINT32_C(0xfffffff8)));
    check_control(branch_word(8, 0, 0, 0), YAN_OK, 0, 0);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_init(&bus, &ram, 0));
    check_control(branch_word(-4, 0, 0, 0), YAN_OK, UINT32_C(0xfffffffc), 0);
}

static void branch_invalid_encoding_and_fetch(void)
{
    check_control(branch_word(2, 2, 0, 0), YAN_UNSUPPORTED_INSTRUCTION, 0, 0);
    check_control(branch_word(2, 3, 0, 0), YAN_UNSUPPORTED_INSTRUCTION, 0, 0);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_write(&ram, 0, 4, UINT32_C(0x00000063)));
    cpu.pc = bus.ram_base + 1;
    YanCpu before = cpu;
    TEST_ASSERT_EQUAL_INT(YAN_UNALIGNED, yan_cpu_step(&cpu, &bus));
    TEST_ASSERT_EQUAL_HEX32(before.pc, cpu.pc);
    TEST_ASSERT_EQUAL_HEX32_ARRAY(before.regs, cpu.regs, 32);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(branch_fixed_vectors);
    RUN_TEST(branch_condition_matrix);
    RUN_TEST(branch_all_offsets);
    RUN_TEST(branch_register_fields);
    RUN_TEST(branch_wrap_and_deferred_fetch);
    RUN_TEST(branch_invalid_encoding_and_fetch);
    return UNITY_END();
}
