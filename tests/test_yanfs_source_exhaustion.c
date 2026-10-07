#include "yanfs.h"
#include "unity.h"

#include <string.h>

/* 0026 source-identity exhaustion, built as its own target with the same
 * os/yanfs.c compiled a second time and a near-UINT64_MAX allocator seed. It
 * must not link the yan_fs library: doing so would define yanfs.c twice.
 *
 * docs/specs/0026-term-search-index.md, "来源观察与token契约": the process
 * global allocator starts from YAN_FS_SOURCE_TOKEN_SEED, hands out SEED + 1 as
 * its first identity and stops being cacheable after UINT64_MAX. The last
 * identity before exhaustion is UINT64_MAX itself; the next allocation returns
 * token 0 with cacheable=false and no later allocation ever wraps back to a
 * small non-zero value. Ordinary failures and read-only calls never allocate.
 *
 * The seed is the fixture's single degree of freedom: this suite pins it to
 * UINT64_MAX - 64 so a handful of real init/mount/published-directory calls can
 * walk across the ceiling. The test never injects a context field or writes a
 * fake token: every identity under test comes from a real filesystem call on a
 * real in-memory device. The driver loop tracks the last live identity instead
 * of counting allocations, so it does not encode an implementation counter.
 *
 * This file is compiled with Unity's strict warning bar (-Wall -Wextra
 * -Wpedantic -Werror on GCC/Clang, /W4 /WX on MSVC); the macro below must be a
 * plain integer literal because MSVC's /D does not parse a parenthesised
 * expression the same way GCC's -D does. */

#ifndef YAN_FS_SOURCE_TOKEN_SEED
#error "test_yanfs_source_exhaustion.c needs -DYAN_FS_SOURCE_TOKEN_SEED=..."
#endif

#define SOURCE_SEED ((uint64_t)YAN_FS_SOURCE_TOKEN_SEED)

#define DEVICE_BLOCKS 16u
/* Enough real allocations to cross the pinned seed (UINT64_MAX - 64) even
 * after the earlier cases consume a few; the loop stops the moment an
 * allocation reports token 0. */
#define DRIVE_LIMIT 128u

typedef struct {
    uint8_t blocks[DEVICE_BLOCKS][YAN_FS_BLOCK_SIZE];
    uint64_t capacity_blocks;
    uint32_t reads;
    uint32_t writes;
    uint32_t read_fail_at;
} Device;

static Device device;

static YanFsIoResult device_capacity(void *context, uint64_t *blocks)
{
    Device *self = (Device *)context;
    *blocks = self->capacity_blocks;
    return YAN_FS_IO_OK;
}

static YanFsIoResult device_read_block(void *context, uint32_t lba,
                                       uint8_t out[4096])
{
    Device *self = (Device *)context;
    ++self->reads;
    if (self->read_fail_at != 0u && self->reads == self->read_fail_at) {
        return YAN_FS_IO_ERROR;
    }
    if (lba >= DEVICE_BLOCKS) {
        return YAN_FS_IO_ERROR;
    }
    memcpy(out, self->blocks[lba], YAN_FS_BLOCK_SIZE);
    return YAN_FS_IO_OK;
}

static YanFsIoResult device_write_block(void *context, uint32_t lba,
                                        const uint8_t data[4096])
{
    Device *self = (Device *)context;
    ++self->writes;
    if (lba >= DEVICE_BLOCKS) {
        return YAN_FS_IO_ERROR;
    }
    memcpy(self->blocks[lba], data, YAN_FS_BLOCK_SIZE);
    return YAN_FS_IO_OK;
}

static YanFsBlockIo backend(void)
{
    YanFsBlockIo io;
    io.context = &device;
    io.capacity = device_capacity;
    io.read_block = device_read_block;
    io.write_block = device_write_block;
    return io;
}

static YanFsSource observe(const YanFs *context)
{
    YanFsSource source;
    memset(&source, 0, sizeof source);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK, yan_fs_source(context, &source),
                                  "the observation must succeed");
    return source;
}

static uint64_t live_token(const YanFs *context, const char *message)
{
    YanFsSource source = observe(context);
    TEST_ASSERT_TRUE_MESSAGE(source.token != 0u, message);
    TEST_ASSERT_TRUE_MESSAGE(source.cacheable, message);
    return source.token;
}

/* Records the token as the last live identity, or reports whether the allocator
 * has crossed the ceiling. An exhausted identity must read as token 0 with
 * cacheable=false. */
static bool record_or_exhausted(const YanFs *context, uint64_t *last_live)
{
    YanFsSource source = observe(context);
    if (source.token == 0u) {
        TEST_ASSERT_FALSE_MESSAGE(source.cacheable,
            "after exhaustion every identity must be uncacheable");
        return true;
    }
    TEST_ASSERT_TRUE_MESSAGE(source.cacheable,
        "every identity before exhaustion must be cacheable");
    *last_live = source.token;
    return false;
}

void setUp(void)
{
    memset(&device, 0, sizeof device);
    device.capacity_blocks = DEVICE_BLOCKS;
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(UINT64_MAX - 64u, SOURCE_SEED,
        "this suite is pinned to YAN_FS_SOURCE_TOKEN_SEED = UINT64_MAX - 64");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK,
        yan_fs_format_metadata(device.blocks[0], DEVICE_BLOCKS),
        "the fixture device must hold a valid empty directory");
}

void tearDown(void)
{
}

static void source_allocator_hands_out_distinct_live_identities(void)
{
    YanFs a;
    YanFs b;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);

    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_init(&a, backend()));
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK, yan_fs_init(&b, backend()),
        "a second object must initialize independently");
    uint64_t a_token = live_token(&a,
        "the first object must receive a live source identity");
    uint64_t b_token = live_token(&b,
        "the second object must receive a live source identity");
    TEST_ASSERT_TRUE_MESSAGE(a_token != b_token,
        "two objects must not share one source identity");
}

static void source_allocator_rejections_and_read_only_do_not_allocate(void)
{
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK,
        yan_fs_format_metadata(device.blocks[0], DEVICE_BLOCKS));
    YanFs context;
    memset(&context, 0, sizeof context);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_init(&context, backend()));
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_mount(&context));
    uint8_t byte = 0x11u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_create(&context, "x", &byte, 1u));
    uint64_t before = live_token(&context,
        "a published create must leave a live source identity");

    /* Rejections and read-only calls must not consume an allocator step. */
    TEST_ASSERT_EQUAL_INT(YAN_FS_EXISTS, yan_fs_create(&context, "x", &byte, 1u));
    TEST_ASSERT_EQUAL_INT(YAN_FS_NOT_FOUND, yan_fs_remove(&context, "missing"));
    YanFsInfo info = {0};
    uint32_t cursor = 0u;
    uint32_t got = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_stat(&context, "x", &info));
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_list(&context, &cursor, &info));
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK,
        yan_fs_read(&context, "x", 0u, &byte, 1u, &got));
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(before,
        live_token(&context, "read-only calls must leave the identity live"),
        "rejections and read-only calls must not consume an allocator step");
}

static void source_allocator_exhaustion_crosses_max_without_wrap(void)
{
    uint64_t last_live = 0u;
    bool exhausted = false;
    YanFs retained;
    memset(&retained, 0, sizeof retained);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_init(&retained, backend()));
    uint64_t retained_token = live_token(&retained,
        "an older instance must initially be cacheable");
    uint8_t retained_snapshot[sizeof retained];
    memcpy(retained_snapshot, &retained, sizeof retained);

    /* Real consecutive init -> mount -> published create, each an identity
     * allocation under the contract. The device is reformatted between rounds
     * with the pure helper, which allocates nothing. */
    for (uint32_t round = 0u; round < DRIVE_LIMIT && !exhausted; ++round) {
        TEST_ASSERT_EQUAL_INT(YAN_FS_OK,
            yan_fs_format_metadata(device.blocks[0], DEVICE_BLOCKS));
        YanFs scratch;
        memset(&scratch, 0, sizeof scratch);
        TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_init(&scratch, backend()));
        if (record_or_exhausted(&scratch, &last_live)) {
            exhausted = true;
            break;
        }
        TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_mount(&scratch));
        if (record_or_exhausted(&scratch, &last_live)) {
            exhausted = true;
            break;
        }
        uint8_t byte = 0x22u;
        TEST_ASSERT_EQUAL_INT(YAN_FS_OK,
            yan_fs_create(&scratch, "x", &byte, 1u));
        if (record_or_exhausted(&scratch, &last_live)) {
            exhausted = true;
            break;
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(exhausted,
        "the near-UINT64_MAX seed must be crossed by the driver");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(UINT64_MAX, last_live,
        "the last cacheable identity before exhaustion must be UINT64_MAX");
    uint32_t reads_before = device.reads;
    uint32_t writes_before = device.writes;
    YanFsSource retained_source = observe(&retained);
    TEST_ASSERT_FALSE_MESSAGE(retained_source.cacheable,
        "global exhaustion must disable caching for older instances too");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(retained_token, retained_source.token,
        "observing global exhaustion must not allocate a new identity");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(retained_snapshot, &retained, sizeof retained,
        "the getter must not mutate an older instance");
    TEST_ASSERT_EQUAL_UINT32(reads_before, device.reads);
    TEST_ASSERT_EQUAL_UINT32(writes_before, device.writes);

    /* Every later allocation stays at token 0 / cacheable=false: the allocator
     * must not wrap back to a freshly non-zero identity. */
    for (uint32_t index = 0u; index < 4u; ++index) {
        YanFs scratch;
        memset(&scratch, 0, sizeof scratch);
        TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_init(&scratch, backend()));
        YanFsSource source = observe(&scratch);
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(0u, source.token,
            "post-exhaustion allocations must stay at token 0");
        TEST_ASSERT_FALSE_MESSAGE(source.cacheable,
            "post-exhaustion allocations must stay uncacheable");
    }

    /* A real fault after exhaustion must not restart the allocator either. */
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK,
        yan_fs_format_metadata(device.blocks[0], DEVICE_BLOCKS));
    YanFs faulted;
    memset(&faulted, 0, sizeof faulted);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_init(&faulted, backend()));
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_mount(&faulted));
    uint8_t byte = 0x33u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_create(&faulted, "x", &byte, 1u));
    device.reads = 0u;
    device.read_fail_at = 1u;
    uint32_t got = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO,
        yan_fs_read(&faulted, "x", 0u, &byte, 1u, &got));
    device.read_fail_at = 0u;
    YanFsSource after_fault = observe(&faulted);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_STATE_FAULTED, after_fault.state,
        "the injected read error must be observable as FAULTED");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(0u, after_fault.token,
        "a post-exhaustion fault must not mint a fresh token");
    TEST_ASSERT_FALSE_MESSAGE(after_fault.cacheable,
        "a post-exhaustion fault must stay uncacheable");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(source_allocator_hands_out_distinct_live_identities);
    RUN_TEST(source_allocator_rejections_and_read_only_do_not_allocate);
    RUN_TEST(source_allocator_exhaustion_crosses_max_without_wrap);
    return UNITY_END();
}
