#include "semantic.h"
#include "unity.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>

#define BLOB_CAP (256u + 32u * 4160u)
/* 0036-R6 capacity and skipped-header cases need more than the original 32
 * fixture entries; the store blob stays small because only one deep entry and
 * small indexes are byte-inspected. */
#define FAKE_MAX 96u
typedef struct {
    char names[FAKE_MAX][32];
    const uint8_t *texts[FAKE_MAX];
    uint8_t repeated[FAKE_MAX];
    uint32_t sizes[FAKE_MAX], slots[FAKE_MAX], count;
    uint32_t reads, nexts, closed, starts, embeds, stops, writes, publishes;
    uint32_t callbacks, sink_limit;
    bool source_fail, fail_after_query, query_done, source_is_closed;
    bool reenter, reenter_ok, precision;
    YanSemanticResult start_error, embed_error, stop_error, close_error;
    YanSemanticResult read_close_error, begin_error, write_error, finish_error, publish_error;
    bool exists, reading, writing;
    bool large_store; /* count record bytes without keeping the blob */
    size_t stored_size, temporary_size;
    uint8_t stored[BLOB_CAP], temporary[BLOB_CAP];
    YanSemanticRow rows[20];
    uint8_t snippets[20][2048];
} Fake;
static Fake fake;
static YanSemantic ctx;
static YanSemanticIdentity identity;
static YanSemanticSummary summary;
static YanSemanticEvidence *watched;
static YanSemanticEvidence watched_before;

static YanSemanticResult next_file(void *opaque, uint32_t *cursor, YanSemanticFile *out)
{
    Fake *f = opaque;
    ++f->nexts;
    if (*cursor >= f->count) return YAN_SEMANTIC_MISSING;
    memset(out, 0, sizeof *out);
    memcpy(out->name, f->names[*cursor], 32);
    out->size = f->sizes[*cursor]; out->slot = f->slots[*cursor]; ++*cursor;
    return YAN_SEMANTIC_OK;
}
static YanSemanticResult read_source(void *opaque, const char *name, uint32_t off,
                                      uint8_t *bytes, uint32_t n)
{
    Fake *f = opaque; ++f->reads;
    if (f->source_fail || f->source_is_closed || (f->query_done && f->fail_after_query))
        return YAN_SEMANTIC_IO;
    for (uint32_t i = 0; i < f->count; ++i) {
        if (strcmp(name, f->names[i]) != 0) continue;
        if (off > f->sizes[i] || n > f->sizes[i] - off) return YAN_SEMANTIC_IO;
        if (f->texts[i] != NULL) memcpy(bytes, f->texts[i] + off, n);
        else memset(bytes, f->repeated[i], n);
        return YAN_SEMANTIC_OK;
    }
    return YAN_SEMANTIC_IO;
}
static YanSemanticResult close_source(void *opaque)
{
    Fake *f = opaque; ++f->closed; f->source_is_closed = true;
    return f->close_error;
}
static YanSemanticResult start(void *opaque)
{
    Fake *f = opaque; ++f->starts;
    return f->start_error;
}
static YanSemanticResult embed(void *opaque, const uint8_t *text, size_t length, float *out)
{
    Fake *f = opaque; ++f->embeds;
    if (f->reenter) {
        uint32_t reads = f->reads, starts = f->starts;
        YanSemanticSummary marker; memset(&marker, 0x55, sizeof marker);
        YanSemanticSummary before = marker;
        f->reenter_ok = yan_semantic_rebuild(&ctx, &marker) == YAN_SEMANTIC_BUSY &&
            memcmp(&before, &marker, sizeof marker) == 0 && f->reads == reads &&
            f->starts == starts;
    }
    if (length >= 9 && memcmp(text, "Instruct:", 9) == 0) f->query_done = true;
    for (uint32_t i = 0; i < 1024; ++i) out[i] = 0;
    if (length && text[0] == 'b') out[1] = 1;
    else out[0] = 1;
    if (f->precision) {
        out[0] = 1;
        out[1] = f->query_done ? 0.0f : 2.0f;
    }
    return f->embed_error;
}
static YanSemanticResult stop(void *opaque)
{
    Fake *f = opaque; ++f->stops; return f->stop_error;
}
static YanSemanticResult open_read(void *opaque, uint64_t *size)
{
    Fake *f = opaque;
    if (!f->exists) return YAN_SEMANTIC_MISSING;
    f->reading = true; *size = f->stored_size; return YAN_SEMANTIC_OK;
}
static YanSemanticResult read_index(void *opaque, uint64_t offset, uint8_t *out, size_t n)
{
    Fake *f = opaque;
    if (!f->reading || offset > f->stored_size || n > f->stored_size - offset)
        return YAN_SEMANTIC_IO;
    memcpy(out, f->stored + (size_t)offset, n); return YAN_SEMANTIC_OK;
}
static YanSemanticResult close_read(void *opaque)
{
    Fake *f = opaque; f->reading = false; return f->read_close_error;
}
static YanSemanticResult begin_write(void *opaque)
{
    Fake *f = opaque;
    if (f->begin_error) return f->begin_error;
    f->writing = true; f->temporary_size = 0; return YAN_SEMANTIC_OK;
}
static YanSemanticResult write_index(void *opaque, const uint8_t *in, size_t n)
{
    Fake *f = opaque; ++f->writes;
    if (f->write_error) return f->write_error;
    if (!f->writing) return YAN_SEMANTIC_IO;
    if (!f->large_store) {
        if (n > BLOB_CAP - f->temporary_size) return YAN_SEMANTIC_IO;
        memcpy(f->temporary + f->temporary_size, in, n);
    }
    f->temporary_size += n;
    return YAN_SEMANTIC_OK;
}
static YanSemanticResult finish_write(void *opaque)
{
    Fake *f = opaque; f->writing = false; return f->finish_error;
}
static YanSemanticResult publish(void *opaque)
{
    Fake *f = opaque;
    if (f->publish_error) return f->publish_error;
    if (!f->source_is_closed) return YAN_SEMANTIC_IO;
    if (!f->large_store) memcpy(f->stored, f->temporary, f->temporary_size);
    f->stored_size = f->temporary_size; f->exists = true; ++f->publishes;
    return YAN_SEMANTIC_OK;
}
static void discard(void *opaque)
{
    Fake *f = opaque; f->writing = false; f->temporary_size = 0;
}
static bool sink(void *opaque, const YanSemanticRow *row)
{
    Fake *f = opaque;
    TEST_ASSERT_TRUE_MESSAGE(f->source_is_closed, "no source I/O after first callback");
    TEST_ASSERT_EQUAL_UINT32(f->starts, f->stops);
    if (f->callbacks == f->sink_limit) return false;
    TEST_ASSERT_LESS_THAN_UINT32(20, f->callbacks);
    uint32_t i = f->callbacks++;
    f->rows[i] = *row;
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(2048, row->raw_length);
    memcpy(f->snippets[i], row->raw, row->raw_length);
    return true;
}
static bool evidence_sink(void *opaque, const YanSemanticRow *row)
{
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&watched_before, watched, sizeof *watched,
                                      "evidence_not_published_during_callback");
    uint32_t reads = fake.reads, nexts = fake.nexts;
    TEST_ASSERT_EQUAL_MESSAGE(YAN_SEMANTIC_BUSY,
        yan_semantic_search_evidence(&ctx, NULL, 100, NULL, NULL, NULL),
        "evidence_reentrant_busy");
    TEST_ASSERT_EQUAL_UINT32(reads, fake.reads);
    TEST_ASSERT_EQUAL_UINT32(nexts, fake.nexts);
    return sink(opaque, row);
}
static YanSemanticResult bind_capacity(uint32_t capacity)
{
    YanSemanticSource source = {&fake, sizeof fake, next_file, read_source, capacity};
    YanSemanticProvider provider = {&fake, sizeof fake, start, embed, stop};
    YanSemanticStore store = {&fake, sizeof fake, open_read, read_index, close_read,
        begin_write, write_index, finish_write, publish, discard};
    fake.source_is_closed = false; fake.query_done = false;
    return yan_semantic_init(&ctx, &source, &provider, &store, &identity, close_source);
}
static YanSemanticResult bind(void) { return bind_capacity(63u); }
static void add(const char *name, const char *text)
{
    uint32_t i = fake.count++;
    memcpy(fake.names[i], name, strlen(name) + 1); fake.texts[i] = (const uint8_t *)text;
    fake.sizes[i] = (uint32_t)strlen(text); fake.slots[i] = i;
}
void setUp(void)
{
    memset(&fake, 0, sizeof fake); memset(&ctx, 0, sizeof ctx);
    memset(&identity, 0x31, sizeof identity); memset(&summary, 0x55, sizeof summary);
    fake.sink_limit = 20;
    watched = NULL;
}
void tearDown(void) {}
static void rebuild(void)
{
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, bind());
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_rebuild(&ctx, &summary));
}
static void test_rebuild_layout_status_and_readonly(void)
{
    add("z", "beta\n"); add("a", "alpha\n"); rebuild();
    TEST_ASSERT_EQUAL_UINT32(2, summary.segments);
    TEST_ASSERT_EQUAL_UINT32(0, summary.skipped);
    TEST_ASSERT_EQUAL_UINT32(1, fake.publishes);
    TEST_ASSERT_EQUAL_UINT32(256 + 2 * 4160, fake.stored_size);
    TEST_ASSERT_EQUAL_MEMORY("YANSEM1\0", fake.stored, 8);
    TEST_ASSERT_EQUAL_STRING("a", fake.stored + 256);
    uint32_t starts = fake.starts, writes = fake.writes;
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, bind());
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_status(&ctx, &summary));
    TEST_ASSERT_EQUAL_MESSAGE(YAN_SEMANTIC_INDEX_READY, summary.state,
                              "semantic_status_ready");
    TEST_ASSERT_EQUAL_UINT32(starts, fake.starts);
    TEST_ASSERT_EQUAL_UINT32(writes, fake.writes);
}
static void test_search_ranking_raw_and_ties(void)
{
    add("b", "beta"); add("z", "alphaZ"); add("a", "alphaA"); rebuild();
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, bind());
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_search(&ctx,
        (const uint8_t *)"q", 1, sink, &fake, &summary));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(3, fake.callbacks, "semantic_rank_three_rows");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("a", fake.rows[0].provenance.name,
                                     "semantic_rank_score_desc");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("z", fake.rows[1].provenance.name,
                                     "semantic_rank_tie_second");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("b", fake.rows[2].provenance.name,
                                     "semantic_rank_low_last");
    TEST_ASSERT_TRUE_MESSAGE(fake.rows[0].score == 1.0, "semantic_rank_score_top");
    TEST_ASSERT_TRUE_MESSAGE(fake.rows[2].score == 0.0, "semantic_rank_score_zero");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE("alphaA", fake.snippets[0], 6,
                                     "semantic_rank_raw_provenance");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(3, summary.shown, "semantic_rank_shown");
}
static void test_missing_stale_and_corrupt_no_provider_or_output(void)
{
    add("a", "a");
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, bind());
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_status(&ctx, &summary));
    TEST_ASSERT_EQUAL_MESSAGE(YAN_SEMANTIC_INDEX_MISSING, summary.state,
                              "semantic_status_missing");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, fake.starts, "semantic_missing_no_provider");
    rebuild(); fake.texts[0] = (const uint8_t *)"changed"; fake.sizes[0] = 7;
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, bind());
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_status(&ctx, &summary));
    TEST_ASSERT_EQUAL_MESSAGE(YAN_SEMANTIC_INDEX_STALE, summary.state,
                              "semantic_status_stale_identity");
    uint32_t starts = fake.starts;
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, bind());
    TEST_ASSERT_EQUAL_MESSAGE(YAN_SEMANTIC_STALE, yan_semantic_search(&ctx,
        (const uint8_t *)"q", 1, sink, &fake, &summary), "semantic_search_stale");
    TEST_ASSERT_EQUAL_UINT32(starts, fake.starts);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, fake.callbacks, "semantic_stale_no_match");
    fake.stored[0] ^= 1;
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, bind());
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_status(&ctx, &summary));
    TEST_ASSERT_EQUAL_MESSAGE(YAN_SEMANTIC_INDEX_CORRUPT, summary.state,
                              "semantic_status_corrupt_magic");
}
static void test_publication_failures_preserve_previous_bytes(void)
{
    add("a", "a"); rebuild();
    static uint8_t old[BLOB_CAP]; size_t n = fake.stored_size;
    memcpy(old, fake.stored, n);
    YanSemanticResult *errors[] = {&fake.start_error, &fake.embed_error, &fake.stop_error,
        &fake.close_error, &fake.write_error, &fake.finish_error, &fake.publish_error};
    for (size_t i = 0; i < sizeof errors / sizeof errors[0]; ++i) {
        *errors[i] = YAN_SEMANTIC_IO;
        TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, bind());
        YanSemanticSummary before = summary;
        TEST_ASSERT_EQUAL(YAN_SEMANTIC_IO, yan_semantic_rebuild(&ctx, &summary));
        TEST_ASSERT_EQUAL_MEMORY(old, fake.stored, n);
        TEST_ASSERT_EQUAL_MEMORY(&before, &summary, sizeof summary);
        TEST_ASSERT_FALSE(fake.writing);
        *errors[i] = YAN_SEMANTIC_OK;
    }
}
static void test_late_reread_failure_zero_callbacks(void)
{
    add("a", "a"); rebuild();
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, bind()); fake.fail_after_query = true;
    YanSemanticSummary before = summary;
    TEST_ASSERT_EQUAL_MESSAGE(YAN_SEMANTIC_IO, yan_semantic_search(&ctx,
        (const uint8_t *)"q", 1, sink, &fake, &summary), "semantic_late_reread_io");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, fake.callbacks,
                                     "semantic_late_reread_no_callbacks");
    TEST_ASSERT_EQUAL_MEMORY(&before, &summary, sizeof summary);
}
static void test_stop_prefix_busy_and_invalid_query(void)
{
    add("a", "a"); add("b", "b"); fake.reenter = true; rebuild();
    TEST_ASSERT_TRUE(fake.reenter_ok);
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, bind());
    uint32_t reads = fake.reads, starts = fake.starts;
    TEST_ASSERT_EQUAL_MESSAGE(YAN_SEMANTIC_INVALID, yan_semantic_search(&ctx,
        (const uint8_t *)"a\0b", 3, sink, &fake, &summary), "semantic_invalid_query");
    TEST_ASSERT_EQUAL_UINT32(reads, fake.reads);
    TEST_ASSERT_EQUAL_UINT32(starts, fake.starts);
    fake.sink_limit = 1; YanSemanticSummary before = summary;
    TEST_ASSERT_EQUAL_MESSAGE(YAN_SEMANTIC_STOPPED, yan_semantic_search(&ctx,
        (const uint8_t *)"q", 1, sink, &fake, &summary), "semantic_stop_is_stopped");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, fake.callbacks, "semantic_stop_prefix_one");
    TEST_ASSERT_EQUAL_MEMORY(&before, &summary, sizeof summary);
}
static void test_top20_after_global_ranking(void)
{
    static char names[25][32];
    for (uint32_t i = 0; i < 25; ++i) {
        names[i][0] = (char)('a' + i); names[i][1] = '\0';
        add(names[i], i == 24 ? "alpha" : "beta");
    }
    rebuild(); TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, bind());
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_search(&ctx,
        (const uint8_t *)"q", 1, sink, &fake, &summary));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(20, summary.shown, "semantic_top20_shown");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("y", fake.rows[0].provenance.name,
                                     "semantic_top20_first");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(25, summary.segments,
                                     "semantic_summary_full_segments");
}
static void test_search_tie_break_is_ascii_name(void)
{
    add("a", "alpha"); add("b", "beta"); add("z", "beta"); rebuild();
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, bind());
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_search(&ctx,
        (const uint8_t *)"q", 1, sink, &fake, &summary));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("a", fake.rows[0].provenance.name,
                                     "semantic_tie_top_unique");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("b", fake.rows[1].provenance.name,
                                     "semantic_tie_ascii_name");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("z", fake.rows[2].provenance.name,
                                     "semantic_tie_ascii_last");
}
static void test_search_source_close_error_stops_before_output(void)
{
    add("a", "a"); rebuild();
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, bind());
    fake.close_error = YAN_SEMANTIC_IO;
    YanSemanticSummary before = summary;
    TEST_ASSERT_EQUAL_MESSAGE(YAN_SEMANTIC_IO, yan_semantic_search(&ctx,
        (const uint8_t *)"q", 1, sink, &fake, &summary),
        "semantic_search_source_close_io");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, fake.callbacks,
                                     "semantic_search_close_no_callbacks");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&before, &summary, sizeof summary,
                                     "semantic_search_close_summary_kept");
}
static void test_finish_error_is_not_published(void)
{
    add("a", "a"); rebuild();
    static uint8_t old[BLOB_CAP]; size_t n = fake.stored_size;
    memcpy(old, fake.stored, n);
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, bind());
    fake.finish_error = YAN_SEMANTIC_IO;
    YanSemanticSummary before = summary;
    TEST_ASSERT_EQUAL_MESSAGE(YAN_SEMANTIC_IO, yan_semantic_rebuild(&ctx, &summary),
                              "semantic_finish_error_not_published");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(old, fake.stored, n,
                                     "semantic_finish_error_old_bytes");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&before, &summary, sizeof summary,
                                     "semantic_finish_error_summary_kept");
}
static void test_begin_write_failure_preserves_previous_bytes(void)
{
    add("a", "a"); rebuild();
    static uint8_t old[BLOB_CAP]; size_t n = fake.stored_size;
    memcpy(old, fake.stored, n);
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, bind());
    fake.begin_error = YAN_SEMANTIC_IO;
    YanSemanticSummary before = summary;
    TEST_ASSERT_EQUAL_MESSAGE(YAN_SEMANTIC_IO, yan_semantic_rebuild(&ctx, &summary),
                              "semantic_begin_write_failure");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(old, fake.stored, n,
                                     "semantic_begin_write_old_bytes");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&before, &summary, sizeof summary,
                                     "semantic_begin_write_summary_kept");
    TEST_ASSERT_FALSE(fake.writing);
}

static YanSemanticIndexState status_state(void)
{
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, bind());
    YanSemanticSummary observed;
    memset(&observed, 0x5A, sizeof observed);
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_status(&ctx, &observed));
    return observed.state;
}

static void rehash_index_payload(uint32_t segments)
{
    YanSemanticSha256 hash;
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_sha256_init(&hash));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK,
        yan_semantic_sha256_update(&hash, fake.stored + 256u,
                                   (size_t)segments * 4160u));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK,
        yan_semantic_sha256_final(&hash, fake.stored + 160u));
}

static void test_index_corruption_variants(void)
{
    add("a", "a");
    rebuild();
    static uint8_t snapshot[BLOB_CAP];
    const size_t size = fake.stored_size;
    memcpy(snapshot, fake.stored, size);

    /* Flipped magic is structurally corrupt. */
    fake.stored[0] ^= 1u;
    TEST_ASSERT_EQUAL_MESSAGE(YAN_SEMANTIC_INDEX_CORRUPT, status_state(),
                              "semantic_index_magic");
    memcpy(fake.stored, snapshot, size);

    /* Flipped payload hash no longer matches the records. */
    fake.stored[160] ^= 1u;
    TEST_ASSERT_EQUAL_MESSAGE(YAN_SEMANTIC_INDEX_CORRUPT, status_state(),
                              "semantic_index_payload_mismatch");
    memcpy(fake.stored, snapshot, size);

    /* A record byte change breaks the payload hash. */
    fake.stored[256u + 4160u - 1u] ^= 1u;
    TEST_ASSERT_EQUAL_MESSAGE(YAN_SEMANTIC_INDEX_CORRUPT, status_state(),
                              "semantic_index_record_payload");
    memcpy(fake.stored, snapshot, size);

    /* A wrong skipped count with matching identity is corrupt. */
    fake.stored[20] = 5u;
    TEST_ASSERT_EQUAL_MESSAGE(YAN_SEMANTIC_INDEX_CORRUPT, status_state(),
                              "semantic_index_skipped_mismatch");
    memcpy(fake.stored, snapshot, size);

    /* One trailing byte changes the exact file length. */
    fake.stored_size = size + 1u;
    fake.stored[size] = 0u;
    TEST_ASSERT_EQUAL_MESSAGE(YAN_SEMANTIC_INDEX_CORRUPT, status_state(),
                              "semantic_index_exact_size");
    fake.stored_size = size;

    /* A nonfinite vector with a matching payload hash. */
    fake.stored[256u + 64u] = 0x00u;
    fake.stored[256u + 65u] = 0x00u;
    fake.stored[256u + 66u] = 0x80u;
    fake.stored[256u + 67u] = 0x7Fu;
    rehash_index_payload(1u);
    TEST_ASSERT_EQUAL_MESSAGE(YAN_SEMANTIC_INDEX_CORRUPT, status_state(),
                              "semantic_index_nonfinite");
    memcpy(fake.stored, snapshot, size);

    /* Forged provenance that passes internal ranges but not the corpus. */
    fake.stored[256u + 40u] = 9u;
    fake.stored[256u + 44u] = 10u;
    rehash_index_payload(1u);
    TEST_ASSERT_EQUAL_MESSAGE(YAN_SEMANTIC_INDEX_CORRUPT, status_state(),
                              "semantic_index_provenance_mismatch");
    memcpy(fake.stored, snapshot, size);

    /* Model, runner and config identity changes are stale, not corrupt. */
    identity.model[0] ^= 1u;
    TEST_ASSERT_EQUAL_MESSAGE(YAN_SEMANTIC_INDEX_STALE, status_state(),
                              "semantic_index_model_stale");
    identity.model[0] ^= 1u;
    identity.runner[0] ^= 1u;
    TEST_ASSERT_EQUAL_MESSAGE(YAN_SEMANTIC_INDEX_STALE, status_state(),
                              "semantic_index_runner_stale");
    identity.runner[0] ^= 1u;
    identity.config[0] ^= 1u;
    TEST_ASSERT_EQUAL_MESSAGE(YAN_SEMANTIC_INDEX_STALE, status_state(),
                              "semantic_index_config_stale");
    identity.config[0] ^= 1u;

    TEST_ASSERT_EQUAL_MESSAGE(YAN_SEMANTIC_INDEX_READY, status_state(),
                              "semantic_index_ready");
}

/* Hash-consistent record fields must be rejected by the structural guards,
 * not merely by the payload hash. Changing the model identity makes a missing
 * guard fall through to the identity comparison and report STALE instead of
 * CORRUPT, so each case owns exactly one guard. */
static void test_index_structural_record_guards(void)
{
    add("a", "a");
    rebuild();
    static uint8_t snapshot[BLOB_CAP];
    const size_t size = fake.stored_size;
    memcpy(snapshot, fake.stored, size);

    /* scalar_count below the 1..512 range. */
    fake.stored[256u + 48u] = 0u;
    fake.stored[256u + 49u] = 0u;
    fake.stored[256u + 50u] = 0u;
    fake.stored[256u + 51u] = 0u;
    rehash_index_payload(1u);
    identity.model[0] ^= 1u;
    TEST_ASSERT_EQUAL_MESSAGE(YAN_SEMANTIC_INDEX_CORRUPT, status_state(),
                              "semantic_record_scalar_zero");
    identity.model[0] ^= 1u;
    memcpy(fake.stored, snapshot, size);

    /* scalar_count above 512. */
    fake.stored[256u + 48u] = 0x01u;
    fake.stored[256u + 49u] = 0x02u; /* 0x0201 = 513 */
    fake.stored[256u + 50u] = 0x00u;
    fake.stored[256u + 51u] = 0x00u;
    rehash_index_payload(1u);
    identity.model[0] ^= 1u;
    TEST_ASSERT_EQUAL_MESSAGE(YAN_SEMANTIC_INDEX_CORRUPT, status_state(),
                              "semantic_record_scalar_max");
    identity.model[0] ^= 1u;
    memcpy(fake.stored, snapshot, size);

    /* A nonzero reserved byte at record offset 52. */
    fake.stored[256u + 52u] = 1u;
    rehash_index_payload(1u);
    identity.model[0] ^= 1u;
    TEST_ASSERT_EQUAL_MESSAGE(YAN_SEMANTIC_INDEX_CORRUPT, status_state(),
                              "semantic_record_reserved_zero");
    identity.model[0] ^= 1u;
    memcpy(fake.stored, snapshot, size);

    /* Nonzero padding after the name NUL. */
    fake.stored[256u + 2u] = (uint8_t)'X';
    rehash_index_payload(1u);
    identity.model[0] ^= 1u;
    TEST_ASSERT_EQUAL_MESSAGE(YAN_SEMANTIC_INDEX_CORRUPT, status_state(),
                              "semantic_record_name_padding");
    identity.model[0] ^= 1u;
    memcpy(fake.stored, snapshot, size);

    /* A name that is not a legal YanFS name. */
    fake.stored[256u] = (uint8_t)'.';
    fake.stored[256u + 1u] = 0u;
    rehash_index_payload(1u);
    identity.model[0] ^= 1u;
    TEST_ASSERT_EQUAL_MESSAGE(YAN_SEMANTIC_INDEX_CORRUPT, status_state(),
                              "semantic_record_name_invalid");
    identity.model[0] ^= 1u;
    memcpy(fake.stored, snapshot, size);
}

static void test_holder_alias_rejection(void)
{
    add("a", "a");
    rebuild();

    TEST_ASSERT_EQUAL_MESSAGE(YAN_SEMANTIC_INVALID,
                      yan_semantic_rebuild(&ctx, (YanSemanticSummary *)&ctx),
                      "semantic_alias_rebuild_out");
    TEST_ASSERT_EQUAL_MESSAGE(YAN_SEMANTIC_INVALID,
                      yan_semantic_status(&ctx, (YanSemanticSummary *)&ctx),
                      "semantic_alias_status_out");
    TEST_ASSERT_EQUAL_MESSAGE(YAN_SEMANTIC_INVALID,
                      yan_semantic_search(&ctx, (const uint8_t *)"q", 1u, sink,
                                          &fake, (YanSemanticSummary *)&ctx),
                      "semantic_alias_search_out");

    /* A query that lies inside a binding context is rejected before I/O. */
    const uint32_t starts = fake.starts;
    TEST_ASSERT_EQUAL_MESSAGE(YAN_SEMANTIC_INVALID,
                      yan_semantic_search(&ctx, (const uint8_t *)fake.names[0],
                                          1u, sink, &fake, &summary),
                      "semantic_query_alias_binding");
    TEST_ASSERT_EQUAL_UINT32(starts, fake.starts);
}

static void test_missing_and_corrupt_search_zero_match(void)
{
    add("a", "a");
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, bind());
    TEST_ASSERT_EQUAL_MESSAGE(YAN_SEMANTIC_MISSING,
                      yan_semantic_search(&ctx, (const uint8_t *)"q", 1u, sink,
                                          &fake, &summary),
                      "semantic_search_missing");
    TEST_ASSERT_EQUAL_UINT32(0u, fake.callbacks);
    TEST_ASSERT_EQUAL_UINT32(0u, fake.starts);

    rebuild();
    fake.stored[0] ^= 1u;
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, bind());
    YanSemanticSummary before = summary;
    TEST_ASSERT_EQUAL_MESSAGE(YAN_SEMANTIC_CORRUPT,
                      yan_semantic_search(&ctx, (const uint8_t *)"q", 1u, sink,
                                          &fake, &summary),
                      "semantic_search_corrupt");
    TEST_ASSERT_EQUAL_UINT32(0u, fake.callbacks);
    TEST_ASSERT_EQUAL_MEMORY(&before, &summary, sizeof summary);
}
static void test_index_close_error_cannot_be_a_successful_corrupt_observation(void)
{
    add("a", "a"); rebuild(); fake.stored[0] ^= 1;
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, bind());
    fake.read_close_error = YAN_SEMANTIC_IO;
    YanSemanticSummary before = summary;
    TEST_ASSERT_EQUAL_MESSAGE(YAN_SEMANTIC_IO, yan_semantic_status(&ctx, &summary),
                              "semantic_index_close_error");
    TEST_ASSERT_EQUAL_MEMORY(&before, &summary, sizeof summary);
    TEST_ASSERT_FALSE(fake.reading);
}
static void test_wrapped_context_and_binding_query_rejected_before_dereference(void)
{
    YanSemantic *wrapped = (YanSemantic *)(uintptr_t)(UINTPTR_MAX - 1);
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID, yan_semantic_init(wrapped, NULL, NULL, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID, yan_semantic_status(wrapped, &summary));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID, yan_semantic_rebuild(wrapped, &summary));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID, yan_semantic_search(wrapped,
        (const uint8_t *)"q", 1, sink, NULL, &summary));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, bind());
    ctx.source.context = (void *)(uintptr_t)(UINTPTR_MAX - 1024);
    ctx.source.context_size = 1024;
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID, yan_semantic_search(&ctx,
        (const uint8_t *)ctx.source.context, 1, sink, NULL, &summary));
    TEST_ASSERT_EQUAL_UINT32(0, fake.reads);
}

static void test_evidence_full_raw_identity_and_late_publication(void)
{
    char text[513]; memset(text, 'x', 512); text[512] = 0;
    add("full", text); add("binary", "");
    fake.texts[1] = (const uint8_t *)"\0"; fake.sizes[1] = 1;
    rebuild();
    YanSemanticEvidence evidence; memset(&evidence, 0xA5, sizeof evidence);
    watched = &evidence; watched_before = evidence;
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, bind());
    TEST_ASSERT_EQUAL_MESSAGE(YAN_SEMANTIC_OK,
        yan_semantic_search_evidence(&ctx, (const uint8_t *)"q", 1,
            evidence_sink, &fake, &evidence), "evidence_search_ok");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(512, fake.rows[0].raw_length, "evidence_full_raw_length");
    TEST_ASSERT_EQUAL_MEMORY(text, fake.snippets[0], 512);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(fake.stored + 32, evidence.source_sha256, 32,
                                      "evidence_source_same_scan");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&identity, &evidence.identity, sizeof identity,
                                      "evidence_verified_identity");
    TEST_ASSERT_EQUAL_UINT32(1, evidence.summary.shown);
    TEST_ASSERT_EQUAL_UINT32(1, evidence.summary.segments);
    TEST_ASSERT_EQUAL_UINT32(1, evidence.summary.skipped);
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INDEX_READY, evidence.summary.state);
}
static void test_evidence_complete_holder_guards_and_busy(void)
{
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, bind());
    YanSemanticEvidence evidence; memset(&evidence, 0xA5, sizeof evidence);
    evidence.source_sha256[0] = 'q'; YanSemanticEvidence before = evidence;
    TEST_ASSERT_EQUAL_MESSAGE(YAN_SEMANTIC_INVALID,
        yan_semantic_search_evidence(&ctx, evidence.source_sha256, 1,
            sink, &fake, &evidence), "evidence_query_suffix_alias");
    TEST_ASSERT_EQUAL_MEMORY(&before, &evidence, sizeof before);
    for (unsigned i = 0; i < 3; ++i) {
        TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, bind());
        /* A real empty Fake keeps a weakened-guard mutant safe: it must fail
         * the INVALID assertion, rather than crashing in an invalid callback. */
        uint8_t *region = calloc(1, sizeof evidence + sizeof(Fake));
        TEST_ASSERT_NOT_NULL(region);
        Fake *protected = (Fake *)(void *)(region + sizeof evidence);
        YanSemanticEvidence *tail = (YanSemanticEvidence *)(void *)(region + 8);
        if (i == 0) { ctx.source.context = protected; ctx.source.context_size = sizeof *protected; }
        if (i == 1) { ctx.provider.context = protected; ctx.provider.context_size = sizeof *protected; }
        if (i == 2) { ctx.store.context = protected; ctx.store.context_size = sizeof *protected; }
        YanSemanticResult result = yan_semantic_search_evidence(&ctx,
            (const uint8_t *)"q", 1, sink, &fake, tail);
        free(region);
        TEST_ASSERT_EQUAL_MESSAGE(YAN_SEMANTIC_INVALID, result, "evidence_binding_suffix_alias");
    }
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, bind());
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID, yan_semantic_search_evidence(&ctx,
        (const uint8_t *)"q", 1, sink, &fake, (YanSemanticEvidence *)&ctx));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID, yan_semantic_search_evidence(&ctx,
        (const uint8_t *)"q", 1, sink, &fake,
        (YanSemanticEvidence *)(uintptr_t)(UINTPTR_MAX - 4u)));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INVALID, yan_semantic_search_evidence(&ctx,
        (const uint8_t *)"q", 1, sink, &fake, NULL));
    ctx.busy = true;
    TEST_ASSERT_EQUAL_MESSAGE(YAN_SEMANTIC_BUSY, yan_semantic_search_evidence(&ctx,
        NULL, 100, NULL, NULL, NULL), "evidence_busy_first");
    TEST_ASSERT_EQUAL_UINT32(0, fake.nexts);
    TEST_ASSERT_EQUAL_UINT32(0, fake.reads);
    TEST_ASSERT_EQUAL_UINT32(0, fake.starts);
    TEST_ASSERT_EQUAL_MEMORY(&before, &evidence, sizeof before);
}
static void test_evidence_all_failures_preserve_entire_holder(void)
{
    add("a", "alpha"); rebuild();
    for (unsigned i = 0; i < 7; ++i) {
        TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, bind());
        fake.callbacks = 0; fake.sink_limit = 20; fake.exists = true;
        fake.stop_error = fake.close_error = YAN_SEMANTIC_OK;
        fake.fail_after_query = false;
        YanSemanticResult expected;
        if (i == 0) { fake.exists = false; expected = YAN_SEMANTIC_MISSING; }
        else if (i == 1) { ctx.identity.model[0] ^= 1; expected = YAN_SEMANTIC_STALE; }
        else if (i == 2) { fake.stored[0] ^= 1; expected = YAN_SEMANTIC_CORRUPT; }
        else if (i == 3) { fake.stop_error = YAN_SEMANTIC_PROVIDER; expected = YAN_SEMANTIC_PROVIDER; }
        else if (i == 4) { fake.close_error = YAN_SEMANTIC_IO; expected = YAN_SEMANTIC_IO; }
        else if (i == 5) { fake.sink_limit = 0; expected = YAN_SEMANTIC_STOPPED; }
        else { fake.fail_after_query = true; expected = YAN_SEMANTIC_IO; }
        YanSemanticEvidence evidence; memset(&evidence, 0xA5, sizeof evidence);
        YanSemanticEvidence before = evidence;
        TEST_ASSERT_EQUAL(expected, yan_semantic_search_evidence(&ctx,
            (const uint8_t *)"q", 1, sink, &fake, &evidence));
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&before, &evidence, sizeof before,
                                          "evidence_failure_preserves_all");
        if (i == 2) fake.stored[0] ^= 1;
    }
}
static void test_evidence_empty_source_has_complete_identity(void)
{
    static const uint8_t empty_sha[32] = {
        0xe3,0xb0,0xc4,0x42,0x98,0xfc,0x1c,0x14,0x9a,0xfb,0xf4,0xc8,0x99,0x6f,0xb9,0x24,
        0x27,0xae,0x41,0xe4,0x64,0x9b,0x93,0x4c,0xa4,0x95,0x99,0x1b,0x78,0x52,0xb8,0x55};
    rebuild(); TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, bind());
    YanSemanticEvidence evidence;
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_search_evidence(&ctx,
        (const uint8_t *)"q", 1, sink, &fake, &evidence));
    TEST_ASSERT_EQUAL_MEMORY(empty_sha, evidence.source_sha256, 32);
    TEST_ASSERT_EQUAL_MEMORY(&identity, &evidence.identity, sizeof identity);
    TEST_ASSERT_EQUAL_UINT32(0, evidence.summary.shown);
    TEST_ASSERT_EQUAL_UINT32(0, fake.callbacks);
}
static void test_evidence_old_new_agree_and_keep_score_precision(void)
{
    fake.precision = true; add("b", "beta"); add("a", "alpha"); rebuild();
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, bind());
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_search(&ctx,
        (const uint8_t *)"q", 1, sink, &fake, &summary));
    YanSemanticProvenance first = fake.rows[0].provenance;
    double score = fake.rows[0].score; fake.callbacks = 0;
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, bind());
    YanSemanticEvidence evidence;
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_search_evidence(&ctx,
        (const uint8_t *)"q", 1, sink, &fake, &evidence));
    TEST_ASSERT_EQUAL_MEMORY(&first, &fake.rows[0].provenance, sizeof first);
    TEST_ASSERT_EQUAL_MEMORY(&summary, &evidence.summary, sizeof summary);
    TEST_ASSERT_TRUE_MESSAGE(fake.rows[0].score == 1.0 / sqrt(5.0),
                               "evidence_score_full_precision");
    TEST_ASSERT_TRUE(score == fake.rows[0].score);
}

static void add_repeated(const char *name, uint32_t size, uint8_t byte)
{
    uint32_t i = fake.count++;
    TEST_ASSERT_LESS_THAN_UINT32(FAKE_MAX, i);
    memcpy(fake.names[i], name, strlen(name) + 1);
    fake.texts[i] = NULL; fake.sizes[i] = size; fake.slots[i] = i;
    fake.repeated[i] = byte;
}
/* 0036-R6: the index header's skipped field is bounded by the mounted image's
 * runtime capacity. 64 skipped files are legal for N=1023 and must be accepted
 * with a matching corpus skipped count (a v1 63 bound would reject the header). */
static void test_index_skipped_uses_runtime_capacity(void)
{
    for (uint32_t i = 0; i < 64u; ++i) {
        char name[8];
        snprintf(name, sizeof name, "f%02u", (unsigned)i);
        add(name, "\xff");
    }
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, bind_capacity(1023u));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_rebuild(&ctx, &summary));
    TEST_ASSERT_EQUAL_UINT32(0, summary.segments);
    TEST_ASSERT_EQUAL_UINT32(64, summary.skipped);
    TEST_ASSERT_EQUAL_UINT32(1, fake.publishes);
    TEST_ASSERT_EQUAL_UINT8(64, fake.stored[20]);
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, bind_capacity(1023u));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_status(&ctx, &summary));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INDEX_READY, summary.state);
    TEST_ASSERT_EQUAL_UINT32(64, summary.skipped);
}
/* The same 64 skipped count is rejected when the mounted capacity is 63, and a
 * 64th file is corrupt for a v1 image. */
static void test_v1_capacity_rejects_skipped_64_and_64_files(void)
{
    add("bad", "\xff");
    rebuild();
    TEST_ASSERT_EQUAL_UINT32(1, summary.skipped);
    fake.stored[20] = 64u;
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, bind_capacity(63u));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_status(&ctx, &summary));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INDEX_CORRUPT, summary.state);

    fake.count = 0;
    for (uint32_t i = 0; i < 64u; ++i) {
        char name[8];
        snprintf(name, sizeof name, "g%02u", (unsigned)i);
        add(name, "x");
    }
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, bind_capacity(63u));
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_CORRUPT, yan_semantic_rebuild(&ctx, &summary));
}
/* 0036-R6: exactly 8192 segments build, the 8193rd returns LIMIT, and a failed
 * scan never touches the previously published index bytes. */
static void test_segment_limit_exact_and_no_partial_index(void)
{
    add_repeated("huge", 512u + 8191u * 448u, 'x');
    fake.large_store = true;
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, bind());
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, yan_semantic_rebuild(&ctx, &summary));
    TEST_ASSERT_EQUAL_UINT32(8192, summary.segments);
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_INDEX_READY, summary.state);
    TEST_ASSERT_EQUAL_UINT32(1, fake.publishes);
    const size_t published_size = fake.stored_size;
    const uint32_t publishes = fake.publishes;
    const YanSemanticSummary before = summary;

    fake.sizes[0] += 1u;
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, bind());
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_LIMIT, yan_semantic_rebuild(&ctx, &summary));
    TEST_ASSERT_EQUAL_UINT32(publishes, fake.publishes);
    TEST_ASSERT_EQUAL_UINT64(published_size, fake.stored_size);
    TEST_ASSERT_FALSE(fake.writing);
    TEST_ASSERT_EQUAL_MEMORY(&before, &summary, sizeof summary);
}
static void test_segment_limit_8193_preserves_small_index(void)
{
    add("a", "a");
    rebuild();
    static uint8_t old[BLOB_CAP];
    const size_t n = fake.stored_size;
    memcpy(old, fake.stored, n);
    fake.count = 0;
    add_repeated("huge", 512u + 8191u * 448u + 1u, 'x');
    const uint32_t publishes = fake.publishes;
    const YanSemanticSummary before = summary;
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_OK, bind());
    TEST_ASSERT_EQUAL(YAN_SEMANTIC_LIMIT, yan_semantic_rebuild(&ctx, &summary));
    TEST_ASSERT_EQUAL_MEMORY(old, fake.stored, n);
    TEST_ASSERT_EQUAL_UINT32(publishes, fake.publishes);
    TEST_ASSERT_EQUAL_UINT64(n, fake.stored_size);
    TEST_ASSERT_FALSE(fake.writing);
    TEST_ASSERT_EQUAL_MEMORY(&before, &summary, sizeof summary);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_rebuild_layout_status_and_readonly);
    RUN_TEST(test_search_ranking_raw_and_ties);
    RUN_TEST(test_missing_stale_and_corrupt_no_provider_or_output);
    RUN_TEST(test_publication_failures_preserve_previous_bytes);
    RUN_TEST(test_late_reread_failure_zero_callbacks);
    RUN_TEST(test_stop_prefix_busy_and_invalid_query);
    RUN_TEST(test_top20_after_global_ranking);
    RUN_TEST(test_search_tie_break_is_ascii_name);
    RUN_TEST(test_search_source_close_error_stops_before_output);
    RUN_TEST(test_finish_error_is_not_published);
    RUN_TEST(test_begin_write_failure_preserves_previous_bytes);
    RUN_TEST(test_index_corruption_variants);
    RUN_TEST(test_index_structural_record_guards);
    RUN_TEST(test_holder_alias_rejection);
    RUN_TEST(test_missing_and_corrupt_search_zero_match);
    RUN_TEST(test_index_close_error_cannot_be_a_successful_corrupt_observation);
    RUN_TEST(test_wrapped_context_and_binding_query_rejected_before_dereference);
    RUN_TEST(test_evidence_full_raw_identity_and_late_publication);
    RUN_TEST(test_evidence_complete_holder_guards_and_busy);
    RUN_TEST(test_evidence_all_failures_preserve_entire_holder);
    RUN_TEST(test_evidence_empty_source_has_complete_identity);
    RUN_TEST(test_evidence_old_new_agree_and_keep_score_precision);
    RUN_TEST(test_index_skipped_uses_runtime_capacity);
    RUN_TEST(test_v1_capacity_rejects_skipped_64_and_64_files);
    RUN_TEST(test_segment_limit_exact_and_no_partial_index);
    RUN_TEST(test_segment_limit_8193_preserves_small_index);
    return UNITY_END();
}
