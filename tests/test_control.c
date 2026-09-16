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

static uint32_t jal_word(int32_t offset, uint32_t rd)
{
    const uint32_t bits = (uint32_t)offset & UINT32_C(0x1fffff);
    return ((bits & 0x100000) << 11) | (bits & 0xff000) |
           ((bits & 0x800) << 9) | ((bits & 0x7fe) << 20) | (rd << 7) | UINT32_C(0x6f);
}

static void jal_fixed_vectors(void)
{
    check_control(0x008002ef, YAN_OK, bus.ram_base + 8, 5);
    check_control(0xffdff2ef, YAN_OK, bus.ram_base - 4, 5);
    check_control(0x800002ef, YAN_OK, bus.ram_base - UINT32_C(0x100000), 5);
    check_control(0x7ffff2ef, YAN_UNALIGNED, 0, 5);
    check_control(0x0000006f, YAN_OK, bus.ram_base, 0);
    check_control(0x0020006f, YAN_UNALIGNED, 0, 0);
}

static void jal_offset_bits_and_boundaries(void)
{
    const int32_t boundaries[] = {-1048576, -1048574, -4096, -2048, -4, -2, 0, 2, 4,
                                   2048, 4096, 1048572, 1048574};
    for (size_t i = 0; i < sizeof boundaries / sizeof boundaries[0]; ++i) {
        const int32_t offset = boundaries[i];
        check_control(jal_word(offset, 1), offset % 4 == 0 ? YAN_OK : YAN_UNALIGNED,
                      (uint32_t)((int64_t)bus.ram_base + offset), 1);
    }
    for (uint32_t bit = 1; bit < 20; ++bit) {
        const int32_t offset = INT32_C(1) << bit;
        check_control(jal_word(offset, 31), bit == 1 ? YAN_UNALIGNED : YAN_OK,
                      bus.ram_base + (uint32_t)offset, 31);
        check_control(jal_word(-offset, 31), bit == 1 ? YAN_UNALIGNED : YAN_OK,
                      bus.ram_base - (uint32_t)offset, 31);
    }
}

static void jal_destinations_and_deferred_fetch(void)
{
    for (uint32_t rd = 0; rd < 32; ++rd) {
        check_control(jal_word(8, rd), YAN_OK, bus.ram_base + 8, rd);
        YanCpu before = cpu;
        TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED, yan_cpu_step(&cpu, &bus));
        TEST_ASSERT_EQUAL_HEX32(before.pc, cpu.pc);
        TEST_ASSERT_EQUAL_HEX32_ARRAY(before.regs, cpu.regs, 32);
        check_control(jal_word(2, rd), YAN_UNALIGNED, 0, rd);
    }
}

static void jal_address_and_link_wrap(void)
{
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_init(&bus, &ram, 0));
    check_control(jal_word(-4, 1), YAN_OK, UINT32_C(0xfffffffc), 1);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_init(&bus, &ram, UINT32_C(0xfffffff8)));
    check_control(jal_word(8, 1), YAN_OK, 0, 1);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_write(&ram, 4, 4, UINT32_C(0x004002ef)));
    cpu.pc = UINT32_C(0xfffffffc);
    YanCpu before = cpu;
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_step(&cpu, &bus));
    TEST_ASSERT_EQUAL_HEX32(0, cpu.pc);
    before.regs[5] = 0;
    TEST_ASSERT_EQUAL_HEX32_ARRAY(before.regs, cpu.regs, 32);
}

static uint32_t jalr_word(int32_t immediate, uint32_t rs1, uint32_t rd)
{
    return (((uint32_t)immediate & UINT32_C(0xfff)) << 20) | (rs1 << 15) |
           (rd << 7) | UINT32_C(0x67);
}

static void jalr_fixed_vectors(void)
{
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, 6, UINT32_C(0x80000100)));
    check_control(0x004302e7, YAN_OK, UINT32_C(0x80000104), 5);
    check_control(0xffc302e7, YAN_OK, UINT32_C(0x800000fc), 5);
    check_control(0x001302e7, YAN_OK, UINT32_C(0x80000100), 5);
    check_control(0x003302e7, YAN_UNALIGNED, 0, 5);
    check_control(0x800302e7, YAN_OK, UINT32_C(0x7ffff900), 5);
    check_control(0x00000067, YAN_OK, 0, 0);
    check_control(0x00200067, YAN_UNALIGNED, 0, 0);
    check_control(0x000310e7, YAN_UNSUPPORTED_INSTRUCTION, 0, 1);
}

static void jalr_all_immediates_and_low_bits(void)
{
    for (int32_t immediate = -2048; immediate <= 2047; ++immediate) {
        for (uint32_t low = 0; low < 4; ++low) {
            const uint32_t source = UINT32_C(0x80000100) + low;
            const uint32_t sum = (uint32_t)((int64_t)source + immediate);
            const uint32_t target = sum - sum % 2;
            TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, 6, source));
            check_control(jalr_word(immediate, 6, 5), target % 4 == 0 ? YAN_OK : YAN_UNALIGNED,
                          target, 5);
        }
    }
}

static void jalr_register_aliases(void)
{
    for (uint32_t rs1 = 0; rs1 < 32; ++rs1) {
        for (uint32_t rd = 0; rd < 32; ++rd) {
            const uint32_t source = rs1 == 0 ? 0 : UINT32_C(0x80000101);
            TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, rs1, source));
            check_control(jalr_word(0, rs1, rd), YAN_OK, source - source % 2, rd);
            TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, rs1, source));
            check_control(jalr_word(2, rs1, rd), YAN_UNALIGNED, 0, rd);
        }
    }
}

static void jalr_wrap_and_deferred_fetch(void)
{
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, 6, UINT32_MAX));
    check_control(jalr_word(1, 6, 5), YAN_OK, 0, 5);
    YanCpu before = cpu;
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED, yan_cpu_step(&cpu, &bus));
    TEST_ASSERT_EQUAL_HEX32(before.pc, cpu.pc);
    TEST_ASSERT_EQUAL_HEX32_ARRAY(before.regs, cpu.regs, 32);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, 6, 0));
    check_control(jalr_word(-3, 6, 5), YAN_OK, UINT32_C(0xfffffffc), 5);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_init(&bus, &ram, UINT32_C(0xfffffff8)));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, 5, 1));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_write(&ram, 4, 4, UINT32_C(0x000282e7)));
    cpu.pc = UINT32_C(0xfffffffc);
    before = cpu;
    before.regs[5] = 0;
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_step(&cpu, &bus));
    TEST_ASSERT_EQUAL_HEX32(0, cpu.pc);
    TEST_ASSERT_EQUAL_HEX32_ARRAY(before.regs, cpu.regs, 32);
}

static void jalr_invalid_encodings(void)
{
    for (uint32_t funct3 = 1; funct3 < 8; ++funct3) {
        check_control(jalr_word(2, 0, 1) | (funct3 << 12), YAN_UNSUPPORTED_INSTRUCTION, 0, 1);
        check_control(jalr_word(0, 0, 0) | (funct3 << 12), YAN_UNSUPPORTED_INSTRUCTION, 0, 0);
    }
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
    RUN_TEST(jal_fixed_vectors);
    RUN_TEST(jal_offset_bits_and_boundaries);
    RUN_TEST(jal_destinations_and_deferred_fetch);
    RUN_TEST(jal_address_and_link_wrap);
    RUN_TEST(jalr_fixed_vectors);
    RUN_TEST(jalr_all_immediates_and_low_bits);
    RUN_TEST(jalr_register_aliases);
    RUN_TEST(jalr_wrap_and_deferred_fetch);
    RUN_TEST(jalr_invalid_encodings);
    return UNITY_END();
}
