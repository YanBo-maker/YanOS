#include <string.h>

#include "yan/machine.h"
#include "unity.h"

static YanMachine machine;
static YanMachine first;
static YanMachine second;

void setUp(void)
{
    machine = (YanMachine){0};
    first = (YanMachine){0};
    second = (YanMachine){0};
}

void tearDown(void)
{
    yan_machine_destroy(&machine);
    yan_machine_destroy(&first);
    yan_machine_destroy(&second);
}

static void default_platform_and_image(void)
{
    uint32_t value = 0;
    const uint8_t image[] = {0x93, 0x02, 0x70, 0x00, 0xaa};
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_init(&machine));
    TEST_ASSERT_EQUAL_UINT64(YAN_RAM_SIZE, machine.ram.size);
    TEST_ASSERT_TRUE(machine.bus.ram == &machine.ram);
    TEST_ASSERT_EQUAL_HEX32(YAN_RAM_BASE, machine.bus.ram_base);
    TEST_ASSERT_EQUAL_HEX32(YAN_RAM_BASE, machine.cpu.pc);
    for (uint32_t index = 0; index < YAN_REGISTER_COUNT; ++index) {
        TEST_ASSERT_TRUE(yan_cpu_read_reg(&machine.cpu, index, &value) == YAN_OK && value == 0);
    }
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_read(&machine.bus, YAN_RAM_BASE, 4, &value).status);
    TEST_ASSERT_TRUE(value == 0);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_write(&machine.bus, UINT32_C(0x81ffffff), 1, 0x5a).status);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_load_image(&machine, image, sizeof image));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_read(&machine.bus, YAN_RAM_BASE, 4, &value).status);
    TEST_ASSERT_TRUE(value == UINT32_C(0x00700293));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_read(&machine.bus, YAN_RAM_BASE + 4, 1, &value).status);
    TEST_ASSERT_TRUE(value == 0xaa);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_read(&machine.bus, UINT32_C(0x81ffffff), 1, &value).status);
    TEST_ASSERT_TRUE(value == 0x5a);
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED, yan_bus_read(&machine.bus, UINT32_C(0x82000000), 1, &value).status);
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED, yan_bus_read(&machine.bus, UINT32_C(0x10000000), 1, &value).status);
    uint8_t *allocated = machine.ram.data;
    machine.cpu.pc = YAN_RAM_BASE + 4;
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&machine.cpu, 5, 99));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_reset(&machine));
    TEST_ASSERT_TRUE(machine.ram.data == allocated && machine.bus.ram == &machine.ram);
    TEST_ASSERT_EQUAL_HEX32(YAN_RAM_BASE, machine.cpu.pc);
    for (uint32_t index = 0; index < YAN_REGISTER_COUNT; ++index) {
        TEST_ASSERT_TRUE(yan_cpu_read_reg(&machine.cpu, index, &value) == YAN_OK && value == 0);
    }
    for (size_t i = 0; i < machine.ram.size; ++i) {
        TEST_ASSERT_TRUE(machine.ram.data[i] == 0);
    }
    yan_machine_destroy(&machine);
}

static void rejected_and_overlapping_images(void)
{
    const uint8_t image[] = {1, 2, 3, 4, 5, 6};
    uint32_t value = 0;
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_init(&machine));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_load_image(&machine, image, sizeof image));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_write(&machine.bus, UINT32_C(0x81ffffff), 1, 0x77).status);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_load_image(&machine, NULL, 0));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT, yan_machine_load_image(&machine, NULL, 1));
    TEST_ASSERT_EQUAL_INT(YAN_OUT_OF_BOUNDS, yan_machine_load_image(&machine, image, YAN_RAM_SIZE + 1));
    TEST_ASSERT_EQUAL_INT(YAN_OUT_OF_BOUNDS, yan_machine_load_image(&machine, image, SIZE_MAX));
    TEST_ASSERT_TRUE(memcmp(machine.ram.data, image, sizeof image) == 0);
    TEST_ASSERT_TRUE(machine.ram.data[YAN_RAM_SIZE - 1] == 0x77);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_load_image(&machine, machine.ram.data + 2, 4));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_read(&machine.bus, YAN_RAM_BASE, 4, &value).status);
    TEST_ASSERT_TRUE(value == UINT32_C(0x06050403));
    TEST_ASSERT_TRUE(machine.ram.data[4] == 5 && machine.ram.data[5] == 6);
    /* A full-size image is valid, including when source and target coincide. */
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_write(&machine.bus, UINT32_C(0x81ffffff), 1, 0xfe).status);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_load_image(&machine, machine.ram.data, machine.ram.size));
    TEST_ASSERT_TRUE(machine.ram.data[YAN_RAM_SIZE - 1] == 0xfe);
    yan_machine_destroy(&machine);
}

static void lifecycle_and_independent_machines(void)
{
    const uint8_t image[] = {0x42};
    uint32_t value = 0;
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT, yan_machine_init(NULL));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT, yan_machine_reset(NULL));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT, yan_machine_load_image(NULL, NULL, 0));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_STATE, yan_machine_reset(&first));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_STATE, yan_machine_load_image(&first, NULL, 0));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_init(&first));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_load_image(&first, image, sizeof image));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&first.cpu, 31, 73));
    first.cpu.pc = YAN_RAM_BASE + 4;
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_STATE, yan_machine_init(&first));
    TEST_ASSERT_EQUAL_UINT64(YAN_RAM_SIZE, first.ram.size);
    TEST_ASSERT_TRUE(first.cpu.pc == YAN_RAM_BASE + 4);
    TEST_ASSERT_TRUE(yan_cpu_read_reg(&first.cpu, 31, &value) == YAN_OK && value == 73);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_init(&second));
    TEST_ASSERT_TRUE(yan_bus_read(&second.bus, YAN_RAM_BASE, 1, &value).status == YAN_OK && value == 0);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_reset(&second));
    TEST_ASSERT_TRUE(yan_cpu_read_reg(&second.cpu, 31, &value) == YAN_OK && value == 0);
    TEST_ASSERT_TRUE(yan_cpu_read_reg(&first.cpu, 31, &value) == YAN_OK && value == 73);
    TEST_ASSERT_TRUE(yan_bus_read(&first.bus, YAN_RAM_BASE, 1, &value).status == YAN_OK && value == 0x42);
    yan_machine_destroy(&first);
    TEST_ASSERT_TRUE(first.ram.data == NULL && first.ram.size == 0 && first.bus.ram == NULL);
    TEST_ASSERT_TRUE(first.cpu.pc == 0);
    for (uint32_t index = 0; index < YAN_REGISTER_COUNT; ++index) {
        TEST_ASSERT_TRUE(yan_cpu_read_reg(&first.cpu, index, &value) == YAN_OK && value == 0);
    }
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_STATE, yan_bus_read(&first.bus, YAN_RAM_BASE, 1, &value).status);
    yan_machine_destroy(&first);
    yan_machine_destroy(NULL);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_init(&first));
    TEST_ASSERT_EQUAL_HEX32(YAN_RAM_BASE, first.cpu.pc);
    TEST_ASSERT_TRUE(yan_bus_read(&first.bus, YAN_RAM_BASE, 1, &value).status == YAN_OK && value == 0);
    yan_machine_destroy(&first);
    yan_machine_destroy(&second);
}

static void cpu_reset_and_image_loading(void)
{
    const uint8_t image[] = {0x93, 0x02, 0x70, 0x00};
    uint32_t value = 0;
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_init(&machine));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_write_reg(&machine.cpu, 5, 77));
    machine.cpu.pc = YAN_RAM_BASE + 4;
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_load_image(&machine, image, sizeof image));
    TEST_ASSERT_TRUE(machine.cpu.pc == YAN_RAM_BASE + 4);
    TEST_ASSERT_TRUE(yan_cpu_read_reg(&machine.cpu, 5, &value) == YAN_OK && value == 77);
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT, yan_machine_load_image(&machine, NULL, 1));
    TEST_ASSERT_TRUE(machine.cpu.pc == YAN_RAM_BASE + 4);
    TEST_ASSERT_TRUE(yan_cpu_read_reg(&machine.cpu, 5, &value) == YAN_OK && value == 77);
    uint8_t *allocated = machine.ram.data;
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_reset(&machine.cpu, YAN_RAM_BASE));
    TEST_ASSERT_TRUE(machine.ram.data == allocated && machine.bus.ram == &machine.ram);
    TEST_ASSERT_TRUE(yan_cpu_read_reg(&machine.cpu, 5, &value) == YAN_OK && value == 0);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_fetch(&machine.cpu, &machine.bus, &value).status);
    TEST_ASSERT_TRUE(value == UINT32_C(0x00700293));
    TEST_ASSERT_EQUAL_HEX32(YAN_RAM_BASE, machine.cpu.pc);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_machine_reset(&machine));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_cpu_fetch(&machine.cpu, &machine.bus, &value).status);
    TEST_ASSERT_TRUE(value == 0);
    TEST_ASSERT_EQUAL_HEX32(YAN_RAM_BASE, machine.cpu.pc);
    yan_machine_destroy(&machine);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(default_platform_and_image);
    RUN_TEST(rejected_and_overlapping_images);
    RUN_TEST(lifecycle_and_independent_machines);
    RUN_TEST(cpu_reset_and_image_loading);
    return UNITY_END();
}
