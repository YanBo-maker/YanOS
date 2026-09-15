#include <string.h>

#include "yan/ram.h"
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

static void lifecycle(void)
{
    uint32_t value = 99;
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT, yan_ram_init(NULL, 8));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT, yan_ram_init(&ram, 0));
    TEST_ASSERT_TRUE(ram.data == NULL && ram.size == 0);
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_STATE, yan_ram_read(&ram, 0, 1, &value));
    TEST_ASSERT_TRUE(value == 99);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_init(&ram, 17));
    for (size_t i = 0; i < ram.size; ++i) {
        TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_read(&ram, i, 1, &value));
        TEST_ASSERT_TRUE(value == 0);
    }
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_write(&ram, 0, 1, 42));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_STATE, yan_ram_init(&ram, 32));
    TEST_ASSERT_TRUE(ram.size == 17);
    TEST_ASSERT_TRUE(yan_ram_read(&ram, 0, 1, &value) == YAN_OK && value == 42);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_clear(&ram));
    for (size_t i = 0; i < ram.size; ++i) {
        TEST_ASSERT_TRUE(yan_ram_read(&ram, i, 1, &value) == YAN_OK && value == 0);
    }
    yan_ram_destroy(&ram);
    TEST_ASSERT_TRUE(ram.data == NULL && ram.size == 0);
    yan_ram_destroy(&ram);
    yan_ram_destroy(NULL);
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_STATE, yan_ram_clear(&ram));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_init(&ram, 1));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_write(&ram, 0, 1, 0xff));
    yan_ram_destroy(&ram);
}

static void little_endian_and_widths(void)
{
    uint32_t value = 0;
    const uint8_t expected[] = {0x78, 0x56, 0x34, 0x12};
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_init(&ram, 8));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_write(&ram, 0, 4, UINT32_C(0x12345678)));
    for (size_t i = 0; i < sizeof expected; ++i) {
        TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_read(&ram, i, 1, &value));
        TEST_ASSERT_TRUE(value == expected[i]);
    }
    TEST_ASSERT_TRUE(yan_ram_read(&ram, 0, 2, &value) == YAN_OK && value == 0x5678);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_read(&ram, 0, 4, &value));
    TEST_ASSERT_TRUE(value == UINT32_C(0x12345678));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_write(&ram, 4, 1, UINT32_C(0xffffffab)));
    TEST_ASSERT_TRUE(yan_ram_read(&ram, 4, 1, &value) == YAN_OK && value == 0xab);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_write(&ram, 5, 2, UINT32_C(0xffffcdef)));
    TEST_ASSERT_TRUE(yan_ram_read(&ram, 5, 2, &value) == YAN_OK && value == 0xcdef);
    /* RAM works with byte offsets; the Bus enforces Guest alignment. */
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_write(&ram, 1, 4, UINT32_C(0x80706050)));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_read(&ram, 1, 4, &value));
    TEST_ASSERT_TRUE(value == UINT32_C(0x80706050));
    yan_ram_destroy(&ram);
}

static void boundaries_and_failed_accesses(void)
{
    uint32_t value = UINT32_C(0xdeadbeef);
    uint8_t before[7];
    const size_t bad_widths[] = {0, 3, 8, SIZE_MAX};
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_init(&ram, sizeof before));
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_write(&ram, 3, 4, UINT32_MAX));
    TEST_ASSERT_TRUE(yan_ram_read(&ram, 3, 4, &value) == YAN_OK && value == UINT32_MAX);
    TEST_ASSERT_TRUE(yan_ram_read(&ram, 6, 1, &value) == YAN_OK && value == 0xff);
    TEST_ASSERT_TRUE(yan_ram_read(&ram, 5, 2, &value) == YAN_OK && value == 0xffff);
    memcpy(before, ram.data, sizeof before);
    value = UINT32_C(0xdeadbeef);
    TEST_ASSERT_EQUAL_INT(YAN_OUT_OF_BOUNDS, yan_ram_read(&ram, 4, 4, &value));
    TEST_ASSERT_EQUAL_INT(YAN_OUT_OF_BOUNDS, yan_ram_write(&ram, 4, 4, 0));
    TEST_ASSERT_EQUAL_INT(YAN_OUT_OF_BOUNDS, yan_ram_read(&ram, 7, 1, &value));
    TEST_ASSERT_EQUAL_INT(YAN_OUT_OF_BOUNDS, yan_ram_write(&ram, SIZE_MAX, 4, 0));
    TEST_ASSERT_EQUAL_INT(YAN_OUT_OF_BOUNDS, yan_ram_read(&ram, SIZE_MAX, 4, &value));
    for (size_t i = 0; i < sizeof bad_widths / sizeof bad_widths[0]; ++i) {
        TEST_ASSERT_EQUAL_INT(YAN_INVALID_WIDTH, yan_ram_read(&ram, 0, bad_widths[i], &value));
        TEST_ASSERT_EQUAL_INT(YAN_INVALID_WIDTH, yan_ram_write(&ram, 0, bad_widths[i], 0));
    }
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT, yan_ram_read(&ram, 0, 1, NULL));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT, yan_ram_read(NULL, 0, 1, &value));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT, yan_ram_write(NULL, 0, 1, 0));
    TEST_ASSERT_TRUE(value == UINT32_C(0xdeadbeef));
    TEST_ASSERT_TRUE(memcmp(before, ram.data, sizeof before) == 0);
    yan_ram_destroy(&ram);
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_init(&ram, 1));
    TEST_ASSERT_EQUAL_INT(YAN_OUT_OF_BOUNDS, yan_ram_read(&ram, 0, 4, &value));
    TEST_ASSERT_EQUAL_INT(YAN_OUT_OF_BOUNDS, yan_ram_write(&ram, 0, 4, 0));
    yan_ram_destroy(&ram);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(lifecycle);
    RUN_TEST(little_endian_and_widths);
    RUN_TEST(boundaries_and_failed_accesses);
    return UNITY_END();
}
