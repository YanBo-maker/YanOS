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
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_init(&ram, 64));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_init(&bus, &ram, UINT32_C(0x80000000)));
    memset(ram.data, 0xa5, ram.size);
    for (uint32_t index = 1; index < 32; ++index) {
        TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, index, index * 17));
    }
}

void tearDown(void)
{
    yan_ram_destroy(&ram);
}

static uint32_t load_word(int32_t offset, uint32_t funct3, uint32_t rs1, uint32_t rd)
{
    return (((uint32_t)offset & UINT32_C(0xfff)) << 20) | (rs1 << 15) |
           (funct3 << 12) | (rd << 7) | UINT32_C(3);
}

static void check_load(uint32_t word, YanStatus status, uint32_t rd, uint32_t value)
{
    cpu.pc = bus.ram_base;
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_write(&ram, 0, 4, word));
    YanCpu expected = cpu;
    if (status == YAN_OK && rd != 0) {
        expected.regs[rd] = value;
    }
    uint8_t memory[64];
    memcpy(memory, ram.data, ram.size);
    TEST_ASSERT_EQUAL_INT(status, yan_cpu_step(&cpu, &bus));
    TEST_ASSERT_EQUAL_HEX32(status == YAN_OK ? bus.ram_base + UINT32_C(4) : expected.pc, cpu.pc);
    TEST_ASSERT_EQUAL_HEX32_ARRAY(expected.regs, cpu.regs, 32);
    TEST_ASSERT_EQUAL_MEMORY(memory, ram.data, ram.size);
}

static void load_fixed_vectors(void)
{
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, 6, bus.ram_base + 32));
    const uint8_t bytes[] = {0x80, 0xff, 0x7f, 0x80};
    memcpy(ram.data + 32, bytes, sizeof bytes);
    check_load(0x00030283, YAN_OK, 5, 0xffffff80);
    check_load(0x00034283, YAN_OK, 5, 0x80);
    check_load(0x00130283, YAN_OK, 5, 0xffffffff);
    check_load(0x00230283, YAN_OK, 5, 0x7f);
    check_load(0x00031283, YAN_OK, 5, 0xffffff80);
    check_load(0x00231283, YAN_OK, 5, 0xffff807f);
    check_load(0x00235283, YAN_OK, 5, 0x807f);
    check_load(0x00032283, YAN_OK, 5, 0x807fff80);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, 6, bus.ram_base + 36));
    check_load(0xffc32283, YAN_OK, 5, 0x807fff80);
}

static void load_sign_boundaries(void)
{
    const uint32_t values[] = {0, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff, 0x80000000, 0xffffffff};
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, 6, bus.ram_base + 32));
    for (size_t i = 0; i < sizeof values / sizeof values[0]; ++i) {
        const uint32_t byte = values[i] % 256, half = values[i] % 65536;
        TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_write(&ram, 32, 4, values[i]));
        check_load(load_word(0, 0, 6, 5), YAN_OK, 5, (uint32_t)((int64_t)byte - (byte >= 128 ? 256 : 0)));
        check_load(load_word(0, 4, 6, 5), YAN_OK, 5, byte);
        check_load(load_word(0, 1, 6, 5), YAN_OK, 5, (uint32_t)((int64_t)half - (half >= 32768 ? 65536 : 0)));
        check_load(load_word(0, 5, 6, 5), YAN_OK, 5, half);
        check_load(load_word(0, 2, 6, 5), YAN_OK, 5, values[i]);
    }
}

static void load_all_offsets(void)
{
    const uint32_t kinds[] = {0, 1, 2, 4, 5};
    const uint32_t expected[] = {0xffffff80, 0xffffff80, 0x807fff80, 0x80, 0xff80};
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_write(&ram, 32, 4, UINT32_C(0x807fff80)));
    for (int32_t offset = -2048; offset <= 2047; ++offset) {
        TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, 6,
            (uint32_t)((int64_t)bus.ram_base + 32 - offset)));
        for (size_t kind = 0; kind < 5; ++kind) {
            check_load(load_word(offset, kinds[kind], 6, 5), YAN_OK, 5, expected[kind]);
        }
    }
}

static void load_register_aliases_and_x0(void)
{
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_init(&bus, &ram, 0));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_write(&ram, 32, 4, UINT32_C(0x807fff80)));
    const uint32_t kinds[] = {0, 1, 2, 4, 5};
    const uint32_t expected[] = {0xffffff80, 0xffffff80, 0x807fff80, 0x80, 0xff80};
    for (uint32_t rs1 = 0; rs1 < 32; ++rs1) {
        for (uint32_t rd = 0; rd < 32; ++rd) {
            for (size_t kind = 0; kind < 5; ++kind) {
                TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, rs1, 32));
                check_load(load_word(rs1 == 0 ? 32 : 0, kinds[kind], rs1, rd), YAN_OK, rd, expected[kind]);
            }
        }
    }
}

static void load_errors_and_boundaries(void)
{
    const uint32_t kinds[] = {0, 1, 2, 4, 5};
    for (size_t kind = 0; kind < 5; ++kind) {
        TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, 6, bus.ram_base + 64));
        check_load(load_word(0, kinds[kind], 6, 5), YAN_UNMAPPED, 5, 0);
        check_load(load_word(0, kinds[kind], 6, 0), YAN_UNMAPPED, 0, 0);
        TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, 6, bus.ram_base - 4));
        check_load(load_word(0, kinds[kind], 6, 5), YAN_UNMAPPED, 5, 0);
    }
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, 6, bus.ram_base));
    check_load(0x00032283, YAN_OK, 5, 0x00032283); /* Data at the first address. */
    for (int32_t low = 1; low < 4; ++low) {
        check_load(load_word(low, 2, 6, 5), YAN_UNALIGNED, 5, 0);
        if (low % 2 != 0) {
            check_load(load_word(low, 1, 6, 0), YAN_UNALIGNED, 0, 0);
            check_load(load_word(low, 5, 6, 5), YAN_UNALIGNED, 5, 0);
        }
    }
    check_load(load_word(63, 4, 6, 5), YAN_OK, 5, 0xa5);
    check_load(load_word(62, 5, 6, 5), YAN_OK, 5, 0xa5a5);
    check_load(load_word(60, 2, 6, 5), YAN_OK, 5, 0xa5a5a5a5);
    yan_ram_destroy(&ram);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_init(&ram, 63));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_init(&bus, &ram, UINT32_C(0x80000000)));
    check_load(load_word(60, 2, 6, 0), YAN_OUT_OF_BOUNDS, 0, 0);
    check_load(load_word(62, 1, 6, 5), YAN_OUT_OF_BOUNDS, 5, 0);
}

static void load_wrap_and_invalid_encoding(void)
{
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_init(&bus, &ram, 0));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, 6, UINT32_MAX));
    check_load(load_word(33, 4, 6, 5), YAN_OK, 5, 0xa5);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_init(&bus, &ram, UINT32_C(0xffffffc0)));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, 6, 0));
    check_load(load_word(-1, 4, 6, 5), YAN_OK, 5, 0xa5);
    check_load(load_word(-4, 2, 6, 5), YAN_OK, 5, 0xa5a5a5a5);
    const uint32_t invalid[] = {3, 6, 7};
    for (size_t i = 0; i < 3; ++i) {
        check_load(load_word(1, invalid[i], 6, 0), YAN_UNSUPPORTED_INSTRUCTION, 0, 0);
    }
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_write(&ram, 60, 4, load_word(-32, 4, 0, 5)));
    cpu.pc = UINT32_C(0xfffffffc);
    YanCpu expected = cpu;
    expected.regs[5] = 0xa5;
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_step(&cpu, &bus));
    TEST_ASSERT_EQUAL_HEX32(0, cpu.pc);
    TEST_ASSERT_EQUAL_HEX32_ARRAY(expected.regs, cpu.regs, 32);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(load_fixed_vectors);
    RUN_TEST(load_sign_boundaries);
    RUN_TEST(load_all_offsets);
    RUN_TEST(load_register_aliases_and_x0);
    RUN_TEST(load_errors_and_boundaries);
    RUN_TEST(load_wrap_and_invalid_encoding);
    return UNITY_END();
}
