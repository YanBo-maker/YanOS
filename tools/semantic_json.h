#ifndef YAN_TOOLS_SEMANTIC_JSON_H
#define YAN_TOOLS_SEMANTIC_JSON_H

/* Private optional Host CLI support, never included by the semantic core. */
#include "semantic.h"
#include <yyjson.h>

typedef struct {
    YanSemanticRow row;
    uint8_t raw[2048];
} YanSemanticJsonRow;
typedef struct {
    uint32_t count;
    YanSemanticJsonRow rows[YAN_SEMANTIC_TOP];
} YanSemanticJsonRows;

/* Complete object plus LF. Caller releases with allocator->free, or free when
 * allocator is NULL. Custom allocators also support allocation-failure tests. */
char *yan_semantic_json_encode(const char *query, const YanSemanticJsonRows *,
    const YanSemanticEvidence *, size_t *length, const yyjson_alc *allocator);
#endif
