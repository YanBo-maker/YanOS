/* 0029 Host semantic-search facade: corpus scan, index, ranking and result
 * delivery behind the fixed public API of host/semantic.h.
 *
 * The behavioural contract is docs/specs/0029-host-semantic-search.md,
 * "派生索引、身份与发布", "向量排名与API责任" and the three CLI sections.
 * This translation unit owns only orchestration: it borrows the caller's
 * source/provider/store callbacks, the corpus scanner and the pure text
 * primitives, and it owns bounded temporary heap (corpus workspace, up to
 * 8192 vectors, one encoded record, selected raw snippets). It contains no
 * model, network, filesystem, console or POSIX call.
 *
 * Rebuild order: validate context/holders, scan and hash the whole source
 * (eligibility, metadata and the 8193-segment limit all fail here), start the
 * provider, embed each canonical segment (document prefix `Title: ` + title
 * + "\n\n" only when the parsed metadata declared a title), stop the
 * provider, close the source, then begin/write/finish the store temporary and
 * publish. Every failure before publish discards the owned temporary and
 * leaves the previous index bytes and the caller summary untouched.
 *
 * Status/search reload the corpus and the index, validate all records
 * structurally before comparing identity, then compare records against the
 * current canonical corpus. status reports MISSING/CORRUPT/STALE/READY with
 * OK and no provider; search returns the matching error for a non-READY
 * index and only reaches the provider for a fully valid READY index.
 *
 * Search embeds the fixed instruction+query bytes, ranks every record by
 * exact cosine over all 1024 dimensions, re-reads every selected raw segment
 * before the first sink call, closes the source and index and stops the
 * provider, then delivers borrowed rows. A false sink stops with STOPPED and
 * leaves the summary untouched.
 *
 * Tests-first history: host/semantic.h and tests/test_semantic.c existed
 * first and were RED against an UNSUPPORTED stub; this source replaced it. */

#include "semantic.h"
#include "note_metadata_format.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------- utilities */

static bool span_ok(const void *base, size_t length)
{
    return (base != NULL || length == 0u) &&
           length <= UINTPTR_MAX - (uintptr_t)base;
}

static bool spans_overlap(const void *first, size_t first_length,
                          const void *second, size_t second_length)
{
    if (first_length == 0u || second_length == 0u) {
        return false;
    }
    const uintptr_t first_begin = (uintptr_t)first;
    const uintptr_t second_begin = (uintptr_t)second;
    if ((uint64_t)first_length > (uint64_t)(UINTPTR_MAX - first_begin) ||
        (uint64_t)second_length > (uint64_t)(UINTPTR_MAX - second_begin)) {
        return true;
    }
    return first_begin < second_begin + second_length &&
           second_begin < first_begin + first_length;
}

static bool float_finite(float value)
{
    return value <= FLT_MAX && value >= -FLT_MAX;
}

static bool double_finite(double value)
{
    return value <= DBL_MAX && value >= -DBL_MAX;
}

/* The persisted vector format is IEEE-754 binary32 little-endian. */
static bool binary32_supported(void)
{
    switch (sizeof(float)) {
    case 4: break;
    default: return false;
    }
    const float one = 1.0f;
    const float minus_two = -2.0f;
    uint32_t bits = 0u;
    memcpy(&bits, &one, sizeof bits);
    if (bits != UINT32_C(0x3F800000)) {
        return false;
    }
    memcpy(&bits, &minus_two, sizeof bits);
    return bits == UINT32_C(0xC0000000);
}

static uint32_t get_u32le(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void put_u32le(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static float get_f32le(const uint8_t *bytes)
{
    const uint32_t bits = get_u32le(bytes);
    float value = 0.0f;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static void put_f32le(uint8_t *bytes, float value)
{
    uint32_t bits = 0u;
    memcpy(&bits, &value, sizeof bits);
    put_u32le(bytes, bits);
}

/* ------------------------------------------------------- holder guarding */

/* True when a caller holder span conflicts with the facade or with any bound
 * binding context. Binding contexts may overlap each other. */
static bool holder_conflicts(const YanSemantic *ctx, const void *base,
                             size_t length)
{
    if (spans_overlap(base, length, ctx, sizeof *ctx)) {
        return true;
    }
    if (ctx->source.context_size != 0u &&
        spans_overlap(base, length, ctx->source.context,
                      ctx->source.context_size)) {
        return true;
    }
    if (ctx->provider.context_size != 0u &&
        spans_overlap(base, length, ctx->provider.context,
                      ctx->provider.context_size)) {
        return true;
    }
    if (ctx->store.context_size != 0u &&
        spans_overlap(base, length, ctx->store.context,
                      ctx->store.context_size)) {
        return true;
    }
    return false;
}

static YanSemanticResult guard_output(const YanSemantic *ctx,
                                      YanSemanticSummary *out)
{
    if (out == NULL) {
        return YAN_SEMANTIC_OK;
    }
    if (!span_ok(out, sizeof *out) || holder_conflicts(ctx, out, sizeof *out)) {
        return YAN_SEMANTIC_INVALID;
    }
    return YAN_SEMANTIC_OK;
}

/* --------------------------------------------------------- index format */

typedef struct {
    uint32_t segments;
    uint32_t skipped;
    uint8_t source[32];
    uint8_t model[32];
    uint8_t runner[32];
    uint8_t config[32];
    uint8_t payload[32];
} YanSemanticIndexMeta;

static const uint8_t yan_semantic_magic[8] = {
    (uint8_t)'Y', (uint8_t)'A', (uint8_t)'N', (uint8_t)'S',
    (uint8_t)'E', (uint8_t)'M', (uint8_t)'1', (uint8_t)0
};

static bool parse_index_header(const uint8_t *header, YanSemanticIndexMeta *meta,
                               uint32_t capacity)
{
    if (memcmp(header, yan_semantic_magic, sizeof yan_semantic_magic) != 0) {
        return false;
    }
    if (get_u32le(header + 8u) != UINT32_C(1) ||
        get_u32le(header + 12u) != YAN_SEMANTIC_DIMENSION) {
        return false;
    }
    const uint32_t segments = get_u32le(header + 16u);
    const uint32_t skipped = get_u32le(header + 20u);
    /* 0036-R6: skipped counts ineligible files, so it is bounded by this
     * mounted image's runtime capacity, not by the v1 constant 63. */
    if (segments > YAN_SEMANTIC_SEGMENT_MAX || skipped > capacity) {
        return false;
    }
    if (get_u32le(header + 24u) != YAN_SEMANTIC_HEADER_BYTES ||
        get_u32le(header + 28u) != YAN_SEMANTIC_RECORD_BYTES) {
        return false;
    }
    for (uint32_t i = 192u; i < 256u; ++i) {
        if (header[i] != 0u) {
            return false;
        }
    }
    meta->segments = segments;
    meta->skipped = skipped;
    memcpy(meta->source, header + 32u, 32u);
    memcpy(meta->model, header + 64u, 32u);
    memcpy(meta->runner, header + 96u, 32u);
    memcpy(meta->config, header + 128u, 32u);
    memcpy(meta->payload, header + 160u, 32u);
    return true;
}

/* Parses one 4160-byte record, validates its internal ranges/flags and, when
 * `vector` is non-NULL, copies the 1024 decoded binary32 values out. */
static bool parse_index_record(const uint8_t *record,
                               YanSemanticProvenance *out, float *vector)
{
    uint32_t name_length = 0u;
    while (name_length < 32u && record[name_length] != 0u) {
        ++name_length;
    }
    if (name_length == 0u || name_length >= 32u) {
        return false;
    }
    for (uint32_t i = name_length + 1u; i < 32u; ++i) {
        if (record[i] != 0u) {
            return false;
        }
    }
    if (!yan_note_name_valid(record, name_length)) {
        return false;
    }
    out->start = get_u32le(record + 32u);
    out->end = get_u32le(record + 36u);
    out->first_line = get_u32le(record + 40u);
    out->last_line = get_u32le(record + 44u);
    out->scalar_count = get_u32le(record + 48u);
    if (out->start >= out->end || out->end - out->start > 2048u ||
        out->first_line < 1u || out->last_line < out->first_line ||
        out->scalar_count < 1u ||
        out->scalar_count > YAN_SEMANTIC_CHUNK_MAX_SCALARS) {
        return false;
    }
    for (uint32_t i = 52u; i < 64u; ++i) {
        if (record[i] != 0u) {
            return false;
        }
    }
    memset(out->name, 0, sizeof out->name);
    memcpy(out->name, record, name_length);

    double norm = 0.0;
    for (uint32_t j = 0u; j < YAN_SEMANTIC_DIMENSION; ++j) {
        const float value = get_f32le(record + 64u + 4u * j);
        if (!float_finite(value)) {
            return false;
        }
        norm += (double)value * (double)value;
        if (vector != NULL) {
            vector[j] = value;
        }
    }
    return norm > 0.0;
}

static void encode_index_record(uint8_t *record,
                                const YanSemanticProvenance *provenance,
                                const float *vector)
{
    memset(record, 0, YAN_SEMANTIC_RECORD_BYTES);
    memcpy(record, provenance->name, strlen(provenance->name));
    put_u32le(record + 32u, provenance->start);
    put_u32le(record + 36u, provenance->end);
    put_u32le(record + 40u, provenance->first_line);
    put_u32le(record + 44u, provenance->last_line);
    put_u32le(record + 48u, provenance->scalar_count);
    for (uint32_t j = 0u; j < YAN_SEMANTIC_DIMENSION; ++j) {
        put_f32le(record + 64u + 4u * j, vector[j]);
    }
}

/* Reads and validates the whole index. Structural faults, order faults and
 * vector faults return CORRUPT; open failure returns MISSING/IO; a close_read
 * error overrides a successful read. Caller frees records/vectors. */
static YanSemanticResult read_index(YanSemantic *ctx, YanSemanticIndexMeta *meta,
                                    YanSemanticProvenance **records_out,
                                    float **vectors_out, bool want_vectors,
                                    uint32_t *count_out)
{
    *records_out = NULL;
    *vectors_out = NULL;
    *count_out = 0u;

    uint64_t size = 0u;
    YanSemanticResult result = ctx->store.open_read(ctx->store.context, &size);
    if (result != YAN_SEMANTIC_OK) {
        return result;
    }

    YanSemanticProvenance *records = NULL;
    float *vectors = NULL;
    uint8_t *record = NULL;
    uint32_t count = 0u;
    uint8_t header[256];

    if (size < YAN_SEMANTIC_HEADER_BYTES) {
        result = YAN_SEMANTIC_CORRUPT;
        goto done;
    }
    result = ctx->store.read(ctx->store.context, 0u, header, sizeof header);
    if (result != YAN_SEMANTIC_OK) {
        goto done;
    }
    if (!parse_index_header(header, meta, ctx->source.file_capacity)) {
        result = YAN_SEMANTIC_CORRUPT;
        goto done;
    }
    count = meta->segments;
    /* parse_index_header bounded count to 8192, so this widened product and
     * addition are representable even when size_t is only 32 bits. */
    if (size != (uint64_t)YAN_SEMANTIC_HEADER_BYTES +
                    (uint64_t)count * YAN_SEMANTIC_RECORD_BYTES) {
        result = YAN_SEMANTIC_CORRUPT;
        goto done;
    }

    {
        const size_t allocated = count != 0u ? (size_t)count : 1u;
        records = malloc(allocated * sizeof *records);
        record = malloc(YAN_SEMANTIC_RECORD_BYTES);
        if (want_vectors) {
            vectors = malloc(allocated * (size_t)YAN_SEMANTIC_DIMENSION *
                             sizeof(float));
        }
        if (records == NULL || record == NULL ||
            (want_vectors && vectors == NULL)) {
            result = YAN_SEMANTIC_NOSPACE;
            goto done;
        }
    }

    {
        YanSemanticSha256 payload_hash;
        result = yan_semantic_sha256_init(&payload_hash);
        if (result != YAN_SEMANTIC_OK) {
            goto done;
        }
        uint8_t previous_name[32] = {0};
        uint32_t previous_start = 0u;
        bool have_previous = false;
        for (uint32_t i = 0u; i < count; ++i) {
            const uint64_t offset = (uint64_t)YAN_SEMANTIC_HEADER_BYTES +
                                    (uint64_t)i * YAN_SEMANTIC_RECORD_BYTES;
            result = ctx->store.read(ctx->store.context, offset, record,
                                     YAN_SEMANTIC_RECORD_BYTES);
            if (result != YAN_SEMANTIC_OK) {
                goto done;
            }
            result = yan_semantic_sha256_update(&payload_hash, record,
                                                YAN_SEMANTIC_RECORD_BYTES);
            if (result != YAN_SEMANTIC_OK) {
                goto done;
            }
            float *vector = NULL;
            if (want_vectors) {
                vector = vectors + (size_t)i * YAN_SEMANTIC_DIMENSION;
            }
            if (!parse_index_record(record, &records[i], vector)) {
                result = YAN_SEMANTIC_CORRUPT;
                goto done;
            }
            if (have_previous) {
                const int order = strcmp((const char *)previous_name,
                                         records[i].name);
                if (order > 0 ||
                    (order == 0 && records[i].start <= previous_start)) {
                    result = YAN_SEMANTIC_CORRUPT;
                    goto done;
                }
            }
            memcpy(previous_name, records[i].name, sizeof previous_name);
            previous_start = records[i].start;
            have_previous = true;
        }
        uint8_t payload[32];
        result = yan_semantic_sha256_final(&payload_hash, payload);
        if (result != YAN_SEMANTIC_OK) {
            goto done;
        }
        if (memcmp(payload, meta->payload, sizeof payload) != 0) {
            result = YAN_SEMANTIC_CORRUPT;
            goto done;
        }
    }

done:
    {
        const YanSemanticResult closed =
            ctx->store.close_read(ctx->store.context);
        if (closed != YAN_SEMANTIC_OK) {
            result = closed;
        }
    }
    if (result != YAN_SEMANTIC_OK) {
        free(records);
        free(vectors);
        free(record);
        return result;
    }
    *records_out = records;
    *vectors_out = vectors;
    *count_out = count;
    free(record);
    return YAN_SEMANTIC_OK;
}

static bool same_provenance(const YanSemanticProvenance *a,
                            const YanSemanticProvenance *b)
{
    return strcmp(a->name, b->name) == 0 && a->start == b->start &&
           a->end == b->end && a->first_line == b->first_line &&
           a->last_line == b->last_line && a->scalar_count == b->scalar_count;
}

/* Identity first, then the canonical corpus records and counts. */
static YanSemanticResult match_index(const YanSemantic *ctx,
                                     const YanSemanticCorpus *corpus,
                                     const YanSemanticIndexMeta *meta,
                                     const YanSemanticProvenance *records)
{
    if (memcmp(meta->source, corpus->source_hash, 32u) != 0 ||
        memcmp(meta->model, ctx->identity.model, 32u) != 0 ||
        memcmp(meta->runner, ctx->identity.runner, 32u) != 0 ||
        memcmp(meta->config, ctx->identity.config, 32u) != 0) {
        return YAN_SEMANTIC_STALE;
    }
    if (meta->segments != corpus->segment_count ||
        meta->skipped != corpus->skipped) {
        return YAN_SEMANTIC_CORRUPT;
    }
    for (uint32_t i = 0u; i < meta->segments; ++i) {
        if (!same_provenance(&records[i], &corpus->segments[i])) {
            return YAN_SEMANTIC_CORRUPT;
        }
    }
    return YAN_SEMANTIC_OK;
}

/* ------------------------------------------------------------ ranking */

static bool vector_valid(const float *vector)
{
    double norm = 0.0;
    for (uint32_t i = 0u; i < YAN_SEMANTIC_DIMENSION; ++i) {
        if (!float_finite(vector[i])) {
            return false;
        }
        norm += (double)vector[i] * (double)vector[i];
    }
    return norm > 0.0;
}

static bool cosine_of(const float *a, const float *b, double *out)
{
    double dot = 0.0;
    double norm_a = 0.0;
    double norm_b = 0.0;
    for (uint32_t i = 0u; i < YAN_SEMANTIC_DIMENSION; ++i) {
        const double x = (double)a[i];
        const double y = (double)b[i];
        dot += x * y;
        norm_a += x * x;
        norm_b += y * y;
    }
    if (norm_a <= 0.0 || norm_b <= 0.0) {
        return false;
    }
    const double denominator = sqrt(norm_a) * sqrt(norm_b);
    if (denominator <= 0.0) {
        return false;
    }
    double value = dot / denominator;
    if (!double_finite(value)) {
        return false;
    }
    if (value > 1.0) {
        value = 1.0;
    }
    if (value < -1.0) {
        value = -1.0;
    }
    *out = value;
    return true;
}

typedef struct {
    uint32_t index;
    double score;
    const char *name;
    uint32_t start;
} YanSemanticRanked;

static int compare_ranked(const void *first, const void *second)
{
    const YanSemanticRanked *a = first;
    const YanSemanticRanked *b = second;
    if (a->score > b->score) {
        return -1;
    }
    if (a->score < b->score) {
        return 1;
    }
    const int order = strcmp(a->name, b->name);
    if (order != 0) {
        return order;
    }
    if (a->start < b->start) {
        return -1;
    }
    if (a->start > b->start) {
        return 1;
    }
    return 0;
}

/* --------------------------------------------------------------- rebuild */

static YanSemanticResult rebuild_locked(YanSemantic *ctx,
                                        YanSemanticSummary *out)
{
    if (!binary32_supported()) {
        return YAN_SEMANTIC_UNSUPPORTED;
    }
    YanSemanticResult result = guard_output(ctx, out);
    if (result != YAN_SEMANTIC_OK) {
        return result;
    }

    YanSemanticCorpus *corpus = malloc(sizeof *corpus);
    if (corpus == NULL) {
        return YAN_SEMANTIC_NOSPACE;
    }
    memset(corpus, 0, sizeof *corpus);
    float *vectors = NULL;
    uint8_t *raw = NULL;
    uint8_t *input = NULL;
    uint8_t *record = NULL;
    bool provider_started = false;
    bool provider_stopped = false;
    bool source_closed = false;
    bool store_started = false;
    uint32_t count = 0u;
    uint32_t skipped = 0u;

    result = yan_semantic_corpus_scan(corpus, &ctx->source);
    if (result != YAN_SEMANTIC_OK) {
        goto cleanup;
    }
    count = corpus->segment_count;
    skipped = corpus->skipped;

    vectors = malloc((count != 0u ? (size_t)count : 1u) *
                     (size_t)YAN_SEMANTIC_DIMENSION * sizeof(float));
    raw = malloc(2048u);
    input = malloc(7u + 256u + 2u + 2048u);
    record = malloc(YAN_SEMANTIC_RECORD_BYTES);
    if (vectors == NULL || raw == NULL || input == NULL || record == NULL) {
        result = YAN_SEMANTIC_NOSPACE;
        goto cleanup;
    }

    result = ctx->provider.start(ctx->provider.context);
    provider_started = true;
    if (result != YAN_SEMANTIC_OK) {
        goto cleanup;
    }

    {
        uint32_t file_index = 0u;
        for (uint32_t i = 0u; i < count; ++i) {
            const YanSemanticProvenance *segment = &corpus->segments[i];
            while (file_index < corpus->file_count &&
                   strcmp(corpus->files[file_index].file.name,
                          segment->name) != 0) {
                ++file_index;
            }
            if (file_index >= corpus->file_count) {
                result = YAN_SEMANTIC_CORRUPT;
                goto cleanup;
            }
            const YanSemanticCorpusFile *file = &corpus->files[file_index];
            const uint32_t length = segment->end - segment->start;
            if (length == 0u || length > 2048u) {
                result = YAN_SEMANTIC_CORRUPT;
                goto cleanup;
            }
            result = ctx->source.read(ctx->source.context, segment->name,
                                      segment->start, raw, length);
            if (result != YAN_SEMANTIC_OK) {
                goto cleanup;
            }
            uint32_t input_length = 0u;
            if (file->title_length != 0u) {
                memcpy(input, "Title: ", 7u);
                memcpy(input + 7u, file->title, file->title_length);
                memcpy(input + 7u + file->title_length, "\n\n", 2u);
                input_length = 7u + file->title_length + 2u;
            }
            memcpy(input + input_length, raw, length);
            input_length += length;
            result = ctx->provider.embed(
                ctx->provider.context, input, input_length,
                vectors + (size_t)i * YAN_SEMANTIC_DIMENSION);
            if (result != YAN_SEMANTIC_OK) {
                goto cleanup;
            }
            if (!vector_valid(vectors + (size_t)i * YAN_SEMANTIC_DIMENSION)) {
                result = YAN_SEMANTIC_PROVIDER;
                goto cleanup;
            }
        }
    }

    result = ctx->provider.stop(ctx->provider.context);
    provider_stopped = true;
    if (result != YAN_SEMANTIC_OK) {
        goto cleanup;
    }
    if (ctx->close_source != NULL) {
        result = ctx->close_source(ctx->source.context);
        source_closed = true;
        if (result != YAN_SEMANTIC_OK) {
            goto cleanup;
        }
    } else {
        source_closed = true;
    }

    {
        uint8_t payload[32];
        YanSemanticSha256 payload_hash;
        result = yan_semantic_sha256_init(&payload_hash);
        if (result != YAN_SEMANTIC_OK) {
            goto cleanup;
        }
        for (uint32_t i = 0u; i < count; ++i) {
            encode_index_record(record, &corpus->segments[i],
                                vectors + (size_t)i * YAN_SEMANTIC_DIMENSION);
            result = yan_semantic_sha256_update(&payload_hash, record,
                                                YAN_SEMANTIC_RECORD_BYTES);
            if (result != YAN_SEMANTIC_OK) {
                goto cleanup;
            }
        }
        result = yan_semantic_sha256_final(&payload_hash, payload);
        if (result != YAN_SEMANTIC_OK) {
            goto cleanup;
        }

        uint8_t header[256];
        memset(header, 0, sizeof header);
        memcpy(header, yan_semantic_magic, sizeof yan_semantic_magic);
        put_u32le(header + 8u, UINT32_C(1));
        put_u32le(header + 12u, YAN_SEMANTIC_DIMENSION);
        put_u32le(header + 16u, count);
        put_u32le(header + 20u, skipped);
        put_u32le(header + 24u, YAN_SEMANTIC_HEADER_BYTES);
        put_u32le(header + 28u, YAN_SEMANTIC_RECORD_BYTES);
        memcpy(header + 32u, corpus->source_hash, 32u);
        memcpy(header + 64u, ctx->identity.model, 32u);
        memcpy(header + 96u, ctx->identity.runner, 32u);
        memcpy(header + 128u, ctx->identity.config, 32u);
        memcpy(header + 160u, payload, sizeof payload);

        result = ctx->store.begin_write(ctx->store.context);
        if (result != YAN_SEMANTIC_OK) {
            goto cleanup;
        }
        store_started = true;
        result = ctx->store.write(ctx->store.context, header, sizeof header);
        if (result != YAN_SEMANTIC_OK) {
            goto cleanup;
        }
        for (uint32_t i = 0u; i < count; ++i) {
            encode_index_record(record, &corpus->segments[i],
                                vectors + (size_t)i * YAN_SEMANTIC_DIMENSION);
            result = ctx->store.write(ctx->store.context, record,
                                      YAN_SEMANTIC_RECORD_BYTES);
            if (result != YAN_SEMANTIC_OK) {
                goto cleanup;
            }
        }
        result = ctx->store.finish_write(ctx->store.context);
        if (result != YAN_SEMANTIC_OK) {
            goto cleanup;
        }
        result = ctx->store.publish(ctx->store.context);
        if (result != YAN_SEMANTIC_OK) {
            goto cleanup;
        }
    }

    if (out != NULL) {
        out->state = YAN_SEMANTIC_INDEX_READY;
        out->segments = count;
        out->skipped = skipped;
        out->shown = 0u;
    }
    result = YAN_SEMANTIC_OK;

cleanup:
    if (provider_started && !provider_stopped) {
        const YanSemanticResult stopped =
            ctx->provider.stop(ctx->provider.context);
        if (result == YAN_SEMANTIC_OK) {
            result = stopped;
        }
    }
    if (!source_closed && ctx->close_source != NULL) {
        const YanSemanticResult closed = ctx->close_source(ctx->source.context);
        if (result == YAN_SEMANTIC_OK) {
            result = closed;
        }
    }
    if (store_started && result != YAN_SEMANTIC_OK) {
        ctx->store.discard(ctx->store.context);
    }
    free(vectors);
    free(raw);
    free(input);
    free(record);
    yan_semantic_corpus_release(corpus);
    free(corpus);
    return result;
}

/* ---------------------------------------------------------------- status */

static YanSemanticResult status_locked(YanSemantic *ctx,
                                       YanSemanticSummary *out)
{
    if (!binary32_supported()) {
        return YAN_SEMANTIC_UNSUPPORTED;
    }
    YanSemanticResult result = guard_output(ctx, out);
    if (result != YAN_SEMANTIC_OK) {
        return result;
    }

    YanSemanticCorpus *corpus = malloc(sizeof *corpus);
    if (corpus == NULL) {
        return YAN_SEMANTIC_NOSPACE;
    }
    memset(corpus, 0, sizeof *corpus);
    YanSemanticIndexMeta meta = {0};
    YanSemanticProvenance *records = NULL;
    float *vectors = NULL;
    uint32_t count = 0u;
    YanSemanticIndexState state = YAN_SEMANTIC_INDEX_CORRUPT;
    uint32_t ready_segments = 0u;
    uint32_t ready_skipped = 0u;
    bool source_closed = false;

    result = yan_semantic_corpus_scan(corpus, &ctx->source);
    if (result != YAN_SEMANTIC_OK) {
        goto cleanup;
    }

    result = read_index(ctx, &meta, &records, &vectors, false, &count);
    if (result == YAN_SEMANTIC_MISSING) {
        state = YAN_SEMANTIC_INDEX_MISSING;
        result = YAN_SEMANTIC_OK;
        goto cleanup;
    }
    if (result == YAN_SEMANTIC_CORRUPT) {
        state = YAN_SEMANTIC_INDEX_CORRUPT;
        result = YAN_SEMANTIC_OK;
        goto cleanup;
    }
    if (result != YAN_SEMANTIC_OK) {
        goto cleanup;
    }

    {
        const YanSemanticResult match = match_index(ctx, corpus, &meta, records);
        if (match == YAN_SEMANTIC_STALE) {
            state = YAN_SEMANTIC_INDEX_STALE;
        } else if (match == YAN_SEMANTIC_CORRUPT) {
            state = YAN_SEMANTIC_INDEX_CORRUPT;
        } else {
            state = YAN_SEMANTIC_INDEX_READY;
            ready_segments = count;
            ready_skipped = meta.skipped;
        }
        result = YAN_SEMANTIC_OK;
    }

cleanup:
    if (!source_closed && ctx->close_source != NULL) {
        const YanSemanticResult closed = ctx->close_source(ctx->source.context);
        source_closed = true;
        if (result == YAN_SEMANTIC_OK) {
            result = closed;
        }
    }
    free(records);
    free(vectors);
    yan_semantic_corpus_release(corpus);
    free(corpus);
    if (result != YAN_SEMANTIC_OK) {
        return result;
    }
    if (out != NULL) {
        out->state = state;
        out->segments =
            state == YAN_SEMANTIC_INDEX_READY ? ready_segments : 0u;
        out->skipped =
            state == YAN_SEMANTIC_INDEX_READY ? ready_skipped : 0u;
        out->shown = 0u;
    }
    return YAN_SEMANTIC_OK;
}

/* ---------------------------------------------------------------- search */

static const char yan_semantic_instruction[] =
    "Instruct: Given a question, retrieve relevant passages from notes.\n"
    "Query: ";

static YanSemanticResult guard_query(const YanSemantic *ctx,
                                     const uint8_t *query, size_t length,
                                     YanSemanticRowFn sink,
                                     YanSemanticSummary *out)
{
    if (sink == NULL) {
        return YAN_SEMANTIC_INVALID;
    }
    if (query == NULL || !span_ok(query, length)) {
        return YAN_SEMANTIC_INVALID;
    }
    if (holder_conflicts(ctx, query, length)) {
        return YAN_SEMANTIC_INVALID;
    }
    if (out != NULL) {
        if (!span_ok(out, sizeof *out) ||
            holder_conflicts(ctx, out, sizeof *out) ||
            spans_overlap(query, length, out, sizeof *out)) {
            return YAN_SEMANTIC_INVALID;
        }
    }
    if (!yan_semantic_query_valid(query, length)) {
        return YAN_SEMANTIC_INVALID;
    }
    return YAN_SEMANTIC_OK;
}

static YanSemanticResult search_locked(YanSemantic *ctx, const uint8_t *query,
                                       size_t length, YanSemanticRowFn sink,
                                       void *sink_context,
                                       YanSemanticSummary *out,
                                       YanSemanticEvidence *evidence_out)
{
    if (!binary32_supported()) {
        return YAN_SEMANTIC_UNSUPPORTED;
    }
    YanSemanticResult result = guard_query(ctx, query, length, sink, out);
    if (result != YAN_SEMANTIC_OK) {
        return result;
    }

    const YanSemanticIdentity verified_identity = ctx->identity;

    YanSemanticCorpus *corpus = malloc(sizeof *corpus);
    if (corpus == NULL) {
        return YAN_SEMANTIC_NOSPACE;
    }
    memset(corpus, 0, sizeof *corpus);
    YanSemanticIndexMeta meta = {0};
    YanSemanticProvenance *records = NULL;
    float *vectors = NULL;
    uint32_t count = 0u;
    float *query_vector = NULL;
    YanSemanticRanked *ranked = NULL;
    uint8_t *raw = NULL;
    YanSemanticRow rows[YAN_SEMANTIC_TOP];
    memset(rows, 0, sizeof rows);
    bool provider_started = false;
    bool provider_stopped = false;
    bool source_closed = false;
    uint32_t selected = 0u;
    uint32_t delivered = 0u;

    result = yan_semantic_corpus_scan(corpus, &ctx->source);
    if (result != YAN_SEMANTIC_OK) {
        goto cleanup;
    }

    result = read_index(ctx, &meta, &records, &vectors, true, &count);
    if (result == YAN_SEMANTIC_MISSING || result == YAN_SEMANTIC_CORRUPT ||
        result == YAN_SEMANTIC_STALE) {
        goto cleanup;
    }
    if (result != YAN_SEMANTIC_OK) {
        goto cleanup;
    }

    result = match_index(ctx, corpus, &meta, records);
    if (result != YAN_SEMANTIC_OK) {
        goto cleanup;
    }

    query_vector = malloc((size_t)YAN_SEMANTIC_DIMENSION * sizeof(float));
    if (query_vector == NULL) {
        result = YAN_SEMANTIC_NOSPACE;
        goto cleanup;
    }
    result = ctx->provider.start(ctx->provider.context);
    provider_started = true;
    if (result != YAN_SEMANTIC_OK) {
        goto cleanup;
    }
    {
        const size_t instruction_bytes = sizeof yan_semantic_instruction - 1u;
        uint8_t *embed_input = malloc(instruction_bytes + length);
        if (embed_input == NULL) {
            result = YAN_SEMANTIC_NOSPACE;
            goto cleanup;
        }
        memcpy(embed_input, yan_semantic_instruction, instruction_bytes);
        memcpy(embed_input + instruction_bytes, query, length);
        result = ctx->provider.embed(ctx->provider.context, embed_input,
                                     instruction_bytes + length, query_vector);
        free(embed_input);
        if (result != YAN_SEMANTIC_OK) {
            goto cleanup;
        }
    }
    if (!vector_valid(query_vector)) {
        result = YAN_SEMANTIC_PROVIDER;
        goto cleanup;
    }
    result = ctx->provider.stop(ctx->provider.context);
    provider_stopped = true;
    if (result != YAN_SEMANTIC_OK) {
        goto cleanup;
    }

    selected = count < YAN_SEMANTIC_TOP ? count : YAN_SEMANTIC_TOP;
    if (count != 0u) {
        ranked = malloc((size_t)count * sizeof *ranked);
        if (ranked == NULL) {
            result = YAN_SEMANTIC_NOSPACE;
            goto cleanup;
        }
        for (uint32_t i = 0u; i < count; ++i) {
            double score = 0.0;
            if (!cosine_of(query_vector,
                           vectors + (size_t)i * YAN_SEMANTIC_DIMENSION,
                           &score)) {
                result = YAN_SEMANTIC_CORRUPT;
                goto cleanup;
            }
            ranked[i].index = i;
            ranked[i].score = score;
            ranked[i].name = records[i].name;
            ranked[i].start = records[i].start;
        }
        qsort(ranked, count, sizeof *ranked, compare_ranked);
    }

    if (selected != 0u) {
        raw = malloc((size_t)selected * 2048u);
        if (raw == NULL) {
            result = YAN_SEMANTIC_NOSPACE;
            goto cleanup;
        }
        for (uint32_t i = 0u; i < selected; ++i) {
            const YanSemanticProvenance *provenance =
                &records[ranked[i].index];
            const uint32_t raw_length = provenance->end - provenance->start;
            if (raw_length == 0u || raw_length > 2048u) {
                result = YAN_SEMANTIC_CORRUPT;
                goto cleanup;
            }
            result = ctx->source.read(ctx->source.context, provenance->name,
                                      provenance->start,
                                      raw + (size_t)i * 2048u, raw_length);
            if (result != YAN_SEMANTIC_OK) {
                goto cleanup;
            }
            rows[i].provenance = *provenance;
            rows[i].score = ranked[i].score;
            rows[i].raw = raw + (size_t)i * 2048u;
            rows[i].raw_length = raw_length;
        }
    }

    if (ctx->close_source != NULL) {
        result = ctx->close_source(ctx->source.context);
        source_closed = true;
        if (result != YAN_SEMANTIC_OK) {
            goto cleanup;
        }
    } else {
        source_closed = true;
    }

    for (uint32_t i = 0u; i < selected; ++i) {
        if (!sink(sink_context, &rows[i])) {
            result = YAN_SEMANTIC_STOPPED;
            goto cleanup;
        }
        delivered += 1u;
    }

    if (out != NULL) {
        out->state = YAN_SEMANTIC_INDEX_READY;
        out->segments = meta.segments;
        out->skipped = meta.skipped;
        out->shown = delivered;
    }
    if (evidence_out != NULL) {
        evidence_out->summary = (YanSemanticSummary){
            YAN_SEMANTIC_INDEX_READY, meta.segments, meta.skipped, delivered};
        evidence_out->identity = verified_identity;
        memcpy(evidence_out->source_sha256, corpus->source_hash, 32u);
    }
    result = YAN_SEMANTIC_OK;

cleanup:
    if (provider_started && !provider_stopped) {
        const YanSemanticResult stopped =
            ctx->provider.stop(ctx->provider.context);
        if (result == YAN_SEMANTIC_OK) {
            result = stopped;
        }
    }
    if (!source_closed && ctx->close_source != NULL) {
        const YanSemanticResult closed = ctx->close_source(ctx->source.context);
        if (result == YAN_SEMANTIC_OK) {
            result = closed;
        }
    }
    free(records);
    free(vectors);
    free(query_vector);
    free(ranked);
    free(raw);
    yan_semantic_corpus_release(corpus);
    free(corpus);
    return result;
}

/* ------------------------------------------------------------ public API */

YanSemanticResult yan_semantic_init(YanSemantic *ctx,
                                    const YanSemanticSource *source,
                                    const YanSemanticProvider *provider,
                                    const YanSemanticStore *store,
                                    const YanSemanticIdentity *identity,
                                    YanSemanticResult (*close_source)(void *))
{
    if (ctx == NULL || !span_ok(ctx, sizeof *ctx)) {
        return YAN_SEMANTIC_INVALID;
    }
    if (ctx->busy) {
        return YAN_SEMANTIC_BUSY;
    }
    if (source == NULL || provider == NULL || store == NULL ||
        identity == NULL) {
        return YAN_SEMANTIC_INVALID;
    }
    if (!span_ok(source, sizeof *source) ||
        !span_ok(provider, sizeof *provider) ||
        !span_ok(store, sizeof *store) ||
        !span_ok(identity, sizeof *identity)) {
        return YAN_SEMANTIC_INVALID;
    }
    if (spans_overlap(ctx, sizeof *ctx, source, sizeof *source) ||
        spans_overlap(ctx, sizeof *ctx, provider, sizeof *provider) ||
        spans_overlap(ctx, sizeof *ctx, store, sizeof *store) ||
        spans_overlap(ctx, sizeof *ctx, identity, sizeof *identity) ||
        spans_overlap(source, sizeof *source, provider, sizeof *provider) ||
        spans_overlap(source, sizeof *source, store, sizeof *store) ||
        spans_overlap(source, sizeof *source, identity, sizeof *identity) ||
        spans_overlap(provider, sizeof *provider, store, sizeof *store) ||
        spans_overlap(provider, sizeof *provider, identity, sizeof *identity) ||
        spans_overlap(store, sizeof *store, identity, sizeof *identity)) {
        return YAN_SEMANTIC_INVALID;
    }
    if (source->next == NULL || source->read == NULL ||
        provider->start == NULL || provider->embed == NULL ||
        provider->stop == NULL || store->open_read == NULL ||
        store->read == NULL || store->close_read == NULL ||
        store->begin_write == NULL || store->write == NULL ||
        store->finish_write == NULL || store->publish == NULL ||
        store->discard == NULL) {
        return YAN_SEMANTIC_INVALID;
    }
    if (!span_ok(source->context, source->context_size) ||
        !span_ok(provider->context, provider->context_size) ||
        !span_ok(store->context, store->context_size)) {
        return YAN_SEMANTIC_INVALID;
    }
    if ((source->context_size != 0u &&
         spans_overlap(ctx, sizeof *ctx, source->context,
                       source->context_size)) ||
        (provider->context_size != 0u &&
         spans_overlap(ctx, sizeof *ctx, provider->context,
                       provider->context_size)) ||
        (store->context_size != 0u &&
         spans_overlap(ctx, sizeof *ctx, store->context,
                       store->context_size))) {
        return YAN_SEMANTIC_INVALID;
    }

    ctx->source = *source;
    ctx->provider = *provider;
    ctx->store = *store;
    ctx->identity = *identity;
    ctx->close_source = close_source;
    ctx->initialized = true;
    ctx->busy = false;
    return YAN_SEMANTIC_OK;
}

YanSemanticResult yan_semantic_rebuild(YanSemantic *ctx,
                                       YanSemanticSummary *out)
{
    if (ctx == NULL || !span_ok(ctx, sizeof *ctx) || !ctx->initialized) {
        return YAN_SEMANTIC_INVALID;
    }
    if (ctx->busy) {
        return YAN_SEMANTIC_BUSY;
    }
    ctx->busy = true;
    const YanSemanticResult result = rebuild_locked(ctx, out);
    ctx->busy = false;
    return result;
}

YanSemanticResult yan_semantic_status(YanSemantic *ctx,
                                      YanSemanticSummary *out)
{
    if (ctx == NULL || !span_ok(ctx, sizeof *ctx) || !ctx->initialized) {
        return YAN_SEMANTIC_INVALID;
    }
    if (ctx->busy) {
        return YAN_SEMANTIC_BUSY;
    }
    ctx->busy = true;
    const YanSemanticResult result = status_locked(ctx, out);
    ctx->busy = false;
    return result;
}

YanSemanticResult yan_semantic_search(YanSemantic *ctx, const uint8_t *query,
                                      size_t length, YanSemanticRowFn sink,
                                      void *sink_context,
                                      YanSemanticSummary *out)
{
    if (ctx == NULL || !span_ok(ctx, sizeof *ctx) || !ctx->initialized) {
        return YAN_SEMANTIC_INVALID;
    }
    if (ctx->busy) {
        return YAN_SEMANTIC_BUSY;
    }
    ctx->busy = true;
    const YanSemanticResult result =
        search_locked(ctx, query, length, sink, sink_context, out, NULL);
    ctx->busy = false;
    return result;
}

YanSemanticResult yan_semantic_search_evidence(YanSemantic *ctx,
    const uint8_t *query, size_t length, YanSemanticRowFn sink,
    void *sink_context, YanSemanticEvidence *evidence_out)
{
    if (ctx == NULL || !span_ok(ctx, sizeof *ctx) || !ctx->initialized) {
        return YAN_SEMANTIC_INVALID;
    }
    if (ctx->busy) {
        return YAN_SEMANTIC_BUSY;
    }
    /* Guard the complete caller object before taking a member address or
     * reading the query. In particular, identity/hash suffixes are protected. */
    if (evidence_out == NULL || !span_ok(evidence_out, sizeof *evidence_out) ||
        holder_conflicts(ctx, evidence_out, sizeof *evidence_out) ||
        !span_ok(query, length) ||
        spans_overlap(query, length, evidence_out, sizeof *evidence_out)) {
        return YAN_SEMANTIC_INVALID;
    }
    YanSemanticEvidence pending = {0};
    ctx->busy = true;
    const YanSemanticResult result = search_locked(ctx, query, length, sink,
        sink_context, NULL, &pending);
    if (result == YAN_SEMANTIC_OK) {
        *evidence_out = pending;
    }
    ctx->busy = false;
    return result;
}
