#include "search_terms.h"
#include "search_terms_index.h"
#include "yanfs.h"
#include "unity.h"

#include <string.h>

/* 0026 index source-exhaustion variant.
 *
 * This target compiles its own copy of os/yanfs.c with a near-UINT64_MAX
 * YAN_FS_SOURCE_TOKEN_SEED (it must not link the yan_fs library: that would
 * define yanfs.c twice). The allocator is driven only through real public FS
 * operations; the test never writes source_token or source_cacheable.
 *
 * Contract under test: once the allocator is exhausted, an index instance that
 * already holds a valid stored token must report UNCACHEABLE with zero counts
 * and a search must fall back to the direct SCAN, without trying to rebuild.
 * The status path reads the real cacheable flag from the public getter, so the
 * UNCACHEABLE answer comes from the exhausted allocator, not from a stub. */

#define EX_BLOCKS 16u

typedef struct {
    uint8_t (*blocks)[YAN_FS_BLOCK_SIZE];
    uint32_t capacity;
    uint32_t reads;
    uint32_t writes;
} ExDevice;

static uint8_t medium_a[EX_BLOCKS][YAN_FS_BLOCK_SIZE];
static uint8_t medium_b[EX_BLOCKS][YAN_FS_BLOCK_SIZE];
static ExDevice dev_a;
static ExDevice dev_b;
static YanFs fs_a;
static YanFs fs_b;
static YanSearchTermsIndex index_a;
static YanSearchTerms terms_a;
static uint32_t ex_matches;

static YanFsIoResult ex_capacity(void *context, uint64_t *blocks)
{
    ExDevice *self = (ExDevice *)context;
    *blocks = self->capacity;
    return YAN_FS_IO_OK;
}

static YanFsIoResult ex_read(void *context, uint32_t lba,
                             uint8_t out[4096])
{
    ExDevice *self = (ExDevice *)context;
    ++self->reads;
    if (lba >= self->capacity) {
        return YAN_FS_IO_ERROR;
    }
    memcpy(out, self->blocks[lba], YAN_FS_BLOCK_SIZE);
    return YAN_FS_IO_OK;
}

static YanFsIoResult ex_write(void *context, uint32_t lba,
                              const uint8_t data[4096])
{
    ExDevice *self = (ExDevice *)context;
    ++self->writes;
    if (lba >= self->capacity) {
        return YAN_FS_IO_ERROR;
    }
    memcpy(self->blocks[lba], data, YAN_FS_BLOCK_SIZE);
    return YAN_FS_IO_OK;
}

static YanFsBlockIo ex_backend(ExDevice *device)
{
    YanFsBlockIo io;
    io.context = device;
    io.capacity = ex_capacity;
    io.read_block = ex_read;
    io.write_block = ex_write;
    return io;
}

static bool ex_match(void *context, const YanSearchTermsMatch *match)
{
    (void)context;
    (void)match;
    ++ex_matches;
    return true;
}

void setUp(void)
{
    memset(medium_a, 0, sizeof medium_a);
    memset(medium_b, 0, sizeof medium_b);
    memset(&dev_a, 0, sizeof dev_a);
    memset(&dev_b, 0, sizeof dev_b);
    dev_a.blocks = medium_a;
    dev_a.capacity = EX_BLOCKS;
    dev_b.blocks = medium_b;
    dev_b.capacity = EX_BLOCKS;
    memset(&fs_a, 0, sizeof fs_a);
    memset(&fs_b, 0, sizeof fs_b);
    memset(&index_a, 0, sizeof index_a);
    memset(&terms_a, 0, sizeof terms_a);
    ex_matches = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK,
                          yan_fs_format_metadata(medium_a[0], EX_BLOCKS));
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK,
                          yan_fs_format_metadata(medium_b[0], EX_BLOCKS));
}

void tearDown(void)
{
}

static void index_source_exhaustion_is_uncacheable_and_scans(void)
{
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(
        UINT64_MAX - 64u, (uint64_t)YAN_FS_SOURCE_TOKEN_SEED,
        "this suite is pinned to YAN_FS_SOURCE_TOKEN_SEED = UINT64_MAX - 64");

    /* Instance A holds a live source and an attached index before exhaustion. */
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_init(&fs_a, ex_backend(&dev_a)));
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_mount(&fs_a));
    TEST_ASSERT_EQUAL_INT(
        YAN_FS_OK,
        yan_fs_create(&fs_a, "note.md", (const uint8_t *)"hit\n", 4u));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          yan_search_terms_index_init(&index_a, &fs_a));
    TEST_ASSERT_EQUAL_INT(
        YAN_SEARCH_TERMS_OK,
        yan_search_terms_init(&terms_a,
                              yan_search_terms_index_backend(&index_a)));

    /* Drive the allocator across UINT64_MAX through B only, with real
     * init/mount/published-create calls. */
    bool exhausted = false;
    for (uint32_t round = 0u; round < 200u && !exhausted; ++round) {
        memset(medium_b, 0, sizeof medium_b);
        TEST_ASSERT_EQUAL_INT(
            YAN_FS_OK, yan_fs_format_metadata(medium_b[0], EX_BLOCKS));
        memset(&fs_b, 0, sizeof fs_b);
        TEST_ASSERT_EQUAL_INT(YAN_FS_OK,
                              yan_fs_init(&fs_b, ex_backend(&dev_b)));
        TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_mount(&fs_b));
        TEST_ASSERT_EQUAL_INT(
            YAN_FS_OK,
            yan_fs_create(&fs_b, "x.md", (const uint8_t *)"y\n", 2u));
        YanFsSource source_b;
        memset(&source_b, 0, sizeof source_b);
        TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_source(&fs_b, &source_b));
        if (source_b.token == 0u) {
            exhausted = true;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(exhausted,
                             "the near-MAX seed must be crossed through B");

    /* A's stored identity is still a valid token, but the process allocator is
     * exhausted, so the getter must report it uncacheable. */
    YanFsSource source_a;
    memset(&source_a, 0, sizeof source_a);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_source(&fs_a, &source_a));
    TEST_ASSERT_TRUE_MESSAGE(source_a.token != 0u,
                             "A must keep the identity it was given before");
    TEST_ASSERT_FALSE_MESSAGE(source_a.cacheable,
                              "global exhaustion makes every identity "
                              "uncacheable");

    YanSearchTermsStatus status;
    memset(&status, 0, sizeof status);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          yan_search_terms_status(&terms_a, &status));
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_INDEX_UNCACHEABLE, status.state,
        "an exhausted allocator must report UNCACHEABLE");
    TEST_ASSERT_EQUAL_UINT32(0u, status.terms);
    TEST_ASSERT_EQUAL_UINT32(0u, status.postings);

    uint32_t reads_before_rebuild = dev_a.reads;
    uint32_t writes_before_rebuild = dev_a.writes;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_TERMS_OK,
                                  yan_search_terms_rebuild(&terms_a),
                                  "approved uncacheable rebuild returns OK");
    TEST_ASSERT_EQUAL_UINT32(reads_before_rebuild, dev_a.reads);
    TEST_ASSERT_EQUAL_UINT32(writes_before_rebuild, dev_a.writes);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          yan_search_terms_status(&terms_a, &status));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_INDEX_UNCACHEABLE, status.state);
    TEST_ASSERT_EQUAL_UINT32(0u, status.terms);
    TEST_ASSERT_EQUAL_UINT32(0u, status.postings);

    /* The search must fall back to a complete SCAN and still find the row. */
    ex_matches = 0u;
    YanSearchTermsSummary summary;
    memset(&summary, 0, sizeof summary);
    YanSearchTermsSink sink;
    sink.context = NULL;
    sink.match = ex_match;
    sink.chunk = NULL;
    YanSearchTermsResult result = yan_search_terms(
        &terms_a, (const uint8_t *)"hit", 3u, sink, &summary);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK, result);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_MODE_SCAN, summary.mode,
                                  "an uncacheable source must scan");
    TEST_ASSERT_EQUAL_UINT32(1u, ex_matches);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(index_source_exhaustion_is_uncacheable_and_scans);
    return UNITY_END();
}
