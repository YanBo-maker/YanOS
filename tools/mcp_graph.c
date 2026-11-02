/* 0045 R2: graph tools over the existing MCP.
 *
 * Contract: docs/specs/0045-graph-mcp-tools.md R2 and
 * docs/decisions/0009-graph-agent-access.md.
 *
 * This file adds only what is graph-specific: the two gated tool schemas and
 * the adapter that calls the shared 0045 R1 service. It contains no JSON
 * encoder of its own -- the object it publishes is the 0044 object produced by
 * host/knowledge_graph_query.c, so the CLI and these tools cannot drift apart in
 * fields, order, page caps or error vocabulary.
 *
 * The schemas describe exactly the fields that object carries. They advertise
 * no mastery, confidence or "understood" field, because none exists and none
 * may be inferred: an empty answer means the notes declare no such concept.
 *
 * Schema text is composed once at runtime from short literals because
 * -Wpedantic -Werror rejects string literals over C17's 4095-byte minimum
 * (-Woverlength-strings), and adjacent-literal concatenation still forms one
 * overlength literal. */

#include "mcp_graph.h"
#include "knowledge_graph_query.h"

#include <stdlib.h>
#include <string.h>

/* One provenance object, shared by a declaration slice and a proposition's
 * `where`, because both use the same ten locked fields. */
static const char provenance_schema[] =
    "{\"type\":\"object\",\"additionalProperties\":false,\"required\":"
    "[\"file\",\"block_id\",\"declaration_line\",\"declaration_start\","
    "\"declaration_end\",\"block_marker_line\",\"block_first_line\","
    "\"block_last_line\",\"block_start\",\"block_end\"],\"properties\":{"
    "\"file\":{\"type\":\"string\"},\"block_id\":{\"type\":\"string\"},"
    "\"declaration_line\":{\"type\":\"integer\",\"minimum\":0},"
    "\"declaration_start\":{\"type\":\"integer\",\"minimum\":0},"
    "\"declaration_end\":{\"type\":\"integer\",\"minimum\":0},"
    "\"block_marker_line\":{\"type\":\"integer\",\"minimum\":0},"
    "\"block_first_line\":{\"type\":\"integer\",\"minimum\":0},"
    "\"block_last_line\":{\"type\":\"integer\",\"minimum\":0},"
    "\"block_start\":{\"type\":\"integer\",\"minimum\":0},"
    "\"block_end\":{\"type\":\"integer\",\"minimum\":0}}}";

static const char hash_schema[] =
    "\"expected_source\":{\"type\":\"string\","
    "\"pattern\":\"^[0-9a-f]{64}$\"}";

static const char annotations[] =
    "\"annotations\":{\"readOnlyHint\":true,\"destructiveHint\":false,"
    "\"idempotentHint\":true,\"openWorldHint\":false}";

static const char source_sha256_schema[] =
    "\"source_sha256\":{\"type\":\"string\","
    "\"pattern\":\"^[0-9a-f]{64}$\"}";

/* A tiny append-only composer over one static buffer per schema. Both tools are
 * built once, on first use, and never modified afterwards, so the returned
 * pointer is as stable as a string literal. */
typedef struct {
    char *bytes;
    size_t length, capacity;
    bool failed;
} Composer;

static void add(Composer *composer, const char *text)
{
    if (composer->failed) {
        return;
    }
    size_t n = strlen(text);
    if (n > composer->capacity - composer->length) {
        composer->failed = true;
        return;
    }
    memcpy(composer->bytes + composer->length, text, n);
    composer->length += n;
    composer->bytes[composer->length] = '\0';
}

/* yanos_graph_list: the 0044 list object, unchanged. The buffers are sized with
 * room to spare for the composed schema; a composition that would not fit sets
 * `failed`, and the caller then reports a broken list rather than a truncated
 * one, so an undersized buffer can never publish half a schema. */
static char graph_list_text[4096];
static bool graph_list_ready;
static size_t graph_list_length;

const char *yan_mcp_graph_list_tool_json(void)
{
    if (graph_list_ready) {
        return graph_list_text;
    }
    Composer composer = {graph_list_text, 0u, sizeof graph_list_text - 1u,
                         false};
    graph_list_text[0] = '\0';
    add(&composer,
        "{\"name\":\"yanos_graph_list\",\"description\":\"Read-only explicit "
        "concepts declared by the notes in concept-ID order, first 20 of the "
        "full total, each with its alias, declaration and relation counts. No "
        "entry for a concept means the notes declare none; that is not a claim "
        "about what the user knows.\",\"inputSchema\":{\"type\":\"object\","
        "\"additionalProperties\":false,\"properties\":{");
    add(&composer, hash_schema);
    add(&composer,
        "}},\"outputSchema\":{\"type\":\"object\",\"additionalProperties\":"
        "false,\"required\":[\"version\",\"operation\",\"source_sha256\","
        "\"skipped\",\"total\",\"shown\",\"concepts\"],\"properties\":{"
        "\"version\":{\"const\":1},\"operation\":{\"const\":\"list\"},");
    add(&composer, source_sha256_schema);
    add(&composer,
        ",\"skipped\":{\"type\":\"integer\",\"minimum\":0},"
        "\"total\":{\"type\":\"integer\",\"minimum\":0},"
        "\"shown\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":20},"
        "\"concepts\":{\"type\":\"array\",\"maxItems\":20,\"items\":{"
        "\"type\":\"object\",\"additionalProperties\":false,\"required\":"
        "[\"id\",\"name\",\"alias_count\",\"declaration_count\","
        "\"relation_count\"],\"properties\":{\"id\":{\"type\":\"string\"},"
        "\"name\":{\"type\":\"string\"},"
        "\"alias_count\":{\"type\":\"integer\",\"minimum\":0},"
        "\"declaration_count\":{\"type\":\"integer\",\"minimum\":0},"
        "\"relation_count\":{\"type\":\"integer\",\"minimum\":0}}}}}},");
    add(&composer, annotations);
    add(&composer, "}");
    if (composer.failed) {
        return NULL;
    }
    graph_list_length = composer.length;
    graph_list_ready = true;
    return graph_list_text;
}

/* yanos_graph_query: the 0044 query object, unchanged. The query text bound is
 * the 0044 one (1..256 UTF-8 bytes, single line). */
static char graph_query_text[12288];
static bool graph_query_ready;

static void add_declaration_block(Composer *composer)
{
    add(composer,
        "\"declarations\":{\"type\":\"object\",\"additionalProperties\":false,"
        "\"required\":[\"total\",\"shown\",\"items\"],\"properties\":{"
        "\"total\":{\"type\":\"integer\",\"minimum\":0},"
        "\"shown\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":20},"
        "\"items\":{\"type\":\"array\",\"maxItems\":20,\"items\":");
    add(composer, provenance_schema);
    add(composer, "}}}");
}

static void add_relation_block(Composer *composer)
{
    add(composer,
        "\"relations\":{\"type\":\"object\",\"additionalProperties\":false,"
        "\"required\":[\"total\",\"shown\",\"items\"],\"properties\":{"
        "\"total\":{\"type\":\"integer\",\"minimum\":0},"
        "\"shown\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":20},"
        "\"items\":{\"type\":\"array\",\"maxItems\":20,\"items\":{"
        "\"type\":\"object\",\"additionalProperties\":false,\"required\":"
        "[\"type\",\"subject\",\"object\",\"direction\",\"where\"],"
        "\"properties\":{\"type\":{\"type\":\"string\"},"
        "\"subject\":{\"type\":\"object\",\"additionalProperties\":false,"
        "\"required\":[\"id\",\"name\"],\"properties\":{\"id\":{\"type\":"
        "\"string\"},\"name\":{\"type\":\"string\"}}},"
        "\"object\":{\"type\":\"object\",\"additionalProperties\":false,"
        "\"required\":[\"id\",\"name\"],\"properties\":{\"id\":{\"type\":"
        "\"string\"},\"name\":{\"type\":\"string\"}}},"
        "\"direction\":{\"enum\":[\"out\",\"in\",\"self\"]},\"where\":");
    add(composer, provenance_schema);
    add(composer, "}}}}}");
}

const char *yan_mcp_graph_query_tool_json(void)
{
    if (graph_query_ready) {
        return graph_query_text;
    }
    Composer composer = {graph_query_text, 0u, sizeof graph_query_text - 1u,
                         false};
    graph_query_text[0] = '\0';
    add(&composer,
        "{\"name\":\"yanos_graph_query\",\"description\":\"Read-only lookup of "
        "one declared concept by exact ID, else a unique name or alias. Returns "
        "the complete alias array, the first 20 declarations and the first 20 "
        "propositions, each with the block provenance needed to read the "
        "original text. Not finding a concept means the notes declare none.\","
        "\"inputSchema\":{\"type\":\"object\",\"additionalProperties\":false,"
        "\"required\":[\"query\"],\"properties\":{\"query\":{\"type\":"
        "\"string\",\"minLength\":1,\"maxLength\":256},");
    add(&composer, hash_schema);
    add(&composer,
        "}},\"outputSchema\":{\"type\":\"object\",\"additionalProperties\":"
        "false,\"required\":[\"version\",\"operation\",\"source_sha256\","
        "\"skipped\",\"query\",\"concept\",\"declarations\",\"relations\"],"
        "\"properties\":{\"version\":{\"const\":1},"
        "\"operation\":{\"const\":\"query\"},");
    add(&composer, source_sha256_schema);
    add(&composer,
        ",\"skipped\":{\"type\":\"integer\",\"minimum\":0},"
        "\"query\":{\"type\":\"string\"},"
        "\"concept\":{\"type\":\"object\",\"additionalProperties\":false,"
        "\"required\":[\"id\",\"name\",\"aliases\"],\"properties\":{"
        "\"id\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},"
        "\"aliases\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}}},");
    add_declaration_block(&composer);
    add(&composer, ",");
    add_relation_block(&composer);
    /* Closes outputSchema.properties, then outputSchema. */
    add(&composer, "}},");
    add(&composer, annotations);
    /* Closes the root tool object. */
    add(&composer, "}");
    if (composer.failed) {
        return NULL;
    }
    graph_query_ready = true;
    return graph_query_text;
}

/* ------------------------------------------------------------------ execution */

YanKgResult yan_mcp_graph_execute(const char *image,
                                  YanKgQueryOperation operation,
                                  const char *query, uint32_t query_length,
                                  const uint8_t *expected,
                                  const volatile sig_atomic_t *cancel,
                                  char **payload, size_t *length)
{
    if (payload == NULL || length == NULL) {
        return YAN_KG_INVALID;
    }
    *payload = NULL;
    *length = 0u;
    YanKgQueryOptions options;
    memset(&options, 0, sizeof options);
    options.operation = operation;
    options.image = image;
    options.query = query;
    options.query_length = query_length;
    options.expected_source = expected;
    options.cancel = cancel;
    /* The 1 MiB ceiling the MCP transport already enforces on any payload. The
     * service turns an over-cap object into LIMIT, so this can never become a
     * silently truncated success. */
    options.output_limit = YAN_MCP_GRAPH_PAYLOAD_MAX;
    return yan_kg_query_json(&options, payload, length);
}

const char *yan_mcp_graph_result_name(YanKgResult result)
{
    /* An explicit mapping of the shared 0044 enumerator. The value crosses a
     * domain boundary (worker frame), so it is never rendered by casting a
     * number from any other result enum. */
    switch (result) {
    case YAN_KG_OK: return NULL;
    case YAN_KG_INVALID: return "INVALID";
    case YAN_KG_BUSY: return "BUSY";
    case YAN_KG_NOSPACE: return "NOSPACE";
    case YAN_KG_UNSUPPORTED: return "UNSUPPORTED";
    case YAN_KG_METADATA: return "METADATA";
    case YAN_KG_BLOCK_FORMAT: return "BLOCK_FORMAT";
    case YAN_KG_BLOCK_AMBIGUOUS: return "BLOCK_AMBIGUOUS";
    case YAN_KG_BLOCK_LIMIT: return "BLOCK_LIMIT";
    case YAN_KG_GRAPH_FORMAT: return "GRAPH_FORMAT";
    case YAN_KG_LIMIT: return "LIMIT";
    case YAN_KG_CONFLICT: return "CONFLICT";
    case YAN_KG_UNRESOLVED: return "UNRESOLVED";
    case YAN_KG_AMBIGUOUS: return "AMBIGUOUS";
    case YAN_KG_NOT_FOUND: return "NOT_FOUND";
    case YAN_KG_STALE: return "STALE";
    case YAN_KG_CORRUPT: return "CORRUPT";
    case YAN_KG_IO: return "IO";
    case YAN_KG_PROTOCOL: return "IO";
    }
    return "IO";
}
