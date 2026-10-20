/* 0036-R2 acceptance: the v2 multi-block directory.

 * The image is a calloc'd memory device and every directory lives in a calloc'd
 * buffer of D * 4096 bytes, so the suite exercises the same external-directory
 * contract the Guest will use after 0036-R3. The CRC oracle in this file is a
 * second implementation, not yanfs.c's encoder: tampered images are resealed
 * here so mount is asked about the field under test, not about a stale CRC. */
#include "yanfs.h"
#include "fs_test_support.h"
#include "unity.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define V2_BLOCK UINT32_C(4096)
#define V2_MAX_DIR_BLOCKS UINT32_C(64)

typedef struct {
    uint8_t *data;
    uint32_t blocks;
    uint32_t writes;
} V2Device;

static V2Device device;
static YanFs fs;
static uint8_t *directory;

static YanFsIoResult v2_capacity(void *context, uint64_t *blocks)
{
    V2Device *self = (V2Device *)context;
    *blocks = self->blocks;
    return YAN_FS_IO_OK;
}

static YanFsIoResult v2_read(void *context, uint32_t lba, uint8_t out[4096])
{
    V2Device *self = (V2Device *)context;
    if (lba >= self->blocks) {
        return YAN_FS_IO_ERROR;
    }
    memcpy(out, self->data + (size_t)lba * V2_BLOCK, V2_BLOCK);
    return YAN_FS_IO_OK;
}

static YanFsIoResult v2_write(void *context, uint32_t lba,
                              const uint8_t data[4096])
{
    V2Device *self = (V2Device *)context;
    if (lba >= self->blocks) {
        return YAN_FS_IO_ERROR;
    }
    ++self->writes;
    memcpy(self->data + (size_t)lba * V2_BLOCK, data, V2_BLOCK);
    return YAN_FS_IO_OK;
}

static YanFsBlockIo v2_backend(void)
{
    YanFsBlockIo io;
    io.context = &device;
    io.capacity = v2_capacity;
    io.read_block = v2_read;
    io.write_block = v2_write;
    return io;
}

/* Independent IEEE reflected CRC32 over D blocks, block 0 offsets 60..63 as
 * zero. It is deliberately a second implementation of the published format. */
static uint32_t v2_crc(const uint8_t *bytes, uint32_t blocks)
{
    uint32_t crc = UINT32_C(0xffffffff);
    for (uint32_t block = 0; block < blocks; ++block) {
        const uint8_t *base = bytes + (size_t)block * V2_BLOCK;
        for (uint32_t offset = 0; offset < V2_BLOCK; ++offset) {
            uint32_t byte = base[offset];
            if (block == 0u && offset >= 60u && offset < 64u) {
                byte = 0u;
            }
            crc ^= byte;
            for (uint32_t bit = 0; bit < 8u; ++bit) {
                if ((crc & UINT32_C(1)) != 0u) {
                    crc = (crc >> 1) ^ UINT32_C(0xedb88320);
                } else {
                    crc >>= 1;
                }
            }
        }
    }
    return crc ^ UINT32_C(0xffffffff);
}

static void v2_reseal(uint8_t *bytes, uint32_t blocks)
{
    for (uint32_t offset = 60u; offset < 64u; ++offset) {
        bytes[offset] = 0u;
    }
    uint32_t crc = v2_crc(bytes, blocks);
    bytes[60] = (uint8_t)crc;
    bytes[61] = (uint8_t)(crc >> 8);
    bytes[62] = (uint8_t)(crc >> 16);
    bytes[63] = (uint8_t)(crc >> 24);
}

static uint8_t *v2_entry(uint8_t *bytes, uint32_t slot)
{
    return bytes + YAN_FS_HEADER_SIZE + (size_t)slot * YAN_FS_ENTRY_SIZE;
}

static void v2_slot_name(uint32_t slot, char name[8])
{
    name[0] = 'f';
    name[1] = (char)('0' + (slot / 1000u) % 10u);
    name[2] = (char)('0' + (slot / 100u) % 10u);
    name[3] = (char)('0' + (slot / 10u) % 10u);
    name[4] = (char)('0' + slot % 10u);
    name[5] = '\0';
}

static void v2_slot_content(uint32_t slot, uint8_t content[4])
{
    content[0] = (uint8_t)slot;
    content[1] = (uint8_t)(slot >> 8);
    content[2] = (uint8_t)(slot >> 16);
    content[3] = (uint8_t)(slot >> 24);
}

static void v2_start_image(uint32_t dir_blocks, uint32_t capacity)
{
    device.blocks = capacity;
    device.writes = 0u;
    device.data = calloc(capacity, V2_BLOCK);
    TEST_ASSERT_NOT_NULL(device.data);
    directory = calloc(dir_blocks, V2_BLOCK);
    TEST_ASSERT_NOT_NULL(directory);
    memset(&fs, 0, sizeof fs);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK,
                          yan_fs_format_directory(directory, dir_blocks,
                                                  capacity));
    memcpy(device.data, directory, (size_t)dir_blocks * V2_BLOCK);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK,
                          yan_fs_init_directory(&fs, v2_backend(), directory,
                                                dir_blocks));
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_mount(&fs));
}

static void v2_create(uint32_t slot)
{
    char name[8];
    uint8_t content[4];
    v2_slot_name(slot, name);
    v2_slot_content(slot, content);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK,
                          yan_fs_create(&fs, name, content, sizeof content));
}

static void v2_expect_corrupt(uint32_t init_blocks)
{
    free(directory);
    directory = calloc(init_blocks, V2_BLOCK);
    TEST_ASSERT_NOT_NULL(directory);
    uint8_t *before = malloc((size_t)device.blocks * V2_BLOCK);
    TEST_ASSERT_NOT_NULL(before);
    memcpy(before, device.data, (size_t)device.blocks * V2_BLOCK);
    device.writes = 0u;

    TEST_ASSERT_EQUAL_INT(YAN_FS_OK,
                          yan_fs_init_directory(&fs, v2_backend(), directory,
                                                init_blocks));
    TEST_ASSERT_EQUAL_INT(YAN_FS_CORRUPT, yan_fs_mount(&fs));
    TEST_ASSERT_EQUAL_UINT32(0u, device.writes);
    TEST_ASSERT_EQUAL_INT(YAN_FS_UNMOUNTED, fs.state);
    TEST_ASSERT_EQUAL_MEMORY(before, device.data,
                             (size_t)device.blocks * V2_BLOCK);
    for (uint32_t i = 0; i < init_blocks * V2_BLOCK; ++i) {
        TEST_ASSERT_EQUAL_UINT8(0u, directory[i]);
    }
    free(before);
}

void setUp(void)
{
    device.data = NULL;
    device.blocks = 0u;
    device.writes = 0u;
    directory = NULL;
    memset(&fs, 0, sizeof fs);
}

void tearDown(void)
{
    free(device.data);
    free(directory);
    device.data = NULL;
    directory = NULL;
}

static void test_0036_R2_AC1_1023_files_round_trip(void)
{
    const uint32_t dir_blocks = 16u;
    const uint32_t capacity = 16u + 1023u + 1u;
    v2_start_image(dir_blocks, capacity);
    for (uint32_t slot = 0; slot < 1023u; ++slot) {
        v2_create(slot);
    }
    char name[8];
    uint8_t content[4];
    v2_slot_name(1023u, name);
    v2_slot_content(1023u, content);
    TEST_ASSERT_EQUAL_INT(YAN_FS_DIRECTORY_FULL,
                          yan_fs_create(&fs, name, content, sizeof content));

    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_unmount(&fs));
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_mount(&fs));

    uint32_t cursor = 0u;
    YanFsInfo info;
    for (uint32_t slot = 0; slot < 1023u; ++slot) {
        TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_list(&fs, &cursor, &info));
        v2_slot_name(slot, name);
        TEST_ASSERT_EQUAL_STRING(name, info.name);
        TEST_ASSERT_EQUAL_UINT32(4u, info.size_bytes);
        uint8_t readback[4] = {0};
        uint32_t got = 0u;
        TEST_ASSERT_EQUAL_INT(YAN_FS_OK,
                              yan_fs_read(&fs, name, 0u, readback,
                                          sizeof readback, &got));
        v2_slot_content(slot, content);
        TEST_ASSERT_EQUAL_UINT32(4u, got);
        TEST_ASSERT_EQUAL_MEMORY(content, readback, sizeof content);
    }
    TEST_ASSERT_EQUAL_UINT32(1023u, cursor);
    TEST_ASSERT_EQUAL_INT(YAN_FS_END, yan_fs_list(&fs, &cursor, &info));
    TEST_ASSERT_EQUAL_UINT32(1023u, cursor);
}

static void test_0036_R2_AC2_mount_rejects_tampered_v2_headers(void)
{
    v2_start_image(2u, 140u);
    const uint8_t one = 'a';
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_create(&fs, "a", &one, 1u));
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_unmount(&fs));

    uint8_t *base = malloc((size_t)device.blocks * V2_BLOCK);
    TEST_ASSERT_NOT_NULL(base);
    memcpy(base, device.data, (size_t)device.blocks * V2_BLOCK);

    /* directory_blocks = 3 with a matching 3-block mount buffer: the header
     * passes the D range but entry_capacity is still 127, not 191. */
    memcpy(device.data, base, (size_t)device.blocks * V2_BLOCK);
    device.data[24] = 3u;
    v2_reseal(device.data, 3u);
    v2_expect_corrupt(3u);

    /* entry_capacity = 63 does not match 64*2-1. */
    memcpy(device.data, base, (size_t)device.blocks * V2_BLOCK);
    device.data[20] = 63u;
    v2_reseal(device.data, 2u);
    v2_expect_corrupt(2u);

    /* A data-block extent start is below the data start D. */
    memcpy(device.data, base, (size_t)device.blocks * V2_BLOCK);
    v2_entry(device.data, 0u)[36] = 1u;
    v2_entry(device.data, 0u)[37] = 0u;
    v2_entry(device.data, 0u)[38] = 0u;
    v2_entry(device.data, 0u)[39] = 0u;
    v2_reseal(device.data, 2u);
    v2_expect_corrupt(2u);

    /* One byte of directory block 1 changes and the CRC is not resealed. */
    memcpy(device.data, base, (size_t)device.blocks * V2_BLOCK);
    device.data[V2_BLOCK + 20u] ^= 0x01u;
    v2_expect_corrupt(2u);

    free(base);
}

static void test_0036_R2_AC3_slot_reuse_touches_only_directory_and_new_extent(void)
{
    v2_start_image(2u, 200u);
    for (uint32_t slot = 0; slot < 65u; ++slot) {
        v2_create(slot);
    }
    uint8_t *before = malloc((size_t)device.blocks * V2_BLOCK);
    TEST_ASSERT_NOT_NULL(before);
    memcpy(before, device.data, (size_t)device.blocks * V2_BLOCK);

    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_remove(&fs, "f0064"));
    const uint8_t content = 'n';
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK,
                          yan_fs_create(&fs, "new", &content, 1u));

    uint32_t cursor = 0u;
    YanFsInfo info;
    for (uint32_t slot = 0; slot <= 64u; ++slot) {
        TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_list(&fs, &cursor, &info));
        if (slot == 64u) {
            TEST_ASSERT_EQUAL_STRING("new", info.name);
        }
    }
    uint32_t new_start = 0u;
    memcpy(&new_start, v2_entry(fs.directory, 64u) + 36u, sizeof new_start);
    TEST_ASSERT_TRUE(new_start >= 2u);
    TEST_ASSERT_TRUE(new_start < device.blocks);

    for (uint32_t block = 0; block < device.blocks; ++block) {
        const uint8_t *old_block = before + (size_t)block * V2_BLOCK;
        const uint8_t *new_block = device.data + (size_t)block * V2_BLOCK;
        if (block == 1u || block == new_start) {
            continue;
        }
        if (block == 0u) {
            for (uint32_t i = 0; i < V2_BLOCK; ++i) {
                if (i >= 60u && i < 64u) {
                    continue;
                }
                TEST_ASSERT_EQUAL_UINT8(old_block[i], new_block[i]);
            }
            continue;
        }
        TEST_ASSERT_EQUAL_MEMORY(old_block, new_block, V2_BLOCK);
    }
    free(before);
}

static void test_0036_R2_v2_mismatch_is_invalid_version_three_unsupported(void)
{
    v2_start_image(2u, 140u);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_unmount(&fs));
    uint8_t *before = malloc((size_t)device.blocks * V2_BLOCK);
    TEST_ASSERT_NOT_NULL(before);
    memcpy(before, device.data, (size_t)device.blocks * V2_BLOCK);

    free(directory);
    directory = calloc(3u, V2_BLOCK);
    TEST_ASSERT_NOT_NULL(directory);
    device.writes = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK,
                          yan_fs_init_directory(&fs, v2_backend(), directory,
                                                3u));
    TEST_ASSERT_EQUAL_INT(YAN_FS_INVALID, yan_fs_mount(&fs));
    TEST_ASSERT_EQUAL_UINT32(0u, device.writes);
    TEST_ASSERT_EQUAL_MEMORY(before, device.data,
                             (size_t)device.blocks * V2_BLOCK);

    device.data[8] = 3u;
    device.writes = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK,
                          yan_fs_init_directory(&fs, v2_backend(), directory,
                                                2u));
    TEST_ASSERT_EQUAL_INT(YAN_FS_UNSUPPORTED, yan_fs_mount(&fs));
    TEST_ASSERT_EQUAL_UINT32(0u, device.writes);
    free(before);
}

static void test_0036_R2_init_directory_rejects_bad_arguments(void)
{
    YanFs local;
    memset(&local, 0, sizeof local);
    YanFsBlockIo io = v2_backend();
    uint8_t *first = directory;
    directory = calloc(2u, V2_BLOCK);
    TEST_ASSERT_NOT_NULL(directory);

    TEST_ASSERT_EQUAL_INT(YAN_FS_INVALID,
                          yan_fs_init_directory(NULL, io, directory, 1u));
    TEST_ASSERT_EQUAL_INT(YAN_FS_INVALID,
                          yan_fs_init_directory(&local, io, NULL, 1u));
    TEST_ASSERT_EQUAL_INT(YAN_FS_INVALID,
                          yan_fs_init_directory(&local, io, directory, 0u));
    TEST_ASSERT_EQUAL_INT(YAN_FS_INVALID,
                          yan_fs_init_directory(&local, io, directory, 65u));
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_INVALID,
        yan_fs_init_directory(&local, io, (uint8_t *)(void *)&local, 1u));

    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, test_fs_init(&local, io));
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_unmount(&local));
    free(first);
}

static void test_0036_R2_external_directory_is_not_caller_scratch(void)
{
    v2_start_image(2u, 10u);
    YanFsInfo info;
    uint32_t cursor = 0u;
    device.writes = 0u;

    TEST_ASSERT_EQUAL_INT(
        YAN_FS_INVALID,
        yan_fs_stat(&fs, (const char *)(const void *)directory, &info));
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_INVALID,
        yan_fs_list(&fs, &cursor, (YanFsInfo *)(void *)directory));
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_INVALID,
        yan_fs_create(&fs, "x", directory + YAN_FS_HEADER_SIZE, 1u));
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_INVALID,
        yan_fs_create(&fs, (const char *)(const void *)directory,
                      (const uint8_t *)"x", 1u));
    TEST_ASSERT_EQUAL_UINT32(0u, device.writes);
}


/* 0036-R3: probe reads block 0 and publishes D without touching the instance
 * or the medium. It is the startup step that decides how large the arena must
 * be, so it must not allocate or write. */
static void test_0036_R3_probe_reads_v1_and_v2(void)
{
    uint8_t scratch[YAN_FS_BLOCK_SIZE];
    uint32_t blocks = 0u;
    YanFsBlockIo io = v2_backend();

    v2_start_image(1u, 16u);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK,
                          yan_fs_probe(&io, scratch, &blocks));
    TEST_ASSERT_EQUAL_UINT32(1u, blocks);

    free(directory);
    free(device.data);
    directory = NULL;
    device.data = NULL;
    v2_start_image(16u, 64u);
    blocks = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK,
                          yan_fs_probe(&io, scratch, &blocks));
    TEST_ASSERT_EQUAL_UINT32(16u, blocks);
}

static void test_0036_R3_probe_rejects_bad_headers_without_writing(void)
{
    uint8_t scratch[YAN_FS_BLOCK_SIZE];
    uint32_t blocks = 0u;
    YanFsBlockIo io = v2_backend();

    v2_start_image(1u, 16u);
    device.writes = 0u;
    /* Unknown version is UNSUPPORTED, not CORRUPT. */
    device.data[8] = 3u;
    device.data[9] = device.data[10] = device.data[11] = 0u;
    blocks = 123u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_UNSUPPORTED,
                          yan_fs_probe(&io, scratch, &blocks));
    TEST_ASSERT_EQUAL_UINT32(123u, blocks);

    /* v2 with D outside 1..64. */
    device.data[8] = 2u;
    device.data[24] = 0u;
    device.data[25] = 0u;
    device.data[26] = 0u;
    device.data[27] = 0u;
    device.data[24] = 65u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_CORRUPT,
                          yan_fs_probe(&io, scratch, &blocks));

    /* v2 with a legal D but entry_capacity != 64*D-1. */
    device.data[24] = 2u;
    device.data[20] = 64u;
    device.data[21] = device.data[22] = device.data[23] = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_CORRUPT,
                          yan_fs_probe(&io, scratch, &blocks));

    /* Bad magic. */
    device.data[0] = (uint8_t)'X';
    TEST_ASSERT_EQUAL_INT(YAN_FS_CORRUPT,
                          yan_fs_probe(&io, scratch, &blocks));

    /* A read error is IO; an unknown callback value is PROTOCOL. */
    device.blocks = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO,
                          yan_fs_probe(&io, scratch, &blocks));

    /* Invalid arguments never read. */
    TEST_ASSERT_EQUAL_INT(YAN_FS_INVALID,
                          yan_fs_probe(NULL, scratch, &blocks));
    TEST_ASSERT_EQUAL_INT(YAN_FS_INVALID,
                          yan_fs_probe(&io, NULL, &blocks));
    TEST_ASSERT_EQUAL_INT(YAN_FS_INVALID,
                          yan_fs_probe(&io, scratch, NULL));
    TEST_ASSERT_EQUAL_UINT32(0u, device.writes);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_0036_R2_AC1_1023_files_round_trip);
    RUN_TEST(test_0036_R2_AC2_mount_rejects_tampered_v2_headers);
    RUN_TEST(test_0036_R2_AC3_slot_reuse_touches_only_directory_and_new_extent);
    RUN_TEST(test_0036_R2_v2_mismatch_is_invalid_version_three_unsupported);
    RUN_TEST(test_0036_R2_init_directory_rejects_bad_arguments);
    RUN_TEST(test_0036_R2_external_directory_is_not_caller_scratch);
    RUN_TEST(test_0036_R3_probe_reads_v1_and_v2);
    RUN_TEST(test_0036_R3_probe_rejects_bad_headers_without_writing);
    return UNITY_END();
}
