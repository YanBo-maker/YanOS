/* 0045 R1: the shared image graph query used by the 0044 command and, later,
 * by the MCP graph worker.
 *
 * Contract: docs/specs/0045-graph-mcp-tools.md R1 and
 * docs/decisions/0009-graph-agent-access.md. The JSON schema, field order, page
 * caps, canonical row order and error tokens are 0044 R2's; this file is the
 * extraction of what tools/yan_graph.c used to own, with three additions the
 * service seam needs: an optional expected source identity, an optional
 * encoded-size cap and an optional cancellation flag.
 *
 * The portable core (knowledge_graph.c) is untouched and stays free of POSIX,
 * YanFS and JSON; this module is the only place that combines it with the
 * read-only image adapter. The image is opened twice -- once for the build and
 * once through a fresh adapter for the post observation -- because the whole
 * point of the A/B comparison is that the second read cannot come from a cache.
 * Nothing here allocates a model, spawns a process or writes to the image.
 *
 * Order of work, which is what makes the failure contract true:
 *   validate arguments (zero I/O) -> observe cancellation -> open+mount ->
 *   digest A -> expected_source comparison (STALE) -> build -> select the
 *   queried concept -> close the build source (a close error outranks OK) ->
 *   fresh adapter, digest B -> A/B comparison (STALE) -> encode ->
 *   publish. Every failure leaves *payload NULL and *length 0, so a caller can
 *   never transmit half an object. */

#define _POSIX_C_SOURCE 200809L

#include "knowledge_graph_query.h"
#include "semantic_posix.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The three page caps the 0044 JSON reports as total/shown. */
#define GRAPH_CONCEPT_SHOWN UINT32_C(20)
#define GRAPH_DECLARATION_SHOWN UINT32_C(20)
#define GRAPH_RELATION_SHOWN UINT32_C(20)

/* ----------------------------------------------------------------- JSON output */

/* One small growable byte buffer. The whole object is built here, so a caller
 * writes it in a single call and no partially encoded object can be published.
 *
 * `limited` and `failed` are deliberately different: a size cap is the
 * caller's chosen bound (LIMIT), while a refused allocation is the host running
 * out of memory (NOSPACE). Conflating them would let a bounded request look
 * like a broken machine, or a broken machine look like a bounded request. */
typedef struct {
    char *bytes;
    size_t length, capacity;
    size_t limit; /* 0: unlimited */
    bool failed;  /* an allocation or an arithmetic bound failed */
    bool limited; /* the object would exceed `limit` */
} Buffer;

static void buffer_reserve(Buffer *buffer, size_t extra)
{
    if (buffer->failed || buffer->limited) {
        return;
    }
    if (extra <= buffer->capacity - buffer->length) {
        return;
    }
    if (extra > SIZE_MAX - buffer->length) {
        buffer->failed = true;
        return;
    }
    const size_t needed = buffer->length + extra;
    if (buffer->limit != 0u && needed > buffer->limit) {
        buffer->limited = true;
        return;
    }
    size_t wanted = buffer->capacity != 0u ? buffer->capacity : 512u;
    while (wanted - buffer->length < extra) {
        if (wanted > SIZE_MAX / 2u) {
            buffer->failed = true;
            return;
        }
        wanted *= 2u;
    }
    /* Never grow past the cap: the capacity itself is what keeps a bounded
     * caller from allocating the whole object before being refused. */
    if (buffer->limit != 0u && wanted > buffer->limit) {
        wanted = buffer->limit;
    }
    char *grown = realloc(buffer->bytes, wanted);
    if (grown == NULL) {
        buffer->failed = true;
        return;
    }
    buffer->bytes = grown;
    buffer->capacity = wanted;
}

static void buffer_add(Buffer *buffer, const char *bytes, size_t length)
{
    buffer_reserve(buffer, length);
    if (buffer->failed || buffer->limited) {
        return;
    }
    memcpy(buffer->bytes + buffer->length, bytes, length);
    buffer->length += length;
}

static void buffer_uint(Buffer *buffer, uint32_t value)
{
    char digits[16];
    int length = snprintf(digits, sizeof digits, "%" PRIu32, value);
    if (length <= 0) {
        buffer->failed = true;
        return;
    }
    buffer_add(buffer, digits, (size_t)length);
}

/* Complete JSON string escaping. Quotes, backslashes and every C0 control get
 * the escape form; DEL is escaped as well because it is not printable text.
 * Everything else, including all non-ASCII UTF-8 lead and continuation bytes,
 * is copied unchanged, so a Chinese label stays readable and valid. */
static void buffer_string(Buffer *buffer, const char *text, uint32_t length)
{
    static const char digits[] = "0123456789abcdef";
    buffer_add(buffer, "\"", 1u);
    for (uint32_t index = 0u; index < length; ++index) {
        uint8_t byte = (uint8_t)text[index];
        switch (byte) {
        case '"': buffer_add(buffer, "\\\"", 2u); break;
        case '\\': buffer_add(buffer, "\\\\", 2u); break;
        case '\n': buffer_add(buffer, "\\n", 2u); break;
        case '\r': buffer_add(buffer, "\\r", 2u); break;
        case '\t': buffer_add(buffer, "\\t", 2u); break;
        case '\b': buffer_add(buffer, "\\b", 2u); break;
        case '\f': buffer_add(buffer, "\\f", 2u); break;
        default:
            if (byte < 0x20u || byte == 0x7Fu) {
                const char escape[6] = {'\\', 'u', '0', '0',
                                        digits[byte >> 4], digits[byte & 15u]};
                buffer_add(buffer, escape, sizeof escape);
            } else {
                buffer_add(buffer, (const char *)&byte, 1u);
            }
            break;
        }
    }
    buffer_add(buffer, "\"", 1u);
}

static void buffer_zed_string(Buffer *buffer, const char *text)
{
    buffer_string(buffer, text, (uint32_t)strlen(text));
}

static void buffer_key(Buffer *buffer, const char *key)
{
    buffer_string(buffer, key, (uint32_t)strlen(key));
    buffer_add(buffer, ":", 1u);
}

static void buffer_hex(Buffer *buffer, const uint8_t digest[32])
{
    static const char digits[] = "0123456789abcdef";
    char text[64];
    for (unsigned index = 0u; index < 32u; ++index) {
        text[2u * index] = digits[digest[index] >> 4];
        text[2u * index + 1u] = digits[digest[index] & 15u];
    }
    buffer_add(buffer, text, sizeof text);
}

/* One provenance object. Both the declaration slice and a proposition's `where`
 * use exactly these ten fields, in this order. */
static void buffer_provenance(Buffer *buffer, const YanKgProvenance *where)
{
    buffer_add(buffer, "{", 1u);
    buffer_key(buffer, "file");
    buffer_zed_string(buffer, where->file);
    buffer_add(buffer, ",", 1u);
    buffer_key(buffer, "block_id");
    buffer_zed_string(buffer, where->block_id);
    buffer_add(buffer, ",", 1u);
    buffer_key(buffer, "declaration_line");
    buffer_uint(buffer, where->declaration_line);
    buffer_add(buffer, ",", 1u);
    buffer_key(buffer, "declaration_start");
    buffer_uint(buffer, where->declaration_start);
    buffer_add(buffer, ",", 1u);
    buffer_key(buffer, "declaration_end");
    buffer_uint(buffer, where->declaration_end);
    buffer_add(buffer, ",", 1u);
    buffer_key(buffer, "block_marker_line");
    buffer_uint(buffer, where->block_marker_line);
    buffer_add(buffer, ",", 1u);
    buffer_key(buffer, "block_first_line");
    buffer_uint(buffer, where->block_first_line);
    buffer_add(buffer, ",", 1u);
    buffer_key(buffer, "block_last_line");
    buffer_uint(buffer, where->block_last_line);
    buffer_add(buffer, ",", 1u);
    buffer_key(buffer, "block_start");
    buffer_uint(buffer, where->block_start);
    buffer_add(buffer, ",", 1u);
    buffer_key(buffer, "block_end");
    buffer_uint(buffer, where->block_end);
    buffer_add(buffer, "}", 1u);
}

/* The common root fields, in the order the locked schema lists them. */
static void buffer_root(Buffer *buffer, const char *operation,
                        const uint8_t digest[32], uint32_t skipped)
{
    buffer_add(buffer, "{", 1u);
    buffer_key(buffer, "version");
    buffer_uint(buffer, 1u);
    buffer_add(buffer, ",", 1u);
    buffer_key(buffer, "operation");
    buffer_zed_string(buffer, operation);
    buffer_add(buffer, ",", 1u);
    buffer_key(buffer, "source_sha256");
    buffer_add(buffer, "\"", 1u);
    buffer_hex(buffer, digest);
    buffer_add(buffer, "\"", 1u);
    buffer_add(buffer, ",", 1u);
    buffer_key(buffer, "skipped");
    buffer_uint(buffer, skipped);
}

/* --------------------------------------------------------------- result rows */

/* One selected proposition plus which adjacency kind yielded it. The three
 * kinds are collected separately because a self loop must be listed once. */
typedef struct {
    const YanKgRelation *relation;
    const char *direction;
} GraphRow;

static int text_compare(const char *left, const char *right)
{
    return strcmp(left, right);
}

/* The canonical proposition order: file name ASCII, then the declaration byte,
 * then the remaining fields so two propositions at the same position still have
 * one deterministic order. */
static bool rows_before(const YanKgRelation *left, const YanKgRelation *right)
{
    int order = text_compare(left->where.file, right->where.file);
    if (order != 0) {
        return order < 0;
    }
    if (left->where.declaration_start != right->where.declaration_start) {
        return left->where.declaration_start < right->where.declaration_start;
    }
    if (left->where.declaration_end != right->where.declaration_end) {
        return left->where.declaration_end < right->where.declaration_end;
    }
    order = text_compare(left->type, right->type);
    if (order != 0) {
        return order < 0;
    }
    order = text_compare(left->subject_id, right->subject_id);
    if (order != 0) {
        return order < 0;
    }
    return text_compare(left->object_id, right->object_id) < 0;
}

/* Merges the three adjacency lists into canonical order. The lists are already
 * sorted, so this is a three-way merge; nothing is re-sorted and no graph is
 * scanned beyond the rows actually selected. */
static uint32_t rows_collect(const YanKgConcept *concept, GraphRow *rows,
                             uint32_t capacity)
{
    uint32_t out_index = 0u, in_index = 0u, self_index = 0u, count = 0u;
    while (count < capacity) {
        const YanKgRelation *best = NULL;
        const char *direction = NULL;
        if (out_index < concept->out_count) {
            best = concept->out[out_index];
            direction = "out";
        }
        if (in_index < concept->in_count) {
            const YanKgRelation *candidate = concept->in[in_index];
            if (best == NULL || rows_before(candidate, best)) {
                best = candidate;
                direction = "in";
            }
        }
        if (self_index < concept->self_count) {
            const YanKgRelation *candidate = concept->self[self_index];
            if (best == NULL || rows_before(candidate, best)) {
                best = candidate;
                direction = "self";
            }
        }
        if (best == NULL) {
            break;
        }
        rows[count].relation = best;
        rows[count].direction = direction;
        ++count;
        if (direction[0] == 'o') {
            ++out_index;
        } else if (direction[0] == 'i') {
            ++in_index;
        } else {
            ++self_index;
        }
    }
    return count;
}

static void buffer_endpoint(Buffer *buffer, const char *id, const char *name)
{
    buffer_add(buffer, "{", 1u);
    buffer_key(buffer, "id");
    buffer_zed_string(buffer, id);
    buffer_add(buffer, ",", 1u);
    buffer_key(buffer, "name");
    buffer_zed_string(buffer, name);
    buffer_add(buffer, "}", 1u);
}

static void buffer_relation(Buffer *buffer, const GraphRow *row,
                            const YanKg *graph)
{
    const YanKgRelation *relation = row->relation;
    const YanKgConcept *subject = yan_kg_concept_at(graph, relation->subject);
    const YanKgConcept *object = yan_kg_concept_at(graph, relation->object);
    buffer_add(buffer, "{", 1u);
    buffer_key(buffer, "type");
    buffer_zed_string(buffer, relation->type);
    buffer_add(buffer, ",", 1u);
    buffer_key(buffer, "subject");
    buffer_endpoint(buffer, relation->subject_id,
                    subject != NULL ? subject->name : "");
    buffer_add(buffer, ",", 1u);
    buffer_key(buffer, "object");
    buffer_endpoint(buffer, relation->object_id,
                    object != NULL ? object->name : "");
    buffer_add(buffer, ",", 1u);
    buffer_key(buffer, "direction");
    buffer_zed_string(buffer, row->direction);
    buffer_add(buffer, ",", 1u);
    buffer_key(buffer, "where");
    buffer_provenance(buffer, &relation->where);
    buffer_add(buffer, "}", 1u);
}

static void buffer_list(Buffer *buffer, const YanKg *graph,
                        const uint8_t digest[32], const YanKgSummary *summary)
{
    const uint32_t total = yan_kg_concept_count(graph);
    const uint32_t shown =
        total < GRAPH_CONCEPT_SHOWN ? total : GRAPH_CONCEPT_SHOWN;
    buffer_root(buffer, "list", digest, summary->skipped);
    buffer_add(buffer, ",", 1u);
    buffer_key(buffer, "total");
    buffer_uint(buffer, total);
    buffer_add(buffer, ",", 1u);
    buffer_key(buffer, "shown");
    buffer_uint(buffer, shown);
    buffer_add(buffer, ",", 1u);
    buffer_key(buffer, "concepts");
    buffer_add(buffer, "[", 1u);
    for (uint32_t index = 0u; index < shown; ++index) {
        const YanKgConcept *concept = yan_kg_concept_at(graph, index);
        if (index != 0u) {
            buffer_add(buffer, ",", 1u);
        }
        buffer_add(buffer, "{", 1u);
        buffer_key(buffer, "id");
        buffer_zed_string(buffer, concept->id);
        buffer_add(buffer, ",", 1u);
        buffer_key(buffer, "name");
        buffer_zed_string(buffer, concept->name);
        buffer_add(buffer, ",", 1u);
        buffer_key(buffer, "alias_count");
        buffer_uint(buffer, concept->alias_count);
        buffer_add(buffer, ",", 1u);
        buffer_key(buffer, "declaration_count");
        buffer_uint(buffer, concept->declaration_count);
        buffer_add(buffer, ",", 1u);
        buffer_key(buffer, "relation_count");
        buffer_uint(buffer, concept->out_count + concept->in_count +
                                concept->self_count);
        buffer_add(buffer, "}", 1u);
    }
    buffer_add(buffer, "]}", 2u);
}

static void buffer_query(Buffer *buffer, const YanKg *graph,
                         const YanKgQueryOptions *options,
                         const uint8_t digest[32], const YanKgSummary *summary,
                         const YanKgConcept *concept)
{
    buffer_root(buffer, "query", digest, summary->skipped);
    buffer_add(buffer, ",", 1u);
    buffer_key(buffer, "query");
    buffer_string(buffer, options->query, options->query_length);
    buffer_add(buffer, ",", 1u);
    buffer_key(buffer, "concept");
    buffer_add(buffer, "{", 1u);
    buffer_key(buffer, "id");
    buffer_zed_string(buffer, concept->id);
    buffer_add(buffer, ",", 1u);
    buffer_key(buffer, "name");
    buffer_zed_string(buffer, concept->name);
    buffer_add(buffer, ",", 1u);
    /* The complete alias array, never truncated. */
    buffer_key(buffer, "aliases");
    buffer_add(buffer, "[", 1u);
    for (uint32_t index = 0u; index < concept->alias_count; ++index) {
        if (index != 0u) {
            buffer_add(buffer, ",", 1u);
        }
        buffer_zed_string(buffer, concept->aliases[index]);
    }
    buffer_add(buffer, "]}", 2u);

    const uint32_t declaration_total = concept->declaration_count;
    const uint32_t declaration_shown =
        declaration_total < GRAPH_DECLARATION_SHOWN ? declaration_total
                                                    : GRAPH_DECLARATION_SHOWN;
    buffer_add(buffer, ",", 1u);
    buffer_key(buffer, "declarations");
    buffer_add(buffer, "{", 1u);
    buffer_key(buffer, "total");
    buffer_uint(buffer, declaration_total);
    buffer_add(buffer, ",", 1u);
    buffer_key(buffer, "shown");
    buffer_uint(buffer, declaration_shown);
    buffer_add(buffer, ",", 1u);
    buffer_key(buffer, "items");
    buffer_add(buffer, "[", 1u);
    for (uint32_t index = 0u; index < declaration_shown; ++index) {
        if (index != 0u) {
            buffer_add(buffer, ",", 1u);
        }
        buffer_provenance(buffer, &concept->declarations[index]);
    }
    buffer_add(buffer, "]}", 2u);

    const uint32_t relation_total =
        concept->out_count + concept->in_count + concept->self_count;
    /* The three-way merge itself stops at the shown cap. */
    GraphRow rows[GRAPH_RELATION_SHOWN];
    uint32_t collected = rows_collect(concept, rows, GRAPH_RELATION_SHOWN);
    buffer_add(buffer, ",", 1u);
    buffer_key(buffer, "relations");
    buffer_add(buffer, "{", 1u);
    buffer_key(buffer, "total");
    buffer_uint(buffer, relation_total);
    buffer_add(buffer, ",", 1u);
    buffer_key(buffer, "shown");
    buffer_uint(buffer, collected);
    buffer_add(buffer, ",", 1u);
    buffer_key(buffer, "items");
    buffer_add(buffer, "[", 1u);
    for (uint32_t index = 0u; index < collected; ++index) {
        if (index != 0u) {
            buffer_add(buffer, ",", 1u);
        }
        buffer_relation(buffer, &rows[index], graph);
    }
    buffer_add(buffer, "]}", 2u);
    buffer_add(buffer, "}", 1u);
}

/* --------------------------------------------------------------- source seams */

/* The image adapter reports its own vocabulary; map it explicitly to the C
 * enumeration instead of casting numbers. */
static YanKgResult map_open(YanImageOpenResult result)
{
    switch (result) {
    case YAN_IMAGE_OPEN_OK: return YAN_KG_OK;
    case YAN_IMAGE_OPEN_INVALID: return YAN_KG_INVALID;
    case YAN_IMAGE_OPEN_IO: return YAN_KG_IO;
    case YAN_IMAGE_OPEN_CORRUPT: return YAN_KG_CORRUPT;
    case YAN_IMAGE_OPEN_UNSUPPORTED: return YAN_KG_UNSUPPORTED;
    case YAN_IMAGE_OPEN_NOT_MOUNTED: return YAN_KG_PROTOCOL;
    case YAN_IMAGE_OPEN_FAULTED: return YAN_KG_PROTOCOL;
    case YAN_IMAGE_OPEN_BUSY: return YAN_KG_BUSY;
    case YAN_IMAGE_OPEN_PROTOCOL: return YAN_KG_PROTOCOL;
    }
    return YAN_KG_PROTOCOL;
}

/* Cancellation-aware borrow of one real source.
 *
 * The core and the digest both call next/read in the middle of a build, so this
 * is the only place a caller's flag can be observed without adding a timeout or
 * a thread: the wrapped callback checks the flag and stops the pass at the next
 * callback boundary. The wrapper borrows the real source; it never copies its
 * context or takes ownership, and it keeps `file_capacity` so the logical-view
 * checks still see the real runtime capacity. */
typedef struct {
    const YanSemanticSource *inner;      /* borrowed, rebound on each observe */
    const volatile sig_atomic_t *cancel; /* borrowed, optional */
} GuardedSource;

static YanSemanticResult guard_next(void *opaque, uint32_t *cursor,
                                    YanSemanticFile *out)
{
    const GuardedSource *guarded = opaque;
    if (guarded->cancel != NULL && *guarded->cancel != 0) {
        return YAN_SEMANTIC_CANCELLED;
    }
    return guarded->inner->next(guarded->inner->context, cursor, out);
}

static YanSemanticResult guard_read(void *opaque, const char *name,
                                    uint32_t offset, uint8_t *out,
                                    uint32_t length)
{
    const GuardedSource *guarded = opaque;
    if (guarded->cancel != NULL && *guarded->cancel != 0) {
        return YAN_SEMANTIC_CANCELLED;
    }
    return guarded->inner->read(guarded->inner->context, name, offset, out,
                                length);
}

/* Binds the adapter's read source and wraps it. Both live in the caller's frame
 * for one observation, which is why the pair is one call: a wrapper that
 * outlived its inner source would read a dangling context. */
static void guard_bind(YanSemanticPosix *adapter, YanSemanticSource *inner,
                       GuardedSource *guarded, YanSemanticSource *out,
                       const volatile sig_atomic_t *cancel)
{
    yan_semantic_posix_bind_source(adapter, inner);
    guarded->inner = inner;
    guarded->cancel = cancel;
    out->context = guarded;
    out->context_size = sizeof *guarded;
    out->next = guard_next;
    out->read = guard_read;
    out->file_capacity = inner->file_capacity;
}

/* Observation: drop the directory cache and mount again, so the directory block
 * is really re-read, then digest the logical source. This is the same
 * change-detection shape the 0033 block query uses. `source` and `guarded` are
 * scratch owned by the caller. */
static YanKgResult observe(YanSemanticPosix *adapter, YanSemanticSource *source,
                           GuardedSource *guarded, YanSemanticSource *wrapped,
                           const volatile sig_atomic_t *cancel,
                           uint8_t digest[32])
{
    YanFsResult state = yan_fs_unmount(&adapter->fs);
    if (state != YAN_FS_OK) {
        return YAN_KG_IO;
    }
    state = yan_fs_mount(&adapter->fs);
    if (state != YAN_FS_OK) {
        return YAN_KG_IO;
    }
    guard_bind(adapter, source, guarded, wrapped, cancel);
    YanSemanticResult result = yan_semantic_source_digest(wrapped, digest);
    switch (result) {
    case YAN_SEMANTIC_OK: return YAN_KG_OK;
    /* A digest that cannot allocate is an I/O failure of the caller's host, not
     * a graph content limit. */
    case YAN_SEMANTIC_IO: case YAN_SEMANTIC_NOSPACE: return YAN_KG_IO;
    /* A cancelled pass is reported as a stopped pass: the caller knows it
     * cancelled, and no core enumerator is added for it. */
    case YAN_SEMANTIC_CANCELLED: return YAN_KG_IO;
    case YAN_SEMANTIC_INVALID: return YAN_KG_INVALID;
    case YAN_SEMANTIC_CORRUPT: return YAN_KG_CORRUPT;
    case YAN_SEMANTIC_UNSUPPORTED: return YAN_KG_UNSUPPORTED;
    case YAN_SEMANTIC_MISSING: return YAN_KG_PROTOCOL;
    case YAN_SEMANTIC_NOT_FOUND: return YAN_KG_PROTOCOL;
    default: return YAN_KG_IO;
    }
}

/* One open observation, or the close error that outranks an otherwise OK
 * result. The adapter is always released here, including on failure, because
 * the image descriptor can outlive a later mount error. */
static YanKgResult fresh_digest(const char *image, uint8_t digest[32],
                                const volatile sig_atomic_t *cancel)
{
    YanSemanticPosix *adapter = calloc(1u, sizeof *adapter);
    if (adapter == NULL) {
        return YAN_KG_NOSPACE;
    }
    adapter->image_fd = -1;
    adapter->index_fd = -1;
    YanSemanticSource source;
    GuardedSource guarded;
    YanSemanticSource wrapped;
    memset(&source, 0, sizeof source);
    memset(&guarded, 0, sizeof guarded);
    memset(&wrapped, 0, sizeof wrapped);

    YanKgResult result = map_open(
        yan_semantic_posix_open_image_ex(adapter, image));
    if (result == YAN_KG_OK) {
        result = observe(adapter, &source, &guarded, &wrapped, cancel, digest);
    }
    YanSemanticResult closed = yan_semantic_posix_cleanup(adapter);
    free(adapter);
    if (closed != YAN_SEMANTIC_OK) {
        /* A close error outranks an otherwise OK observation, and an earlier
         * real error is kept rather than masked. */
        if (result == YAN_KG_OK) {
            result = closed == YAN_SEMANTIC_INVALID ? YAN_KG_INVALID
                                                    : YAN_KG_IO;
        }
    }
    return result;
}

/* ------------------------------------------------------------------ the entry */

/* Argument validation, with no I/O at all, so a rejected call cannot touch the
 * image. The query text rules are the shared 0029 single-line qualification
 * plus the 1..YAN_KG_QUERY_MAX byte bound the core also applies. */
static bool options_valid(const YanKgQueryOptions *options)
{
    if (options == NULL || options->image == NULL ||
        options->image[0] == '\0') {
        return false;
    }
    switch (options->operation) {
    case YAN_KG_QUERY_LIST:
        return true;
    case YAN_KG_QUERY_LOOKUP:
        if (options->query == NULL || options->query_length == 0u ||
            options->query_length > YAN_KG_QUERY_MAX) {
            return false;
        }
        return yan_semantic_query_valid((const uint8_t *)options->query,
                                        options->query_length);
    }
    return false;
}

YanKgResult yan_kg_query_json(const YanKgQueryOptions *options, char **payload,
                             size_t *length)
{
    if (payload == NULL || length == NULL || payload == (char **)length) {
        return YAN_KG_INVALID;
    }
    *payload = NULL;
    *length = 0u;
    if (!options_valid(options)) {
        return YAN_KG_INVALID;
    }
    /* The flag is honoured before any descriptor is opened, so a request that
     * was already cancelled costs the image nothing. */
    if (options->cancel != NULL && *options->cancel != 0) {
        return YAN_KG_IO;
    }

    YanSemanticPosix *adapter = calloc(1u, sizeof *adapter);
    if (adapter == NULL) {
        return YAN_KG_NOSPACE;
    }
    adapter->image_fd = -1;
    adapter->index_fd = -1;

    YanKg graph;
    memset(&graph, 0, sizeof graph);
    YanKgSummary summary;
    memset(&summary, 0, sizeof summary);
    YanSemanticSource source;
    GuardedSource guarded;
    YanSemanticSource wrapped;
    memset(&source, 0, sizeof source);
    memset(&guarded, 0, sizeof guarded);
    memset(&wrapped, 0, sizeof wrapped);
    uint8_t before[32], after[32];

    /* The graph stays published until the bytes are encoded, so it is destroyed
     * only on the way out of every path below. */
    YanKgResult result = map_open(
        yan_semantic_posix_open_image_ex(adapter, options->image));
    if (result == YAN_KG_OK) {
        result = observe(adapter, &source, &guarded, &wrapped, options->cancel,
                         before);
    }
    /* The expected identity is decided before the build: a stale request must
     * not pay for a whole graph. */
    if (result == YAN_KG_OK && options->expected_source != NULL &&
        memcmp(before, options->expected_source, sizeof before) != 0) {
        result = YAN_KG_STALE;
    }
    if (result == YAN_KG_OK) {
        /* The wrapper bound during observation still borrows this adapter and
         * keeps the real file_capacity, so the build reads through the same
         * cancellation-aware seam without a second bind. */
        result = yan_kg_build(&graph, &wrapped, &summary);
    }
    const YanKgConcept *concept = NULL;
    if (result == YAN_KG_OK && options->operation == YAN_KG_QUERY_LOOKUP) {
        YanKgMatch match;
        memset(&match, 0, sizeof match);
        result = yan_kg_lookup(&graph, (const uint8_t *)options->query,
                               options->query_length, &match);
        if (result == YAN_KG_OK) {
            concept = yan_kg_concept_at(&graph, match.concept);
            if (concept == NULL) {
                result = YAN_KG_PROTOCOL;
            }
        }
    }

    /* Close the build source and require it to succeed: a close error outranks
     * an otherwise OK result. */
    YanSemanticResult closed = yan_semantic_posix_cleanup(adapter);
    free(adapter);
    if (result == YAN_KG_OK && closed != YAN_SEMANTIC_OK) {
        result = closed == YAN_SEMANTIC_INVALID ? YAN_KG_INVALID : YAN_KG_IO;
    }
    if (result != YAN_KG_OK) {
        yan_kg_destroy(&graph);
        return result;
    }

    /* Re-observe the image through a fresh image-only adapter, so the directory
     * block is really re-read and the comparison is not satisfied by a cache. */
    YanKgResult reobserved = fresh_digest(options->image, after,
                                          options->cancel);
    if (reobserved != YAN_KG_OK) {
        yan_kg_destroy(&graph);
        return reobserved;
    }
    if (memcmp(before, after, sizeof before) != 0) {
        yan_kg_destroy(&graph);
        return YAN_KG_STALE;
    }

    /* Encode with the digest that was actually verified on both sides. */
    Buffer output;
    memset(&output, 0, sizeof output);
    output.limit = options->output_limit;
    if (options->operation == YAN_KG_QUERY_LIST) {
        buffer_list(&output, &graph, after, &summary);
    } else {
        buffer_query(&output, &graph, options, after, &summary, concept);
    }
    yan_kg_destroy(&graph);
    if (output.failed) {
        free(output.bytes);
        return YAN_KG_NOSPACE;
    }
    if (output.limited) {
        free(output.bytes);
        return YAN_KG_LIMIT;
    }
    /* One terminator byte so the buffer is also a C string for the worker's
     * JSON transport. It is outside `output_limit`, which bounds the object
     * itself, so a payload exactly at the cap is still accepted. */
    if (output.length == SIZE_MAX) {
        free(output.bytes);
        return YAN_KG_NOSPACE;
    }
    char *terminated = realloc(output.bytes, output.length + 1u);
    if (terminated == NULL) {
        free(output.bytes);
        return YAN_KG_NOSPACE;
    }
    terminated[output.length] = '\0';
    *payload = terminated;
    *length = output.length;
    return YAN_KG_OK;
}
