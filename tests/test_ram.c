#include <string.h>

#include "yan/ram.h"
#include "test.h"

static int lifecycle(void)
{
    YanRam ram = {0};
    uint32_t value = 99;
    CHECK(yan_ram_init(NULL, 8) == YAN_INVALID_ARGUMENT);
    CHECK(yan_ram_init(&ram, 0) == YAN_INVALID_ARGUMENT);
    CHECK(ram.data == NULL && ram.size == 0);
    CHECK(yan_ram_read(&ram, 0, 1, &value) == YAN_INVALID_STATE);
    CHECK(value == 99);
    CHECK(yan_ram_init(&ram, 17) == YAN_OK);
    for (size_t i = 0; i < ram.size; ++i) {
        CHECK(yan_ram_read(&ram, i, 1, &value) == YAN_OK);
        CHECK(value == 0);
    }
    CHECK(yan_ram_write(&ram, 0, 1, 42) == YAN_OK);
    CHECK(yan_ram_init(&ram, 32) == YAN_INVALID_STATE);
    CHECK(ram.size == 17);
    CHECK(yan_ram_read(&ram, 0, 1, &value) == YAN_OK && value == 42);
    CHECK(yan_ram_clear(&ram) == YAN_OK);
    for (size_t i = 0; i < ram.size; ++i) {
        CHECK(yan_ram_read(&ram, i, 1, &value) == YAN_OK && value == 0);
    }
    yan_ram_destroy(&ram);
    CHECK(ram.data == NULL && ram.size == 0);
    yan_ram_destroy(&ram);
    yan_ram_destroy(NULL);
    CHECK(yan_ram_clear(&ram) == YAN_INVALID_STATE);
    CHECK(yan_ram_init(&ram, 1) == YAN_OK);
    CHECK(yan_ram_write(&ram, 0, 1, 0xff) == YAN_OK);
    yan_ram_destroy(&ram);
    return EXIT_SUCCESS;
}

static int little_endian_and_widths(void)
{
    YanRam ram = {0};
    uint32_t value = 0;
    const uint8_t expected[] = {0x78, 0x56, 0x34, 0x12};
    CHECK(yan_ram_init(&ram, 8) == YAN_OK);
    CHECK(yan_ram_write(&ram, 0, 4, UINT32_C(0x12345678)) == YAN_OK);
    for (size_t i = 0; i < sizeof expected; ++i) {
        CHECK(yan_ram_read(&ram, i, 1, &value) == YAN_OK);
        CHECK(value == expected[i]);
    }
    CHECK(yan_ram_read(&ram, 0, 2, &value) == YAN_OK && value == 0x5678);
    CHECK(yan_ram_read(&ram, 0, 4, &value) == YAN_OK);
    CHECK(value == UINT32_C(0x12345678));
    CHECK(yan_ram_write(&ram, 4, 1, UINT32_C(0xffffffab)) == YAN_OK);
    CHECK(yan_ram_read(&ram, 4, 1, &value) == YAN_OK && value == 0xab);
    CHECK(yan_ram_write(&ram, 5, 2, UINT32_C(0xffffcdef)) == YAN_OK);
    CHECK(yan_ram_read(&ram, 5, 2, &value) == YAN_OK && value == 0xcdef);
    /* RAM works with byte offsets; the Bus enforces Guest alignment. */
    CHECK(yan_ram_write(&ram, 1, 4, UINT32_C(0x80706050)) == YAN_OK);
    CHECK(yan_ram_read(&ram, 1, 4, &value) == YAN_OK);
    CHECK(value == UINT32_C(0x80706050));
    yan_ram_destroy(&ram);
    return EXIT_SUCCESS;
}

static int boundaries_and_failed_accesses(void)
{
    YanRam ram = {0};
    uint32_t value = UINT32_C(0xdeadbeef);
    uint8_t before[7];
    const size_t bad_widths[] = {0, 3, 8, SIZE_MAX};
    CHECK(yan_ram_init(&ram, sizeof before) == YAN_OK);
    CHECK(yan_ram_write(&ram, 3, 4, UINT32_MAX) == YAN_OK);
    CHECK(yan_ram_read(&ram, 3, 4, &value) == YAN_OK && value == UINT32_MAX);
    CHECK(yan_ram_read(&ram, 6, 1, &value) == YAN_OK && value == 0xff);
    CHECK(yan_ram_read(&ram, 5, 2, &value) == YAN_OK && value == 0xffff);
    memcpy(before, ram.data, sizeof before);
    value = UINT32_C(0xdeadbeef);
    CHECK(yan_ram_read(&ram, 4, 4, &value) == YAN_OUT_OF_BOUNDS);
    CHECK(yan_ram_write(&ram, 4, 4, 0) == YAN_OUT_OF_BOUNDS);
    CHECK(yan_ram_read(&ram, 7, 1, &value) == YAN_OUT_OF_BOUNDS);
    CHECK(yan_ram_write(&ram, SIZE_MAX, 4, 0) == YAN_OUT_OF_BOUNDS);
    CHECK(yan_ram_read(&ram, SIZE_MAX, 4, &value) == YAN_OUT_OF_BOUNDS);
    for (size_t i = 0; i < sizeof bad_widths / sizeof bad_widths[0]; ++i) {
        CHECK(yan_ram_read(&ram, 0, bad_widths[i], &value) == YAN_INVALID_WIDTH);
        CHECK(yan_ram_write(&ram, 0, bad_widths[i], 0) == YAN_INVALID_WIDTH);
    }
    CHECK(yan_ram_read(&ram, 0, 1, NULL) == YAN_INVALID_ARGUMENT);
    CHECK(yan_ram_read(NULL, 0, 1, &value) == YAN_INVALID_ARGUMENT);
    CHECK(yan_ram_write(NULL, 0, 1, 0) == YAN_INVALID_ARGUMENT);
    CHECK(value == UINT32_C(0xdeadbeef));
    CHECK(memcmp(before, ram.data, sizeof before) == 0);
    yan_ram_destroy(&ram);
    CHECK(yan_ram_init(&ram, 1) == YAN_OK);
    CHECK(yan_ram_read(&ram, 0, 4, &value) == YAN_OUT_OF_BOUNDS);
    CHECK(yan_ram_write(&ram, 0, 4, 0) == YAN_OUT_OF_BOUNDS);
    yan_ram_destroy(&ram);
    return EXIT_SUCCESS;
}

int main(void)
{
    RUN_TEST(lifecycle);
    RUN_TEST(little_endian_and_widths);
    RUN_TEST(boundaries_and_failed_accesses);
    return EXIT_SUCCESS;
}
