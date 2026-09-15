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
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_reset(&cpu, bus.ram_base));
}

void tearDown(void)
{
    yan_ram_destroy(&ram);
}

static void check_result(uint32_t word, uint32_t rd, uint32_t value)
{
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_write(&ram, 0, 4, word));
    cpu.pc = bus.ram_base;
    YanCpu expected = cpu;
    if (rd != 0) {
        expected.regs[rd] = value;
    }
    uint8_t memory[8];
    memcpy(memory, ram.data, sizeof memory);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_step(&cpu, &bus));
    TEST_ASSERT_EQUAL_HEX32_ARRAY(expected.regs, cpu.regs, 32);
    TEST_ASSERT_EQUAL_HEX32(bus.ram_base + UINT32_C(4), cpu.pc);
    TEST_ASSERT_EQUAL_MEMORY(memory, ram.data, sizeof memory);
}

static void check_rejected(uint32_t word)
{
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_write(&ram, 0, 4, word));
    cpu.pc = bus.ram_base;
    YanCpu before = cpu;
    uint8_t memory[8];
    memcpy(memory, ram.data, sizeof memory);
    TEST_ASSERT_EQUAL_INT(YAN_UNSUPPORTED_INSTRUCTION, yan_cpu_step(&cpu, &bus));
    TEST_ASSERT_EQUAL_HEX32(before.pc, cpu.pc);
    TEST_ASSERT_EQUAL_HEX32_ARRAY(before.regs, cpu.regs, 32);
    TEST_ASSERT_EQUAL_MEMORY(memory, ram.data, sizeof memory);
}

/* Mathematical two's-complement interpretation, without an out-of-range cast. */
static int64_t signed_value(uint32_t value)
{
    return value <= INT32_MAX ? (int64_t)value : (int64_t)value - INT64_C(4294967296);
}

static uint32_t arithmetic_shift_oracle(uint32_t value, uint32_t amount)
{
    const int64_t divisor = INT64_C(1) << amount;
    const int64_t number = signed_value(value);
    return (uint32_t)(number >= 0 ? number / divisor : -((-number + divisor - 1) / divisor));
}

static void immediate_fixed_vectors(void)
{
    const struct { uint32_t word, source, result; } cases[] = {
        {0x00032293, 0xffffffff, 1}, /* SLTI x5,x6,0 */
        {0xfff32293, 0xffffffff, 0},
        {0x7ff32293, 0x80000000, 1},
        {0x80032293, 0x7fffffff, 0},
        {0xfff33293, 0x80000000, 1}, /* SLTIU sign-extends before comparison. */
        {0xfff33293, 0xffffffff, 0},
        {0x80033293, 0xfffff800, 0},
        {0xfff34293, 0xaaaaaaaa, 0x55555555},
        {0x80034293, 0xffffffff, 0x000007ff},
        {0x80036293, 0x00000555, 0xfffffd55},
        {0x7ff37293, 0xabcdef12, 0x00000712},
        {0x80037293, 0xffffffff, 0xfffff800},
        {0x01f31293, 1, 0x80000000},
        {0x01f35293, 0x80000000, 1},
        {0x41f35293, 0x80000000, 0xffffffff},
        {0x40035293, 0x80000001, 0x80000001}
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, 6, cases[i].source));
        check_result(cases[i].word, 5, cases[i].result);
    }
}

static void immediate_sign_extension(void)
{
    const uint32_t sources[] = {0, 0x7fffffff, 0x80000000, 0xffffffff, 0xaaaaaaaa};
    for (int32_t immediate = -2048; immediate <= 2047; ++immediate) {
        const uint32_t encoded = ((uint32_t)immediate & UINT32_C(0xfff)) << 20;
        const uint32_t operand = (uint32_t)immediate;
        for (size_t i = 0; i < sizeof sources / sizeof sources[0]; ++i) {
            const uint32_t source = sources[i];
            TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, 6, source));
            check_result(encoded | UINT32_C(0x00032293), 5, signed_value(source) < immediate);
            check_result(encoded | UINT32_C(0x00033293), 5, source < operand);
            check_result(encoded | UINT32_C(0x00034293), 5, source ^ operand);
            check_result(encoded | UINT32_C(0x00036293), 5, source | operand);
            check_result(encoded | UINT32_C(0x00037293), 5, source & operand);
        }
    }
}

static void immediate_shift_amounts(void)
{
    const uint32_t sources[] = {0, 1, 0x7fffffff, 0x80000000, 0x80000001, 0xffffffff};
    for (uint32_t amount = 0; amount < 32; ++amount) {
        for (size_t i = 0; i < sizeof sources / sizeof sources[0]; ++i) {
            const uint32_t source = sources[i];
            TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, 6, source));
            check_result(UINT32_C(0x00031293) | (amount << 20), 5,
                         (uint32_t)((uint64_t)source * (UINT64_C(1) << amount)));
            check_result(UINT32_C(0x00035293) | (amount << 20), 5,
                         (uint32_t)((uint64_t)source / (UINT64_C(1) << amount)));
            check_result(UINT32_C(0x40035293) | (amount << 20), 5,
                         arithmetic_shift_oracle(source, amount));
        }
    }
}

static void immediate_register_aliases(void)
{
    const uint32_t operations[] = {0x00101013, 0x00102013, 0x00103013, 0x00104013,
                                  0x00105013, 0x40105013, 0x00106013, 0x00107013};
    const uint32_t nonzero[] = {10, 0, 0, 4, 2, 2, 5, 1};
    const uint32_t zero[] = {0, 1, 1, 1, 0, 0, 1, 0};
    for (size_t op = 0; op < sizeof operations / sizeof operations[0]; ++op) {
        for (uint32_t rs1 = 0; rs1 < 32; ++rs1) {
            for (uint32_t rd = 0; rd < 32; ++rd) {
                TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, rs1, 5));
                check_result(operations[op] | (rs1 << 15) | (rd << 7), rd,
                             rs1 == 0 ? zero[op] : nonzero[op]);
            }
        }
    }
}

static void invalid_immediate_shift_encodings(void)
{
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, 6, UINT32_MAX));
    for (uint32_t upper = 0; upper < 128; ++upper) {
        if (upper != 0) {
            check_rejected((upper << 25) | UINT32_C(0x01f31293));
            check_rejected((upper << 25) | UINT32_C(0x01f31013)); /* rd=x0 */
        }
        if (upper != 0 && upper != 0x20) {
            check_rejected((upper << 25) | UINT32_C(0x01f35293));
        }
    }
}

static void register_fixed_vectors(void)
{
    const struct { uint32_t word, left, right, result; } cases[] = {
        {0x007302b3, 0xffffffff, 1, 0},
        {0x007302b3, 0x7fffffff, 1, 0x80000000},
        {0x407302b3, 0, 1, 0xffffffff},
        {0x407302b3, 0x80000000, 1, 0x7fffffff},
        {0x007322b3, 0x80000000, 0x7fffffff, 1},
        {0x007322b3, 0xffffffff, 0xffffffff, 0},
        {0x007332b3, 0x80000000, 0x7fffffff, 0},
        {0x007342b3, 0xaaaaaaaa, 0x55555555, 0xffffffff},
        {0x007362b3, 0x12345678, 0xffff0000, 0xffff5678},
        {0x007372b3, 0x12345678, 0xffff0000, 0x12340000},
        {0x007312b3, 1, 0xffffffff, 0x80000000},
        {0x007352b3, 0x80000000, 0xffffffff, 1},
        {0x407352b3, 0x80000000, 0xffffffff, 0xffffffff},
        {0x407352b3, 0x80000001, 32, 0x80000001}
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, 6, cases[i].left));
        TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, 7, cases[i].right));
        check_result(cases[i].word, 5, cases[i].result);
    }
}

static void register_boundary_matrix(void)
{
    const uint32_t operands[] = {0, 1, 31, 32, 33, 0x7fffffff, 0x80000000,
                                 0x80000001, 0xffffffff, 0xaaaaaaaa, 0x55555555};
    for (size_t i = 0; i < sizeof operands / sizeof operands[0]; ++i) {
        for (size_t j = 0; j < sizeof operands / sizeof operands[0]; ++j) {
            const uint32_t left = operands[i], right = operands[j];
            TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, 6, left));
            TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, 7, right));
            check_result(0x007302b3, 5, (uint32_t)((uint64_t)left + right));
            check_result(0x407302b3, 5, (uint32_t)((int64_t)left - right));
            check_result(0x007322b3, 5, signed_value(left) < signed_value(right));
            check_result(0x007332b3, 5, left < right);
            check_result(0x007342b3, 5, left ^ right);
            check_result(0x007362b3, 5, left | right);
            check_result(0x007372b3, 5, left & right);
        }
    }
}

static void register_shift_masking(void)
{
    const uint32_t sources[] = {0, 1, 0x7fffffff, 0x80000000, 0x80000001, 0xffffffff};
    const uint32_t upper[] = {0, 32, 0x80000000, 0xffffffe0};
    for (uint32_t amount = 0; amount < 32; ++amount) {
        for (size_t hi = 0; hi < sizeof upper / sizeof upper[0]; ++hi) {
            for (size_t i = 0; i < sizeof sources / sizeof sources[0]; ++i) {
                const uint32_t source = sources[i];
                TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, 6, source));
                TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, 7, upper[hi] + amount));
                check_result(0x007312b3, 5, (uint32_t)((uint64_t)source * (UINT64_C(1) << amount)));
                check_result(0x007352b3, 5, (uint32_t)((uint64_t)source / (UINT64_C(1) << amount)));
                check_result(0x407352b3, 5, arithmetic_shift_oracle(source, amount));
            }
        }
    }
}

static void register_aliases_and_x0(void)
{
    const uint32_t operations[] = {0x33, 0x40000033, 0x1033, 0x2033, 0x3033,
                                  0x4033, 0x5033, 0x40005033, 0x6033, 0x7033};
    /* All three encoded register fields, including every overlap and x0. */
    for (uint32_t rs1 = 0; rs1 < 32; ++rs1) {
        for (uint32_t rs2 = 0; rs2 < 32; ++rs2) {
            for (uint32_t rd = 0; rd < 32; ++rd) {
                for (size_t op = 0; op < sizeof operations / sizeof operations[0]; ++op) {
                    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, rs1, rs1));
                    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, rs2, rs2));
                    const uint32_t expected[] = {rs1 + rs2, (uint32_t)((int64_t)rs1 - rs2),
                        (uint32_t)((uint64_t)rs1 * (UINT64_C(1) << rs2)), rs1 < rs2, rs1 < rs2,
                        rs1 ^ rs2, rs1 >> rs2, rs1 >> rs2, rs1 | rs2, rs1 & rs2};
                    check_result(operations[op] | (rs1 << 15) | (rs2 << 20) | (rd << 7), rd, expected[op]);
                }
            }
        }
    }
}

static void invalid_register_encodings(void)
{
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, 6, UINT32_MAX));
    for (uint32_t funct7 = 0; funct7 < 128; ++funct7) {
        for (uint32_t funct3 = 0; funct3 < 8; ++funct3) {
            if (funct7 == 0 || (funct7 == 0x20 && (funct3 == 0 || funct3 == 5))) {
                continue;
            }
            check_rejected((funct7 << 25) | (funct3 << 12) | UINT32_C(0x007302b3));
            check_rejected((funct7 << 25) | (funct3 << 12) | UINT32_C(0x00730033));
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(immediate_fixed_vectors);
    RUN_TEST(immediate_sign_extension);
    RUN_TEST(immediate_shift_amounts);
    RUN_TEST(immediate_register_aliases);
    RUN_TEST(invalid_immediate_shift_encodings);
    RUN_TEST(register_fixed_vectors);
    RUN_TEST(register_boundary_matrix);
    RUN_TEST(register_shift_masking);
    RUN_TEST(register_aliases_and_x0);
    RUN_TEST(invalid_register_encodings);
    return UNITY_END();
}
