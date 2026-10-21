/* 0036-R8-AC2: randomized directory operations against a linear oracle.
 *
 * This is a standalone native suite. It compiles the real os/yanfs.c with
 * -DYAN_FS_TEST_HOOKS so the production binary search (find_entry) and the
 * published name_order table can be compared with an independent linear
 * directory oracle. N=2047 (D=32), fixed seed, exactly 10000 create / delete /
 * rename / copy operations.
 *
 * The oracle is a slot array plus a dense live-slot list. It is built only
 * from the documented API results, never from name_order or find_entry.
 * After every operation the test scans the real in-memory directory (the
 * caller-owned cache) and checks:
 *   - every slot's name and size agree with the oracle;
 *   - name_order_count equals the number of non-empty slots;
 *   - every name_order entry names a non-empty slot, has no duplicate slot,
 *     and the sequence is strictly increasing by name.
 * Since the table holds exactly as many distinct, valid, non-empty slots as
 * the directory has, those checks also prove that no non-empty slot is
 * missing. The final sweep additionally sorts the expected slots and compares
 * the table entry by entry, then calls yan_fs_stat and the production
 * find_entry for every live name.
 *
 * A separate fault test fills the directory, deletes a slot above block 0,
 * then creates an empty file with the second metadata write (the block-0 CRC
 * write) forced to fail. The first write (the entry block) succeeded, so the
 * mutation is visible in the in-memory cache while the instance is FAULTED.
 * The test checks name_order and find_entry against a fresh linear scan of that
 * same cache. It does not assume the image is usable: a separate fixture
 * mounting a copy of the faulted device must return CORRUPT. A third test
 * exercises the documented re-open/remount path only on a separately written,
 * successful image.
 *
 * Build (from the repository root, C17, warnings as errors):
 *   gcc -std=c17 -Wall -Wextra -Werror -DYAN_FS_TEST_HOOKS \
 *       -Ios -Itests -Ibuild/coord-b/unity \
 *       tests/test_yanfs_name_order.c os/yanfs.c build/coord-b/unity/unity.c \
 *       -o build/coord-b/test_yanfs_name_order
 */

#define _POSIX_C_SOURCE 200809L

#include "yanfs.h"
#include "unity.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define AC2_N 2047u
#define AC2_D 32u
/* Every file in this test occupies at most two data blocks. The capacity is
 * large enough that, even with maximal fragmentation, a two-block allocation
 * cannot fail: free blocks > 3*N and a fragmented free set has at most N+1
 * runs. This keeps the oracle free of extent-allocation emulation. */
#define AC2_CAPACITY_BLOCKS 8192u
#define AC2_OPS 10000u
#define AC2_SEED UINT64_C(0x0036AC2D00C0FFEE)
#define AC2_SAMPLE_EXISTING 8u
#define AC2_SAMPLE_MISSING 4u
#define AC2_PAYLOAD_MAX 8192u

_Static_assert(AC2_D * (YAN_FS_BLOCK_SIZE / YAN_FS_ENTRY_SIZE) - 1u == AC2_N,
               "AC2 must use the N that follows from D=32");
_Static_assert(AC2_CAPACITY_BLOCKS > AC2_D + 3u * AC2_N,
               "capacity must make NOSPACE impossible for two-block files");

typedef struct {
    uint8_t *data;
    uint32_t blocks;
    uint64_t write_attempts;
    uint64_t writes_succeeded;
    uint64_t failed_writes;
    uint64_t fail_at_write; /* 1-based absolute attempt; 0 disables */
} Device;

typedef struct {
    YanFs fs;
    Device device;
    uint8_t *directory;
    uint16_t *name_order;
} Fixture;

typedef struct {
    char name[AC2_N][32];
    uint32_t size[AC2_N];
    uint16_t live[AC2_N];
    uint32_t live_count;
} Oracle;

static Fixture g_fixture;
static Oracle g_oracle;
/* The last scan of the real cache, used by the name_order checks. */
static char g_actual[AC2_N][32];
static uint8_t g_payload[AC2_PAYLOAD_MAX];

static uint64_t g_rng;
static uint32_t g_unique;
static uint32_t g_missing;
static uint32_t g_step;
static uint32_t g_ops;
static uint32_t g_success;
static uint32_t g_failure;
static uint32_t g_lookups;
static char g_message[192];
static bool g_fixture_ready;

static uint32_t g_create_ok;
static uint32_t g_create_exists;
static uint32_t g_create_full;
static uint32_t g_delete_ok;
static uint32_t g_delete_missing;
static uint32_t g_rename_ok;
static uint32_t g_rename_missing;
static uint32_t g_rename_exists;
static uint32_t g_rename_self;
static uint32_t g_copy_ok;
static uint32_t g_copy_missing;
static uint32_t g_copy_exists;
static uint32_t g_copy_full;
static uint32_t g_invalid_ops;

static struct timespec g_10k_start;
static struct timespec g_10k_end;

static void fail_message(uint32_t step, uint32_t slot, const char *what)
{
    snprintf(g_message, sizeof g_message, "step %u slot %u: %s", step, slot,
             what);
}

static double seconds_between(struct timespec begin, struct timespec end)
{
    return (double)(end.tv_sec - begin.tv_sec) +
           (double)(end.tv_nsec - begin.tv_nsec) / 1000000000.0;
}

static uint32_t load_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

/* The documented name rule, reimplemented here so the oracle does not call the
 * implementation's validator. */
static bool oracle_name_valid(const char *name)
{
    size_t length = strlen(name);
    if (length < 1u || length > YAN_FS_NAME_MAX) {
        return false;
    }
    if (length == 1u && name[0] == '.') {
        return false;
    }
    if (length == 2u && name[0] == '.' && name[1] == '.') {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        unsigned char byte = (unsigned char)name[i];
        bool allowed =
            (byte >= (unsigned char)'A' && byte <= (unsigned char)'Z') ||
            (byte >= (unsigned char)'a' && byte <= (unsigned char)'z') ||
            (byte >= (unsigned char)'0' && byte <= (unsigned char)'9') ||
            byte == (unsigned char)'.' || byte == (unsigned char)'_' ||
            byte == (unsigned char)'-';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

static void oracle_clear(Oracle *oracle)
{
    memset(oracle, 0, sizeof *oracle);
}

static int oracle_find(const Oracle *oracle, const char *name)
{
    for (uint32_t i = 0; i < oracle->live_count; ++i) {
        uint32_t slot = oracle->live[i];
        if (strcmp(oracle->name[slot], name) == 0) {
            return (int)slot;
        }
    }
    return -1;
}

static uint32_t oracle_first_free(const Oracle *oracle)
{
    for (uint32_t slot = 0; slot < AC2_N; ++slot) {
        if (oracle->name[slot][0] == '\0') {
            return slot;
        }
    }
    TEST_FAIL_MESSAGE("oracle has no free slot");
}

static void oracle_insert(Oracle *oracle, uint32_t slot, const char *name,
                          uint32_t size)
{
    TEST_ASSERT_TRUE_MESSAGE(slot < AC2_N, "oracle insert slot in range");
    if (slot >= AC2_N) {
        return;
    }
    TEST_ASSERT_TRUE_MESSAGE(oracle->name[slot][0] == '\0',
                             "oracle insert into empty slot");
    TEST_ASSERT_TRUE_MESSAGE(oracle_name_valid(name),
                             "oracle insert valid name");
    TEST_ASSERT_TRUE_MESSAGE(strlen(name) < sizeof oracle->name[slot],
                             "oracle insert name fits");
    memcpy(oracle->name[slot], name, strlen(name) + 1u);
    oracle->size[slot] = size;
    TEST_ASSERT_TRUE_MESSAGE(oracle->live_count < AC2_N,
                             "oracle live list has room");
    oracle->live[oracle->live_count++] = (uint16_t)slot;
}

static void oracle_remove(Oracle *oracle, uint32_t slot)
{
    TEST_ASSERT_TRUE_MESSAGE(slot < AC2_N, "oracle remove slot in range");
    for (uint32_t i = 0; i < oracle->live_count; ++i) {
        if (oracle->live[i] == slot) {
            oracle->live[i] = oracle->live[--oracle->live_count];
            memset(oracle->name[slot], 0, sizeof oracle->name[slot]);
            oracle->size[slot] = 0u;
            return;
        }
    }
    TEST_FAIL_MESSAGE("oracle remove of a live slot");
}

static void oracle_rename(Oracle *oracle, uint32_t slot, const char *name)
{
    TEST_ASSERT_TRUE_MESSAGE(slot < AC2_N, "oracle rename slot in range");
    TEST_ASSERT_TRUE_MESSAGE(oracle_name_valid(name), "oracle rename valid");
    memcpy(oracle->name[slot], name, strlen(name) + 1u);
}

static void make_unique_name(char out[32])
{
    uint32_t value = g_unique++;
    switch (value % 5u) {
    case 0u:
        snprintf(out, 32, "n%06u", value);
        break;
    case 1u:
        snprintf(out, 32, "n%06u.x", value);
        break;
    case 2u:
        snprintf(out, 32, "n%06u-2", value);
        break;
    case 3u:
        snprintf(out, 32, "n%06u_", value);
        break;
    default:
        snprintf(out, 32, "m%06u", value);
        break;
    }
}

static void make_phase_a_name(uint32_t index, char out[32])
{
    if (index < 100u) {
        /* 50 prefix pairs: "pNNNN" sorts immediately before "pNNNN.x". */
        uint32_t pair = index / 2u;
        if ((index % 2u) == 0u) {
            snprintf(out, 32, "p%04u", pair);
        } else {
            snprintf(out, 32, "p%04u.x", pair);
        }
        return;
    }
    make_unique_name(out);
}

static void make_missing_name(char out[32])
{
    for (uint32_t attempt = 0; attempt < 1000u; ++attempt) {
        ++g_missing;
        snprintf(out, 32, "zmiss%05u", g_missing % 100000u);
        if (oracle_find(&g_oracle, out) < 0) {
            return;
        }
    }
    TEST_FAIL_MESSAGE("could not make a missing name");
}

static uint32_t data_length_for(uint32_t value)
{
    if ((value % 97u) == 0u) {
        return 8192u;
    }
    if ((value % 53u) == 0u) {
        return 4096u;
    }
    if ((value % 17u) == 0u) {
        return 37u;
    }
    if ((value % 5u) == 0u) {
        return 1u;
    }
    return 0u;
}

static uint64_t rng_next(void)
{
    g_rng ^= g_rng >> 12;
    g_rng ^= g_rng << 25;
    g_rng ^= g_rng >> 27;
    return g_rng * UINT64_C(2685821657736338717);
}

static uint32_t rng_bounded(uint32_t bound)
{
    TEST_ASSERT_TRUE_MESSAGE(bound > 0u, "rng bound");
    return (uint32_t)(((rng_next() >> 32) * (uint64_t)bound) >> 32);
}

static uint32_t pick_live_slot(void)
{
    TEST_ASSERT_TRUE_MESSAGE(g_oracle.live_count > 0u, "pick live slot");
    return g_oracle.live[rng_bounded(g_oracle.live_count)];
}

/* ------------------------------ memory device --------------------------- */

static YanFsIoResult device_capacity(void *context, uint64_t *blocks)
{
    Device *device = (Device *)context;
    *blocks = device->blocks;
    return YAN_FS_IO_OK;
}

static YanFsIoResult device_read(void *context, uint32_t lba,
                                 uint8_t out[4096])
{
    Device *device = (Device *)context;
    if (device->data == NULL || lba >= device->blocks) {
        return YAN_FS_IO_ERROR;
    }
    memcpy(out, device->data + (size_t)lba * YAN_FS_BLOCK_SIZE,
           YAN_FS_BLOCK_SIZE);
    return YAN_FS_IO_OK;
}

static YanFsIoResult device_write(void *context, uint32_t lba,
                                  const uint8_t data[4096])
{
    Device *device = (Device *)context;
    ++device->write_attempts;
    if (device->fail_at_write != 0u &&
        device->write_attempts == device->fail_at_write) {
        ++device->failed_writes;
        return YAN_FS_IO_ERROR;
    }
    if (device->data == NULL || lba >= device->blocks) {
        return YAN_FS_IO_ERROR;
    }
    memcpy(device->data + (size_t)lba * YAN_FS_BLOCK_SIZE, data,
           YAN_FS_BLOCK_SIZE);
    ++device->writes_succeeded;
    return YAN_FS_IO_OK;
}

static YanFsBlockIo device_io(Device *device)
{
    YanFsBlockIo io;
    io.context = device;
    io.capacity = device_capacity;
    io.read_block = device_read;
    io.write_block = device_write;
    return io;
}

/* ------------------------------- fixtures ------------------------------ */

static void fixture_zero(Fixture *fixture)
{
    memset(fixture, 0, sizeof *fixture);
}

static void fixture_start(Fixture *fixture, uint32_t capacity_blocks)
{
    fixture_zero(fixture);
    fixture->device.blocks = capacity_blocks;
    fixture->device.data = calloc((size_t)capacity_blocks, YAN_FS_BLOCK_SIZE);
    fixture->directory = calloc(AC2_D, YAN_FS_BLOCK_SIZE);
    fixture->name_order = calloc(AC2_N, sizeof(uint16_t));
    TEST_ASSERT_NOT_NULL(fixture->device.data);
    TEST_ASSERT_NOT_NULL(fixture->directory);
    TEST_ASSERT_NOT_NULL(fixture->name_order);
    if (fixture->device.data == NULL || fixture->directory == NULL ||
        fixture->name_order == NULL) {
        return;
    }
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK,
                          yan_fs_format_directory(fixture->directory, AC2_D,
                                                  capacity_blocks));
    memcpy(fixture->device.data, fixture->directory,
           (size_t)AC2_D * YAN_FS_BLOCK_SIZE);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK,
                          yan_fs_init_directory(&fixture->fs,
                                                device_io(&fixture->device),
                                                fixture->directory, AC2_D,
                                                fixture->name_order));
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_mount(&fixture->fs));
    TEST_ASSERT_EQUAL_UINT32(AC2_D, fixture->fs.directory_blocks);
    TEST_ASSERT_EQUAL_UINT32(AC2_N, fixture->fs.entry_capacity);
}

/* A separate fixture over a copy of an existing device. Init is done, mount is
 * left to the caller so a test can assert a rejected image. */
static YanFsResult fixture_copy(Fixture *fixture, const Device *source)
{
    fixture_zero(fixture);
    fixture->device.blocks = source->blocks;
    fixture->device.data = malloc((size_t)source->blocks * YAN_FS_BLOCK_SIZE);
    fixture->directory = calloc(AC2_D, YAN_FS_BLOCK_SIZE);
    fixture->name_order = calloc(AC2_N, sizeof(uint16_t));
    TEST_ASSERT_NOT_NULL(fixture->device.data);
    TEST_ASSERT_NOT_NULL(fixture->directory);
    TEST_ASSERT_NOT_NULL(fixture->name_order);
    if (fixture->device.data == NULL || fixture->directory == NULL ||
        fixture->name_order == NULL) {
        return YAN_FS_IO;
    }
    memcpy(fixture->device.data, source->data,
           (size_t)source->blocks * YAN_FS_BLOCK_SIZE);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK,
                          yan_fs_init_directory(&fixture->fs,
                                                device_io(&fixture->device),
                                                fixture->directory, AC2_D,
                                                fixture->name_order));
    return yan_fs_mount(&fixture->fs);
}

static void fixture_free(Fixture *fixture)
{
    free(fixture->device.data);
    free(fixture->directory);
    free(fixture->name_order);
    fixture_zero(fixture);
}

/* --------------------------- directory observation ---------------------- */

/* Reads the real in-memory directory into oracle `observed` and into
 * g_actual. This is the linear structure every name_order check uses. */
static void observe_directory(const Fixture *fixture, Oracle *observed)
{
    oracle_clear(observed);
    memset(g_actual, 0, sizeof g_actual);
    for (uint32_t slot = 0; slot < AC2_N; ++slot) {
        const uint8_t *entry =
            fixture->fs.directory + YAN_FS_HEADER_SIZE +
            (size_t)slot * YAN_FS_ENTRY_SIZE;
        if (entry[0] == 0u) {
            continue;
        }
        uint32_t length = 0u;
        while (length < YAN_FS_ENTRY_SIZE &&
               entry[length] != 0u) {
            ++length;
        }
        fail_message(g_step, slot, "stored name is terminated");
        TEST_ASSERT_TRUE_MESSAGE(length < YAN_FS_ENTRY_SIZE, g_message);
        if (length >= YAN_FS_ENTRY_SIZE) {
            length = YAN_FS_ENTRY_SIZE - 1u;
        }
        memcpy(observed->name[slot], entry, length);
        observed->name[slot][length] = '\0';
        observed->size[slot] = load_le32(entry + 32u);
        fail_message(g_step, slot, "stored name is valid");
        TEST_ASSERT_TRUE_MESSAGE(oracle_name_valid(observed->name[slot]),
                                 g_message);
        memcpy(g_actual[slot], observed->name[slot], length + 1u);
        observed->live[observed->live_count++] = (uint16_t)slot;
    }
}

static void assert_directory_matches_oracle(const Fixture *fixture,
                                            const Oracle *oracle)
{
    memset(g_actual, 0, sizeof g_actual);
    uint32_t live = 0u;
    for (uint32_t slot = 0; slot < AC2_N; ++slot) {
        const uint8_t *entry =
            fixture->fs.directory + YAN_FS_HEADER_SIZE +
            (size_t)slot * YAN_FS_ENTRY_SIZE;
        bool directory_live = entry[0] != 0u;
        bool oracle_live = oracle->name[slot][0] != '\0';
        fail_message(g_step, slot, "empty/non-empty agrees with oracle");
        TEST_ASSERT_EQUAL_INT_MESSAGE((int)oracle_live, (int)directory_live,
                                      g_message);
        if (!directory_live) {
            continue;
        }
        uint32_t length = 0u;
        while (length < YAN_FS_ENTRY_SIZE && entry[length] != 0u) {
            ++length;
        }
        fail_message(g_step, slot, "directory name is terminated");
        TEST_ASSERT_TRUE_MESSAGE(length < YAN_FS_ENTRY_SIZE, g_message);
        if (length >= YAN_FS_ENTRY_SIZE) {
            length = YAN_FS_ENTRY_SIZE - 1u;
        }
        memcpy(g_actual[slot], entry, length);
        g_actual[slot][length] = '\0';
        fail_message(g_step, slot, "directory name equals oracle");
        TEST_ASSERT_EQUAL_STRING_MESSAGE(oracle->name[slot], g_actual[slot],
                                         g_message);
        fail_message(g_step, slot, "directory size equals oracle");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(oracle->size[slot],
                                         load_le32(entry + 32u), g_message);
        ++live;
    }
    fail_message(g_step, 0u, "live slot count equals oracle");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(oracle->live_count, live, g_message);
}

/* Strictly increasing names plus complete coverage. With count == non-empty
 * slots, all entries valid and distinct, a missing non-empty slot would leave
 * fewer than `live` distinct entries, which is impossible. */
static void assert_name_order_matches_directory(const Fixture *fixture,
                                                uint32_t live)
{
    uint32_t count = yan_fs_test_name_order_count(&fixture->fs);
    fail_message(g_step, 0u, "name_order_count equals non-empty slots");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(live, count, g_message);
    TEST_ASSERT_TRUE_MESSAGE(count <= AC2_N, "name_order_count within N");
    uint32_t checked = count <= AC2_N ? count : AC2_N;
    uint8_t seen[AC2_N];
    memset(seen, 0, sizeof seen);
    const char *previous = NULL;
    for (uint32_t i = 0; i < checked; ++i) {
        uint32_t slot = yan_fs_test_name_order_at(&fixture->fs, i);
        fail_message(g_step, slot, "name_order slot is in range");
        TEST_ASSERT_TRUE_MESSAGE(slot < AC2_N, g_message);
        if (slot >= AC2_N) {
            continue;
        }
        fail_message(g_step, slot, "name_order names a non-empty slot");
        TEST_ASSERT_TRUE_MESSAGE(g_actual[slot][0] != '\0', g_message);
        fail_message(g_step, slot, "name_order has no duplicate slot");
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(0u, seen[slot], g_message);
        seen[slot] = 1u;
        if (i > 0u) {
            fail_message(g_step, slot, "name_order is strictly increasing");
            TEST_ASSERT_TRUE_MESSAGE(previous != NULL &&
                                         strcmp(previous, g_actual[slot]) < 0,
                                     g_message);
        }
        previous = g_actual[slot];
    }
}

static int compare_slots_by_actual_name(const void *left, const void *right)
{
    uint16_t a = *(const uint16_t *)left;
    uint16_t b = *(const uint16_t *)right;
    return strcmp(g_actual[a], g_actual[b]);
}

/* One exact sorted comparison, used at the end and after the fault. */
static void assert_name_order_exact_sorted(const Fixture *fixture,
                                           uint32_t live)
{
    uint16_t expected[AC2_N];
    uint32_t count = 0u;
    for (uint32_t slot = 0; slot < AC2_N; ++slot) {
        if (g_actual[slot][0] != '\0') {
            expected[count++] = (uint16_t)slot;
        }
    }
    qsort(expected, count, sizeof expected[0], compare_slots_by_actual_name);
    fail_message(g_step, 0u, "name_order exact count");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(count, live, g_message);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        count, yan_fs_test_name_order_count(&fixture->fs), g_message);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t actual = yan_fs_test_name_order_at(&fixture->fs, i);
        fail_message(g_step, actual, "name_order exact sorted entry");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(expected[i], actual, g_message);
    }
}

/* ------------------------------- lookups -------------------------------- */

static void check_stat_lookup(const Fixture *fixture, const Oracle *oracle,
                              const char *name)
{
    YanFsInfo info;
    YanFsResult result = yan_fs_stat((YanFs *)&fixture->fs, name, &info);
    ++g_lookups;
    if (!oracle_name_valid(name)) {
        fail_message(g_step, 0u, "stat of invalid name is INVALID");
        TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_INVALID, (int)result, g_message);
        return;
    }
    int slot = oracle_find(oracle, name);
    if (slot < 0) {
        fail_message(g_step, 0u, "stat of absent name is NOT_FOUND");
        TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_NOT_FOUND, (int)result, g_message);
        return;
    }
    fail_message(g_step, (uint32_t)slot, "stat returns OK");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK, (int)result, g_message);
    fail_message(g_step, (uint32_t)slot, "stat name");
    TEST_ASSERT_EQUAL_STRING_MESSAGE(oracle->name[slot], info.name, g_message);
    fail_message(g_step, (uint32_t)slot, "stat size");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(oracle->size[slot], info.size_bytes,
                                     g_message);
}

static void check_find_lookup(const Fixture *fixture, const Oracle *oracle,
                              const char *name)
{
    if (!oracle_name_valid(name)) {
        return;
    }
    uint32_t slot = UINT32_MAX;
    bool found = yan_fs_test_find_entry(&fixture->fs, name,
                                        (uint32_t)strlen(name), &slot);
    ++g_lookups;
    int expected_slot = oracle_find(oracle, name);
    fail_message(g_step, 0u, "find_entry found flag equals oracle");
    TEST_ASSERT_EQUAL_INT_MESSAGE(expected_slot >= 0, (int)found, g_message);
    if (found) {
        fail_message(g_step, slot, "find_entry slot equals oracle");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)expected_slot, slot,
                                         g_message);
    }
}

static void sample_checks(const Fixture *fixture, const Oracle *oracle,
                          uint32_t step)
{
    if (oracle->live_count > 0u) {
        for (uint32_t i = 0; i < AC2_SAMPLE_EXISTING; ++i) {
            uint32_t index =
                (step * 131u + i * 977u + 17u) % oracle->live_count;
            uint32_t slot = oracle->live[index];
            check_stat_lookup(fixture, oracle, oracle->name[slot]);
            check_find_lookup(fixture, oracle, oracle->name[slot]);
        }
    }
    for (uint32_t i = 0; i < AC2_SAMPLE_MISSING; ++i) {
        char name[32];
        snprintf(name, sizeof name, "zmiss%05u",
                 (step * 11u + i * 7u + 3u) % 100000u);
        fail_message(step, 0u, "sample absent name is absent");
        TEST_ASSERT_TRUE_MESSAGE(oracle_find(oracle, name) < 0, g_message);
        check_stat_lookup(fixture, oracle, name);
        check_find_lookup(fixture, oracle, name);
    }
}

static void post_step_checks(Fixture *fixture, YanFsResult result,
                             const char *first, const char *second,
                             uint32_t step)
{
    ++g_ops;
    if (result == YAN_FS_OK) {
        ++g_success;
    } else {
        ++g_failure;
    }
    fail_message(step, 0u, "instance remains MOUNTED");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_MOUNTED, (int)fixture->fs.state,
                                  g_message);
    check_stat_lookup(fixture, &g_oracle, first);
    check_find_lookup(fixture, &g_oracle, first);
    if (second != NULL) {
        check_stat_lookup(fixture, &g_oracle, second);
        check_find_lookup(fixture, &g_oracle, second);
    }
    sample_checks(fixture, &g_oracle, step);
    assert_directory_matches_oracle(fixture, &g_oracle);
    assert_name_order_matches_directory(fixture, g_oracle.live_count);
}

/* ----------------------------- operations ------------------------------- */

static YanFsResult do_create(Fixture *fixture, const char *name,
                             const uint8_t *bytes, uint32_t length)
{
    int existing = oracle_name_valid(name) ? oracle_find(&g_oracle, name) : -1;
    YanFsResult expected;
    uint32_t slot = UINT32_MAX;
    if (!oracle_name_valid(name)) {
        expected = YAN_FS_INVALID;
        ++g_invalid_ops;
    } else if (existing >= 0) {
        expected = YAN_FS_EXISTS;
        ++g_create_exists;
    } else if (g_oracle.live_count == AC2_N) {
        expected = YAN_FS_DIRECTORY_FULL;
        ++g_create_full;
    } else {
        expected = YAN_FS_OK;
        slot = oracle_first_free(&g_oracle);
        TEST_ASSERT_TRUE_MESSAGE(slot < AC2_N, "create has a free slot");
    }
    YanFsResult result =
        yan_fs_create(&fixture->fs, name, length > 0u ? bytes : NULL, length);
    fail_message(g_step, 0u, "create result matches oracle");
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)expected, (int)result, g_message);
    if (result == YAN_FS_OK) {
        ++g_create_ok;
        TEST_ASSERT_TRUE_MESSAGE(slot < AC2_N, "successful create slot");
        oracle_insert(&g_oracle, slot, name, length);
    }
    post_step_checks(fixture, result, name, NULL, g_step);
    return result;
}

static YanFsResult do_remove(Fixture *fixture, const char *name)
{
    int slot = oracle_name_valid(name) ? oracle_find(&g_oracle, name) : -1;
    YanFsResult expected;
    if (!oracle_name_valid(name)) {
        expected = YAN_FS_INVALID;
        ++g_invalid_ops;
    } else if (slot < 0) {
        expected = YAN_FS_NOT_FOUND;
        ++g_delete_missing;
    } else {
        expected = YAN_FS_OK;
    }
    YanFsResult result = yan_fs_remove(&fixture->fs, name);
    fail_message(g_step, 0u, "remove result matches oracle");
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)expected, (int)result, g_message);
    if (result == YAN_FS_OK) {
        ++g_delete_ok;
        TEST_ASSERT_TRUE_MESSAGE(slot >= 0, "successful remove slot");
        oracle_remove(&g_oracle, (uint32_t)slot);
    }
    post_step_checks(fixture, result, name, NULL, g_step);
    return result;
}

static YanFsResult do_rename(Fixture *fixture, const char *old_name,
                             const char *new_name)
{
    bool old_valid = oracle_name_valid(old_name);
    bool new_valid = oracle_name_valid(new_name);
    int old_slot = old_valid ? oracle_find(&g_oracle, old_name) : -1;
    int new_slot = new_valid ? oracle_find(&g_oracle, new_name) : -1;
    YanFsResult expected;
    if (!old_valid || !new_valid) {
        expected = YAN_FS_INVALID;
        ++g_invalid_ops;
    } else if (old_slot < 0) {
        expected = YAN_FS_NOT_FOUND;
        ++g_rename_missing;
    } else if (new_slot >= 0 && new_slot != old_slot) {
        expected = YAN_FS_EXISTS;
        ++g_rename_exists;
    } else if (new_slot == old_slot) {
        expected = YAN_FS_OK;
        ++g_rename_self;
    } else {
        expected = YAN_FS_OK;
    }
    YanFsResult result =
        yan_fs_rename(&fixture->fs, old_name, new_name);
    fail_message(g_step, 0u, "rename result matches oracle");
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)expected, (int)result, g_message);
    if (result == YAN_FS_OK) {
        ++g_rename_ok;
        if (old_slot >= 0 && strcmp(old_name, new_name) != 0) {
            oracle_rename(&g_oracle, (uint32_t)old_slot, new_name);
        }
    }
    post_step_checks(fixture, result, old_name, new_name, g_step);
    return result;
}

static YanFsResult do_copy(Fixture *fixture, const char *source_name,
                           const char *destination_name)
{
    bool source_valid = oracle_name_valid(source_name);
    bool destination_valid = oracle_name_valid(destination_name);
    int source_slot = source_valid ? oracle_find(&g_oracle, source_name) : -1;
    int destination_slot =
        destination_valid ? oracle_find(&g_oracle, destination_name) : -1;
    YanFsResult expected;
    uint32_t insert_slot = UINT32_MAX;
    uint32_t insert_size = 0u;
    if (!source_valid || !destination_valid) {
        expected = YAN_FS_INVALID;
        ++g_invalid_ops;
    } else if (source_slot < 0) {
        expected = YAN_FS_NOT_FOUND;
        ++g_copy_missing;
    } else if (destination_slot >= 0) {
        expected = YAN_FS_EXISTS;
        ++g_copy_exists;
    } else if (g_oracle.live_count == AC2_N) {
        expected = YAN_FS_DIRECTORY_FULL;
        ++g_copy_full;
    } else {
        expected = YAN_FS_OK;
        insert_slot = oracle_first_free(&g_oracle);
        insert_size = g_oracle.size[source_slot];
        TEST_ASSERT_TRUE_MESSAGE(insert_slot < AC2_N, "copy has a free slot");
    }
    YanFsResult result =
        yan_fs_copy(&fixture->fs, source_name, destination_name);
    fail_message(g_step, 0u, "copy result matches oracle");
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)expected, (int)result, g_message);
    if (result == YAN_FS_OK) {
        ++g_copy_ok;
        TEST_ASSERT_TRUE_MESSAGE(insert_slot < AC2_N, "copy insert slot");
        oracle_insert(&g_oracle, insert_slot, destination_name, insert_size);
    }
    post_step_checks(fixture, result, source_name, destination_name, g_step);
    return result;
}

/* --------------------------- randomized workload ------------------------ */

static const char *const g_invalid_names[] = {
    "", ".", "..", "bad/name", "bad name", "a\\b", "a:b", "a*b",
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "\xC3\xA9"
};
#define INVALID_NAME_COUNT \
    (sizeof g_invalid_names / sizeof g_invalid_names[0])

static void random_create(Fixture *fixture)
{
    char name[64];
    uint32_t length = 0u;
    uint32_t choice = rng_bounded(100u);
    if (choice < 70u) {
        make_unique_name(name);
        length = data_length_for(g_unique);
    } else if (choice < 88u && g_oracle.live_count > 0u) {
        uint32_t slot = pick_live_slot();
        memcpy(name, g_oracle.name[slot], strlen(g_oracle.name[slot]) + 1u);
        length = data_length_for(g_unique);
    } else {
        const char *invalid =
            g_invalid_names[rng_bounded((uint32_t)INVALID_NAME_COUNT)];
        memcpy(name, invalid, strlen(invalid) + 1u);
    }
    (void)do_create(fixture, name, g_payload, length);
}

static void random_remove(Fixture *fixture)
{
    char name[64];
    if (rng_bounded(100u) < 75u && g_oracle.live_count > 0u) {
        uint32_t slot = pick_live_slot();
        memcpy(name, g_oracle.name[slot], strlen(g_oracle.name[slot]) + 1u);
    } else {
        make_missing_name(name);
    }
    (void)do_remove(fixture, name);
}

static void random_rename(Fixture *fixture)
{
    char old_name[64];
    char new_name[64];
    if (rng_bounded(100u) < 75u && g_oracle.live_count > 0u) {
        uint32_t slot = pick_live_slot();
        memcpy(old_name, g_oracle.name[slot],
               strlen(g_oracle.name[slot]) + 1u);
    } else {
        make_missing_name(old_name);
    }
    uint32_t choice = rng_bounded(100u);
    if (choice < 50u) {
        make_unique_name(new_name);
    } else if (choice < 65u && g_oracle.live_count > 0u) {
        uint32_t slot = pick_live_slot();
        memcpy(new_name, g_oracle.name[slot],
               strlen(g_oracle.name[slot]) + 1u);
    } else if (choice < 75u) {
        memcpy(new_name, old_name, strlen(old_name) + 1u);
    } else {
        const char *invalid =
            g_invalid_names[rng_bounded((uint32_t)INVALID_NAME_COUNT)];
        memcpy(new_name, invalid, strlen(invalid) + 1u);
    }
    (void)do_rename(fixture, old_name, new_name);
}

static void random_copy(Fixture *fixture)
{
    char source[64];
    char destination[64];
    if (rng_bounded(100u) < 75u && g_oracle.live_count > 0u) {
        uint32_t slot = pick_live_slot();
        memcpy(source, g_oracle.name[slot],
               strlen(g_oracle.name[slot]) + 1u);
    } else {
        make_missing_name(source);
    }
    uint32_t choice = rng_bounded(100u);
    if (choice < 50u) {
        make_unique_name(destination);
    } else if (choice < 65u && g_oracle.live_count > 0u) {
        uint32_t slot = pick_live_slot();
        memcpy(destination, g_oracle.name[slot],
               strlen(g_oracle.name[slot]) + 1u);
    } else {
        const char *invalid =
            g_invalid_names[rng_bounded((uint32_t)INVALID_NAME_COUNT)];
        memcpy(destination, invalid, strlen(invalid) + 1u);
    }
    (void)do_copy(fixture, source, destination);
}

static void run_random_operation(Fixture *fixture)
{
    uint32_t choice = rng_bounded(100u);
    if (choice < 30u) {
        random_remove(fixture);
    } else if (choice < 70u) {
        random_create(fixture);
    } else if (choice < 90u) {
        random_rename(fixture);
    } else {
        random_copy(fixture);
    }
}

/* ------------------------------- test cases ----------------------------- */

static void test_ac2_random_10k(void)
{
    oracle_clear(&g_oracle);
    g_rng = AC2_SEED;
    g_unique = 0u;
    g_missing = 0u;
    g_ops = 0u;
    g_success = 0u;
    g_failure = 0u;
    g_lookups = 0u;
    g_create_ok = g_create_exists = g_create_full = 0u;
    g_delete_ok = g_delete_missing = 0u;
    g_rename_ok = g_rename_missing = g_rename_exists = g_rename_self = 0u;
    g_copy_ok = g_copy_missing = g_copy_exists = g_copy_full = 0u;
    g_invalid_ops = 0u;

    fixture_start(&g_fixture, AC2_CAPACITY_BLOCKS);

    timespec_get(&g_10k_start, TIME_UTC);

    /* Fill all 2047 slots with unique names. The first 100 names are prefix
     * pairs, so neither the binary search nor the sort can assume uniform
     * names. */
    for (uint32_t step = 0; step < AC2_N; ++step) {
        g_step = step;
        char name[32];
        make_phase_a_name(step, name);
        TEST_ASSERT_TRUE_MESSAGE(oracle_find(&g_oracle, name) < 0,
                                 "phase A name is unique");
        uint32_t length = data_length_for(step);
        YanFsResult result =
            do_create(&g_fixture, name, g_payload, length);
        fail_message(step, 0u, "phase A create succeeds");
        TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK, (int)result, g_message);
    }
    TEST_ASSERT_EQUAL_UINT32(AC2_N, g_oracle.live_count);

    /* Deterministic edge cases while the directory is exactly full. */
    g_step = AC2_N;
    {
        char name[32];
        make_unique_name(name);
        YanFsResult result = do_create(&g_fixture, name, NULL, 0u);
        fail_message(g_step, 0u, "create on a full directory");
        TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_DIRECTORY_FULL, (int)result,
                                      g_message);
    }

    g_step = AC2_N + 1u;
    {
        char name[32];
        memcpy(name, g_oracle.name[g_oracle.live[0]],
               strlen(g_oracle.name[g_oracle.live[0]]) + 1u);
        YanFsResult result = do_create(&g_fixture, name, g_payload, 1u);
        fail_message(g_step, 0u, "duplicate create");
        TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_EXISTS, (int)result, g_message);
    }

    g_step = AC2_N + 2u;
    {
        YanFsResult result = do_create(&g_fixture, "bad/name", g_payload, 1u);
        fail_message(g_step, 0u, "invalid create");
        TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_INVALID, (int)result, g_message);
    }

    /* Fixed probes for every failing family required by the task. */
    g_step = AC2_N + 3u;
    {
        char missing[32];
        make_missing_name(missing);
        YanFsResult result = do_remove(&g_fixture, missing);
        fail_message(g_step, 0u, "missing delete");
        TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_NOT_FOUND, (int)result, g_message);
    }
    g_step = AC2_N + 4u;
    {
        char missing[32];
        char destination[32];
        make_missing_name(missing);
        make_unique_name(destination);
        YanFsResult result = do_rename(&g_fixture, missing, destination);
        fail_message(g_step, 0u, "missing rename source");
        TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_NOT_FOUND, (int)result, g_message);
    }
    g_step = AC2_N + 5u;
    {
        char missing[32];
        char destination[32];
        make_missing_name(missing);
        make_unique_name(destination);
        YanFsResult result = do_copy(&g_fixture, missing, destination);
        fail_message(g_step, 0u, "missing copy source");
        TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_NOT_FOUND, (int)result, g_message);
    }
    g_step = AC2_N + 6u;
    {
        uint32_t source_slot = g_oracle.live[0];
        uint32_t destination_slot = g_oracle.live[1];
        YanFsResult result =
            do_rename(&g_fixture, g_oracle.name[source_slot],
                      g_oracle.name[destination_slot]);
        fail_message(g_step, 0u, "rename onto an existing name");
        TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_EXISTS, (int)result, g_message);
    }
    g_step = AC2_N + 7u;
    {
        uint32_t source_slot = g_oracle.live[0];
        uint32_t destination_slot = g_oracle.live[1];
        YanFsResult result =
            do_copy(&g_fixture, g_oracle.name[source_slot],
                    g_oracle.name[destination_slot]);
        fail_message(g_step, 0u, "copy onto an existing name");
        TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_EXISTS, (int)result, g_message);
    }
    g_step = AC2_N + 8u;
    {
        YanFsResult result = do_remove(&g_fixture, "bad name");
        fail_message(g_step, 0u, "invalid delete");
        TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_INVALID, (int)result, g_message);
    }
    g_step = AC2_N + 9u;
    {
        char name[32];
        make_unique_name(name);
        YanFsResult result = do_create(&g_fixture, name, NULL, 0u);
        fail_message(g_step, 0u, "full create probe");
        TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_DIRECTORY_FULL, (int)result,
                                      g_message);
    }

    /* Remaining 7942 operations: 30% delete, 40% create, 20% rename, 10% copy.
     * Starting from a full directory, creates fail until deletes free slots, so
     * the run repeatedly exercises both the full and near-full paths. */
    for (uint32_t step = AC2_N + 10u; step < AC2_OPS; ++step) {
        g_step = step;
        run_random_operation(&g_fixture);
    }

    fail_message(g_step, 0u, "operation count");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(AC2_OPS, g_ops, g_message);
    /* Final exhaustive check over the live set. Every name is looked up by the
     * public API and by the production binary search; the table is compared
     * with an independently sorted slot list. */
    assert_directory_matches_oracle(&g_fixture, &g_oracle);
    assert_name_order_matches_directory(&g_fixture, g_oracle.live_count);
    assert_name_order_exact_sorted(&g_fixture, g_oracle.live_count);

    uint32_t exhaustive = 0u;
    for (uint32_t slot = 0; slot < AC2_N; ++slot) {
        if (g_oracle.name[slot][0] == '\0') {
            continue;
        }
        check_stat_lookup(&g_fixture, &g_oracle, g_oracle.name[slot]);
        check_find_lookup(&g_fixture, &g_oracle, g_oracle.name[slot]);
        ++exhaustive;
    }
    fail_message(g_step, 0u, "final exhaustive live count");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(g_oracle.live_count, exhaustive, g_message);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        g_oracle.live_count,
        yan_fs_test_name_order_count(&g_fixture.fs), g_message);
    for (uint32_t i = 0; i < AC2_SAMPLE_MISSING; ++i) {
        char name[32];
        snprintf(name, sizeof name, "zzzz%05u", i);
        fail_message(0u, 0u, "final absent name");
        TEST_ASSERT_TRUE_MESSAGE(oracle_find(&g_oracle, name) < 0, g_message);
        check_stat_lookup(&g_fixture, &g_oracle, name);
        check_find_lookup(&g_fixture, &g_oracle, name);
    }

    timespec_get(&g_10k_end, TIME_UTC);

    TEST_ASSERT_TRUE_MESSAGE(g_success > 0u, "some operations succeeded");
    TEST_ASSERT_TRUE_MESSAGE(g_failure > 0u, "some operations failed");
    TEST_ASSERT_TRUE_MESSAGE(g_create_exists > 0u, "duplicate create covered");
    TEST_ASSERT_TRUE_MESSAGE(g_create_full > 0u, "full create covered");
    TEST_ASSERT_TRUE_MESSAGE(g_delete_missing > 0u, "missing delete covered");
    TEST_ASSERT_TRUE_MESSAGE(g_rename_missing > 0u, "missing rename covered");
    TEST_ASSERT_TRUE_MESSAGE(g_rename_exists > 0u, "existing rename covered");
    TEST_ASSERT_TRUE_MESSAGE(g_copy_missing > 0u, "missing copy covered");
    TEST_ASSERT_TRUE_MESSAGE(g_copy_exists > 0u, "existing copy covered");
    TEST_ASSERT_TRUE_MESSAGE(g_invalid_ops > 0u, "invalid names covered");

    printf("AC2: ops=%u success=%u failure=%u (create_ok=%u exists=%u "
           "full=%u; delete_ok=%u missing=%u; rename_ok=%u missing=%u "
           "exists=%u self=%u; copy_ok=%u missing=%u exists=%u full=%u; "
           "invalid=%u)\n",
           g_ops, g_success, g_failure, g_create_ok, g_create_exists,
           g_create_full, g_delete_ok, g_delete_missing, g_rename_ok,
           g_rename_missing, g_rename_exists, g_rename_self, g_copy_ok,
           g_copy_missing, g_copy_exists, g_copy_full, g_invalid_ops);
    printf("AC2: 10000-op phase %.3f s; final live=%u name_order_count=%u; "
           "lookups=%u; final exhaustive=%u\n",
           seconds_between(g_10k_start, g_10k_end), g_oracle.live_count,
           yan_fs_test_name_order_count(&g_fixture.fs), g_lookups, exhaustive);
    g_fixture_ready = true;
}

static void fill_to_capacity(Fixture *fixture)
{
    while (g_oracle.live_count < AC2_N) {
        char name[32];
        make_unique_name(name);
        TEST_ASSERT_TRUE_MESSAGE(oracle_find(&g_oracle, name) < 0,
                                 "fill name is unique");
        YanFsResult result = do_create(fixture, name, NULL, 0u);
        fail_message(g_step, 0u, "fill create succeeds");
        TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK, (int)result, g_message);
    }
    TEST_ASSERT_EQUAL_UINT32(AC2_N, g_oracle.live_count);
}

static void test_ac2_fault_injection(void)
{
    TEST_ASSERT_TRUE_MESSAGE(g_fixture_ready,
                             "the randomized phase must have succeeded");
    Fixture *fixture = &g_fixture;

    /* Fill to exactly N and do a full invariant pass before the fault. */
    fill_to_capacity(fixture);
    assert_directory_matches_oracle(fixture, &g_oracle);
    assert_name_order_matches_directory(fixture, g_oracle.live_count);
    assert_name_order_exact_sorted(fixture, g_oracle.live_count);

    /* Slot 100 is in directory block 1: its commit has an entry-block write
     * followed by the block-0 CRC write. Delete it, then create an empty file
     * so find_free_slot returns exactly slot 100 again. */
    TEST_ASSERT_TRUE_MESSAGE(g_oracle.name[100][0] != '\0',
                             "slot 100 is live after filling");
    char deleted_name[32];
    memcpy(deleted_name, g_oracle.name[100], strlen(g_oracle.name[100]) + 1u);
    g_step = AC2_OPS + 1u;
    YanFsResult removed = do_remove(fixture, deleted_name);
    fail_message(g_step, 100u, "fault setup delete");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK, (int)removed, g_message);
    TEST_ASSERT_TRUE_MESSAGE(g_oracle.name[100][0] == '\0',
                             "slot 100 is free after delete");

    char fault_name[32];
    snprintf(fault_name, sizeof fault_name, "fault-target");
    TEST_ASSERT_TRUE_MESSAGE(oracle_find(&g_oracle, fault_name) < 0,
                             "fault name is absent");
    uint32_t expected_slot = oracle_first_free(&g_oracle);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(100u, expected_slot,
                                     "next free slot is block 1 slot 100");

    uint64_t attempts_before = fixture->device.write_attempts;
    /* First write after arming (entry block 1) succeeds; the second (CRC in
     * block 0) fails. This is a mutation that is visible in the cache but not
     * durably published. */
    fixture->device.fail_at_write = attempts_before + 2u;

    g_step = AC2_OPS + 2u;
    YanFsResult faulted =
        yan_fs_create(&fixture->fs, fault_name, NULL, 0u);
    fail_message(g_step, 100u, "faulted create returns IO");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_IO, (int)faulted, g_message);
    TEST_ASSERT_EQUAL_UINT64(attempts_before + 2u,
                             fixture->device.write_attempts);
    TEST_ASSERT_EQUAL_UINT64(attempts_before + 1u,
                             fixture->device.writes_succeeded);
    TEST_ASSERT_EQUAL_UINT64(1u, fixture->device.failed_writes);
    TEST_ASSERT_EQUAL_INT(YAN_FS_STATE_FAULTED, (int)fixture->fs.state);
    TEST_ASSERT_EQUAL_UINT32(0u, fixture->fs.busy ? 1u : 0u);

    /* The published cache now contains the entry from the successful first
     * write, even though block 0's CRC is stale. Build a fresh linear oracle
     * from the real cache and compare the table and the production lookup
     * against it. No API lookup can run: all file operations return FAULTED. */
    Oracle cache;
    observe_directory(fixture, &cache);
    TEST_ASSERT_EQUAL_UINT32(AC2_N, cache.live_count);
    fail_message(g_step, 100u, "fault mutation is in the cache");
    TEST_ASSERT_EQUAL_STRING_MESSAGE(fault_name, cache.name[100], g_message);
    assert_name_order_matches_directory(fixture, cache.live_count);
    assert_name_order_exact_sorted(fixture, cache.live_count);

    uint32_t fault_lookups = 0u;
    for (uint32_t slot = 0; slot < AC2_N; ++slot) {
        if (cache.name[slot][0] == '\0') {
            continue;
        }
        check_find_lookup(fixture, &cache, cache.name[slot]);
        ++fault_lookups;
    }
    fail_message(g_step, 0u, "fault exhaustive live count");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(AC2_N, fault_lookups, g_message);
    for (uint32_t i = 0; i < AC2_SAMPLE_MISSING; ++i) {
        char absent[32];
        snprintf(absent, sizeof absent, "zmiss-fault%u", i);
        TEST_ASSERT_TRUE_MESSAGE(oracle_find(&cache, absent) < 0,
                                 "fault absent name");
        check_find_lookup(fixture, &cache, absent);
    }

    YanFsInfo info;
    fail_message(g_step, 0u, "stat is rejected while faulted");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_FS_FAULTED, (int)yan_fs_stat(&fixture->fs, fault_name, &info),
        g_message);
    fail_message(g_step, 0u, "create is rejected while faulted");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_FS_FAULTED,
        (int)yan_fs_create(&fixture->fs, "fault-other", NULL, 0u), g_message);
    fail_message(g_step, 0u, "mount is rejected while faulted");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_FAULTED,
                                  (int)yan_fs_mount(&fixture->fs), g_message);

    YanFsSource source;
    fail_message(g_step, 0u, "source observation succeeds");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK,
                                  (int)yan_fs_source(&fixture->fs, &source),
                                  g_message);
    TEST_ASSERT_EQUAL_INT(YAN_FS_STATE_FAULTED, (int)source.state);

    /* The cache's stored CRC no longer covers its bytes: the second metadata
     * write really did not land. */
    uint32_t stored_crc = load_le32(fixture->fs.directory + 60u);
    uint32_t computed_crc =
        yan_fs_directory_crc(fixture->fs.directory, AC2_D);
    fail_message(g_step, 0u, "faulted cache has a stale CRC");
    TEST_ASSERT_TRUE_MESSAGE(stored_crc != computed_crc, g_message);

    YanFsState fault_state = fixture->fs.state;
    uint32_t fault_order_count =
        yan_fs_test_name_order_count(&fixture->fs);

    /* unmount may clear the local view; the medium is still suspect. */
    fail_message(g_step, 0u, "unmount from FAULTED");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK,
                                  (int)yan_fs_unmount(&fixture->fs), g_message);
    TEST_ASSERT_EQUAL_INT(YAN_FS_UNMOUNTED, (int)fixture->fs.state);
    TEST_ASSERT_EQUAL_UINT32(0u,
                             yan_fs_test_name_order_count(&fixture->fs));

    /* Do not assume the failed create produced a mountable image. A separate
     * fixture mounting a copy must reject it, with zero writes. */
    Fixture rejected;
    YanFsResult mount_result = fixture_copy(&rejected, &fixture->device);
    fail_message(g_step, 0u, "faulted image mounts as CORRUPT");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_CORRUPT, (int)mount_result, g_message);
    TEST_ASSERT_EQUAL_UINT64(0u, rejected.device.write_attempts);
    TEST_ASSERT_EQUAL_UINT32(0u,
                             yan_fs_test_name_order_count(&rejected.fs));
    printf("AC2-FAULT: slot=%u name=%s result=%d state=%d; write attempts %llu "
           "(success %llu, failed %llu); cache_live=%u name_order=%u "
           "slot100=%s; stored_crc=%08x computed_crc=%08x; remount=%d\n",
           expected_slot, fault_name, (int)faulted, (int)fault_state,
           (unsigned long long)fixture->device.write_attempts,
           (unsigned long long)fixture->device.writes_succeeded,
           (unsigned long long)fixture->device.failed_writes, cache.live_count,
           fault_order_count, cache.name[100],
           stored_crc, computed_crc, (int)mount_result);
    fixture_free(&rejected);

    fixture_free(fixture);
    g_fixture_ready = false;
}

static void test_ac2_clean_remount(void)
{
    /* Positive control for the documented re-open/remount path: this image was
     * written successfully, and the remount uses a separate fixture over a
     * copy of the device. It is not a claim about the faulted image above. */
    Fixture clean;
    fixture_start(&clean, 128u);
    uint8_t content[100];
    memcpy(content, g_payload, sizeof content);
    fail_message(0u, 0u, "clean create");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_FS_OK,
        (int)yan_fs_create(&clean.fs, "persist.txt", content,
                           (uint32_t)sizeof content),
        g_message);
    fail_message(0u, 0u, "clean unmount");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK,
                                  (int)yan_fs_unmount(&clean.fs), g_message);

    Fixture reopened;
    fail_message(0u, 0u, "clean remount succeeds");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK,
                                  (int)fixture_copy(&reopened, &clean.device),
                                  g_message);
    YanFsInfo info;
    fail_message(0u, 0u, "clean remount stat");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_FS_OK,
        (int)yan_fs_stat(&reopened.fs, "persist.txt", &info), g_message);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)sizeof content, info.size_bytes);
    uint32_t slot = UINT32_MAX;
    fail_message(0u, 0u, "clean remount find_entry");
    TEST_ASSERT_TRUE_MESSAGE(
        yan_fs_test_find_entry(&reopened.fs, "persist.txt",
                               (uint32_t)strlen("persist.txt"), &slot),
        g_message);
    TEST_ASSERT_EQUAL_UINT32(0u, slot);
    uint8_t readback[100];
    uint32_t read_bytes = 0u;
    fail_message(0u, 0u, "clean remount read");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_FS_OK,
        (int)yan_fs_read(&reopened.fs, "persist.txt", 0u, readback,
                         (uint32_t)sizeof readback, &read_bytes),
        g_message);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)sizeof content, read_bytes);
    TEST_ASSERT_EQUAL_MEMORY(content, readback, sizeof content);

    fixture_free(&reopened);
    fixture_free(&clean);
}

void setUp(void)
{
    for (uint32_t i = 0; i < AC2_PAYLOAD_MAX; ++i) {
        g_payload[i] = (uint8_t)((i * 37u + 11u) & 0xffu);
    }
}

void tearDown(void)
{
}

int main(void)
{
    struct timespec suite_start;
    timespec_get(&suite_start, TIME_UTC);
    UNITY_BEGIN();
    RUN_TEST(test_ac2_random_10k);
    RUN_TEST(test_ac2_fault_injection);
    RUN_TEST(test_ac2_clean_remount);
    int result = UNITY_END();
    struct timespec suite_end;
    timespec_get(&suite_end, TIME_UTC);
    printf("AC2: suite wall time %.3f s\n",
           seconds_between(suite_start, suite_end));
    return result;
}
