#include "search_terms.h"
#include "search_terms_index.h"
#include "search_terms_linear.h"
#include "yanfs.h"
#include "unity.h"

#include <string.h>

/* 0026 in-memory index contract tests.
 *
 * Every case goes through the public facade with an injected index backend
 * over a real mounted YanFs on a caller-owned static 8 MiB byte device. A
 * second facade wraps a plain linear backend so the same query can be compared
 * row for row (name, line, score, truncation flags, raw snippet) and summary
 * for summary. The index build/lookup algorithm is now implemented, so the
 * indexed-only expectations (READY counts, mode=INDEX, STALE, LIMIT, capacity
 * boundaries) are asserted against the real build. Captures deep-copy every name
 * and raw snippet byte, so no borrowed pointer survives the callback. No test
 * writes a filesystem identity field or an index private field. */

#define INDEX_BLOCKS 2048u /* 8 MiB */
#define CAP_MAX 24

static uint8_t medium[INDEX_BLOCKS][YAN_FS_BLOCK_SIZE];
static uint32_t device_reads;
static uint32_t device_writes;
static uint32_t device_read_fail_at;
static YanFsIoResult device_read_fail_code;
static YanFsIoResult device_capacity_status;
static YanFs fs;

static YanSearchTermsIndex index;
static YanSearchTerms terms;
static YanSearchTermsLinear linear_ref;
static YanSearchTerms terms_ref;
/* Every index context is statically owned; the object is over a MiB and must
 * never sit on a task stack. */
static YanSearchTermsIndex spare_index;
static YanSearchTermsIndex virgin_index;
static YanSearchTerms virgin_terms;
static YanFs virgin_fs;

/* Reentrancy probe: run from inside a device read callback while the facade is
 * busy with an indexed query. */
static bool reenter_armed;
static bool reenter_ran;
static YanSearchTerms *reenter_terms;
static YanSearchTermsResult reenter_result;
static bool backend_status_armed;
static bool backend_status_ran;
static YanSearchTermsStatus backend_status_holder;
static YanSearchTermsResult backend_status_result;

static void reenter_run(void)
{
    if (!reenter_armed) {
        return;
    }
    reenter_armed = false;
    reenter_ran = true;
    YanSearchTermsStatus status;
    memset(&status, 0, sizeof status);
    reenter_result = yan_search_terms_status(reenter_terms, &status);
}

/* ------------------------------------------------------------ byte device */

static YanFsIoResult device_capacity(void *context, uint64_t *blocks)
{
    (void)context;
    if (device_capacity_status != YAN_FS_IO_OK) {
        return device_capacity_status;
    }
    *blocks = INDEX_BLOCKS;
    return YAN_FS_IO_OK;
}

static YanFsIoResult device_read(void *context, uint32_t lba,
                                 uint8_t out[4096])
{
    (void)context;
    ++device_reads;
    if (backend_status_armed) {
        backend_status_armed = false;
        backend_status_ran = true;
        YanSearchTermsBackend backend = yan_search_terms_index_backend(&index);
        backend_status_result = backend.status(backend.context,
                                               &backend_status_holder);
    }
    reenter_run();
    if (device_read_fail_at != 0u && device_reads == device_read_fail_at) {
        return device_read_fail_code;
    }
    if (lba >= INDEX_BLOCKS) {
        return YAN_FS_IO_ERROR;
    }
    memcpy(out, medium[lba], YAN_FS_BLOCK_SIZE);
    return YAN_FS_IO_OK;
}

static YanFsIoResult device_write(void *context, uint32_t lba,
                                  const uint8_t data[4096])
{
    (void)context;
    ++device_writes;
    if (lba >= INDEX_BLOCKS) {
        return YAN_FS_IO_ERROR;
    }
    memcpy(medium[lba], data, YAN_FS_BLOCK_SIZE);
    return YAN_FS_IO_OK;
}

static YanFsBlockIo fs_backend(void)
{
    YanFsBlockIo io;
    io.context = NULL;
    io.capacity = device_capacity;
    io.read_block = device_read;
    io.write_block = device_write;
    return io;
}

/* ------------------------------------------------------------- capture */

typedef struct {
    uint32_t matches;
    char name[CAP_MAX][32];
    uint32_t line[CAP_MAX];
    uint16_t score[CAP_MAX];
    bool left[CAP_MAX];
    bool right[CAP_MAX];
    uint32_t snippet_length[CAP_MAX];
    uint8_t snippet[CAP_MAX][YAN_SEARCH_TERMS_SNIPPET_MAX];
    YanSearchTermsSummary summary;
} Capture;

static Capture capture_index;
static Capture capture_ref;
static YanSearchTerms *capture_terms;

static bool capture_match(void *context, const YanSearchTermsMatch *match)
{
    Capture *self = (Capture *)context;
    uint32_t i = self->matches;
    if (i < CAP_MAX) {
        self->line[i] = match->line_number;
        self->score[i] = match->score;
        self->left[i] = match->left_truncated;
        self->right[i] = match->right_truncated;
        uint32_t n = 0u;
        while (n < 31u && match->name[n] != '\0') {
            self->name[i][n] = match->name[n];
            ++n;
        }
        self->name[i][n] = '\0';
    }
    ++self->matches;
    if (capture_terms != NULL && i < CAP_MAX) {
        const uint8_t *bytes = NULL;
        uint32_t length = 0u;
        YanSearchTermsResult result = yan_search_terms_read_snippet(
            capture_terms, 0u, YAN_SEARCH_TERMS_SNIPPET_MAX, &bytes, &length);
        if (result == YAN_SEARCH_TERMS_OK && bytes != NULL &&
            length <= YAN_SEARCH_TERMS_SNIPPET_MAX) {
            self->snippet_length[i] = length;
            memcpy(self->snippet[i], bytes, length);
        }
    }
    return true;
}

static YanSearchTermsResult capture_run(YanSearchTerms *facade, Capture *cap,
                                        const char *query)
{
    memset(cap, 0, sizeof *cap);
    capture_terms = facade;
    YanSearchTermsSink sink;
    sink.context = cap;
    sink.match = capture_match;
    sink.chunk = NULL;
    return yan_search_terms(facade, (const uint8_t *)query,
                            (uint32_t)strlen(query), sink, &cap->summary);
}

static void assert_rows_equal(const Capture *expected, const Capture *actual)
{
    TEST_ASSERT_EQUAL_UINT32(expected->matches, actual->matches);
    uint32_t count = expected->matches < CAP_MAX ? expected->matches : CAP_MAX;
    for (uint32_t i = 0u; i < count; ++i) {
        TEST_ASSERT_EQUAL_STRING(expected->name[i], actual->name[i]);
        TEST_ASSERT_EQUAL_UINT32(expected->line[i], actual->line[i]);
        TEST_ASSERT_EQUAL_UINT16(expected->score[i], actual->score[i]);
        TEST_ASSERT_EQUAL_INT(expected->left[i], actual->left[i]);
        TEST_ASSERT_EQUAL_INT(expected->right[i], actual->right[i]);
        TEST_ASSERT_EQUAL_UINT32(expected->snippet_length[i],
                                 actual->snippet_length[i]);
        if (expected->snippet_length[i] > 0u) {
            TEST_ASSERT_EQUAL_MEMORY(expected->snippet[i], actual->snippet[i],
                                     expected->snippet_length[i]);
        }
    }
    TEST_ASSERT_EQUAL_UINT64(expected->summary.total, actual->summary.total);
    TEST_ASSERT_EQUAL_UINT32(expected->summary.shown, actual->summary.shown);
    TEST_ASSERT_EQUAL_UINT32(expected->summary.skipped, actual->summary.skipped);
}

/* ------------------------------------------------------------- fixtures */

static void mount_fs(void)
{
    memset(&fs, 0, sizeof fs);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_init(&fs, fs_backend()));
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_mount(&fs));
}

static void create_file(const char *name, const uint8_t *bytes, uint32_t length)
{
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_create(&fs, name, bytes, length));
}

static void index_attach(void)
{
    memset(&index, 0, sizeof index);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          yan_search_terms_index_init(&index, &fs));
    memset(&terms, 0, sizeof terms);
    TEST_ASSERT_EQUAL_INT(
        YAN_SEARCH_TERMS_OK,
        yan_search_terms_init(&terms, yan_search_terms_index_backend(&index)));
    capture_terms = &terms;
}

static void ref_attach(void)
{
    memset(&linear_ref, 0, sizeof linear_ref);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          yan_search_terms_linear_init(&linear_ref, &fs));
    memset(&terms_ref, 0, sizeof terms_ref);
    TEST_ASSERT_EQUAL_INT(
        YAN_SEARCH_TERMS_OK,
        yan_search_terms_init(&terms_ref,
                              yan_search_terms_linear_backend(&linear_ref)));
}

static YanSearchTermsStatus status_now(void)
{
    YanSearchTermsStatus status;
    memset(&status, 0, sizeof status);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          yan_search_terms_status(&terms, &status));
    return status;
}

/* Deterministic source builders. They write whole words separated by LF; the
 * exact byte totals are asserted by the capacity cases. */
static uint32_t emit_base26(uint8_t *out, uint32_t value, uint32_t width)
{
    for (uint32_t i = 0u; i < width; ++i) {
        out[width - 1u - i] = (uint8_t)('a' + (value % 26u));
        value /= 26u;
    }
    return width;
}

static uint8_t payload[300000];

static uint32_t build_keys_source(uint32_t count, uint32_t word_length)
{
    uint32_t at = 0u;
    for (uint32_t i = 0u; i < count; ++i) {
        at += emit_base26(payload + at, i, 4u);
        for (uint32_t j = 4u; j < word_length; ++j) {
            payload[at++] = (uint8_t)'a';
        }
        payload[at++] = (uint8_t)'\n';
    }
    return at;
}

static uint32_t build_repeated_word_source(uint32_t count)
{
    uint32_t at = 0u;
    for (uint32_t i = 0u; i < count; ++i) {
        payload[at++] = (uint8_t)'a';
        payload[at++] = (uint8_t)' ';
    }
    payload[at++] = (uint8_t)'\n';
    return at;
}

/* ------------------------------------------------------------- lifecycle */

void setUp(void)
{
    memset(medium, 0, sizeof medium);
    device_reads = 0u;
    device_writes = 0u;
    device_read_fail_at = 0u;
    device_read_fail_code = YAN_FS_IO_ERROR;
    device_capacity_status = YAN_FS_IO_OK;
    memset(&fs, 0, sizeof fs);
    memset(&index, 0, sizeof index);
    memset(&terms, 0, sizeof terms);
    memset(&linear_ref, 0, sizeof linear_ref);
    memset(&terms_ref, 0, sizeof terms_ref);
    memset(&spare_index, 0, sizeof spare_index);
    memset(&virgin_index, 0, sizeof virgin_index);
    memset(&virgin_terms, 0, sizeof virgin_terms);
    memset(&virgin_fs, 0, sizeof virgin_fs);
    memset(&capture_index, 0, sizeof capture_index);
    memset(&capture_ref, 0, sizeof capture_ref);
    reenter_armed = false;
    reenter_ran = false;
    reenter_terms = NULL;
    reenter_result = YAN_SEARCH_TERMS_OK;
    backend_status_armed = false;
    backend_status_ran = false;
    memset(&backend_status_holder, 0xA5, sizeof backend_status_holder);
    backend_status_result = YAN_SEARCH_TERMS_OK;
    capture_terms = NULL;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK,
                          yan_fs_format_metadata(medium[0], INDEX_BLOCKS));
}

void tearDown(void)
{
}

static void index_init_rejects_null_and_alias(void)
{
    mount_fs();
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_INVALID,
                          yan_search_terms_index_init(NULL, &fs));
    memset(&spare_index, 0, sizeof spare_index);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_INVALID,
                          yan_search_terms_index_init(&spare_index, NULL));
    /* The context placed over the borrowed filesystem overlaps its own span. */
    TEST_ASSERT_EQUAL_INT(
        YAN_SEARCH_TERMS_INVALID,
        yan_search_terms_index_init((YanSearchTermsIndex *)(void *)&fs, &fs));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          yan_search_terms_index_init(&spare_index, &fs));
    TEST_ASSERT_TRUE(spare_index.initialized);
    /* An idle context may be re-initialized. */
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          yan_search_terms_index_init(&spare_index, &fs));
}

static void index_status_empty_is_zero_io(void)
{
    mount_fs();
    index_attach();
    uint32_t reads = device_reads;
    uint32_t writes = device_writes;
    YanSearchTermsStatus status = status_now();
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_INDEX_EMPTY, status.state);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_SOURCE_MOUNTED, status.source);
    TEST_ASSERT_EQUAL_UINT32(0u, status.terms);
    TEST_ASSERT_EQUAL_UINT32(0u, status.postings);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(reads, device_reads,
                                     "status must not read the medium");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(writes, device_writes,
                                     "status must not write the medium");
}

static void index_clear_is_zero_io_and_does_not_recover(void)
{
    mount_fs();
    index_attach();
    uint32_t reads = device_reads;
    uint32_t writes = device_writes;
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK, yan_search_terms_clear(&terms));
    TEST_ASSERT_EQUAL_UINT32(reads, device_reads);
    TEST_ASSERT_EQUAL_UINT32(writes, device_writes);

    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_unmount(&fs));
    YanSearchTermsStatus status = status_now();
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_INDEX_UNAVAILABLE, status.state);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_SOURCE_UNMOUNTED, status.source);
    TEST_ASSERT_EQUAL_UINT32(0u, status.terms);
    /* clear is allowed on an unavailable source and must not revive it. */
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK, yan_search_terms_clear(&terms));
    status = status_now();
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_INDEX_UNAVAILABLE, status.state);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_SOURCE_UNMOUNTED, status.source);
}

static void index_rebuild_empty_is_ready(void)
{
    mount_fs();
    index_attach();
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          yan_search_terms_rebuild(&terms));
    YanSearchTermsStatus status = status_now();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_INDEX_READY, status.state,
                                  "rebuilding an empty source is a valid READY");
    TEST_ASSERT_EQUAL_UINT32(0u, status.terms);
    TEST_ASSERT_EQUAL_UINT32(0u, status.postings);
}

/* ------------------------------------------------ delegated SCAN parity */

static const uint8_t example_source[] =
    "interruption pending\nInterrupt here\n"
    "\xe4\xb8\xad x \xe6\x96\xad\n"
    "\xe5\xa4\x84\xe7\x90\x86\xe4\xb8\xad\xe6\x96\xad\n"
    "CPU cpu CPU\n"
    "aaaa\n"
    "\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad\n"
    "hit\rnext hit\r\nhit\r\n";

static void index_query_rows_match_direct_linear(void)
{
    static const char *queries[] = {
        "interrupt", "CPU cpu",
        "\xe4\xb8\xad\xe6\x96\xad",
        "CPU\xe5\xa4\x84\xe7\x90\x86\xe4\xb8\xad\xe6\x96\xad",
        "aa", "\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad", "hit", "hit next"
    };
    mount_fs();
    create_file("a.md", example_source,
                (uint32_t)(sizeof example_source - 1u));
    index_attach();
    ref_attach();

    for (uint32_t q = 0u;
         q < (uint32_t)(sizeof queries / sizeof queries[0]); ++q) {
        YanSearchTermsResult indexed =
            capture_run(&terms, &capture_index, queries[q]);
        YanSearchTermsResult linear =
            capture_run(&terms_ref, &capture_ref, queries[q]);
        TEST_ASSERT_EQUAL_INT_MESSAGE(linear, indexed,
                                      "indexed and linear results must agree");
        assert_rows_equal(&capture_ref, &capture_index);
    }
}

static void index_first_search_publishes_ready(void)
{
    mount_fs();
    create_file("a.md", (const uint8_t *)"hit\n", 4u);
    index_attach();
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK, capture_run(&terms,
                                                           &capture_index,
                                                           "hit"));
    YanSearchTermsStatus status = status_now();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_INDEX_READY, status.state,
                                  "the first search must publish READY");
    TEST_ASSERT_TRUE_MESSAGE(status.terms > 0u,
                             "a READY index must report its keys");
    TEST_ASSERT_TRUE_MESSAGE(status.postings > 0u,
                             "a READY index must report its postings");
}

static void index_query_summary_mode_is_index(void)
{
    mount_fs();
    create_file("a.md", (const uint8_t *)"hit\n", 4u);
    index_attach();
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          capture_run(&terms, &capture_index, "hit"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_MODE_INDEX,
                                  capture_index.summary.mode,
                                  "an indexed search must report mode=INDEX");
}

static void index_second_search_reuses_cache_without_build_writes(void)
{
    mount_fs();
    create_file("a.md", (const uint8_t *)"hit\n", 4u);
    index_attach();
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          capture_run(&terms, &capture_index, "hit"));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_INDEX_READY, status_now().state);
    uint32_t writes = device_writes;
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          capture_run(&terms, &capture_index, "hit"));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(writes, device_writes,
                                     "a cached search must not build or write");
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_MODE_INDEX, capture_index.summary.mode);
}

/* ------------------------------------------------------ invalidation */

static void index_create_invalidates_ready_to_stale(void)
{
    mount_fs();
    create_file("a.md", (const uint8_t *)"hit\n", 4u);
    index_attach();
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          capture_run(&terms, &capture_index, "hit"));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_INDEX_READY, status_now().state);

    create_file("b.md", (const uint8_t *)"hit\n", 4u);
    YanSearchTermsStatus stale = status_now();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_INDEX_STALE, stale.state,
                                  "a published mutation must invalidate READY");
    TEST_ASSERT_EQUAL_UINT32(0u, stale.terms);
    TEST_ASSERT_EQUAL_UINT32(0u, stale.postings);

    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          capture_run(&terms, &capture_index, "hit"));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_INDEX_READY, status_now().state);
}

static void index_unchanged_operations_keep_ready(void)
{
    mount_fs();
    create_file("a.md", (const uint8_t *)"hit\n", 4u);
    create_file("b.md", (const uint8_t *)"hit\n", 4u);
    index_attach();
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          capture_run(&terms, &capture_index, "hit"));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_INDEX_READY, status_now().state);

    TEST_ASSERT_EQUAL_INT(YAN_FS_EXISTS,
                          yan_fs_create(&fs, "a.md", (const uint8_t *)"x", 1u));
    TEST_ASSERT_EQUAL_INT(YAN_FS_NOT_FOUND, yan_fs_remove(&fs, "missing"));
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_rename(&fs, "a.md", "a.md"));
    YanFsInfo info;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_stat(&fs, "a.md", &info));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_INDEX_READY, status_now().state);
}

static void index_unavailable_sources_report_zero_counts(void)
{
    mount_fs();
    create_file("a.md", (const uint8_t *)"hit\n", 4u);
    index_attach();

    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_unmount(&fs));
    YanSearchTermsStatus status = status_now();
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_INDEX_UNAVAILABLE, status.state);
    TEST_ASSERT_EQUAL_UINT32(0u, status.terms);
    TEST_ASSERT_EQUAL_UINT32(0u, status.postings);

    device_capacity_status = YAN_FS_IO_ERROR;
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO, yan_fs_mount(&fs));
    status = status_now();
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_INDEX_UNAVAILABLE, status.state);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_SOURCE_FAULTED, status.source);
    TEST_ASSERT_EQUAL_UINT32(0u, status.terms);
    device_capacity_status = YAN_FS_IO_OK;

    /* An uninitialized filesystem maps to UNINITIALIZED/UNAVAILABLE. */
    memset(&virgin_fs, 0, sizeof virgin_fs);
    memset(&virgin_index, 0, sizeof virgin_index);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          yan_search_terms_index_init(&virgin_index,
                                                      &virgin_fs));
    memset(&virgin_terms, 0, sizeof virgin_terms);
    TEST_ASSERT_EQUAL_INT(
        YAN_SEARCH_TERMS_OK,
        yan_search_terms_init(&virgin_terms,
                              yan_search_terms_index_backend(&virgin_index)));
    YanSearchTermsStatus virgin_status;
    memset(&virgin_status, 0, sizeof virgin_status);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          yan_search_terms_status(&virgin_terms,
                                                  &virgin_status));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_INDEX_UNAVAILABLE, virgin_status.state);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_SOURCE_UNINITIALIZED,
                          virgin_status.source);
}

/* ------------------------------------------------------ capacity bounds */

static void index_capacity_exact_keys_ready(void)
{
    mount_fs();
    uint32_t length = build_keys_source(YAN_SEARCH_TERMS_INDEX_KEYS, 5u);
    create_file("src.md", payload, length);
    index_attach();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_TERMS_OK,
                                  yan_search_terms_rebuild(&terms),
                                  "8192 distinct keys must fit");
    YanSearchTermsStatus status = status_now();
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_INDEX_READY, status.state);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(YAN_SEARCH_TERMS_INDEX_KEYS,
                                     status.terms,
                                     "READY must report all 8192 keys");
    TEST_ASSERT_EQUAL_UINT32(YAN_SEARCH_TERMS_INDEX_KEYS, status.postings);
}

static void index_capacity_8193_keys_limit(void)
{
    mount_fs();
    uint32_t length =
        build_keys_source(YAN_SEARCH_TERMS_INDEX_KEYS + 1u, 5u);
    create_file("src.md", payload, length);
    index_attach();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_TERMS_INDEX_LIMIT, yan_search_terms_rebuild(&terms),
        "8193 distinct keys must not publish a partial index");
    YanSearchTermsStatus status = status_now();
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_INDEX_LIMIT, status.state);
    TEST_ASSERT_EQUAL_UINT32(0u, status.terms);
    TEST_ASSERT_EQUAL_UINT32(0u, status.postings);
}

static void index_capacity_key_pool_exact_ready(void)
{
    mount_fs();
    uint32_t length = build_keys_source(1028u, 255u);
    TEST_ASSERT_EQUAL_UINT32(1028u * 255u, length - 1028u);
    memcpy(payload + length, "zzzz\n", 5u);
    length += 5u; /* +4 raw key bytes == 262144 in total */
    create_file("src.md", payload, length);
    index_attach();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_TERMS_OK,
                                  yan_search_terms_rebuild(&terms),
                                  "a 262144-byte key pool must fit exactly");
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_INDEX_READY, status_now().state);
}

static void index_capacity_key_pool_over_limit(void)
{
    mount_fs();
    uint32_t length = build_keys_source(1028u, 255u);
    memcpy(payload + length, "zzzzz\n", 6u);
    length += 6u; /* +5 raw key bytes == 262145, one over */
    create_file("src.md", payload, length);
    index_attach();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_TERMS_INDEX_LIMIT, yan_search_terms_rebuild(&terms),
        "one key byte over the pool must be INDEX_LIMIT");
    YanSearchTermsStatus status = status_now();
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_INDEX_LIMIT, status.state);
    TEST_ASSERT_EQUAL_UINT32(0u, status.terms);
    TEST_ASSERT_EQUAL_UINT32(0u, status.postings);
}

static void index_capacity_postings_exact_ready(void)
{
    mount_fs();
    uint32_t length = build_repeated_word_source(
        YAN_SEARCH_TERMS_INDEX_POSTINGS);
    create_file("src.md", payload, length);
    index_attach();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_TERMS_OK,
                                  yan_search_terms_rebuild(&terms),
                                  "65536 postings must fit exactly");
    YanSearchTermsStatus status = status_now();
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_INDEX_READY, status.state);
    TEST_ASSERT_EQUAL_UINT32(1u, status.terms);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(YAN_SEARCH_TERMS_INDEX_POSTINGS,
                                     status.postings,
                                     "READY must report all 65536 postings");
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          capture_run(&terms, &capture_index, "a"));
    TEST_ASSERT_EQUAL_UINT64(1u, capture_index.summary.total);
    TEST_ASSERT_EQUAL_UINT32(1u, capture_index.matches);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(255u, capture_index.score[0],
                                    "indexed group count must clamp at 255");
}

static void index_capacity_postings_over_limit(void)
{
    mount_fs();
    uint32_t length = build_repeated_word_source(
        YAN_SEARCH_TERMS_INDEX_POSTINGS + 1u);
    create_file("src.md", payload, length);
    index_attach();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_TERMS_INDEX_LIMIT, yan_search_terms_rebuild(&terms),
        "65537 postings must be INDEX_LIMIT");
    YanSearchTermsStatus status = status_now();
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_INDEX_LIMIT, status.state);
    TEST_ASSERT_EQUAL_UINT32(0u, status.terms);
    TEST_ASSERT_EQUAL_UINT32(0u, status.postings);
}

/* --------------------------------------------------------- limit / scan */

static void index_auto_limit_falls_back_to_scan(void)
{
    mount_fs();
    uint32_t length = build_keys_source(1028u, 255u);
    memcpy(payload + length, "zzzzz\n", 6u);
    length += 6u;
    memcpy(payload + length, "hit\n", 4u);
    length += 4u;
    create_file("src.md", payload, length);
    index_attach();
    YanSearchTermsResult result =
        capture_run(&terms, &capture_index, "hit");
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK, result);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_MODE_SCAN,
                                  capture_index.summary.mode,
                                  "an over-capacity source must scan");
    TEST_ASSERT_EQUAL_UINT32(1u, capture_index.matches);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_INDEX_LIMIT, status_now().state,
        "the observed limit must be remembered as LIMIT");
}

static void index_long_source_word_limits_but_still_scans(void)
{
    mount_fs();
    static uint8_t source[300];
    uint32_t at = 0u;
    for (uint32_t i = 0u; i < 256u; ++i) {
        source[at++] = (uint8_t)'a';
    }
    source[at++] = (uint8_t)'\n';
    memcpy(source + at, "hit\n", 4u);
    at += 4u;
    create_file("src.md", source, at);
    index_attach();
    YanSearchTermsResult result = capture_run(&terms, &capture_index, "hit");
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK, result);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        1u, capture_index.matches,
        "a 256-byte source word must not hide the legitimate short word");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_INDEX_LIMIT, status_now().state,
        "an over-long source word must keep the index at LIMIT");
}

/* ------------------------------------------------------------- faults */

static void index_io_fault_propagates_without_ok(void)
{
    mount_fs();
    create_file("a.md", (const uint8_t *)"hit\n", 4u);
    index_attach();
    device_reads = 0u;
    device_read_fail_at = 2u; /* the scan read, not the preflight */
    YanSearchTermsResult result =
        capture_run(&terms, &capture_index, "hit");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_TERMS_IO, result,
                                  "a real read fault must propagate");
    device_read_fail_at = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_INDEX_UNAVAILABLE, status_now().state);
}

static void index_reentrant_management_is_busy(void)
{
    /* The facade busy window rejects a nested status/clear/rebuild/query while
     * an indexed query is inside a device callback. */
    mount_fs();
    create_file("a.md", (const uint8_t *)"hit\n", 4u);
    index_attach();
    reenter_armed = true;
    reenter_terms = &terms;
    reenter_result = YAN_SEARCH_TERMS_OK;
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          capture_run(&terms, &capture_index, "hit"));
    TEST_ASSERT_TRUE_MESSAGE(reenter_ran, "the probe must have run");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_TERMS_BUSY, reenter_result,
                                  "nested management must answer BUSY");
}

static bool stopping_match(void *context, const YanSearchTermsMatch *match)
{
    (void)context;
    (void)match;
    return false;
}

/* Asserts the cached READY was invalidated by a published mutation, then
 * rebuilds through an ordinary query and confirms READY again. */
static void assert_stale_then_rebuild(const char *query)
{
    YanSearchTermsStatus stale = status_now();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_INDEX_STALE, stale.state,
                                  "a published mutation must invalidate READY");
    TEST_ASSERT_EQUAL_UINT32(0u, stale.terms);
    TEST_ASSERT_EQUAL_UINT32(0u, stale.postings);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          capture_run(&terms, &capture_index, query));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_INDEX_READY, status_now().state);
}

static void index_replace_remove_copy_rename_invalidate_ready(void)
{
    mount_fs();
    create_file("a.md", (const uint8_t *)"hit\n", 4u);
    create_file("b.md", (const uint8_t *)"hit\n", 4u);
    index_attach();
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          capture_run(&terms, &capture_index, "hit"));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_INDEX_READY, status_now().state);
    TEST_ASSERT_EQUAL_UINT32(2u, capture_index.matches);

    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_replace(
                                         &fs, "a.md",
                                         (const uint8_t *)"hit hit\n", 8u));
    assert_stale_then_rebuild("hit");
    TEST_ASSERT_EQUAL_UINT32(2u, capture_index.matches);

    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_copy(&fs, "a.md", "c.md"));
    assert_stale_then_rebuild("hit");
    TEST_ASSERT_EQUAL_UINT32(3u, capture_index.matches);

    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_rename(&fs, "c.md", "d.md"));
    assert_stale_then_rebuild("hit");
    TEST_ASSERT_EQUAL_UINT32(3u, capture_index.matches);

    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_remove(&fs, "d.md"));
    assert_stale_then_rebuild("hit");
    TEST_ASSERT_EQUAL_UINT32(2u, capture_index.matches);
}

static void index_remount_and_address_reuse_invalidate_ready(void)
{
    mount_fs();
    create_file("a.md", (const uint8_t *)"hit\n", 4u);
    index_attach();
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          capture_run(&terms, &capture_index, "hit"));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_INDEX_READY, status_now().state);

    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_unmount(&fs));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_INDEX_UNAVAILABLE, status_now().state);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_mount(&fs));
    YanSearchTermsStatus status = status_now();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_INDEX_STALE, status.state,
                                  "a remount must invalidate the old READY");
    TEST_ASSERT_EQUAL_UINT32(0u, status.terms);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          capture_run(&terms, &capture_index, "hit"));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_INDEX_READY, status_now().state);

    /* Reusing the same address with a fresh init must not match the old token. */
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_unmount(&fs));
    memset(&fs, 0, sizeof fs);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_init(&fs, fs_backend()));
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_mount(&fs));
    status = status_now();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_INDEX_STALE, status.state,
                                  "address reuse must not revive the old READY");
    TEST_ASSERT_EQUAL_UINT32(0u, status.terms);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          capture_run(&terms, &capture_index, "hit"));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_INDEX_READY, status_now().state);
}

static void index_ready_no_result_reads_nothing(void)
{
    mount_fs();
    create_file("a.md", (const uint8_t *)"hit\n", 4u);
    index_attach();
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          capture_run(&terms, &capture_index, "hit"));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_INDEX_READY, status_now().state);
    uint32_t reads = device_reads;
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          capture_run(&terms, &capture_index, "ghost"));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        reads, device_reads,
        "a READY zero-result query must not read the medium");
    TEST_ASSERT_EQUAL_UINT32(0u, capture_index.matches);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_MODE_INDEX, capture_index.summary.mode);
}

static void index_same_identity_limit_does_not_rebuild(void)
{
    mount_fs();
    uint32_t length = build_keys_source(1028u, 255u);
    memcpy(payload + length, "zzzzz\n", 6u);
    length += 6u;
    memcpy(payload + length, "hit\n", 4u);
    length += 4u;
    create_file("src.md", payload, length);
    index_attach();
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          capture_run(&terms, &capture_index, "hit"));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_INDEX_LIMIT, status_now().state);

    device_reads = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          capture_run(&terms, &capture_index, "hit"));
    uint32_t index_reads = device_reads;

    ref_attach();
    device_reads = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          capture_run(&terms_ref, &capture_ref, "hit"));
    uint32_t linear_reads = device_reads;
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        linear_reads, index_reads,
        "a same-identity LIMIT query scans once, like the linear baseline");
    assert_rows_equal(&capture_ref, &capture_index);
}

static void index_limit_ordinary_reject_keeps_limit(void)
{
    mount_fs();
    uint32_t length = build_keys_source(1028u, 255u);
    memcpy(payload + length, "zzzzz\n", 6u);
    length += 6u;
    create_file("src.md", payload, length);
    index_attach();
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          capture_run(&terms, &capture_index, "zzzzz"));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_INDEX_LIMIT, status_now().state);

    TEST_ASSERT_EQUAL_INT(YAN_FS_EXISTS,
                          yan_fs_create(&fs, "src.md", (const uint8_t *)"x",
                                        1u));
    TEST_ASSERT_EQUAL_INT(YAN_FS_NOT_FOUND, yan_fs_remove(&fs, "missing"));
    YanSearchTermsStatus status = status_now();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_INDEX_LIMIT, status.state,
                                  "ordinary rejections must keep LIMIT");
    TEST_ASSERT_EQUAL_UINT32(0u, status.terms);

    /* A real source change retries and can reach READY. */
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_remove(&fs, "src.md"));
    create_file("a.md", (const uint8_t *)"hit\n", 4u);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          capture_run(&terms, &capture_index, "hit"));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_INDEX_READY, status_now().state);
}

static int32_t hash_bucket_word[16384];

static uint32_t test_fnv(const uint8_t *key, uint32_t length)
{
    uint32_t hash = UINT32_C(2166136261);
    for (uint32_t i = 0u; i < length; ++i) {
        hash ^= key[i];
        hash *= UINT32_C(16777619);
    }
    return hash;
}

/* "k" + 4 base-26 letters; unique for value < 26^4. Returns the length. */
static uint32_t emit_word(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)'k';
    out[1] = (uint8_t)('a' + ((value / 17576u) % 26u));
    out[2] = (uint8_t)('a' + ((value / 676u) % 26u));
    out[3] = (uint8_t)('a' + ((value / 26u) % 26u));
    out[4] = (uint8_t)('a' + (value % 26u));
    return 5u;
}

static void index_hash_collision_keys_are_raw_compared(void)
{
    for (uint32_t i = 0u; i < 16384u; ++i) {
        hash_bucket_word[i] = -1;
    }
    uint8_t word[8];
    uint32_t first_index = 0u;
    uint32_t second_index = 0u;
    bool found = false;
    for (uint32_t i = 0u; i < 4096u && !found; ++i) {
        uint32_t length = emit_word(word, i);
        uint32_t bucket = test_fnv(word, length) & 16383u;
        if (hash_bucket_word[bucket] >= 0) {
            first_index = (uint32_t)hash_bucket_word[bucket];
            second_index = i;
            found = true;
            break;
        }
        hash_bucket_word[bucket] = (int32_t)i;
    }
    TEST_ASSERT_TRUE_MESSAGE(found, "a 14-bit FNV collision must exist");

    uint8_t first[8];
    uint8_t second[8];
    uint32_t first_length = emit_word(first, first_index);
    uint32_t second_length = emit_word(second, second_index);
    first[first_length] = '\0';
    second[second_length] = '\0';
    TEST_ASSERT_EQUAL_UINT32(test_fnv(first, first_length) & 16383u,
                             test_fnv(second, second_length) & 16383u);

    mount_fs();
    static uint8_t collision_source[64];
    uint32_t at = 0u;
    memcpy(collision_source + at, first, first_length);
    at += first_length;
    collision_source[at++] = (uint8_t)'\n';
    memcpy(collision_source + at, second, second_length);
    at += second_length;
    collision_source[at++] = (uint8_t)'\n';
    create_file("c.md", collision_source, at);
    index_attach();

    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          capture_run(&terms, &capture_index,
                                      (const char *)first));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, capture_index.matches,
                                     "the first colliding key must be found");
    TEST_ASSERT_EQUAL_STRING("c.md", capture_index.name[0]);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          capture_run(&terms, &capture_index,
                                      (const char *)second));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, capture_index.matches,
                                     "the second colliding key must be found");
    TEST_ASSERT_EQUAL_STRING("c.md", capture_index.name[0]);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_INDEX_READY, status_now().state);

    /* A word that is not in the source must not match, even though its hash may
     * share a bucket with an indexed key. */
    static const char *absent = "kzzzz";
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          capture_run(&terms, &capture_index, absent));
    TEST_ASSERT_EQUAL_UINT32(0u, capture_index.matches);
}

static void index_callback_stop_no_extra_io_and_summary_unchanged(void)
{
    mount_fs();
    create_file("a.md", (const uint8_t *)"hit\nhit\nhit\n", 12u);
    index_attach();
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          capture_run(&terms, &capture_index, "hit"));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_INDEX_READY, status_now().state);
    TEST_ASSERT_EQUAL_UINT32(3u, capture_index.matches);

    device_reads = 0u;
    YanSearchTermsSummary summary;
    memset(&summary, 0, sizeof summary);
    summary.total = 123u;
    summary.shown = 4u;
    YanSearchTermsSink sink;
    sink.context = NULL;
    sink.match = stopping_match;
    sink.chunk = NULL;
    YanSearchTermsResult result =
        yan_search_terms(&terms, (const uint8_t *)"hit", 3u, sink, &summary);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_STOPPED, result);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(
        123u, summary.total,
        "a stopped query leaves the caller's summary untouched");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        1u, device_reads,
        "only the first snippet window may be read before the stop");
}

static void index_snippet_fault_propagates(void)
{
    mount_fs();
    create_file("a.md", (const uint8_t *)"hit\n", 4u);
    index_attach();
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          capture_run(&terms, &capture_index, "hit"));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_INDEX_READY, status_now().state);

    device_reads = 0u;
    device_read_fail_at = 1u; /* the snippet window read */
    YanSearchTermsSummary summary;
    memset(&summary, 0, sizeof summary);
    YanSearchTermsSink sink;
    sink.context = &capture_index;
    sink.match = capture_match;
    sink.chunk = NULL;
    YanSearchTermsResult result =
        yan_search_terms(&terms, (const uint8_t *)"hit", 3u, sink, &summary);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_TERMS_IO, result,
                                  "a snippet read fault must propagate");
    device_read_fail_at = 0u;
}

static void index_preflight_fault_propagates(void)
{
    mount_fs();
    create_file("a.md", (const uint8_t *)"hit\n", 4u);
    index_attach();
    device_reads = 0u;
    device_read_fail_at = 1u; /* the build preflight read */
    YanSearchTermsResult result =
        capture_run(&terms, &capture_index, "hit");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_TERMS_IO, result,
                                  "a preflight read fault must propagate");
    device_read_fail_at = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_INDEX_UNAVAILABLE, status_now().state);
}

static void index_top20_total_and_order(void)
{
    mount_fs();
    for (uint32_t i = 0u; i < 25u; ++i) {
        char name[8];
        name[0] = 'f';
        name[1] = (char)('a' + (i / 10u));
        name[2] = (char)('0' + (i % 10u));
        name[3] = '\0';
        create_file(name, (const uint8_t *)"hit\n", 4u);
    }
    index_attach();
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          capture_run(&terms, &capture_index, "hit"));
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(25u, capture_index.summary.total,
                                     "total counts every matching line");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(20u, capture_index.summary.shown,
                                     "shown is capped at 20");
    TEST_ASSERT_EQUAL_UINT32(20u, capture_index.matches);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_MODE_INDEX, capture_index.summary.mode);
}

static void index_top20_last_high_score_wins(void)
{
    mount_fs();
    for (uint32_t i = 0u; i < 20u; ++i) {
        char name[8];
        name[0] = 'f';
        name[1] = (char)('a' + (i / 10u));
        name[2] = (char)('0' + (i % 10u));
        name[3] = '\0';
        create_file(name, (const uint8_t *)"hit\n", 4u);
    }
    create_file("z.md", (const uint8_t *)"hit hit hit\n", 12u);
    index_attach();
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          capture_run(&terms, &capture_index, "hit"));
    TEST_ASSERT_EQUAL_UINT64(21u, capture_index.summary.total);
    TEST_ASSERT_EQUAL_UINT32(20u, capture_index.summary.shown);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(
        "z.md", capture_index.name[0],
        "the highest score must be shown first even when it is in the last slot");
    TEST_ASSERT_EQUAL_UINT16(3u, capture_index.score[0]);
}

static void index_slot_holes_keep_directory_order(void)
{
    mount_fs();
    create_file("a.md", (const uint8_t *)"hit\n", 4u);
    create_file("b.md", (const uint8_t *)"hit\n", 4u);
    create_file("c.md", (const uint8_t *)"hit\n", 4u);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_remove(&fs, "b.md"));
    index_attach();
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          capture_run(&terms, &capture_index, "hit"));
    TEST_ASSERT_EQUAL_UINT32(2u, capture_index.matches);
    TEST_ASSERT_EQUAL_STRING("a.md", capture_index.name[0]);
    TEST_ASSERT_EQUAL_STRING("c.md", capture_index.name[1]);
}

static void index_invalid_files_are_wholly_skipped(void)
{
    static const uint8_t bad_nul[] = {'h','i','t','\n',0};
    static const uint8_t bad_utf8[] = {'h','i','t','\n',0xE4,0xB8};
    mount_fs();
    create_file("nul.md", bad_nul, sizeof bad_nul);
    create_file("utf8.md", bad_utf8, sizeof bad_utf8);
    create_file("good.md", (const uint8_t *)"hit\n", 4u);
    index_attach();
    ref_attach();
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          capture_run(&terms, &capture_index, "hit"));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_MODE_INDEX, capture_index.summary.mode);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2u, capture_index.summary.skipped,
                                    "index must count whole invalid files");
    TEST_ASSERT_EQUAL_UINT32(1u, capture_index.matches);
    TEST_ASSERT_EQUAL_STRING("good.md", capture_index.name[0]);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          capture_run(&terms_ref, &capture_ref, "hit"));
    assert_rows_equal(&capture_ref, &capture_index);
}

static void index_backend_source_busy_keeps_holder(void)
{
    mount_fs();
    create_file("a.md", (const uint8_t *)"hit\n", 4u);
    index_attach();
    YanSearchTermsStatus before = backend_status_holder;
    backend_status_armed = true;
    uint8_t byte;
    uint32_t got;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK,
                          yan_fs_read(&fs, "a.md", 0u, &byte, 1u, &got));
    TEST_ASSERT_TRUE_MESSAGE(backend_status_ran, "FS device probe must execute");
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_BUSY, backend_status_result);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&before, &backend_status_holder,
                                     sizeof before,
                                     "FS BUSY must preserve index status holder");
}

static void index_preflight_protocol_does_not_publish(void)
{
    mount_fs();
    create_file("a.md", (const uint8_t *)"hit\n", 4u);
    index_attach();
    device_reads = 0u;
    device_read_fail_at = 1u;
    device_read_fail_code = YAN_FS_IO_PROTOCOL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_TERMS_PROTOCOL,
                                  capture_run(&terms, &capture_index, "hit"),
                                  "index preflight protocol fault must propagate");
    TEST_ASSERT_EQUAL_UINT32(0u, capture_index.matches);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_INDEX_UNAVAILABLE, status_now().state);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(index_init_rejects_null_and_alias);
    RUN_TEST(index_status_empty_is_zero_io);
    RUN_TEST(index_clear_is_zero_io_and_does_not_recover);
    RUN_TEST(index_rebuild_empty_is_ready);
    RUN_TEST(index_query_rows_match_direct_linear);
    RUN_TEST(index_first_search_publishes_ready);
    RUN_TEST(index_query_summary_mode_is_index);
    RUN_TEST(index_second_search_reuses_cache_without_build_writes);
    RUN_TEST(index_create_invalidates_ready_to_stale);
    RUN_TEST(index_replace_remove_copy_rename_invalidate_ready);
    RUN_TEST(index_remount_and_address_reuse_invalidate_ready);
    RUN_TEST(index_unchanged_operations_keep_ready);
    RUN_TEST(index_unavailable_sources_report_zero_counts);
    RUN_TEST(index_ready_no_result_reads_nothing);
    RUN_TEST(index_capacity_exact_keys_ready);
    RUN_TEST(index_capacity_8193_keys_limit);
    RUN_TEST(index_capacity_key_pool_exact_ready);
    RUN_TEST(index_capacity_key_pool_over_limit);
    RUN_TEST(index_capacity_postings_exact_ready);
    RUN_TEST(index_capacity_postings_over_limit);
    RUN_TEST(index_auto_limit_falls_back_to_scan);
    RUN_TEST(index_long_source_word_limits_but_still_scans);
    RUN_TEST(index_same_identity_limit_does_not_rebuild);
    RUN_TEST(index_limit_ordinary_reject_keeps_limit);
    RUN_TEST(index_hash_collision_keys_are_raw_compared);
    RUN_TEST(index_io_fault_propagates_without_ok);
    RUN_TEST(index_preflight_fault_propagates);
    RUN_TEST(index_snippet_fault_propagates);
    RUN_TEST(index_callback_stop_no_extra_io_and_summary_unchanged);
    RUN_TEST(index_top20_total_and_order);
    RUN_TEST(index_top20_last_high_score_wins);
    RUN_TEST(index_slot_holes_keep_directory_order);
    RUN_TEST(index_reentrant_management_is_busy);
    RUN_TEST(index_invalid_files_are_wholly_skipped);
    RUN_TEST(index_backend_source_busy_keeps_holder);
    RUN_TEST(index_preflight_protocol_does_not_publish);
    return UNITY_END();
}
