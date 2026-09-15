#include <string.h>

#include "yan/bus.h"
#include "test.h"

static int mapping_and_endianness(void)
{
    YanRam ram = {0};
    YanBus bus = {0};
    uint32_t value = 0;
    const uint32_t base = UINT32_C(0x80000000);
    CHECK(yan_ram_init(&ram, 8) == YAN_OK);
    CHECK(yan_bus_init(&bus, &ram, base) == YAN_OK);
    YanBusResult result = yan_bus_write(&bus, base, 4, UINT32_C(0x12345678));
    CHECK(result.status == YAN_OK);
    CHECK(result.address == base && result.width == 4);
    CHECK(result.access == YAN_ACCESS_WRITE);
    const uint8_t expected[] = {0x78, 0x56, 0x34, 0x12};
    for (uint32_t i = 0; i < 4; ++i) {
        result = yan_bus_read(&bus, base + i, 1, &value);
        CHECK(result.status == YAN_OK && value == expected[i]);
        CHECK(result.access == YAN_ACCESS_READ && result.address == base + i);
    }
    CHECK(yan_bus_read(&bus, base, 2, &value).status == YAN_OK);
    CHECK(value == 0x5678);
    CHECK(yan_bus_write(&bus, base + 6, 2, UINT32_C(0xffffabcd)).status == YAN_OK);
    CHECK(yan_bus_read(&bus, base + 6, 2, &value).status == YAN_OK && value == 0xabcd);
    CHECK(yan_bus_read(&bus, base + 7, 1, &value).status == YAN_OK && value == 0xab);
    /* Guest alignment applies to the address, not the RAM-relative offset. */
    CHECK(yan_bus_init(&bus, &ram, base + 1) == YAN_OK);
    CHECK(yan_bus_write(&bus, base + 4, 4, UINT32_C(0xabcdef01)).status == YAN_OK);
    CHECK(yan_ram_read(&ram, 3, 4, &value) == YAN_OK && value == UINT32_C(0xabcdef01));
    yan_ram_destroy(&ram);
    return EXIT_SUCCESS;
}

static int errors_preserve_state(void)
{
    YanRam ram = {0};
    YanBus bus = {0};
    const uint32_t base = UINT32_C(0x80000000);
    uint32_t value = 123;
    uint8_t before[7];
    CHECK(yan_ram_init(&ram, sizeof before) == YAN_OK);
    CHECK(yan_bus_init(&bus, &ram, base) == YAN_OK);
    memset(ram.data, 0xa5, ram.size);
    memcpy(before, ram.data, sizeof before);
    YanBusResult result = yan_bus_write(&bus, base + 1, 4, 0);
    CHECK(result.status == YAN_UNALIGNED);
    CHECK(result.address == base + 1 && result.width == 4);
    CHECK(result.access == YAN_ACCESS_WRITE);
    CHECK(yan_bus_read(&bus, base + 1, 2, &value).status == YAN_UNALIGNED);
    CHECK(yan_bus_write(&bus, base + 4, 4, 0).status == YAN_OUT_OF_BOUNDS);
    CHECK(yan_bus_read(&bus, base + 4, 4, &value).status == YAN_OUT_OF_BOUNDS);
    CHECK(yan_bus_read(&bus, base - 1, 1, &value).status == YAN_UNMAPPED);
    result = yan_bus_read(&bus, UINT32_C(0x10000000), 1, &value);
    CHECK(result.status == YAN_UNMAPPED && result.address == UINT32_C(0x10000000));
    CHECK(result.width == 1 && result.access == YAN_ACCESS_READ);
    CHECK(yan_bus_write(&bus, base + 7, 1, 0).status == YAN_UNMAPPED);
    CHECK(yan_bus_read(&bus, base + 1, 3, &value).status == YAN_INVALID_WIDTH);
    CHECK(yan_bus_write(&bus, base, SIZE_MAX, 0).status == YAN_INVALID_WIDTH);
    CHECK(yan_bus_read(&bus, base, 1, NULL).status == YAN_INVALID_ARGUMENT);
    CHECK(yan_bus_read(NULL, base, 1, &value).status == YAN_INVALID_ARGUMENT);
    CHECK(yan_bus_write(NULL, base, 1, 0).status == YAN_INVALID_ARGUMENT);
    CHECK(value == 123 && memcmp(before, ram.data, sizeof before) == 0);
    yan_ram_destroy(&ram);
    CHECK(yan_bus_read(&bus, base, 1, &value).status == YAN_INVALID_STATE);
    return EXIT_SUCCESS;
}

static int address_space_top_and_invalid_mapping(void)
{
    YanRam ram = {0};
    YanRam empty = {0};
    YanBus bus = {0};
    uint32_t value = 0;
    const uint32_t top = UINT32_C(0xfffffffc);
    CHECK(yan_bus_read(&bus, 0, 1, &value).status == YAN_INVALID_STATE);
    CHECK(yan_bus_init(NULL, &ram, 0) == YAN_INVALID_ARGUMENT);
    CHECK(yan_bus_init(&bus, NULL, 0) == YAN_INVALID_ARGUMENT);
    CHECK(yan_bus_init(&bus, &empty, 0) == YAN_INVALID_STATE);
    CHECK(yan_ram_init(&ram, 4) == YAN_OK);
    CHECK(yan_bus_init(&bus, &ram, top) == YAN_OK);
    CHECK(yan_bus_write(&bus, top, 4, UINT32_MAX).status == YAN_OK);
    CHECK(yan_bus_read(&bus, top, 4, &value).status == YAN_OK && value == UINT32_MAX);
    CHECK(yan_bus_read(&bus, UINT32_MAX, 1, &value).status == YAN_OK && value == 0xff);
    CHECK(yan_bus_init(&bus, &ram, top + 1) == YAN_OUT_OF_BOUNDS);
    CHECK(bus.ram == &ram && bus.ram_base == top);
    CHECK(yan_bus_read(&bus, 0, 4, &value).status == YAN_UNMAPPED);
    CHECK(yan_bus_write(&bus, UINT32_MAX, 4, 0).status == YAN_UNALIGNED);
    yan_ram_destroy(&ram);
    CHECK(yan_ram_init(&ram, 1) == YAN_OK);
    CHECK(yan_bus_init(&bus, &ram, UINT32_MAX) == YAN_OK);
    CHECK(yan_bus_write(&bus, UINT32_MAX, 1, 0x7f).status == YAN_OK);
    CHECK(yan_bus_read(&bus, UINT32_MAX, 1, &value).status == YAN_OK && value == 0x7f);
    yan_ram_destroy(&ram);
    return EXIT_SUCCESS;
}

int main(void)
{
    RUN_TEST(mapping_and_endianness);
    RUN_TEST(errors_preserve_state);
    RUN_TEST(address_space_top_and_invalid_mapping);
    return EXIT_SUCCESS;
}
