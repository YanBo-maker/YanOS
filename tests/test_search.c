#include "search.h"
#include "search_linear.h"
#include "unity.h"

#include <stddef.h>
#include <string.h>

/* Native tests for the 0025 unified search facade (os/search.c) and the linear
 * backend contract (os/search_linear.h / os/search_linear.c).
 *
 * Two drivers share the public core API:
 *
 *   - The real driver mounts os/yanfs.c over an in-memory block device with 48
 *     fixed blocks (enough for the 100 KiB single-line fixture) and uses the
 *     linear backend. Those tests own the byte/line/NUL/reader semantics.
 *   - A fake backend with no filesystem at all drives the facade lifecycle:
 *     validation order, busy, callback stop, sticky reader errors, backend
 *     swapping and reader period/range/alias guards.
 *
 * The capture callbacks and the trace are independent of the implementation:
 * every expected byte, line number and read LBA is written here, so a future
 * mutation of the implementation should fail the test named for that rule.
 *
 * Nothing here manipulates an editor draft, touches the console or assumes any
 * filesystem layout beyond the public yan_fs_* calls. */

#define TEST_DEVICE_BLOCKS 48u
#define TEST_TRACE_MAX 64u
#define TEST_MATCH_MAX 64u
#define TEST_LINE_BYTES 102400u
#define TEST_CONTENT_MAX 131072u
#define TEST_FAKE_MATCH_MAX 12u

/* ------------------------------------------------------------ shared state */

typedef struct {
    uint8_t blocks[TEST_DEVICE_BLOCKS][YAN_FS_BLOCK_SIZE];
    uint64_t capacity_blocks;
    uint32_t reads;
    uint32_t writes;
    uint32_t read_fail_at; /* 1-based read call to fail; 0 = never */
    YanFsIoResult read_status;
    YanFsIoResult read_fail_code;
    uint32_t write_fail_at;
    YanFsIoResult write_status;
    YanFsIoResult write_fail_code;
    bool trace_enabled;
    uint32_t trace_count;
    char trace_kind[TEST_TRACE_MAX];
    uint32_t trace_lba[TEST_TRACE_MAX];
} TestDevice;

static TestDevice device;
static YanFs fs;
static YanSearchLinear linear;
static YanSearch search;
static uint8_t source[TEST_LINE_BYTES];
static uint8_t medium_snapshot[TEST_DEVICE_BLOCKS][YAN_FS_BLOCK_SIZE];

typedef struct {
    uint32_t matches;
    char names[TEST_MATCH_MAX][YAN_FS_NAME_MAX + 1u];
    uint32_t lines[TEST_MATCH_MAX];
    uint32_t lengths[TEST_MATCH_MAX];
    uint32_t chunks;
    uint32_t max_chunk;
    uint32_t content_length;
    uint8_t content[TEST_CONTENT_MAX];
    uint32_t stop_after; /* return false after this many matches; 0 = never */
    bool manual_read;    /* match callback pulls content with read_match */
    bool ignore_reader_error;
    YanSearchResult reader_result;
    bool overflow;
} Capture;

static Capture capture;

/* -------------------------------------------------------- reader probe state */

typedef enum {
    READER_PROBE_NONE = 0,
    READER_PROBE_OK,
    READER_PROBE_NULL_BYTES,
    READER_PROBE_NULL_LENGTH,
    READER_PROBE_ALIAS_SEARCH,
    READER_PROBE_ALIAS_OUTPUTS,
    READER_PROBE_OFFSET_PAST_END,
    READER_PROBE_OFFSET_AT_END,
    READER_PROBE_CAPACITY_ZERO,
    READER_PROBE_ADDRESS_OVERFLOW
} ReaderProbeKind;

static ReaderProbeKind reader_probe_kind;
static YanSearchResult reader_probe_result;
static uint32_t reader_probe_length;
static uint32_t *reader_probe_io;
static uint32_t reader_probe_io_before;
static uint32_t reader_probe_io_after;

/* Reentrant-callback probes for the real backend. */
static bool reentry_armed;
static YanSearchResult reentry_result;

/* -------------------------------------------------------------- fake backend */

typedef struct {
    uint32_t query_calls;
    uint32_t read_calls;
    uint32_t match_count;
    YanSearchResult query_result;
    YanSearchResult read_result;
    uint32_t read_fail_at;
    YanSearchResult read_fail_code;
    const char *names[TEST_FAKE_MATCH_MAX];
    uint32_t lines[TEST_FAKE_MATCH_MAX];
    const uint8_t *contents[TEST_FAKE_MATCH_MAX];
    uint32_t content_lengths[TEST_FAKE_MATCH_MAX];
    uint32_t emitted;
    uint32_t read_chunk; /* 0 = return the whole requested range */
    uint32_t malform;    /* FakeMalform: 0 = well formed chunk */
    bool reentry_query;
    bool reentry_init;
    YanSearch *reentry_search;
    YanSearchResult reentry_result;
} FakeBackend;

/* Malformed chunk shapes a backend must never produce. They let the facade
 * contract be pinned before the linear backend can exercise it for real. */
typedef enum {
    FAKE_MALFORM_NONE = 0,
    FAKE_MALFORM_ZERO_PROGRESS, /* length 0 while the line is not finished */
    FAKE_MALFORM_OVERLONG,      /* length past the requested/remaining range */
    FAKE_MALFORM_NULL_CHUNK,    /* length > 0 with a null byte pointer */
    FAKE_MALFORM_RANGE_WRAP     /* byte range that leaves uintptr_t */
} FakeMalform;

static FakeBackend fake;

static const uint8_t fake_body_a[4] = { (uint8_t)'a', (uint8_t)'b',
                                        (uint8_t)'c', (uint8_t)'d' };
static const uint8_t fake_body_b[4] = { (uint8_t)'w', (uint8_t)'x',
                                        (uint8_t)'y', (uint8_t)'z' };
static const uint8_t fake_body_c[6] = { (uint8_t)'p', (uint8_t)'e',
                                        (uint8_t)'e', (uint8_t)'k',
                                        (uint8_t)'e', (uint8_t)'d' };

/* ---------------------------------------------------------------- utilities */

static void capture_reset(void)
{
    memset(&capture, 0, sizeof capture);
}

static void capture_name(uint32_t index, const char *name)
{
    uint32_t length = 0u;
    while (name[length] != '\0' && length < YAN_FS_NAME_MAX) {
        capture.names[index][length] = name[length];
        ++length;
    }
    capture.names[index][length] = '\0';
}

static bool capture_match(void *context, const YanSearchMatch *match)
{
    (void)context;
    if (capture.matches >= TEST_MATCH_MAX) {
        capture.overflow = true;
        return false;
    }
    uint32_t index = capture.matches;
    capture_name(index, match->name);
    capture.lines[index] = match->line_number;
    capture.lengths[index] = match->content_length;
    ++capture.matches;
    if (capture.stop_after != 0u && capture.matches >= capture.stop_after) {
        return false;
    }
    return true;
}

static bool capture_chunk(void *context, uint32_t offset,
                          const uint8_t *bytes, uint32_t length)
{
    (void)context;
    (void)offset;
    ++capture.chunks;
    if (length > capture.max_chunk) {
        capture.max_chunk = length;
    }
    if (length > TEST_CONTENT_MAX - capture.content_length) {
        capture.overflow = true;
        return false;
    }
    for (uint32_t i = 0; i < length; ++i) {
        capture.content[capture.content_length + i] = bytes[i];
    }
    capture.content_length += length;
    return true;
}

/* Manual style: the match callback pulls the content itself through the public
 * reader, so it can also choose to ignore a reader error. */
static bool capture_manual_match(void *context, const YanSearchMatch *match)
{
    if (!capture_match(context, match)) {
        return false;
    }
    const uint8_t *bytes = NULL;
    uint32_t until_end = match->content_length;
    uint32_t offset = 0u;
    while (offset < until_end) {
        uint32_t length = 0u;
        YanSearchResult result = yan_search_read_match(
            &search, offset, until_end - offset, &bytes, &length);
        capture.reader_result = result;
        if (result != YAN_SEARCH_OK) {
            return capture.ignore_reader_error;
        }
        if (length == 0u) {
            break;
        }
        if (!capture_chunk(context, offset, bytes, length)) {
            return false;
        }
        offset += length;
    }
    return true;
}

static bool capture_stop_first_chunk(void *context, uint32_t offset,
                                     const uint8_t *bytes, uint32_t length)
{
    (void)context;
    (void)offset;
    (void)bytes;
    (void)length;
    return false;
}

static YanSearchSink capture_sink(void)
{
    YanSearchSink sink;
    sink.context = NULL;
    sink.match = capture.manual_read ? capture_manual_match : capture_match;
    sink.chunk = capture.manual_read ? NULL : capture_chunk;
    return sink;
}

static YanSearchResult run_query(const uint8_t *pattern, uint32_t length)
{
    return yan_search_query(&search, pattern, length, capture_sink());
}

/* ------------------------------------------------------------- device driver */

static YanFsIoResult device_capacity(void *context, uint64_t *blocks)
{
    TestDevice *self = (TestDevice *)context;
    *blocks = self->capacity_blocks;
    return YAN_FS_IO_OK;
}

static void trace_record(char kind, uint32_t lba)
{
    if (!device.trace_enabled || device.trace_count >= TEST_TRACE_MAX) {
        return;
    }
    device.trace_kind[device.trace_count] = kind;
    device.trace_lba[device.trace_count] = lba;
    ++device.trace_count;
}

static YanFsIoResult device_read_block(void *context, uint32_t lba,
                                       uint8_t out[4096])
{
    TestDevice *self = (TestDevice *)context;
    ++self->reads;
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
    ++self->writes;
    trace_record('W', lba);
    if (lba >= TEST_DEVICE_BLOCKS) {
        return YAN_FS_IO_ERROR;
    }
    if (self->write_status != YAN_FS_IO_OK) {
        return self->write_status;
    }
    if (self->write_fail_at != 0u && self->writes == self->write_fail_at) {
        return self->write_fail_code != YAN_FS_IO_OK ? self->write_fail_code
                                                     : YAN_FS_IO_ERROR;
    }
    for (uint32_t i = 0; i < YAN_FS_BLOCK_SIZE; ++i) {
        self->blocks[lba][i] = data[i];
    }
    return YAN_FS_IO_OK;
}

/* ------------------------------------------------------------ real fixtures */

static void real_setup(void)
{
    memset(&device, 0, sizeof device);
    device.capacity_blocks = TEST_DEVICE_BLOCKS;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_FS_OK,
        yan_fs_format_metadata(device.blocks[0], TEST_DEVICE_BLOCKS),
        "the fixture must format an empty directory");
    memset(&fs, 0, sizeof fs);
    YanFsBlockIo io;
    io.context = &device;
    io.capacity = device_capacity;
    io.read_block = device_read_block;
    io.write_block = device_write_block;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK, yan_fs_init(&fs, io),
                                  "the fixture must initialize the filesystem");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK, yan_fs_mount(&fs),
                                  "the fixture must mount the empty directory");
    memset(&linear, 0, sizeof linear);
    memset(&search, 0, sizeof search);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_OK,
                                  yan_search_linear_init(&linear, &fs),
                                  "the linear backend must initialize");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK, yan_search_init(&search, yan_search_linear_backend(&linear)),
        "the Search must accept the linear backend");
}

static void create_file(const char *name, const uint8_t *bytes, uint32_t length)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK,
                                  yan_fs_create(&fs, name, bytes, length),
                                  "the fixture file must be created");
}

static void snapshot_medium(void)
{
    memcpy(medium_snapshot, device.blocks, sizeof device.blocks);
}

static bool medium_unchanged(void)
{
    return memcmp(medium_snapshot, device.blocks, sizeof device.blocks) == 0;
}

static void assert_trace(const char *kinds, const uint32_t *lbas, uint32_t count,
                         const char *message)
{
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(count, device.trace_count, message);
    for (uint32_t i = 0; i < count; ++i) {
        TEST_ASSERT_EQUAL_INT_MESSAGE((int)kinds[i], (int)device.trace_kind[i],
                                      message);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(lbas[i], device.trace_lba[i], message);
    }
}

/* ------------------------------------------------------------- fake backend */

static bool fake_noop_match(void *context, const YanSearchMatch *match)
{
    (void)context;
    (void)match;
    return true;
}

static YanSearchResult fake_read_match(void *context, uint32_t offset,
                                       uint32_t capacity,
                                       const uint8_t **bytes, uint32_t *length)
{
    FakeBackend *self = (FakeBackend *)context;
    ++self->read_calls;
    if (self->read_fail_at != 0u && self->read_calls == self->read_fail_at) {
        return self->read_fail_code;
    }
    if (self->read_result != YAN_SEARCH_OK) {
        return self->read_result;
    }
    const uint8_t *content = self->contents[self->emitted];
    uint32_t total = self->content_lengths[self->emitted];
    switch ((FakeMalform)self->malform) {
    case FAKE_MALFORM_ZERO_PROGRESS:
        *bytes = content;
        *length = 0u;
        return YAN_SEARCH_OK;
    case FAKE_MALFORM_OVERLONG:
        *bytes = content;
        *length = total + 4u;
        return YAN_SEARCH_OK;
    case FAKE_MALFORM_NULL_CHUNK:
        *bytes = NULL;
        *length = total;
        return YAN_SEARCH_OK;
    case FAKE_MALFORM_RANGE_WRAP:
        *bytes = (const uint8_t *)(uintptr_t)(UINTPTR_MAX - 1u);
        *length = 8u;
        return YAN_SEARCH_OK;
    case FAKE_MALFORM_NONE:
    default:
        break;
    }
    if (offset >= total) {
        *bytes = NULL;
        *length = 0u;
        return YAN_SEARCH_OK;
    }
    uint32_t available = total - offset;
    uint32_t chunk = self->read_chunk != 0u ? self->read_chunk : capacity;
    if (chunk > available) {
        chunk = available;
    }
    if (chunk > capacity) {
        chunk = capacity;
    }
    *bytes = content + offset;
    *length = chunk;
    return YAN_SEARCH_OK;
}

static YanSearchResult fake_query(void *context, const uint8_t *pattern,
                                  uint32_t pattern_length,
                                  YanSearchMatchFn match, void *match_context)
{
    FakeBackend *self = (FakeBackend *)context;
    (void)pattern;
    (void)pattern_length;
    ++self->query_calls;
    if (self->reentry_query) {
        YanSearchSink sink;
        sink.context = NULL;
        sink.match = fake_noop_match;
        sink.chunk = NULL;
        self->reentry_result = yan_search_query(
            self->reentry_search, (const uint8_t *)"x", 1u, sink);
    }
    if (self->reentry_init) {
        YanSearchBackend backend;
        backend.context = self;
        backend.context_size = sizeof(FakeBackend);
        backend.source_context = NULL;
        backend.source_size = 0u;
        backend.query = fake_query;
        backend.read_match = fake_read_match;
        self->reentry_result = yan_search_init(self->reentry_search, backend);
    }
    for (uint32_t i = 0; i < self->match_count; ++i) {
        YanSearchMatch value;
        value.name = self->names[i] != NULL ? self->names[i] : "fake";
        value.line_number = self->lines[i];
        value.content_length = self->content_lengths[i];
        self->emitted = i;
        if (!match(match_context, &value)) {
            return self->query_result;
        }
    }
    return self->query_result;
}

static YanSearchBackend fake_vtable_for(void *context, size_t context_size,
                                        void *borrowed_source, size_t source_size)
{
    YanSearchBackend backend;
    backend.context = context;
    backend.context_size = context_size;
    backend.source_context = borrowed_source;
    backend.source_size = source_size;
    backend.query = fake_query;
    backend.read_match = fake_read_match;
    return backend;
}

static YanSearchBackend fake_vtable(void)
{
    /* The fake backend borrows no filesystem, but its context is a real
     * protected span so the alias contract is exercised without an FS. */
    return fake_vtable_for(&fake, sizeof fake, NULL, 0u);
}

static void fake_install(void)
{
    memset(&search, 0, sizeof search);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_OK, yan_search_init(&search, fake_vtable()),
                                  "the fake backend must initialize");
}

static void fake_single_match(const char *name, uint32_t line,
                              const uint8_t *body, uint32_t length)
{
    memset(&fake, 0, sizeof fake);
    fake.query_result = YAN_SEARCH_OK;
    fake.read_result = YAN_SEARCH_OK;
    fake.read_fail_code = YAN_SEARCH_IO;
    fake.match_count = 1u;
    fake.names[0] = name;
    fake.lines[0] = line;
    fake.contents[0] = body;
    fake.content_lengths[0] = length;
}

/* --------------------------------------------------- protected-span fixtures */

/* One aligned container large enough for a Search, a YanFs or a
 * YanSearchLinear. Every union member starts at the same address, so a test can
 * make any pair overlap without ever forming an out-of-bounds pointer. */
typedef union {
    YanSearch search;
    YanFs fs;
    YanSearchLinear linear;
} AliasUnion;

static AliasUnion alias_union;

/* Aligned byte region used to place overlapping or adjacent protected spans. */
static uint32_t span_words[32];

/* A protected region whose first fields double as reader output holders, so the
 * holder-alias contract can be driven at naturally aligned addresses. */
typedef struct {
    void *bytes_holder;
    uint32_t length_holder;
    uint8_t pad[64];
} GuardRegion;

static GuardRegion guard_region;

/* A backend context that serves one four-byte match, used for reader holder
 * alias tests without a filesystem. */
typedef struct {
    void *bytes_holder;
    uint32_t length_holder;
    uint8_t pad[64];
    uint32_t query_calls;
    uint32_t read_calls;
} HolderContext;

static HolderContext holder_context;
static const uint8_t holder_body[4] = { (uint8_t)'h', (uint8_t)'o',
                                        (uint8_t)'l', (uint8_t)'d' };

static const uint8_t **holder_out_bytes;
static uint32_t *holder_out_length;
static YanSearchResult holder_result;
static uint32_t holder_offset;
static uint32_t holder_capacity;

/* A protected span with an adjacent pattern buffer immediately after it. */
typedef struct {
    uint32_t region[8];
    uint8_t pattern[8];
} SpanWithAdjacentPattern;

static SpanWithAdjacentPattern adjacent_span;

/* A protected context with the reader output holders immediately after it. */
typedef struct {
    HolderContext context;
    uint32_t out_length;
    void *out_bytes;
} HolderWithAdjacentOutputs;

static HolderWithAdjacentOutputs holder_adjacent;

/* Manual peek state: a match callback that reads one chunk by hand while a
 * chunk callback is also installed. */
static YanSearchResult peek_result;
static uint32_t peek_length;

static YanSearchResult holder_query(void *context, const uint8_t *pattern,
                                    uint32_t pattern_length,
                                    YanSearchMatchFn match, void *match_context)
{
    HolderContext *self = (HolderContext *)context;
    (void)pattern;
    (void)pattern_length;
    ++self->query_calls;
    YanSearchMatch value;
    value.name = "holder.txt";
    value.line_number = 1u;
    value.content_length = 4u;
    (void)match(match_context, &value);
    return YAN_SEARCH_OK;
}

static YanSearchResult holder_read(void *context, uint32_t offset,
                                   uint32_t capacity,
                                   const uint8_t **bytes, uint32_t *length)
{
    HolderContext *self = (HolderContext *)context;
    (void)offset;
    (void)capacity;
    ++self->read_calls;
    *bytes = holder_body;
    *length = 4u;
    return YAN_SEARCH_OK;
}

static YanSearchBackend holder_vtable_for(HolderContext *context, void *borrowed_source,
                                          size_t source_size)
{
    YanSearchBackend backend;
    backend.context = context;
    backend.context_size = sizeof(HolderContext);
    backend.source_context = borrowed_source;
    backend.source_size = source_size;
    backend.query = holder_query;
    backend.read_match = holder_read;
    return backend;
}

static YanSearchBackend holder_vtable(void)
{
    return holder_vtable_for(&holder_context, NULL, 0u);
}

static bool holder_alias_match(void *context, const YanSearchMatch *match)
{
    (void)context;
    (void)match;
    holder_result = yan_search_read_match(&search, holder_offset, holder_capacity,
                                          holder_out_bytes, holder_out_length);
    return false;
}

static bool peek_match(void *context, const YanSearchMatch *match)
{
    (void)context;
    (void)match;
    const uint8_t *bytes = NULL;
    uint32_t length = 0u;
    peek_result = yan_search_read_match(&search, 0u, 6u, &bytes, &length);
    peek_length = length;
    return true;
}

static void fake_malform_setup(FakeMalform malform, uint32_t match_count,
                               uint32_t content_length)
{
    memset(&fake, 0, sizeof fake);
    fake.query_result = YAN_SEARCH_OK;
    fake.read_result = YAN_SEARCH_OK;
    fake.malform = (uint32_t)malform;
    fake.match_count = match_count;
    for (uint32_t i = 0; i < match_count; ++i) {
        fake.names[i] = "malformed.txt";
        fake.lines[i] = i + 1u;
        fake.contents[i] = fake_body_a;
        fake.content_lengths[i] = content_length;
    }
}

/* ------------------------------------------------------------- reader probes */

static bool reader_probe_match(void *context, const YanSearchMatch *match)
{
    (void)context;
    (void)match;
    const uint8_t *bytes = NULL;
    uint32_t length = 0u;
    const uint8_t *alias_bytes = NULL;
    switch (reader_probe_kind) {
    case READER_PROBE_OK:
        reader_probe_result =
            yan_search_read_match(&search, 0u, 4u, &bytes, &length);
        reader_probe_length = length;
        break;
    case READER_PROBE_NULL_BYTES:
        reader_probe_result = yan_search_read_match(&search, 0u, 4u, NULL, &length);
        break;
    case READER_PROBE_NULL_LENGTH:
        reader_probe_result =
            yan_search_read_match(&search, 0u, 4u, &bytes, NULL);
        break;
    case READER_PROBE_ALIAS_SEARCH:
        reader_probe_result = yan_search_read_match(
            &search, 0u, 4u, (const uint8_t **)(void *)&search, &length);
        break;
    case READER_PROBE_ALIAS_OUTPUTS:
        reader_probe_result = yan_search_read_match(
            &search, 0u, sizeof(uint32_t), &alias_bytes,
            (uint32_t *)(void *)&alias_bytes);
        break;
    case READER_PROBE_OFFSET_PAST_END:
        reader_probe_result =
            yan_search_read_match(&search, 5u, 4u, &bytes, &length);
        break;
    case READER_PROBE_OFFSET_AT_END:
        reader_probe_result =
            yan_search_read_match(&search, 4u, 4u, &bytes, &length);
        reader_probe_length = length;
        break;
    case READER_PROBE_CAPACITY_ZERO:
        reader_probe_result =
            yan_search_read_match(&search, 0u, 0u, &bytes, &length);
        reader_probe_length = length;
        break;
    case READER_PROBE_ADDRESS_OVERFLOW:
        reader_probe_result = yan_search_read_match(
            &search, 0u, 4u, (const uint8_t **)(uintptr_t)(UINTPTR_MAX - 1u),
            &length);
        break;
    case READER_PROBE_NONE:
    default:
        break;
    }
    return false;
}

static bool reader_probe_chunk(void *context, uint32_t offset,
                               const uint8_t *bytes, uint32_t length)
{
    (void)context;
    (void)offset;
    (void)bytes;
    (void)length;
    const uint8_t *got = NULL;
    uint32_t got_length = 0u;
    reader_probe_io_before = *reader_probe_io;
    reader_probe_result = yan_search_read_match(&search, 0u, 4u, &got, &got_length);
    reader_probe_io_after = *reader_probe_io;
    return false;
}

static YanSearchResult run_reader_probe(void)
{
    YanSearchSink sink;
    sink.context = NULL;
    sink.match = reader_probe_match;
    sink.chunk = NULL;
    return yan_search_query(&search, (const uint8_t *)"x", 1u, sink);
}

/* Reentrancy probes driven from a real linear match callback. */
static bool real_reentry_match(void *context, const YanSearchMatch *match)
{
    (void)context;
    (void)match;
    if (reentry_armed) {
        reentry_armed = false;
        YanSearchSink sink;
        sink.context = NULL;
        sink.match = capture_match;
        sink.chunk = NULL;
        reentry_result =
            yan_search_query(&search, (const uint8_t *)"x", 1u, sink);
    }
    return false;
}

static bool real_init_reentry_match(void *context, const YanSearchMatch *match)
{
    (void)context;
    (void)match;
    reentry_result =
        yan_search_init(&search, yan_search_linear_backend(&linear));
    return false;
}

/* ------------------------------------------------------- facade init/guards */

static void search_init_rejects_null_search(void)
{
    YanSearchBackend backend = fake_vtable();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_INVALID, yan_search_init(NULL, backend),
        "yan_search_init must reject a null Search pointer");
}

static void search_init_rejects_an_incomplete_backend(void)
{
    YanSearchBackend backend = fake_vtable();
    backend.query = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_INVALID, yan_search_init(&search, backend),
        "a backend without a query callback must be rejected");
    backend = fake_vtable();
    backend.read_match = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_INVALID, yan_search_init(&search, backend),
        "a backend without a read_match callback must be rejected");
    backend.query = NULL;
    backend.read_match = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_INVALID, yan_search_init(&search, backend),
        "a backend without either callback must be rejected");
}

static void search_init_accepts_a_well_formed_backend_and_reinitializes_idle(void)
{
    YanSearchBackend backend = fake_vtable();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK, yan_search_init(&search, backend),
        "a well formed backend must initialize the Search");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK, yan_search_init(&search, backend),
        "an idle Search may be re-initialized for a backend swap");
}

static void search_query_before_init_is_invalid_without_a_backend_call(void)
{
    capture_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_INVALID, run_query((const uint8_t *)"x", 1u),
        "a zero-initialized Search must not run a query");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, fake.query_calls,
        "the backend must not be reached before initialization");
}

static void search_query_rejects_a_null_pattern(void)
{
    fake_install();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_INVALID, run_query(NULL, 3u),
        "a null pattern pointer must be rejected");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, fake.query_calls,
        "an invalid pattern must not reach the backend");
}

static void search_query_rejects_an_empty_pattern(void)
{
    fake_install();
    const uint8_t byte = (uint8_t)'x';
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_INVALID, yan_search_query(&search, &byte, 0u, capture_sink()),
        "an empty pattern must be rejected");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, fake.query_calls,
        "an empty pattern must not reach the backend");
}

static void search_query_accepts_1023_bytes_and_rejects_1024(void)
{
    static uint8_t pattern[1024];
    memset(pattern, (int)'a', sizeof pattern);
    fake_install();
    fake.query_result = YAN_SEARCH_OK;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK, yan_search_query(&search, pattern, 1023u, capture_sink()),
        "1023 bytes is the largest accepted pattern");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        1u, fake.query_calls,
        "a pattern at the length limit must reach the backend");
    fake.query_calls = 0u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_INVALID, yan_search_query(&search, pattern, 1024u, capture_sink()),
        "1024 bytes is one past the pattern limit");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, fake.query_calls,
        "an over-long pattern must not reach the backend");
}

static void search_query_rejects_nul_and_lf_pattern_bytes(void)
{
    static const uint8_t with_nul[3] = { (uint8_t)'a', 0u, (uint8_t)'b' };
    static const uint8_t with_lf[3] = { (uint8_t)'a', (uint8_t)'\n', (uint8_t)'b' };
    fake_install();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_INVALID, yan_search_query(&search, with_nul, 3u, capture_sink()),
        "a NUL byte in the pattern must be rejected");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_INVALID, yan_search_query(&search, with_lf, 3u, capture_sink()),
        "an LF byte in the pattern must be rejected");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, fake.query_calls,
        "rejected pattern bytes must not reach the backend");
}

static void search_query_accepts_tab_cr_and_non_utf8_pattern_bytes(void)
{
    static const uint8_t raw[3] = { 0x09u, 0x0Du, 0xFFu };
    fake_install();
    fake.query_result = YAN_SEARCH_OK;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK, yan_search_query(&search, raw, 3u, capture_sink()),
        "TAB, bare CR and non-UTF8 bytes are legal raw pattern bytes");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        1u, fake.query_calls,
        "a legal raw pattern must reach the backend");
}

static void search_query_rejects_a_null_match_callback(void)
{
    fake_install();
    YanSearchSink sink;
    sink.context = NULL;
    sink.match = NULL;
    sink.chunk = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_INVALID,
        yan_search_query(&search, (const uint8_t *)"x", 1u, sink),
        "a query without a match callback must be rejected");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, fake.query_calls,
        "a missing match callback must not reach the backend");
}

static void search_query_rejects_a_pattern_range_that_leaves_uintptr(void)
{
    fake_install();
    const uint8_t *bad = (const uint8_t *)(uintptr_t)(UINTPTR_MAX - 1u);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_INVALID, yan_search_query(&search, bad, 8u, capture_sink()),
        "a pattern range leaving uintptr must be rejected without a dereference");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, fake.query_calls,
        "an overflowing pattern range must not reach the backend");
}

static void search_query_rejects_a_pattern_overlapping_the_search(void)
{
    fake_install();
    const uint8_t *aliased = (const uint8_t *)(const void *)&search;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_INVALID,
        yan_search_query(&search, aliased, 4u, capture_sink()),
        "a pattern overlapping the Search object must be rejected");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, fake.query_calls,
        "an aliased pattern must not reach the backend");
}

static void search_query_reentry_returns_busy(void)
{
    memset(&fake, 0, sizeof fake);
    fake.query_result = YAN_SEARCH_OK;
    fake.read_result = YAN_SEARCH_OK;
    fake.reentry_search = &search;
    fake.reentry_query = true;
    fake_install();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK, run_query((const uint8_t *)"x", 1u),
        "the outer query must complete");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_BUSY, fake.reentry_result,
        "a reentrant query while the Search is busy must be BUSY");
}

static void search_init_reentry_returns_busy(void)
{
    memset(&fake, 0, sizeof fake);
    fake.query_result = YAN_SEARCH_OK;
    fake.read_result = YAN_SEARCH_OK;
    fake.reentry_search = &search;
    fake.reentry_init = true;
    fake_install();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK, run_query((const uint8_t *)"x", 1u),
        "the outer query must complete");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_BUSY, fake.reentry_result,
        "re-initializing a busy Search must be BUSY");
}

static void search_reinitializes_with_a_swapped_backend(void)
{
    static FakeBackend other;
    memset(&other, 0, sizeof other);
    other.query_result = YAN_SEARCH_OK;
    other.read_result = YAN_SEARCH_OK;

    memset(&fake, 0, sizeof fake);
    fake.query_result = YAN_SEARCH_OK;
    fake.read_result = YAN_SEARCH_OK;
    fake_install();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK, run_query((const uint8_t *)"x", 1u),
        "the first backend must run the query");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, fake.query_calls,
                                     "the first backend must be called once");

    YanSearchBackend backend;
    backend.context = &other;
    backend.context_size = sizeof other;
    backend.source_context = NULL;
    backend.source_size = 0u;
    backend.query = fake_query;
    backend.read_match = fake_read_match;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK, yan_search_init(&search, backend),
        "swapping an idle backend must succeed");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK, run_query((const uint8_t *)"y", 1u),
        "the replacement backend must run the query");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        1u, other.query_calls,
        "the replacement backend must receive the next query");
}

/* ------------------------------------------------------- reader period guards */

static void search_read_match_outside_a_callback_is_invalid(void)
{
    fake_install();
    const uint8_t *bytes = NULL;
    uint32_t length = 0u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_INVALID, yan_search_read_match(&search, 0u, 4u, &bytes, &length),
        "the match reader is only valid during a match callback");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, fake.read_calls,
        "a reader call outside the callback must perform no backend read");
}

static void search_read_match_rejects_null_output_holders(void)
{
    fake_single_match("fake.txt", 1u, fake_body_a, 4u);
    fake_install();
    reader_probe_kind = READER_PROBE_NULL_BYTES;
    reader_probe_result = YAN_SEARCH_OK;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_STOPPED, run_reader_probe(),
                                  "the probe callback stops the query");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_INVALID, reader_probe_result,
                                  "a null bytes holder is INVALID");
    reader_probe_kind = READER_PROBE_NULL_LENGTH;
    reader_probe_result = YAN_SEARCH_OK;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_STOPPED, run_reader_probe(),
                                  "the probe callback stops the query");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_INVALID, reader_probe_result,
                                  "a null length holder is INVALID");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, fake.read_calls,
                                     "invalid output holders must not read");
}

static void search_read_match_rejects_outputs_aliasing_the_search(void)
{
    fake_single_match("fake.txt", 1u, fake_body_a, 4u);
    fake_install();
    reader_probe_kind = READER_PROBE_ALIAS_SEARCH;
    reader_probe_result = YAN_SEARCH_OK;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_STOPPED, run_reader_probe(),
                                  "the probe callback stops the query");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_INVALID, reader_probe_result,
        "an output holder inside the Search is rejected");
    reader_probe_kind = READER_PROBE_ALIAS_OUTPUTS;
    reader_probe_result = YAN_SEARCH_OK;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_STOPPED, run_reader_probe(),
                                  "the probe callback stops the query");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_INVALID, reader_probe_result,
        "the two output holders must not overlap each other");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, fake.read_calls,
                                     "aliased outputs must not read");
}

static void search_read_match_rejects_an_offset_past_the_content(void)
{
    fake_single_match("fake.txt", 1u, fake_body_a, 4u);
    fake_install();
    reader_probe_kind = READER_PROBE_OFFSET_PAST_END;
    reader_probe_result = YAN_SEARCH_OK;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_STOPPED, run_reader_probe(),
                                  "the probe callback stops the query");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_INVALID, reader_probe_result,
                                  "an offset past content_length is rejected");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, fake.read_calls,
        "an out-of-range offset must not perform a backend read");
}

static void search_read_match_rejects_an_output_address_that_overflows(void)
{
    fake_single_match("fake.txt", 1u, fake_body_a, 4u);
    fake_install();
    reader_probe_kind = READER_PROBE_ADDRESS_OVERFLOW;
    reader_probe_result = YAN_SEARCH_OK;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_STOPPED, run_reader_probe(),
                                  "the probe callback stops the query");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_INVALID, reader_probe_result,
        "an output pointer whose range leaves uintptr is rejected without a write");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, fake.read_calls,
                                     "an overflowing output must not read");
}

static void search_read_match_reads_the_current_content_range(void)
{
    fake_single_match("fake.txt", 1u, fake_body_a, 4u);
    fake_install();
    reader_probe_kind = READER_PROBE_OK;
    reader_probe_result = YAN_SEARCH_OK;
    reader_probe_length = 0u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_STOPPED, run_reader_probe(),
                                  "the probe callback stops the query");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_OK, reader_probe_result,
                                  "the current match content is readable");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(4u, reader_probe_length,
                                     "the reader returns the requested chunk");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, fake.read_calls,
                                     "one reader call performs one backend read");
}

static void search_read_match_zero_capacity_and_end_offset_are_empty_reads(void)
{
    fake_single_match("fake.txt", 1u, fake_body_a, 4u);
    fake_install();
    reader_probe_kind = READER_PROBE_CAPACITY_ZERO;
    reader_probe_result = YAN_SEARCH_OK;
    reader_probe_length = 1u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_STOPPED, run_reader_probe(),
                                  "the probe callback stops the query");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_OK, reader_probe_result,
                                  "a zero capacity read is a successful empty read");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, reader_probe_length,
                                     "a zero capacity read returns no bytes");
    reader_probe_kind = READER_PROBE_OFFSET_AT_END;
    reader_probe_result = YAN_SEARCH_OK;
    reader_probe_length = 1u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_STOPPED, run_reader_probe(),
                                  "the probe callback stops the query");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_OK, reader_probe_result,
                                  "an offset at the end is a successful empty read");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, reader_probe_length,
                                     "an offset at the end returns no bytes");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, fake.read_calls,
        "empty reads must not perform a backend read");
}

/* ------------------------------------------------- fake backend behaviour */

static void fake_backend_reports_value_matches_in_order(void)
{
    memset(&fake, 0, sizeof fake);
    fake.query_result = YAN_SEARCH_OK;
    fake.read_result = YAN_SEARCH_OK;
    fake.match_count = 3u;
    fake.names[0] = "a.txt";
    fake.lines[0] = 1u;
    fake.contents[0] = fake_body_a;
    fake.content_lengths[0] = 4u;
    fake.names[1] = "b.txt";
    fake.lines[1] = 7u;
    fake.contents[1] = fake_body_b;
    fake.content_lengths[1] = 4u;
    fake.names[2] = "c.txt";
    fake.lines[2] = 2u;
    fake.contents[2] = fake_body_a;
    fake.content_lengths[2] = 4u;
    fake_install();
    capture_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_OK, run_query((const uint8_t *)"x", 1u),
                                  "a fake backend query can succeed");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(3u, capture.matches,
                                     "every fake match must be reported");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("a.txt", capture.names[0],
                                     "the first match keeps slot order");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("b.txt", capture.names[1],
                                     "the second match keeps slot order");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("c.txt", capture.names[2],
                                     "the third match keeps slot order");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, capture.lines[0],
                                     "the first line number is exposed as a value");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(7u, capture.lines[1],
                                     "the second line number is exposed as a value");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(4u, capture.lengths[2],
                                     "the content length is exposed as a value");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(12u, capture.content_length,
                                     "the concatenated raw content follows match order");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(fake_body_a, capture.content, 4u,
                                     "the first raw content chunk is delivered");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(fake_body_b, capture.content + 4u, 4u,
                                     "the second raw content chunk is delivered");
}

static void match_callback_false_ends_with_stopped(void)
{
    memset(&fake, 0, sizeof fake);
    fake.query_result = YAN_SEARCH_OK;
    fake.read_result = YAN_SEARCH_OK;
    fake.match_count = 3u;
    for (uint32_t i = 0; i < 3u; ++i) {
        fake.names[i] = "fake.txt";
        fake.lines[i] = i + 1u;
        fake.contents[i] = fake_body_a;
        fake.content_lengths[i] = 4u;
    }
    fake_install();
    capture_reset();
    capture.stop_after = 2u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_STOPPED, run_query((const uint8_t *)"x", 1u),
                                  "a match callback stop is a healthy STOPPED");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2u, capture.matches,
                                     "no match is reported after the stopping callback");
}

static void chunk_callback_false_ends_with_stopped(void)
{
    memset(&fake, 0, sizeof fake);
    fake.query_result = YAN_SEARCH_OK;
    fake.read_result = YAN_SEARCH_OK;
    fake.read_chunk = 2u;
    fake.match_count = 2u;
    for (uint32_t i = 0; i < 2u; ++i) {
        fake.names[i] = "fake.txt";
        fake.lines[i] = i + 1u;
        fake.contents[i] = fake_body_a;
        fake.content_lengths[i] = 4u;
    }
    fake_install();
    capture_reset();
    YanSearchSink sink;
    sink.context = NULL;
    sink.match = capture_match;
    sink.chunk = capture_stop_first_chunk;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_STOPPED,
        yan_search_query(&search, (const uint8_t *)"x", 1u, sink),
        "a chunk callback stop is a healthy STOPPED");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, capture.matches,
                                     "the stopping chunk ends the first match only");
}

static void reader_error_wins_over_a_callback_stop(void)
{
    fake_single_match("fake.txt", 1u, fake_body_a, 4u);
    fake.read_fail_at = 1u;
    fake.read_fail_code = YAN_SEARCH_IO;
    fake_install();
    capture_reset();
    /* The match callback pulls the content itself, hits the reader error and
     * then returns false. The query must report the source error, not STOPPED,
     * because the error outranks the callback stop that follows it. */
    capture.manual_read = true;
    capture.ignore_reader_error = false;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_IO, run_query((const uint8_t *)"x", 1u),
        "a sticky reader error outranks the callback stop that follows it");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_IO, capture.reader_result,
        "the manual reader observes the source error before it stops");
}

static void an_ignored_reader_error_still_reports_the_source_error(void)
{
    fake_single_match("fake.txt", 1u, fake_body_a, 4u);
    fake.query_result = YAN_SEARCH_OK;
    fake.read_fail_at = 1u;
    fake.read_fail_code = YAN_SEARCH_IO;
    fake_install();
    capture_reset();
    capture.manual_read = true;
    capture.ignore_reader_error = true;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_IO, run_query((const uint8_t *)"x", 1u),
        "ignoring the reader result must not turn a source error into success");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_IO, capture.reader_result,
                                  "the manual reader observes the source error");
}

static void fake_backend_source_result_is_propagated(void)
{
    memset(&fake, 0, sizeof fake);
    fake.query_result = YAN_SEARCH_CORRUPT;
    fake.read_result = YAN_SEARCH_OK;
    fake_install();
    capture_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_CORRUPT, run_query((const uint8_t *)"x", 1u),
        "the backend source result reaches the caller unchanged");
}

static void source_error_classes_are_distinct_and_not_success(void)
{
    static const YanSearchResult classes[] = {
        YAN_SEARCH_NOT_MOUNTED, YAN_SEARCH_FAULTED, YAN_SEARCH_IO,
        YAN_SEARCH_PROTOCOL, YAN_SEARCH_CORRUPT, YAN_SEARCH_UNSUPPORTED
    };
    const uint32_t count = (uint32_t)(sizeof classes / sizeof classes[0]);
    for (uint32_t i = 0; i < count; ++i) {
        TEST_ASSERT_TRUE_MESSAGE(classes[i] != YAN_SEARCH_OK,
                                 "a source error class is never OK");
        TEST_ASSERT_TRUE_MESSAGE(classes[i] != YAN_SEARCH_STOPPED,
                                 "a source error class is never STOPPED");
        TEST_ASSERT_TRUE_MESSAGE(classes[i] != YAN_SEARCH_INVALID,
                                 "a source error class is not a parameter error");
        for (uint32_t j = i + 1u; j < count; ++j) {
            TEST_ASSERT_TRUE_MESSAGE(classes[i] != classes[j],
                                     "each source error class has a distinct value");
        }
    }
}

static void reader_reentry_from_a_chunk_is_busy_without_extra_reads(void)
{
    fake_single_match("fake.txt", 1u, fake_body_a, 4u);
    fake_install();
    capture_reset();
    reader_probe_result = YAN_SEARCH_OK;
    reader_probe_io = &fake.read_calls;
    reader_probe_io_before = 0u;
    reader_probe_io_after = 0u;
    YanSearchSink sink;
    sink.context = NULL;
    sink.match = capture_match;
    sink.chunk = reader_probe_chunk;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_STOPPED,
        yan_search_query(&search, (const uint8_t *)"x", 1u, sink),
        "a stopping chunk callback ends the query as STOPPED");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_BUSY, reader_probe_result,
        "a reader call from inside a chunk callback is BUSY");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        reader_probe_io_before, reader_probe_io_after,
        "the BUSY reader reentry performs no extra backend read");
}

/* --------------------------------------------------- real linear: empty sets */

static void real_search_empty_directory_yields_zero_matches(void)
{
    real_setup();
    capture_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK, run_query((const uint8_t *)"x", 1u),
        "an empty directory is a successful zero-match query");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, capture.matches,
                                     "no file can produce a match");
}

static void real_search_empty_file_contributes_no_lines(void)
{
    real_setup();
    create_file("empty.txt", NULL, 0u);
    capture_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK, run_query((const uint8_t *)"x", 1u),
        "an empty file is scanned successfully");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, capture.matches,
                                     "an empty file has no line to match");
}

static void real_search_no_hits_is_ok_with_zero_matches(void)
{
    real_setup();
    create_file("note.txt", (const uint8_t *)"hello world", 11u);
    capture_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK, run_query((const uint8_t *)"zzz", 3u),
        "a miss is a successful zero-match query");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, capture.matches,
                                     "a miss produces no match callback");
}

/* ------------------------------------------------ real linear: ordering/lines */

static void real_search_reports_each_matched_line_once_in_slot_order(void)
{
    real_setup();
    create_file("a.txt", (const uint8_t *)"token token\nplain\n", 18u);
    create_file("b.txt", (const uint8_t *)"plain\n", 6u);
    create_file("c.txt", (const uint8_t *)"token\n", 6u);
    capture_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK, run_query((const uint8_t *)"token", 5u),
        "the real query must succeed");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        2u, capture.matches,
        "a line with two hits still produces exactly one match");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("a.txt", capture.names[0],
                                     "results follow physical slot order");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, capture.lines[0],
                                     "the first hit is on line one");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("c.txt", capture.names[1],
                                     "results keep following slot order");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, capture.lines[1],
                                     "the second file hit is on its line one");
}

static void real_search_orders_files_across_a_slot_hole(void)
{
    real_setup();
    create_file("first.txt", (const uint8_t *)"hit\n", 4u);
    create_file("middle.txt", (const uint8_t *)"hit\n", 4u);
    create_file("last.txt", (const uint8_t *)"hit\n", 4u);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK, yan_fs_remove(&fs, "middle.txt"),
                                  "the fixture must empty the middle slot");
    capture_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK, run_query((const uint8_t *)"hit", 3u),
        "a directory hole does not end the scan");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2u, capture.matches,
                                     "only the two surviving files match");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("first.txt", capture.names[0],
                                     "the low slot is reported first");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("last.txt", capture.names[1],
                                     "the empty slot is skipped without a result");
}

static void real_search_matches_a_word_across_the_scan_chunk_boundary(void)
{
    real_setup();
    memset(source, (int)'a', 4096u + 16u);
    memcpy(source + 4093u, "NEEDLE", 6u);
    create_file("split.txt", source, 4096u + 16u);
    capture_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK, run_query((const uint8_t *)"NEEDLE", 6u),
        "the scan must succeed");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        1u, capture.matches,
        "a word straddling the 4096 boundary is still found");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, capture.lines[0],
                                     "the single line is line one");
}

static void real_search_matches_chinese_bytes_across_the_boundary(void)
{
    real_setup();
    static const uint8_t chinese[3] = { 0xE4u, 0xB8u, 0xADu };
    memset(source, (int)'x', 4096u + 8u);
    memcpy(source + 4095u, chinese, 3u);
    create_file("cn.txt", source, 4096u + 8u);
    capture_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK, run_query(chinese, 3u),
        "a multi-byte pattern across the boundary must match");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, capture.matches,
                                     "the Chinese pattern matches once");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(4096u + 8u, capture.lengths[0],
                                     "the whole line length is reported");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(chinese, capture.content + 4095u, 3u,
                                     "the raw multi-byte bytes are delivered intact");
}

static void real_search_repeated_prefix_does_not_miss_a_hit(void)
{
    real_setup();
    create_file("kmp.txt", (const uint8_t *)"AAAAAAAB\n", 9u);
    capture_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK, run_query((const uint8_t *)"AAAB", 4u),
        "a repeated-prefix pattern must be scanned correctly");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        1u, capture.matches,
        "AAAB occurs exactly once in AAAAAAAB");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, capture.lines[0],
                                     "the match is on line one");
}

static void real_search_does_not_match_across_a_line_break(void)
{
    real_setup();
    create_file("lines.txt", (const uint8_t *)"AB\nCD", 5u);
    capture_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK, run_query((const uint8_t *)"BC", 2u),
        "the query succeeds with no hit across the break");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, capture.matches,
        "bytes split by LF must not form a match across lines");
}

static void real_search_splits_lf_and_crlf_without_terminator_content(void)
{
    real_setup();
    create_file("mixed.txt", (const uint8_t *)"a\r\nb\nc\r\nd", 9u);
    capture_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK, run_query((const uint8_t *)"b", 1u),
        "the mixed-ending file is scanned");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, capture.matches,
                                     "only line two holds the hit");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2u, capture.lines[0],
                                     "CRLF advances from line one to line two");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, capture.lengths[0],
                                     "the CRLF terminator is not part of the content");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE("b", capture.content, 1u,
                                     "the matched content is the byte b");
}

static void real_search_treats_bare_cr_as_content(void)
{
    real_setup();
    create_file("cr.txt", (const uint8_t *)"a\rb\n", 4u);
    static const uint8_t cr = 0x0Du;
    capture_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK, run_query(&cr, 1u),
        "the API may search for a bare CR byte");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, capture.matches,
                                     "a bare CR is ordinary content");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, capture.lines[0],
                                     "the bare CR stays on line one");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(3u, capture.lengths[0],
                                     "the line content is a\\rb without the LF");
}

static void real_search_counts_an_unterminated_final_line(void)
{
    real_setup();
    create_file("tail.txt", (const uint8_t *)"A\nB", 3u);
    capture_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK, run_query((const uint8_t *)"B", 1u),
        "the unterminated tail is scanned");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, capture.matches,
                                     "the final line can match");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2u, capture.lines[0],
                                     "the unterminated final line is line two");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, capture.lengths[0],
                                     "the unterminated line content is one byte");
}

static void real_search_final_lf_adds_no_phantom_line(void)
{
    real_setup();
    create_file("one.txt", (const uint8_t *)"A\n", 2u);
    capture_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK, run_query((const uint8_t *)"A", 1u),
        "the terminated file is scanned");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, capture.matches,
                                     "the final terminator adds no empty line");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, capture.lines[0],
                                     "the hit is on line one");
}

static void real_search_empty_lines_still_advance_the_line_number(void)
{
    real_setup();
    create_file("gaps.txt", (const uint8_t *)"\n\nA\n", 4u);
    capture_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK, run_query((const uint8_t *)"A", 1u),
        "the file with empty lines is scanned");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, capture.matches,
                                     "only the byte A matches");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        3u, capture.lines[0],
        "two empty lines precede the hit, so its number is three");
}

/* ---------------------------------------------------- real linear: long lines */

static void real_search_returns_a_complete_100kib_line(void)
{
    real_setup();
    memset(source, (int)'x', TEST_LINE_BYTES);
    memcpy(source + 50000u, "NEEDLE", 6u);
    create_file("big.txt", source, TEST_LINE_BYTES);
    capture_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK, run_query((const uint8_t *)"NEEDLE", 6u),
        "the long line query must succeed");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, capture.matches,
                                     "the long single line matches once");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        TEST_LINE_BYTES, capture.lengths[0],
        "the reported content length is the full 100 KiB line");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        TEST_LINE_BYTES, capture.content_length,
        "the full 100 KiB line is delivered, not truncated to the editor limit");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(
        source, capture.content, TEST_LINE_BYTES,
        "the captured raw content equals the stored long line");
}

static void real_search_delivers_bounded_chunks_not_a_line_pointer(void)
{
    real_setup();
    memset(source, (int)'x', TEST_LINE_BYTES);
    memcpy(source + 1024u, "NEEDLE", 6u);
    create_file("big.txt", source, TEST_LINE_BYTES);
    capture_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK, run_query((const uint8_t *)"NEEDLE", 6u),
        "the long line query must succeed");
    TEST_ASSERT_TRUE_MESSAGE(capture.chunks > 1u,
                             "the content is delivered in more than one chunk");
    TEST_ASSERT_TRUE_MESSAGE(
        capture.max_chunk <= YAN_SEARCH_LINEAR_SCAN_SIZE,
        "no delivered chunk exceeds the fixed linear reader buffer");
}

/* ------------------------------------------------------ real linear: binary */

static void real_search_skips_a_file_at_its_first_nul(void)
{
    real_setup();
    static uint8_t binary[16];
    memset(binary, 0, sizeof binary);
    memcpy(binary, "hit\n", 4u);
    memcpy(binary + 5u, "tail", 4u);
    create_file("bin.txt", binary, 9u);
    capture_reset();
    device.reads = 0u;
    device.trace_enabled = true;
    device.trace_count = 0u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK, run_query((const uint8_t *)"hit", 3u),
        "a binary file is skipped without an ordinary result");
    device.trace_enabled = false;
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, capture.matches,
        "a hit before the first NUL still produces no result");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        1u, device.reads,
        "preflight stops reading after the block that holds the first NUL");
}

static void real_search_skips_a_file_with_a_late_nul(void)
{
    real_setup();
    memset(source, (int)'a', 5000u);
    memcpy(source + 100u, "hit", 3u);
    source[4999] = 0u;
    create_file("late.txt", source, 5000u);
    capture_reset();
    device.reads = 0u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK, run_query((const uint8_t *)"hit", 3u),
        "a late NUL still skips the whole file");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, capture.matches,
        "a NUL anywhere in the file means zero matches for that file");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        2u, device.reads,
        "the full preflight reads both blocks before it can decide to skip");
}

static void real_search_continues_to_the_next_file_after_binary(void)
{
    real_setup();
    static const uint8_t binary[8] = { (uint8_t)'h', (uint8_t)'i',
                                       (uint8_t)'t', 0u,        (uint8_t)'h',
                                       (uint8_t)'i', (uint8_t)'t', 0u };
    create_file("bin.txt", binary, 8u);
    create_file("text.txt", (const uint8_t *)"hit\n", 4u);
    capture_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK, run_query((const uint8_t *)"hit", 3u),
        "the query continues to the text file");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, capture.matches,
                                     "only the text file contributes a match");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("text.txt", capture.names[0],
                                     "the surviving match names the text file");
}

/* ------------------------------------------------- real linear: trace and I/O */

static void real_search_crlf_split_exactly_at_the_chunk_boundary(void)
{
    real_setup();
    memset(source, (int)'a', 4095u);
    source[4095] = (uint8_t)'\r';
    source[4096] = (uint8_t)'\n';
    source[4097] = (uint8_t)'b';
    source[4098] = (uint8_t)'\n';
    create_file("boundary.txt", source, 4099u);
    capture_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK, run_query((const uint8_t *)"b", 1u),
        "a CRLF split across the scan chunk boundary is one terminator");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, capture.matches,
                                     "only the second line matches");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2u, capture.lines[0],
                                     "the split CR belongs to line one, not line two");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, capture.lengths[0],
                                     "the matched line is the single byte b");
}

static void real_search_nul_short_circuit_suppresses_a_later_read_fault(void)
{
    real_setup();
    memset(source, (int)'a', 5000u);
    source[10] = 0u;
    create_file("nul.txt", source, 5000u);
    capture_reset();
    device.reads = 0u;
    device.read_fail_at = 2u; /* a second preflight read would fail */
    device.read_fail_code = YAN_FS_IO_ERROR;
    YanSearchResult result = run_query((const uint8_t *)"a", 1u);
    device.read_fail_at = 0u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK, result,
        "a first-block NUL skips the file before the failing second read");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, capture.matches,
                                     "the skipped binary file contributes no match");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        1u, device.reads,
        "the NUL short circuit stops preflight after the first block");
}

static void real_search_reader_mid_phase_failure_is_sticky(void)
{
    real_setup();
    memset(source, (int)'a', 5000u);
    memcpy(source, "NEEDLE", 6u);
    create_file("mid.txt", source, 5000u);
    capture_reset();
    device.reads = 0u;
    device.read_fail_at = 6u; /* two preflight + two scan reads pass first */
    device.read_fail_code = YAN_FS_IO_ERROR;
    YanSearchResult result = run_query((const uint8_t *)"NEEDLE", 6u);
    device.read_fail_at = 0u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_IO, result,
        "a failure on the second reader pass is sticky, not a partial success");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        1u, capture.matches,
        "the match was reported before the second reader pass failed");
}

static void real_search_unmounted_source_reports_not_mounted(void)
{
    memset(&device, 0, sizeof device);
    device.capacity_blocks = TEST_DEVICE_BLOCKS;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_FS_OK,
        yan_fs_format_metadata(device.blocks[0], TEST_DEVICE_BLOCKS),
        "the unmounted fixture must format an empty directory");
    memset(&fs, 0, sizeof fs);
    YanFsBlockIo io;
    io.context = &device;
    io.capacity = device_capacity;
    io.read_block = device_read_block;
    io.write_block = device_write_block;
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_init(&fs, io));
    /* Deliberately no yan_fs_mount: the public FS call must be the one that
     * reports the unmounted state. */
    memset(&linear, 0, sizeof linear);
    memset(&search, 0, sizeof search);
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_OK, yan_search_linear_init(&linear, &fs));
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_OK,
                          yan_search_init(&search, yan_search_linear_backend(&linear)));
    capture_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_NOT_MOUNTED, run_query((const uint8_t *)"x", 1u),
        "an unmounted borrowed filesystem reports NOT_MOUNTED through the public call");
}

static void real_search_read_trace_is_preflight_scan_then_reader(void)
{
    real_setup();
    memset(source, (int)'a', 5000u);
    memcpy(source + 2u, "NEEDLE", 6u);
    create_file("trace.txt", source, 5000u);
    static const char expected_kinds[6] = { 'R', 'R', 'R', 'R', 'R', 'R' };
    static const uint32_t expected_lbas[6] = { 1u, 2u, 1u, 2u, 1u, 2u };
    snapshot_medium();
    device.reads = 0u;
    device.writes = 0u;
    device.trace_enabled = true;
    device.trace_count = 0u;
    capture_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK, run_query((const uint8_t *)"NEEDLE", 6u),
        "the traced query must succeed");
    device.trace_enabled = false;
    assert_trace(expected_kinds, expected_lbas, 6u,
                 "preflight and scan stream two blocks each, then the matched line is re-read");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, device.writes,
                                     "a search performs no device writes");
    TEST_ASSERT_TRUE_MESSAGE(medium_unchanged(),
                             "the whole medium is byte-identical after a search");
}

static void real_search_preflight_read_failure_reports_io(void)
{
    real_setup();
    memset(source, (int)'a', 5000u);
    memcpy(source, "NEEDLE", 6u);
    create_file("fault.txt", source, 5000u);
    capture_reset();
    device.reads = 0u;
    device.read_fail_at = 1u;
    device.read_fail_code = YAN_FS_IO_ERROR;
    YanSearchResult result = run_query((const uint8_t *)"NEEDLE", 6u);
    device.read_fail_at = 0u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_IO, result,
        "a preflight read failure is a source IO error, not a zero-match success");
}

static void real_search_scan_read_failure_reports_io(void)
{
    real_setup();
    memset(source, (int)'a', 5000u);
    memcpy(source, "NEEDLE", 6u);
    create_file("fault.txt", source, 5000u);
    capture_reset();
    device.reads = 0u;
    device.read_fail_at = 3u; /* two preflight reads pass first */
    device.read_fail_code = YAN_FS_IO_ERROR;
    YanSearchResult result = run_query((const uint8_t *)"NEEDLE", 6u);
    device.read_fail_at = 0u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_IO, result,
        "a scan read failure is a source IO error");
}

static void real_search_match_reader_failure_is_not_success(void)
{
    real_setup();
    memset(source, (int)'a', 5000u);
    memcpy(source, "NEEDLE", 6u);
    create_file("fault.txt", source, 5000u);
    capture_reset();
    device.reads = 0u;
    device.read_fail_at = 5u; /* two preflight + two scan reads pass first */
    device.read_fail_code = YAN_FS_IO_ERROR;
    YanSearchResult result = run_query((const uint8_t *)"NEEDLE", 6u);
    device.read_fail_at = 0u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_IO, result,
        "a matched-range read failure must never be reported as OK");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        1u, capture.matches,
        "the match was reported before its content read failed");
}

static void real_search_protocol_read_failure_maps_to_protocol(void)
{
    real_setup();
    create_file("fault.txt", (const uint8_t *)"NEEDLE\n", 7u);
    capture_reset();
    device.reads = 0u;
    device.read_fail_at = 1u;
    device.read_fail_code = YAN_FS_IO_PROTOCOL;
    YanSearchResult result = run_query((const uint8_t *)"NEEDLE", 6u);
    device.read_fail_at = 0u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_PROTOCOL, result,
        "a protocol device failure maps to the unified PROTOCOL class");
}

static void real_search_unknown_read_status_maps_to_protocol(void)
{
    real_setup();
    create_file("fault.txt", (const uint8_t *)"NEEDLE\n", 7u);
    capture_reset();
    device.reads = 0u;
    device.read_fail_at = 1u;
    device.read_fail_code = (YanFsIoResult)77;
    YanSearchResult result = run_query((const uint8_t *)"NEEDLE", 6u);
    device.read_fail_at = 0u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_PROTOCOL, result,
        "an unknown device status is a protocol violation, not a normal result");
}

static void real_search_reader_error_outranks_an_ignored_result(void)
{
    real_setup();
    memset(source, (int)'a', 5000u);
    memcpy(source, "NEEDLE", 6u);
    create_file("fault.txt", source, 5000u);
    capture_reset();
    device.reads = 0u;
    device.read_fail_at = 5u;
    device.read_fail_code = YAN_FS_IO_ERROR;
    capture.manual_read = true;
    capture.ignore_reader_error = true;
    YanSearchResult result = run_query((const uint8_t *)"NEEDLE", 6u);
    device.read_fail_at = 0u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_IO, result,
        "a caller that ignores its reader error cannot make the query succeed");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_IO, capture.reader_result,
                                  "the manual reader sees the sticky source error");
}

static void real_search_callback_stop_reports_stopped(void)
{
    real_setup();
    create_file("a.txt", (const uint8_t *)"hit\n", 4u);
    create_file("b.txt", (const uint8_t *)"hit\n", 4u);
    capture_reset();
    capture.stop_after = 1u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_STOPPED, run_query((const uint8_t *)"hit", 3u),
        "a clean callback stop reports STOPPED");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, capture.matches,
                                     "no further match is reported after the stop");
}

static void real_search_query_reentry_is_busy_without_extra_reads(void)
{
    real_setup();
    create_file("a.txt", (const uint8_t *)"hit\n", 4u);
    create_file("b.txt", (const uint8_t *)"hit\n", 4u);
    capture_reset();
    reentry_armed = true;
    reentry_result = YAN_SEARCH_OK;
    YanSearchSink sink;
    sink.context = NULL;
    sink.match = real_reentry_match;
    sink.chunk = NULL;
    device.reads = 0u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_STOPPED,
        yan_search_query(&search, (const uint8_t *)"hit", 3u, sink),
        "the outer query stops after the first match");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_BUSY, reentry_result,
        "a reentrant query from inside the callback is BUSY");
}

static void real_search_init_reentry_is_busy(void)
{
    real_setup();
    create_file("a.txt", (const uint8_t *)"hit\n", 4u);
    capture_reset();
    reentry_result = YAN_SEARCH_OK;
    YanSearchSink sink;
    sink.context = NULL;
    sink.match = real_init_reentry_match;
    sink.chunk = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_STOPPED,
        yan_search_query(&search, (const uint8_t *)"hit", 3u, sink),
        "the outer query stops after the first match");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_BUSY, reentry_result,
        "re-initializing from inside a query is BUSY");
}

static void real_search_reader_reentry_from_chunk_is_busy(void)
{
    real_setup();
    memset(source, (int)'a', 5000u);
    memcpy(source, "NEEDLE", 6u);
    create_file("trace.txt", source, 5000u);
    capture_reset();
    reader_probe_result = YAN_SEARCH_OK;
    reader_probe_io = &device.reads;
    reader_probe_io_before = 0u;
    reader_probe_io_after = 0u;
    YanSearchSink sink;
    sink.context = NULL;
    sink.match = capture_match;
    sink.chunk = reader_probe_chunk;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_STOPPED,
        yan_search_query(&search, (const uint8_t *)"NEEDLE", 6u, sink),
        "the stopping chunk ends the query as STOPPED");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_BUSY, reader_probe_result,
        "a reader call from inside a chunk callback is BUSY");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        reader_probe_io_before, reader_probe_io_after,
        "the BUSY reader reentry performs no extra device read");
}

/* ----------------------------------------- protected-span contract: init */

static void search_init_rejects_backend_context_overlapping_search(void)
{
    memset(&alias_union, 0, sizeof alias_union);
    YanSearchBackend backend =
        fake_vtable_for(&alias_union, sizeof alias_union, NULL, 0u);
    static uint8_t before[sizeof(AliasUnion)];
    memcpy(before, &alias_union, sizeof before);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_INVALID,
        yan_search_init(&alias_union.search, backend),
        "a backend context overlapping the Search must be rejected");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(
        before, &alias_union, sizeof before,
        "a rejected init must leave the aliased storage byte-identical");
}

static void search_init_rejects_borrowed_source_overlapping_search(void)
{
    memset(&alias_union, 0, sizeof alias_union);
    YanSearchBackend backend =
        fake_vtable_for(&fake, sizeof fake, &alias_union.search, sizeof(YanSearch));
    static uint8_t before[sizeof(AliasUnion)];
    memcpy(before, &alias_union, sizeof before);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_INVALID,
        yan_search_init(&alias_union.search, backend),
        "a borrowed source overlapping the Search must be rejected");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(
        before, &alias_union, sizeof before,
        "a rejected init must leave the aliased storage byte-identical");
}

static void search_init_rejects_context_and_source_overlapping_each_other(void)
{
    memset(span_words, 0, sizeof span_words);
    YanSearchBackend backend =
        fake_vtable_for(span_words, 8u, (uint8_t *)span_words + 4u, 8u);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_INVALID, yan_search_init(&search, backend),
        "backend context and source spans must not partially overlap");
}

static void search_init_accepts_adjacent_protected_spans(void)
{
    memset(span_words, 0, sizeof span_words);
    YanSearchBackend backend =
        fake_vtable_for(span_words, 8u, (uint8_t *)span_words + 8u, 8u);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK, yan_search_init(&search, backend),
        "spans that merely touch at their boundary must be accepted");
}

static void search_init_rejects_context_span_that_wraps_uintptr(void)
{
    YanSearchBackend backend = fake_vtable_for(
        (void *)(uintptr_t)(UINTPTR_MAX - 3u), 8u, NULL, 0u);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_INVALID, yan_search_init(&search, backend),
        "a context span whose range leaves uintptr_t must be rejected");
}

static void search_init_rejects_source_span_that_wraps_uintptr(void)
{
    YanSearchBackend backend = fake_vtable_for(
        NULL, 0u, (void *)(uintptr_t)(UINTPTR_MAX - 3u), 8u);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_INVALID, yan_search_init(&search, backend),
        "a source span whose range leaves uintptr_t must be rejected");
}

static void search_init_rejects_nonempty_span_with_null_base(void)
{
    YanSearchBackend backend = fake_vtable_for(NULL, 8u, NULL, 0u);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_INVALID, yan_search_init(&search, backend),
        "a nonempty span with a null base must be rejected");
}

/* ---------------------------------------- protected-span contract: query */

static void search_query_rejects_pattern_overlapping_backend_context(void)
{
    memset(&holder_context, 0, sizeof holder_context);
    memset(&holder_context.bytes_holder, 0x41, sizeof holder_context.bytes_holder);
    memset(&search, 0, sizeof search);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK, yan_search_init(&search, holder_vtable()),
        "the holder backend must initialize");
    capture_reset();
    const uint8_t *pattern = (const uint8_t *)(const void *)&holder_context;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_INVALID, run_query(pattern, 4u),
        "a pattern overlapping the backend context must be rejected");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, holder_context.query_calls,
        "an aliased pattern must not reach the backend");
}

static void search_query_rejects_pattern_partially_overlapping_context(void)
{
    memset(&holder_context, 0, sizeof holder_context);
    memset(&holder_context.bytes_holder, 0x41, sizeof holder_context.bytes_holder);
    memset(&holder_context.length_holder, 0x41, sizeof holder_context.length_holder);
    memset(&search, 0, sizeof search);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK, yan_search_init(&search, holder_vtable()),
        "the holder backend must initialize");
    capture_reset();
    const uint8_t *pattern =
        (const uint8_t *)(const void *)&holder_context + 2u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_INVALID, run_query(pattern, 4u),
        "a pattern that only partly overlaps the context must be rejected");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, holder_context.query_calls,
        "a partially aliased pattern must not reach the backend");
}

static void search_query_rejects_pattern_overlapping_borrowed_source(void)
{
    memset(span_words, 0x41, sizeof span_words);
    memset(&fake, 0, sizeof fake);
    fake.query_result = YAN_SEARCH_OK;
    fake.read_result = YAN_SEARCH_OK;
    memset(&search, 0, sizeof search);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK,
        yan_search_init(&search,
                        fake_vtable_for(&fake, sizeof fake, span_words, 16u)),
        "the fake backend with a source span must initialize");
    capture_reset();
    const uint8_t *pattern = (const uint8_t *)(const void *)span_words;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_INVALID, run_query(pattern, 4u),
        "a pattern overlapping the borrowed source must be rejected");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, fake.query_calls,
        "an aliased pattern must not reach the backend");
}

static void search_query_accepts_pattern_adjacent_to_protected_span(void)
{
    memset(&adjacent_span, 0x41, sizeof adjacent_span);
    memset(&fake, 0, sizeof fake);
    fake.query_result = YAN_SEARCH_OK;
    fake.read_result = YAN_SEARCH_OK;
    memset(&search, 0, sizeof search);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK,
        yan_search_init(&search,
                        fake_vtable_for(&fake, sizeof fake,
                                        adjacent_span.region,
                                        sizeof adjacent_span.region)),
        "the fake backend with an adjacent source span must initialize");
    capture_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK, run_query(adjacent_span.pattern, 4u),
        "a pattern immediately after a protected span is legal");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        1u, fake.query_calls,
        "a legal adjacent pattern must reach the backend");
}

/* --------------------------------------- protected-span contract: reader */

static void search_read_match_rejects_outputs_overlapping_backend_context(void)
{
    memset(&holder_context, 0, sizeof holder_context);
    memset(&search, 0, sizeof search);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK, yan_search_init(&search, holder_vtable()),
        "the holder backend must initialize");
    holder_out_bytes = (const uint8_t **)(void *)&holder_context.bytes_holder;
    holder_out_length = &holder_context.length_holder;
    holder_offset = 0u;
    holder_capacity = 4u;
    holder_result = YAN_SEARCH_OK;
    YanSearchSink sink;
    sink.context = NULL;
    sink.match = holder_alias_match;
    sink.chunk = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_STOPPED,
        yan_search_query(&search, (const uint8_t *)"x", 1u, sink),
        "the holder probe callback stops the query");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_INVALID, holder_result,
        "output holders overlapping the backend context must be rejected");
    /* The backend context's own call counters advance with the query, so the
     * invariant is the holder storage itself plus the absence of a backend
     * read; the whole-object snapshot cannot be used here. */
    TEST_ASSERT_NULL_MESSAGE(
        holder_context.bytes_holder,
        "a rejected reader call must not write the aliased bytes holder");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, holder_context.length_holder,
        "a rejected reader call must not write the aliased length holder");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, holder_context.read_calls,
        "a rejected reader call must not reach the backend reader");
}

static void search_read_match_rejects_outputs_overlapping_borrowed_source(void)
{
    memset(&holder_context, 0, sizeof holder_context);
    memset(&guard_region, 0, sizeof guard_region);
    memset(&search, 0, sizeof search);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK,
        yan_search_init(&search,
                        holder_vtable_for(&holder_context, &guard_region,
                                          sizeof guard_region)),
        "the holder backend with a source span must initialize");
    holder_out_bytes = (const uint8_t **)(void *)&guard_region.bytes_holder;
    holder_out_length = &guard_region.length_holder;
    holder_offset = 0u;
    holder_capacity = 4u;
    holder_result = YAN_SEARCH_OK;
    static uint8_t before[sizeof(GuardRegion)];
    memcpy(before, &guard_region, sizeof before);
    YanSearchSink sink;
    sink.context = NULL;
    sink.match = holder_alias_match;
    sink.chunk = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_STOPPED,
        yan_search_query(&search, (const uint8_t *)"x", 1u, sink),
        "the holder probe callback stops the query");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_INVALID, holder_result,
        "output holders overlapping the borrowed source must be rejected");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(
        before, &guard_region, sizeof before,
        "a rejected reader call must not write the aliased source");
}

static void search_read_match_rejects_zero_capacity_outputs_over_source(void)
{
    memset(&holder_context, 0, sizeof holder_context);
    memset(&guard_region, 0, sizeof guard_region);
    memset(&search, 0, sizeof search);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK,
        yan_search_init(&search,
                        holder_vtable_for(&holder_context, &guard_region,
                                          sizeof guard_region)),
        "the holder backend with a source span must initialize");
    holder_out_bytes = (const uint8_t **)(void *)&guard_region.bytes_holder;
    holder_out_length = &guard_region.length_holder;
    holder_offset = 0u;
    holder_capacity = 0u;
    holder_result = YAN_SEARCH_OK;
    YanSearchSink sink;
    sink.context = NULL;
    sink.match = holder_alias_match;
    sink.chunk = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_STOPPED,
        yan_search_query(&search, (const uint8_t *)"x", 1u, sink),
        "the holder probe callback stops the query");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_INVALID, holder_result,
        "even a zero-capacity read must reject holders over the source");
}

static void search_read_match_rejects_end_offset_outputs_over_source(void)
{
    memset(&holder_context, 0, sizeof holder_context);
    memset(&guard_region, 0, sizeof guard_region);
    memset(&search, 0, sizeof search);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK,
        yan_search_init(&search,
                        holder_vtable_for(&holder_context, &guard_region,
                                          sizeof guard_region)),
        "the holder backend with a source span must initialize");
    holder_out_bytes = (const uint8_t **)(void *)&guard_region.bytes_holder;
    holder_out_length = &guard_region.length_holder;
    holder_offset = 4u;
    holder_capacity = 4u;
    holder_result = YAN_SEARCH_OK;
    YanSearchSink sink;
    sink.context = NULL;
    sink.match = holder_alias_match;
    sink.chunk = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_STOPPED,
        yan_search_query(&search, (const uint8_t *)"x", 1u, sink),
        "the holder probe callback stops the query");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_INVALID, holder_result,
        "even an end-offset read must reject holders over the source");
}

static void search_read_match_accepts_outputs_adjacent_to_protected_span(void)
{
    memset(&holder_adjacent, 0, sizeof holder_adjacent);
    memset(&search, 0, sizeof search);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK,
        yan_search_init(&search,
                        holder_vtable_for(&holder_adjacent.context, NULL, 0u)),
        "the adjacent holder backend must initialize");
    holder_out_bytes = (const uint8_t **)(void *)&holder_adjacent.out_bytes;
    holder_out_length = &holder_adjacent.out_length;
    holder_offset = 0u;
    holder_capacity = 4u;
    holder_result = YAN_SEARCH_OK;
    YanSearchSink sink;
    sink.context = NULL;
    sink.match = holder_alias_match;
    sink.chunk = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_STOPPED,
        yan_search_query(&search, (const uint8_t *)"x", 1u, sink),
        "the holder probe callback stops the query");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK, holder_result,
        "output holders immediately after the protected span are legal");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        4u, holder_adjacent.out_length,
        "the reader writes the adjacent length holder");
}

/* ------------------------------------------------ malformed reader chunks */

static void facade_stream_zero_progress_before_end_is_protocol(void)
{
    fake_malform_setup(FAKE_MALFORM_ZERO_PROGRESS, 2u, 4u);
    fake_install();
    capture_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_PROTOCOL, run_query((const uint8_t *)"x", 1u),
        "a backend that makes no progress before the line end is a protocol error");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        1u, capture.matches,
        "a zero-progress reader stops the query after the first match");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        1u, fake.read_calls,
        "the facade must not retry a zero-progress read");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, capture.chunks,
        "no chunk is delivered for a zero-progress read");
}

static void manual_reader_zero_progress_before_end_is_protocol(void)
{
    fake_malform_setup(FAKE_MALFORM_ZERO_PROGRESS, 1u, 4u);
    fake_install();
    capture_reset();
    capture.manual_read = true;
    capture.ignore_reader_error = true;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_PROTOCOL, run_query((const uint8_t *)"x", 1u),
        "a manual zero-progress read must latch a sticky protocol error");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_PROTOCOL, capture.reader_result,
        "the manual reader reports the zero-progress protocol error");
}

static void facade_stream_overlong_chunk_is_protocol(void)
{
    fake_malform_setup(FAKE_MALFORM_OVERLONG, 2u, 4u);
    fake_install();
    capture_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_PROTOCOL, run_query((const uint8_t *)"x", 1u),
        "a chunk longer than the requested range is a protocol error");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        1u, capture.matches,
        "an over-long chunk stops the query after the first match");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, capture.chunks,
        "an over-long chunk is rejected before any chunk is delivered");
}

static void facade_stream_null_chunk_is_protocol(void)
{
    fake_malform_setup(FAKE_MALFORM_NULL_CHUNK, 2u, 4u);
    fake_install();
    capture_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_PROTOCOL, run_query((const uint8_t *)"x", 1u),
        "a nonempty chunk with a null byte pointer is a protocol error");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        1u, capture.matches,
        "a null chunk stops the query after the first match");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, capture.chunks,
        "a null chunk is rejected before any chunk is delivered");
}

static void facade_stream_range_wrap_chunk_is_protocol(void)
{
    fake_malform_setup(FAKE_MALFORM_RANGE_WRAP, 2u, 16u);
    fake_install();
    capture_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_PROTOCOL, run_query((const uint8_t *)"x", 1u),
        "a chunk byte range that leaves uintptr_t is a protocol error");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        1u, capture.matches,
        "a wrapping chunk stops the query after the first match");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, capture.chunks,
        "a wrapping chunk is rejected before any chunk is delivered");
}

/* ------------------------------------------- manual peek beside auto stream */

static void match_callback_manual_peek_succeeds_while_chunk_streams(void)
{
    memset(&fake, 0, sizeof fake);
    fake.query_result = YAN_SEARCH_OK;
    fake.read_result = YAN_SEARCH_OK;
    fake.match_count = 1u;
    fake.names[0] = "peek.txt";
    fake.lines[0] = 1u;
    fake.contents[0] = fake_body_c;
    fake.content_lengths[0] = 6u;
    fake_install();
    capture_reset();
    peek_result = YAN_SEARCH_INVALID;
    peek_length = 0u;
    YanSearchSink sink;
    sink.context = NULL;
    sink.match = peek_match;
    sink.chunk = capture_chunk;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK,
        yan_search_query(&search, (const uint8_t *)"x", 1u, sink),
        "a query with a legal manual peek must still succeed");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK, peek_result,
        "a manual read from the match callback is legal beside a chunk callback");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        6u, peek_length,
        "the manual peek returns the whole current chunk");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        6u, capture.content_length,
        "the facade still auto-streams the whole line after the peek");
}

/* ------------------------------------------- linear init overlap (no OOB) */

static void linear_init_rejects_context_overlapping_borrowed_filesystem(void)
{
    static union {
        YanFs fs;
        YanSearchLinear linear;
    } container;
    memset(&device, 0, sizeof device);
    device.capacity_blocks = TEST_DEVICE_BLOCKS;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_FS_OK,
        yan_fs_format_metadata(device.blocks[0], TEST_DEVICE_BLOCKS),
        "the overlap fixture must format an empty directory");
    memset(&container, 0, sizeof container);
    YanFsBlockIo io;
    io.context = &device;
    io.capacity = device_capacity;
    io.read_block = device_read_block;
    io.write_block = device_write_block;
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK, yan_fs_init(&container.fs, io),
                                  "the overlap fixture must initialize the FS");
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_FS_OK, yan_fs_mount(&container.fs),
                                  "the overlap fixture must mount the FS");
    static uint8_t before[sizeof container];
    memcpy(before, &container, sizeof before);
    YanSearchResult result =
        yan_search_linear_init(&container.linear, &container.fs);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_INVALID, result,
        "linear_init must reject a context that overlaps the borrowed filesystem");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(
        before, &container, sizeof before,
        "a rejected linear_init must leave every filesystem byte unchanged");
}

/* --------------------------------------------- public bounds and stop paths */

static void fake_backend_propagates_each_source_class(void)
{
    static const struct {
        YanSearchResult result;
        const char *message;
    } cases[] = {
        { YAN_SEARCH_NOT_MOUNTED, "NOT_MOUNTED must propagate from the backend" },
        { YAN_SEARCH_FAULTED, "FAULTED must propagate from the backend" },
        { YAN_SEARCH_IO, "IO must propagate from the backend" },
        { YAN_SEARCH_PROTOCOL, "PROTOCOL must propagate from the backend" },
        { YAN_SEARCH_CORRUPT, "CORRUPT must propagate from the backend" },
        { YAN_SEARCH_UNSUPPORTED, "UNSUPPORTED must propagate from the backend" }
    };
    const uint32_t count = (uint32_t)(sizeof cases / sizeof cases[0]);
    for (uint32_t i = 0; i < count; ++i) {
        memset(&fake, 0, sizeof fake);
        fake.query_result = cases[i].result;
        fake.read_result = YAN_SEARCH_OK;
        fake_install();
        capture_reset();
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            cases[i].result, run_query((const uint8_t *)"x", 1u),
            cases[i].message);
    }
}

static void fake_backend_emits_ten_matches_in_order(void)
{
    memset(&fake, 0, sizeof fake);
    fake.query_result = YAN_SEARCH_OK;
    fake.read_result = YAN_SEARCH_OK;
    fake.match_count = 10u;
    for (uint32_t i = 0; i < 10u; ++i) {
        fake.names[i] = "ten.txt";
        fake.lines[i] = i + 1u;
        fake.contents[i] = fake_body_a;
        fake.content_lengths[i] = 1u;
    }
    fake_install();
    capture_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK, run_query((const uint8_t *)"x", 1u),
        "a ten-match fake query must succeed");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        10u, capture.matches,
        "the facade must report all ten fake matches");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        10u, capture.lines[9],
        "the tenth match keeps its own line number");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        10u, fake.read_calls,
        "each of the ten matches is streamed exactly once");
}

static void default_callback_stop_is_not_a_reader_error(void)
{
    memset(&fake, 0, sizeof fake);
    fake.query_result = YAN_SEARCH_OK;
    fake.read_result = YAN_SEARCH_OK;
    fake.match_count = 2u;
    for (uint32_t i = 0; i < 2u; ++i) {
        fake.names[i] = "stop.txt";
        fake.lines[i] = i + 1u;
        fake.contents[i] = fake_body_a;
        fake.content_lengths[i] = 4u;
    }
    fake_install();
    capture_reset();
    capture.stop_after = 1u;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_STOPPED, run_query((const uint8_t *)"x", 1u),
        "a clean callback stop reports STOPPED");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_SEARCH_OK, capture.reader_result,
        "a clean callback stop must not fabricate a reader error");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0u, fake.read_calls,
        "the stopping match callback performs no reader read");
}

/* Root review additions: exercise the real prefix table at its maximum, and
 * every first/middle/last device read of the three independent passes. */
static void real_search_maximum_pattern_crosses_a_chunk_boundary(void)
{
    static uint8_t pattern[1023];
    memset(pattern, 'a', sizeof pattern);
    pattern[1022] = 'b';
    real_setup();
    memset(source, 'x', 6000u);
    memcpy(source + 3584u, pattern, sizeof pattern);
    create_file("max.txt", source, 6000u);
    capture_reset();
    TEST_ASSERT_EQUAL_INT(YAN_SEARCH_OK, run_query(pattern, sizeof pattern));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, capture.matches,
        "the complete 1023-byte repeated-prefix pattern spans two scan chunks");
    TEST_ASSERT_EQUAL_UINT32(6000u, capture.content_length);
    TEST_ASSERT_EQUAL_MEMORY(source, capture.content, 6000u);
}

static void real_search_each_phase_first_middle_last_failure_stops_reads(void)
{
    for (uint32_t fault = 1u; fault <= 9u; ++fault) {
        real_setup();
        memset(source, 'x', 9000u);
        memcpy(source + 8994u, "NEEDLE", 6u);
        create_file("phases.txt", source, 9000u);
        snapshot_medium();
        capture_reset();
        device.reads = 0u;
        device.writes = 0u;
        device.read_fail_at = fault;
        device.read_fail_code = YAN_FS_IO_ERROR;
        TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_SEARCH_IO,
            run_query((const uint8_t *)"NEEDLE", 6u),
            "preflight, scan and reader first/middle/last faults propagate IO");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(fault, device.reads,
            "no read is attempted after the failed device request");
        TEST_ASSERT_EQUAL_UINT32(fault <= 6u ? 0u : 1u, capture.matches);
        TEST_ASSERT_EQUAL_UINT32(0u, device.writes);
        TEST_ASSERT_TRUE(medium_unchanged());
        TEST_ASSERT_EQUAL_INT(YAN_FS_STATE_FAULTED, fs.state);
        TEST_ASSERT_FALSE(search.busy);
        TEST_ASSERT_FALSE(linear.busy);
    }
}

/* -------------------------------------------------------------------- Unity */

void setUp(void)
{
    memset(&device, 0, sizeof device);
    memset(&fs, 0, sizeof fs);
    memset(&linear, 0, sizeof linear);
    memset(&search, 0, sizeof search);
    memset(&fake, 0, sizeof fake);
    memset(&capture, 0, sizeof capture);
    memset(source, 0, sizeof source);
    memset(medium_snapshot, 0, sizeof medium_snapshot);
    memset(&alias_union, 0, sizeof alias_union);
    memset(span_words, 0, sizeof span_words);
    memset(&guard_region, 0, sizeof guard_region);
    memset(&holder_context, 0, sizeof holder_context);
    memset(&adjacent_span, 0, sizeof adjacent_span);
    memset(&holder_adjacent, 0, sizeof holder_adjacent);
    holder_out_bytes = NULL;
    holder_out_length = NULL;
    holder_result = YAN_SEARCH_OK;
    holder_offset = 0u;
    holder_capacity = 0u;
    peek_result = YAN_SEARCH_OK;
    peek_length = 0u;
    fake.query_result = YAN_SEARCH_OK;
    fake.read_result = YAN_SEARCH_OK;
    fake.read_fail_code = YAN_SEARCH_IO;
    reader_probe_kind = READER_PROBE_NONE;
    reader_probe_result = YAN_SEARCH_OK;
    reader_probe_length = 0u;
    reader_probe_io = &fake.read_calls;
    reader_probe_io_before = 0u;
    reader_probe_io_after = 0u;
    reentry_armed = false;
    reentry_result = YAN_SEARCH_OK;
}

void tearDown(void)
{
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(real_search_maximum_pattern_crosses_a_chunk_boundary);
    RUN_TEST(real_search_each_phase_first_middle_last_failure_stops_reads);

    /* facade init and query validation */
    RUN_TEST(search_init_rejects_null_search);
    RUN_TEST(search_init_rejects_an_incomplete_backend);
    RUN_TEST(search_init_accepts_a_well_formed_backend_and_reinitializes_idle);
    RUN_TEST(search_query_before_init_is_invalid_without_a_backend_call);
    RUN_TEST(search_query_rejects_a_null_pattern);
    RUN_TEST(search_query_rejects_an_empty_pattern);
    RUN_TEST(search_query_accepts_1023_bytes_and_rejects_1024);
    RUN_TEST(search_query_rejects_nul_and_lf_pattern_bytes);
    RUN_TEST(search_query_accepts_tab_cr_and_non_utf8_pattern_bytes);
    RUN_TEST(search_query_rejects_a_null_match_callback);
    RUN_TEST(search_query_rejects_a_pattern_range_that_leaves_uintptr);
    RUN_TEST(search_query_rejects_a_pattern_overlapping_the_search);
    RUN_TEST(search_query_reentry_returns_busy);
    RUN_TEST(search_init_reentry_returns_busy);
    RUN_TEST(search_reinitializes_with_a_swapped_backend);

    /* reader period and argument guards */
    RUN_TEST(search_read_match_outside_a_callback_is_invalid);
    RUN_TEST(search_read_match_rejects_null_output_holders);
    RUN_TEST(search_read_match_rejects_outputs_aliasing_the_search);
    RUN_TEST(search_read_match_rejects_an_offset_past_the_content);
    RUN_TEST(search_read_match_rejects_an_output_address_that_overflows);
    RUN_TEST(search_read_match_reads_the_current_content_range);
    RUN_TEST(search_read_match_zero_capacity_and_end_offset_are_empty_reads);

    /* fake backend lifecycle with no filesystem */
    RUN_TEST(fake_backend_reports_value_matches_in_order);
    RUN_TEST(match_callback_false_ends_with_stopped);
    RUN_TEST(chunk_callback_false_ends_with_stopped);
    RUN_TEST(reader_error_wins_over_a_callback_stop);
    RUN_TEST(an_ignored_reader_error_still_reports_the_source_error);
    RUN_TEST(fake_backend_source_result_is_propagated);
    RUN_TEST(source_error_classes_are_distinct_and_not_success);
    RUN_TEST(reader_reentry_from_a_chunk_is_busy_without_extra_reads);

    /* protected-span alias contracts (guard strengthening, expected RED) */
    RUN_TEST(search_init_rejects_backend_context_overlapping_search);
    RUN_TEST(search_init_rejects_borrowed_source_overlapping_search);
    RUN_TEST(search_init_rejects_context_and_source_overlapping_each_other);
    RUN_TEST(search_init_accepts_adjacent_protected_spans);
    RUN_TEST(search_init_rejects_context_span_that_wraps_uintptr);
    RUN_TEST(search_init_rejects_source_span_that_wraps_uintptr);
    RUN_TEST(search_init_rejects_nonempty_span_with_null_base);
    RUN_TEST(search_query_rejects_pattern_overlapping_backend_context);
    RUN_TEST(search_query_rejects_pattern_partially_overlapping_context);
    RUN_TEST(search_query_rejects_pattern_overlapping_borrowed_source);
    RUN_TEST(search_query_accepts_pattern_adjacent_to_protected_span);
    RUN_TEST(search_read_match_rejects_outputs_overlapping_backend_context);
    RUN_TEST(search_read_match_rejects_outputs_overlapping_borrowed_source);
    RUN_TEST(search_read_match_rejects_zero_capacity_outputs_over_source);
    RUN_TEST(search_read_match_rejects_end_offset_outputs_over_source);
    RUN_TEST(search_read_match_accepts_outputs_adjacent_to_protected_span);

    /* malformed reader chunks and the legal manual peek */
    RUN_TEST(facade_stream_zero_progress_before_end_is_protocol);
    RUN_TEST(manual_reader_zero_progress_before_end_is_protocol);
    RUN_TEST(facade_stream_overlong_chunk_is_protocol);
    RUN_TEST(facade_stream_null_chunk_is_protocol);
    RUN_TEST(facade_stream_range_wrap_chunk_is_protocol);
    RUN_TEST(match_callback_manual_peek_succeeds_while_chunk_streams);
    RUN_TEST(linear_init_rejects_context_overlapping_borrowed_filesystem);

    /* public bounds and stop paths */
    RUN_TEST(fake_backend_propagates_each_source_class);
    RUN_TEST(fake_backend_emits_ten_matches_in_order);
    RUN_TEST(default_callback_stop_is_not_a_reader_error);

    /* real linear backend semantics (RED while the scan is stubbed) */
    RUN_TEST(real_search_empty_directory_yields_zero_matches);
    RUN_TEST(real_search_empty_file_contributes_no_lines);
    RUN_TEST(real_search_no_hits_is_ok_with_zero_matches);
    RUN_TEST(real_search_reports_each_matched_line_once_in_slot_order);
    RUN_TEST(real_search_orders_files_across_a_slot_hole);
    RUN_TEST(real_search_matches_a_word_across_the_scan_chunk_boundary);
    RUN_TEST(real_search_matches_chinese_bytes_across_the_boundary);
    RUN_TEST(real_search_repeated_prefix_does_not_miss_a_hit);
    RUN_TEST(real_search_does_not_match_across_a_line_break);
    RUN_TEST(real_search_splits_lf_and_crlf_without_terminator_content);
    RUN_TEST(real_search_treats_bare_cr_as_content);
    RUN_TEST(real_search_counts_an_unterminated_final_line);
    RUN_TEST(real_search_final_lf_adds_no_phantom_line);
    RUN_TEST(real_search_empty_lines_still_advance_the_line_number);
    RUN_TEST(real_search_returns_a_complete_100kib_line);
    RUN_TEST(real_search_delivers_bounded_chunks_not_a_line_pointer);
    RUN_TEST(real_search_skips_a_file_at_its_first_nul);
    RUN_TEST(real_search_skips_a_file_with_a_late_nul);
    RUN_TEST(real_search_continues_to_the_next_file_after_binary);
    RUN_TEST(real_search_crlf_split_exactly_at_the_chunk_boundary);
    RUN_TEST(real_search_nul_short_circuit_suppresses_a_later_read_fault);
    RUN_TEST(real_search_reader_mid_phase_failure_is_sticky);
    RUN_TEST(real_search_unmounted_source_reports_not_mounted);
    RUN_TEST(real_search_read_trace_is_preflight_scan_then_reader);
    RUN_TEST(real_search_preflight_read_failure_reports_io);
    RUN_TEST(real_search_scan_read_failure_reports_io);
    RUN_TEST(real_search_match_reader_failure_is_not_success);
    RUN_TEST(real_search_protocol_read_failure_maps_to_protocol);
    RUN_TEST(real_search_unknown_read_status_maps_to_protocol);
    RUN_TEST(real_search_reader_error_outranks_an_ignored_result);
    RUN_TEST(real_search_callback_stop_reports_stopped);
    RUN_TEST(real_search_query_reentry_is_busy_without_extra_reads);
    RUN_TEST(real_search_init_reentry_is_busy);
    RUN_TEST(real_search_reader_reentry_from_chunk_is_busy);

    return UNITY_END();
}
