#ifndef YAN_HOST_SEMANTIC_H
#define YAN_HOST_SEMANTIC_H

#include "semantic_corpus.h"

#define YAN_SEMANTIC_DIMENSION UINT32_C(1024)
#define YAN_SEMANTIC_TOP UINT32_C(20)
#define YAN_SEMANTIC_HEADER_BYTES UINT32_C(256)
#define YAN_SEMANTIC_RECORD_BYTES UINT32_C(4160)

/* Synchronous borrowed interfaces. Context spans are supplied for detectable
 * range/alias validation. Model, runner and configuration hashes are computed
 * from actual files/configuration by the adapter, never from a display name.
 * Source stability and a single index writer are caller preconditions. */
typedef struct {
    uint8_t model[32], runner[32], config[32];
} YanSemanticIdentity;
typedef struct {
    void *context;
    size_t context_size;
    YanSemanticResult (*start)(void *);
    YanSemanticResult (*embed)(void *, const uint8_t *, size_t,
                               float out[YAN_SEMANTIC_DIMENSION]);
    /* stop reaps only its owned child on success and every failure path. */
    YanSemanticResult (*stop)(void *);
} YanSemanticProvider;
typedef struct {
    void *context;
    size_t context_size;
    YanSemanticResult (*open_read)(void *, uint64_t *size);
    YanSemanticResult (*read)(void *, uint64_t offset, uint8_t *, size_t);
    YanSemanticResult (*close_read)(void *);
    YanSemanticResult (*begin_write)(void *);
    YanSemanticResult (*write)(void *, const uint8_t *, size_t);
    YanSemanticResult (*finish_write)(void *);
    YanSemanticResult (*publish)(void *);
    void (*discard)(void *);
} YanSemanticStore;
typedef enum {
    YAN_SEMANTIC_INDEX_READY = 0, YAN_SEMANTIC_INDEX_MISSING,
    YAN_SEMANTIC_INDEX_STALE, YAN_SEMANTIC_INDEX_CORRUPT
} YanSemanticIndexState;
typedef struct {
    YanSemanticIndexState state;
    uint32_t segments, skipped, shown;
} YanSemanticSummary;

/* 0030: one successful search's verified identities. Published only after all
 * borrowed row callbacks succeed; failure preserves every output byte. */
typedef struct {
    YanSemanticSummary summary;
    YanSemanticIdentity identity;
    uint8_t source_sha256[32];
} YanSemanticEvidence;
typedef struct {
    YanSemanticProvenance provenance;
    double score;
    const uint8_t *raw;
    uint32_t raw_length;
} YanSemanticRow;
typedef bool (*YanSemanticRowFn)(void *, const YanSemanticRow *);

/* Zero initialize before first init. Core owns bounded temporary heap (at most
 * 8192 vectors plus corpus and selected snippets); no model implementation,
 * console, filesystem or POSIX call belongs here. close_source propagates
 * adapter close errors and is called before output or atomic publication.
 * Each init binding is borrowed for one command. BUSY is checked before other
 * input/holder validation and starts no new work. A failure preserves summary;
 * status observations OK publish state, with counts only when READY. */
typedef struct {
    bool initialized, busy;
    YanSemanticSource source;
    YanSemanticProvider provider;
    YanSemanticStore store;
    YanSemanticIdentity identity;
    YanSemanticResult (*close_source)(void *);
} YanSemantic;

YanSemanticResult yan_semantic_init(YanSemantic *, const YanSemanticSource *,
                                     const YanSemanticProvider *,
                                     const YanSemanticStore *,
                                     const YanSemanticIdentity *,
                                     YanSemanticResult (*close_source)(void *));
YanSemanticResult yan_semantic_rebuild(YanSemantic *, YanSemanticSummary *);
YanSemanticResult yan_semantic_status(YanSemantic *, YanSemanticSummary *);
YanSemanticResult yan_semantic_search(YanSemantic *, const uint8_t *query,
                                     size_t length, YanSemanticRowFn sink,
                                     void *sink_context, YanSemanticSummary *);
/* Mandatory output: its entire span must be disjoint from query, facade and
 * bound contexts. No second scan or post-query index read supplies identities.
 * As with search, raw is borrowed only during each synchronous callback. */
YanSemanticResult yan_semantic_search_evidence(YanSemantic *, const uint8_t *query,
    size_t length, YanSemanticRowFn sink, void *sink_context, YanSemanticEvidence *);
#endif
