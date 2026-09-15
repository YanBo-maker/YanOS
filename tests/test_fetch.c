#include <string.h>

#include "yan/cpu.h"
#include "test.h"

static int fetch_preserves_state(void)
{
    YanRam ram = {0};
    YanBus bus = {0};
    YanCpu cpu = {0};
    const uint32_t base = UINT32_C(0x80000000);
    const uint8_t image[] = {0x93, 0x02, 0x70, 0x00};
    uint32_t instruction = 0;
    CHECK(yan_ram_init(&ram, 8) == YAN_OK);
    CHECK(yan_bus_init(&bus, &ram, base) == YAN_OK);
    CHECK(yan_cpu_reset(&cpu, base) == YAN_OK);
    memcpy(ram.data, image, sizeof image);
    for (uint32_t index = 1; index < YAN_REGISTER_COUNT; ++index) {
        CHECK(yan_cpu_write_reg(&cpu, index, index * 7) == YAN_OK);
    }
    YanCpu before = cpu;
    YanBusResult result = yan_bus_fetch32(&bus, base, &instruction);
    CHECK(result.status == YAN_OK && instruction == UINT32_C(0x00700293));
    CHECK(result.access == YAN_ACCESS_FETCH && result.width == 4 && result.address == base);
    for (int repeat = 0; repeat < 2; ++repeat) {
        result = yan_cpu_fetch(&cpu, &bus, &instruction);
        CHECK(result.status == YAN_OK && instruction == UINT32_C(0x00700293));
        CHECK(result.access == YAN_ACCESS_FETCH && result.width == 4 && result.address == base);
        CHECK(cpu.pc == before.pc && memcmp(cpu.regs, before.regs, sizeof cpu.regs) == 0);
        CHECK(memcmp(ram.data, image, sizeof image) == 0);
    }
    /* Changing RAM is visible on the next fetch; there is no instruction cache. */
    CHECK(yan_ram_write(&ram, 0, 4, UINT32_MAX) == YAN_OK);
    CHECK(yan_cpu_fetch(&cpu, &bus, &instruction).status == YAN_OK);
    CHECK(instruction == UINT32_MAX && cpu.pc == base);
    cpu.pc = base + 4;
    CHECK(yan_cpu_fetch(&cpu, &bus, &instruction).status == YAN_OK);
    CHECK(instruction == 0 && cpu.pc == base + 4);
    CHECK(memcmp(cpu.regs, before.regs, sizeof cpu.regs) == 0);
    yan_ram_destroy(&ram);
    return EXIT_SUCCESS;
}

static int fetch_errors_preserve_state(void)
{
    YanRam ram = {0};
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
    CHECK(yan_ram_init(&ram, sizeof memory_before) == YAN_OK);
    CHECK(yan_bus_init(&bus, &ram, UINT32_C(0x80000000)) == YAN_OK);
    CHECK(yan_cpu_reset(&cpu, UINT32_C(0x10000000)) == YAN_OK);
    CHECK(yan_cpu_write_reg(&cpu, 5, 123) == YAN_OK);
    memset(ram.data, 0x7e, ram.size);
    memcpy(memory_before, ram.data, sizeof memory_before);
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        cpu.pc = cases[i].address;
        YanCpu before = cpu;
        YanBusResult result = yan_cpu_fetch(&cpu, &bus, &instruction);
        CHECK(result.status == cases[i].status && result.address == cpu.pc);
        CHECK(result.width == 4 && result.access == YAN_ACCESS_FETCH);
        CHECK(cpu.pc == before.pc && memcmp(cpu.regs, before.regs, sizeof cpu.regs) == 0);
        CHECK(memcmp(ram.data, memory_before, sizeof memory_before) == 0);
        CHECK(instruction == UINT32_C(0xdeadbeef));
    }
    YanBusResult result = yan_cpu_fetch(NULL, &bus, &instruction);
    CHECK(result.status == YAN_INVALID_ARGUMENT && result.address == 0);
    CHECK(result.width == 4 && result.access == YAN_ACCESS_FETCH);
    result = yan_cpu_fetch(&cpu, NULL, &instruction);
    CHECK(result.status == YAN_INVALID_ARGUMENT && result.address == cpu.pc);
    CHECK(result.width == 4 && result.access == YAN_ACCESS_FETCH);
    CHECK(yan_cpu_fetch(&cpu, &bus, NULL).status == YAN_INVALID_ARGUMENT);
    CHECK(yan_cpu_fetch(&cpu, &empty, &instruction).status == YAN_INVALID_STATE);
    CHECK(instruction == UINT32_C(0xdeadbeef));
    result = yan_bus_fetch32(&bus, cpu.pc, NULL);
    CHECK(result.status == YAN_INVALID_ARGUMENT && result.access == YAN_ACCESS_FETCH);
    yan_ram_destroy(&ram);
    return EXIT_SUCCESS;
}

static int fetch_at_address_space_top(void)
{
    YanRam ram = {0};
    YanBus bus = {0};
    YanCpu cpu = {0};
    uint32_t instruction = 0;
    const uint32_t entry = UINT32_C(0xfffffffc);
    CHECK(yan_ram_init(&ram, 4) == YAN_OK);
    CHECK(yan_bus_init(&bus, &ram, entry) == YAN_OK);
    CHECK(yan_ram_write(&ram, 0, 4, UINT32_C(0x01020304)) == YAN_OK);
    CHECK(yan_cpu_reset(&cpu, entry) == YAN_OK);
    YanBusResult result = yan_cpu_fetch(&cpu, &bus, &instruction);
    CHECK(result.status == YAN_OK && result.address == entry);
    CHECK(instruction == UINT32_C(0x01020304) && cpu.pc == entry);
    cpu.pc = UINT32_MAX;
    CHECK(yan_cpu_fetch(&cpu, &bus, &instruction).status == YAN_UNALIGNED);
    CHECK(cpu.pc == UINT32_MAX && instruction == UINT32_C(0x01020304));
    yan_ram_destroy(&ram);
    return EXIT_SUCCESS;
}

int main(void)
{
    RUN_TEST(fetch_preserves_state);
    RUN_TEST(fetch_errors_preserve_state);
    RUN_TEST(fetch_at_address_space_top);
    return EXIT_SUCCESS;
}
