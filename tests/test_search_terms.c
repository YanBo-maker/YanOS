#include "search_terms.h"
#include "search_terms_linear.h"
#include "search_text.h"
#include "yanfs.h"
#include "unity.h"

#include <string.h>

/* 0026 term search native tests.
 *
 * Two fixtures share one file on purpose:
 *
 *   the fake backend   needs no filesystem at all, so the facade's validation,
 *                      busy window, sticky reader error and stopped/summary
 *                      contract are asserted without depending on matching.
 *   the linear backend borrows a real mounted YanFS over a caller-owned static
 *                      byte device, so the semantic cases exercise the actual
 *                      list/read path and never a hand-written scan.
 *
 * The linear matcher performs a whole-file preflight, full streaming scan,
 * ranking and bounded snippet reads. Facade tests use the fake backend to
 * isolate lifecycle and error handling from those matching semantics. */

#define SMALL_BLOCKS 16u
#define BIG_BLOCKS 256u
#define CAPTURE_MAX 20

/* ------------------------------------------------------------- byte device */

static uint8_t small_medium[SMALL_BLOCKS][YAN_FS_BLOCK_SIZE];
static uint8_t big_medium[BIG_BLOCKS][YAN_FS_BLOCK_SIZE];
static uint8_t *medium_blocks;
static uint32_t medium_capacity;
static uint32_t device_reads;
static uint32_t device_writes;
static uint32_t device_read_fail_at;
static YanFsIoResult device_read_fail_code;
static YanFsIoResult device_capacity_status;
static YanFs fs;

/* Probe run from inside a device read callback. The filesystem is busy there,
 * so a direct linear-status observation must report TERMS_BUSY and leave the
 * holder untouched instead of collapsing BUSY into UNINITIALIZED. */
static bool device_probe_armed;
static void *device_probe_linear;
static YanSearchTermsResult device_probe_result;
static YanSearchTermsStatus device_probe_status;

static void device_probe_run(void)
{
    if (!device_probe_armed) {
        return;
    }
    device_probe_armed = false;
    device_probe_result =
        yan_search_terms_linear_status(device_probe_linear,
                                       &device_probe_status);
}

static YanFsIoResult device_capacity(void *context, uint64_t *blocks)
{
    (void)context;
    if (device_capacity_status != YAN_FS_IO_OK) {
        return device_capacity_status;
    }
    *blocks = medium_capacity;
    return YAN_FS_IO_OK;
}

static YanFsIoResult device_read(void *context, uint32_t lba,
                                 uint8_t out[4096])
{
    (void)context;
    ++device_reads;
    device_probe_run();
    if (device_read_fail_at != 0u && device_reads == device_read_fail_at) {
        return device_read_fail_code;
    }
    if (lba >= medium_capacity) {
        return YAN_FS_IO_ERROR;
    }
    memcpy(out, medium_blocks + (size_t)lba * YAN_FS_BLOCK_SIZE,
           YAN_FS_BLOCK_SIZE);
    return YAN_FS_IO_OK;
}

static YanFsIoResult device_write(void *context, uint32_t lba,
                                  const uint8_t data[4096])
{
    (void)context;
    ++device_writes;
    if (lba >= medium_capacity) {
        return YAN_FS_IO_ERROR;
    }
    memcpy(medium_blocks + (size_t)lba * YAN_FS_BLOCK_SIZE, data,
           YAN_FS_BLOCK_SIZE);
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

static void fixture_use(uint8_t (*blocks)[YAN_FS_BLOCK_SIZE], uint32_t capacity)
{
    medium_blocks = (uint8_t *)blocks;
    medium_capacity = capacity;
    device_reads = 0u;
    device_writes = 0u;
    device_read_fail_at = 0u;
    device_read_fail_code = YAN_FS_IO_ERROR;
    device_capacity_status = YAN_FS_IO_OK;
    memset(blocks, 0, (size_t)capacity * YAN_FS_BLOCK_SIZE);
    memset(&fs, 0, sizeof fs);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_FS_OK, yan_fs_format_metadata(blocks[0], capacity),
        "the fixture device must hold a valid empty directory");
}

static void fixture_mount(void)
{
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_init(&fs, fs_backend()));
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_mount(&fs));
}

static void fixture_create(const char *name, const uint8_t *bytes,
                           uint32_t length)
{
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK,
                          yan_fs_create(&fs, name, bytes, length));
}

/* ---------------------------------------------------------- capture sink */

typedef struct {
    uint32_t matches;
    YanSearchTermsMatch value[CAPTURE_MAX];
    char names[CAPTURE_MAX][32];
    bool do_read;
    YanSearchTermsResult read_result;
    uint32_t read_length;
    uint8_t snippet[YAN_SEARCH_TERMS_SNIPPET_MAX];
    /* Per-match copies, so a case can compare every emitted snippet. */
    uint32_t snippet_length[CAPTURE_MAX];
    uint8_t snippet_body[CAPTURE_MAX][YAN_SEARCH_TERMS_SNIPPET_MAX];
} Capture;

static Capture capture;
static YanSearchTerms *capture_terms;

static void capture_reset(void)
{
    memset(&capture, 0, sizeof capture);
    capture.read_result = YAN_SEARCH_TERMS_OK;
}

static bool capture_match(void *context, const YanSearchTermsMatch *match)
{
    Capture *self = (Capture *)context;
    uint32_t index = self->matches;
    if (index < CAPTURE_MAX) {
        self->value[index] = *match;
        uint32_t i = 0u;
        while (i < 31u && match->name[i] != '\0') {
            self->names[index][i] = match->name[i];
            ++i;
        }
        self->names[index][i] = '\0';
        self->value[index].name = self->names[index];
    }
    ++self->matches;
    if (self->do_read && capture_terms != NULL) {
        const uint8_t *bytes = NULL;
        uint32_t length = 0u;
        self->read_result = yan_search_terms_read_snippet(
            capture_terms, 0u, (uint32_t)sizeof self->snippet, &bytes, &length);
        self->read_length = length;
        if (self->read_result == YAN_SEARCH_TERMS_OK && bytes != NULL &&
            length <= sizeof self->snippet) {
            memcpy(self->snippet, bytes, length);
            if (index < CAPTURE_MAX) {
                self->snippet_length[index] = length;
                memcpy(self->snippet_body[index], bytes, length);
            }
        }
    }
    return true;
}

static bool capture_chunk(void *context, uint32_t offset, const uint8_t *bytes,
                          uint32_t length)
{
    (void)context;
    (void)offset;
    (void)bytes;
    (void)length;
    return true;
}

/* A match callback that probes the snippet reader's alias guard while the
 * facade is genuinely inside the callback. */
static YanSearchTerms *alias_probe_terms;
static YanSearchTermsResult alias_probe_result;

static bool alias_probe_match(void *context, const YanSearchTermsMatch *match)
{
    (void)context;
    (void)match;
    uint32_t length = 0u;
    alias_probe_result = yan_search_terms_read_snippet(
        alias_probe_terms, 0u, 4u,
        (const uint8_t **)(void *)alias_probe_terms, &length);
    return true;
}

/* A second probe for a holder whose range leaves uintptr_t. */
static YanSearchTerms *wrap_probe_terms;
static YanSearchTermsResult wrap_probe_result;

static bool wrap_probe_match(void *context, const YanSearchTermsMatch *match)
{
    (void)context;
    (void)match;
    uint32_t length = 0u;
    wrap_probe_result = yan_search_terms_read_snippet(
        wrap_probe_terms, 0u, 4u,
        (const uint8_t **)(uintptr_t)(UINTPTR_MAX - 1u), &length);
    return true;
}

/* ------------------------------------------------------------ fake backend */

typedef struct {
    uint32_t query_calls;
    uint32_t read_calls;
    uint32_t status_calls;
    uint32_t rebuild_calls;
    uint32_t clear_calls;
    YanSearchTermsResult query_result;
    YanSearchTermsResult read_result;
    YanSearchTermsResult status_result;
    YanSearchTermsResult rebuild_result;
    YanSearchTermsResult clear_result;
    bool emit_match;
    bool reenter;
    YanSearchTerms *reenter_terms;
    YanSearchTermsResult reenter_query_result;
    YanSearchTermsResult reenter_status_result;
    YanSearchTermsResult reenter_clear_result;
    YanSearchTermsResult reenter_rebuild_result;
    bool manage_reenter;
    YanSearchTerms *manage_terms;
    YanSearchTermsResult manage_reenter_result;
    char name[32];
    uint32_t line_number;
    uint16_t score;
    uint32_t snippet_length;
    bool left_truncated;
    bool right_truncated;
    uint8_t snippet[YAN_SEARCH_TERMS_SNIPPET_MAX];
    YanSearchTermsStatus status_value;
} FakeTerms;

static FakeTerms fake;

static bool noop_match(void *context, const YanSearchTermsMatch *match)
{
    (void)context;
    (void)match;
    return true;
}

static bool stopping_match(void *context, const YanSearchTermsMatch *match)
{
    (void)context;
    (void)match;
    return false;
}

static void fake_reset(void)
{
    memset(&fake, 0, sizeof fake);
    fake.name[0] = 'f';
    fake.name[1] = '\0';
    fake.line_number = 1u;
    fake.query_result = YAN_SEARCH_TERMS_OK;
    fake.read_result = YAN_SEARCH_TERMS_OK;
    fake.status_result = YAN_SEARCH_TERMS_OK;
    fake.rebuild_result = YAN_SEARCH_TERMS_OK;
    fake.clear_result = YAN_SEARCH_TERMS_OK;
    fake.status_value.state = YAN_SEARCH_INDEX_EMPTY;
    fake.status_value.source = YAN_SEARCH_SOURCE_UNMOUNTED;
}

static YanSearchTermsResult fake_query(void *context, const uint8_t *query,
                                       uint32_t query_length,
                                       YanSearchTermsMatchFn match,
                                       void *match_context,
                                       YanSearchTermsSummary *summary)
{
    FakeTerms *self = (FakeTerms *)context;
    (void)query;
    (void)query_length;
    ++self->query_calls;
    if (self->reenter && self->reenter_terms != NULL) {
        uint8_t byte = (uint8_t)'x';
        YanSearchTermsSummary inner_summary;
        YanSearchTermsSink sink;
        sink.context = NULL;
        sink.match = noop_match;
        sink.chunk = NULL;
        self->reenter_query_result = yan_search_terms(
            self->reenter_terms, &byte, 1u, sink, &inner_summary);
        YanSearchTermsStatus status;
        self->reenter_status_result =
            yan_search_terms_status(self->reenter_terms, &status);
        self->reenter_clear_result = yan_search_terms_clear(self->reenter_terms);
        self->reenter_rebuild_result =
            yan_search_terms_rebuild(self->reenter_terms);
    }
    if (self->emit_match) {
        YanSearchTermsMatch value;
        value.name = self->name;
        value.line_number = self->line_number;
        value.score = self->score;
        value.snippet_length = self->snippet_length;
        value.left_truncated = self->left_truncated;
        value.right_truncated = self->right_truncated;
        (void)match(match_context, &value);
    }
    if (summary != NULL) {
        summary->total = 5u;
        summary->shown = 2u;
        summary->skipped = 1u;
        summary->mode = YAN_SEARCH_MODE_INDEX;
    }
    return self->query_result;
}

static YanSearchTermsResult fake_read(void *context, uint32_t offset,
                                      uint32_t capacity,
                                      const uint8_t **bytes, uint32_t *length)
{
    FakeTerms *self = (FakeTerms *)context;
    (void)offset;
    ++self->read_calls;
    if (self->read_result != YAN_SEARCH_TERMS_OK) {
        return self->read_result;
    }
    uint32_t got = self->snippet_length;
    if (got > capacity) {
        got = capacity;
    }
    *bytes = self->snippet;
    *length = got;
    return YAN_SEARCH_TERMS_OK;
}

static YanSearchTermsResult fake_status(void *context,
                                        YanSearchTermsStatus *out)
{
    FakeTerms *self = (FakeTerms *)context;
    ++self->status_calls;
    if (out != NULL) {
        *out = self->status_value;
    }
    if (self->manage_reenter) {
        YanSearchTermsStatus nested;
        self->manage_reenter_result =
            yan_search_terms_status(self->manage_terms, &nested);
    }
    return self->status_result;
}

static YanSearchTermsResult fake_rebuild(void *context)
{
    FakeTerms *self = (FakeTerms *)context;
    ++self->rebuild_calls;
    if (self->manage_reenter) {
        YanSearchTermsStatus nested;
        self->manage_reenter_result =
            yan_search_terms_status(self->manage_terms, &nested);
    }
    return self->rebuild_result;
}

static YanSearchTermsResult fake_clear(void *context)
{
    FakeTerms *self = (FakeTerms *)context;
    ++self->clear_calls;
    if (self->manage_reenter) {
        YanSearchTermsStatus nested;
        self->manage_reenter_result =
            yan_search_terms_status(self->manage_terms, &nested);
    }
    return self->clear_result;
}

static YanSearchTermsBackend fake_backend(void)
{
    YanSearchTermsBackend backend;
    backend.context = &fake;
    backend.context_size = sizeof fake;
    backend.source_context = NULL;
    backend.source_size = 0u;
    backend.query = fake_query;
    backend.read_match = fake_read;
    backend.status = fake_status;
    backend.rebuild = fake_rebuild;
    backend.clear = fake_clear;
    return backend;
}

/* ------------------------------------------------------------- lifecycle */

void setUp(void)
{
    medium_blocks = (uint8_t *)small_medium;
    medium_capacity = SMALL_BLOCKS;
    device_reads = 0u;
    device_writes = 0u;
    device_read_fail_at = 0u;
    device_read_fail_code = YAN_FS_IO_ERROR;
    device_capacity_status = YAN_FS_IO_OK;
    device_probe_armed = false;
    memset(small_medium, 0, sizeof small_medium);
    memset(big_medium, 0, sizeof big_medium);
    memset(&fs, 0, sizeof fs);
    memset(&capture, 0, sizeof capture);
    memset(&fake, 0, sizeof fake);
    capture_reset();
    fake_reset();
    capture_terms = NULL;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK,
                          yan_fs_format_metadata(small_medium[0], SMALL_BLOCKS));
}

void tearDown(void)
{
}

/* --------------------------------------------------------- pure text helpers */

static void text_decodes_ascii_and_multibyte_scalars(void)
{
    static const uint8_t ascii[] = {'A'};
    static const uint8_t two[] = {0xc3u, 0xa9u};             /* é */
    static const uint8_t three[] = {0xe4u, 0xb8u, 0xadu};    /* 中 */
    static const uint8_t four[] = {0xf0u, 0x9fu, 0x98u, 0x80u}; /* U+1F600 */
    uint32_t code = 0u;
    TEST_ASSERT_EQUAL_UINT32(1u, yan_search_text_utf8_scalar(ascii, 1u, &code));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)'A', code);
    TEST_ASSERT_EQUAL_UINT32(2u, yan_search_text_utf8_scalar(two, 2u, &code));
    TEST_ASSERT_EQUAL_UINT32(UINT32_C(0xe9), code);
    TEST_ASSERT_EQUAL_UINT32(3u, yan_search_text_utf8_scalar(three, 3u, &code));
    TEST_ASSERT_EQUAL_UINT32(UINT32_C(0x4e2d), code);
    TEST_ASSERT_EQUAL_UINT32(4u, yan_search_text_utf8_scalar(four, 4u, &code));
    TEST_ASSERT_EQUAL_UINT32(UINT32_C(0x1f600), code);
}

static void text_rejects_invalid_and_truncated_sequences(void)
{
    static const uint8_t overlong2[] = {0xc0u, 0xafu};
    static const uint8_t surrogate[] = {0xedu, 0xa0u, 0x80u};
    static const uint8_t too_high[] = {0xf4u, 0x90u, 0x80u, 0x80u};
    static const uint8_t truncated[] = {0xe4u, 0xb8u};
    static const uint8_t lone_continuation[] = {0x80u};
    static const uint8_t null_pointer[] = {0x41u};
    uint32_t code = 0u;
    TEST_ASSERT_EQUAL_UINT32(0u, yan_search_text_utf8_scalar(overlong2, 2u, &code));
    TEST_ASSERT_EQUAL_UINT32(0u, yan_search_text_utf8_scalar(surrogate, 3u, &code));
    TEST_ASSERT_EQUAL_UINT32(0u, yan_search_text_utf8_scalar(too_high, 4u, &code));
    TEST_ASSERT_EQUAL_UINT32(0u, yan_search_text_utf8_scalar(truncated, 2u, &code));
    TEST_ASSERT_EQUAL_UINT32(0u,
                             yan_search_text_utf8_scalar(lone_continuation, 1u,
                                                         &code));
    TEST_ASSERT_EQUAL_UINT32(0u, yan_search_text_utf8_scalar(NULL, 1u, &code));
    TEST_ASSERT_EQUAL_UINT32(0u,
                             yan_search_text_utf8_scalar(null_pointer, 0u, &code));
}

static void text_validates_whole_buffers(void)
{
    static const uint8_t good[] = {0x61u, 0xe4u, 0xb8u, 0xadu, 0x0au};
    static const uint8_t bad[] = {0x61u, 0xffu, 0x0au};
    TEST_ASSERT_TRUE(yan_search_text_utf8_valid(good, sizeof good));
    TEST_ASSERT_TRUE(yan_search_text_utf8_valid(NULL, 0u));
    TEST_ASSERT_FALSE(yan_search_text_utf8_valid(bad, sizeof bad));
    TEST_ASSERT_FALSE(yan_search_text_utf8_valid(NULL, 1u));
}

static void text_ascii_word_and_fold(void)
{
    TEST_ASSERT_TRUE(yan_search_text_is_ascii_word((uint8_t)'a'));
    TEST_ASSERT_TRUE(yan_search_text_is_ascii_word((uint8_t)'Z'));
    TEST_ASSERT_TRUE(yan_search_text_is_ascii_word((uint8_t)'0'));
    TEST_ASSERT_TRUE(yan_search_text_is_ascii_word((uint8_t)'_'));
    TEST_ASSERT_FALSE(yan_search_text_is_ascii_word((uint8_t)'-'));
    TEST_ASSERT_FALSE(yan_search_text_is_ascii_word((uint8_t)0xe4u));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)'a',
                            yan_search_text_ascii_fold((uint8_t)'A'));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)'-',
                            yan_search_text_ascii_fold((uint8_t)'-'));
}

/* ----------------------------------------------------------- facade guards */

static void terms_init_rejects_invalid_backends_and_aliases(void)
{
    YanSearchTerms facade;
    memset(&facade, 0, sizeof facade);
    YanSearchTermsBackend backend = fake_backend();

    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_INVALID,
                          yan_search_terms_init(NULL, backend));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          yan_search_terms_init(&facade, backend));

    YanSearchTermsBackend incomplete = fake_backend();
    incomplete.query = NULL;
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_INVALID,
                          yan_search_terms_init(&facade, incomplete));
    incomplete = fake_backend();
    incomplete.read_match = NULL;
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_INVALID,
                          yan_search_terms_init(&facade, incomplete));

    YanSearchTermsBackend bad_span = fake_backend();
    bad_span.source_context = NULL;
    bad_span.source_size = 16u;
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_INVALID,
                          yan_search_terms_init(&facade, bad_span));
    bad_span = fake_backend();
    bad_span.source_context = (void *)(uintptr_t)(UINTPTR_MAX - 2u);
    bad_span.source_size = 8u;
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_INVALID,
                          yan_search_terms_init(&facade, bad_span));

    /* The facade may not sit inside either protected span. */
    YanSearchTermsBackend self_span = fake_backend();
    self_span.context = &facade;
    self_span.context_size = sizeof facade;
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_INVALID,
                          yan_search_terms_init(&facade, self_span));

    /* The two spans may not overlap each other. */
    YanSearchTermsBackend pair = fake_backend();
    pair.source_context = &fake;
    pair.source_size = sizeof fake;
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_INVALID,
                          yan_search_terms_init(&facade, pair));
}

static void terms_init_accepts_a_good_backend_and_can_reinit(void)
{
    YanSearchTerms facade;
    memset(&facade, 0, sizeof facade);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          yan_search_terms_init(&facade, fake_backend()));
    TEST_ASSERT_TRUE(facade.initialized);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          yan_search_terms_init(&facade, fake_backend()));
}

static void terms_query_rejects_invalid_arguments(void)
{
    YanSearchTerms facade;
    memset(&facade, 0, sizeof facade);
    uint8_t query = (uint8_t)'x';
    YanSearchTermsSummary summary;
    YanSearchTermsSink sink;
    sink.context = &capture;
    sink.match = capture_match;
    sink.chunk = NULL;

    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_INVALID,
                          yan_search_terms(&facade, &query, 1u, sink, &summary));
    TEST_ASSERT_EQUAL_INT(
        YAN_SEARCH_TERMS_OK,
        yan_search_terms_init(&facade, fake_backend()));

    TEST_ASSERT_EQUAL_INT(
        YAN_SEARCH_TERMS_INVALID,
        yan_search_terms(&facade, NULL, 1u, sink, &summary));
    TEST_ASSERT_EQUAL_INT(
        YAN_SEARCH_TERMS_INVALID,
        yan_search_terms(&facade, &query, 0u, sink, &summary));
    TEST_ASSERT_EQUAL_INT(
        YAN_SEARCH_TERMS_INVALID,
        yan_search_terms(&facade, &query,
                         YAN_SEARCH_TERMS_QUERY_MAX + 1u, sink, &summary));

    /* A query aliasing the facade or the backend context is INVALID. */
    TEST_ASSERT_EQUAL_INT(
        YAN_SEARCH_TERMS_INVALID,
        yan_search_terms(&facade, (const uint8_t *)&facade, 1u, sink,
                         &summary));
    TEST_ASSERT_EQUAL_INT(
        YAN_SEARCH_TERMS_INVALID,
        yan_search_terms(&facade, (const uint8_t *)&fake, 1u, sink, &summary));
    /* A summary holder aliasing the facade is INVALID. */
    TEST_ASSERT_EQUAL_INT(
        YAN_SEARCH_TERMS_INVALID,
        yan_search_terms(&facade, &query, 1u, sink,
                         (YanSearchTermsSummary *)(void *)&facade));
    /* A NULL match callback is required. */
    sink.match = NULL;
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_INVALID,
                          yan_search_terms(&facade, &query, 1u, sink, &summary));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, fake.query_calls,
        "an invalid query must never reach the backend");
}

static void terms_query_relays_backend_matches_and_summary(void)
{
    YanSearchTerms facade;
    memset(&facade, 0, sizeof facade);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          yan_search_terms_init(&facade, fake_backend()));
    capture_terms = &facade;
    fake.emit_match = true;
    fake.line_number = 7u;
    fake.score = 3u;
    fake.snippet_length = 4u;
    fake.left_truncated = true;
    fake.right_truncated = false;
    memcpy(fake.snippet, "abcd", 4u);

    uint8_t query = (uint8_t)'x';
    YanSearchTermsSummary summary;
    YanSearchTermsSink sink;
    sink.context = &capture;
    sink.match = capture_match;
    sink.chunk = NULL;
    TEST_ASSERT_EQUAL_INT(
        YAN_SEARCH_TERMS_OK,
        yan_search_terms(&facade, &query, 1u, sink, &summary));
    TEST_ASSERT_EQUAL_UINT32(1u, fake.query_calls);
    TEST_ASSERT_EQUAL_UINT32(1u, capture.matches);
    TEST_ASSERT_EQUAL_UINT32(7u, capture.value[0].line_number);
    TEST_ASSERT_EQUAL_UINT16(3u, capture.value[0].score);
    TEST_ASSERT_EQUAL_UINT32(4u, capture.value[0].snippet_length);
    TEST_ASSERT_TRUE(capture.value[0].left_truncated);
    TEST_ASSERT_FALSE(capture.value[0].right_truncated);
    TEST_ASSERT_EQUAL_STRING("f", capture.names[0]);
    TEST_ASSERT_EQUAL_UINT64(5u, summary.total);
    TEST_ASSERT_EQUAL_UINT32(2u, summary.shown);
    TEST_ASSERT_EQUAL_UINT32(1u, summary.skipped);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_MODE_INDEX, summary.mode);
}

static void terms_query_maps_unknown_backend_result_to_protocol(void)
{
    YanSearchTerms facade;
    memset(&facade, 0, sizeof facade);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          yan_search_terms_init(&facade, fake_backend()));
    fake.query_result = (YanSearchTermsResult)200;
    uint8_t query = (uint8_t)'x';
    YanSearchTermsSummary summary;
    YanSearchTermsSink sink;
    sink.context = &capture;
    sink.match = capture_match;
    sink.chunk = NULL;
    TEST_ASSERT_EQUAL_INT(
        YAN_SEARCH_TERMS_PROTOCOL,
        yan_search_terms(&facade, &query, 1u, sink, &summary));
}

static void terms_query_reports_stopped_when_a_sink_stops(void)
{
    YanSearchTerms facade;
    memset(&facade, 0, sizeof facade);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          yan_search_terms_init(&facade, fake_backend()));
    fake.emit_match = true;
    fake.snippet_length = 0u;

    uint8_t query = (uint8_t)'x';
    YanSearchTermsSummary summary;
    summary.total = 123u;
    summary.shown = 4u;
    YanSearchTermsSink sink;
    sink.context = NULL;
    sink.match = stopping_match;
    sink.chunk = NULL;
    YanSearchTermsResult result =
        yan_search_terms(&facade, &query, 1u, sink, &summary);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_TERMS_STOPPED, result,
                                  "a clean callback stop must report STOPPED");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(
        123u, summary.total,
        "a cancelled query must leave the caller's summary untouched");
    TEST_ASSERT_EQUAL_UINT32(4u, summary.shown);
}

static void terms_management_is_busy_inside_a_query(void)
{
    YanSearchTerms facade;
    memset(&facade, 0, sizeof facade);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          yan_search_terms_init(&facade, fake_backend()));
    fake.reenter = true;
    fake.reenter_terms = &facade;
    capture_terms = &facade;

    uint8_t query = (uint8_t)'x';
    YanSearchTermsSummary summary;
    YanSearchTermsSink sink;
    sink.context = &capture;
    sink.match = capture_match;
    sink.chunk = NULL;
    TEST_ASSERT_EQUAL_INT(
        YAN_SEARCH_TERMS_OK,
        yan_search_terms(&facade, &query, 1u, sink, &summary));
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_TERMS_BUSY,
                                  fake.reenter_query_result,
                                  "a reentrant query must answer BUSY");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_TERMS_BUSY,
                                  fake.reenter_status_result,
                                  "status inside a query must answer BUSY");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_TERMS_BUSY,
                                  fake.reenter_clear_result,
                                  "clear inside a query must answer BUSY");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_TERMS_BUSY,
                                  fake.reenter_rebuild_result,
                                  "rebuild inside a query must answer BUSY");
}

static void terms_read_snippet_outside_a_callback_is_invalid(void)
{
    YanSearchTerms facade;
    memset(&facade, 0, sizeof facade);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          yan_search_terms_init(&facade, fake_backend()));
    const uint8_t *bytes = NULL;
    uint32_t length = 0u;
    TEST_ASSERT_EQUAL_INT(
        YAN_SEARCH_TERMS_INVALID,
        yan_search_terms_read_snippet(&facade, 0u, 4u, &bytes, &length));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, fake.read_calls,
        "a reader outside the callback must not reach the backend");
}

static void terms_read_snippet_reads_inside_the_callback(void)
{
    YanSearchTerms facade;
    memset(&facade, 0, sizeof facade);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          yan_search_terms_init(&facade, fake_backend()));
    capture_terms = &facade;
    capture.do_read = true;
    fake.emit_match = true;
    fake.snippet_length = 4u;
    fake.read_result = YAN_SEARCH_TERMS_OK;
    memcpy(fake.snippet, "abcd", 4u);

    uint8_t query = (uint8_t)'x';
    YanSearchTermsSummary summary;
    YanSearchTermsSink sink;
    sink.context = &capture;
    sink.match = capture_match;
    sink.chunk = NULL;
    TEST_ASSERT_EQUAL_INT(
        YAN_SEARCH_TERMS_OK,
        yan_search_terms(&facade, &query, 1u, sink, &summary));
    TEST_ASSERT_EQUAL_UINT32(1u, fake.read_calls);
    TEST_ASSERT_EQUAL_UINT32(4u, capture.read_length);
    TEST_ASSERT_EQUAL_MEMORY("abcd", capture.snippet, 4u);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK, capture.read_result);
}

static void terms_read_snippet_rejects_aliasing_holders(void)
{
    YanSearchTerms facade;
    memset(&facade, 0, sizeof facade);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          yan_search_terms_init(&facade, fake_backend()));
    alias_probe_terms = &facade;
    alias_probe_result = YAN_SEARCH_TERMS_OK;
    fake.emit_match = true;
    fake.snippet_length = 4u;

    uint8_t query = (uint8_t)'x';
    YanSearchTermsSummary summary;
    YanSearchTermsSink sink;
    sink.context = NULL;
    sink.match = alias_probe_match;
    sink.chunk = NULL;
    TEST_ASSERT_EQUAL_INT(
        YAN_SEARCH_TERMS_OK,
        yan_search_terms(&facade, &query, 1u, sink, &summary));
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_TERMS_INVALID, alias_probe_result,
        "a snippet holder aliasing the facade must be rejected");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, fake.read_calls,
        "a rejected holder must not reach the backend reader");
}

static void terms_read_snippet_sticky_error_beats_a_stop(void)
{
    YanSearchTerms facade;
    memset(&facade, 0, sizeof facade);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          yan_search_terms_init(&facade, fake_backend()));
    capture_terms = &facade;
    capture.do_read = true;
    fake.emit_match = true;
    fake.snippet_length = 4u;
    fake.read_result = YAN_SEARCH_TERMS_IO;

    uint8_t query = (uint8_t)'x';
    YanSearchTermsSummary summary;
    YanSearchTermsSink sink;
    sink.context = &capture;
    sink.match = capture_match;
    sink.chunk = capture_chunk;
    YanSearchTermsResult result =
        yan_search_terms(&facade, &query, 1u, sink, &summary);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_TERMS_IO, result,
                                  "a latched reader error must win");
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_IO, capture.read_result);
}

static void terms_status_and_management_dispatch(void)
{
    YanSearchTerms facade;
    memset(&facade, 0, sizeof facade);
    YanSearchTermsBackend backend = fake_backend();
    backend.status = NULL;
    backend.rebuild = NULL;
    backend.clear = NULL;
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          yan_search_terms_init(&facade, backend));
    YanSearchTermsStatus status;
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_UNSUPPORTED,
                          yan_search_terms_status(&facade, &status));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_UNSUPPORTED,
                          yan_search_terms_clear(&facade));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_UNSUPPORTED,
                          yan_search_terms_rebuild(&facade));

    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          yan_search_terms_init(&facade, fake_backend()));
    fake.status_value.state = YAN_SEARCH_INDEX_READY;
    fake.status_value.source = YAN_SEARCH_SOURCE_MOUNTED;
    fake.status_value.terms = 42u;
    fake.status_value.postings = 170u;
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          yan_search_terms_status(&facade, &status));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_INDEX_READY, status.state);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_SOURCE_MOUNTED, status.source);
    TEST_ASSERT_EQUAL_UINT32(42u, status.terms);
    TEST_ASSERT_EQUAL_UINT32(170u, status.postings);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK, yan_search_terms_clear(&facade));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          yan_search_terms_rebuild(&facade));
    TEST_ASSERT_EQUAL_UINT32(1u, fake.status_calls);
    TEST_ASSERT_EQUAL_UINT32(1u, fake.clear_calls);
    TEST_ASSERT_EQUAL_UINT32(1u, fake.rebuild_calls);

    /* A status holder aliasing the facade is INVALID. */
    TEST_ASSERT_EQUAL_INT(
        YAN_SEARCH_TERMS_INVALID,
        yan_search_terms_status(&facade, (YanSearchTermsStatus *)(void *)&facade));
}

/* ------------------------------------------------ linear backend (real FS) */

static YanSearchTermsLinear linear;
static YanSearchTerms terms;

static void setup_linear_terms(void)
{
    fixture_mount();
    memset(&linear, 0, sizeof linear);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          yan_search_terms_linear_init(&linear, &fs));
    memset(&terms, 0, sizeof terms);
    TEST_ASSERT_EQUAL_INT(
        YAN_SEARCH_TERMS_OK,
        yan_search_terms_init(&terms, yan_search_terms_linear_backend(&linear)));
    capture_terms = &terms;
}

static void linear_init_rejects_invalid_and_overlapping_pairs(void)
{
    YanSearchTermsLinear context;
    memset(&context, 0, sizeof context);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_INVALID,
                          yan_search_terms_linear_init(NULL, &fs));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_INVALID,
                          yan_search_terms_linear_init(&context, NULL));
    /* The context placed over the filesystem overlaps its own source span. */
    TEST_ASSERT_EQUAL_INT(
        YAN_SEARCH_TERMS_INVALID,
        yan_search_terms_linear_init((YanSearchTermsLinear *)(void *)&fs, &fs));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          yan_search_terms_linear_init(&context, &fs));
}

static void linear_status_reports_empty_and_the_real_source(void)
{
    fixture_use(small_medium, SMALL_BLOCKS);
    fixture_mount();
    memset(&linear, 0, sizeof linear);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          yan_search_terms_linear_init(&linear, &fs));
    memset(&terms, 0, sizeof terms);
    TEST_ASSERT_EQUAL_INT(
        YAN_SEARCH_TERMS_OK,
        yan_search_terms_init(&terms, yan_search_terms_linear_backend(&linear)));

    YanSearchTermsStatus status;
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          yan_search_terms_status(&terms, &status));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_INDEX_EMPTY, status.state);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_SOURCE_MOUNTED, status.source);
    TEST_ASSERT_EQUAL_UINT32(0u, status.terms);
    TEST_ASSERT_EQUAL_UINT32(0u, status.postings);

    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_unmount(&fs));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          yan_search_terms_status(&terms, &status));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_SOURCE_UNMOUNTED, status.source);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_INDEX_UNAVAILABLE, status.state,
                                  "an unmounted source has no usable index");

    /* A real fault is reported, not hidden. */
    device_capacity_status = YAN_FS_IO_ERROR;
    TEST_ASSERT_EQUAL_INT(YAN_FS_IO, yan_fs_mount(&fs));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          yan_search_terms_status(&terms, &status));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_SOURCE_FAULTED, status.source);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_INDEX_UNAVAILABLE, status.state);
    device_capacity_status = YAN_FS_IO_OK;

    /* An uninitialized filesystem is UNINITIALIZED, never guessed. */
    YanFs virgin;
    memset(&virgin, 0, sizeof virgin);
    YanSearchTermsLinear virgin_linear;
    memset(&virgin_linear, 0, sizeof virgin_linear);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          yan_search_terms_linear_init(&virgin_linear, &virgin));
    memset(&terms, 0, sizeof terms);
    TEST_ASSERT_EQUAL_INT(
        YAN_SEARCH_TERMS_OK,
        yan_search_terms_init(&terms,
                              yan_search_terms_linear_backend(&virgin_linear)));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          yan_search_terms_status(&terms, &status));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_SOURCE_UNINITIALIZED, status.source);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_INDEX_UNAVAILABLE, status.state);
}

static void linear_clear_is_a_noop_and_rebuild_is_unsupported(void)
{
    fixture_use(small_medium, SMALL_BLOCKS);
    fixture_mount();
    memset(&linear, 0, sizeof linear);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          yan_search_terms_linear_init(&linear, &fs));
    memset(&terms, 0, sizeof terms);
    TEST_ASSERT_EQUAL_INT(
        YAN_SEARCH_TERMS_OK,
        yan_search_terms_init(&terms, yan_search_terms_linear_backend(&linear)));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK, yan_search_terms_clear(&terms));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_UNSUPPORTED,
                          yan_search_terms_rebuild(&terms));
}

/* ------------------------------------------------------- semantic contract */

static YanSearchTermsSummary run_query_common(const char *query,
                                               bool read_snippets)
{
    YanSearchTermsSummary summary;
    memset(&summary, 0, sizeof summary);
    capture_reset();
    capture.do_read = read_snippets;
    capture_terms = &terms;
    YanSearchTermsSink sink;
    sink.context = &capture;
    sink.match = capture_match;
    sink.chunk = NULL;
    YanSearchTermsResult result = yan_search_terms(
        &terms, (const uint8_t *)query, (uint32_t)strlen(query), sink, &summary);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_TERMS_OK, result,
                                  "the term query must complete");
    return summary;
}

static YanSearchTermsSummary run_ok_query(const char *query)
{
    return run_query_common(query, false);
}

static YanSearchTermsSummary run_ok_query_snippets(const char *query)
{
    return run_query_common(query, true);
}

static void semantic_ascii_whole_word_and_case(void)
{
    fixture_use(small_medium, SMALL_BLOCKS);
    const char *content = "interruption pending\nInterrupt here\n";
    setup_linear_terms();
    fixture_create("note.md", (const uint8_t *)content,
                   (uint32_t)strlen(content));
    run_ok_query("interrupt");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, capture.matches,
                                     "interrupt must not match interruption");
    TEST_ASSERT_EQUAL_UINT32(2u, capture.value[0].line_number);
}

static void semantic_ascii_word_chars_digits_and_underscore(void)
{
    fixture_use(small_medium, SMALL_BLOCKS);
    const char *content = "interrupt_2 x\ninterrupt-2 y\n";
    setup_linear_terms();
    fixture_create("note.md", (const uint8_t *)content,
                   (uint32_t)strlen(content));
    run_ok_query("interrupt_2");
    TEST_ASSERT_EQUAL_UINT32(1u, capture.matches);
    TEST_ASSERT_EQUAL_UINT32(1u, capture.value[0].line_number);
}

static void semantic_chinese_scalars_must_be_contiguous(void)
{
    fixture_use(small_medium, SMALL_BLOCKS);
    const char *content =
        "\xe4\xb8\xad x \xe6\x96\xad\n"          /* 中 x 断 */
        "\xe5\xa4\x84\xe7\x90\x86\xe4\xb8\xad\xe6\x96\xad\n"; /* 处理中断 */
    setup_linear_terms();
    fixture_create("note.md", (const uint8_t *)content,
                   (uint32_t)strlen(content));
    run_ok_query("\xe4\xb8\xad\xe6\x96\xad"); /* 中断 */
    TEST_ASSERT_EQUAL_UINT32(1u, capture.matches);
    TEST_ASSERT_EQUAL_UINT32(2u, capture.value[0].line_number);
}

static void semantic_mixed_group_ascii_then_scalars(void)
{
    fixture_use(small_medium, SMALL_BLOCKS);
    const char *content =
        "CPU\xe5\xa4\x84\xe7\x90\x86\xe4\xb8\xad\xe6\x96\xad\n"
        "CPU \xe6\xad\xa3\xe5\x9c\xa8\xe5\xa4\x84\xe7\x90\x86\xe4\xb8\xad\xe6\x96\xad\n";
    setup_linear_terms();
    fixture_create("note.md", (const uint8_t *)content,
                   (uint32_t)strlen(content));
    run_ok_query("CPU\xe5\xa4\x84\xe7\x90\x86\xe4\xb8\xad\xe6\x96\xad");
    TEST_ASSERT_EQUAL_UINT32(1u, capture.matches);
    TEST_ASSERT_EQUAL_UINT32(1u, capture.value[0].line_number);
}

static void semantic_groups_require_the_same_line(void)
{
    fixture_use(small_medium, SMALL_BLOCKS);
    const char *content = "CPU alone\ninterrupt alone\nCPU and interrupt\n";
    setup_linear_terms();
    fixture_create("note.md", (const uint8_t *)content,
                   (uint32_t)strlen(content));
    run_ok_query("CPU interrupt");
    TEST_ASSERT_EQUAL_UINT32(1u, capture.matches);
    TEST_ASSERT_EQUAL_UINT32(3u, capture.value[0].line_number);
}

static void semantic_duplicate_groups_do_not_multiply_the_score(void)
{
    fixture_use(small_medium, SMALL_BLOCKS);
    const char *content = "CPU cpu CPU\n";
    setup_linear_terms();
    fixture_create("note.md", (const uint8_t *)content,
                   (uint32_t)strlen(content));
    run_ok_query("CPU cpu");
    TEST_ASSERT_EQUAL_UINT32(1u, capture.matches);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(
        3u, capture.value[0].score,
        "one canonical group found three times must score 3, not 6");
}

static void semantic_overlapping_chinese_counts_each_start(void)
{
    fixture_use(small_medium, SMALL_BLOCKS);
    const char *content = "\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad\n";
    setup_linear_terms();
    fixture_create("note.md", (const uint8_t *)content,
                   (uint32_t)strlen(content));
    run_ok_query("\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad"); /* 中中中 */
    TEST_ASSERT_EQUAL_UINT32(1u, capture.matches);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(2u, capture.value[0].score,
        "overlapping matches must count each start");
}

static void semantic_ascii_aa_does_not_match_inside_aaaa(void)
{
    fixture_use(small_medium, SMALL_BLOCKS);
    const char *content = "aaaa\n";
    setup_linear_terms();
    fixture_create("note.md", (const uint8_t *)content,
                   (uint32_t)strlen(content));
    YanSearchTermsSummary summary = run_ok_query("aa");
    TEST_ASSERT_EQUAL_UINT32(0u, capture.matches);
    TEST_ASSERT_EQUAL_UINT64(0u, summary.total);
}

static void semantic_nul_file_is_skipped_once(void)
{
    fixture_use(small_medium, SMALL_BLOCKS);
    static const uint8_t bad[] = {'o', 'k', '\n', 0u, 'x', '\n'};
    static const uint8_t good[] = {'x', '\n'};
    setup_linear_terms();
    fixture_create("bad.md", bad, sizeof bad);
    fixture_create("good.md", good, sizeof good);
    YanSearchTermsSummary summary = run_ok_query("ok");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, summary.skipped,
        "a NUL file must be skipped once");
    TEST_ASSERT_EQUAL_UINT32(0u, capture.matches);
}

static void semantic_line_endings_and_missing_final_newline(void)
{
    fixture_use(small_medium, SMALL_BLOCKS);
    /* LF and CRLF end lines; a bare CR does not. The logical lines are "a",
     * "b\rc" and "last", so "last" is line 3. */
    const char *content = "a\r\nb\rc\nlast";
    setup_linear_terms();
    fixture_create("note.md", (const uint8_t *)content,
                   (uint32_t)strlen(content));
    run_ok_query("last");
    TEST_ASSERT_EQUAL_UINT32(1u, capture.matches);
    TEST_ASSERT_EQUAL_UINT32(3u, capture.value[0].line_number);
}

/* SPEC0026 line endings are LF and CRLF only. A bare CR is a word separator
 * that stays in the raw snippet; the CR of a CRLF belongs to the terminator and
 * is excluded. Owner case: bytes "hit\rnext hit\r\nhit\r", query "hit". */
static void semantic_bare_cr_is_content_and_only_lf_ends_a_line(void)
{
    fixture_use(small_medium, SMALL_BLOCKS);
    static const uint8_t content[] = {
        'h', 'i', 't', '\r', 'n', 'e', 'x', 't', ' ', 'h', 'i', 't',
        '\r', '\n', 'h', 'i', 't', '\r'
    };
    setup_linear_terms();
    fixture_create("a.md", content, sizeof content);
    YanSearchTermsSummary summary = run_ok_query_snippets("hit");
    TEST_ASSERT_EQUAL_UINT64(2u, summary.total);
    TEST_ASSERT_EQUAL_UINT32(2u, capture.matches);

    TEST_ASSERT_EQUAL_UINT32(1u, capture.value[0].line_number);
    TEST_ASSERT_EQUAL_UINT16(2u, capture.value[0].score);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(12u, capture.snippet_length[0],
        "a bare CR stays in the raw snippet");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(
        "hit\rnext hit", capture.snippet_body[0], 12u,
        "a bare CR stays in the raw snippet");

    TEST_ASSERT_EQUAL_UINT32(2u, capture.value[1].line_number);
    TEST_ASSERT_EQUAL_UINT16(1u, capture.value[1].score);
    TEST_ASSERT_EQUAL_UINT32(4u, capture.snippet_length[1]);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(
        "hit\r", capture.snippet_body[1], 4u,
        "a bare CR at end of file stays in the raw snippet");
}

static void semantic_query_group_matches_through_a_bare_cr(void)
{
    fixture_use(small_medium, SMALL_BLOCKS);
    static const uint8_t content[] = {
        'h', 'i', 't', '\r', 'n', 'e', 'x', 't', ' ', 'h', 'i', 't',
        '\r', '\n'
    };
    setup_linear_terms();
    fixture_create("a.md", content, sizeof content);
    /* The bare CR is a separator, so both groups match the same LF-line. */
    run_ok_query("hit next");
    TEST_ASSERT_EQUAL_UINT32(1u, capture.matches);
    TEST_ASSERT_EQUAL_UINT32(1u, capture.value[0].line_number);
    TEST_ASSERT_EQUAL_UINT16(3u, capture.value[0].score);
}

static void semantic_top20_total_and_score_order(void)
{
    fixture_use(small_medium, SMALL_BLOCKS);
    static char content[128];
    uint32_t offset = 0u;
    /* 25 equally scored matching lines: total counts all of them, shown stops
     * at 20. */
    for (uint32_t i = 0u; i < 25u; ++i) {
        content[offset++] = 'a';
        content[offset++] = '\n';
    }
    setup_linear_terms();
    fixture_create("note.md", (const uint8_t *)content, offset);
    YanSearchTermsSummary summary = run_ok_query("a");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(25u, summary.total,
                                     "total counts every matching line");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(20u, summary.shown,
                                     "shown is capped at 20");
    TEST_ASSERT_EQUAL_UINT32(20u, capture.matches);
    for (uint32_t i = 0u; i < 20u; ++i) {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(i + 1u, capture.value[i].line_number,
            "equal-score rows must retain ascending line order");
    }
    fixture_create("later.md", (const uint8_t *)"a a a\n", 6u);
    summary = run_ok_query("a");
    TEST_ASSERT_EQUAL_UINT64(26u, summary.total);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("later.md", capture.names[0],
        "the final high-score row must outrank earlier rows");
    TEST_ASSERT_EQUAL_UINT16(3u, capture.value[0].score);
    for (uint32_t i = 1u; i < 20u; ++i) {
        TEST_ASSERT_EQUAL_STRING("note.md", capture.names[i]);
        TEST_ASSERT_EQUAL_UINT32(i, capture.value[i].line_number);
    }
}

static void semantic_long_line_window_is_not_rescanned(void)
{
    fixture_use(big_medium, BIG_BLOCKS);
    static uint8_t content[100000];
    memset(content, (uint8_t)'a', sizeof content);
    size_t at = 90000u;
    /* The match must be a complete word: surround it with a non-word byte. */
    content[at - 1u] = (uint8_t)' ';
    memcpy(content + at, "hit", 3u);
    content[at + 3u] = (uint8_t)' ';
    setup_linear_terms();
    fixture_create("long.md", content, (uint32_t)sizeof content);
    device_reads = 0u;
    run_ok_query_snippets("hit");
    TEST_ASSERT_EQUAL_UINT32(1u, capture.matches);
    TEST_ASSERT_EQUAL_UINT32(1u, capture.value[0].line_number);
    TEST_ASSERT_TRUE(capture.value[0].snippet_length <=
                     YAN_SEARCH_TERMS_SNIPPET_MAX);
    TEST_ASSERT_TRUE_MESSAGE(device_reads <= 52u,
        "two full scans plus a bounded snippet must not rescan the file head");
    TEST_ASSERT_EQUAL_UINT32(160u, capture.snippet_length[0]);
    TEST_ASSERT_EQUAL_MEMORY("hit", capture.snippet_body[0] + 40u, 3u);
}

static void semantic_source_io_fault_propagates_without_ok(void)
{
    fixture_use(small_medium, SMALL_BLOCKS);
    const char *content = "hit\n";
    setup_linear_terms();
    fixture_create("note.md", (const uint8_t *)content,
                   (uint32_t)strlen(content));
    /* The next data read fails: the query must surface IO, never a zero-result
     * success. */
    device_reads = 0u;
    device_read_fail_at = 2u;
    capture_reset();
    capture_terms = &terms;
    YanSearchTermsSummary summary;
    YanSearchTermsSink sink;
    sink.context = &capture;
    sink.match = capture_match;
    sink.chunk = NULL;
    YanSearchTermsResult result = yan_search_terms(
        &terms, (const uint8_t *)"hit", 3u, sink, &summary);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_TERMS_IO, result,
                                  "an I/O fault must not become a success");
}

static void semantic_search_never_writes_the_medium(void)
{
    fixture_use(small_medium, SMALL_BLOCKS);
    const char *content = "hit\n";
    setup_linear_terms();
    fixture_create("note.md", (const uint8_t *)content,
                   (uint32_t)strlen(content));
    uint32_t writes = device_writes;
    run_ok_query("hit");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        writes, device_writes,
        "a search is read-only and must not write the medium");
}

static void semantic_query_grammar_accepts_legal_and_rejects_illegal(void)
{
    fixture_use(small_medium, SMALL_BLOCKS);
    const char *content = "interrupt\n";
    setup_linear_terms();
    fixture_create("a.md", (const uint8_t *)content,
                   (uint32_t)strlen(content));
    YanSearchTermsSummary summary;
    YanSearchTermsSink sink;
    sink.context = &capture;
    sink.match = capture_match;
    sink.chunk = NULL;

    /* Empty is a facade-level INVALID and never reaches the backend. */
    uint8_t empty = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_INVALID,
                          yan_search_terms(&terms, &empty, 0u, sink, &summary));

    /* NUL, LF, DEL and invalid UTF-8 are illegal query bytes and must be
     * rejected with no I/O. */
    static const uint8_t with_nul[] = {'a', 0u, 'b'};
    static const uint8_t with_lf[] = {'a', '\n', 'b'};
    static const uint8_t with_del[] = {'a', 0x7fu, 'b'};
    static const uint8_t with_bare_cr[] = {'a', '\r', 'b'};
    static const uint8_t with_bad_utf8[] = {0xffu, 0xfeu};
    TEST_ASSERT_EQUAL_INT(
        YAN_SEARCH_TERMS_INVALID,
        yan_search_terms(&terms, with_nul, sizeof with_nul, sink, &summary));
    TEST_ASSERT_EQUAL_INT(
        YAN_SEARCH_TERMS_INVALID,
        yan_search_terms(&terms, with_lf, sizeof with_lf, sink, &summary));
    TEST_ASSERT_EQUAL_INT(
        YAN_SEARCH_TERMS_INVALID,
        yan_search_terms(&terms, with_del, sizeof with_del, sink, &summary));
    TEST_ASSERT_EQUAL_INT(
        YAN_SEARCH_TERMS_INVALID,
        yan_search_terms(&terms, with_bare_cr, sizeof with_bare_cr, sink,
                         &summary));
    TEST_ASSERT_EQUAL_INT(
        YAN_SEARCH_TERMS_INVALID,
        yan_search_terms(&terms, with_bad_utf8, sizeof with_bad_utf8, sink,
                         &summary));

    /* TAB and ASCII punctuation separate groups. */
    static const uint8_t tabbed[] = {'i', 'n', 't', 'e', 'r', '\t', 'r', 'u',
                                     'p', 't'};
    TEST_ASSERT_EQUAL_INT(
        YAN_SEARCH_TERMS_OK,
        yan_search_terms(&terms, tabbed, sizeof tabbed, sink, &summary));
}

static YanSearchTermsResult query_bytes(const uint8_t *bytes, uint32_t length)
{
    YanSearchTermsSummary summary;
    YanSearchTermsSink sink;
    sink.context = &capture;
    sink.match = capture_match;
    sink.chunk = NULL;
    return yan_search_terms(&terms, bytes, length, sink, &summary);
}

static void semantic_query_group_and_word_limits(void)
{
    fixture_use(small_medium, SMALL_BLOCKS);
    const char *content = "a\n";
    setup_linear_terms();
    fixture_create("a.md", (const uint8_t *)content,
                   (uint32_t)strlen(content));

    /* A 255-byte ASCII word is the limit; 256 bytes is INVALID. */
    static uint8_t word255[255];
    static uint8_t word256[256];
    memset(word255, (uint8_t)'a', sizeof word255);
    memset(word256, (uint8_t)'a', sizeof word256);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK, query_bytes(word255, 255u));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_INVALID, query_bytes(word256, 256u));

    /* 16 distinct single-character groups is legal; 17 is INVALID. */
    static const char groups16[] = "a b c d e f g h i j k l m n o p";
    static const char groups17[] = "a b c d e f g h i j k l m n o p q";
    TEST_ASSERT_EQUAL_INT(
        YAN_SEARCH_TERMS_OK,
        query_bytes((const uint8_t *)groups16, (uint32_t)strlen(groups16)));
    TEST_ASSERT_EQUAL_INT(
        YAN_SEARCH_TERMS_INVALID,
        query_bytes((const uint8_t *)groups17, (uint32_t)strlen(groups17)));

    /* Repeating one canonical ASCII group past 16 times is still one group. */
    static const char repeated[] =
        "CPU cpu CPU cpu CPU cpu CPU cpu CPU cpu CPU cpu CPU cpu CPU cpu "
        "CPU cpu";
    TEST_ASSERT_EQUAL_INT(
        YAN_SEARCH_TERMS_OK,
        query_bytes((const uint8_t *)repeated, (uint32_t)strlen(repeated)));
}

static void semantic_snippet_window_truncates_and_caps(void)
{
    fixture_use(small_medium, SMALL_BLOCKS);
    static char content[400];
    uint32_t offset = 0u;
    for (uint32_t i = 0u; i < 100u; ++i) {
        content[offset++] = '.';
    }
    memcpy(content + offset, "hit", 3u);
    offset += 3u;
    for (uint32_t i = 0u; i < 200u; ++i) {
        content[offset++] = '.';
    }
    content[offset++] = '\n';
    setup_linear_terms();
    fixture_create("long.md", (const uint8_t *)content, offset);
    run_ok_query_snippets("hit");
    TEST_ASSERT_EQUAL_UINT32(1u, capture.matches);
    TEST_ASSERT_TRUE(capture.value[0].snippet_length <=
                     YAN_SEARCH_TERMS_SNIPPET_MAX);
    TEST_ASSERT_TRUE_MESSAGE(capture.value[0].left_truncated,
                             "a match 100 scalars in must be left-truncated");
    TEST_ASSERT_TRUE_MESSAGE(capture.value[0].right_truncated,
                             "a match 100 scalars from the end must be "
                             "right-truncated");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(160u, capture.snippet_length[0],
        "the snippet must contain exactly 160 scalars at this boundary");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(content + 60u, capture.snippet_body[0], 160u,
        "the snippet must start exactly 40 scalars before the hit");
}

/* ------------------------------------------------ grammar / management --- */

static void terms_query_invalid_grammar_never_calls_backend(void)
{
    YanSearchTerms facade;
    memset(&facade, 0, sizeof facade);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          yan_search_terms_init(&facade, fake_backend()));
    YanSearchTermsSink sink;
    sink.context = &capture;
    sink.match = capture_match;
    sink.chunk = NULL;
    YanSearchTermsSummary summary;
    static const uint8_t bad_utf8[] = {0xffu, 0xfeu};
    static const uint8_t with_nul[] = {'a', 0u};
    static const char groups17[] = "a b c d e f g h i j k l m n o p q";
    static uint8_t word256[256];
    memset(word256, (uint8_t)'a', sizeof word256);
    TEST_ASSERT_EQUAL_INT(
        YAN_SEARCH_TERMS_INVALID,
        yan_search_terms(&facade, bad_utf8, sizeof bad_utf8, sink, &summary));
    TEST_ASSERT_EQUAL_INT(
        YAN_SEARCH_TERMS_INVALID,
        yan_search_terms(&facade, with_nul, sizeof with_nul, sink, &summary));
    TEST_ASSERT_EQUAL_INT(
        YAN_SEARCH_TERMS_INVALID,
        yan_search_terms(&facade, (const uint8_t *)groups17,
                         (uint32_t)strlen(groups17), sink, &summary));
    TEST_ASSERT_EQUAL_INT(
        YAN_SEARCH_TERMS_INVALID,
        yan_search_terms(&facade, word256, 256u, sink, &summary));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_INVALID,
        yan_search_terms(&facade, (const uint8_t *)" /- ", 4u, sink, &summary));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, fake.query_calls,
        "invalid grammar must be rejected before the backend runs");
}

static void terms_query_accepts_repeated_canonical_groups(void)
{
    YanSearchTerms facade;
    memset(&facade, 0, sizeof facade);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          yan_search_terms_init(&facade, fake_backend()));
    static uint8_t repeated[128];
    uint32_t at = 0u;
    for (uint32_t i = 0u; i < 18u; ++i) {
        if (i > 0u) {
            repeated[at++] = (uint8_t)' ';
        }
        const char *word = (i % 2u) == 0u ? "CPU" : "cpu";
        repeated[at++] = (uint8_t)word[0];
        repeated[at++] = (uint8_t)word[1];
        repeated[at++] = (uint8_t)word[2];
    }
    YanSearchTermsSink sink;
    sink.context = &capture;
    sink.match = capture_match;
    sink.chunk = NULL;
    YanSearchTermsSummary summary;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_TERMS_OK,
        yan_search_terms(&facade, repeated, at, sink, &summary),
        "18 repeats of one canonical group are still one legal group");
    TEST_ASSERT_EQUAL_UINT32(1u, fake.query_calls);
}

static void terms_management_reentry_is_busy(void)
{
    YanSearchTerms facade;
    memset(&facade, 0, sizeof facade);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          yan_search_terms_init(&facade, fake_backend()));
    fake.manage_reenter = true;
    fake.manage_terms = &facade;
    YanSearchTermsStatus status;
    fake.manage_reenter_result = YAN_SEARCH_TERMS_OK;
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          yan_search_terms_status(&facade, &status));
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_TERMS_BUSY,
                                  fake.manage_reenter_result,
                                  "status reentry must answer BUSY");
    fake.manage_reenter_result = YAN_SEARCH_TERMS_OK;
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK, yan_search_terms_clear(&facade));
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_TERMS_BUSY,
                                  fake.manage_reenter_result,
                                  "clear reentry must answer BUSY");
    fake.manage_reenter_result = YAN_SEARCH_TERMS_OK;
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          yan_search_terms_rebuild(&facade));
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_TERMS_BUSY,
                                  fake.manage_reenter_result,
                                  "rebuild reentry must answer BUSY");
}

static void terms_management_unknown_result_maps_protocol(void)
{
    YanSearchTerms facade;
    memset(&facade, 0, sizeof facade);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          yan_search_terms_init(&facade, fake_backend()));
    fake.status_result = (YanSearchTermsResult)200;
    fake.rebuild_result = (YanSearchTermsResult)201;
    fake.clear_result = (YanSearchTermsResult)202;
    YanSearchTermsStatus status;
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_PROTOCOL,
                          yan_search_terms_status(&facade, &status));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_PROTOCOL,
                          yan_search_terms_clear(&facade));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_PROTOCOL,
                          yan_search_terms_rebuild(&facade));
}

static void terms_read_snippet_rejects_wrapping_holder(void)
{
    YanSearchTerms facade;
    memset(&facade, 0, sizeof facade);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          yan_search_terms_init(&facade, fake_backend()));
    wrap_probe_terms = &facade;
    wrap_probe_result = YAN_SEARCH_TERMS_OK;
    fake.emit_match = true;
    fake.snippet_length = 4u;
    uint8_t query = (uint8_t)'x';
    YanSearchTermsSummary summary;
    YanSearchTermsSink sink;
    sink.context = NULL;
    sink.match = wrap_probe_match;
    sink.chunk = NULL;
    TEST_ASSERT_EQUAL_INT(
        YAN_SEARCH_TERMS_OK,
        yan_search_terms(&facade, &query, 1u, sink, &summary));
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_TERMS_INVALID, wrap_probe_result,
        "a snippet holder whose range leaves uintptr must be rejected");
}

/* ------------------------------------------------------ linear semantics --- */

static void semantic_skips_invalid_utf8_at_end_and_continues(void)
{
    fixture_use(small_medium, SMALL_BLOCKS);
    static const uint8_t bad[] = {'h', 'i', 't', '\n', 0xe4u, 0xb8u};
    static const uint8_t good[] = {'h', 'i', 't', '\n'};
    setup_linear_terms();
    fixture_create("bad.md", bad, sizeof bad);
    fixture_create("good.md", good, sizeof good);
    YanSearchTermsSummary summary = run_ok_query("hit");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, summary.skipped,
        "a whole invalid file must be skipped once");
    TEST_ASSERT_EQUAL_UINT64(1u, summary.total);
    TEST_ASSERT_EQUAL_STRING("good.md", capture.names[0]);
}

static void semantic_skips_nul_at_end_and_continues(void)
{
    fixture_use(small_medium, SMALL_BLOCKS);
    static const uint8_t bad[] = {'h', 'i', 't', '\n', 0u};
    static const uint8_t good[] = {'h', 'i', 't', '\n'};
    setup_linear_terms();
    fixture_create("bad.md", bad, sizeof bad);
    fixture_create("good.md", good, sizeof good);
    YanSearchTermsSummary summary = run_ok_query("hit");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, summary.skipped,
        "a whole invalid file must be skipped once");
    TEST_ASSERT_EQUAL_UINT64(1u, summary.total);
    TEST_ASSERT_EQUAL_STRING("good.md", capture.names[0]);
}

static void semantic_match_across_read_boundary(void)
{
    fixture_use(small_medium, SMALL_BLOCKS);
    static uint8_t content[5000];
    memset(content, (uint8_t)'.', sizeof content);
    content[4093] = (uint8_t)' ';
    memcpy(content + 4094, "hit", 3u);
    content[4097] = (uint8_t)' ';
    content[4999] = (uint8_t)'\n';
    setup_linear_terms();
    fixture_create("a.md", content, (uint32_t)sizeof content);
    run_ok_query("hit");
    TEST_ASSERT_EQUAL_UINT32(1u, capture.matches);
    TEST_ASSERT_EQUAL_UINT32(1u, capture.value[0].line_number);
}

static void semantic_crlf_across_read_boundary(void)
{
    fixture_use(small_medium, SMALL_BLOCKS);
    static uint8_t content[5000];
    memset(content, (uint8_t)'.', 4095u);
    content[4095] = (uint8_t)'\r';
    content[4096] = (uint8_t)'\n';
    memcpy(content + 4097, "hit\n", 4u);
    setup_linear_terms();
    fixture_create("a.md", content, 4101u);
    run_ok_query("hit");
    TEST_ASSERT_EQUAL_UINT32(1u, capture.matches);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        2u, capture.value[0].line_number,
        "the CRLF straddles the 4096-byte read boundary");
}

static void semantic_snippet_keeps_four_byte_scalars(void)
{
    fixture_use(small_medium, SMALL_BLOCKS);
    static const uint8_t content[] = {
        '.', '.', '.', 0xf0u, 0x9fu, 0x98u, 0x80u, '.', 'h', 'i', 't',
        '.', '.', '\n'
    };
    setup_linear_terms();
    fixture_create("a.md", content, sizeof content);
    run_ok_query_snippets("hit");
    TEST_ASSERT_EQUAL_UINT32(1u, capture.matches);
    TEST_ASSERT_EQUAL_UINT32(13u, capture.snippet_length[0]);
    TEST_ASSERT_TRUE_MESSAGE(
        yan_search_text_utf8_valid(capture.snippet_body[0],
                                   capture.snippet_length[0]),
        "the snippet must not split a four-byte scalar");
}

static void semantic_group_score_saturates_above_255(void)
{
    fixture_use(small_medium, SMALL_BLOCKS);
    static uint8_t content[1041];
    for (uint32_t i = 0u; i < 260u; ++i) {
        memcpy(content + 4u * i, "hit ", 4u);
    }
    content[1040] = '\n';
    setup_linear_terms();
    fixture_create("cap.md", content, (uint32_t)sizeof content);
    YanSearchTermsSummary summary = run_ok_query("hit");
    TEST_ASSERT_EQUAL_UINT64(1u, summary.total);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(255u, capture.value[0].score,
        "a group must cap accepted occurrences at 255");
}

static void semantic_score_is_capped_at_4080(void)
{
    fixture_use(big_medium, BIG_BLOCKS);
    static uint8_t content[9000];
    uint32_t at = 0u;
    for (uint32_t g = 0u; g < 16u; ++g) {
        uint8_t letter = (uint8_t)('a' + g);
        for (uint32_t n = 0u; n < 260u; ++n) {
            content[at++] = letter;
            content[at++] = (uint8_t)' ';
        }
    }
    content[at++] = (uint8_t)'\n';
    setup_linear_terms();
    fixture_create("cap.md", content, at);
    YanSearchTermsSummary summary =
        run_ok_query("a b c d e f g h i j k l m n o p");
    TEST_ASSERT_EQUAL_UINT64(1u, summary.total);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(
        4080u, capture.value[0].score,
        "16 groups capped at 255 each must saturate the uint16 score");
}

static void semantic_slot_holes_keep_directory_order(void)
{
    fixture_use(small_medium, SMALL_BLOCKS);
    setup_linear_terms();
    fixture_create("a", (const uint8_t *)"hit\n", 4u);
    fixture_create("b", (const uint8_t *)"hit\n", 4u);
    fixture_create("c", (const uint8_t *)"hit\n", 4u);
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_remove(&fs, "b"));
    YanSearchTermsSummary summary = run_ok_query("hit");
    TEST_ASSERT_EQUAL_UINT64(2u, summary.total);
    TEST_ASSERT_EQUAL_UINT32(2u, capture.matches);
    TEST_ASSERT_EQUAL_STRING("a", capture.names[0]);
    TEST_ASSERT_EQUAL_STRING("c", capture.names[1]);
}

static void semantic_protocol_fault_propagates(void)
{
    fixture_use(small_medium, SMALL_BLOCKS);
    const char *content = "hit\n";
    setup_linear_terms();
    fixture_create("a.md", (const uint8_t *)content,
                   (uint32_t)strlen(content));
    device_reads = 0u;
    device_read_fail_at = 1u;
    device_read_fail_code = YAN_FS_IO_PROTOCOL;
    capture_reset();
    capture_terms = &terms;
    YanSearchTermsSummary summary;
    YanSearchTermsSink sink;
    sink.context = &capture;
    sink.match = capture_match;
    sink.chunk = NULL;
    YanSearchTermsResult result = yan_search_terms(
        &terms, (const uint8_t *)"hit", 3u, sink, &summary);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_TERMS_PROTOCOL, result,
                                  "a PROTOCOL read must propagate");
    device_read_fail_at = 0u;
    device_read_fail_code = YAN_FS_IO_ERROR;
}

static YanSearchTerms *nested_reader_terms;
static YanSearchTermsResult nested_reader_result;
static bool nested_holder_unchanged;

static bool nested_reader_chunk(void *context, uint32_t offset,
                               const uint8_t *bytes, uint32_t length)
{
    (void)context; (void)offset; (void)bytes; (void)length;
    const uint8_t *out = NULL;
    uint32_t got = UINT32_MAX;
    nested_reader_result = yan_search_terms_read_snippet(
        nested_reader_terms, 0u, 4u, &out, &got);
    nested_holder_unchanged = out == NULL && got == UINT32_MAX;
    return true;
}

static bool overlapping_reader_match(void *context,
                                     const YanSearchTermsMatch *match)
{
    (void)context; (void)match;
    const uint8_t *out = NULL;
    uint8_t before[sizeof out];
    memcpy(before, &out, sizeof out);
    nested_reader_result = yan_search_terms_read_snippet(
        nested_reader_terms, 0u, 4u, &out, (uint32_t *)(void *)&out);
    nested_holder_unchanged = memcmp(before, &out, sizeof out) == 0;
    return true;
}

static void terms_nested_reader_is_busy_without_backend_io(void)
{
    YanSearchTerms facade = {0};
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
        yan_search_terms_init(&facade, fake_backend()));
    fake.emit_match = true;
    fake.snippet_length = 4u;
    memcpy(fake.snippet, "abcd", 4u);
    nested_reader_terms = &facade;
    nested_reader_result = YAN_SEARCH_TERMS_OK;
    nested_holder_unchanged = false;
    YanSearchTermsSink sink = {NULL, noop_match, nested_reader_chunk};
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
        yan_search_terms(&facade, (const uint8_t *)"x", 1u, sink, NULL));
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_TERMS_BUSY, nested_reader_result,
        "a nested reader must return BUSY");
    TEST_ASSERT_TRUE(nested_holder_unchanged);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, fake.read_calls,
        "a nested reader must not perform another backend read");
}

static void terms_reader_rejects_overlapping_outputs_unchanged(void)
{
    YanSearchTerms facade = {0};
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
        yan_search_terms_init(&facade, fake_backend()));
    fake.emit_match = true;
    fake.snippet_length = 4u;
    nested_reader_terms = &facade;
    nested_reader_result = YAN_SEARCH_TERMS_OK;
    nested_holder_unchanged = false;
    YanSearchTermsSink sink = {NULL, overlapping_reader_match, NULL};
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
        yan_search_terms(&facade, (const uint8_t *)"x", 1u, sink, NULL));
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_TERMS_INVALID, nested_reader_result,
        "overlapping snippet outputs must be rejected");
    TEST_ASSERT_TRUE(nested_holder_unchanged);
    TEST_ASSERT_EQUAL_UINT32(0u, fake.read_calls);
}

static void terms_summary_is_optional_and_unchanged_on_error(void)
{
    YanSearchTerms facade = {0};
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
        yan_search_terms_init(&facade, fake_backend()));
    YanSearchTermsSink sink = {NULL, noop_match, NULL};
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
        yan_search_terms(&facade, (const uint8_t *)"x", 1u, sink, NULL));
    fake.query_result = YAN_SEARCH_TERMS_IO;
    YanSearchTermsSummary summary;
    uint8_t before[sizeof summary];
    memset(&summary, 0xa5, sizeof summary);
    memcpy(before, &summary, sizeof summary);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_IO,
        yan_search_terms(&facade, (const uint8_t *)"x", 1u, sink, &summary));
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(before, &summary, sizeof summary,
        "an unsuccessful query must not publish its summary");
}

static void semantic_utf8_scalars_cross_block_edges_without_skip(void)
{
    static uint8_t content[4101];
    const char *scalars[] = {"\xc3\xa9", "\xe4\xb8\xad", "\xf0\x9f\x99\x82"};
    for (uint32_t i = 0u; i < 3u; ++i) {
        fixture_use(big_medium, BIG_BLOCKS);
        memset(content, '.', sizeof content);
        uint32_t length = (uint32_t)strlen(scalars[i]);
        memcpy(content + 4095u, scalars[i], length);
        content[4095u + length] = '\n';
        setup_linear_terms();
        fixture_create("split.md", content, 4096u + length);
        YanSearchTermsSummary summary = run_ok_query_snippets(scalars[i]);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, summary.skipped,
            "a valid scalar across a block boundary must not be skipped");
        TEST_ASSERT_EQUAL_UINT64(1u, summary.total);
        TEST_ASSERT_EQUAL_UINT32(40u + length, capture.snippet_length[0]);
        TEST_ASSERT_EQUAL_MEMORY(scalars[i], capture.snippet_body[0] + 40u, length);
    }
}

static void semantic_non_ascii_punctuation_and_accent_keep_literal_scalars(void)
{
    fixture_use(small_medium, SMALL_BLOCKS);
    setup_linear_terms();
    const char *content = "中，断 café\n中 断 cafe\n";
    fixture_create("unicode.md", (const uint8_t *)content, (uint32_t)strlen(content));
    const char *queries[] = {"中，断", "caf", "café"};
    for (uint32_t i = 0u; i < 3u; ++i) {
        YanSearchTermsSummary summary = run_ok_query(queries[i]);
        TEST_ASSERT_EQUAL_UINT64(1u, summary.total);
        TEST_ASSERT_EQUAL_UINT32(1u, capture.value[0].line_number);
        TEST_ASSERT_EQUAL_UINT16(1u, capture.value[0].score);
    }
    YanSearchTermsSummary summary = run_ok_query("中 断");
    TEST_ASSERT_EQUAL_UINT64(2u, summary.total);
    TEST_ASSERT_EQUAL_UINT16(2u, capture.value[0].score);
    TEST_ASSERT_EQUAL_UINT16(2u, capture.value[1].score);
}

static void linear_status_busy_is_not_swallowed(void)
{
    fixture_use(small_medium, SMALL_BLOCKS);
    const char *content = "hit\n";
    setup_linear_terms();
    fixture_create("a.md", (const uint8_t *)content,
                   (uint32_t)strlen(content));
    device_probe_armed = true;
    device_probe_linear = &linear;
    device_probe_result = YAN_SEARCH_TERMS_OK;
    device_probe_status.terms = 0xdeadbeefu;
    device_reads = 0u;
    capture_reset();
    capture_terms = &terms;
    YanSearchTermsSummary summary;
    YanSearchTermsSink sink;
    sink.context = &capture;
    sink.match = capture_match;
    sink.chunk = NULL;
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_TERMS_OK,
                          yan_search_terms(&terms, (const uint8_t *)"hit", 3u,
                                           sink, &summary));
    TEST_ASSERT_FALSE_MESSAGE(device_probe_armed,
                              "the probe must have run inside a read callback");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_TERMS_BUSY, device_probe_result,
        "a busy filesystem must be reported as TERMS_BUSY, not UNINITIALIZED");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0xdeadbeefu, device_probe_status.terms,
        "a BUSY observation must leave the status holder untouched");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(text_decodes_ascii_and_multibyte_scalars);
    RUN_TEST(text_rejects_invalid_and_truncated_sequences);
    RUN_TEST(text_validates_whole_buffers);
    RUN_TEST(text_ascii_word_and_fold);
    RUN_TEST(terms_init_rejects_invalid_backends_and_aliases);
    RUN_TEST(terms_init_accepts_a_good_backend_and_can_reinit);
    RUN_TEST(terms_query_rejects_invalid_arguments);
    RUN_TEST(terms_query_relays_backend_matches_and_summary);
    RUN_TEST(terms_query_maps_unknown_backend_result_to_protocol);
    RUN_TEST(terms_query_reports_stopped_when_a_sink_stops);
    RUN_TEST(terms_management_is_busy_inside_a_query);
    RUN_TEST(terms_read_snippet_outside_a_callback_is_invalid);
    RUN_TEST(terms_read_snippet_reads_inside_the_callback);
    RUN_TEST(terms_read_snippet_rejects_aliasing_holders);
    RUN_TEST(terms_read_snippet_sticky_error_beats_a_stop);
    RUN_TEST(terms_status_and_management_dispatch);
    RUN_TEST(linear_init_rejects_invalid_and_overlapping_pairs);
    RUN_TEST(linear_status_reports_empty_and_the_real_source);
    RUN_TEST(linear_clear_is_a_noop_and_rebuild_is_unsupported);
    RUN_TEST(semantic_ascii_whole_word_and_case);
    RUN_TEST(semantic_ascii_word_chars_digits_and_underscore);
    RUN_TEST(semantic_chinese_scalars_must_be_contiguous);
    RUN_TEST(semantic_mixed_group_ascii_then_scalars);
    RUN_TEST(semantic_groups_require_the_same_line);
    RUN_TEST(semantic_duplicate_groups_do_not_multiply_the_score);
    RUN_TEST(semantic_overlapping_chinese_counts_each_start);
    RUN_TEST(semantic_ascii_aa_does_not_match_inside_aaaa);
    RUN_TEST(semantic_nul_file_is_skipped_once);
    RUN_TEST(semantic_line_endings_and_missing_final_newline);
    RUN_TEST(semantic_bare_cr_is_content_and_only_lf_ends_a_line);
    RUN_TEST(semantic_query_group_matches_through_a_bare_cr);
    RUN_TEST(semantic_top20_total_and_score_order);
    RUN_TEST(semantic_long_line_window_is_not_rescanned);
    RUN_TEST(semantic_source_io_fault_propagates_without_ok);
    RUN_TEST(semantic_search_never_writes_the_medium);
    RUN_TEST(semantic_query_grammar_accepts_legal_and_rejects_illegal);
    RUN_TEST(semantic_query_group_and_word_limits);
    RUN_TEST(semantic_snippet_window_truncates_and_caps);
    RUN_TEST(terms_query_invalid_grammar_never_calls_backend);
    RUN_TEST(terms_query_accepts_repeated_canonical_groups);
    RUN_TEST(terms_management_reentry_is_busy);
    RUN_TEST(terms_management_unknown_result_maps_protocol);
    RUN_TEST(terms_read_snippet_rejects_wrapping_holder);
    RUN_TEST(semantic_skips_invalid_utf8_at_end_and_continues);
    RUN_TEST(semantic_skips_nul_at_end_and_continues);
    RUN_TEST(semantic_match_across_read_boundary);
    RUN_TEST(semantic_crlf_across_read_boundary);
    RUN_TEST(semantic_snippet_keeps_four_byte_scalars);
    RUN_TEST(semantic_score_is_capped_at_4080);
    RUN_TEST(semantic_group_score_saturates_above_255);
    RUN_TEST(semantic_slot_holes_keep_directory_order);
    RUN_TEST(semantic_protocol_fault_propagates);
    RUN_TEST(linear_status_busy_is_not_swallowed);
    RUN_TEST(terms_nested_reader_is_busy_without_backend_io);
    RUN_TEST(terms_reader_rejects_overlapping_outputs_unchanged);
    RUN_TEST(terms_summary_is_optional_and_unchanged_on_error);
    RUN_TEST(semantic_utf8_scalars_cross_block_edges_without_skip);
    RUN_TEST(semantic_non_ascii_punctuation_and_accent_keep_literal_scalars);
    return UNITY_END();
}
