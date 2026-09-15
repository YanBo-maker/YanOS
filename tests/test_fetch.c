#include <string.h>

#include "yan/cpu.h"
#include "unity.h"

static YanRam ram;

void setUp(void)
{
    ram = (YanRam){0};
}

void tearDown(void)
{
    yan_ram_destroy(&ram);
}

static void fetch_preserves_state(void)
{
    YanBus bus = {0};
    YanCpu cpu = {0};
    const uint32_t base = UINT32_C(0x80000000);
    const uint8_t image[] = {0x93, 0x02, 0x70, 0x00};
    uint32_t instruction = 0;
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_init(&ram, 8));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_init(&bus, &ram, base));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_reset(&cpu, base));
    memcpy(ram.data, image, sizeof image);
    for (uint32_t index = 1; index < YAN_REGISTER_COUNT; ++index) {
        TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, index, index * 7));
    }
    YanCpu before = cpu;
    YanBusResult result = yan_bus_fetch32(&bus, base, &instruction);
    TEST_ASSERT_TRUE(result.status == YAN_OK && instruction == UINT32_C(0x00700293));
    TEST_ASSERT_TRUE(result.access == YAN_ACCESS_FETCH && result.width == 4 && result.address == base);
    for (int repeat = 0; repeat < 2; ++repeat) {
        result = yan_cpu_fetch(&cpu, &bus, &instruction);
        TEST_ASSERT_TRUE(result.status == YAN_OK && instruction == UINT32_C(0x00700293));
        TEST_ASSERT_TRUE(result.access == YAN_ACCESS_FETCH && result.width == 4 && result.address == base);
        TEST_ASSERT_TRUE(cpu.pc == before.pc && memcmp(cpu.regs, before.regs, sizeof cpu.regs) == 0);
        TEST_ASSERT_TRUE(memcmp(ram.data, image, sizeof image) == 0);
    }
    /* Changing RAM is visible on the next fetch; there is no instruction cache. */
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_write(&ram, 0, 4, UINT32_MAX));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_fetch(&cpu, &bus, &instruction).status);
    TEST_ASSERT_TRUE(instruction == UINT32_MAX && cpu.pc == base);
    cpu.pc = base + 4;
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_fetch(&cpu, &bus, &instruction).status);
    TEST_ASSERT_TRUE(instruction == 0 && cpu.pc == base + 4);
    TEST_ASSERT_TRUE(memcmp(cpu.regs, before.regs, sizeof cpu.regs) == 0);
    yan_ram_destroy(&ram);
}

static void fetch_errors_preserve_state(void)
{
    YanBus bus = {0};
    YanBus empty = {0};
    YanCpu cpu = {0};
    uint32_t instruction = UINT32_C(0xdeadbeef);
    uint8_t memory_before[7];
    const struct {
        uint32_t address;
        YanStatus status;
    } cases[] = {
        {UINT32_C(0x80000001), YAN_UNALIGNED},
        {UINT32_C(0x80000002), YAN_UNALIGNED},
        {UINT32_C(0x80000003), YAN_UNALIGNED},
        {UINT32_C(0x80000004), YAN_OUT_OF_BOUNDS},
        {UINT32_C(0x80000008), YAN_UNMAPPED},
        {UINT32_C(0x7ffffffc), YAN_UNMAPPED},
        {UINT32_C(0x10000000), YAN_UNMAPPED}
    };
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_init(&ram, sizeof memory_before));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_init(&bus, &ram, UINT32_C(0x80000000)));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_reset(&cpu, UINT32_C(0x10000000)));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&cpu, 5, 123));
    memset(ram.data, 0x7e, ram.size);
    memcpy(memory_before, ram.data, sizeof memory_before);
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        cpu.pc = cases[i].address;
        YanCpu before = cpu;
        YanBusResult result = yan_cpu_fetch(&cpu, &bus, &instruction);
        TEST_ASSERT_TRUE(result.status == cases[i].status && result.address == cpu.pc);
        TEST_ASSERT_TRUE(result.width == 4);
        TEST_ASSERT_EQUAL_INT(YAN_ACCESS_FETCH, result.access);
        TEST_ASSERT_TRUE(cpu.pc == before.pc && memcmp(cpu.regs, before.regs, sizeof cpu.regs) == 0);
        TEST_ASSERT_TRUE(memcmp(ram.data, memory_before, sizeof memory_before) == 0);
        TEST_ASSERT_TRUE(instruction == UINT32_C(0xdeadbeef));
    }
    YanBusResult result = yan_cpu_fetch(NULL, &bus, &instruction);
    TEST_ASSERT_TRUE(result.status == YAN_INVALID_ARGUMENT && result.address == 0);
    TEST_ASSERT_TRUE(result.width == 4);
    TEST_ASSERT_EQUAL_INT(YAN_ACCESS_FETCH, result.access);
    result = yan_cpu_fetch(&cpu, NULL, &instruction);
    TEST_ASSERT_TRUE(result.status == YAN_INVALID_ARGUMENT && result.address == cpu.pc);
    TEST_ASSERT_TRUE(result.width == 4);
    TEST_ASSERT_EQUAL_INT(YAN_ACCESS_FETCH, result.access);
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT, yan_cpu_fetch(&cpu, &bus, NULL).status);
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_STATE, yan_cpu_fetch(&cpu, &empty, &instruction).status);
    TEST_ASSERT_TRUE(instruction == UINT32_C(0xdeadbeef));
    result = yan_bus_fetch32(&bus, cpu.pc, NULL);
    TEST_ASSERT_TRUE(result.status == YAN_INVALID_ARGUMENT);
    TEST_ASSERT_EQUAL_INT(YAN_ACCESS_FETCH, result.access);
    yan_ram_destroy(&ram);
}

static void fetch_at_address_space_top(void)
{
    YanBus bus = {0};
    YanCpu cpu = {0};
    uint32_t instruction = 0;
    const uint32_t entry = UINT32_C(0xfffffffc);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_init(&ram, 4));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_init(&bus, &ram, entry));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_write(&ram, 0, 4, UINT32_C(0x01020304)));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_reset(&cpu, entry));
    YanBusResult result = yan_cpu_fetch(&cpu, &bus, &instruction);
    TEST_ASSERT_TRUE(result.status == YAN_OK && result.address == entry);
    TEST_ASSERT_TRUE(instruction == UINT32_C(0x01020304) && cpu.pc == entry);
    cpu.pc = UINT32_MAX;
    TEST_ASSERT_EQUAL_INT(YAN_UNALIGNED, yan_cpu_fetch(&cpu, &bus, &instruction).status);
    TEST_ASSERT_TRUE(cpu.pc == UINT32_MAX && instruction == UINT32_C(0x01020304));
    yan_ram_destroy(&ram);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(fetch_preserves_state);
    RUN_TEST(fetch_errors_preserve_state);
    RUN_TEST(fetch_at_address_space_top);
    return UNITY_END();
}
