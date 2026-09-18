#include <stdint.h>

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

void tearDown(void) { yan_ram_destroy(&ram); }

static uint32_t encode_m(uint32_t funct3, uint32_t rd, uint32_t rs1, uint32_t rs2)
{
    return (UINT32_C(1) << 25) | (rs2 << 20) | (rs1 << 15) |
           (funct3 << 12) | (rd << 7) | UINT32_C(0x33);
}

static uint32_t oracle(uint32_t funct3, uint32_t left, uint32_t right)
{
    const int64_t a = left <= INT32_MAX ? (int64_t)left : (int64_t)left - INT64_C(4294967296);
    const int64_t b = right <= INT32_MAX ? (int64_t)right : (int64_t)right - INT64_C(4294967296);
    switch (funct3) {
    case 0: return (uint32_t)((uint64_t)left * right);
    case 1: return (uint32_t)(((uint64_t)(a * b)) >> 32);
    case 2: return (uint32_t)(((uint64_t)(a * (int64_t)(uint64_t)right)) >> 32);
    case 3: return (uint32_t)(((uint64_t)left * right) >> 32);
    case 4:
        if (right == 0) return UINT32_MAX;
        if (left == UINT32_C(0x80000000) && right == UINT32_MAX) return left;
        return (uint32_t)(a / b);
    case 5: return right == 0 ? UINT32_MAX : left / right;
    case 6:
        if (right == 0) return left;
        if (left == UINT32_C(0x80000000) && right == UINT32_MAX) return 0;
        return (uint32_t)(a % b);
    default: return right == 0 ? left : left % right;
    }
}

static void run_one(uint32_t funct3, uint32_t rd, uint32_t rs1, uint32_t rs2,
                    uint32_t left, uint32_t right)
{
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, rs1, left));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, rs2, right));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_write(&ram, 0, 4, encode_m(funct3, rd, rs1, rs2)));
    cpu.pc = bus.ram_base;
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_step(&cpu, &bus));
    const uint32_t actual_left = rs1 == 0 ? 0 : (rs1 == rs2 ? right : left);
    const uint32_t actual_right = rs2 == 0 ? 0 : right;
    const uint32_t expected = rd == 0 ? 0 : oracle(funct3, actual_left, actual_right);
    TEST_ASSERT_EQUAL_HEX32(expected, cpu.regs[rd]);
    TEST_ASSERT_EQUAL_HEX32(bus.ram_base + 4, cpu.pc);
}

static void multiply_vectors(void)
{
    const uint32_t values[] = {0, 1, UINT32_MAX, UINT32_C(0x7fffffff),
        UINT32_C(0x80000000), UINT32_C(0x80000001), UINT32_C(0x12345678), UINT32_C(0xfedcba98)};
    for (uint32_t funct3 = 0; funct3 < 4; ++funct3) {
        for (size_t i = 0; i < sizeof values / sizeof values[0]; ++i) {
            for (size_t j = 0; j < sizeof values / sizeof values[0]; ++j) {
                run_one(funct3, 5, 6, 7, values[i], values[j]);
            }
        }
    }
}

static void division_boundaries(void)
{
    const uint32_t cases[][2] = {{0, 0}, {1, 0}, {UINT32_MAX, 0},
        {UINT32_C(0x80000000), UINT32_MAX}, {UINT32_C(0x80000000), 1},
        {UINT32_C(0x80000000), 2}, {UINT32_MAX, 2},
        {UINT32_C(0x7fffffff), UINT32_C(0xffffffff)}, {UINT32_C(0x12345678), 0x1000}};
    for (uint32_t funct3 = 4; funct3 < 8; ++funct3) {
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
            run_one(funct3, 5, 6, 7, cases[i][0], cases[i][1]);
        }
    }
}

static void register_fields_and_x0(void)
{
    const uint32_t left = UINT32_C(0x81234567);
    const uint32_t right = UINT32_C(0x10203040);
    for (uint32_t funct3 = 0; funct3 < 8; ++funct3) {
        for (uint32_t rd = 0; rd < 32; ++rd) {
            for (uint32_t rs1 = 0; rs1 < 32; ++rs1) {
                for (uint32_t rs2 = 0; rs2 < 32; ++rs2) {
                    run_one(funct3, rd, rs1, rs2, left, right);
                    if (rd == 0) {
                        TEST_ASSERT_EQUAL_HEX32(0, cpu.regs[0]);
                    } else {
                        const uint32_t expected_left = rs1 == 0 ? 0 : (rs1 == rs2 ? right : left);
                        const uint32_t expected_right = rs2 == 0 ? 0 : right;
                        TEST_ASSERT_EQUAL_HEX32(oracle(funct3, expected_left, expected_right), cpu.regs[rd]);
                    }
                }
            }
        }
    }
}

static void invalid_m_encoding(void)
{
    const uint32_t word = encode_m(0, 5, 6, 7) | (UINT32_C(2) << 25);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_write(&ram, 0, 4, word));
    cpu.pc = bus.ram_base;
    TEST_ASSERT_EQUAL_INT(YAN_TRAP, yan_cpu_step(&cpu, &bus));
    TEST_ASSERT_EQUAL_HEX32(2, cpu.csr.mcause);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(multiply_vectors);
    RUN_TEST(division_boundaries);
    RUN_TEST(register_fields_and_x0);
    RUN_TEST(invalid_m_encoding);
    return UNITY_END();
}
