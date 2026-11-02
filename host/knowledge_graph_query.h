#ifndef YAN_HOST_KNOWLEDGE_GRAPH_QUERY_H
#define YAN_HOST_KNOWLEDGE_GRAPH_QUERY_H

#include "knowledge_graph.h"

#include <signal.h>
#include <stddef.h>
#include <stdint.h>

/* 0045 R1 shared image graph query. Contract:
 * docs/specs/0045-graph-mcp-tools.md R1 and
 * docs/decisions/0009-graph-agent-access.md.
 *
 * This is the single implementation of "one graph build over a stable image,
 * observed before and after, encoded as one JSON object" that both consumers
 * need: the 0044 `yan_graph` command and the MCP graph worker. The source is
 * opened twice on purpose -- once for the build and once, through a fresh
 * adapter, for the post observation -- so change detection cannot be satisfied
 * by a cached directory block. The 0044 JSON schema, field order, page caps,
 * canonical row order and error tokens are the ones defined by 0044 R2; this
 * module is where they live now, and the command keeps only argument parsing,
 * SIGPIPE handling and the stdout write.
 *
 * It borrows the existing image-only read adapter and the 0033 source digest,
 * so it needs no CURL, yyjson, model, runner, semantic index or extra process,
 * and it writes nothing to the image or its attachments. The portable graph
 * core (knowledge_graph.h/.c) is unchanged and still has no POSIX dependency;
 * only this module is Linux/WSL.
 *
 * Order of work, which is what makes the failure contract true:
 *   validate every argument (no I/O at all) -> observe the cancellation flag ->
 *   open+mount -> observe digest A -> compare expected_source (STALE) -> build ->
 *   select the queried concept -> close the build source (a close error outranks
 *   OK) -> re-open and observe digest B through a fresh adapter -> compare A/B
 *   (STALE) -> encode the whole object into one buffer -> publish it.
 * Every failure therefore leaves *payload NULL and *length 0, so a caller
 * cannot transmit half an object; only the caller's own stdout/write failure
 * can leave a prefix behind. */

typedef enum {
    YAN_KG_QUERY_LIST = 0,
    YAN_KG_QUERY_LOOKUP
} YanKgQueryOperation;

/* All pointers are borrowed for the duration of the call only.
 *
 * image           non-empty path of the offline YanFS image.
 * query/length    YAN_KG_QUERY_LOOKUP only: the 1..YAN_KG_QUERY_MAX byte text
 *                 that must pass the shared 0029 single-line qualification.
 *                 Ignored by YAN_KG_QUERY_LIST.
 * expected_source optional 32-byte logical source digest (0033 identity). A
 *                 mismatch is STALE and is decided before the graph is built,
 *                 so a stale request never pays for a build.
 * cancel          optional borrowed flag. It is polled once before any I/O and
 *                 between every source enumeration/read callback of both the
 *                 digest passes and the build, so a cancelled request stops
 *                 inside the borrowed source instead of finishing. In practice
 *                 the earliest in-pass observation point is the read callback
 *                 of the INITIAL source-identity digest, which is therefore
 *                 where a request that is cancelled mid-flight normally stops;
 *                 reaching the build's callbacks requires the flag to be raised
 *                 later than that pass, and the shipped tests drive the digest
 *                 callback. Cancellation
 *                 reports YAN_KG_IO: the caller knows it cancelled and
 *                 suppresses the response, and no new core enumerator is added
 *                 for a condition the core does not model.
 * output_limit    optional cap on the encoded object's byte length; 0 means
 *                 unlimited. Exceeding it is YAN_KG_LIMIT, which is distinct
 *                 from a failed allocation (YAN_KG_NOSPACE); an object exactly
 *                 output_limit bytes long is accepted. The 0044 command passes
 *                 0 and keeps its 0044 behaviour; the MCP worker passes the
 *                 0033 payload bound. */
typedef struct {
    YanKgQueryOperation operation;
    const char *image;
    const char *query;
    uint32_t query_length;
    const uint8_t *expected_source;
    const volatile sig_atomic_t *cancel;
    size_t output_limit;
} YanKgQueryOptions;

/* Encodes one complete 0044 JSON object (no trailing newline) for `options`.
 *
 * On YAN_KG_OK, *payload is a malloc-owned, NUL-terminated buffer of *length
 * bytes that the caller frees with free(); the buffer and its length are the
 * only allocations that survive the call, because the graph, both adapters and
 * every source are released before returning. On any failure *payload is NULL,
 * *length is 0 and no partial object exists; that statement is about these two
 * output parameters, not about a NULL `payload`/`length` argument, which is
 * rejected as YAN_KG_INVALID before anything else happens.
 *
 * The returned object describes a source that was observed before and after the
 * build and closed successfully on both sides; A/B observation is change
 * detection, not a snapshot, so the image must otherwise remain stable for the
 * duration of the call. */
YanKgResult yan_kg_query_json(const YanKgQueryOptions *, char **payload,
                              size_t *length);

#endif
