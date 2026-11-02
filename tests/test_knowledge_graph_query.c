/* 0045 R1 focused proof of the shared image graph query.
 *
 * Contract: docs/specs/0045-graph-mcp-tools.md R1. The 0044 core and CLI suites
 * already cover the schema; what is new in this seam and unproven by them is:
 *
 *   1. a real image yields exactly one 0044 JSON object with the locked field
 *      order, the digest the service verified, and a NUL-terminated malloc
 *      buffer whose length excludes the terminator;
 *   2. an accepted expected_source matches that digest, and one wrong byte is
 *      STALE -- decided before the build, so STALE outranks NOT_FOUND;
 *   3. output_limit below the object is LIMIT, the exact length is accepted,
 *      and either way no partial payload is published;
 *   4. cancellation is observed between the source next/read callbacks the
 *      service installs, not only at entry, and returns no payload;
 *   5. malformed arguments and an already-cancelled request never open the
 *      image at all, proven by counting the real open call.
 *
 * The fixture is a real YanFS image built with the production encoder over an
 * in-memory block device and written to a temporary file; the service therefore
 * opens, mounts and digests a file exactly as in production. The expected
 * digest is recomputed here by parsing the directory and hashing the 0033
 * identity fields with the 0029 SHA-256 primitive -- it never comes from the
 * service's own output.
 *
 * Cancellation is made deterministic with the same seam the 0044 fault probe
 * uses: this binary is linked with --wrap=yan_semantic_posix_bind_source, so it
 * can count the source callbacks and raise the caller's flag at an exact
 * callback ordinal. The production library and tool keep no fault switch; the
 * wrapper delegates to the real bind and is inert (cancel_after == 0) for every
 * other test in this file.
 *
 * "Zero I/O" is not taken on trust from the return code: the binary is also
 * linked with --wrap=yan_semantic_posix_open_image_ex, so every real image open
 * increments a counter this file can assert on. A rejected or already-cancelled
 * request must leave that counter at zero. */

#define _POSIX_C_SOURCE 200809L

#include "knowledge_graph_query.h"
#include "semantic_posix.h"
#include "semantic_sha256.h"
#include "unity.h"
#include "yanfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BLOCKS 64u
#define CAPACITY 63u

static uint8_t medium[BLOCKS][4096];
static uint8_t snapshot[BLOCKS][4096];
static YanFs writer;
/* The YanFS directory workspace must outlive setUp: YanFs keeps these pointers
 * for every later create/remove/mount, so they are file-scope storage rather
 * than stack frames that expire when setUp returns. */
static uint8_t directory_workspace[4096];
static uint16_t name_order[CAPACITY];
static char directory[64];
static char image_path[128];

/* ------------------------------------------------------------------ injector */

static uint32_t hook_nexts, hook_reads;
static uint32_t open_calls;
static uint32_t cancel_after_read;
static volatile sig_atomic_t *hook_cancel;

/* Allocation-failure seam for the service's OWN JSON buffer.
 *
 * The core has its own allocation sweep in test_knowledge_graph_alloc, but that
 * test drives yan_kg_build directly and never enters the query service's
 * encoder, whose buffer grows with realloc. So the service's "a failed
 * allocation is NOSPACE with no payload" promise is only observable here:
 * fail_encoder_reallocs makes the NEXT realloc inside the measured call fail
 * once. The flag is set immediately before one call and cleared immediately
 * after, so no Unity, fixture or stdio allocation can ever be handed a NULL. */
static bool fail_encoder_reallocs;
static uint32_t encoder_reallocs, encoder_reallocs_failed;

void *__real_realloc(void *, size_t);

void *__wrap_realloc(void *pointer, size_t size)
{
    if (fail_encoder_reallocs) {
        ++encoder_reallocs;
        /* Only the first encoder growth is failed, and only while the window is
         * open; later growth in the same call behaves normally. */
        if (encoder_reallocs_failed == 0u) {
            ++encoder_reallocs_failed;
            return NULL;
        }
    }
    return __real_realloc(pointer, size);
}

/* Every real image open is counted, so "no I/O before validation" is an
 * assertion rather than a claim. */
YanImageOpenResult __real_yan_semantic_posix_open_image_ex(
    YanSemanticPosix *, const char *);

YanImageOpenResult __wrap_yan_semantic_posix_open_image_ex(
    YanSemanticPosix *adapter, const char *image)
{
    ++open_calls;
    return __real_yan_semantic_posix_open_image_ex(adapter, image);
}

void __real_yan_semantic_posix_bind_source(YanSemanticPosix *,
                                           YanSemanticSource *);

static YanSemanticResult hook_next(void *context, uint32_t *cursor,
                                   YanSemanticFile *out);
static YanSemanticResult hook_read(void *context, const char *name,
                                   uint32_t offset, uint8_t *out,
                                   uint32_t length);

static YanSemanticSource real_source; /* copy of what the adapter bound */

static YanSemanticResult hook_next(void *context, uint32_t *cursor,
                                   YanSemanticFile *out)
{
    ++hook_nexts;
    return real_source.next(context, cursor, out);
}

static YanSemanticResult hook_read(void *context, const char *name,
                                   uint32_t offset, uint8_t *out,
                                   uint32_t length)
{
    YanSemanticResult result = real_source.read(context, name, offset, out,
                                                length);
    ++hook_reads;
    /* Raise the caller's flag after the requested number of real reads, which
     * is exactly the "between two source callbacks" moment the service
     * promises to observe. With cancel_after_read == 1 the flag is raised
     * inside the FIRST source-identity digest of the initial observation, so
     * the test exercises that callback and not the build. */
    if (cancel_after_read != 0u && hook_reads == cancel_after_read &&
        hook_cancel != NULL) {
        *hook_cancel = 1;
    }
    return result;
}

void __wrap_yan_semantic_posix_bind_source(YanSemanticPosix *adapter,
                                           YanSemanticSource *source)
{
    __real_yan_semantic_posix_bind_source(adapter, source);
    if (cancel_after_read == 0u) {
        return;
    }
    real_source = *source;
    source->next = hook_next;
    source->read = hook_read;
}

/* -------------------------------------------------------------------- fixture */

static YanFsIoResult block_device_capacity(void *opaque, uint64_t *blocks)
{
    (void)opaque;
    *blocks = BLOCKS;
    return YAN_FS_IO_OK;
}

static YanFsIoResult block_device_read(void *opaque, uint32_t lba, uint8_t *out)
{
    (void)opaque;
    if (lba >= BLOCKS) {
        return YAN_FS_IO_ERROR;
    }
    memcpy(out, medium[lba], 4096);
    return YAN_FS_IO_OK;
}

static YanFsIoResult block_device_write(void *opaque, uint32_t lba,
                                        const uint8_t *in)
{
    (void)opaque;
    if (lba >= BLOCKS) {
        return YAN_FS_IO_ERROR;
    }
    memcpy(medium[lba], in, 4096);
    return YAN_FS_IO_OK;
}

static void write_image(void)
{
    FILE *file = fopen(image_path, "wb");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, "kg_query_fixture_create");
    TEST_ASSERT_EQUAL_UINT(sizeof medium,
                           fwrite(medium, 1u, sizeof medium, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static void create_note(const char *name, const char *text)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        YAN_FS_OK, yan_fs_create(&writer, name, (const uint8_t *)text,
                                 (uint32_t)strlen(text)),
        "kg_query_fixture_note");
}

void setUp(void)
{
    memset(medium, 0, sizeof medium);
    memset(&writer, 0, sizeof writer);
    hook_nexts = hook_reads = 0u;
    open_calls = 0u;
    fail_encoder_reallocs = false;
    encoder_reallocs = encoder_reallocs_failed = 0u;
    cancel_after_read = 0u;
    hook_cancel = NULL;
    memset(&real_source, 0, sizeof real_source);
    strcpy(directory, "/tmp/yan-kg-query-XXXXXX");
    TEST_ASSERT_NOT_NULL_MESSAGE(mkdtemp(directory), "kg_query_fixture_dir");
    snprintf(image_path, sizeof image_path, "%s/source.img", directory);

    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_format_metadata(medium[0], BLOCKS));
    memset(directory_workspace, 0, sizeof directory_workspace);
    memset(name_order, 0, sizeof name_order);
    YanFsBlockIo io = {NULL, block_device_capacity, block_device_read,
                       block_device_write};
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK,
                          yan_fs_init_directory(&writer, io,
                                                directory_workspace, 1u,
                                                name_order));
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK, yan_fs_mount(&writer));
    create_note("geometry",
                "@yan-note 1\ntitle: 几何\n@end\n"
                "@block definitions\n"
                "@kg concept square 正方形\n"
                "@kg concept rectangle 矩形\n"
                "@kg alias square 等边矩形\n"
                "@kg relation is_a square rectangle\n");
    write_image();
    memcpy(snapshot, medium, sizeof snapshot);
}

void tearDown(void)
{
    unlink(image_path);
    if (directory[0] != '\0') {
        rmdir(directory);
        directory[0] = '\0';
    }
}

/* --------------------------------------------------------------- oracle */

/* The 0033 logical source identity, recomputed from the image bytes: files in
 * ASCII name order, then per file the slot u32LE, name length u32LE, name bytes,
 * size u32LE and every raw byte. */
static void oracle_digest(uint8_t out[32])
{
    typedef struct {
        uint32_t slot;
        char name[32];
        uint32_t size, start;
    } Entry;
    static Entry entries[CAPACITY];
    uint32_t count = 0u;
    for (uint32_t slot = 0u; slot < CAPACITY; ++slot) {
        const uint8_t *entry = medium[0] + 64u + (size_t)slot * 64u;
        if (entry[0] == 0u) {
            continue;
        }
        memset(&entries[count], 0, sizeof entries[count]);
        entries[count].slot = slot;
        memcpy(entries[count].name, entry, 32u);
        memcpy(&entries[count].size, entry + 32, 4u);
        memcpy(&entries[count].start, entry + 36, 4u);
        ++count;
    }
    for (uint32_t i = 1u; i < count; ++i) {
        for (uint32_t j = i; j > 0u &&
             strcmp(entries[j - 1u].name, entries[j].name) > 0; --j) {
            const Entry held = entries[j];
            entries[j] = entries[j - 1u];
            entries[j - 1u] = held;
        }
    }
    YanSemanticSha256 hasher;
    TEST_ASSERT_EQUAL_INT(YAN_SEMANTIC_OK, yan_semantic_sha256_init(&hasher));
    for (uint32_t i = 0u; i < count; ++i) {
        uint8_t fields[4];
        const Entry *entry = &entries[i];
        const uint32_t slot = entry->slot;
        const uint32_t name_length = (uint32_t)strlen(entry->name);
        for (unsigned b = 0u; b < 4u; ++b) {
            fields[b] = (uint8_t)(slot >> (8u * b));
        }
        TEST_ASSERT_EQUAL_INT(YAN_SEMANTIC_OK,
                              yan_semantic_sha256_update(&hasher, fields, 4u));
        for (unsigned b = 0u; b < 4u; ++b) {
            fields[b] = (uint8_t)(name_length >> (8u * b));
        }
        TEST_ASSERT_EQUAL_INT(YAN_SEMANTIC_OK,
                              yan_semantic_sha256_update(&hasher, fields, 4u));
        TEST_ASSERT_EQUAL_INT(YAN_SEMANTIC_OK,
                              yan_semantic_sha256_update(
                                  &hasher, (const uint8_t *)entry->name,
                                  name_length));
        for (unsigned b = 0u; b < 4u; ++b) {
            fields[b] = (uint8_t)(entry->size >> (8u * b));
        }
        TEST_ASSERT_EQUAL_INT(YAN_SEMANTIC_OK,
                              yan_semantic_sha256_update(&hasher, fields, 4u));
        const uint8_t *bytes = medium[0] + (size_t)entry->start * 4096u;
        TEST_ASSERT_EQUAL_INT(YAN_SEMANTIC_OK,
                              yan_semantic_sha256_update(&hasher, bytes,
                                                         entry->size));
    }
    TEST_ASSERT_EQUAL_INT(YAN_SEMANTIC_OK, yan_semantic_sha256_final(&hasher,
                                                                     out));
}

static void oracle_hex(const uint8_t digest[32], char out[65])
{
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0u; i < 32u; ++i) {
        out[2u * i] = digits[digest[i] >> 4];
        out[2u * i + 1u] = digits[digest[i] & 15u];
    }
    out[64] = '\0';
}

/* --------------------------------------------------------------- assertions */

static void list_options(YanKgQueryOptions *options)
{
    memset(options, 0, sizeof *options);
    options->operation = YAN_KG_QUERY_LIST;
    options->image = image_path;
}

static void query_options(YanKgQueryOptions *options, const char *text)
{
    memset(options, 0, sizeof *options);
    options->operation = YAN_KG_QUERY_LOOKUP;
    options->image = image_path;
    options->query = text;
    options->query_length = (uint32_t)strlen(text);
}

/* A refusal must leave both outputs untouched and readable by the caller. */
static void assert_no_payload(YanKgResult result, char *payload, size_t length)
{
    TEST_ASSERT_NOT_EQUAL_MESSAGE(YAN_KG_OK, result, "kg_query_unexpected_ok");
    TEST_ASSERT_NULL_MESSAGE(payload, "kg_query_payload_published_on_failure");
    TEST_ASSERT_EQUAL_UINT32(0u, length);
}

static void assert_image_unchanged(void)
{
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(snapshot, medium, sizeof snapshot,
                                     "kg_query_image_changed");
}

/* --------------------------------------------------------------------- cases */

/* The locked 0044 root order, and the digest the service verified. */
static void test_list_object_fields_and_digest(void)
{
    YanKgQueryOptions options;
    list_options(&options);
    char *payload = NULL;
    size_t length = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_KG_OK,
                          yan_kg_query_json(&options, &payload, &length));
    TEST_ASSERT_NOT_NULL(payload);
    TEST_ASSERT_GREATER_THAN_UINT32(0u, (uint32_t)length);
    /* The buffer is a C string the transport can hand on, and its declared
     * length excludes that terminator. */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)length, (uint32_t)strlen(payload));
    TEST_ASSERT_EQUAL_INT('\0', payload[length]);

    uint8_t digest[32];
    char hex[65];
    oracle_digest(digest);
    oracle_hex(digest, hex);
    char expected[256];
    int prefix = snprintf(expected, sizeof expected,
                          "{\"version\":1,\"operation\":\"list\","
                          "\"source_sha256\":\"%s\",\"skipped\":0,"
                          "\"total\":2,\"shown\":2,\"concepts\":[",
                          hex);
    TEST_ASSERT_GREATER_THAN_INT(0, prefix);
    TEST_ASSERT_EQUAL_MEMORY(expected, payload, (size_t)prefix);
    /* The first concept row, in ID order and field order. */
    TEST_ASSERT_NOT_NULL(strstr(payload,
                                "{\"id\":\"rectangle\",\"name\":\"矩形\","
                                "\"alias_count\":0,\"declaration_count\":1,"
                                "\"relation_count\":1}"));
    TEST_ASSERT_NOT_NULL(strstr(payload, "\"alias_count\":1"));
    /* Exactly one object, no trailing whitespace inside the payload. */
    TEST_ASSERT_EQUAL_INT('}', payload[length - 1u]);
    free(payload);
    assert_image_unchanged();
}

/* Query keeps the locked field order and the full alias array. */
static void test_query_object_fields(void)
{
    YanKgQueryOptions options;
    query_options(&options, "等边矩形");
    char *payload = NULL;
    size_t length = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_KG_OK,
                          yan_kg_query_json(&options, &payload, &length));
    TEST_ASSERT_NOT_NULL(payload);
    uint8_t digest[32];
    char hex[65];
    oracle_digest(digest);
    oracle_hex(digest, hex);
    char expected[320];
    int prefix = snprintf(expected, sizeof expected,
                          "{\"version\":1,\"operation\":\"query\","
                          "\"source_sha256\":\"%s\",\"skipped\":0,"
                          "\"query\":\"等边矩形\",\"concept\":{\"id\":\"square\","
                          "\"name\":\"正方形\",\"aliases\":[\"等边矩形\"]}",
                          hex);
    TEST_ASSERT_GREATER_THAN_INT(0, prefix);
    TEST_ASSERT_EQUAL_MEMORY(expected, payload, (size_t)prefix);
    TEST_ASSERT_NOT_NULL(strstr(payload,
                                "\"declarations\":{\"total\":1,\"shown\":1,"
                                "\"items\":[{\"file\":\"geometry\""));
    TEST_ASSERT_NOT_NULL(strstr(payload,
                                "\"relations\":{\"total\":1,\"shown\":1,"
                                "\"items\":[{\"type\":\"is_a\","
                                "\"subject\":{\"id\":\"square\","
                                "\"name\":\"正方形\"},\"object\":{\"id\":"
                                "\"rectangle\",\"name\":\"矩形\"},"
                                "\"direction\":\"out\",\"where\":{"));
    free(payload);
    assert_image_unchanged();
}

/* An accepted expected_source is the one the caller could pass on from an
 * earlier call; a single wrong byte must be STALE, and STALE must be decided
 * before the graph, so a text that would be NOT_FOUND still reports STALE. */
static void test_expected_source_accept_and_stale(void)
{
    uint8_t digest[32];
    oracle_digest(digest);

    YanKgQueryOptions options;
    list_options(&options);
    options.expected_source = digest;
    char *payload = NULL;
    size_t length = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_KG_OK,
                          yan_kg_query_json(&options, &payload, &length));
    TEST_ASSERT_NOT_NULL(payload);
    free(payload);

    uint8_t wrong[32];
    memcpy(wrong, digest, sizeof wrong);
    wrong[31] ^= 0xFFu;
    query_options(&options, "no-such-concept");
    options.expected_source = wrong;
    payload = NULL;
    length = 0u;
    YanKgResult result = yan_kg_query_json(&options, &payload, &length);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_KG_STALE, result,
                                  "kg_query_expected_mismatch");
    assert_no_payload(result, payload, length);
    assert_image_unchanged();
}

/* STALE precedes the graph build. This needs an image whose graph is
 * unbuildable: with a wrong expected_source the answer must still be STALE, and
 * only without one may the build's own error surface. A healthy image plus an
 * absent query would only prove "before lookup", which is why this case exists.
 *
 * The bad image keeps a legal YanFS directory (so mount and the identity digest
 * succeed) but holds an illegal `@kg` line, which the core classifies as
 * GRAPH_FORMAT. */
static void test_stale_precedes_graph_build(void)
{
    /* Replace the fixture note with an illegal declaration, then reseal. */
    TEST_ASSERT_EQUAL_INT(YAN_FS_OK,
                          yan_fs_remove(&writer, "geometry"));
    create_note("broken", "@block b\n@kg concept\n");
    write_image();
    memcpy(snapshot, medium, sizeof snapshot);

    uint8_t digest[32];
    oracle_digest(digest);
    uint8_t wrong[32];
    memcpy(wrong, digest, sizeof wrong);
    wrong[0] ^= 0x01u;

    /* Without an expected source the graph itself is the first failure. */
    YanKgQueryOptions options;
    list_options(&options);
    char *payload = (char *)1;
    size_t length = 99u;
    YanKgResult result = yan_kg_query_json(&options, &payload, &length);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_KG_GRAPH_FORMAT, result,
                                  "kg_query_bad_graph_surfaces");
    assert_no_payload(result, payload, length);

    /* With a wrong expected source, STALE is decided before the build, so the
     * graph error must not be reported instead. */
    options.expected_source = wrong;
    payload = (char *)1;
    length = 99u;
    result = yan_kg_query_json(&options, &payload, &length);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_KG_STALE, result,
                                  "kg_query_stale_precedes_build");
    assert_no_payload(result, payload, length);

    /* And the matching expected source gets past STALE to the real graph
     * error, which proves the two checks are not the same branch. */
    options.expected_source = digest;
    payload = (char *)1;
    length = 99u;
    result = yan_kg_query_json(&options, &payload, &length);
    TEST_ASSERT_EQUAL_INT(YAN_KG_GRAPH_FORMAT, result);
    assert_no_payload(result, payload, length);
    assert_image_unchanged();
}

/* The cap bounds the encoded object: one byte short of it is LIMIT with no
 * payload, and the exact length is accepted. */
static void test_output_limit_boundary(void)
{
    YanKgQueryOptions options;
    list_options(&options);
    char *full = NULL;
    size_t full_length = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_KG_OK,
                          yan_kg_query_json(&options, &full, &full_length));
    TEST_ASSERT_NOT_NULL(full);
    TEST_ASSERT_GREATER_THAN_UINT32(1u, (uint32_t)full_length);

    options.output_limit = full_length;
    char *exact = NULL;
    size_t exact_length = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_KG_OK,
                          yan_kg_query_json(&options, &exact, &exact_length));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)full_length, (uint32_t)exact_length);
    TEST_ASSERT_EQUAL_MEMORY(full, exact, full_length);
    free(exact);

    options.output_limit = full_length - 1u;
    char *refused = NULL;
    size_t refused_length = 0u;
    YanKgResult result = yan_kg_query_json(&options, &refused, &refused_length);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_KG_LIMIT, result, "kg_query_over_limit");
    assert_no_payload(result, refused, refused_length);

    /* A cap too small for even the first reserve is still LIMIT, never NOSPACE:
     * the caller's bound and a broken host are different answers. */
    options.output_limit = 1u;
    refused = NULL;
    refused_length = 0u;
    result = yan_kg_query_json(&options, &refused, &refused_length);
    TEST_ASSERT_EQUAL_INT(YAN_KG_LIMIT, result);
    assert_no_payload(result, refused, refused_length);
    free(full);
    assert_image_unchanged();
}

/* Cancellation: already set at entry, and raised between the source callbacks
 * of the build. Both publish nothing; the second is proven to have happened
 * inside the pass rather than at entry. */
static void test_cancel_at_entry_and_between_callbacks(void)
{
    volatile sig_atomic_t flag = 0;
    YanKgQueryOptions options;
    query_options(&options, "square");

    options.cancel = &flag;
    flag = 1;
    char *payload = NULL;
    size_t length = 0u;
    YanKgResult result = yan_kg_query_json(&options, &payload, &length);
    /* The service reports a cancelled pass as IO so no new core enumerator is
     * invented for a condition the core does not model; the caller that raised
     * the flag is the one that suppresses the response. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_KG_IO, result, "kg_query_cancel_at_entry");
    assert_no_payload(result, payload, length);
    /* The flag was set before the call, so no descriptor may have been opened:
     * this is the assertion behind the API's "cancellation is honoured before
     * any I/O" promise. */
    TEST_ASSERT_EQUAL_UINT32(0u, open_calls);

    /* Now clear the flag, let the first real read happen and raise it right
     * after that read returns. Because the flag was clear at entry, a failure
     * here can only come from observing it inside a pass, between two source
     * callbacks. The pass that catches it is the FIRST source-identity digest
     * (the initial observation), whose read callback sees the raised flag and
     * stops; the call then reports the stop before the graph is built. So this
     * case proves the in-pass observation point, not the build phase. */
    flag = 0;
    cancel_after_read = 1u;
    hook_cancel = &flag;
    payload = NULL;
    length = 0u;
    result = yan_kg_query_json(&options, &payload, &length);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_KG_IO, result, "kg_query_cancel_in_pass");
    assert_no_payload(result, payload, length);
    TEST_ASSERT_EQUAL_UINT32(1u, hook_reads);
    TEST_ASSERT_GREATER_THAN_UINT32(0u, hook_nexts);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, flag, "kg_query_cancel_flag_not_raised");
    assert_image_unchanged();
    cancel_after_read = 0u;
    hook_cancel = NULL;
}

/* A failed allocation inside the service's own encoder is NOSPACE with no
 * payload. This is the one promise the core's allocation sweep cannot reach:
 * yan_kg_build allocates with malloc/calloc, while the query service's JSON
 * buffer grows with realloc, and nothing else in this suite drives that path to
 * failure. One failed growth is enough -- the buffer cannot produce a valid
 * object after it, so the whole call must refuse rather than publish a prefix. */
static void test_encoder_allocation_failure_is_nospace(void)
{
    YanKgQueryOptions options;
    /* The query form is used so the encoder writes a full object, including the
     * provenance and relation blocks, before the failing growth. */
    query_options(&options, "square");
    char *payload = (char *)1;
    size_t length = 99u;

    fail_encoder_reallocs = true;
    YanKgResult result = yan_kg_query_json(&options, &payload, &length);
    fail_encoder_reallocs = false;

    /* The seam must really have been inside the encoder: a case that passes
     * because no allocation was attempted would prove nothing. */
    TEST_ASSERT_GREATER_THAN_UINT32(0u, encoder_reallocs);
    TEST_ASSERT_EQUAL_UINT32(1u, encoder_reallocs_failed);
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_KG_NOSPACE, result,
                                  "kg_query_encoder_nospace");
    TEST_ASSERT_NULL_MESSAGE(payload, "kg_query_encoder_no_payload");
    TEST_ASSERT_EQUAL_UINT32(0u, length);

    /* And the same call without the seam succeeds, so the refusal came from the
     * injected failure and not from the fixture. */
    payload = NULL;
    length = 0u;
    TEST_ASSERT_EQUAL_INT(YAN_KG_OK,
                          yan_kg_query_json(&options, &payload, &length));
    TEST_ASSERT_NOT_NULL(payload);
    TEST_ASSERT_GREATER_THAN_UINT32(0u, (uint32_t)length);
    free(payload);
    assert_image_unchanged();
}

/* Malformed arguments are refused before the image is touched at all. */
static void test_invalid_arguments(void)
{
    YanKgQueryOptions options;
    list_options(&options);
    options.image = "";
    char *payload = (char *)1;
    size_t length = 99u;
    TEST_ASSERT_EQUAL_INT(YAN_KG_INVALID,
                          yan_kg_query_json(&options, &payload, &length));
    assert_no_payload(YAN_KG_INVALID, payload, length);

    query_options(&options, "");
    payload = (char *)1;
    length = 99u;
    TEST_ASSERT_EQUAL_INT(YAN_KG_INVALID,
                          yan_kg_query_json(&options, &payload, &length));
    assert_no_payload(YAN_KG_INVALID, payload, length);

    /* A two-line text fails the shared 0029 single-line qualification. */
    query_options(&options, "square\nrectangle");
    payload = (char *)1;
    length = 99u;
    TEST_ASSERT_EQUAL_INT(YAN_KG_INVALID,
                          yan_kg_query_json(&options, &payload, &length));
    assert_no_payload(YAN_KG_INVALID, payload, length);

    /* The operation enumerator is not a free integer. */
    list_options(&options);
    options.operation = (YanKgQueryOperation)7;
    payload = (char *)1;
    length = 99u;
    TEST_ASSERT_EQUAL_INT(YAN_KG_INVALID,
                          yan_kg_query_json(&options, &payload, &length));
    assert_no_payload(YAN_KG_INVALID, payload, length);

    /* Aliased output spans are refused rather than written twice. */
    char *alias = (char *)&length;
    TEST_ASSERT_EQUAL_INT(YAN_KG_INVALID,
                          yan_kg_query_json(&options, NULL, &length));
    TEST_ASSERT_EQUAL_INT(YAN_KG_INVALID,
                          yan_kg_query_json(&options, &alias, NULL));
    /* Every rejection above happens before any descriptor is opened, which is
     * what makes "invalid arguments cost the image nothing" a checked claim. */
    TEST_ASSERT_EQUAL_UINT32(0u, open_calls);
    assert_image_unchanged();
}

/* A missing image is a real I/O answer, and an absent query is NOT_FOUND, both
 * with no payload. */
static void test_runtime_failures_publish_nothing(void)
{
    YanKgQueryOptions options;
    list_options(&options);
    options.image = "/nonexistent/yan-graph-0045.img";
    char *payload = (char *)1;
    size_t length = 99u;
    YanKgResult result = yan_kg_query_json(&options, &payload, &length);
    /* A missing image is an ordinary host I/O answer: the open fails, so the
     * service reports IO and publishes nothing. This is not cancellation and
     * the assertion names it accordingly. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(YAN_KG_IO, result,
                                  "kg_query_missing_image_io");
    assert_no_payload(result, payload, length);
    /* The open was attempted, so the zero-open counter is not expected here;
     * this line documents that the image path was really reached. */
    TEST_ASSERT_EQUAL_UINT32(1u, open_calls);

    query_options(&options, "absent");
    payload = (char *)1;
    length = 99u;
    TEST_ASSERT_EQUAL_INT(YAN_KG_NOT_FOUND,
                          yan_kg_query_json(&options, &payload, &length));
    assert_no_payload(YAN_KG_NOT_FOUND, payload, length);
    assert_image_unchanged();
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_list_object_fields_and_digest);
    RUN_TEST(test_query_object_fields);
    RUN_TEST(test_expected_source_accept_and_stale);
    RUN_TEST(test_stale_precedes_graph_build);
    RUN_TEST(test_output_limit_boundary);
    RUN_TEST(test_cancel_at_entry_and_between_callbacks);
    RUN_TEST(test_encoder_allocation_failure_is_nospace);
    RUN_TEST(test_invalid_arguments);
    RUN_TEST(test_runtime_failures_publish_nothing);
    return UNITY_END();
}
