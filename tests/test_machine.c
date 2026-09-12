#include <string.h>

#include "yan/machine.h"
#include "test.h"

static int default_platform_and_image(void)
{
    YanMachine machine = {0};
    uint32_t value = 0;
    const uint8_t image[] = {0x93, 0x02, 0x70, 0x00, 0xaa};
    CHECK(yan_machine_init(&machine) == YAN_OK);
    CHECK(machine.ram.size == YAN_RAM_SIZE);
    CHECK(machine.bus.ram == &machine.ram && machine.bus.ram_base == YAN_RAM_BASE);
    CHECK(yan_bus_read(&machine.bus, YAN_RAM_BASE, 4, &value).status == YAN_OK);
    CHECK(value == 0);
    CHECK(yan_bus_write(&machine.bus, UINT32_C(0x81ffffff), 1, 0x5a).status == YAN_OK);
    CHECK(yan_machine_load_image(&machine, image, sizeof image) == YAN_OK);
    CHECK(yan_bus_read(&machine.bus, YAN_RAM_BASE, 4, &value).status == YAN_OK);
    CHECK(value == UINT32_C(0x00700293));
    CHECK(yan_bus_read(&machine.bus, YAN_RAM_BASE + 4, 1, &value).status == YAN_OK);
    CHECK(value == 0xaa);
    CHECK(yan_bus_read(&machine.bus, UINT32_C(0x81ffffff), 1, &value).status == YAN_OK);
    CHECK(value == 0x5a);
    CHECK(yan_bus_read(&machine.bus, UINT32_C(0x82000000), 1, &value).status == YAN_UNMAPPED);
    CHECK(yan_bus_read(&machine.bus, UINT32_C(0x10000000), 1, &value).status == YAN_UNMAPPED);
    uint8_t *allocated = machine.ram.data;
    CHECK(yan_machine_reset(&machine) == YAN_OK);
    CHECK(machine.ram.data == allocated && machine.bus.ram == &machine.ram);
    for (size_t i = 0; i < machine.ram.size; ++i) {
        CHECK(machine.ram.data[i] == 0);
    }
    yan_machine_destroy(&machine);
    return EXIT_SUCCESS;
}

static int rejected_and_overlapping_images(void)
{
    YanMachine machine = {0};
    const uint8_t image[] = {1, 2, 3, 4, 5, 6};
    uint32_t value = 0;
    CHECK(yan_machine_init(&machine) == YAN_OK);
    CHECK(yan_machine_load_image(&machine, image, sizeof image) == YAN_OK);
    CHECK(yan_bus_write(&machine.bus, UINT32_C(0x81ffffff), 1, 0x77).status == YAN_OK);
    CHECK(yan_machine_load_image(&machine, NULL, 0) == YAN_OK);
    CHECK(yan_machine_load_image(&machine, NULL, 1) == YAN_INVALID_ARGUMENT);
    CHECK(yan_machine_load_image(&machine, image, YAN_RAM_SIZE + 1) == YAN_OUT_OF_BOUNDS);
    CHECK(yan_machine_load_image(&machine, image, SIZE_MAX) == YAN_OUT_OF_BOUNDS);
    CHECK(memcmp(machine.ram.data, image, sizeof image) == 0);
    CHECK(machine.ram.data[YAN_RAM_SIZE - 1] == 0x77);
    CHECK(yan_machine_load_image(&machine, machine.ram.data + 2, 4) == YAN_OK);
    CHECK(yan_bus_read(&machine.bus, YAN_RAM_BASE, 4, &value).status == YAN_OK);
    CHECK(value == UINT32_C(0x06050403));
    CHECK(machine.ram.data[4] == 5 && machine.ram.data[5] == 6);
    /* A full-size image is valid, including when source and target coincide. */
    CHECK(yan_bus_write(&machine.bus, UINT32_C(0x81ffffff), 1, 0xfe).status == YAN_OK);
    CHECK(yan_machine_load_image(&machine, machine.ram.data, machine.ram.size) == YAN_OK);
    CHECK(machine.ram.data[YAN_RAM_SIZE - 1] == 0xfe);
    yan_machine_destroy(&machine);
    return EXIT_SUCCESS;
}

static int lifecycle_and_independent_machines(void)
{
    YanMachine first = {0};
    YanMachine second = {0};
    const uint8_t image[] = {0x42};
    uint32_t value = 0;
    CHECK(yan_machine_init(NULL) == YAN_INVALID_ARGUMENT);
    CHECK(yan_machine_reset(NULL) == YAN_INVALID_ARGUMENT);
    CHECK(yan_machine_load_image(NULL, NULL, 0) == YAN_INVALID_ARGUMENT);
    CHECK(yan_machine_reset(&first) == YAN_INVALID_STATE);
    CHECK(yan_machine_load_image(&first, NULL, 0) == YAN_INVALID_STATE);
    CHECK(yan_machine_init(&first) == YAN_OK);
    CHECK(yan_machine_load_image(&first, image, sizeof image) == YAN_OK);
    CHECK(yan_machine_init(&first) == YAN_INVALID_STATE);
    CHECK(first.ram.size == YAN_RAM_SIZE);
    CHECK(yan_machine_init(&second) == YAN_OK);
    CHECK(yan_bus_read(&second.bus, YAN_RAM_BASE, 1, &value).status == YAN_OK && value == 0);
    CHECK(yan_machine_reset(&second) == YAN_OK);
    CHECK(yan_bus_read(&first.bus, YAN_RAM_BASE, 1, &value).status == YAN_OK && value == 0x42);
    yan_machine_destroy(&first);
    CHECK(first.ram.data == NULL && first.ram.size == 0 && first.bus.ram == NULL);
    CHECK(yan_bus_read(&first.bus, YAN_RAM_BASE, 1, &value).status == YAN_INVALID_STATE);
    yan_machine_destroy(&first);
    yan_machine_destroy(NULL);
    CHECK(yan_machine_init(&first) == YAN_OK);
    CHECK(yan_bus_read(&first.bus, YAN_RAM_BASE, 1, &value).status == YAN_OK && value == 0);
    yan_machine_destroy(&first);
    yan_machine_destroy(&second);
    return EXIT_SUCCESS;
}

int main(void)
{
    RUN_TEST(default_platform_and_image);
    RUN_TEST(rejected_and_overlapping_images);
    RUN_TEST(lifecycle_and_independent_machines);
    return EXIT_SUCCESS;
}
