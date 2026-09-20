#include <string.h>

#include "yan/image.h"
#include "unity.h"

void setUp(void)
{
}

void tearDown(void)
{
}

/* A hand-built ELF32 image keeps these tests independent of a RISC-V
 * toolchain: a Host without a cross compiler still validates the loader. */
enum {
    RAM_SIZE = 4096,
    ELF_SIZE = 512,
    PH_OFFSET = 52,
    PH_NUM = 1,
    PH_SIZE = 32,
    TEXT_OFFSET = 128,
    TEXT_FILESZ = 8,
    TEXT_MEMSZ = 16,
    SYMTAB_OFFSET = 160,
    SYMTAB_SIZE = 48,
    STRTAB_OFFSET = 208,
    STRTAB_SIZE = 15,
    SH_OFFSET = 224,
    SH_NUM = 3,
    SH_SIZE = 40,
    STRTAB_INDEX = 2,
    SHN_UNDEF = 0,
    SHT_SYMTAB = 2,
    SHT_STRTAB = 3
};

/* Guest addresses exceed the range an enumerator may have. */
static const uint32_t TEXT_ADDRESS = UINT32_C(0x80000000);
static const uint32_t TOHOST_ADDRESS = UINT32_C(0x80001000);

static const uint8_t guest_text[TEXT_FILESZ] = {
    0x13, 0x05, 0x00, 0x00, 0x6f, 0x00, 0x00, 0x00
};
static const char guest_strings[STRTAB_SIZE] = "\0_start\0tohost";

static void put_u16(uint8_t *image, size_t at, uint16_t value)
{
    image[at] = (uint8_t)value;
    image[at + 1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *image, size_t at, uint32_t value)
{
    for (size_t index = 0; index < 4; ++index) {
        image[at + index] = (uint8_t)(value >> (8 * index));
    }
}

static void put_symbol(uint8_t *image, size_t index, uint32_t name_offset,
                       uint32_t value, uint16_t section)
{
    const size_t at = SYMTAB_OFFSET + index * 16;
    put_u32(image, at + 0, name_offset);
    put_u32(image, at + 4, value);
    put_u32(image, at + 8, 0);
    image[at + 12] = 0x12;
    image[at + 13] = 0;
    put_u16(image, at + 14, section);
}

/* Lays out a minimal but well-formed image: one PT_LOAD segment whose file
 * size is smaller than its memory size, plus .symtab and .strtab. */
static size_t build_image(uint8_t *image)
{
    memset(image, 0, ELF_SIZE);
    image[0] = 0x7f;
    image[1] = 'E';
    image[2] = 'L';
    image[3] = 'F';
    image[4] = 1; /* ELFCLASS32 */
    image[5] = 1; /* ELFDATA2LSB */
    image[6] = 1; /* EV_CURRENT */
    put_u16(image, 16, 2);
    put_u16(image, 18, 243); /* EM_RISCV */
    put_u32(image, 20, 1);
    put_u32(image, 24, TEXT_ADDRESS);
    put_u32(image, 28, PH_OFFSET);
    put_u32(image, 32, SH_OFFSET);
    put_u16(image, 40, 52);
    put_u16(image, 42, PH_SIZE);
    put_u16(image, 44, PH_NUM);
    put_u16(image, 46, SH_SIZE);
    put_u16(image, 48, SH_NUM);
    put_u16(image, 50, 0);

    put_u32(image, PH_OFFSET + 0, 1); /* PT_LOAD */
    put_u32(image, PH_OFFSET + 4, TEXT_OFFSET);
    put_u32(image, PH_OFFSET + 12, TEXT_ADDRESS);
    put_u32(image, PH_OFFSET + 16, TEXT_FILESZ);
    put_u32(image, PH_OFFSET + 20, TEXT_MEMSZ);
    memcpy(image + TEXT_OFFSET, guest_text, sizeof guest_text);

    put_symbol(image, 0, 0, 0, SHN_UNDEF);
    put_symbol(image, 1, 1, TEXT_ADDRESS, 1);
    put_symbol(image, 2, 8, TOHOST_ADDRESS, 1);
    memcpy(image + STRTAB_OFFSET, guest_strings, sizeof guest_strings);

    put_u32(image, SH_OFFSET + SH_SIZE + 4, SHT_SYMTAB);
    put_u32(image, SH_OFFSET + SH_SIZE + 16, SYMTAB_OFFSET);
    put_u32(image, SH_OFFSET + SH_SIZE + 20, SYMTAB_SIZE);
    put_u32(image, SH_OFFSET + SH_SIZE + 24, STRTAB_INDEX);
    put_u32(image, SH_OFFSET + SH_SIZE + 36, 16);

    put_u32(image, SH_OFFSET + 2 * SH_SIZE + 4, SHT_STRTAB);
    put_u32(image, SH_OFFSET + 2 * SH_SIZE + 16, STRTAB_OFFSET);
    put_u32(image, SH_OFFSET + 2 * SH_SIZE + 20, STRTAB_SIZE);
    return ELF_SIZE;
}

static void load_maps_segments_and_clears_bss(void)
{
    uint8_t image[ELF_SIZE];
    const size_t size = build_image(image);
    YanRam ram = {0};
    YanImageInfo info = {0};
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_init(&ram, RAM_SIZE));
    memset(ram.data, 0xaa, ram.size);

    TEST_ASSERT_EQUAL_INT(YAN_OK,
                          yan_image_load_elf(&ram, TEXT_ADDRESS, image, size, &info));
    TEST_ASSERT_EQUAL_HEX32(TEXT_ADDRESS, info.entry);
    TEST_ASSERT_EQUAL_MEMORY(guest_text, ram.data, TEXT_FILESZ);
    for (size_t at = TEXT_FILESZ; at < TEXT_MEMSZ; ++at) {
        TEST_ASSERT_EQUAL_UINT8(0, ram.data[at]);
    }
    TEST_ASSERT_EQUAL_UINT8(0xaa, ram.data[TEXT_MEMSZ]);
    yan_ram_destroy(&ram);
}

static void symbols_are_found_by_name(void)
{
    uint8_t image[ELF_SIZE];
    const size_t size = build_image(image);
    uint32_t address = 0;

    TEST_ASSERT_EQUAL_INT(YAN_OK,
                          yan_image_find_symbol(image, size, "tohost", &address));
    TEST_ASSERT_EQUAL_HEX32(TOHOST_ADDRESS, address);
    TEST_ASSERT_EQUAL_INT(YAN_OK,
                          yan_image_find_symbol(image, size, "_start", &address));
    TEST_ASSERT_EQUAL_HEX32(TEXT_ADDRESS, address);
    TEST_ASSERT_EQUAL_INT(YAN_UNMAPPED,
                          yan_image_find_symbol(image, size, "missing", &address));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT,
                          yan_image_find_symbol(NULL, size, "tohost", &address));
}

static void malformed_images_are_rejected(void)
{
    uint8_t image[ELF_SIZE];
    uint8_t copy[ELF_SIZE];
    const size_t size = build_image(image);
    YanRam ram = {0};
    YanImageInfo info = {0};
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_init(&ram, RAM_SIZE));
    memset(ram.data, 0xaa, ram.size);

    memcpy(copy, image, size);
    copy[1] = 'X';
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_IMAGE,
                          yan_image_load_elf(&ram, TEXT_ADDRESS, copy, size, &info));

    memcpy(copy, image, size);
    copy[4] = 2; /* ELFCLASS64 */
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_IMAGE,
                          yan_image_load_elf(&ram, TEXT_ADDRESS, copy, size, &info));

    memcpy(copy, image, size);
    copy[5] = 2; /* ELFDATA2MSB */
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_IMAGE,
                          yan_image_load_elf(&ram, TEXT_ADDRESS, copy, size, &info));

    memcpy(copy, image, size);
    put_u16(copy, 18, 62); /* EM_X86_64 */
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_IMAGE,
                          yan_image_load_elf(&ram, TEXT_ADDRESS, copy, size, &info));

    memcpy(copy, image, size);
    put_u16(copy, 44, 64); /* program headers do not fit */
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_IMAGE,
                          yan_image_load_elf(&ram, TEXT_ADDRESS, copy, size, &info));

    memcpy(copy, image, size);
    put_u32(copy, PH_OFFSET + 16, TEXT_FILESZ + TEXT_MEMSZ); /* filesz > memsz */
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_IMAGE,
                          yan_image_load_elf(&ram, TEXT_ADDRESS, copy, size, &info));

    memcpy(copy, image, size);
    put_u32(copy, PH_OFFSET + 12, TEXT_ADDRESS - 4); /* below RAM base */
    TEST_ASSERT_EQUAL_INT(YAN_OUT_OF_BOUNDS,
                          yan_image_load_elf(&ram, TEXT_ADDRESS, copy, size, &info));

    memcpy(copy, image, size);
    put_u32(copy, PH_OFFSET + 20, RAM_SIZE + 4); /* past the end of RAM */
    TEST_ASSERT_EQUAL_INT(YAN_OUT_OF_BOUNDS,
                          yan_image_load_elf(&ram, TEXT_ADDRESS, copy, size, &info));

    TEST_ASSERT_EQUAL_INT(YAN_INVALID_IMAGE,
                          yan_image_load_elf(&ram, TEXT_ADDRESS, image, 8, &info));

    for (size_t at = 0; at < ram.size; ++at) {
        TEST_ASSERT_EQUAL_UINT8(0xaa, ram.data[at]);
    }
    yan_ram_destroy(&ram);
}

static void missing_arguments_are_rejected(void)
{
    uint8_t image[ELF_SIZE];
    const size_t size = build_image(image);
    YanRam ram = {0};
    YanImageInfo info = {0};
    TEST_ASSERT_EQUAL_INT(YAN_OK, yan_ram_init(&ram, RAM_SIZE));

    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT,
                          yan_image_load_elf(NULL, TEXT_ADDRESS, image, size, &info));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT,
                          yan_image_load_elf(&ram, TEXT_ADDRESS, NULL, size, &info));
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_ARGUMENT,
                          yan_image_load_elf(&ram, TEXT_ADDRESS, image, size, NULL));

    YanRam empty = {0};
    TEST_ASSERT_EQUAL_INT(YAN_INVALID_STATE,
                          yan_image_load_elf(&empty, TEXT_ADDRESS, image, size, &info));
    yan_ram_destroy(&ram);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(load_maps_segments_and_clears_bss);
    RUN_TEST(symbols_are_found_by_name);
    RUN_TEST(malformed_images_are_rejected);
    RUN_TEST(missing_arguments_are_rejected);
    return UNITY_END();
}
