#include <string.h>

#include "yan/bus.h"
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

static void mapping_and_endianness(void)
{
    YanBus bus = {0};
    uint32_t value = 0;
    const uint32_t base = UINT32_C(0x80000000);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_init(&ram, 8));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_init(&bus, &ram, base));
    YanBusResult result = yan_bus_write(&bus, base, 4, UINT32_C(0x12345678));
    TEST_ASSERT_EQUAL_INT(YAN_OK, result.status);
    TEST_ASSERT_TRUE(result.address == base && result.width == 4);
    TEST_ASSERT_EQUAL_INT(YAN_ACCESS_WRITE, result.access);
    const uint8_t expected[] = {0x78, 0x56, 0x34, 0x12};
    for (uint32_t i = 0; i < 4; ++i) {
        result = yan_bus_read(&bus, base + i, 1, &value);
        TEST_ASSERT_TRUE(result.status == YAN_OK && value == expected[i]);
        TEST_ASSERT_TRUE(result.access == YAN_ACCESS_READ && result.address == base + i);
    }
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_read(&bus, base, 2, &value).status);
    TEST_ASSERT_TRUE(value == 0x5678);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_write(&bus, base + 6, 2, UINT32_C(0xffffabcd)).status);
    TEST_ASSERT_TRUE(yan_bus_read(&bus, base + 6, 2, &value).status == YAN_OK && value == 0xabcd);
    TEST_ASSERT_TRUE(yan_bus_read(&bus, base + 7, 1, &value).status == YAN_OK && value == 0xab);
    /* Guest alignment applies to the address, not the RAM-relative offset. */
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_init(&bus, &ram, base + 1));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_write(&bus, base + 4, 4, UINT32_C(0xabcdef01)).status);
    TEST_ASSERT_TRUE(yan_ram_read(&ram, 3, 4, &value) == YAN_OK && value == UINT32_C(0xabcdef01));
    yan_ram_destroy(&ram);
}

static void errors_preserve_state(void)
{
    YanBus bus = {0};
    const uint32_t base = UINT32_C(0x80000000);
    uint32_t value = 123;
    uint8_t before[7];
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_init(&ram, sizeof before));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_init(&bus, &ram, base));
    memset(ram.data, 0xa5, ram.size);
    memcpy(before, ram.data, sizeof before);
    YanBusResult result = yan_bus_write(&bus, base + 1, 4, 0);
    TEST_ASSERT_EQUAL_INT(YAN_UNALIGNED, result.status);
    TEST_ASSERT_TRUE(result.address == base + 1 && result.width == 4);
    TEST_ASSERT_EQUAL_INT(YAN_ACCESS_WRITE, result.access);
    TEST_ASSERT_EQUAL_INT(YAN_UNALIGNED, yan_bus_read(&bus, base + 1, 2, &value).status);
    TEST_ASSERT_EQUAL_INT(YAN_OUT_OF_BOUNDS, yan_bus_write(&bus, base + 4, 4, 0).status);
    TEST_ASSERT_EQUAL_INT(YAN_OUT_OF_BOUNDS, yan_bus_read(&bus, base + 4, 4, &value).status);
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED, yan_bus_read(&bus, base - 1, 1, &value).status);
    result = yan_bus_read(&bus, UINT32_C(0x10000000), 1, &value);
    TEST_ASSERT_TRUE(result.status == YAN_UNMAPPED && result.address == UINT32_C(0x10000000));
    TEST_ASSERT_TRUE(result.width == 1);
    TEST_ASSERT_EQUAL_INT(YAN_ACCESS_READ, result.access);
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED, yan_bus_write(&bus, base + 7, 1, 0).status);
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_WIDTH, yan_bus_read(&bus, base + 1, 3, &value).status);
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_WIDTH, yan_bus_write(&bus, base, SIZE_MAX, 0).status);
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT, yan_bus_read(&bus, base, 1, NULL).status);
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT, yan_bus_read(NULL, base, 1, &value).status);
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT, yan_bus_write(NULL, base, 1, 0).status);
    TEST_ASSERT_TRUE(value == 123 && memcmp(before, ram.data, sizeof before) == 0);
    yan_ram_destroy(&ram);
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_STATE, yan_bus_read(&bus, base, 1, &value).status);
}

static void address_space_top_and_invalid_mapping(void)
{
    YanRam empty = {0};
    YanBus bus = {0};
    uint32_t value = 0;
    const uint32_t top = UINT32_C(0xfffffffc);
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_STATE, yan_bus_read(&bus, 0, 1, &value).status);
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT, yan_bus_init(NULL, &ram, 0));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT, yan_bus_init(&bus, NULL, 0));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_STATE, yan_bus_init(&bus, &empty, 0));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_init(&ram, 4));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_init(&bus, &ram, top));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_write(&bus, top, 4, UINT32_MAX).status);
    TEST_ASSERT_TRUE(yan_bus_read(&bus, top, 4, &value).status == YAN_OK && value == UINT32_MAX);
    TEST_ASSERT_TRUE(yan_bus_read(&bus, UINT32_MAX, 1, &value).status == YAN_OK && value == 0xff);
    TEST_ASSERT_EQUAL_INT(YAN_OUT_OF_BOUNDS, yan_bus_init(&bus, &ram, top + 1));
    TEST_ASSERT_TRUE(bus.ram == &ram && bus.ram_base == top);
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED, yan_bus_read(&bus, 0, 4, &value).status);
    TEST_ASSERT_EQUAL_INT(YAN_UNALIGNED, yan_bus_write(&bus, UINT32_MAX, 4, 0).status);
    yan_ram_destroy(&ram);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_init(&ram, 1));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_init(&bus, &ram, UINT32_MAX));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_bus_write(&bus, UINT32_MAX, 1, 0x7f).status);
    TEST_ASSERT_TRUE(yan_bus_read(&bus, UINT32_MAX, 1, &value).status == YAN_OK && value == 0x7f);
    yan_ram_destroy(&ram);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(mapping_and_endianness);
    RUN_TEST(errors_preserve_state);
    RUN_TEST(address_space_top_and_invalid_mapping);
    return UNITY_END();
}
