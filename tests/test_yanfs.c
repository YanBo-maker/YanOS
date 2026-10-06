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
/* 0024 copy sizes go past the 5000-byte payload, so the larger source lives in
 * static storage: no test allocates a whole block, or 20 KiB, on the stack. */
static uint8_t large_payload[20000u];

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
static YanFsResult probe_rename_result;
static YanFsResult probe_rename_null_result;
static YanFsResult probe_rename_high_result;
static bool probe_rename_null_fs_unchanged;
static uint32_t probe_rename_null_io;
static YanFsResult probe_copy_result;
static YanFsResult probe_copy_null_result;
static YanFsResult probe_copy_high_result;
static bool probe_copy_null_fs_unchanged;
static uint32_t probe_copy_null_io;
static uint32_t probe_cache_crc;
static bool probe_cache_unpublished;

/* Opt-in callback trace. It is disabled unless a test arms it, so the old
 * fixtures keep their exact callback counts. While armed it records, for every
 * device callback in order, whether it was a read or a write, the LBA, and
 * whether the whole metadata cache still equals the directory captured at
 * arm time. The last field is what proves the block-0 callback itself sees the
 * old directory, not just the first callback of the operation. */
#define TRACE_MAX 64u
static bool trace_enabled;
static uint32_t trace_count;
static char trace_kind[TRACE_MAX];
static uint32_t trace_lba[TRACE_MAX];
static bool trace_cache_old[TRACE_MAX];
static uint8_t trace_old_metadata[YAN_FS_BLOCK_SIZE];

static void trace_reset(void)
{
    trace_enabled = false;
    trace_count = 0u;
    memset(trace_kind, 0, sizeof trace_kind);
    memset(trace_lba, 0, sizeof trace_lba);
    memset(trace_cache_old, 0, sizeof trace_cache_old);
    memset(trace_old_metadata, 0, sizeof trace_old_metadata);
}

static void trace_begin(void)
{
    trace_count = 0u;
    memcpy(trace_old_metadata, fs.metadata, YAN_FS_BLOCK_SIZE);
    trace_enabled = true;
}

static void trace_record(char kind, uint32_t lba)
{
    if (!trace_enabled || trace_count >= TRACE_MAX) {
        return;
    }
    trace_kind[trace_count] = kind;
    trace_lba[trace_count] = lba;
    trace_cache_old[trace_count] =
        memcmp(fs.metadata, trace_old_metadata, YAN_FS_BLOCK_SIZE) == 0;
    ++trace_count;
}

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

static void fill_large_payload(uint32_t length, uint8_t seed)
{
    for (uint32_t i = 0; i < length; ++i) {
        large_payload[i] = (uint8_t)(seed + i * 7u + (i >> 8));
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

    /* 0024 state priority: BUSY is the second check for both new primitives, so
     * even a NULL name must answer BUSY, and neither call may touch the cache or
     * reach the device. The snapshot covers the pair of calls each time. */
    memcpy(fs_snapshot, probe_fs, sizeof fs_snapshot);
    io_before = device.reads + device.writes;
    probe_rename_result = yan_fs_rename(probe_fs, "hello.txt", "renamed.txt");
    probe_rename_null_result = yan_fs_rename(probe_fs, NULL, "renamed.txt");
    probe_rename_high_result = yan_fs_rename(
        probe_fs, (const char *)(uintptr_t)(UINTPTR_MAX - 1u), "renamed.txt");
    probe_rename_null_fs_unchanged =
        memcmp(fs_snapshot, probe_fs, sizeof fs_snapshot) == 0;
    probe_rename_null_io = (device.reads + device.writes) - io_before;

    memcpy(fs_snapshot, probe_fs, sizeof fs_snapshot);
    io_before = device.reads + device.writes;
    probe_copy_result = yan_fs_copy(probe_fs, "hello.txt", "copy.txt");
    probe_copy_null_result = yan_fs_copy(probe_fs, NULL, "copy.txt");
    probe_copy_high_result = yan_fs_copy(
        probe_fs, (const char *)(uintptr_t)(UINTPTR_MAX - 1u), "copy.txt");
    probe_copy_null_fs_unchanged =
        memcmp(fs_snapshot, probe_fs, sizeof fs_snapshot) == 0;
    probe_copy_null_io = (device.reads + device.writes) - io_before;

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
    trace_record('R', lba);
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
    trace_record('W', lba);
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

/* Entry view for a second, independently mounted context (the context-boundary
 * fixture does not use the global fs). */
static const uint8_t *context_entry(const YanFs *context, uint32_t slot)
{
    return context->metadata + YAN_FS_HEADER_SIZE + slot * YAN_FS_ENTRY_SIZE;
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

/* Independent directory oracle: the CRC stored on the medium is recomputed from
 * the raw block by this file's own encoder, and the published cache must carry
 * the same sealed bytes. Neither value comes from yanfs.c. */
static void assert_disk_directory_sealed(void)
{
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        fx_block_crc(device.blocks[0]), fx_get_le32(device.blocks[0] + 60u),
        "the directory on the medium must carry its independently computed CRC");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        fx_get_le32(device.blocks[0] + 60u), fx_get_le32(fs.metadata + 60u),
        "the published cache must hold the same sealed directory as the medium");
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
    memset(large_payload, 0, sizeof large_payload);
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
    probe_rename_result = YAN_FS_OK;
    probe_rename_null_result = YAN_FS_OK;
    probe_rename_high_result = YAN_FS_OK;
    probe_rename_null_fs_unchanged = true;
    probe_rename_null_io = 0u;
    probe_copy_result = YAN_FS_OK;
    probe_copy_null_result = YAN_FS_OK;
    probe_copy_high_result = YAN_FS_OK;
    probe_copy_null_fs_unchanged = true;
    probe_copy_null_io = 0u;
    probe_cache_crc = 0u;
    probe_cache_unpublished = false;
    trace_reset();
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
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_CORRUPT, mount_fresh(),
                                  "YFS bad_crc accepted");

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
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_CORRUPT, mount_fresh(),
                                  "YFS extent start accepted");

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
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_CORRUPT, mount_fresh(),
                                  "YFS extent overlap accepted");
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
    TEST_ASSERT_TRUE_MESSAGE(region_is_zero(fs.metadata, YAN_FS_BLOCK_SIZE),
                             "YFS cache published early");
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
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_FAULTED,
                                  yan_fs_list(&fs, &cursor, &info),
                                  "YFS faulted read allowed");
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

/* ========================================================== 0024 rename =====
 * These groups follow docs/specs/0024-file-rename-copy.md:
 *   rename slot/name     original physical slot, normalized name field, one
 *                        directory write, every data block and other slot kept
 *   same-name            OK with zero device callbacks, even full directory/disk
 *   error order          both names before source existence, source before the
 *                        same-name/target rules, state guard before names
 *   inputs               overlapping read-only names allowed, context aliases not
 *   failure              metadata write ERROR/PROTOCOL/unknown faults the
 *                        instance and keeps the last successful cache and the
 *                        source data; medium bytes are not promised
 * ========================================================================= */

static void rename_moves_name_in_original_slot_with_single_metadata_write(void)
{
    mount_empty(16u);
    fill_payload(5000u, 0x80);
    create_ok("note.txt", payload, 5000u); /* slot 0, blocks 1-2 */
    uint32_t start = cache_start(0u);
    uint32_t count = cache_count(0u);
    TEST_ASSERT_EQUAL_UINT32(1u, start);
    TEST_ASSERT_EQUAL_UINT32(2u, count);
    snapshot_medium();
    uint32_t reads = device.reads;
    uint32_t writes = device.writes;

    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_FS_OK, yan_fs_rename(&fs, "note.txt", "diary.txt"),
        "renaming an existing file must succeed");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(reads, device.reads,
        "a rename must not read the medium");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(writes + 1u, device.writes,
        "a rename writes the directory exactly once");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, device.last_write_lba,
        "the single rename write must be block 0");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("diary.txt", (const char *)cache_entry(0u),
        "the renamed file must stay in its original physical slot");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(5000u, cache_size(0u),
        "a rename must not change the file size");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(start, cache_start(0u),
        "a rename must keep the original extent start");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(count, cache_count(0u),
        "a rename must keep the original extent count");
    TEST_ASSERT_TRUE_MESSAGE(cache_slot_empty(1u),
        "a rename must not allocate a new directory slot");
    TEST_ASSERT_TRUE_MESSAGE(
        region_is_zero(cache_entry(0u) + 10u, YAN_FS_NAME_MAX + 1u - 10u),
        "the stored name must be zero-padded inside its 32-byte area");
    TEST_ASSERT_TRUE_MESSAGE(region_equal(medium_before[1], device.blocks[1], 4096u),
        "a rename must not touch data block 1");
    TEST_ASSERT_TRUE_MESSAGE(region_equal(medium_before[2], device.blocks[2], 4096u),
        "a rename must not touch data block 2");

    YanFsInfo info = {0};
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_stat(&fs, "diary.txt", &info));
    TEST_ASSERT_EQUAL_UINT32(5000u, info.size_bytes);
    TEST_ASSERT_EQUAL_INT(YAN_FS_NOT_FOUND, yan_fs_stat(&fs, "note.txt", &info));
    assert_disk_directory_sealed();
}

static void rename_keeps_other_slots_and_data_blocks_unchanged(void)
{
    mount_empty(16u);
    fill_payload(100u, 0x81);
    create_ok("alpha", payload, 100u); /* slot 0, block 1 */
    fill_payload(100u, 0x82);
    create_ok("beta", payload, 100u); /* slot 1, block 2 */
    memcpy(entry_before, cache_entry(1u), YAN_FS_ENTRY_SIZE);
    snapshot_medium();
    uint32_t writes = device.writes;

    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_rename(&fs, "alpha", "gamma"));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(writes + 1u, device.writes,
        "renaming alpha must write only the directory once");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("gamma", (const char *)cache_entry(0u),
        "the renamed entry must keep slot 0");
    TEST_ASSERT_TRUE_MESSAGE(region_equal(entry_before, cache_entry(1u), YAN_FS_ENTRY_SIZE),
        "renaming alpha must not touch beta's directory entry");
    TEST_ASSERT_TRUE_MESSAGE(region_equal(medium_before[1], device.blocks[1], 4096u),
        "renaming alpha must not touch alpha's data block");
    TEST_ASSERT_TRUE_MESSAGE(region_equal(medium_before[2], device.blocks[2], 4096u),
        "renaming alpha must not touch beta's data block");
    TEST_ASSERT_FALSE_MESSAGE(region_equal(medium_before[0], device.blocks[0], 4096u),
        "a rename must actually write the new directory to the medium");
    assert_disk_directory_sealed();
}

static void rename_normalizes_name_area_for_longer_and_shorter_names(void)
{
    static const char longest[] = "0123456789abcdef0123456789abcde"; /* 31 */
    mount_empty(16u);
    create_ok("a", NULL, 0u);
    uint32_t writes = device.writes;

    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_rename(&fs, "a", longest));
    TEST_ASSERT_EQUAL_STRING_MESSAGE(longest, (const char *)cache_entry(0u),
        "a rename to a 31-byte name must store the whole name");
    TEST_ASSERT_TRUE_MESSAGE(region_is_zero(cache_entry(0u) + 31u, 1u),
        "a 31-byte name must still be terminated inside its 32-byte area");
    TEST_ASSERT_EQUAL_UINT32(writes + 1u, device.writes);

    /* Shorter name: the old longer tail must not survive after the terminator. */
    writes = device.writes;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_rename(&fs, longest, "b"));
    TEST_ASSERT_EQUAL_STRING("b", (const char *)cache_entry(0u));
    TEST_ASSERT_TRUE_MESSAGE(region_is_zero(cache_entry(0u) + 2u, 30u),
        "a shorter rename must clear the stale name tail");
    TEST_ASSERT_EQUAL_UINT32(writes + 1u, device.writes);
    assert_disk_directory_sealed();
}

static void rename_same_name_is_zero_io_even_with_full_directory_and_disk(void)
{
    fx_device(16u);
    for (uint32_t slot = 0; slot < YAN_FS_MAX_FILES; ++slot) {
        char name[8];
        name[0] = 'f';
        name[1] = (char)('0' + (slot / 10u));
        name[2] = (char)('0' + (slot % 10u));
        name[3] = '\0';
        if (slot < 15u) {
            /* Blocks 1..15 are all in use, so the disk is full too. */
            fx_entry(device.blocks[0], slot, name, 4096u, 1u + slot, 1u);
        } else {
            fx_entry(device.blocks[0], slot, name, 0u, 0u, 0u);
        }
    }
    fx_seal(device.blocks[0]);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, mount_fresh());
    uint32_t used = 0u;
    for (uint32_t slot = 0; slot < YAN_FS_MAX_FILES; ++slot) {
        if (!cache_slot_empty(slot)) {
            ++used;
        }
    }
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(YAN_FS_MAX_FILES, used,
        "the fixture must fill every directory slot");
    snapshot_medium();
    uint32_t cache_crc = yan_fs_metadata_crc(fs.metadata);
    uint32_t reads = device.reads;
    uint32_t writes = device.writes;

    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK, yan_fs_rename(&fs, "f00", "f00"),
        "renaming a file to its own name must succeed");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(reads, device.reads,
        "a same-name rename must not read the device");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(writes, device.writes,
        "a same-name rename must not write the device, even with a full directory");
    TEST_ASSERT_TRUE_MESSAGE(medium_equal_snapshot(),
        "a same-name rename must leave every medium byte unchanged");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(cache_crc, yan_fs_metadata_crc(fs.metadata),
        "a same-name rename must not touch the metadata cache");

    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK, yan_fs_rename(&fs, "f62", "f62"),
        "renaming an empty file to its own name must also succeed");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(writes, device.writes,
        "a same-name rename of an empty file must not write");
    TEST_ASSERT_TRUE_MESSAGE(medium_equal_snapshot(),
        "a same-name rename of an empty file must leave the medium unchanged");

    /* A rename needs neither a free slot nor a free extent, so it still works
     * when the directory and the disk are both full. */
    reads = device.reads;
    writes = device.writes;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_FS_OK, yan_fs_rename(&fs, "f01", "g01"),
        "a rename must not need a free slot or extent when both are exhausted");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(reads, device.reads,
        "a full-directory rename must not read the device");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(writes + 1u, device.writes,
        "a full-directory rename writes the directory once");
    TEST_ASSERT_EQUAL_STRING("g01", (const char *)cache_entry(1u));
    TEST_ASSERT_EQUAL_UINT32(4096u, cache_size(1u));
    TEST_ASSERT_EQUAL_UINT32(2u, cache_start(1u));
    TEST_ASSERT_EQUAL_UINT32(1u, cache_count(1u));
    assert_disk_directory_sealed();
}

static void rename_rejects_existing_target_and_missing_source_without_io(void)
{
    mount_empty(16u);
    create_ok("a", payload, 1u);
    create_ok("b", payload, 1u);
    snapshot_medium();
    uint32_t writes = device.writes;
    uint32_t reads = device.reads;

    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_EXISTS, yan_fs_rename(&fs, "a", "b"),
        "renaming onto an existing name must not overwrite it");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_NOT_FOUND, yan_fs_rename(&fs, "ghost", "c"),
        "renaming a missing source must report NOT_FOUND");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(writes, device.writes,
        "a rejected rename must perform no medium write");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(reads, device.reads,
        "a rejected rename must perform no medium read");
    TEST_ASSERT_TRUE_MESSAGE(medium_equal_snapshot(),
        "a rejected rename must leave every medium byte unchanged");
    TEST_ASSERT_EQUAL_STRING("a", (const char *)cache_entry(0u));
    TEST_ASSERT_EQUAL_STRING("b", (const char *)cache_entry(1u));
    TEST_ASSERT_EQUAL_INT(YAN_FS_MOUNTED, fs.state);
}

static void rename_validates_both_names_before_source_existence(void)
{
    static const char missing_name[] = "ghost";
    mount_empty(16u);
    snapshot_medium();
    uint32_t writes = device.writes;
    uint32_t reads = device.reads;

    /* The target name is invalid while the source does not exist: the name
     * error must win, proving both names are checked before the source lookup. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_INVALID, yan_fs_rename(&fs, "ghost", "bad/name"),
        "an invalid destination must be rejected before the source lookup");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_INVALID, yan_fs_rename(&fs, "bad/name", "dest"),
        "an invalid source name must be rejected as INVALID");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_INVALID, yan_fs_rename(&fs, "ghost", NULL),
        "a NULL destination name must be INVALID");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_INVALID, yan_fs_rename(&fs, NULL, "dest"),
        "a NULL source name must be INVALID");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_INVALID,
        yan_fs_rename(&fs, "ghost", "0123456789abcdef0123456789abcdef"),
        "a 32-byte unterminated destination must be INVALID");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_INVALID, yan_fs_rename(&fs, "..", "dest"),
        "a rejected dot name must be INVALID");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_NOT_FOUND,
        yan_fs_rename(&fs, missing_name, missing_name),
        "a missing source must not succeed as a same-name rename");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(writes, device.writes,
        "an invalid rename must perform no medium write");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(reads, device.reads,
        "an invalid rename must perform no medium read");
    TEST_ASSERT_TRUE_MESSAGE(medium_equal_snapshot(),
        "an invalid rename must leave every medium byte unchanged");
}

static void rename_and_copy_reject_name_aliasing_context(void)
{
    mount_empty(16u);
    create_ok("hello", NULL, 0u);
    uint32_t before = yan_fs_metadata_crc(fs.metadata);
    uint32_t writes = device.writes;

    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_INVALID,
        yan_fs_rename(&fs, (const char *)(const void *)fs.metadata, "x"),
        "a rename source name inside the context must be rejected");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_INVALID,
        yan_fs_rename(&fs, "hello", (const char *)(const void *)fs.scratch),
        "a rename destination name inside the context must be rejected");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_INVALID,
        yan_fs_rename(&fs, (const char *)(const void *)&fs, "x"),
        "a rename source name at the context head must be rejected");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_INVALID,
        yan_fs_rename(&fs, "hello",
                      (const char *)(const void *)(fs.scratch + YAN_FS_BLOCK_SIZE - 1u)),
        "a rename destination name at the context tail must be rejected");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_INVALID,
        yan_fs_copy(&fs, (const char *)(const void *)(fs.metadata + 63u), "x"),
        "a copy source name inside the context header tail must be rejected");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_INVALID,
        yan_fs_copy(&fs, "hello", (const char *)(const void *)(fs.scratch + 4080u)),
        "a copy destination name inside the context must be rejected");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_INVALID,
        yan_fs_copy(&fs, "hello",
                    (const char *)(const void *)(fs.scratch + YAN_FS_BLOCK_SIZE - 1u)),
        "a copy destination name at the context tail must be rejected");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(writes, device.writes,
        "an aliasing name must perform no medium write");
    TEST_ASSERT_EQUAL_UINT32(before, yan_fs_metadata_crc(fs.metadata));
}

static void rename_and_copy_name_ranges_may_overlap_each_other(void)
{
    mount_empty(16u);
    create_ok("ab", payload, 1u);

    /* The two read-only names come from one buffer and overlap: old "ab" spans
     * bytes 0..2, new "b" is byte 1. */
    char shared[4] = {0};
    memcpy(shared, "ab", 3u);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK, yan_fs_rename(&fs, shared, shared + 1u),
        "two read-only name ranges may overlap each other");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("b", (const char *)cache_entry(0u),
        "the overlapping destination name must be the one stored");

    create_ok("cd", NULL, 0u);
    char shared2[4] = {0};
    memcpy(shared2, "cd", 3u);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK, yan_fs_copy(&fs, shared2, shared2 + 1u),
        "two overlapping read-only copy names must be accepted");
    TEST_ASSERT_EQUAL_STRING("d", (const char *)cache_entry(2u));
    TEST_ASSERT_EQUAL_UINT32(0u, cache_size(2u));

    /* The same string for both names is the tightest overlap case. */
    char same[4] = {0};
    memcpy(same, "d", 2u);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_EXISTS, yan_fs_copy(&fs, same, same),
        "a copy whose two names are one string must report the source as EXISTS");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK, yan_fs_rename(&fs, same, same),
        "a rename whose two names are one string must be a zero-I/O success");
}

static void rename_and_copy_state_order_precedes_name_validation(void)
{
    YanFs virgin;
    memset(&virgin, 0, sizeof virgin);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_INVALID, yan_fs_rename(&virgin, NULL, NULL),
        "an uninitialized context must answer INVALID before the names");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_INVALID, yan_fs_copy(&virgin, NULL, NULL),
        "an uninitialized context must answer INVALID before the names");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_INVALID,
        yan_fs_rename(&virgin, (const char *)(uintptr_t)(UINTPTR_MAX - 1u),
                      (const char *)(void *)virgin.metadata),
        "an uninitialized context must answer INVALID before an unreadable name");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_INVALID,
        yan_fs_copy(&virgin, (const char *)(void *)virgin.metadata,
                    (const char *)(uintptr_t)(UINTPTR_MAX - 1u)),
        "an uninitialized context must answer INVALID before an unreadable name");

    fx_device(16u);
    memset(&fs, 0, sizeof fs);
    init_fs();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_NOT_MOUNTED, yan_fs_rename(&fs, NULL, NULL),
        "an unmounted instance must answer NOT_MOUNTED before the names");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_NOT_MOUNTED, yan_fs_copy(&fs, NULL, NULL),
        "an unmounted instance must answer NOT_MOUNTED before the names");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_NOT_MOUNTED,
        yan_fs_rename(&fs, (const char *)(uintptr_t)(UINTPTR_MAX - 1u),
                      (const char *)(void *)fs.metadata),
        "an unmounted instance must answer NOT_MOUNTED before an unreadable name");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_NOT_MOUNTED,
        yan_fs_copy(&fs, (const char *)(void *)fs.metadata,
                    (const char *)(uintptr_t)(UINTPTR_MAX - 1u)),
        "an unmounted instance must answer NOT_MOUNTED before an unreadable name");

    device.capacity_status = YAN_FS_IO_ERROR;
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO, yan_fs_mount(&fs));
    TEST_ASSERT_EQUAL_INT(YAN_FS_STATE_FAULTED, fs.state);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_FAULTED, yan_fs_rename(&fs, NULL, NULL),
        "a faulted instance must answer FAULTED before the names");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_FAULTED, yan_fs_copy(&fs, NULL, NULL),
        "a faulted instance must answer FAULTED before the names");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_FAULTED,
        yan_fs_rename(&fs, (const char *)(uintptr_t)(UINTPTR_MAX - 1u),
                      (const char *)(void *)fs.metadata),
        "a faulted instance must answer FAULTED before an unreadable name");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_FAULTED,
        yan_fs_copy(&fs, (const char *)(void *)fs.metadata,
                    (const char *)(uintptr_t)(UINTPTR_MAX - 1u)),
        "a faulted instance must answer FAULTED before an unreadable name");
}

static void rename_and_copy_honor_busy_guard_from_callback(void)
{
    mount_empty(16u);
    fill_payload(5u, 0x75);
    create_ok("hello.txt", payload, 5u);
    probe_fs = &fs;
    probe_ran = false;
    uint32_t before = yan_fs_metadata_crc(fs.metadata);

    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_create(&fs, "second", payload, 5u));
    TEST_ASSERT_TRUE_MESSAGE(probe_ran, "the write callback must run the probe");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_BUSY, probe_rename_result,
        "a rename reentered from a device callback must answer BUSY");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_BUSY, probe_rename_null_result,
        "BUSY must win over a NULL rename name");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_BUSY, probe_rename_high_result,
        "BUSY must win over a hostile rename name pointer");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_BUSY, probe_copy_result,
        "a copy reentered from a device callback must answer BUSY");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_BUSY, probe_copy_null_result,
        "BUSY must win over a NULL copy name");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_BUSY, probe_copy_high_result,
        "BUSY must win over a hostile copy name pointer");
    TEST_ASSERT_TRUE_MESSAGE(probe_rename_null_fs_unchanged,
        "a busy rename must leave the whole fs byte-identical");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, probe_rename_null_io,
        "a busy rename must perform no device callback");
    TEST_ASSERT_TRUE_MESSAGE(probe_copy_null_fs_unchanged,
        "a busy copy must leave the whole fs byte-identical");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, probe_copy_null_io,
        "a busy copy must perform no device callback");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(before, probe_cache_crc,
        "a reentered operation must not publish a half-built cache");
    TEST_ASSERT_EQUAL_INT(YAN_FS_MOUNTED, fs.state);
}

/* ============================================================ 0024 copy =====
 * copy slot/extent     first empty slot, low-address first-fit new contiguous
 *                      extent, source slot and extent preserved
 * sizes                empty, 1, 4096, 4097 and greater than 16 KiB binary
 *                      payloads, tail zeroed past the logical end
 * error order          names before source, source before same-name/target,
 *                      then free slot before contiguous extent
 * publication          one directory write last; cache published only after it
 * failure              read/write/metadata ERROR, PROTOCOL and unknowns fault
 *                      the instance and keep the last successful cache
 * ========================================================================= */

static void copy_allocates_first_slot_and_low_first_fit_extent(void)
{
    mount_empty(16u);
    fill_payload(5000u, 0x83);
    create_ok("note.txt", payload, 5000u); /* slot 0, blocks 1-2 */
    snapshot_medium();
    device.reads = 0u;
    device.writes = 0u;

    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK, yan_fs_copy(&fs, "note.txt", "backup.txt"),
        "copying an existing file must succeed");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2u, device.reads,
        "copy must read both source blocks");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(3u, device.writes,
        "copy writes two data blocks and one directory block");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, device.last_write_lba,
        "the directory write must come last, after every data write");

    TEST_ASSERT_EQUAL_STRING_MESSAGE("backup.txt", (const char *)cache_entry(1u),
        "copy must use the first empty directory slot");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(5000u, cache_size(1u),
        "the copy must carry the source size");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(3u, cache_start(1u),
        "the copy must take the lowest free extent after the source");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2u, cache_count(1u),
        "the copy must own its own two-block extent");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, cache_start(0u),
        "the source extent must not move");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2u, cache_count(0u),
        "the source extent must not shrink");

    TEST_ASSERT_TRUE_MESSAGE(region_equal(medium_before[1], device.blocks[1], 4096u),
        "copy must not write the source data block");
    TEST_ASSERT_TRUE_MESSAGE(region_equal(medium_before[2], device.blocks[2], 4096u),
        "copy must not write the source tail block");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(payload, device.blocks[3], 4096u,
        "the copy's first data block must hold the source bytes");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(payload + 4096u, device.blocks[4], 904u,
        "the copy's second data block must hold the source bytes");
    TEST_ASSERT_TRUE_MESSAGE(region_is_zero(device.blocks[4] + 904u, 4096u - 904u),
        "copy must zero the bytes past the logical end of the tail block");
    assert_disk_directory_sealed();
}

static void copy_handles_empty_and_block_rounding_sizes(void)
{
    static const uint32_t sizes[] = {0u, 1u, 4096u, 4097u, 20000u};
    for (uint32_t index = 0u; index < 5u; ++index) {
        uint32_t length = sizes[index];
        fx_device(16u);
        TEST_ASSERT_EQUAL_INT(YAN_FS_OK, mount_fresh());
        fill_large_payload(length, (uint8_t)(0x90u + index));
        uint32_t needed = length / 4096u + ((length % 4096u) != 0u ? 1u : 0u);
        TEST_ASSERT_EQUAL_INT(YAN_FS_OK,
            yan_fs_create(&fs, "src", length > 0u ? large_payload : NULL, length));
        uint32_t source_start = cache_start(0u);
        TEST_ASSERT_EQUAL_UINT32(needed, cache_count(0u));

        device.reads = 0u;
        device.writes = 0u;
        TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK, yan_fs_copy(&fs, "src", "dup"),
            "copy must handle every binary size, including empty and unaligned");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(source_start, cache_start(0u),
            "the source extent start must not move");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(needed, cache_count(0u),
            "the source extent count must not change");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(length, cache_size(0u),
            "the source size must not change");
        TEST_ASSERT_EQUAL_STRING_MESSAGE("dup", (const char *)cache_entry(1u),
            "the copy must land in the first empty slot");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(length, cache_size(1u),
            "the copy must equal the source size");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(needed, cache_count(1u),
            "the copy must own its own extent");

        if (length == 0u) {
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, cache_start(1u),
                "an empty copy must have no extent start");
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, device.reads,
                "an empty copy must not read data blocks");
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, device.writes,
                "an empty copy writes only the directory block");
        } else {
            uint32_t copy_start = cache_start(1u);
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(source_start + needed, copy_start,
                "the copy extent must start right after the source extent");
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(needed, device.reads,
                "copy reads the source once per block");
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(needed + 1u, device.writes,
                "copy writes one block per source block, then the directory");
            for (uint32_t i = 0; i < needed; ++i) {
                uint32_t offset = i * 4096u;
                uint32_t remaining = length - offset;
                uint32_t chunk = remaining < 4096u ? remaining : 4096u;
                TEST_ASSERT_EQUAL_MEMORY_MESSAGE(large_payload + offset,
                    device.blocks[source_start + i], chunk,
                    "the copy must not write the source data block");
                TEST_ASSERT_EQUAL_MEMORY_MESSAGE(large_payload + offset,
                    device.blocks[copy_start + i], chunk,
                    "every copied data block must equal the source block");
                TEST_ASSERT_TRUE_MESSAGE(
                    region_is_zero(device.blocks[copy_start + i] + chunk, 4096u - chunk),
                    "copy must zero the bytes past the logical end of the tail block");
            }
        }
        assert_disk_directory_sealed();
    }
}

static void copy_preserves_source_after_modifying_the_copy(void)
{
    mount_empty(16u);
    fill_payload(5000u, 0x84);
    create_ok("master", payload, 5000u);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK, yan_fs_copy(&fs, "master", "draft"),
        "copying must create an independent file");

    fill_payload(3u, 0x85);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK, yan_fs_replace(&fs, "draft", payload, 3u),
        "the copy must be writable on its own");
    TEST_ASSERT_EQUAL_UINT32(3u, cache_size(1u));

    fill_payload(5000u, 0x84);
    memset(readback, 0xa5, sizeof readback);
    uint32_t got = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK,
        yan_fs_read(&fs, "master", 0u, readback, 5000u, &got));
    TEST_ASSERT_EQUAL_UINT32(5000u, got);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(payload, readback, 5000u,
        "changing the copy must not change the source bytes");
    TEST_ASSERT_EQUAL_UINT32(1u, cache_start(0u));
    TEST_ASSERT_EQUAL_UINT32(2u, cache_count(0u));
}

static void copy_reports_directory_full_before_nospace(void)
{
    fx_device(2u);
    for (uint32_t slot = 0; slot < YAN_FS_MAX_FILES; ++slot) {
        char name[8];
        name[0] = 'f';
        name[1] = (char)('0' + (slot / 10u));
        name[2] = (char)('0' + (slot % 10u));
        name[3] = '\0';
        if (slot == 0u) {
            fx_entry(device.blocks[0], slot, name, 4096u, 1u, 1u);
        } else {
            fx_entry(device.blocks[0], slot, name, 0u, 0u, 0u);
        }
    }
    fx_seal(device.blocks[0]);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, mount_fresh());
    snapshot_medium();
    uint32_t writes = device.writes;
    uint32_t reads = device.reads;

    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_DIRECTORY_FULL,
        yan_fs_copy(&fs, "f00", "copy"),
        "a full directory must win over the missing extent");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(writes, device.writes,
        "a directory-full copy must not write");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(reads, device.reads,
        "a directory-full copy must not read");
    TEST_ASSERT_TRUE(medium_equal_snapshot());
}

static void copy_reports_nospace_for_fragmented_free_space(void)
{
    mount_empty(6u);
    fill_payload(5000u, 0x86);
    create_ok("src", payload, 5000u); /* blocks 1-2 */
    create_ok("x", payload, 1u);      /* block 3 */
    create_ok("y", payload, 1u);      /* block 4 */
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_remove(&fs, "x")); /* hole at block 3 */
    snapshot_medium();
    uint32_t writes = device.writes;
    uint32_t reads = device.reads;

    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_NOSPACE, yan_fs_copy(&fs, "src", "dup"),
        "a copy must not borrow or split a non-contiguous free extent");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(writes, device.writes,
        "a no-space copy must not write");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(reads, device.reads,
        "a no-space copy must not read");
    TEST_ASSERT_TRUE(medium_equal_snapshot());
    TEST_ASSERT_EQUAL_STRING("src", (const char *)cache_entry(0u));
    TEST_ASSERT_TRUE(cache_slot_empty(1u));
}

static void copy_empty_file_adds_an_empty_entry_without_data_io(void)
{
    mount_empty(16u);
    create_ok("empty", NULL, 0u); /* slot 0, no extent */
    snapshot_medium();
    uint32_t reads = device.reads;
    uint32_t writes = device.writes;

    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK, yan_fs_copy(&fs, "empty", "empty2"),
        "copying an empty file must succeed");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(reads, device.reads,
        "an empty copy must not read data blocks");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(writes + 1u, device.writes,
        "an empty copy writes only the directory block");
    TEST_ASSERT_EQUAL_STRING("empty2", (const char *)cache_entry(1u));
    TEST_ASSERT_EQUAL_UINT32(0u, cache_size(1u));
    TEST_ASSERT_EQUAL_UINT32(0u, cache_start(1u));
    TEST_ASSERT_EQUAL_UINT32(0u, cache_count(1u));
    assert_disk_directory_sealed();
}

static void copy_validates_both_names_before_source_existence(void)
{
    static const char missing_name[] = "ghost";
    mount_empty(16u);
    snapshot_medium();
    uint32_t writes = device.writes;
    uint32_t reads = device.reads;

    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_INVALID, yan_fs_copy(&fs, "ghost", "bad/name"),
        "an invalid copy destination must be rejected before the source lookup");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_INVALID, yan_fs_copy(&fs, "bad/name", "dest"),
        "an invalid copy source name must be rejected as INVALID");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_INVALID, yan_fs_copy(&fs, "ghost", NULL),
        "a NULL copy destination must be INVALID");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_NOT_FOUND, yan_fs_copy(&fs, "ghost", "dest"),
        "a missing copy source with valid names must be NOT_FOUND");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_NOT_FOUND,
        yan_fs_copy(&fs, missing_name, missing_name),
        "a missing source must not report EXISTS for a same-name copy");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(writes, device.writes,
        "an invalid copy must perform no medium write");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(reads, device.reads,
        "an invalid copy must perform no medium read");
    TEST_ASSERT_TRUE_MESSAGE(medium_equal_snapshot(),
        "an invalid copy must leave every medium byte unchanged");
}

static void copy_same_name_and_existing_target_are_rejected_without_io(void)
{
    mount_empty(16u);
    create_ok("a", payload, 1u);
    create_ok("b", payload, 1u);
    snapshot_medium();
    uint32_t writes = device.writes;
    uint32_t reads = device.reads;

    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_EXISTS, yan_fs_copy(&fs, "a", "a"),
        "copying a file onto its own name must be rejected as EXISTS");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_EXISTS, yan_fs_copy(&fs, "a", "b"),
        "copy must not overwrite an existing destination");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(writes, device.writes,
        "a rejected copy must perform no medium write");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(reads, device.reads,
        "a rejected copy must perform no medium read");
    TEST_ASSERT_TRUE(medium_equal_snapshot());
    TEST_ASSERT_EQUAL_UINT32(1u, cache_size(0u));
    TEST_ASSERT_EQUAL_UINT32(1u, cache_size(1u));
}

static void copy_read_failure_faults_without_writing_metadata(void)
{
    for (uint32_t fail_at = 1u; fail_at <= 2u; ++fail_at) {
        fx_device(16u);
        TEST_ASSERT_EQUAL_INT(YAN_FS_OK, mount_fresh());
        fill_payload(5000u, 0x87);
        create_ok("src", payload, 5000u);
        snapshot_medium();
        uint32_t cache_crc = yan_fs_metadata_crc(fs.metadata);
        device.reads = 0u;
        device.writes = 0u;
        device.read_fail_at = fail_at;
        device.read_fail_code = YAN_FS_IO_ERROR;

        TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_IO, yan_fs_copy(&fs, "src", "dup"),
            "a source read error must surface as YAN_FS_IO");
        TEST_ASSERT_EQUAL_INT(YAN_FS_STATE_FAULTED, fs.state);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(cache_crc, yan_fs_metadata_crc(fs.metadata),
            "a failed copy must keep the last successful directory in the cache");
        TEST_ASSERT_TRUE_MESSAGE(cache_slot_empty(1u),
            "a failed copy must not add a directory entry to the cache");
        TEST_ASSERT_TRUE_MESSAGE(region_equal(medium_before[1], device.blocks[1], 4096u),
            "a failed copy must not write the source data block");
        TEST_ASSERT_TRUE_MESSAGE(region_equal(medium_before[2], device.blocks[2], 4096u),
            "a failed copy must not write the source tail block");
        TEST_ASSERT_TRUE_MESSAGE(region_equal(medium_before[0], device.blocks[0], 4096u),
            "a failed source read must leave the directory on the medium unchanged");
        TEST_ASSERT_FALSE_MESSAGE(fs.busy, "a failed copy must clear busy");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(fail_at, device.reads,
            "a failed source read must stop the block loop");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(fail_at - 1u, device.writes,
            "no target write may follow a failed source read");
        uint32_t reads = device.reads;
        uint32_t writes = device.writes;
        TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_FAULTED, yan_fs_copy(&fs, "src", "dup"),
            "a faulted instance must refuse a follow-up copy");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(reads, device.reads,
            "a faulted copy must not issue follow-up reads");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(writes, device.writes,
            "a faulted copy must not issue follow-up writes");
        device.read_fail_at = 0u;
    }
}

static void copy_write_failure_faults_and_leaves_no_entry(void)
{
    for (uint32_t fail_at = 1u; fail_at <= 3u; ++fail_at) {
        fx_device(16u);
        TEST_ASSERT_EQUAL_INT(YAN_FS_OK, mount_fresh());
        fill_payload(5000u, 0x88);
        create_ok("src", payload, 5000u);
        snapshot_medium();
        uint32_t cache_crc = yan_fs_metadata_crc(fs.metadata);
        device.reads = 0u;
        device.writes = 0u;
        device.write_fail_at = fail_at;
        device.write_partial = 0u;
        device.write_fail_code = YAN_FS_IO_ERROR;

        TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_IO, yan_fs_copy(&fs, "src", "dup"),
            "a target write error must surface as YAN_FS_IO");
        TEST_ASSERT_EQUAL_INT(YAN_FS_STATE_FAULTED, fs.state);
        /* Read a source block, then write its target block: at the failing data
         * write the reads are exactly the ones already consumed, so a mutant
         * that reads every block before writing any is rejected here. */
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(fail_at <= 2u ? fail_at : 2u, device.reads,
            "copy must read a source block before writing its target block");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(fail_at, device.writes,
            "a failed target write must stop the block loop");
        TEST_ASSERT_FALSE_MESSAGE(fs.busy, "a failed copy must clear busy");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(cache_crc, yan_fs_metadata_crc(fs.metadata),
            "a failed copy must keep the last successful directory in the cache");
        TEST_ASSERT_TRUE_MESSAGE(cache_slot_empty(1u),
            "a failed copy must not publish a directory entry");
        TEST_ASSERT_TRUE_MESSAGE(region_equal(medium_before[1], device.blocks[1], 4096u),
            "a failed copy must not write the source data block");
        TEST_ASSERT_TRUE_MESSAGE(region_equal(medium_before[2], device.blocks[2], 4096u),
            "a failed copy must not write the source tail block");
        if (fail_at <= 2u) {
            TEST_ASSERT_TRUE_MESSAGE(
                region_equal(medium_before[0], device.blocks[0], 4096u),
                "a data-write failure must not touch the directory on the medium");
        }
        uint32_t reads = device.reads;
        uint32_t writes = device.writes;
        TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_FAULTED, yan_fs_copy(&fs, "src", "dup"),
            "a faulted instance must refuse a follow-up copy");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(reads, device.reads,
            "a faulted copy must not issue follow-up reads");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(writes, device.writes,
            "a faulted copy must not issue follow-up writes");
        device.write_fail_at = 0u;
    }
}

static void copy_metadata_write_failure_keeps_old_cache(void)
{
    static const YanFsIoResult codes[] = {
        YAN_FS_IO_ERROR, YAN_FS_IO_PROTOCOL, (YanFsIoResult)68
    };
    static const YanFsResult expected_results[] = {
        YAN_FS_IO, YAN_FS_PROTOCOL, YAN_FS_PROTOCOL
    };
    for (uint32_t index = 0u; index < 3u; ++index) {
        fx_device(16u);
        TEST_ASSERT_EQUAL_INT(YAN_FS_OK, mount_fresh());
        fill_payload(5000u, 0x8a);
        create_ok("src", payload, 5000u);
        snapshot_medium();
        uint32_t cache_crc = yan_fs_metadata_crc(fs.metadata);
        device.reads = 0u;
        device.writes = 0u;
        device.write_fail_at = 3u;
        device.write_partial = 0u;
        device.write_fail_code = codes[index];

        TEST_ASSERT_EQUAL_INT_MESSAGE(expected_results[index], yan_fs_copy(&fs, "src", "dup"),
            "a failed directory write must keep its ERROR/PROTOCOL mapping");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(2u, device.reads,
            "copy must have read both source blocks before the directory write");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(3u, device.writes,
            "copy must attempt the directory write only after both data writes");
        TEST_ASSERT_EQUAL_INT(YAN_FS_STATE_FAULTED, fs.state);
        TEST_ASSERT_FALSE_MESSAGE(fs.busy, "a failed copy must clear busy");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(cache_crc, yan_fs_metadata_crc(fs.metadata),
            "a failed directory write must keep the previous directory in the cache");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(5000u, cache_size(0u),
            "the source must still be visible in the cache");
        TEST_ASSERT_TRUE_MESSAGE(cache_slot_empty(1u),
            "the copy must not be published after a failed directory write");
        TEST_ASSERT_TRUE_MESSAGE(region_equal(medium_before[1], device.blocks[1], 4096u),
            "the source data block must be unchanged by a failed copy");
        TEST_ASSERT_TRUE_MESSAGE(region_equal(medium_before[2], device.blocks[2], 4096u),
            "the source tail block must be unchanged by a failed copy");
        /* Block 0 on the medium is deliberately not asserted: 0024 does not
         * promise what a failed metadata write leaves there. */
        uint32_t reads = device.reads;
        uint32_t writes = device.writes;
        TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_FAULTED, yan_fs_copy(&fs, "src", "dup"),
            "a faulted instance must refuse a follow-up copy");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(reads, device.reads,
            "a faulted copy must not issue follow-up reads");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(writes, device.writes,
            "a faulted copy must not issue follow-up writes");
        device.write_fail_at = 0u;
        device.write_fail_code = YAN_FS_IO_OK;
    }
}

static void copy_write_and_read_protocol_and_unknown_status_fault_instance(void)
{
    /* Target data write reports PROTOCOL. */
    mount_empty(16u);
    fill_payload(5000u, 0x89);
    create_ok("src", payload, 5000u);
    device.writes = 0u;
    device.write_fail_at = 1u;
    device.write_partial = 0u;
    device.write_fail_code = YAN_FS_IO_PROTOCOL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_PROTOCOL, yan_fs_copy(&fs, "src", "dup"),
        "a PROTOCOL target write must map to YAN_FS_PROTOCOL");
    TEST_ASSERT_EQUAL_INT(YAN_FS_STATE_FAULTED, fs.state);
    TEST_ASSERT_FALSE_MESSAGE(fs.busy, "a PROTOCOL copy must clear busy");
    device.write_fail_at = 0u;
    device.write_fail_code = YAN_FS_IO_OK;

    /* Target data write reports an unknown status. */
    mount_empty(16u);
    create_ok("src", payload, 5000u);
    device.writes = 0u;
    device.write_fail_at = 1u;
    device.write_partial = 0u;
    device.write_fail_code = (YanFsIoResult)66;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_PROTOCOL, yan_fs_copy(&fs, "src", "dup"),
        "an unknown target write status must map to YAN_FS_PROTOCOL");
    TEST_ASSERT_EQUAL_INT(YAN_FS_STATE_FAULTED, fs.state);
    TEST_ASSERT_FALSE_MESSAGE(fs.busy, "an unknown-status copy must clear busy");
    device.write_fail_at = 0u;
    device.write_fail_code = YAN_FS_IO_OK;

    /* Source read reports PROTOCOL. */
    mount_empty(16u);
    create_ok("src", payload, 5000u);
    device.reads = 0u;
    device.read_fail_at = 1u;
    device.read_fail_code = YAN_FS_IO_PROTOCOL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_PROTOCOL, yan_fs_copy(&fs, "src", "dup"),
        "a PROTOCOL source read must map to YAN_FS_PROTOCOL");
    TEST_ASSERT_EQUAL_INT(YAN_FS_STATE_FAULTED, fs.state);
    TEST_ASSERT_FALSE_MESSAGE(fs.busy, "a PROTOCOL read must clear busy");
    device.read_fail_at = 0u;
    device.read_fail_code = YAN_FS_IO_OK;

    /* Source read reports an unknown status. */
    mount_empty(16u);
    create_ok("src", payload, 5000u);
    device.reads = 0u;
    device.read_fail_at = 1u;
    device.read_fail_code = (YanFsIoResult)67;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_PROTOCOL, yan_fs_copy(&fs, "src", "dup"),
        "an unknown source read status must map to YAN_FS_PROTOCOL");
    TEST_ASSERT_EQUAL_INT(YAN_FS_STATE_FAULTED, fs.state);
    TEST_ASSERT_FALSE_MESSAGE(fs.busy, "an unknown-status read must clear busy");
    device.read_fail_at = 0u;
    device.read_fail_code = YAN_FS_IO_OK;
}

static void copy_never_publishes_cache_before_metadata_write(void)
{
    mount_empty(16u);
    fill_payload(5000u, 0x8b);
    create_ok("src", payload, 5000u);
    uint32_t before = yan_fs_metadata_crc(fs.metadata);
    probe_fs = &fs;
    probe_ran = false;

    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_copy(&fs, "src", "dup"));
    TEST_ASSERT_TRUE_MESSAGE(probe_ran, "the source read callback must run the probe");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(before, probe_cache_crc,
        "the directory cache must not be published while copy data moves");
    TEST_ASSERT_EQUAL_STRING("dup", (const char *)cache_entry(1u));
    assert_disk_directory_sealed();
}

/* ------------------------------------------------ 0024 strengthened cases ---
 * Evidence the first RED round did not separate:
 *   tail/padding     target tail zeroed while the source's own dirty padding
 *                    and whole data blocks survive
 *   callback log     every read/write LBA in order, and the whole cache still
 *                    old at every callback, block 0 included
 *   allocation       holes below the source and before the occupied sibling
 *   rename failure   ERROR/PROTOCOL/unknown full and partial metadata writes
 *   ceiling          UINT32_MAX size must NOSPACE before any data callback
 *   names            length limit, context-boundary terminator, state priority
 * ========================================================================= */

static void copy_zeroes_target_tail_while_source_padding_survives(void)
{
    mount_empty(16u);
    fill_payload(5000u, 0x8b);
    create_ok("src", payload, 5000u); /* blocks 1-2 */
    /* Dirty the source's own tail padding on the medium. A copy must not leak
     * it into the target and must not rewrite the source block. */
    for (uint32_t i = 904u; i < YAN_FS_BLOCK_SIZE; ++i) {
        device.blocks[2][i] = (uint8_t)(0xa5u + i);
    }
    snapshot_medium();
    device.reads = 0u;
    device.writes = 0u;

    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK, yan_fs_copy(&fs, "src", "dup"),
        "copy must succeed when the source tail padding is non-zero");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2u, device.reads, "copy must read both source blocks");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(3u, device.writes,
        "copy must write both target blocks and the directory");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(payload, device.blocks[3], 4096u,
        "the copy's full first block must equal the source");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(payload + 4096u, device.blocks[4], 904u,
        "the copy's logical tail bytes must equal the source");
    TEST_ASSERT_TRUE_MESSAGE(region_is_zero(device.blocks[4] + 904u, 4096u - 904u),
        "the copy must zero its tail even when the source padding is non-zero");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(medium_before[1], device.blocks[1], 4096u,
        "copy must leave the source's first block whole");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(medium_before[2], device.blocks[2], 4096u,
        "copy must leave the source's dirty tail block whole");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)((0xa5u + 904u) & 0xffu), device.blocks[2][904],
        "the source's non-zero padding must survive the copy");
    assert_disk_directory_sealed();
}

static void copy_callback_log_is_interleaved_and_cache_stays_old(void)
{
    mount_empty(16u);
    fill_payload(5000u, 0x8d);
    create_ok("src", payload, 5000u);
    trace_begin();
    device.reads = 0u;
    device.writes = 0u;

    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_copy(&fs, "src", "dup"));
    trace_enabled = false;
    static const char kinds[5] = {'R', 'W', 'R', 'W', 'W'};
    static const uint32_t lbas[5] = {1u, 3u, 2u, 4u, 0u};
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(5u, trace_count,
        "copy of two blocks must log read/write/read/write/directory");
    for (uint32_t i = 0u; i < 5u; ++i) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(kinds[i], trace_kind[i],
            "each source read must be followed by that block's target write");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(lbas[i], trace_lba[i],
            "each copy callback must use the expected logical block");
        TEST_ASSERT_TRUE_MESSAGE(trace_cache_old[i],
            "every copy callback, block 0 included, must still see the old cache");
    }
    TEST_ASSERT_EQUAL_STRING("dup", (const char *)cache_entry(1u));
    assert_disk_directory_sealed();
}

static void rename_callback_log_is_one_directory_write_seeing_old_cache(void)
{
    mount_empty(16u);
    fill_payload(5000u, 0x8c);
    create_ok("old", payload, 5000u);
    trace_begin();
    device.reads = 0u;
    device.writes = 0u;

    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_rename(&fs, "old", "new"));
    trace_enabled = false;
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, trace_count,
        "rename must issue exactly one medium callback");
    TEST_ASSERT_EQUAL_INT_MESSAGE('W', trace_kind[0], "the rename callback must be a write");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, trace_lba[0], "the rename write must be block 0");
    TEST_ASSERT_TRUE_MESSAGE(trace_cache_old[0],
        "the rename metadata write must still see the old directory cache");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, device.reads,
        "rename must not read any data block");
}

static void copy_empty_callback_log_is_one_directory_write(void)
{
    mount_empty(16u);
    create_ok("empty", NULL, 0u);
    trace_begin();

    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_copy(&fs, "empty", "empty2"));
    trace_enabled = false;
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, trace_count,
        "an empty copy must issue only the directory write");
    TEST_ASSERT_EQUAL_INT('W', trace_kind[0]);
    TEST_ASSERT_EQUAL_UINT32(0u, trace_lba[0]);
    TEST_ASSERT_TRUE_MESSAGE(trace_cache_old[0],
        "the empty copy's metadata write must still see the old cache");
}

static void copy_uses_low_hole_below_source_and_slot_below_sibling(void)
{
    mount_empty(16u);
    fill_payload(1u, 0x8e);
    create_ok("low", payload, 1u); /* slot 0, block 1 */
    fill_payload(1u, 0x8f);
    create_ok("src", payload, 1u); /* slot 1, block 2 */
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_remove(&fs, "low"));
    snapshot_medium();
    device.reads = 0u;
    device.writes = 0u;

    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK, yan_fs_copy(&fs, "src", "dst"),
        "copy must allocate when the holes are below the source");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("dst", (const char *)cache_entry(0u),
        "copy must take the first empty slot, not the slot after the sibling");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, cache_start(0u),
        "copy must take the low first-fit hole below the source");
    TEST_ASSERT_EQUAL_UINT32(1u, cache_count(0u));
    TEST_ASSERT_EQUAL_STRING("src", (const char *)cache_entry(1u));
    TEST_ASSERT_EQUAL_UINT32(2u, cache_start(1u));
    TEST_ASSERT_TRUE_MESSAGE(cache_slot_empty(2u),
        "copy must not append a later directory slot");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x8fu, device.blocks[1][0],
        "the copy's bytes must come from the source");
    assert_disk_directory_sealed();
}

static void rename_metadata_failure_keeps_cache_without_rollback_promise(void)
{
    static const YanFsIoResult codes[] = {
        YAN_FS_IO_ERROR, YAN_FS_IO_PROTOCOL, (YanFsIoResult)69
    };
    static const YanFsResult expected_results[] = {
        YAN_FS_IO, YAN_FS_PROTOCOL, YAN_FS_PROTOCOL
    };
    static const uint32_t partials[] = {0u, 100u};
    for (uint32_t index = 0u; index < 3u; ++index) {
        for (uint32_t p = 0u; p < 2u; ++p) {
            fx_device(16u);
            TEST_ASSERT_EQUAL_INT(YAN_FS_OK, mount_fresh());
            fill_payload(5000u, 0x81);
            create_ok("old", payload, 5000u);
            snapshot_medium();
            uint32_t cache_crc = yan_fs_metadata_crc(fs.metadata);
            device.writes = 0u;
            device.write_fail_at = 1u;
            device.write_partial = partials[p];
            device.write_fail_code = codes[index];

            TEST_ASSERT_EQUAL_INT_MESSAGE(expected_results[index],
                yan_fs_rename(&fs, "old", "new"),
                "a failed rename directory write must keep its ERROR/PROTOCOL mapping");
            TEST_ASSERT_EQUAL_INT(YAN_FS_STATE_FAULTED, fs.state);
            TEST_ASSERT_FALSE_MESSAGE(fs.busy, "a failed rename must clear busy");
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(cache_crc, yan_fs_metadata_crc(fs.metadata),
                "a failed rename must keep the previous directory in the cache");
            TEST_ASSERT_EQUAL_STRING_MESSAGE("old", (const char *)cache_entry(0u),
                "the cache must still hold the old name after a failed rename");
            TEST_ASSERT_EQUAL_UINT32(5000u, cache_size(0u));
            TEST_ASSERT_TRUE_MESSAGE(region_equal(medium_before[1], device.blocks[1], 4096u),
                "a failed rename must not write the source data block");
            TEST_ASSERT_TRUE_MESSAGE(region_equal(medium_before[2], device.blocks[2], 4096u),
                "a failed rename must not write the source tail block");
            /* Block 0 on the medium is deliberately not asserted: 0024 does not
             * promise what a failed or partial metadata write leaves there. */
            uint32_t reads = device.reads;
            uint32_t writes = device.writes;
            TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_FAULTED,
                yan_fs_rename(&fs, "old", "new"),
                "a faulted instance must refuse a follow-up rename");
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(reads, device.reads,
                "a faulted rename must not issue follow-up reads");
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(writes, device.writes,
                "a faulted rename must not issue follow-up writes");
            device.write_fail_at = 0u;
            device.write_fail_code = YAN_FS_IO_OK;
        }
    }
}

static void copy_rejects_max_length_nospace_before_data_callbacks(void)
{
    /* Independent seeded directory: one file of size UINT32_MAX needs exactly
     * 1048576 blocks, and a device of 1 + 1048576 blocks leaves no contiguous
     * room for the copy. No 4 GiB allocation happens anywhere. */
    fx_device(UINT32_C(1) + UINT32_C(1048576));
    fx_entry(device.blocks[0], 0u, "huge", UINT32_MAX, 1u, UINT32_C(1048576));
    fx_seal(device.blocks[0]);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, mount_fresh());
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, cache_size(0u));
    snapshot_medium();
    uint32_t reads = device.reads;
    uint32_t writes = device.writes;

    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_NOSPACE, yan_fs_copy(&fs, "huge", "dup"),
        "the UINT32_MAX ceiling must not wrap into a false allocation");
    TEST_ASSERT_FALSE_MESSAGE(fs.busy, "a NOSPACE copy must clear busy");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(reads, device.reads,
        "a max-length NOSPACE copy must not read the device");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(writes, device.writes,
        "a max-length NOSPACE copy must not write the device");
    TEST_ASSERT_TRUE(medium_equal_snapshot());
    TEST_ASSERT_EQUAL_INT(YAN_FS_MOUNTED, fs.state);
}

static void rename_and_copy_name_length_limits(void)
{
    static const char name31a[] = "0123456789abcdef0123456789abcde";
    static const char name31b[] = "0123456789abcdef0123456789abcdf";
    static const char name32[] = "0123456789abcdef0123456789abcdef";
    TEST_ASSERT_EQUAL_UINT32(31u, (uint32_t)strlen(name31a));
    TEST_ASSERT_EQUAL_UINT32(31u, (uint32_t)strlen(name31b));
    TEST_ASSERT_EQUAL_UINT32(32u, (uint32_t)strlen(name32));

    mount_empty(16u);
    create_ok(name31a, NULL, 0u);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK, yan_fs_rename(&fs, name31a, name31b),
        "rename must accept a 31-byte source and destination");
    TEST_ASSERT_EQUAL_STRING(name31b, (const char *)cache_entry(0u));
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK, yan_fs_copy(&fs, name31b, name31a),
        "copy must accept a 31-byte source and destination");
    TEST_ASSERT_EQUAL_STRING(name31a, (const char *)cache_entry(1u));

    uint32_t reads = device.reads;
    uint32_t writes = device.writes;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_INVALID, yan_fs_rename(&fs, name31b, name32),
        "a 32-byte unterminated rename destination must be INVALID");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_INVALID, yan_fs_rename(&fs, name32, name31b),
        "a 32-byte unterminated rename source must be INVALID");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_INVALID, yan_fs_copy(&fs, name31b, name32),
        "a 32-byte unterminated copy destination must be INVALID");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_INVALID, yan_fs_copy(&fs, name32, name31b),
        "a 32-byte unterminated copy source must be INVALID");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(reads, device.reads,
        "a rejected over-long name must perform no medium read");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(writes, device.writes,
        "a rejected over-long name must perform no medium write");
}

static void rename_and_copy_accept_name_terminating_before_context(void)
{
    static struct {
        char name[8];
        YanFs context;
    } adjacent;
    fx_device(16u);
    fx_entry(device.blocks[0], 0u, "abcdefg", 0u, 0u, 0u);
    fx_seal(device.blocks[0]);
    memset(&adjacent, 0, sizeof adjacent);
    memcpy(adjacent.name, "abcdefg", 8u);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        1u, (uint32_t)((uintptr_t)&adjacent.context - (uintptr_t)&adjacent.name[7]),
        "the fixture must place the context right after the name terminator");
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_init(&adjacent.context, fx_backend()));
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_mount(&adjacent.context));
    YanFsInfo info = {0};
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_stat(&adjacent.context, adjacent.name, &info));
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK,
        yan_fs_rename(&adjacent.context, adjacent.name, "other"),
        "a legal name whose terminator touches the context must be accepted");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK,
        yan_fs_copy(&adjacent.context, "other", adjacent.name),
        "a copy destination whose terminator touches the context must be accepted");
    TEST_ASSERT_EQUAL_STRING("other", (const char *)context_entry(&adjacent.context, 0u));
    TEST_ASSERT_EQUAL_STRING(adjacent.name,
                             (const char *)context_entry(&adjacent.context, 1u));
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
    RUN_TEST(rename_moves_name_in_original_slot_with_single_metadata_write);
    RUN_TEST(rename_keeps_other_slots_and_data_blocks_unchanged);
    RUN_TEST(rename_normalizes_name_area_for_longer_and_shorter_names);
    RUN_TEST(rename_same_name_is_zero_io_even_with_full_directory_and_disk);
    RUN_TEST(rename_rejects_existing_target_and_missing_source_without_io);
    RUN_TEST(rename_validates_both_names_before_source_existence);
    RUN_TEST(rename_and_copy_reject_name_aliasing_context);
    RUN_TEST(rename_and_copy_name_ranges_may_overlap_each_other);
    RUN_TEST(rename_and_copy_state_order_precedes_name_validation);
    RUN_TEST(rename_and_copy_honor_busy_guard_from_callback);
    RUN_TEST(copy_allocates_first_slot_and_low_first_fit_extent);
    RUN_TEST(copy_handles_empty_and_block_rounding_sizes);
    RUN_TEST(copy_preserves_source_after_modifying_the_copy);
    RUN_TEST(copy_reports_directory_full_before_nospace);
    RUN_TEST(copy_reports_nospace_for_fragmented_free_space);
    RUN_TEST(copy_empty_file_adds_an_empty_entry_without_data_io);
    RUN_TEST(copy_validates_both_names_before_source_existence);
    RUN_TEST(copy_same_name_and_existing_target_are_rejected_without_io);
    RUN_TEST(copy_read_failure_faults_without_writing_metadata);
    RUN_TEST(copy_write_failure_faults_and_leaves_no_entry);
    RUN_TEST(copy_metadata_write_failure_keeps_old_cache);
    RUN_TEST(copy_write_and_read_protocol_and_unknown_status_fault_instance);
    RUN_TEST(copy_never_publishes_cache_before_metadata_write);
    RUN_TEST(copy_zeroes_target_tail_while_source_padding_survives);
    RUN_TEST(copy_callback_log_is_interleaved_and_cache_stays_old);
    RUN_TEST(rename_callback_log_is_one_directory_write_seeing_old_cache);
    RUN_TEST(copy_empty_callback_log_is_one_directory_write);
    RUN_TEST(copy_uses_low_hole_below_source_and_slot_below_sibling);
    RUN_TEST(rename_metadata_failure_keeps_cache_without_rollback_promise);
    RUN_TEST(copy_rejects_max_length_nospace_before_data_callbacks);
    RUN_TEST(rename_and_copy_name_length_limits);
    RUN_TEST(rename_and_copy_accept_name_terminating_before_context);
    return UNITY_END();
}
