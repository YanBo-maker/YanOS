#include "yanfs.h"
#include "unity.h"

#include <string.h>

/* YanFS core tests: the on-disk format, mount validation, the alias guards,
 * list/stat and the create/replace/read/remove path. The device is an
 * in-memory array behind the three synchronous callbacks, so every fault
 * injection happens at a call boundary the core really uses.
 *
 * The fixtures are encoded independently of os/yanfs.c: a separate little-
 * endian writer, a separate CRC32 implementation and a separate header builder.
 * Comparing production output against a fixture built by the same production
 * function would only prove self-consistency; the tests below compare it
 * against this file's own encoder, and use a published CRC vector to pin the
 * algorithm itself.
 *
 * The write injection deliberately copies part of a failing block before it
 * reports the error, so "no rollback promised" is shown on bytes that really
 * moved rather than on a callback that only pretends to fail. */

#define TEST_DEVICE_BLOCKS 16u

typedef struct {
    uint8_t blocks[TEST_DEVICE_BLOCKS][YAN_FS_BLOCK_SIZE];
    uint64_t capacity_blocks;
    uint32_t capacity_calls;
    uint32_t reads;
    uint32_t writes;
    uint32_t last_read_lba;
    uint32_t last_write_lba;
    YanFsIoResult capacity_status;
    YanFsIoResult read_status;
    YanFsIoResult write_status;
    /* Fail the N-th call after the counters are reset (0 = never) and copy this
     * many bytes into the block before returning the error. */
    uint32_t read_fail_at;
    YanFsIoResult read_fail_code;
    uint32_t write_fail_at;
    YanFsIoResult write_fail_code;
    uint32_t write_partial;
} TestDevice;

static TestDevice device;
static YanFs fs;
static uint8_t block[YAN_FS_BLOCK_SIZE];
static uint8_t expected[YAN_FS_BLOCK_SIZE];
static uint8_t payload[5000];
static uint8_t readback[5000 + 16];
static uint32_t aligned_words[8];
static uint8_t medium_before[TEST_DEVICE_BLOCKS][YAN_FS_BLOCK_SIZE];
static uint8_t neighbour[128];
static uint8_t entry_before[YAN_FS_ENTRY_SIZE];

/* The probe runs from inside a device callback while an outer operation is
 * busy: every other entry point must answer BUSY, and the metadata cache must
 * still hold the previous directory. */
static YanFs *probe_fs;
static bool probe_ran;
static YanFsResult probe_list_result;
static YanFsResult probe_stat_result;
static YanFsResult probe_mount_result;
static YanFsResult probe_unmount_result;
static YanFsResult probe_init_result;
static YanFsResult probe_read_result;
static YanFsResult probe_read_null_result;
static YanFsResult probe_read_alias_result;
/* Isolated around each new probe read: the whole YanFs must stay byte-identical
 * and no extra device callback may run, even though the outer operation is
 * mutating the medium in the same callback. */
static bool probe_read_null_fs_unchanged;
static uint32_t probe_read_null_io;
static bool probe_read_alias_fs_unchanged;
static uint32_t probe_read_alias_io;
static YanFsResult probe_create_result;
static YanFsResult probe_remove_result;
static uint32_t probe_cache_crc;
static bool probe_cache_unpublished;

static bool region_is_zero(const uint8_t *bytes, uint32_t length)
{
    for (uint32_t i = 0; i < length; ++i) {
        if (bytes[i] != 0u) {
            return false;
        }
    }
    return true;
}

static bool region_is_byte(const uint8_t *bytes, uint32_t length, uint8_t value)
{
    for (uint32_t i = 0; i < length; ++i) {
        if (bytes[i] != value) {
            return false;
        }
    }
    return true;
}

static bool region_equal(const uint8_t *left, const uint8_t *right, uint32_t length)
{
    for (uint32_t i = 0; i < length; ++i) {
        if (left[i] != right[i]) {
            return false;
        }
    }
    return true;
}

static void fill_payload(uint32_t length, uint8_t seed)
{
    for (uint32_t i = 0; i < length; ++i) {
        payload[i] = (uint8_t)(seed + i * 7u + (i >> 8));
    }
}

static void snapshot_medium(void)
{
    memcpy(medium_before, device.blocks, sizeof device.blocks);
}

static bool medium_equal_snapshot(void)
{
    return memcmp(medium_before, device.blocks, sizeof device.blocks) == 0;
}

/* ---------------------------------------------------------------- fixtures */

static void fx_put_le32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

static uint32_t fx_get_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static uint32_t fx_crc32_bytes(const uint8_t *bytes, uint32_t length)
{
    uint32_t crc = UINT32_C(0xffffffff);
    for (uint32_t i = 0; i < length; ++i) {
        crc ^= (uint32_t)bytes[i];
        for (uint32_t bit = 0; bit < 8; ++bit) {
            uint32_t low = crc & UINT32_C(1);
            crc >>= 1;
            if (low != 0u) {
                crc ^= UINT32_C(0xedb88320);
            }
        }
    }
    return crc ^ UINT32_C(0xffffffff);
}

/* The same CRC over a whole block, with the stored CRC field read as zero. */
static uint32_t fx_block_crc(const uint8_t *data)
{
    uint32_t crc = UINT32_C(0xffffffff);
    for (uint32_t offset = 0; offset < YAN_FS_BLOCK_SIZE; ++offset) {
        uint8_t byte = (offset >= 60u && offset < 64u) ? 0u : data[offset];
        crc ^= (uint32_t)byte;
        for (uint32_t bit = 0; bit < 8; ++bit) {
            uint32_t low = crc & UINT32_C(1);
            crc >>= 1;
            if (low != 0u) {
                crc ^= UINT32_C(0xedb88320);
            }
        }
    }
    return crc ^ UINT32_C(0xffffffff);
}

static void fx_seal(uint8_t *data)
{
    fx_put_le32(data + 60u, fx_block_crc(data));
}

static void fx_header(uint8_t *data, uint32_t capacity)
{
    static const uint8_t magic[8] = {
        (uint8_t)'Y', (uint8_t)'A', (uint8_t)'N', (uint8_t)'F',
        (uint8_t)'S', (uint8_t)'0', (uint8_t)'1', 0u
    };
    for (uint32_t i = 0; i < 8u; ++i) {
        data[i] = magic[i];
    }
    fx_put_le32(data + 8u, YAN_FS_VERSION);
    fx_put_le32(data + 12u, YAN_FS_BLOCK_SIZE);
    fx_put_le32(data + 16u, capacity);
    fx_put_le32(data + 20u, YAN_FS_MAX_FILES);
    fx_seal(data);
}

static void fx_entry(uint8_t *data, uint32_t slot, const char *name,
                     uint32_t size_bytes, uint32_t start_block, uint32_t block_count)
{
    uint8_t *entry = data + YAN_FS_HEADER_SIZE + slot * YAN_FS_ENTRY_SIZE;
    for (uint32_t i = 0; i < YAN_FS_ENTRY_SIZE; ++i) {
        entry[i] = 0;
    }
    for (uint32_t i = 0; name[i] != '\0'; ++i) {
        entry[i] = (uint8_t)name[i];
    }
    fx_put_le32(entry + 32u, size_bytes);
    fx_put_le32(entry + 36u, start_block);
    fx_put_le32(entry + 40u, block_count);
}

static void fx_device(uint32_t capacity)
{
    memset(device.blocks, 0, sizeof device.blocks);
    device.capacity_blocks = capacity;
    fx_header(device.blocks[0], capacity);
}

/* ---------------------------------------------------------------- callbacks */

static void run_callback_probe(void)
{
    if (probe_fs == NULL || probe_ran) {
        return;
    }
    probe_ran = true;
    uint32_t cursor = 0;
    YanFsInfo info = {0};
    uint32_t read_bytes = 0xffffffffu;
    uint8_t byte = 0;
    probe_list_result = yan_fs_list(probe_fs, &cursor, &info);
    probe_stat_result = yan_fs_stat(probe_fs, "hello.txt", &info);
    probe_mount_result = yan_fs_mount(probe_fs);
    probe_unmount_result = yan_fs_unmount(probe_fs);
    probe_init_result = yan_fs_init(probe_fs, probe_fs->io);
    probe_read_result = yan_fs_read(probe_fs, "hello.txt", 0u, &byte, 1u, &read_bytes);
    /* 0021 state priority: while the instance is busy, an invalid count pointer
     * (NULL, or one aliasing the context) is still a later check, so both must
     * answer BUSY rather than INVALID. */
    static uint8_t fs_snapshot[sizeof(YanFs)];
    uint32_t io_before;
    memcpy(fs_snapshot, probe_fs, sizeof fs_snapshot);
    io_before = device.reads + device.writes;
    probe_read_null_result =
        yan_fs_read(probe_fs, "hello.txt", 0u, &byte, 1u, NULL);
    probe_read_null_fs_unchanged =
        memcmp(fs_snapshot, probe_fs, sizeof fs_snapshot) == 0;
    probe_read_null_io = (device.reads + device.writes) - io_before;

    memcpy(fs_snapshot, probe_fs, sizeof fs_snapshot);
    io_before = device.reads + device.writes;
    probe_read_alias_result =
        yan_fs_read(probe_fs, "hello.txt", 0u, &byte, 1u,
                    (uint32_t *)(void *)probe_fs->metadata);
    probe_read_alias_fs_unchanged =
        memcmp(fs_snapshot, probe_fs, sizeof fs_snapshot) == 0;
    probe_read_alias_io = (device.reads + device.writes) - io_before;
    probe_create_result = yan_fs_create(probe_fs, "probe", &byte, 1u);
    probe_remove_result = yan_fs_remove(probe_fs, "hello.txt");
    probe_cache_crc = yan_fs_metadata_crc(probe_fs->metadata);
    probe_cache_unpublished = probe_fs->state == YAN_FS_UNMOUNTED &&
                              probe_fs->capacity_blocks == 0u &&
                              region_is_zero(probe_fs->metadata, YAN_FS_BLOCK_SIZE);
}

static YanFsIoResult device_capacity(void *context, uint64_t *blocks)
{
    TestDevice *self = (TestDevice *)context;
    ++self->capacity_calls;
    if (self->capacity_status != YAN_FS_IO_OK) {
        return self->capacity_status;
    }
    *blocks = self->capacity_blocks;
    return YAN_FS_IO_OK;
}

static YanFsIoResult device_read_block(void *context, uint32_t lba, uint8_t out[4096])
{
    TestDevice *self = (TestDevice *)context;
    run_callback_probe();
    ++self->reads;
    self->last_read_lba = lba;
    if (self->read_status != YAN_FS_IO_OK) {
        return self->read_status;
    }
    if (self->read_fail_at != 0u && self->reads == self->read_fail_at) {
        return self->read_fail_code != YAN_FS_IO_OK ? self->read_fail_code
                                                    : YAN_FS_IO_ERROR;
    }
    if (lba >= TEST_DEVICE_BLOCKS) {
        return YAN_FS_IO_ERROR;
    }
    for (uint32_t i = 0; i < YAN_FS_BLOCK_SIZE; ++i) {
        out[i] = self->blocks[lba][i];
    }
    return YAN_FS_IO_OK;
}

static YanFsIoResult device_write_block(void *context, uint32_t lba,
                                        const uint8_t data[4096])
{
    TestDevice *self = (TestDevice *)context;
    run_callback_probe();
    ++self->writes;
    self->last_write_lba = lba;
    if (lba >= TEST_DEVICE_BLOCKS) {
        return YAN_FS_IO_ERROR;
    }
    bool fail = false;
    YanFsIoResult code = YAN_FS_IO_OK;
    uint32_t copy = YAN_FS_BLOCK_SIZE;
    if (self->write_status != YAN_FS_IO_OK) {
        fail = true;
        code = self->write_status;
        copy = self->write_partial;
    } else if (self->write_fail_at != 0u && self->writes == self->write_fail_at) {
        fail = true;
        code = self->write_fail_code != YAN_FS_IO_OK ? self->write_fail_code
                                                     : YAN_FS_IO_ERROR;
        copy = self->write_partial;
    }
    uint32_t limit = copy < YAN_FS_BLOCK_SIZE ? copy : YAN_FS_BLOCK_SIZE;
    for (uint32_t i = 0; i < limit; ++i) {
        self->blocks[lba][i] = data[i];
    }
    return fail ? code : YAN_FS_IO_OK;
}

static YanFsBlockIo fx_backend(void)
{
    YanFsBlockIo io;
    io.context = &device;
    io.capacity = device_capacity;
    io.read_block = device_read_block;
    io.write_block = device_write_block;
    return io;
}

/* ------------------------------------------------------------ test helpers */

static void init_fs(void)
{
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_init(&fs, fx_backend()));
}

static YanFsResult mount_fresh(void)
{
    memset(&fs, 0, sizeof fs);
    init_fs();
    return yan_fs_mount(&fs);
}

static void mount_empty(uint32_t capacity)
{
    fx_device(capacity);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, mount_fresh());
}

static void create_ok(const char *name, const uint8_t *bytes, uint32_t length)
{
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_create(&fs, name, bytes, length));
}

static const uint8_t *cache_entry(uint32_t slot)
{
    return fs.metadata + YAN_FS_HEADER_SIZE + slot * YAN_FS_ENTRY_SIZE;
}

static uint32_t cache_size(uint32_t slot)
{
    return fx_get_le32(cache_entry(slot) + 32u);
}

static uint32_t cache_start(uint32_t slot)
{
    return fx_get_le32(cache_entry(slot) + 36u);
}

static uint32_t cache_count(uint32_t slot)
{
    return fx_get_le32(cache_entry(slot) + 40u);
}

static bool cache_slot_empty(uint32_t slot)
{
    return region_is_zero(cache_entry(slot), YAN_FS_ENTRY_SIZE);
}

void setUp(void)
{
    memset(&device, 0, sizeof device);
    memset(&fs, 0, sizeof fs);
    memset(block, 0, sizeof block);
    memset(expected, 0, sizeof expected);
    memset(payload, 0, sizeof payload);
    memset(readback, 0, sizeof readback);
    memset(aligned_words, 0, sizeof aligned_words);
    memset(medium_before, 0, sizeof medium_before);
    memset(neighbour, 0, sizeof neighbour);
    memset(entry_before, 0, sizeof entry_before);
    device.capacity_blocks = TEST_DEVICE_BLOCKS;
    probe_fs = NULL;
    probe_ran = false;
    probe_list_result = YAN_FS_OK;
    probe_stat_result = YAN_FS_OK;
    probe_mount_result = YAN_FS_OK;
    probe_unmount_result = YAN_FS_OK;
    probe_init_result = YAN_FS_OK;
    probe_read_result = YAN_FS_OK;
    probe_create_result = YAN_FS_OK;
    probe_remove_result = YAN_FS_OK;
    probe_cache_crc = 0u;
    probe_cache_unpublished = false;
}

void tearDown(void)
{
}

/* ------------------------------------------------------------ format and CRC */

static void format_writes_header_and_empty_entries(void)
{
    memset(block, 0xa5, sizeof block);
    memset(expected, 0, sizeof expected);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_format_metadata(block, 16u));

    /* The magic and each header field, byte by byte and little-endian. */
    TEST_ASSERT_EQUAL_UINT8((uint8_t)'Y', block[0]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)'A', block[1]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)'N', block[2]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)'F', block[3]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)'S', block[4]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)'0', block[5]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)'1', block[6]);
    TEST_ASSERT_EQUAL_UINT8(0u, block[7]);
    TEST_ASSERT_EQUAL_UINT8(1u, block[8]);
    TEST_ASSERT_EQUAL_UINT8(0u, block[9]);
    TEST_ASSERT_EQUAL_UINT8(0u, block[10]);
    TEST_ASSERT_EQUAL_UINT8(0u, block[11]);
    TEST_ASSERT_EQUAL_UINT8(0x00u, block[12]);
    TEST_ASSERT_EQUAL_UINT8(0x10u, block[13]);
    TEST_ASSERT_EQUAL_UINT8(0x00u, block[14]);
    TEST_ASSERT_EQUAL_UINT8(0x00u, block[15]);
    TEST_ASSERT_EQUAL_UINT8(0x10u, block[16]);
    TEST_ASSERT_EQUAL_UINT8(0x00u, block[17]);
    TEST_ASSERT_EQUAL_UINT8(0x00u, block[18]);
    TEST_ASSERT_EQUAL_UINT8(0x00u, block[19]);
    TEST_ASSERT_EQUAL_UINT8(63u, block[20]);
    TEST_ASSERT_EQUAL_UINT8(0x00u, block[21]);
    TEST_ASSERT_EQUAL_UINT8(0x00u, block[22]);
    TEST_ASSERT_EQUAL_UINT8(0x00u, block[23]);
    TEST_ASSERT_TRUE_MESSAGE(region_is_byte(block + 24u, 36u, 0u),
                             "reserved header bytes must be zero");
    TEST_ASSERT_TRUE_MESSAGE(
        region_is_zero(block + YAN_FS_HEADER_SIZE,
                       YAN_FS_BLOCK_SIZE - YAN_FS_HEADER_SIZE),
        "every directory slot must start empty");

    /* The whole block must equal the fixture encoder's output, including CRC. */
    fx_header(expected, 16u);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(
        expected, block, YAN_FS_BLOCK_SIZE,
        "format must match the independently encoded empty directory");
}

static void format_encodes_capacity_little_endian(void)
{
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_OK, yan_fs_format_metadata(block, UINT32_C(0x01020304)));
    TEST_ASSERT_EQUAL_UINT8(0x04u, block[16]);
    TEST_ASSERT_EQUAL_UINT8(0x03u, block[17]);
    TEST_ASSERT_EQUAL_UINT8(0x02u, block[18]);
    TEST_ASSERT_EQUAL_UINT8(0x01u, block[19]);
    TEST_ASSERT_EQUAL_UINT32(UINT32_C(0x01020304), fx_get_le32(block + 16u));
    TEST_ASSERT_EQUAL_UINT32(YAN_FS_VERSION, fx_get_le32(block + 8u));
    TEST_ASSERT_EQUAL_UINT32(YAN_FS_BLOCK_SIZE, fx_get_le32(block + 12u));
    TEST_ASSERT_EQUAL_UINT32(YAN_FS_MAX_FILES, fx_get_le32(block + 20u));
    TEST_ASSERT_EQUAL_UINT32(fx_block_crc(block), fx_get_le32(block + 60u));
}

static void format_rejects_invalid_arguments_without_writing(void)
{
    memset(block, 0xa5, sizeof block);
    TEST_ASSERT_EQUAL_INT(YAN_FS_INVALID, yan_fs_format_metadata(block, 0u));
    TEST_ASSERT_TRUE_MESSAGE(region_is_byte(block, YAN_FS_BLOCK_SIZE, 0xa5),
                             "a rejected format argument must not write output");
    TEST_ASSERT_EQUAL_INT(YAN_FS_INVALID, yan_fs_format_metadata(NULL, 1u));

    /* Both ends of the accepted range produce a sealed block. */
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_format_metadata(block, 1u));
    TEST_ASSERT_EQUAL_UINT32(1u, fx_get_le32(block + 16u));
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_format_metadata(block, UINT32_MAX));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, fx_get_le32(block + 16u));
    TEST_ASSERT_EQUAL_UINT32(fx_block_crc(block), fx_get_le32(block + 60u));
}

static void metadata_crc_matches_known_vector_and_ignores_crc_field(void)
{
    static const uint8_t digits[] = "123456789";
    TEST_ASSERT_EQUAL_UINT32(UINT32_C(0xcbf43926), fx_crc32_bytes(digits, 9u));

    fx_device(16u);
    uint32_t stored = yan_fs_metadata_crc(device.blocks[0]);
    TEST_ASSERT_EQUAL_UINT32(fx_block_crc(device.blocks[0]), stored);
    TEST_ASSERT_EQUAL_UINT32(stored, fx_get_le32(device.blocks[0] + 60u));

    device.blocks[0][4000] ^= 0x01u;
    TEST_ASSERT_TRUE_MESSAGE(yan_fs_metadata_crc(device.blocks[0]) != stored,
                             "a changed data byte must change the CRC");

    /* The stored CRC field is not part of its own input. */
    fx_device(16u);
    stored = yan_fs_metadata_crc(device.blocks[0]);
    for (uint32_t i = 60u; i < 64u; ++i) {
        device.blocks[0][i] = (uint8_t)(0x5au + i);
    }
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        stored, yan_fs_metadata_crc(device.blocks[0]),
        "the CRC field itself is excluded from the computation");
}

/* ------------------------------------------------------------------- mount */

static void mount_publishes_validated_cache_and_stat_reads_it(void)
{
    fx_device(16u);
    fx_entry(device.blocks[0], 0u, "hello.txt", 5u, 1u, 1u);
    fx_entry(device.blocks[0], 1u, "empty", 0u, 0u, 0u);
    fx_seal(device.blocks[0]);

    init_fs();
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_mount(&fs));
    TEST_ASSERT_EQUAL_INT(YAN_FS_MOUNTED, fs.state);
    TEST_ASSERT_EQUAL_UINT32(16u, fs.capacity_blocks);
    TEST_ASSERT_EQUAL_UINT32(1u, device.reads);
    TEST_ASSERT_EQUAL_UINT32(0u, device.writes);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(device.blocks[0], fs.metadata,
                                     YAN_FS_BLOCK_SIZE,
                                     "mount must publish the validated block");

    YanFsInfo info = {0};
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_stat(&fs, "hello.txt", &info));
    TEST_ASSERT_EQUAL_STRING("hello.txt", info.name);
    TEST_ASSERT_EQUAL_UINT32(5u, info.size_bytes);
    info = (YanFsInfo){0};
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_stat(&fs, "empty", &info));
    TEST_ASSERT_EQUAL_STRING("empty", info.name);
    TEST_ASSERT_EQUAL_UINT32(0u, info.size_bytes);
}

static void mount_accepts_capacity_one_empty_directory(void)
{
    fx_device(1u);
    init_fs();
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_mount(&fs));
    TEST_ASSERT_EQUAL_UINT32(1u, fs.capacity_blocks);
    uint32_t cursor = 0;
    YanFsInfo info = {0};
    TEST_ASSERT_EQUAL_INT(YAN_FS_END, yan_fs_list(&fs, &cursor, &info));
    TEST_ASSERT_EQUAL_UINT32(63u, cursor);
}

static void mount_rejects_zero_and_oversized_device_capacity(void)
{
    fx_device(16u);

    device.capacity_blocks = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_UNSUPPORTED, mount_fresh());
    TEST_ASSERT_EQUAL_INT(YAN_FS_UNMOUNTED, fs.state);

    device.capacity_blocks = UINT64_C(0x100000000);
    TEST_ASSERT_EQUAL_INT(YAN_FS_UNSUPPORTED, mount_fresh());
    TEST_ASSERT_EQUAL_INT(YAN_FS_UNMOUNTED, fs.state);

    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, device.reads,
        "capacity is checked before block 0 is read");
    TEST_ASSERT_TRUE(region_is_zero(fs.metadata, YAN_FS_BLOCK_SIZE));
    TEST_ASSERT_EQUAL_UINT32(0u, device.writes);
}

static void mount_rejects_unknown_version_as_unsupported(void)
{
    fx_device(16u);
    fx_put_le32(device.blocks[0] + 8u, 2u);
    fx_seal(device.blocks[0]);
    TEST_ASSERT_EQUAL_INT(YAN_FS_UNSUPPORTED, mount_fresh());
    TEST_ASSERT_EQUAL_INT(YAN_FS_UNMOUNTED, fs.state);
    TEST_ASSERT_TRUE(region_is_zero(fs.metadata, YAN_FS_BLOCK_SIZE));
    TEST_ASSERT_EQUAL_UINT32(0u, device.writes);

    fx_device(16u);
    fx_put_le32(device.blocks[0] + 8u, 0u);
    fx_seal(device.blocks[0]);
    TEST_ASSERT_EQUAL_INT(YAN_FS_UNSUPPORTED, mount_fresh());
}

static void mount_rejects_capacity_mismatch(void)
{
    fx_device(16u);
    fx_put_le32(device.blocks[0] + 16u, 15u);
    fx_seal(device.blocks[0]);
    TEST_ASSERT_EQUAL_INT(YAN_FS_CORRUPT, mount_fresh());
    TEST_ASSERT_EQUAL_INT(YAN_FS_UNMOUNTED, fs.state);
}

static void mount_rejects_wrong_crc_unknown_magic_and_wrong_sizes(void)
{
    /* A correct layout with a deliberately wrong CRC. */
    fx_device(16u);
    fx_put_le32(device.blocks[0] + 60u, fx_block_crc(device.blocks[0]) ^ 1u);
    TEST_ASSERT_EQUAL_INT(YAN_FS_CORRUPT, mount_fresh());

    /* A bad magic with a CRC that really covers it. */
    fx_device(16u);
    device.blocks[0][0] = (uint8_t)'X';
    fx_seal(device.blocks[0]);
    TEST_ASSERT_EQUAL_INT(YAN_FS_CORRUPT, mount_fresh());

    fx_device(16u);
    fx_put_le32(device.blocks[0] + 12u, 512u);
    fx_seal(device.blocks[0]);
    TEST_ASSERT_EQUAL_INT(YAN_FS_CORRUPT, mount_fresh());

    fx_device(16u);
    fx_put_le32(device.blocks[0] + 20u, 62u);
    fx_seal(device.blocks[0]);
    TEST_ASSERT_EQUAL_INT(YAN_FS_CORRUPT, mount_fresh());
}

static void mount_rejects_nonzero_reserved_bytes(void)
{
    fx_device(16u);
    device.blocks[0][24] = 1u;
    fx_seal(device.blocks[0]);
    TEST_ASSERT_EQUAL_INT(YAN_FS_CORRUPT, mount_fresh());

    fx_device(16u);
    device.blocks[0][59] = 1u;
    fx_seal(device.blocks[0]);
    TEST_ASSERT_EQUAL_INT(YAN_FS_CORRUPT, mount_fresh());

    /* Directory-entry reserved bytes 44..63 must be zero as well. Both a
     * single entry with a real extent and no writes are checked. */
    fx_device(16u);
    fx_entry(device.blocks[0], 0u, "e", 1u, 1u, 1u);
    device.blocks[0][YAN_FS_HEADER_SIZE + 44u] = 1u;
    fx_seal(device.blocks[0]);
    snapshot_medium();
    uint32_t writes = device.writes;
    TEST_ASSERT_EQUAL_INT(YAN_FS_CORRUPT, mount_fresh());
    TEST_ASSERT_EQUAL_UINT32(writes, device.writes);
    TEST_ASSERT_TRUE(medium_equal_snapshot());

    fx_device(16u);
    fx_entry(device.blocks[0], 0u, "e", 1u, 1u, 1u);
    device.blocks[0][YAN_FS_HEADER_SIZE + 63u] = 1u;
    fx_seal(device.blocks[0]);
    TEST_ASSERT_EQUAL_INT(YAN_FS_CORRUPT, mount_fresh());
}

static void mount_rejects_name_without_terminator(void)
{
    fx_device(16u);
    for (uint32_t i = 0; i < 32u; ++i) {
        device.blocks[0][YAN_FS_HEADER_SIZE + i] = (uint8_t)'a';
    }
    fx_seal(device.blocks[0]);
    TEST_ASSERT_EQUAL_INT(YAN_FS_CORRUPT, mount_fresh());
}

static void mount_rejects_name_padding_and_illegal_characters(void)
{
    /* "abc" is terminated at index 3; a byte after it is padding damage. */
    fx_device(16u);
    fx_entry(device.blocks[0], 0u, "abc", 0u, 0u, 0u);
    device.blocks[0][YAN_FS_HEADER_SIZE + 4u] = (uint8_t)'x';
    fx_seal(device.blocks[0]);
    TEST_ASSERT_EQUAL_INT(YAN_FS_CORRUPT, mount_fresh());

    fx_device(16u);
    fx_entry(device.blocks[0], 0u, "a/b", 0u, 0u, 0u);
    fx_seal(device.blocks[0]);
    TEST_ASSERT_EQUAL_INT(YAN_FS_CORRUPT, mount_fresh());
}

static void mount_rejects_dot_names_and_duplicate_names(void)
{
    fx_device(16u);
    fx_entry(device.blocks[0], 0u, ".", 0u, 0u, 0u);
    fx_seal(device.blocks[0]);
    TEST_ASSERT_EQUAL_INT(YAN_FS_CORRUPT, mount_fresh());

    fx_device(16u);
    fx_entry(device.blocks[0], 0u, "..", 0u, 0u, 0u);
    fx_seal(device.blocks[0]);
    TEST_ASSERT_EQUAL_INT(YAN_FS_CORRUPT, mount_fresh());

    fx_device(16u);
    fx_entry(device.blocks[0], 0u, "same", 0u, 0u, 0u);
    fx_entry(device.blocks[0], 3u, "same", 0u, 0u, 0u);
    fx_seal(device.blocks[0]);
    TEST_ASSERT_EQUAL_INT(YAN_FS_CORRUPT, mount_fresh());
}

static void mount_rejects_nonzero_empty_slot(void)
{
    /* A zero first name byte with anything else set is damage, not a free
     * slot. */
    fx_device(16u);
    fx_put_le32(device.blocks[0] + YAN_FS_HEADER_SIZE + 32u, 1u);
    fx_seal(device.blocks[0]);
    TEST_ASSERT_EQUAL_INT(YAN_FS_CORRUPT, mount_fresh());
}

static void mount_rejects_empty_file_with_extent(void)
{
    fx_device(16u);
    fx_entry(device.blocks[0], 0u, "e", 0u, 1u, 1u);
    fx_seal(device.blocks[0]);
    TEST_ASSERT_EQUAL_INT(YAN_FS_CORRUPT, mount_fresh());

    fx_device(16u);
    fx_entry(device.blocks[0], 0u, "e", 0u, 0u, 1u);
    fx_seal(device.blocks[0]);
    TEST_ASSERT_EQUAL_INT(YAN_FS_CORRUPT, mount_fresh());

    fx_device(16u);
    fx_entry(device.blocks[0], 0u, "e", 0u, 1u, 0u);
    fx_seal(device.blocks[0]);
    TEST_ASSERT_EQUAL_INT(YAN_FS_CORRUPT, mount_fresh());
}

static void mount_rejects_size_count_mismatch(void)
{
    /* 5000 bytes need two blocks, not one. */
    fx_device(16u);
    fx_entry(device.blocks[0], 0u, "short", 5000u, 1u, 1u);
    fx_seal(device.blocks[0]);
    TEST_ASSERT_EQUAL_INT(YAN_FS_CORRUPT, mount_fresh());

    /* One byte needs one block, not two. */
    fx_device(16u);
    fx_entry(device.blocks[0], 0u, "long", 1u, 1u, 2u);
    fx_seal(device.blocks[0]);
    TEST_ASSERT_EQUAL_INT(YAN_FS_CORRUPT, mount_fresh());
}

static void mount_rejects_out_of_range_and_overlapping_extents(void)
{
    fx_device(16u);
    fx_entry(device.blocks[0], 0u, "zero-start", 1u, 0u, 1u);
    fx_seal(device.blocks[0]);
    TEST_ASSERT_EQUAL_INT(YAN_FS_CORRUPT, mount_fresh());

    /* start == capacity leaves no room, because count is at least one. */
    fx_device(16u);
    fx_entry(device.blocks[0], 0u, "past-end", 1u, 16u, 1u);
    fx_seal(device.blocks[0]);
    TEST_ASSERT_EQUAL_INT(YAN_FS_CORRUPT, mount_fresh());

    /* 15 + 2 spills past the end; checked without wrapping. */
    fx_device(16u);
    fx_entry(device.blocks[0], 0u, "spill", 5000u, 15u, 2u);
    fx_seal(device.blocks[0]);
    TEST_ASSERT_EQUAL_INT(YAN_FS_CORRUPT, mount_fresh());

    fx_device(16u);
    fx_entry(device.blocks[0], 0u, "one", 1u, 1u, 1u);
    fx_entry(device.blocks[0], 1u, "two", 1u, 1u, 1u);
    fx_seal(device.blocks[0]);
    TEST_ASSERT_EQUAL_INT(YAN_FS_CORRUPT, mount_fresh());
}

static void mount_checks_partial_extent_overlap_and_adjacency(void)
{
    /* [1,3) and [2,4) overlap by one block: rejected, and mounting writes
     * nothing. */
    fx_device(16u);
    fx_entry(device.blocks[0], 0u, "a", 8192u, 1u, 2u);
    fx_entry(device.blocks[0], 1u, "b", 8192u, 2u, 2u);
    fx_seal(device.blocks[0]);
    snapshot_medium();
    uint32_t writes = device.writes;
    TEST_ASSERT_EQUAL_INT(YAN_FS_CORRUPT, mount_fresh());
    TEST_ASSERT_EQUAL_UINT32(writes, device.writes);
    TEST_ASSERT_TRUE(medium_equal_snapshot());

    /* [1,3) and [3,5) only touch at the boundary: accepted. */
    fx_device(16u);
    fx_entry(device.blocks[0], 0u, "a", 8192u, 1u, 2u);
    fx_entry(device.blocks[0], 1u, "b", 8192u, 3u, 2u);
    fx_seal(device.blocks[0]);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, mount_fresh());
    YanFsInfo info = {0};
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_stat(&fs, "a", &info));
    TEST_ASSERT_EQUAL_UINT32(8192u, info.size_bytes);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_stat(&fs, "b", &info));
    TEST_ASSERT_EQUAL_UINT32(8192u, info.size_bytes);
}

static void mount_accepts_size_rounding_and_last_block_extent(void)
{
    fx_device(16u);
    fx_entry(device.blocks[0], 0u, "exact", 4096u, 1u, 1u);
    fx_entry(device.blocks[0], 1u, "ceil", 4097u, 2u, 2u);
    fx_entry(device.blocks[0], 2u, "last", 1u, 15u, 1u);
    fx_seal(device.blocks[0]);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, mount_fresh());
    YanFsInfo info = {0};
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_stat(&fs, "ceil", &info));
    TEST_ASSERT_EQUAL_UINT32(4097u, info.size_bytes);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_stat(&fs, "last", &info));
    TEST_ASSERT_EQUAL_UINT32(1u, info.size_bytes);
}

static void mount_reports_capacity_io_error_and_faults(void)
{
    fx_device(16u);
    device.capacity_status = YAN_FS_IO_ERROR;
    init_fs();
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO, yan_fs_mount(&fs));
    TEST_ASSERT_EQUAL_INT(YAN_FS_STATE_FAULTED, fs.state);

    uint32_t cursor = 0;
    YanFsInfo info = {0};
    info.size_bytes = 0xdeadbeefu;
    TEST_ASSERT_EQUAL_INT(YAN_FS_FAULTED, yan_fs_list(&fs, &cursor, &info));
    TEST_ASSERT_EQUAL_INT(YAN_FS_FAULTED, yan_fs_stat(&fs, "x", &info));
    TEST_ASSERT_EQUAL_INT(YAN_FS_FAULTED, yan_fs_mount(&fs));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0xdeadbeefu, info.size_bytes,
        "a faulted instance must not modify caller outputs");

    /* unmount is the only way out; the backend failure itself is the test's to
     * clear, which no Guest unmount can do for a real one. */
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_unmount(&fs));
    TEST_ASSERT_EQUAL_INT(YAN_FS_UNMOUNTED, fs.state);
    device.capacity_status = YAN_FS_IO_OK;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_mount(&fs));
    TEST_ASSERT_EQUAL_INT(YAN_FS_MOUNTED, fs.state);
}

static void mount_reports_read_protocol_and_unknown_status_as_protocol(void)
{
    fx_device(16u);

    device.read_status = YAN_FS_IO_PROTOCOL;
    TEST_ASSERT_EQUAL_INT(YAN_FS_PROTOCOL, mount_fresh());
    TEST_ASSERT_EQUAL_INT(YAN_FS_STATE_FAULTED, fs.state);

    device.read_status = (YanFsIoResult)9;
    TEST_ASSERT_EQUAL_INT(YAN_FS_PROTOCOL, mount_fresh());
    TEST_ASSERT_EQUAL_INT(YAN_FS_STATE_FAULTED, fs.state);

    device.read_status = YAN_FS_IO_ERROR;
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO, mount_fresh());
    TEST_ASSERT_EQUAL_INT(YAN_FS_STATE_FAULTED, fs.state);

    device.capacity_status = (YanFsIoResult)200;
    TEST_ASSERT_EQUAL_INT(YAN_FS_PROTOCOL, mount_fresh());
    TEST_ASSERT_EQUAL_INT(YAN_FS_STATE_FAULTED, fs.state);
}

static void mount_never_writes_and_publishes_cache_only_after_validation(void)
{
    fx_device(16u);
    fx_entry(device.blocks[0], 0u, "hello.txt", 5u, 1u, 1u);
    fx_seal(device.blocks[0]);
    init_fs();
    probe_fs = &fs;
    probe_ran = false;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_mount(&fs));
    TEST_ASSERT_TRUE(probe_ran);
    TEST_ASSERT_TRUE_MESSAGE(probe_cache_unpublished,
                             "no cache may be visible while mount reads");
    TEST_ASSERT_EQUAL_UINT32(0u, device.writes);
    TEST_ASSERT_EQUAL_MEMORY(device.blocks[0], fs.metadata, YAN_FS_BLOCK_SIZE);

    /* A corrupt image is read into scratch, rejected, and never published. */
    fx_device(16u);
    fx_put_le32(device.blocks[0] + 60u, fx_block_crc(device.blocks[0]) ^ 1u);
    probe_fs = &fs;
    probe_ran = false;
    TEST_ASSERT_EQUAL_INT(YAN_FS_CORRUPT, mount_fresh());
    TEST_ASSERT_TRUE(probe_ran);
    TEST_ASSERT_TRUE(probe_cache_unpublished);
    TEST_ASSERT_EQUAL_UINT32(0u, device.writes);
    TEST_ASSERT_TRUE(region_is_zero(fs.metadata, YAN_FS_BLOCK_SIZE));
}

/* -------------------------------------------------------------------- list */

static void list_returns_slots_in_order_then_end(void)
{
    fx_device(16u);
    fx_entry(device.blocks[0], 0u, "a", 4096u, 1u, 1u);
    fx_entry(device.blocks[0], 2u, "b", 4097u, 2u, 2u);
    fx_entry(device.blocks[0], 5u, "c", 1u, 15u, 1u);
    fx_seal(device.blocks[0]);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, mount_fresh());

    uint32_t cursor = 0;
    YanFsInfo info = {0};
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_list(&fs, &cursor, &info));
    TEST_ASSERT_EQUAL_UINT32(1u, cursor);
    TEST_ASSERT_EQUAL_STRING("a", info.name);
    TEST_ASSERT_EQUAL_UINT32(4096u, info.size_bytes);

    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_list(&fs, &cursor, &info));
    TEST_ASSERT_EQUAL_UINT32(3u, cursor);
    TEST_ASSERT_EQUAL_STRING("b", info.name);
    TEST_ASSERT_EQUAL_UINT32(4097u, info.size_bytes);

    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_list(&fs, &cursor, &info));
    TEST_ASSERT_EQUAL_UINT32(6u, cursor);
    TEST_ASSERT_EQUAL_STRING("c", info.name);
    TEST_ASSERT_EQUAL_UINT32(1u, info.size_bytes);

    TEST_ASSERT_EQUAL_INT(YAN_FS_END, yan_fs_list(&fs, &cursor, &info));
    TEST_ASSERT_EQUAL_UINT32(63u, cursor);
}

static void list_rejects_cursor_beyond_last_slot(void)
{
    fx_device(16u);
    fx_entry(device.blocks[0], 0u, "only", 0u, 0u, 0u);
    fx_seal(device.blocks[0]);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, mount_fresh());

    uint32_t cursor = 64u;
    YanFsInfo info = {0};
    info.size_bytes = 0xabcdu;
    TEST_ASSERT_EQUAL_INT(YAN_FS_INVALID, yan_fs_list(&fs, &cursor, &info));
    TEST_ASSERT_EQUAL_UINT32(64u, cursor);
    TEST_ASSERT_EQUAL_UINT32(0xabcdu, info.size_bytes);

    cursor = 63u;
    info.size_bytes = 0xabcdu;
    TEST_ASSERT_EQUAL_INT(YAN_FS_END, yan_fs_list(&fs, &cursor, &info));
    TEST_ASSERT_EQUAL_UINT32(63u, cursor);
    TEST_ASSERT_EQUAL_UINT32(0xabcdu, info.size_bytes);

    cursor = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_list(&fs, &cursor, &info));
    TEST_ASSERT_EQUAL_UINT32(1u, cursor);
    TEST_ASSERT_EQUAL_INT(YAN_FS_END, yan_fs_list(&fs, &cursor, &info));
    TEST_ASSERT_EQUAL_UINT32(63u, cursor);

    TEST_ASSERT_EQUAL_INT(YAN_FS_INVALID, yan_fs_list(&fs, NULL, &info));
    TEST_ASSERT_EQUAL_INT(YAN_FS_INVALID, yan_fs_list(&fs, &cursor, NULL));
}

static void unmounted_and_uninitialized_instances_reject_list_and_stat(void)
{
    init_fs();
    uint32_t cursor = 5u;
    YanFsInfo info = {0};
    info.size_bytes = 0xfeedu;
    TEST_ASSERT_EQUAL_INT(YAN_FS_NOT_MOUNTED, yan_fs_list(&fs, &cursor, &info));
    TEST_ASSERT_EQUAL_UINT32(5u, cursor);
    TEST_ASSERT_EQUAL_UINT32(0xfeedu, info.size_bytes);
    TEST_ASSERT_EQUAL_INT(YAN_FS_NOT_MOUNTED, yan_fs_stat(&fs, "any", &info));
    TEST_ASSERT_EQUAL_UINT32(0xfeedu, info.size_bytes);

    memset(&fs, 0, sizeof fs);
    TEST_ASSERT_EQUAL_INT(YAN_FS_INVALID, yan_fs_list(&fs, &cursor, &info));
    TEST_ASSERT_EQUAL_INT(YAN_FS_INVALID, yan_fs_stat(&fs, "any", &info));
    TEST_ASSERT_EQUAL_INT(YAN_FS_INVALID, yan_fs_mount(&fs));
    TEST_ASSERT_EQUAL_INT(YAN_FS_INVALID, yan_fs_unmount(&fs));
    TEST_ASSERT_EQUAL_UINT32(5u, cursor);
}

static void stat_reports_not_found_and_rejects_invalid_names(void)
{
    fx_device(16u);
    fx_entry(device.blocks[0], 0u, "hello.txt", 5u, 1u, 1u);
    fx_seal(device.blocks[0]);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, mount_fresh());

    YanFsInfo info = {0};
    info.size_bytes = 0xfeedfaceu;
    TEST_ASSERT_EQUAL_INT(YAN_FS_NOT_FOUND, yan_fs_stat(&fs, "missing", &info));
    TEST_ASSERT_EQUAL_UINT32(0xfeedfaceu, info.size_bytes);
    TEST_ASSERT_EQUAL_INT(YAN_FS_INVALID, yan_fs_stat(&fs, NULL, &info));
    TEST_ASSERT_EQUAL_INT(YAN_FS_INVALID, yan_fs_stat(&fs, "hello.txt", NULL));
    TEST_ASSERT_EQUAL_INT(YAN_FS_INVALID, yan_fs_stat(&fs, "", &info));
    TEST_ASSERT_EQUAL_INT(YAN_FS_INVALID, yan_fs_stat(&fs, ".", &info));
    TEST_ASSERT_EQUAL_INT(YAN_FS_INVALID, yan_fs_stat(&fs, "..", &info));
    TEST_ASSERT_EQUAL_INT(YAN_FS_INVALID, yan_fs_stat(&fs, "a/b", &info));
    TEST_ASSERT_EQUAL_INT(YAN_FS_INVALID, yan_fs_stat(&fs, "hello world", &info));
    /* 32 bytes of legal characters is one past the limit, with no terminator
     * inside the name area. */
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_INVALID,
        yan_fs_stat(&fs, "0123456789abcdef0123456789abcdef", &info));
}

static void stat_is_case_sensitive_and_accepts_names_at_the_length_limit(void)
{
    static const char longest[] = "0123456789abcdef0123456789abcde"; /* 31 */
    TEST_ASSERT_EQUAL_UINT32(31u, (uint32_t)strlen(longest));

    fx_device(16u);
    fx_entry(device.blocks[0], 0u, "A", 0u, 0u, 0u);
    fx_entry(device.blocks[0], 1u, "a", 0u, 0u, 0u);
    fx_entry(device.blocks[0], 2u, longest, 0u, 0u, 0u);
    fx_seal(device.blocks[0]);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, mount_fresh());

    YanFsInfo info = {0};
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_stat(&fs, "A", &info));
    TEST_ASSERT_EQUAL_STRING("A", info.name);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_stat(&fs, "a", &info));
    TEST_ASSERT_EQUAL_STRING("a", info.name);
    TEST_ASSERT_EQUAL_INT(YAN_FS_NOT_FOUND, yan_fs_stat(&fs, "HELLO", &info));
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_stat(&fs, longest, &info));
    TEST_ASSERT_EQUAL_STRING(longest, info.name);
}

/* ------------------------------------------------------------------- alias */

static void list_rejects_cursor_and_out_aliasing_context(void)
{
    mount_empty(16u);
    uint32_t cursor = 0u;
    YanFsInfo info = {0};
    uint32_t before = yan_fs_metadata_crc(fs.metadata);
    uint32_t scratch_before = yan_fs_metadata_crc(fs.scratch);

    /* The exact regression the independent probe reported: a cursor inside the
     * reserved header bytes used to be written, corrupting the cache. */
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_INVALID,
        yan_fs_list(&fs, (uint32_t *)(void *)(fs.metadata + 24u), &info));
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_INVALID, yan_fs_list(&fs, (uint32_t *)(void *)&fs, &info));
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_INVALID,
        yan_fs_list(&fs, (uint32_t *)(void *)(fs.scratch + 100u), &info));
    /* A range whose end wraps must be rejected without being dereferenced. */
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_INVALID,
        yan_fs_list(&fs, (uint32_t *)(uintptr_t)(UINTPTR_MAX - 1u), &info));
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_INVALID,
        yan_fs_list(&fs, &cursor, (YanFsInfo *)(void *)fs.metadata));
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_INVALID,
        yan_fs_list(&fs, &cursor, (YanFsInfo *)(void *)(fs.scratch + 4080u)));

    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, cursor, "an aliasing list call must not update the caller cursor");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        before, yan_fs_metadata_crc(fs.metadata),
        "an aliasing list cursor must not touch the metadata cache");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        scratch_before, yan_fs_metadata_crc(fs.scratch),
        "an aliasing list call must not touch the scratch block");
}

static void stat_rejects_name_and_out_aliasing_context(void)
{
    mount_empty(16u);
    create_ok("hello", NULL, 0u);
    uint32_t before = yan_fs_metadata_crc(fs.metadata);
    YanFsInfo info = {0};

    TEST_ASSERT_EQUAL_INT(
        YAN_FS_INVALID,
        yan_fs_stat(&fs, (const char *)(const void *)fs.metadata, &info));
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_INVALID,
        yan_fs_stat(&fs, (const char *)(const void *)(fs.metadata + 64u), &info));
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_INVALID,
        yan_fs_stat(&fs, "hello", (YanFsInfo *)(void *)fs.metadata));
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_INVALID,
        yan_fs_stat(&fs, "hello", (YanFsInfo *)(void *)(fs.scratch + 4080u)));

    TEST_ASSERT_EQUAL_UINT32(before, yan_fs_metadata_crc(fs.metadata));
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_stat(&fs, "hello", &info));
}

static void list_rejects_cursor_overlapping_info(void)
{
    mount_empty(16u);
    create_ok("only", NULL, 0u);
    uint32_t cursor = 7u;
    YanFsInfo info = {0};
    uint32_t reads_before = device.reads;

    /* cursor points at info.size_bytes, so the four cursor bytes lie inside the
     * whole YanFsInfo output range. */
    info.size_bytes = 0x1234u;
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_INVALID,
        yan_fs_list(&fs, (uint32_t *)(void *)&info.size_bytes, &info));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0x1234u, info.size_bytes, "an overlapping list must not modify info");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        7u, cursor, "the disjoint caller cursor must not be touched either");

    /* cursor points inside info.name. */
    memset(&info, 0, sizeof info);
    info.size_bytes = 0x1234u;
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_INVALID, yan_fs_list(&fs, (uint32_t *)(void *)info.name, &info));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0x1234u, info.size_bytes, "an overlapping list must not modify info");

    /* Disjoint cursor and info behave normally. */
    cursor = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_list(&fs, &cursor, &info));
    TEST_ASSERT_EQUAL_UINT32(1u, cursor);
    TEST_ASSERT_EQUAL_STRING("only", info.name);

    TEST_ASSERT_EQUAL_UINT32_MESSAGE(reads_before, device.reads,
                                     "list never calls the device");
}

static void output_ranges_that_exceed_uintptr_are_rejected(void)
{
    mount_empty(16u);
    create_ok("f", payload, 5u);
    uint32_t cursor = 9u;
    YanFsInfo info = {0};
    info.size_bytes = 0x55u;
    uint32_t got = 0xffffffffu;
    uint32_t reads_before = device.reads;

    /* Each range would run past UINTPTR_MAX. The guard has to reject it in the
     * uintptr domain; a widened uint64_t sum would have looked fine on a 32-bit
     * Guest. */
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_INVALID,
        yan_fs_list(&fs, (uint32_t *)(uintptr_t)(UINTPTR_MAX - 2u), &info));
    TEST_ASSERT_EQUAL_UINT32(0x55u, info.size_bytes);
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_INVALID,
        yan_fs_list(&fs, &cursor, (YanFsInfo *)(uintptr_t)(UINTPTR_MAX - 4u)));
    TEST_ASSERT_EQUAL_UINT32(9u, cursor);
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_INVALID,
        yan_fs_read(&fs, "f", 0u, readback, 5u,
                    (uint32_t *)(uintptr_t)(UINTPTR_MAX - 1u)));
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_INVALID,
        yan_fs_read(&fs, "f", 0u, (uint8_t *)(uintptr_t)(UINTPTR_MAX - 4u), 8u, &got));
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_INVALID,
        yan_fs_create(&fs, "g", (const uint8_t *)(uintptr_t)(UINTPTR_MAX - 4u), 8u));

    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        reads_before, device.reads,
        "overflowing ranges are rejected before any device callback");
}

static void read_rejects_read_bytes_and_out_aliasing_context(void)
{
    mount_empty(16u);
    fill_payload(5u, 0x30);
    create_ok("hello", payload, 5u);
    uint32_t before = yan_fs_metadata_crc(fs.metadata);
    uint32_t got = 0xffffffffu;

    /* An invalid read_bytes returns INVALID and must not be written. */
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_INVALID,
        yan_fs_read(&fs, "hello", 0u, readback, 5u, (uint32_t *)(void *)fs.metadata));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0xffffffffu, got, "read_bytes itself is not written by an earlier alias");
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_INVALID,
        yan_fs_read(&fs, "hello", 0u, readback, 5u,
                    (uint32_t *)(uintptr_t)(UINTPTR_MAX - 1u)));
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_INVALID,
        yan_fs_read(&fs, "hello", 0u, (uint8_t *)(void *)fs.metadata, 5u, &got));
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_INVALID, yan_fs_read(&fs, "hello", 0u, readback, 5u, NULL));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        before, yan_fs_metadata_crc(fs.metadata),
        "an aliasing read must not touch the metadata cache");
}

static void create_replace_remove_reject_name_and_bytes_aliasing_context(void)
{
    mount_empty(16u);
    uint32_t before = yan_fs_metadata_crc(fs.metadata);
    uint32_t writes = device.writes;

    TEST_ASSERT_EQUAL_INT(
        YAN_FS_INVALID,
        yan_fs_create(&fs, (const char *)(const void *)fs.metadata, payload, 1u));
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_INVALID,
        yan_fs_create(&fs, "x", (const uint8_t *)(const void *)fs.scratch, 4u));
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_INVALID,
        yan_fs_create(&fs, "x", (const uint8_t *)(const void *)(fs.metadata + 10u), 4u));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        writes, device.writes, "an aliasing create must perform no medium write");
    TEST_ASSERT_EQUAL_UINT32(before, yan_fs_metadata_crc(fs.metadata));

    create_ok("x", payload, 4u);
    before = yan_fs_metadata_crc(fs.metadata);
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_INVALID,
        yan_fs_replace(&fs, "x", (const uint8_t *)(const void *)fs.metadata, 4u));
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_INVALID, yan_fs_remove(&fs, (const char *)(const void *)fs.metadata));
    TEST_ASSERT_EQUAL_UINT32(before, yan_fs_metadata_crc(fs.metadata));
    TEST_ASSERT_EQUAL_UINT32(4u, cache_size(0u));
}

static void name_inside_scratch_is_rejected_without_reading_past_it(void)
{
    mount_empty(16u);
    create_ok("hello", payload, 5u);

    /* Fill the last 16 bytes of scratch with non-zero bytes and no terminator.
     * A scanner that read a fixed 32-byte window before the context check would
     * walk past the object here; the per-byte check must reject before reading. */
    for (uint32_t i = YAN_FS_BLOCK_SIZE - 16u; i < YAN_FS_BLOCK_SIZE; ++i) {
        fs.scratch[i] = (uint8_t)'a';
    }
    const char *bad = (const char *)(const void *)(fs.scratch + YAN_FS_BLOCK_SIZE - 16u);
    uint32_t before = yan_fs_metadata_crc(fs.metadata);
    uint32_t writes = device.writes;
    YanFsInfo info = {0};
    uint32_t got = 123u;

    TEST_ASSERT_EQUAL_INT(YAN_FS_INVALID, yan_fs_stat(&fs, bad, &info));
    TEST_ASSERT_EQUAL_INT(YAN_FS_INVALID, yan_fs_create(&fs, bad, payload, 1u));
    TEST_ASSERT_EQUAL_INT(YAN_FS_INVALID, yan_fs_replace(&fs, bad, payload, 1u));
    TEST_ASSERT_EQUAL_INT(YAN_FS_INVALID, yan_fs_remove(&fs, bad));
    TEST_ASSERT_EQUAL_INT(YAN_FS_INVALID,
                          yan_fs_read(&fs, bad, 0u, readback, 5u, &got));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, got, "a rejected name still clears a valid read_bytes");
    TEST_ASSERT_EQUAL_UINT32(writes, device.writes);
    TEST_ASSERT_EQUAL_UINT32(before, yan_fs_metadata_crc(fs.metadata));
    TEST_ASSERT_EQUAL_INT(YAN_FS_MOUNTED, fs.state);

    /* The opposite direction: a legal short name whose terminator is the byte
     * immediately before the context object is accepted. The check covers only
     * the bytes actually read, so it does not reject a whole 32-byte window. */
    static struct {
        char name[8];
        YanFs context;
    } adjacent;
    fx_device(16u);
    fx_entry(device.blocks[0], 0u, "abcdefg", 0u, 0u, 0u);
    fx_seal(device.blocks[0]);
    memset(&adjacent, 0, sizeof adjacent);
    const char *legal = "abcdefg";
    for (uint32_t i = 0; i < 8u; ++i) {
        adjacent.name[i] = legal[i];
    }
    TEST_ASSERT_EQUAL_UINT32(
        1u, (uint32_t)((uintptr_t)&adjacent.context - (uintptr_t)&adjacent.name[7]));
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_init(&adjacent.context, fx_backend()));
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_mount(&adjacent.context));
    YanFsInfo adjacent_info = {0};
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_OK, yan_fs_stat(&adjacent.context, adjacent.name, &adjacent_info));
    TEST_ASSERT_EQUAL_UINT32(0u, adjacent_info.size_bytes);
}

static void read_before_mount_and_uninitialized_report_zero(void)
{
    /* Initialised but not mounted: a valid read_bytes is cleared, the name is
     * never parsed, no callback runs and the output buffer is untouched. */
    fx_device(16u);
    memset(&fs, 0, sizeof fs);
    init_fs();
    uint8_t out[8];
    memset(out, 0xa5, sizeof out);
    uint32_t got = 123u;
    uint32_t writes = device.writes;
    uint32_t reads = device.reads;
    TEST_ASSERT_EQUAL_INT(YAN_FS_NOT_MOUNTED,
                          yan_fs_read(&fs, "f", 0u, out, 4u, &got));
    TEST_ASSERT_EQUAL_UINT32(0u, got);
    TEST_ASSERT_TRUE(region_is_byte(out, sizeof out, 0xa5u));
    TEST_ASSERT_EQUAL_UINT32(writes, device.writes);
    TEST_ASSERT_EQUAL_UINT32(reads, device.reads);

    /* A zero-initialised, never-initialised context is INVALID, but its count
     * pointer is an ordinary caller output and is still cleared to zero. */
    YanFs virgin;
    memset(&virgin, 0, sizeof virgin);
    got = 123u;
    memset(out, 0xa5, sizeof out);
    TEST_ASSERT_EQUAL_INT(YAN_FS_INVALID,
                          yan_fs_read(&virgin, "f", 0u, out, 4u, &got));
    TEST_ASSERT_EQUAL_UINT32(0u, got);
    TEST_ASSERT_TRUE(region_is_byte(out, sizeof out, 0xa5u));
}

/* 0021 file-API state order: context valid/initialized -> busy -> FAULTED ->
 * MOUNTED -> the operation's other parameters. An invalid count pointer (NULL,
 * or one aliasing the context) is an "other parameter", so a state refusal must
 * win: BUSY, FAULTED and NOT_MOUNTED beat INVALID, and only a mounted, not-busy
 * instance may answer INVALID for the count itself. The never-initialised
 * context is INVALID first, because the context check precedes the state. */
static void read_state_priority_precedes_an_invalid_count(void)
{
    uint8_t out[8];
    uint32_t got = 0xfeedfaceu;
    memset(out, 0xa5, sizeof out);

    /* Never initialised: the context check comes first, so INVALID whatever the
     * count pointer is. */
    YanFs virgin;
    memset(&virgin, 0, sizeof virgin);
    TEST_ASSERT_EQUAL_INT(YAN_FS_INVALID,
                          yan_fs_read(&virgin, "f", 0u, out, 4u, NULL));
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_INVALID,
        yan_fs_read(&virgin, "f", 0u, out, 4u,
                    (uint32_t *)(void *)virgin.metadata));
    TEST_ASSERT_TRUE(region_is_byte(out, sizeof out, 0xa5u));

    /* Initialised and UNMOUNTED: NOT_MOUNTED wins over either invalid count,
     * and a valid count is still cleared to zero. No I/O, medium untouched. */
    fx_device(16u);
    memset(&fs, 0, sizeof fs);
    init_fs();
    uint32_t reads_before = device.reads;
    uint32_t writes_before = device.writes;
    static uint8_t fs_before[sizeof(YanFs)];
    memcpy(fs_before, &fs, sizeof fs_before);
    TEST_ASSERT_EQUAL_INT(YAN_FS_NOT_MOUNTED,
                          yan_fs_read(&fs, "f", 0u, out, 4u, NULL));
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(fs_before, &fs, sizeof fs_before,
                                     "a NULL count must not change the fs");
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_NOT_MOUNTED,
        yan_fs_read(&fs, "f", 0u, out, 4u, (uint32_t *)(void *)fs.metadata));
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(
        fs_before, &fs, sizeof fs_before,
        "a context-aliasing count must not change the fs");
    got = 0xfeedfaceu;
    TEST_ASSERT_EQUAL_INT(YAN_FS_NOT_MOUNTED,
                          yan_fs_read(&fs, "f", 0u, out, 4u, &got));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, got, "a valid count is cleared even when the state is refused");
    TEST_ASSERT_EQUAL_MEMORY(fs_before, &fs, sizeof fs_before);
    TEST_ASSERT_EQUAL_UINT32(reads_before, device.reads);
    TEST_ASSERT_EQUAL_UINT32(writes_before, device.writes);
    TEST_ASSERT_TRUE(region_is_byte(out, sizeof out, 0xa5u));

    /* FAULTED: FAULTED wins over either invalid count, and a valid count is
     * cleared to zero. The fault is injected on the first medium write. */
    mount_empty(16u);
    fill_payload(5u, 0x21);
    create_ok("f", payload, 5u);
    device.writes = 0u;
    device.write_fail_at = 1u;
    device.write_partial = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO, yan_fs_replace(&fs, "f", payload, 5u));
    device.write_fail_at = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_STATE_FAULTED, fs.state);
    reads_before = device.reads;
    writes_before = device.writes;
    memcpy(fs_before, &fs, sizeof fs_before);
    TEST_ASSERT_EQUAL_INT(YAN_FS_FAULTED,
                          yan_fs_read(&fs, "f", 0u, out, 4u, NULL));
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(fs_before, &fs, sizeof fs_before,
                                     "a NULL count must not change the faulted fs");
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_FAULTED,
        yan_fs_read(&fs, "f", 0u, out, 4u, (uint32_t *)(void *)fs.metadata));
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(
        fs_before, &fs, sizeof fs_before,
        "an aliasing count must not change the faulted fs");
    got = 0xfeedfaceu;
    TEST_ASSERT_EQUAL_INT(YAN_FS_FAULTED,
                          yan_fs_read(&fs, "f", 0u, out, 4u, &got));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, got, "a faulted read still clears a valid count");
    TEST_ASSERT_EQUAL_MEMORY(fs_before, &fs, sizeof fs_before);
    TEST_ASSERT_EQUAL_UINT32(reads_before, device.reads);
    TEST_ASSERT_EQUAL_UINT32(writes_before, device.writes);
    TEST_ASSERT_TRUE(region_is_byte(out, sizeof out, 0xa5u));

    /* BUSY: the probe runs from inside a real write callback, so the instance
     * is busy. Both invalid count forms must answer BUSY, not INVALID. */
    mount_empty(16u);
    fill_payload(5u, 0x22);
    create_ok("hello.txt", payload, 5u);
    probe_fs = &fs;
    probe_ran = false;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK,
                          yan_fs_replace(&fs, "hello.txt", payload, 5u));
    TEST_ASSERT_TRUE_MESSAGE(probe_ran, "the write callback must run the probe");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_FS_BUSY, probe_read_null_result,
        "busy wins over a NULL count pointer");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_FS_BUSY, probe_read_alias_result,
        "busy wins over a context-aliasing count pointer");
    TEST_ASSERT_TRUE_MESSAGE(
        probe_read_null_fs_unchanged,
        "busy NULL count must leave the whole fs byte-identical");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, probe_read_null_io, "busy NULL count must run no device callback");
    TEST_ASSERT_TRUE_MESSAGE(
        probe_read_alias_fs_unchanged,
        "busy aliasing count must leave the whole fs byte-identical");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, probe_read_alias_io,
        "busy aliasing count must run no device callback");
    TEST_ASSERT_EQUAL_INT(YAN_FS_MOUNTED, fs.state);
}

/* -------------------------------------------------------------------- read */

static void read_handles_all_lengths_and_tail_zero(void)
{
    static const uint32_t sizes[] = {0u, 1u, 4095u, 4096u, 4097u, 5000u};
    for (uint32_t i = 0; i < 6u; ++i) {
        uint32_t length = sizes[i];
        fx_device(16u);
        TEST_ASSERT_EQUAL_INT(YAN_FS_OK, mount_fresh());
        fill_payload(length, (uint8_t)(0x10u + i));
        uint32_t needed = length / 4096u + ((length % 4096u) != 0u ? 1u : 0u);
        TEST_ASSERT_EQUAL_INT(
            YAN_FS_OK, yan_fs_create(&fs, "f", length > 0u ? payload : NULL, length));
        TEST_ASSERT_EQUAL_UINT32(needed, cache_count(0u));
        TEST_ASSERT_EQUAL_UINT32(length, cache_size(0u));

        memset(readback, 0xa5, sizeof readback);
        uint32_t got = 0xffffffffu;
        TEST_ASSERT_EQUAL_INT(
            YAN_FS_OK,
            yan_fs_read(&fs, "f", 0u, length > 0u ? readback : NULL, length, &got));
        TEST_ASSERT_EQUAL_UINT32(length, got);
        if (length > 0u) {
            TEST_ASSERT_EQUAL_MEMORY(payload, readback, length);
        } else {
            /* A zero-length read returns OK and zero bytes; it must not touch
             * the buffer, and Unity cannot compare zero bytes ("compare
             * nothing"). */
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(
                0xa5u, readback[0],
                "a zero-length read must not touch the output buffer");
        }

        if (length > 0u && (length % 4096u) != 0u) {
            uint32_t last = cache_start(0u) + needed - 1u;
            uint32_t used = length % 4096u;
            TEST_ASSERT_TRUE_MESSAGE(
                region_is_zero(device.blocks[last] + used, YAN_FS_BLOCK_SIZE - used),
                "the bytes past the logical end must be zero");
        }
    }
}

static void read_rejects_read_bytes_overlapping_out(void)
{
    mount_empty(16u);
    fill_payload(5u, 0x36);
    create_ok("f", payload, 5u);
    uint32_t reads_before = device.reads;

    /* read_bytes equals out. */
    memset(aligned_words, 0xa5, sizeof aligned_words);
    uint8_t *out = (uint8_t *)(void *)aligned_words;
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_INVALID,
        yan_fs_read(&fs, "f", 0u, out, 8u, (uint32_t *)(void *)aligned_words));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, aligned_words[0],
        "read_bytes is cleared even when it overlaps out");

    /* read_bytes in the middle of out. out starts two bytes in, so the word is
     * naturally aligned and the sanitizer build sees no misaligned store. */
    memset(aligned_words, 0xa5, sizeof aligned_words);
    out = (uint8_t *)(void *)aligned_words + 2u;
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_INVALID, yan_fs_read(&fs, "f", 0u, out, 8u, &aligned_words[1]));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, aligned_words[1],
        "read_bytes is cleared even when it overlaps out");

    /* read_bytes straddles the end boundary of out: [out+6, out+10) against
     * [out, out+8). */
    memset(aligned_words, 0xa5, sizeof aligned_words);
    out = (uint8_t *)(void *)aligned_words + 2u;
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_INVALID, yan_fs_read(&fs, "f", 0u, out, 8u, &aligned_words[2]));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, aligned_words[2],
        "read_bytes is cleared even when it overlaps out");

    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        reads_before, device.reads,
        "a rejected overlapping read must perform no device I/O");
}

static void read_accepts_disjoint_and_zero_length_outputs(void)
{
    mount_empty(16u);
    fill_payload(5u, 0x37);
    create_ok("f", payload, 5u);
    uint32_t reads_before = device.reads;

    /* Adjacent but disjoint: out = [w+2, w+10), read_bytes = [w+12, w+16). */
    memset(aligned_words, 0xa5, sizeof aligned_words);
    uint8_t *out = (uint8_t *)(void *)aligned_words + 2u;
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_OK, yan_fs_read(&fs, "f", 0u, out, 8u, &aligned_words[3]));
    TEST_ASSERT_EQUAL_UINT32(5u, aligned_words[3]);
    TEST_ASSERT_EQUAL_MEMORY(payload, out, 5u);
    TEST_ASSERT_EQUAL_UINT32(reads_before + 1u, device.reads);

    /* length = 0 makes the out range empty, so pointing read_bytes at out is
     * not an overlap; the call returns OK with zero bytes and no I/O. The
     * valid read_bytes is still cleared, which is the four bytes of out. */
    memset(aligned_words, 0xa5, sizeof aligned_words);
    out = (uint8_t *)(void *)aligned_words;
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_OK,
        yan_fs_read(&fs, "f", 0u, out, 0u, (uint32_t *)(void *)aligned_words));
    TEST_ASSERT_EQUAL_UINT32(0u, aligned_words[0]);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(
        0xa5u, out[4], "a zero-length read must not touch bytes past read_bytes");
    TEST_ASSERT_EQUAL_UINT32(reads_before + 1u, device.reads);
}

static void read_allows_name_shared_with_count_or_out(void)
{
    mount_empty(16u);
    fill_payload(5u, 0x38);
    create_ok("hello", payload, 5u);

    /* 0021 forbids context aliases and the read out/read_bytes overlap, but the
     * caller may use the same writable memory for name and read_bytes. The
     * count clear must not destroy the name before the lookup. */
    union {
        uint32_t words[8];
        char name[32];
    } shared;
    memset(&shared, 0, sizeof shared);
    memcpy(shared.name, "hello", 6u);
    uint8_t out[8];
    memset(out, 0xa5, sizeof out);
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_OK, yan_fs_read(&fs, shared.name, 0u, out, 5u, shared.words));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        5u, shared.words[0], "the count is written after the name was used");
    TEST_ASSERT_EQUAL_MEMORY(payload, out, 5u);

    /* name and out may share memory too: the name is copied before the file
     * bytes are written into out. */
    union {
        uint32_t words[8];
        char name[32];
    } shared2;
    memset(&shared2, 0, sizeof shared2);
    memcpy(shared2.name, "hello", 6u);
    uint32_t count = 0xffffffffu;
    uint8_t *shared_out = (uint8_t *)(void *)shared2.name;
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_OK, yan_fs_read(&fs, shared2.name, 0u, shared_out, 5u, &count));
    TEST_ASSERT_EQUAL_UINT32(5u, count);
    TEST_ASSERT_EQUAL_MEMORY(payload, shared_out, 5u);
}

static void read_stops_at_eof_and_truncates(void)
{
    mount_empty(16u);
    fill_payload(5000u, 0x31);
    create_ok("f", payload, 5000u);

    uint32_t got = 0xffffffffu;
    memset(readback, 0xa5, sizeof readback);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_read(&fs, "f", 5000u, readback, 100u, &got));
    TEST_ASSERT_EQUAL_UINT32(0u, got);
    TEST_ASSERT_EQUAL_UINT8(0xa5u, readback[0]);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_read(&fs, "f", 6000u, readback, 10u, &got));
    TEST_ASSERT_EQUAL_UINT32(0u, got);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_read(&fs, "f", 4990u, readback, 100u, &got));
    TEST_ASSERT_EQUAL_UINT32(10u, got);
    TEST_ASSERT_EQUAL_MEMORY(payload + 4990u, readback, 10u);
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_NOT_FOUND, yan_fs_read(&fs, "missing", 0u, readback, 1u, &got));
    TEST_ASSERT_EQUAL_UINT32(0u, got);

    /* length = 0 accepts a NULL output and reads nothing. */
    got = 0xffffffffu;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_read(&fs, "f", 0u, NULL, 0u, &got));
    TEST_ASSERT_EQUAL_UINT32(0u, got);
}

static void read_across_block_boundary_returns_exact_bytes(void)
{
    mount_empty(16u);
    fill_payload(5000u, 0x32);
    create_ok("f", payload, 5000u);

    uint32_t got = 0u;
    memset(readback, 0xa5, sizeof readback);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_read(&fs, "f", 4090u, readback, 20u, &got));
    TEST_ASSERT_EQUAL_UINT32(20u, got);
    TEST_ASSERT_EQUAL_MEMORY(payload + 4090u, readback, 20u);

    memset(readback, 0xa5, sizeof readback);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_read(&fs, "f", 0u, readback, 4096u, &got));
    TEST_ASSERT_EQUAL_UINT32(4096u, got);
    TEST_ASSERT_EQUAL_MEMORY(payload, readback, 4096u);

    memset(readback, 0xa5, sizeof readback);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_read(&fs, "f", 4096u, readback, 4096u, &got));
    TEST_ASSERT_EQUAL_UINT32(904u, got);
    TEST_ASSERT_EQUAL_MEMORY(payload + 4096u, readback, 904u);
}

static void read_second_block_failure_keeps_prefix_and_sentinel(void)
{
    mount_empty(16u);
    fill_payload(5000u, 0x33);
    create_ok("f", payload, 5000u);

    device.reads = 0u; /* count data reads from zero; mount's read is done */
    device.read_fail_at = 2u;
    uint32_t got = 0xffffffffu;
    memset(readback, 0x5a, sizeof readback);
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO, yan_fs_read(&fs, "f", 4090u, readback, 20u, &got));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        6u, got, "only the first, fully read block is counted");
    TEST_ASSERT_EQUAL_MEMORY(payload + 4090u, readback, 6u);
    TEST_ASSERT_TRUE_MESSAGE(
        region_is_byte(readback + 6u, 14u, 0x5au),
        "the failing block's bytes must not reach the caller buffer");
    TEST_ASSERT_EQUAL_INT(YAN_FS_STATE_FAULTED, fs.state);
}

static void read_unknown_callback_status_maps_protocol(void)
{
    mount_empty(16u);
    fill_payload(5u, 0x34);
    create_ok("f", payload, 5u);

    device.reads = 0u;
    device.read_fail_at = 1u;
    device.read_fail_code = (YanFsIoResult)88;
    uint32_t got = 0xffffffffu;
    uint8_t byte = 0;
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_PROTOCOL, yan_fs_read(&fs, "f", 0u, &byte, 1u, &got));
    TEST_ASSERT_EQUAL_UINT32(0u, got);
    TEST_ASSERT_EQUAL_INT(YAN_FS_STATE_FAULTED, fs.state);
}

/* ------------------------------------------------ create / replace / remove */

static void create_writes_entry_and_data_then_reads_back(void)
{
    mount_empty(16u);
    fill_payload(5u, 0x40);
    create_ok("hello.txt", payload, 5u);

    TEST_ASSERT_EQUAL_UINT32(5u, cache_size(0u));
    TEST_ASSERT_EQUAL_UINT32(1u, cache_start(0u));
    TEST_ASSERT_EQUAL_UINT32(1u, cache_count(0u));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        2u, device.writes, "one data block plus one metadata block");
    TEST_ASSERT_EQUAL_MEMORY(payload, device.blocks[1], 5u);
    TEST_ASSERT_TRUE(region_is_zero(device.blocks[1] + 5u, 4096u - 5u));

    uint32_t got = 0u;
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_OK, yan_fs_read(&fs, "hello.txt", 0u, readback, 5u, &got));
    TEST_ASSERT_EQUAL_UINT32(5u, got);
    TEST_ASSERT_EQUAL_MEMORY(payload, readback, 5u);
}

static void create_zero_length_is_empty_file_without_data_write(void)
{
    mount_empty(16u);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_create(&fs, "empty", NULL, 0u));
    TEST_ASSERT_EQUAL_UINT32(0u, cache_size(0u));
    TEST_ASSERT_EQUAL_UINT32(0u, cache_start(0u));
    TEST_ASSERT_EQUAL_UINT32(0u, cache_count(0u));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        1u, device.writes, "an empty file writes only the directory");

    YanFsInfo info = {0};
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_stat(&fs, "empty", &info));
    TEST_ASSERT_EQUAL_UINT32(0u, info.size_bytes);
    uint32_t got = 0xffffffffu;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_read(&fs, "empty", 0u, NULL, 0u, &got));
    TEST_ASSERT_EQUAL_UINT32(0u, got);
}

static void create_rejects_duplicate_and_invalid_names_without_io(void)
{
    mount_empty(16u);
    create_ok("a", payload, 1u);
    snapshot_medium();
    uint32_t writes = device.writes;
    TEST_ASSERT_EQUAL_INT(YAN_FS_EXISTS, yan_fs_create(&fs, "a", payload, 1u));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(writes, device.writes,
                                     "a duplicate name must not write");
    TEST_ASSERT_TRUE(medium_equal_snapshot());
    TEST_ASSERT_EQUAL_INT(YAN_FS_MOUNTED, fs.state);

    static const char longest[] = "0123456789abcdef0123456789abcde"; /* 31 */
    TEST_ASSERT_EQUAL_UINT32(31u, (uint32_t)strlen(longest));
    create_ok(longest, NULL, 0u);
    snapshot_medium();
    writes = device.writes;
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_INVALID,
        yan_fs_create(&fs, "0123456789abcdef0123456789abcdef", NULL, 0u));
    TEST_ASSERT_EQUAL_INT(YAN_FS_INVALID, yan_fs_create(&fs, "a/b", NULL, 0u));
    TEST_ASSERT_EQUAL_INT(YAN_FS_INVALID, yan_fs_create(&fs, "", NULL, 0u));
    TEST_ASSERT_EQUAL_INT(YAN_FS_INVALID, yan_fs_create(&fs, NULL, NULL, 0u));
    TEST_ASSERT_EQUAL_UINT32(writes, device.writes);
    TEST_ASSERT_TRUE(medium_equal_snapshot());
}

static void create_reports_directory_full_without_writing(void)
{
    fx_device(16u);
    for (uint32_t slot = 0; slot < YAN_FS_MAX_FILES; ++slot) {
        char name[8];
        name[0] = 'f';
        name[1] = (char)('0' + (slot / 10u));
        name[2] = (char)('0' + (slot % 10u));
        name[3] = '\0';
        fx_entry(device.blocks[0], slot, name, 0u, 0u, 0u);
    }
    fx_seal(device.blocks[0]);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, mount_fresh());
    snapshot_medium();
    uint32_t writes = device.writes;
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_DIRECTORY_FULL, yan_fs_create(&fs, "overflow", NULL, 0u));
    TEST_ASSERT_EQUAL_UINT32(writes, device.writes);
    TEST_ASSERT_TRUE(medium_equal_snapshot());
    TEST_ASSERT_EQUAL_INT(YAN_FS_MOUNTED, fs.state);
}

static void create_uses_lowest_contiguous_free_run(void)
{
    mount_empty(16u);
    create_ok("a", payload, 1u);
    create_ok("b", payload, 1u);
    TEST_ASSERT_EQUAL_UINT32(1u, cache_start(0u));
    TEST_ASSERT_EQUAL_UINT32(2u, cache_start(1u));

    /* Removing the first file leaves slot 0 and block 1 free; the next create
     * must reuse both. */
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_remove(&fs, "a"));
    TEST_ASSERT_TRUE(cache_slot_empty(0u));
    create_ok("c", payload, 1u);
    TEST_ASSERT_EQUAL_STRING("c", (const char *)cache_entry(0u));
    TEST_ASSERT_EQUAL_UINT32(1u, cache_start(0u));
}

static void create_reports_nospace_for_fragmented_free_space(void)
{
    mount_empty(5u);
    create_ok("a", payload, 1u); /* block 1 */
    create_ok("b", payload, 1u); /* block 2 */
    create_ok("c", payload, 1u); /* block 3 */
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_remove(&fs, "b")); /* hole at block 2 */

    snapshot_medium();
    uint32_t writes = device.writes;
    fill_payload(5000u, 0x70);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_FS_NOSPACE, yan_fs_create(&fs, "big", payload, 5000u),
        "two free blocks that are not adjacent cannot hold a two-block file");
    TEST_ASSERT_EQUAL_UINT32(writes, device.writes);
    TEST_ASSERT_TRUE(medium_equal_snapshot());
    TEST_ASSERT_EQUAL_INT(YAN_FS_MOUNTED, fs.state);
}

static void create_huge_length_reports_nospace_without_writing(void)
{
    mount_empty(16u);
    snapshot_medium();
    uint32_t writes = device.writes;
    /* The ceiling for UINT32_MAX is 1048576 blocks and must not wrap; the
     * allocation fails before bytes are read. The declared range starts exactly
     * one past the context object, so the context guard accepts it; the fixture
     * does not reuse a small payload whose declared range would cover the
     * context (that would be a caller error the guard correctly rejects). The
     * bytes are never dereferenced: a 16-block device cannot hold the extent. */
    const uint8_t *declared =
        (const uint8_t *)(uintptr_t)((uintptr_t)&fs + sizeof fs);
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_NOSPACE, yan_fs_create(&fs, "huge", declared, UINT32_MAX));
    TEST_ASSERT_EQUAL_UINT32(writes, device.writes);
    TEST_ASSERT_TRUE(medium_equal_snapshot());
    TEST_ASSERT_EQUAL_INT(YAN_FS_MOUNTED, fs.state);
}

static void huge_capacity_first_fit_does_not_scan_blocks(void)
{
    fx_device(UINT32_MAX);
    device.capacity_blocks = UINT32_MAX;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, mount_fresh());
    fill_payload(1u, 0x71);
    create_ok("h", payload, 1u);
    TEST_ASSERT_EQUAL_UINT32(1u, cache_start(0u));
    TEST_ASSERT_EQUAL_UINT32(1u, cache_count(0u));
    TEST_ASSERT_EQUAL_UINT8(0x71u, device.blocks[1][0]);

    uint32_t got = 0u;
    uint8_t byte = 0;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_read(&fs, "h", 0u, &byte, 1u, &got));
    TEST_ASSERT_EQUAL_UINT32(1u, got);
    TEST_ASSERT_EQUAL_UINT8(0x71u, byte);
}

static void replace_grows_to_new_extent_and_frees_old(void)
{
    mount_empty(16u);
    fill_payload(5u, 0x50);
    create_ok("hello.txt", payload, 5u); /* block 1 */
    fill_payload(5000u, 0x51);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_replace(&fs, "hello.txt", payload, 5000u));

    TEST_ASSERT_EQUAL_UINT32(5000u, cache_size(0u));
    TEST_ASSERT_EQUAL_UINT32(2u, cache_start(0u));
    TEST_ASSERT_EQUAL_UINT32(2u, cache_count(0u));
    TEST_ASSERT_EQUAL_MEMORY(payload, device.blocks[2], 4096u);
    TEST_ASSERT_EQUAL_MEMORY(payload + 4096u, device.blocks[3], 904u);

    /* Removing the grown file frees blocks 2 and 3; the next file takes block 1
     * only if the replaced-away block 1 really stopped being referenced. */
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_remove(&fs, "hello.txt"));
    TEST_ASSERT_TRUE(cache_slot_empty(0u));
    create_ok("new", payload, 1u);
    TEST_ASSERT_EQUAL_UINT32(1u, cache_start(0u));
}

static void replace_shrinks_and_empties_keeping_slot_and_name(void)
{
    mount_empty(16u);
    fill_payload(5u, 0x52);
    create_ok("f", payload, 5u); /* slot 0, block 1 */
    fill_payload(3u, 0x53);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_replace(&fs, "f", payload, 3u));
    TEST_ASSERT_EQUAL_STRING("f", (const char *)cache_entry(0u));
    TEST_ASSERT_EQUAL_UINT32(3u, cache_size(0u));
    /* The old extent is not reusable for the new one, so the shrunken file
     * moves to block 2 and block 1 becomes free. */
    TEST_ASSERT_EQUAL_UINT32(2u, cache_start(0u));

    uint32_t writes = device.writes;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_replace(&fs, "f", NULL, 0u));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        writes + 1u, device.writes,
        "replacing with an empty file writes only the directory");
    TEST_ASSERT_EQUAL_STRING("f", (const char *)cache_entry(0u));
    TEST_ASSERT_EQUAL_UINT32(0u, cache_size(0u));
    TEST_ASSERT_EQUAL_UINT32(0u, cache_start(0u));
    TEST_ASSERT_EQUAL_UINT32(0u, cache_count(0u));

    /* Block 1 and the freed extent are both available now. */
    create_ok("g", payload, 1u);
    TEST_ASSERT_EQUAL_UINT32(1u, cache_start(1u));
}

static void replace_requires_an_existing_file(void)
{
    mount_empty(16u);
    snapshot_medium();
    uint32_t writes = device.writes;
    TEST_ASSERT_EQUAL_INT(YAN_FS_NOT_FOUND, yan_fs_replace(&fs, "missing", payload, 1u));
    TEST_ASSERT_EQUAL_UINT32(writes, device.writes);
    TEST_ASSERT_TRUE(medium_equal_snapshot());
    TEST_ASSERT_EQUAL_INT(YAN_FS_MOUNTED, fs.state);
}

static void replace_cannot_borrow_its_own_extent_at_peak(void)
{
    /* The 0021 example: a three-block device where a 5-byte file cannot grow to
     * 5000 bytes because the new data must not overwrite the old extent. */
    mount_empty(3u);
    fill_payload(5u, 0x54);
    create_ok("hello.txt", payload, 5u); /* block 1 */
    snapshot_medium();
    uint32_t writes = device.writes;
    fill_payload(5000u, 0x55);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_FS_NOSPACE, yan_fs_replace(&fs, "hello.txt", payload, 5000u),
        "a replacement cannot reuse the extent it is replacing");
    TEST_ASSERT_EQUAL_UINT32(writes, device.writes);
    TEST_ASSERT_TRUE(medium_equal_snapshot());
    TEST_ASSERT_EQUAL_UINT32(5u, cache_size(0u));
    TEST_ASSERT_EQUAL_UINT32(1u, cache_start(0u));
    TEST_ASSERT_EQUAL_UINT32(1u, cache_count(0u));
}

static void replace_write_failure_keeps_old_directory_on_medium(void)
{
    mount_empty(16u);
    fill_payload(5u, 0x56);
    create_ok("f", payload, 5u);
    snapshot_medium();
    fill_payload(5000u, 0x57);
    device.writes = 0u;
    device.write_fail_at = 1u;
    device.write_partial = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO, yan_fs_replace(&fs, "f", payload, 5000u));
    TEST_ASSERT_EQUAL_INT(YAN_FS_STATE_FAULTED, fs.state);
    TEST_ASSERT_TRUE_MESSAGE(region_equal(medium_before[0], device.blocks[0], 4096u),
                             "the directory on the medium must still be the old one");
    device.write_fail_at = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, mount_fresh());
    YanFsInfo info = {0};
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_stat(&fs, "f", &info));
    TEST_ASSERT_EQUAL_UINT32(5u, info.size_bytes);
}

static void remove_clears_slot_with_single_metadata_write(void)
{
    mount_empty(16u);
    fill_payload(5000u, 0x58);
    create_ok("f", payload, 5000u); /* blocks 1 and 2 */
    snapshot_medium();
    uint32_t writes = device.writes;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_remove(&fs, "f"));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(writes + 1u, device.writes,
                                     "remove is a single directory write");
    TEST_ASSERT_TRUE(cache_slot_empty(0u));
    TEST_ASSERT_EQUAL_MEMORY(medium_before[1], device.blocks[1], 4096u);
    TEST_ASSERT_EQUAL_MEMORY(medium_before[2], device.blocks[2], 4096u);
    TEST_ASSERT_EQUAL_INT(YAN_FS_NOT_FOUND, yan_fs_remove(&fs, "f"));

    create_ok("g", payload, 1u);
    TEST_ASSERT_EQUAL_UINT32(1u, cache_start(0u));
}

static void operations_leave_the_neighbour_file_unchanged(void)
{
    mount_empty(16u);
    fill_payload(100u, 0x60);
    create_ok("a", payload, 100u); /* slot 0, block 1 */
    fill_payload(100u, 0x61);
    create_ok("b", payload, 100u); /* slot 1, block 2 */
    memcpy(neighbour, device.blocks[2], 100u);
    memcpy(entry_before, cache_entry(1u), YAN_FS_ENTRY_SIZE);

    fill_payload(5000u, 0x62);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_replace(&fs, "a", payload, 5000u));
    TEST_ASSERT_TRUE_MESSAGE(region_equal(neighbour, device.blocks[2], 100u),
                             "replacing a must not touch b's data");
    TEST_ASSERT_TRUE_MESSAGE(
        region_equal(entry_before, cache_entry(1u), YAN_FS_ENTRY_SIZE),
        "replacing a must not touch b's directory entry");

    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_remove(&fs, "a"));
    TEST_ASSERT_TRUE(region_equal(neighbour, device.blocks[2], 100u));
    TEST_ASSERT_TRUE(region_equal(entry_before, cache_entry(1u), YAN_FS_ENTRY_SIZE));
    TEST_ASSERT_TRUE(cache_slot_empty(0u));
}

/* --------------------------------------------------------- failure injection */

static void create_failure_before_each_write_leaves_only_completed_blocks(void)
{
    for (uint32_t fail_at = 1u; fail_at <= 3u; ++fail_at) {
        fx_device(16u);
        TEST_ASSERT_EQUAL_INT(YAN_FS_OK, mount_fresh());
        fill_payload(5000u, 0x09);
        snapshot_medium();
        device.writes = 0u; /* injection counts calls of this attempt only */
        device.write_fail_at = fail_at;
        device.write_partial = 0u;
        device.write_fail_code = YAN_FS_IO_OK;
        TEST_ASSERT_EQUAL_INT(YAN_FS_IO, yan_fs_create(&fs, "f", payload, 5000u));
        TEST_ASSERT_EQUAL_INT(YAN_FS_STATE_FAULTED, fs.state);
        TEST_ASSERT_EQUAL_UINT32(fail_at, device.writes);

        bool block0_changed = !region_equal(medium_before[0], device.blocks[0], 4096u);
        bool block1_changed = !region_equal(medium_before[1], device.blocks[1], 4096u);
        bool block2_changed = !region_equal(medium_before[2], device.blocks[2], 4096u);
        if (fail_at == 1u) {
            TEST_ASSERT_FALSE(block1_changed);
            TEST_ASSERT_FALSE(block2_changed);
        } else if (fail_at == 2u) {
            TEST_ASSERT_TRUE(block1_changed);
            TEST_ASSERT_FALSE(block2_changed);
        } else {
            TEST_ASSERT_TRUE(block1_changed);
            TEST_ASSERT_TRUE(block2_changed);
        }
        TEST_ASSERT_FALSE_MESSAGE(
            block0_changed, "the directory block is written only after all data");

        /* The medium still holds the old, empty directory. */
        TEST_ASSERT_EQUAL_INT(YAN_FS_OK, mount_fresh());
        YanFsInfo info = {0};
        TEST_ASSERT_EQUAL_INT(YAN_FS_NOT_FOUND, yan_fs_stat(&fs, "f", &info));
    }
}

static void create_partial_data_write_leaves_bytes_without_entry(void)
{
    mount_empty(16u);
    fill_payload(5000u, 0x0a);
    snapshot_medium();
    device.writes = 0u;
    device.write_fail_at = 1u;
    device.write_partial = 1000u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO, yan_fs_create(&fs, "f", payload, 5000u));
    TEST_ASSERT_EQUAL_INT(YAN_FS_STATE_FAULTED, fs.state);
    TEST_ASSERT_EQUAL_MEMORY(payload, device.blocks[1], 1000u);
    TEST_ASSERT_TRUE(region_equal(medium_before[1] + 1000u, device.blocks[1] + 1000u,
                                  4096u - 1000u));
    TEST_ASSERT_TRUE(region_equal(medium_before[0], device.blocks[0], 4096u));

    /* Reopening sees an empty directory; the stray block is unreferenced and
     * immediately allocatable again. */
    device.write_fail_at = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, mount_fresh());
    create_ok("g", payload, 1u);
    TEST_ASSERT_EQUAL_UINT32(1u, cache_start(0u));
}

static void create_write_error_after_full_block_leaves_no_entry(void)
{
    mount_empty(16u);
    fill_payload(5000u, 0x0b);
    snapshot_medium();
    device.write_fail_at = 1u;
    device.write_partial = 4096u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO, yan_fs_create(&fs, "f", payload, 5000u));
    TEST_ASSERT_EQUAL_MEMORY(payload, device.blocks[1], 4096u);
    TEST_ASSERT_TRUE(region_equal(medium_before[0], device.blocks[0], 4096u));
    device.write_fail_at = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, mount_fresh());
    YanFsInfo info = {0};
    TEST_ASSERT_EQUAL_INT(YAN_FS_NOT_FOUND, yan_fs_stat(&fs, "f", &info));
}

static void create_metadata_write_failure_keeps_old_directory(void)
{
    mount_empty(16u);
    fill_payload(5000u, 0x0c);
    snapshot_medium();
    device.write_fail_at = 3u;
    device.write_partial = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO, yan_fs_create(&fs, "f", payload, 5000u));
    TEST_ASSERT_EQUAL_INT(YAN_FS_STATE_FAULTED, fs.state);
    TEST_ASSERT_TRUE(region_equal(medium_before[0], device.blocks[0], 4096u));
    TEST_ASSERT_FALSE(region_equal(medium_before[1], device.blocks[1], 4096u));
    device.write_fail_at = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, mount_fresh());
    YanFsInfo info = {0};
    TEST_ASSERT_EQUAL_INT(YAN_FS_NOT_FOUND, yan_fs_stat(&fs, "f", &info));
}

static void partial_metadata_write_is_not_rolled_back(void)
{
    mount_empty(16u);
    fill_payload(5000u, 0x0d);
    snapshot_medium();
    device.write_fail_at = 3u;
    device.write_partial = 100u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO, yan_fs_create(&fs, "f", payload, 5000u));
    TEST_ASSERT_FALSE(region_equal(medium_before[0], device.blocks[0], 4096u));
    device.write_fail_at = 0u;
    /* A partial directory write is damage, not a repaired state. */
    TEST_ASSERT_EQUAL_INT(YAN_FS_CORRUPT, mount_fresh());
}

static void write_protocol_and_unknown_status_fault_instance(void)
{
    mount_empty(16u);
    fill_payload(1u, 0x0e);
    device.writes = 0u;
    device.write_fail_at = 1u;
    device.write_fail_code = YAN_FS_IO_PROTOCOL;
    TEST_ASSERT_EQUAL_INT(YAN_FS_PROTOCOL, yan_fs_create(&fs, "f", payload, 1u));
    TEST_ASSERT_EQUAL_INT(YAN_FS_STATE_FAULTED, fs.state);

    mount_empty(16u);
    device.writes = 0u;
    device.write_fail_at = 1u;
    device.write_fail_code = (YanFsIoResult)77;
    TEST_ASSERT_EQUAL_INT(YAN_FS_PROTOCOL, yan_fs_create(&fs, "f", payload, 1u));
    TEST_ASSERT_EQUAL_INT(YAN_FS_STATE_FAULTED, fs.state);
}

static void faulted_instance_refuses_cached_views(void)
{
    mount_empty(16u);
    fill_payload(5u, 0x0f);
    create_ok("f", payload, 5u);
    device.writes = 0u;
    device.write_fail_at = 1u;
    device.write_partial = 0u;
    fill_payload(5u, 0x11);
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO, yan_fs_replace(&fs, "f", payload, 5u));
    TEST_ASSERT_EQUAL_INT(YAN_FS_STATE_FAULTED, fs.state);

    uint32_t cursor = 0u;
    YanFsInfo info = {0};
    uint32_t got = 0xffffffffu;
    uint8_t byte = 0;
    TEST_ASSERT_EQUAL_INT(YAN_FS_FAULTED, yan_fs_list(&fs, &cursor, &info));
    TEST_ASSERT_EQUAL_INT(YAN_FS_FAULTED, yan_fs_stat(&fs, "f", &info));
    TEST_ASSERT_EQUAL_INT(YAN_FS_FAULTED, yan_fs_read(&fs, "f", 0u, &byte, 1u, &got));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, got, "a faulted read still reports the zero prefix it can prove");
    TEST_ASSERT_EQUAL_INT(YAN_FS_FAULTED, yan_fs_create(&fs, "g", &byte, 1u));
    TEST_ASSERT_EQUAL_INT(YAN_FS_FAULTED, yan_fs_remove(&fs, "f"));
    TEST_ASSERT_EQUAL_INT(YAN_FS_FAULTED, yan_fs_replace(&fs, "f", &byte, 1u));
    TEST_ASSERT_EQUAL_INT(YAN_FS_FAULTED, yan_fs_mount(&fs));
}

static void remount_sees_new_old_or_corrupt_metadata(void)
{
    /* New: a successful create survives a real unmount/mount. */
    mount_empty(16u);
    fill_payload(5u, 0x1a);
    create_ok("f", payload, 5u);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_unmount(&fs));
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_mount(&fs));
    YanFsInfo info = {0};
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_stat(&fs, "f", &info));
    TEST_ASSERT_EQUAL_UINT32(5u, info.size_bytes);

    /* Old: the metadata write fails before block 0 is touched. The fault is
     * targeted relative to the writes already counted, because device.writes
     * keeps accumulating across the sub-cases: +1 is the data block, +2 is the
     * metadata block. */
    mount_empty(16u);
    fill_payload(5u, 0x1b);
    snapshot_medium();
    device.write_fail_at = device.writes + 2u;
    device.write_partial = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO, yan_fs_create(&fs, "f", payload, 5u));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, device.last_write_lba,
        "the injected failure must land on the metadata write, not on data");
    TEST_ASSERT_EQUAL_UINT32(device.write_fail_at, device.writes);
    TEST_ASSERT_TRUE(region_equal(medium_before[0], device.blocks[0], 4096u));
    device.write_fail_at = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, mount_fresh());
    TEST_ASSERT_EQUAL_INT(YAN_FS_NOT_FOUND, yan_fs_stat(&fs, "f", &info));

    /* Corrupt: a short metadata write leaves an unrepairable block 0. */
    mount_empty(16u);
    fill_payload(5u, 0x1c);
    device.write_fail_at = device.writes + 2u;
    device.write_partial = 100u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO, yan_fs_create(&fs, "f", payload, 5u));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, device.last_write_lba,
        "the short write must land on the metadata block");
    device.write_fail_at = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_CORRUPT, mount_fresh());
}

/* -------------------------------------------------------------------- busy */

static void list_and_stat_honor_busy_guard_from_callback(void)
{
    mount_empty(16u);
    create_ok("hello.txt", payload, 5u);
    probe_fs = &fs;
    probe_ran = false;
    uint32_t before = yan_fs_metadata_crc(fs.metadata);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_replace(&fs, "hello.txt", payload, 5u));
    TEST_ASSERT_TRUE_MESSAGE(probe_ran, "the write callback must run the probe");
    TEST_ASSERT_EQUAL_INT(YAN_FS_BUSY, probe_list_result);
    TEST_ASSERT_EQUAL_INT(YAN_FS_BUSY, probe_stat_result);
    TEST_ASSERT_EQUAL_INT(YAN_FS_BUSY, probe_mount_result);
    TEST_ASSERT_EQUAL_INT(YAN_FS_BUSY, probe_unmount_result);
    TEST_ASSERT_EQUAL_INT(YAN_FS_BUSY, probe_init_result);
    TEST_ASSERT_EQUAL_INT(YAN_FS_BUSY, probe_read_result);
    TEST_ASSERT_EQUAL_INT(YAN_FS_BUSY, probe_create_result);
    TEST_ASSERT_EQUAL_INT(YAN_FS_BUSY, probe_remove_result);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        before, probe_cache_crc,
        "a callback must not observe a cache that is still being built");
    TEST_ASSERT_EQUAL_INT(YAN_FS_MOUNTED, fs.state);

    uint32_t cursor = 0;
    YanFsInfo info = {0};
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_list(&fs, &cursor, &info));
}

static void create_honors_busy_guard_and_does_not_publish_early(void)
{
    mount_empty(16u);
    fill_payload(5000u, 0x1e);
    probe_fs = &fs;
    probe_ran = false;
    uint32_t before = yan_fs_metadata_crc(fs.metadata);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_create(&fs, "f", payload, 5000u));
    TEST_ASSERT_TRUE(probe_ran);
    TEST_ASSERT_EQUAL_INT(YAN_FS_BUSY, probe_list_result);
    TEST_ASSERT_EQUAL_INT(YAN_FS_BUSY, probe_unmount_result);
    TEST_ASSERT_EQUAL_INT(YAN_FS_BUSY, probe_read_result);
    TEST_ASSERT_EQUAL_INT(YAN_FS_BUSY, probe_create_result);
    TEST_ASSERT_EQUAL_INT(YAN_FS_BUSY, probe_remove_result);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        before, probe_cache_crc,
        "the directory must stay unpublished while create writes data");
    TEST_ASSERT_EQUAL_STRING("f", (const char *)cache_entry(0u));
    TEST_ASSERT_EQUAL_UINT32(5000u, cache_size(0u));
}

/* ----------------------------------------------------------------- unmount */

static void unmount_clears_cache_and_keeps_instance_usable(void)
{
    fx_device(16u);
    fx_entry(device.blocks[0], 0u, "hello.txt", 5u, 1u, 1u);
    fx_seal(device.blocks[0]);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, mount_fresh());
    fs.scratch[0] = 0x5au; /* dirty on purpose, the clear must be observable */

    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_unmount(&fs));
    TEST_ASSERT_EQUAL_INT(YAN_FS_UNMOUNTED, fs.state);
    TEST_ASSERT_EQUAL_UINT32(0u, fs.capacity_blocks);
    TEST_ASSERT_TRUE(region_is_zero(fs.metadata, YAN_FS_BLOCK_SIZE));
    TEST_ASSERT_TRUE(region_is_zero(fs.scratch, YAN_FS_BLOCK_SIZE));
    TEST_ASSERT_TRUE(fs.initialized);

    /* io and initialized survive, so a new mount needs no init. */
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_mount(&fs));
    TEST_ASSERT_EQUAL_INT(YAN_FS_MOUNTED, fs.state);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_unmount(&fs));
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_unmount(&fs));
}

static void unmount_is_allowed_from_unmounted_and_faulted(void)
{
    init_fs();
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_unmount(&fs));

    fx_device(16u);
    device.capacity_status = YAN_FS_IO_ERROR;
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO, yan_fs_mount(&fs));
    TEST_ASSERT_EQUAL_INT(YAN_FS_STATE_FAULTED, fs.state);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_unmount(&fs));
    TEST_ASSERT_EQUAL_INT(YAN_FS_UNMOUNTED, fs.state);
}

/* -------------------------------------------------------------------- init */

static void init_rules_reject_incomplete_active_and_faulted_instances(void)
{
    YanFsBlockIo incomplete = fx_backend();
    incomplete.write_block = NULL;
    TEST_ASSERT_EQUAL_INT(YAN_FS_INVALID, yan_fs_init(&fs, incomplete));
    TEST_ASSERT_FALSE(fs.initialized);
    TEST_ASSERT_EQUAL_INT(YAN_FS_INVALID, yan_fs_init(NULL, fx_backend()));

    init_fs();
    TEST_ASSERT_TRUE(fs.initialized);
    TEST_ASSERT_EQUAL_INT(YAN_FS_UNMOUNTED, fs.state);

    /* Re-init of an idle, unmounted instance is allowed. */
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_init(&fs, fx_backend()));

    fx_device(16u);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_mount(&fs));
    TEST_ASSERT_EQUAL_INT(YAN_FS_INVALID, yan_fs_init(&fs, fx_backend()));
    TEST_ASSERT_EQUAL_INT(YAN_FS_MOUNTED, fs.state);

    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_unmount(&fs));
    device.capacity_status = YAN_FS_IO_ERROR;
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO, yan_fs_mount(&fs));
    TEST_ASSERT_EQUAL_INT(YAN_FS_STATE_FAULTED, fs.state);
    TEST_ASSERT_EQUAL_INT(YAN_FS_INVALID, yan_fs_init(&fs, fx_backend()));
    TEST_ASSERT_EQUAL_INT(YAN_FS_STATE_FAULTED, fs.state);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(format_writes_header_and_empty_entries);
    RUN_TEST(format_encodes_capacity_little_endian);
    RUN_TEST(format_rejects_invalid_arguments_without_writing);
    RUN_TEST(metadata_crc_matches_known_vector_and_ignores_crc_field);
    RUN_TEST(mount_publishes_validated_cache_and_stat_reads_it);
    RUN_TEST(mount_accepts_capacity_one_empty_directory);
    RUN_TEST(mount_rejects_zero_and_oversized_device_capacity);
    RUN_TEST(mount_rejects_unknown_version_as_unsupported);
    RUN_TEST(mount_rejects_capacity_mismatch);
    RUN_TEST(mount_rejects_wrong_crc_unknown_magic_and_wrong_sizes);
    RUN_TEST(mount_rejects_nonzero_reserved_bytes);
    RUN_TEST(mount_rejects_name_without_terminator);
    RUN_TEST(mount_rejects_name_padding_and_illegal_characters);
    RUN_TEST(mount_rejects_dot_names_and_duplicate_names);
    RUN_TEST(mount_rejects_nonzero_empty_slot);
    RUN_TEST(mount_rejects_empty_file_with_extent);
    RUN_TEST(mount_rejects_size_count_mismatch);
    RUN_TEST(mount_rejects_out_of_range_and_overlapping_extents);
    RUN_TEST(mount_checks_partial_extent_overlap_and_adjacency);
    RUN_TEST(mount_accepts_size_rounding_and_last_block_extent);
    RUN_TEST(mount_reports_capacity_io_error_and_faults);
    RUN_TEST(mount_reports_read_protocol_and_unknown_status_as_protocol);
    RUN_TEST(mount_never_writes_and_publishes_cache_only_after_validation);
    RUN_TEST(list_returns_slots_in_order_then_end);
    RUN_TEST(list_rejects_cursor_beyond_last_slot);
    RUN_TEST(unmounted_and_uninitialized_instances_reject_list_and_stat);
    RUN_TEST(stat_reports_not_found_and_rejects_invalid_names);
    RUN_TEST(stat_is_case_sensitive_and_accepts_names_at_the_length_limit);
    RUN_TEST(list_rejects_cursor_and_out_aliasing_context);
    RUN_TEST(stat_rejects_name_and_out_aliasing_context);
    RUN_TEST(list_rejects_cursor_overlapping_info);
    RUN_TEST(output_ranges_that_exceed_uintptr_are_rejected);
    RUN_TEST(read_rejects_read_bytes_and_out_aliasing_context);
    RUN_TEST(create_replace_remove_reject_name_and_bytes_aliasing_context);
    RUN_TEST(name_inside_scratch_is_rejected_without_reading_past_it);
    RUN_TEST(read_handles_all_lengths_and_tail_zero);
    RUN_TEST(read_before_mount_and_uninitialized_report_zero);
    RUN_TEST(read_state_priority_precedes_an_invalid_count);
    RUN_TEST(read_rejects_read_bytes_overlapping_out);
    RUN_TEST(read_accepts_disjoint_and_zero_length_outputs);
    RUN_TEST(read_allows_name_shared_with_count_or_out);
    RUN_TEST(read_stops_at_eof_and_truncates);
    RUN_TEST(read_across_block_boundary_returns_exact_bytes);
    RUN_TEST(read_second_block_failure_keeps_prefix_and_sentinel);
    RUN_TEST(read_unknown_callback_status_maps_protocol);
    RUN_TEST(create_writes_entry_and_data_then_reads_back);
    RUN_TEST(create_zero_length_is_empty_file_without_data_write);
    RUN_TEST(create_rejects_duplicate_and_invalid_names_without_io);
    RUN_TEST(create_reports_directory_full_without_writing);
    RUN_TEST(create_uses_lowest_contiguous_free_run);
    RUN_TEST(create_reports_nospace_for_fragmented_free_space);
    RUN_TEST(create_huge_length_reports_nospace_without_writing);
    RUN_TEST(huge_capacity_first_fit_does_not_scan_blocks);
    RUN_TEST(replace_grows_to_new_extent_and_frees_old);
    RUN_TEST(replace_shrinks_and_empties_keeping_slot_and_name);
    RUN_TEST(replace_requires_an_existing_file);
    RUN_TEST(replace_cannot_borrow_its_own_extent_at_peak);
    RUN_TEST(replace_write_failure_keeps_old_directory_on_medium);
    RUN_TEST(remove_clears_slot_with_single_metadata_write);
    RUN_TEST(operations_leave_the_neighbour_file_unchanged);
    RUN_TEST(create_failure_before_each_write_leaves_only_completed_blocks);
    RUN_TEST(create_partial_data_write_leaves_bytes_without_entry);
    RUN_TEST(create_write_error_after_full_block_leaves_no_entry);
    RUN_TEST(create_metadata_write_failure_keeps_old_directory);
    RUN_TEST(partial_metadata_write_is_not_rolled_back);
    RUN_TEST(write_protocol_and_unknown_status_fault_instance);
    RUN_TEST(faulted_instance_refuses_cached_views);
    RUN_TEST(remount_sees_new_old_or_corrupt_metadata);
    RUN_TEST(list_and_stat_honor_busy_guard_from_callback);
    RUN_TEST(create_honors_busy_guard_and_does_not_publish_early);
    RUN_TEST(unmount_clears_cache_and_keeps_instance_usable);
    RUN_TEST(unmount_is_allowed_from_unmounted_and_faulted);
    RUN_TEST(init_rules_reject_incomplete_active_and_faulted_instances);
    return UNITY_END();
}
