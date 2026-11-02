#ifndef YAN_HOST_KNOWLEDGE_GRAPH_H
#define YAN_HOST_KNOWLEDGE_GRAPH_H

#include "semantic_corpus.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 0044 R1 portable knowledge-graph core. Contract:
 * docs/specs/0044-evidence-knowledge-graph.md (R1 and "R2接口字段锁定") and
 * docs/decisions/0008-evidence-knowledge-graph.md.
 *
 * The only input is the borrowed 0029/0033 YanSemanticSource callback seam, so
 * this module needs no YanFS, POSIX, path, JSON, model or console dependency and
 * compiles unchanged on Windows and Linux. Text qualification, the 0032 block
 * grammar, the 0027 header parser, UTF-8 scalar decoding and the 0029 query
 * rules are reused from their owners; this module adds only `@kg` line
 * recognition, concept/alias aggregation and the indexes.
 *
 * `@kg` is reserved only at column 0 inside the content of a legal named block,
 * of a file whose complete 0032 text preflight already passed, after the 0027
 * header. Grammar is one ASCII space between the command word and each field:
 *
 *   @kg concept ID LABEL
 *   @kg alias ID LABEL
 *   @kg relation TYPE SUBJECT OBJECT
 *
 * A reserved line that is not a legal declaration, or that has no enclosing
 * block, is GRAPH_FORMAT; a TAB right after `@kg` is reserved and invalid, never
 * prose. Everything else is ordinary text.
 *
 * Ownership: every allocation belongs to the YanKg and is released by
 * yan_kg_destroy. A failed build frees its candidate and leaves the previous
 * graph and summary byte identical, so a published graph keeps serving. The
 * read-only views are borrowed until the next successful yan_kg_build or the
 * next yan_kg_destroy, and refer to each other by ordinal only, never by an
 * internal pointer-to-pointer. */

#define YAN_KG_CONCEPT_MAX UINT32_C(8192)
#define YAN_KG_DECLARATION_MAX UINT32_C(16384)
#define YAN_KG_ALIAS_MAX UINT32_C(16384)
#define YAN_KG_RELATION_MAX UINT32_C(16384)
#define YAN_KG_LABEL_MAX UINT32_C(256)
/* Shared bound for the R2 query text, applied by yan_kg_lookup itself. */
#define YAN_KG_QUERY_MAX UINT32_C(256)

typedef enum {
    YAN_KG_OK = 0,
    /* Null/aliased/out-of-range argument, malformed query, or a source whose
     * declared capacity is not 1..4095. */
    YAN_KG_INVALID,
    /* The graph is inside a build. */
    YAN_KG_BUSY,
    /* A candidate allocation failed. */
    YAN_KG_NOSPACE,
    /* The mounted format is not supported. */
    YAN_KG_UNSUPPORTED,
    /* Legal text with a broken 0027 header. */
    YAN_KG_METADATA,
    /* A broken `@block` marker line. */
    YAN_KG_BLOCK_FORMAT,
    /* A duplicate block ID inside one file. */
    YAN_KG_BLOCK_AMBIGUOUS,
    /* More than 64 blocks in one file. */
    YAN_KG_BLOCK_LIMIT,
    /* A reserved `@kg` line with no block, an unknown command word or an
     * invalid field. */
    YAN_KG_GRAPH_FORMAT,
    /* One of the four R1 caps was exceeded. */
    YAN_KG_LIMIT,
    /* One concept ID declared with two different labels. */
    YAN_KG_CONFLICT,
    /* An alias or relation names a concept ID that was never declared. */
    YAN_KG_UNRESOLVED,
    /* A label names more than one concept; never auto-merged. */
    YAN_KG_AMBIGUOUS,
    /* The queried text matches no concept ID, name or alias. */
    YAN_KG_NOT_FOUND,
    /* Reserved for the R2 CLI source-identity observation. */
    YAN_KG_STALE,
    /* The logical view violated the 0033 enumeration rules. */
    YAN_KG_CORRUPT,
    /* A source callback or the caller's I/O failed. */
    YAN_KG_IO,
    /* A source callback contradicted its contract. */
    YAN_KG_PROTOCOL
} YanKgResult;

/* One declaration occurrence's position in the original note. Byte offsets are
 * absolute half-open positions; lines are physical 1-based lines. The
 * declaration range excludes its CR/LF. block_* describe the whole enclosing
 * block content with the 0032 convention, so the R2 CLI can read it back
 * through the existing block tools. An empty block reports 0 block lines. */
typedef struct {
    char file[32];
    char block_id[32];
    uint32_t declaration_line;
    uint32_t declaration_start;
    uint32_t declaration_end;
    uint32_t block_marker_line;
    uint32_t block_first_line;
    uint32_t block_last_line;
    uint32_t block_start;
    uint32_t block_end;
} YanKgProvenance;

/* One retained proposition. subject/object are concept ordinals in the graph
 * that owns this relation; subject_id/object_id are the same concept IDs as
 * borrowed NUL-terminated text. A self loop has subject == object. */
typedef struct {
    const char *type;
    uint32_t subject;
    uint32_t object;
    const char *subject_id;
    const char *object_id;
    YanKgProvenance where;
} YanKgRelation;

/* A concept plus its read-only indexes. aliases, declarations and the three
 * adjacency arrays are sorted: aliases by label bytes, declarations and
 * relations by (file name, declaration byte). A self loop appears only in
 * `self`, so no proposition is listed twice for one concept; R2's
 * self/out/in direction is which array yielded the row. */
typedef struct {
    const char *id;
    const char *name;
    uint32_t alias_count;
    const char *const *aliases;
    uint32_t declaration_count;
    const YanKgProvenance *declarations;
    uint32_t out_count;
    uint32_t in_count;
    uint32_t self_count;
    const YanKgRelation *const *out;
    const YanKgRelation *const *in;
    const YanKgRelation *const *self;
} YanKgConcept;

typedef struct {
    uint32_t concepts;
    uint32_t relations;
    uint32_t declarations;
    uint32_t aliases;
    /* All visited logical files, and how many of them were skipped as non-text.
     * `files - skipped` is the set that was actually parsed. */
    uint32_t files;
    uint32_t skipped;
} YanKgSummary;

typedef enum {
    YAN_KG_MATCH_ID = 0,
    YAN_KG_MATCH_LABEL
} YanKgMatchKind;

/* One resolved query. candidate_count is 0 for NOT_FOUND, 1 for a hit, and the
 * number of DISTINCT matching concepts for AMBIGUOUS. concept is an ordinal and
 * is meaningful only when candidate_count == 1. */
typedef struct {
    YanKgMatchKind kind;
    uint32_t concept;
    uint32_t candidate_count;
} YanKgMatch;

/* One name/alias lookup table entry. `is_name` marks the concept's own name, so
 * the per-concept alias lists can be carved out of the same sorted table.
 * Public only so a caller can stack allocate the graph object. */
typedef struct {
    const char *text;
    uint32_t length;
    uint32_t concept;
    bool is_name;
} YanKgLabelRef;

/* Zero initialize before the first build. Every pointer is owned by this object
 * and a zeroed or already-destroyed object is a valid empty graph. */
typedef struct {
    YanKgConcept *concepts;
    YanKgRelation *relations;
    YanKgProvenance *declarations;
    YanKgLabelRef *labels;
    const char **alias_pool;
    uint32_t *out_offsets;
    uint32_t *in_offsets;
    uint32_t *self_offsets;
    const YanKgRelation **out_items;
    const YanKgRelation **in_items;
    const YanKgRelation **self_items;
    char *arena;
    uint32_t arena_size;
    uint32_t concept_count;
    uint32_t relation_count;
    uint32_t declaration_count;
    uint32_t alias_count;
    uint32_t label_count;
    YanKgSummary summary;
    int building;
} YanKg;

/* Streams the borrowed source into a candidate graph. `summary` is optional and
 * is written only on OK. Files are read in bounded chunks: there is no
 * whole-file buffer and no line-length limit for ordinary prose. */
YanKgResult yan_kg_build(YanKg *, const YanSemanticSource *, YanKgSummary *);
/* Releases every owned allocation and clears the object. Idempotent. */
void yan_kg_destroy(YanKg *);
/* Exact concept ID first, else a unique name or alias. Text is 1..256 bytes and
 * must pass the shared 0029 single-line query validator. */
YanKgResult yan_kg_lookup(const YanKg *, const uint8_t *text, uint32_t length,
                          YanKgMatch *);
const YanKgConcept *yan_kg_concept_at(const YanKg *, uint32_t ordinal);
const YanKgRelation *yan_kg_relation_at(const YanKg *, uint32_t ordinal);
uint32_t yan_kg_concept_count(const YanKg *);
uint32_t yan_kg_relation_count(const YanKg *);
void yan_kg_summary(const YanKg *, YanKgSummary *);
/* Stable ASCII token for diagnostics. Never NULL. */
const char *yan_kg_result_name(YanKgResult);

#endif
