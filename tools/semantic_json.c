#include "semantic_json.h"
#include <string.h>

_Static_assert(YYJSON_VERSION_HEX == 0x000D00, "yyjson 0.13.0 required");

static bool add_hash(yyjson_mut_doc *doc, yyjson_mut_val *obj,
                     const char *key, const uint8_t hash[32])
{
    static const char digits[] = "0123456789abcdef";
    char text[64];
    for (unsigned i = 0; i < 32; ++i) {
        text[2*i] = digits[hash[i] >> 4]; text[2*i+1] = digits[hash[i] & 15u];
    }
    return yyjson_mut_obj_add_strncpy(doc, obj, key, text, sizeof text);
}

char *yan_semantic_json_encode(const char *query, const YanSemanticJsonRows *saved,
    const YanSemanticEvidence *evidence, size_t *length, const yyjson_alc *allocator)
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(allocator);
    if (doc == NULL) return NULL;
    yyjson_mut_val *root = yyjson_mut_obj(doc), *array = yyjson_mut_arr(doc);
    yyjson_mut_doc_set_root(doc, root);
    bool ok = root != NULL && array != NULL &&
        yyjson_mut_obj_add_uint(doc, root, "schema_version", 1) &&
        yyjson_mut_obj_add_strncpy(doc, root, "query", query, strlen(query)) &&
        add_hash(doc, root, "source_sha256", evidence->source_sha256) &&
        add_hash(doc, root, "model_sha256", evidence->identity.model) &&
        add_hash(doc, root, "runner_sha256", evidence->identity.runner) &&
        add_hash(doc, root, "config_sha256", evidence->identity.config) &&
        yyjson_mut_obj_add_uint(doc, root, "segments", evidence->summary.segments) &&
        yyjson_mut_obj_add_uint(doc, root, "skipped", evidence->summary.skipped) &&
        yyjson_mut_obj_add_uint(doc, root, "shown", evidence->summary.shown) &&
        yyjson_mut_obj_add_val(doc, root, "results", array);
    for (uint32_t i = 0; ok && i < saved->count; ++i) {
        const YanSemanticRow *row = &saved->rows[i].row;
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        ok = item != NULL &&
            yyjson_mut_obj_add_strncpy(doc, item, "file", row->provenance.name,
                                       strlen(row->provenance.name)) &&
            yyjson_mut_obj_add_uint(doc, item, "first_line", row->provenance.first_line) &&
            yyjson_mut_obj_add_uint(doc, item, "last_line", row->provenance.last_line) &&
            yyjson_mut_obj_add_uint(doc, item, "start", row->provenance.start) &&
            yyjson_mut_obj_add_uint(doc, item, "end", row->provenance.end) &&
            yyjson_mut_obj_add_real(doc, item, "score", row->score) &&
            yyjson_mut_obj_add_strncpy(doc, item, "text", (const char *)row->raw,
                                       row->raw_length) &&
            yyjson_mut_arr_add_val(array, item);
    }
    char *encoded = ok ? yyjson_mut_write_opts(doc, 0, allocator, length, NULL) : NULL;
    yyjson_mut_doc_free(doc);
    /* Writer allocates len+1 bytes. Replace its terminator with final LF. */
    if (encoded != NULL) encoded[(*length)++] = '\n';
    return encoded;
}
