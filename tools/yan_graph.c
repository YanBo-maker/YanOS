/* yan_graph: the 0044 R2 read-only knowledge-graph command.
 *
 * Contract: docs/specs/0044-evidence-knowledge-graph.md R2 and its
 * "R2接口字段锁定", plus docs/decisions/0008-evidence-knowledge-graph.md.
 *
 *   yan_graph --disk-image IMAGE list
 *   yan_graph --disk-image IMAGE query TEXT
 *
 * One open of a stable offline image, one graph build, one JSON object on
 * stdout. The command borrows the existing read-only YanFS adapter and the 0033
 * source digest; it writes nothing to the image or its attachments, keeps no
 * cache across invocations, and needs no model, runner, semantic index or third
 * party JSON library.
 *
 * Order of work, which is what makes the failure contract true:
 *   parse arguments (no I/O at all) -> open+mount -> observe digest A -> build ->
 *   select results -> encode the whole JSON into memory -> close the source
 *   (a close error outranks OK) -> observe digest B and compare (STALE) -> write
 *   the encoded object with one fwrite and one fflush.
 * Therefore every ordinary error leaves stdout empty, and only a real stdout
 * failure can leave a prefix behind. The caller must check both the exit status
 * and that the whole object arrived.
 *
 * The graph query itself never scans: concepts come from the immutable ID-ordered
 * array, and propositions come from the concept's own out/in/self adjacency
 * slices, merged by the canonical (file name, declaration byte) order. */

#define _POSIX_C_SOURCE 200809L

#include "knowledge_graph.h"
#include "semantic_posix.h"

#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The two page/row caps the JSON reports as total/shown. */
#define GRAPH_CONCEPT_SHOWN UINT32_C(20)
#define GRAPH_DECLARATION_SHOWN UINT32_C(20)
#define GRAPH_RELATION_SHOWN UINT32_C(20)

typedef enum {
    OPERATION_LIST = 0,
    OPERATION_QUERY
} Operation;

typedef struct {
    Operation operation;
    const char *image;
    const char *query;
    uint32_t query_length;
} Options;

/* ------------------------------------------------------------ argument parsing */

/* The failure contract is one diagnostic line, so the reminder of the accepted
 * forms is appended after the ERROR token rather than replacing it. */
static void usage(void)
{
    fputs("ERROR USAGE\n", stderr);
    fputs("usage: yan_graph --disk-image IMAGE list\n", stderr);
    fputs("       yan_graph --disk-image IMAGE query TEXT\n", stderr);
    fputs("  list        concepts in ID order, first 20 of total\n", stderr);
    fputs("  query TEXT  exact concept ID, else a unique name or alias\n", stderr);
    fputs("              (1..256 bytes, single line, valid UTF-8)\n", stderr);
}

/* Exactly the two advertised forms are accepted:
 *
 *   yan_graph --disk-image IMAGE list
 *   yan_graph --disk-image IMAGE query TEXT
 *
 * The positions are fixed, so TEXT is data by construction. A concept ID may
 * legally begin with `-` (the ID alphabet includes it), so a query argument that
 * spells `-x` or even `--disk-image` must reach the graph as a lookup text; the
 * option parser must never inspect it. Only argv[1] is ever compared with
 * `--disk-image`, which is the one place an option can appear, and an unknown
 * spelling there stays USAGE.
 *
 * Nothing here opens, stats or reads the image, so a usage error performs zero
 * I/O. */
static bool options_parse(int argc, char **argv, Options *options)
{
    memset(options, 0, sizeof *options);
    if (argc < 4 || strcmp(argv[1], "--disk-image") != 0) {
        return false;
    }
    if (argv[2][0] == '\0') {
        return false;
    }
    options->image = argv[2];
    if (strcmp(argv[3], "list") == 0) {
        return argc == 4;
    }
    if (strcmp(argv[3], "query") != 0 || argc != 5) {
        return false;
    }
    /* argv[4] is the query text, whatever it looks like. Its own rules are the
     * shared 0029 single-line qualification plus the 1..256 byte bound. */
    size_t length = strlen(argv[4]);
    if (length == 0u || length > YAN_KG_QUERY_MAX ||
        !yan_semantic_query_valid((const uint8_t *)argv[4], length)) {
        return false;
    }
    options->operation = OPERATION_QUERY;
    options->query = argv[4];
    options->query_length = (uint32_t)length;
    return true;
}

/* ----------------------------------------------------------------- JSON output */

/* One small growable byte buffer. The whole object is built here and written in
 * a single call, so no partially encoded object reaches stdout. */
typedef struct {
    char *bytes;
    size_t length, capacity;
    bool failed;
} Buffer;

static void buffer_reserve(Buffer *buffer, size_t extra)
{
    if (buffer->failed) {
        return;
    }
    if (extra <= buffer->capacity - buffer->length) {
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
    if (buffer->failed) {
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

static void buffer_query(Buffer *buffer, const YanKg *graph, const Options *options,
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

/* ------------------------------------------------------------------- failures */

static int fail(YanKgResult result)
{
    fprintf(stderr, "ERROR %s\n", yan_kg_result_name(result));
    return 1;
}

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

/* Observation: drop the directory cache and mount again, so the directory block
 * is really re-read, then digest the logical source. This is the same
 * change-detection shape the 0033 block query uses. */
static YanKgResult observe(YanSemanticPosix *adapter, YanSemanticSource *source,
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
    yan_semantic_posix_bind_source(adapter, source);
    YanSemanticResult result = yan_semantic_source_digest(source, digest);
    switch (result) {
    case YAN_SEMANTIC_OK: return YAN_KG_OK;
    /* A digest that cannot allocate is an I/O failure of the caller's host, not
     * a graph content limit. */
    case YAN_SEMANTIC_IO: case YAN_SEMANTIC_NOSPACE: return YAN_KG_IO;
    case YAN_SEMANTIC_INVALID: return YAN_KG_INVALID;
    case YAN_SEMANTIC_CORRUPT: return YAN_KG_CORRUPT;
    case YAN_SEMANTIC_UNSUPPORTED: return YAN_KG_UNSUPPORTED;
    case YAN_SEMANTIC_MISSING: return YAN_KG_PROTOCOL;
    case YAN_SEMANTIC_NOT_FOUND: return YAN_KG_PROTOCOL;
    default: return YAN_KG_IO;
    }
}

int main(int argc, char **argv)
{
    Options options;
    if (!options_parse(argc, argv, &options)) {
        usage();
        return 2;
    }
    /* A closed stdout must be an error at write time, not a signal that kills
     * the process in the middle of the object. */
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = SIG_IGN;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGPIPE, &action, NULL) != 0) {
        return fail(YAN_KG_IO);
    }

    YanSemanticPosix *adapter = calloc(1u, sizeof *adapter);
    if (adapter == NULL) {
        return fail(YAN_KG_NOSPACE);
    }
    adapter->image_fd = -1;
    adapter->index_fd = -1;

    YanKg graph;
    memset(&graph, 0, sizeof graph);
    YanKgSummary summary;
    memset(&summary, 0, sizeof summary);
    YanSemanticSource source;
    memset(&source, 0, sizeof source);
    uint8_t before[32], after[32];

    /* The graph stays published until the bytes are encoded, so it is destroyed
     * only on the way out of every path below. */
    YanKgResult result = map_open(yan_semantic_posix_open_image_ex(adapter,
                                                                   options.image));
    if (result == YAN_KG_OK) {
        result = observe(adapter, &source, before);
    }
    if (result == YAN_KG_OK) {
        result = yan_kg_build(&graph, &source, &summary);
    }
    const YanKgConcept *concept = NULL;
    if (result == YAN_KG_OK && options.operation == OPERATION_QUERY) {
        YanKgMatch match;
        memset(&match, 0, sizeof match);
        result = yan_kg_lookup(&graph, (const uint8_t *)options.query,
                              options.query_length, &match);
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
        return fail(result);
    }

    /* Re-observe the image through a fresh image-only adapter, so the directory
     * block is really re-read and the comparison is not satisfied by a cache. */
    YanSemanticPosix *again = calloc(1u, sizeof *again);
    if (again == NULL) {
        yan_kg_destroy(&graph);
        return fail(YAN_KG_NOSPACE);
    }
    again->image_fd = -1;
    again->index_fd = -1;
    YanKgResult reobserved = map_open(yan_semantic_posix_open_image_ex(
        again, options.image));
    if (reobserved == YAN_KG_OK) {
        YanSemanticSource latest;
        memset(&latest, 0, sizeof latest);
        reobserved = observe(again, &latest, after);
    }
    YanSemanticResult reclosed = yan_semantic_posix_cleanup(again);
    free(again);
    if (reobserved == YAN_KG_OK && reclosed != YAN_SEMANTIC_OK) {
        reobserved = YAN_KG_IO;
    }
    if (reobserved != YAN_KG_OK) {
        yan_kg_destroy(&graph);
        return fail(reobserved);
    }
    if (memcmp(before, after, sizeof before) != 0) {
        yan_kg_destroy(&graph);
        return fail(YAN_KG_STALE);
    }

    /* Encode with the digest that was actually verified on both sides. */
    Buffer output;
    memset(&output, 0, sizeof output);
    if (options.operation == OPERATION_LIST) {
        buffer_list(&output, &graph, after, &summary);
    } else {
        buffer_query(&output, &graph, &options, after, &summary, concept);
    }
    yan_kg_destroy(&graph);
    if (output.failed) {
        free(output.bytes);
        return fail(YAN_KG_NOSPACE);
    }
    bool written = fwrite(output.bytes, 1u, output.length, stdout) == output.length;
    bool newline = fputc('\n', stdout) != EOF;
    bool flushed = fflush(stdout) == 0;
    free(output.bytes);
    if (!written || !newline || !flushed) {
        return fail(YAN_KG_IO);
    }
    return 0;
}
