#ifndef YAN_HOST_SEMANTIC_RESULT_H
#define YAN_HOST_SEMANTIC_RESULT_H

/* Shared 0029 Host semantic-search vocabulary.
 *
 * The contract is docs/specs/0029-host-semantic-search.md, "入口与可观测结果"
 * and "向量排名与API责任". This header carries only the result enum the pure
 * Host pieces (SHA-256 identity input, streaming text/chunker) report, so the
 * later facade, index and provider layers can share one set of names instead
 * of each re-spelling "OK"/"INVALID"/"STOPPED".
 *
 * The names mirror the 0027 lifecycle where they overlap: OK, INVALID and
 * STOPPED mean the same kind of call outcome, BUSY is a re-entrant/active
 * object, and UNSUPPORTED is an honest "this build or this input is not
 * handled" answer. The remaining names are reserved for the facade so a later
 * CLI mapping (exit 2/3/4/5) can use the same enum without a second
 * translation. This header declares no storage, allocates nothing and has no
 * dependency on Host, YanFS, console or the provider. */

typedef enum {
    /* The call did what its contract says. */
    YAN_SEMANTIC_OK = 0,
    /* Caller error: null/aliased/out-of-range argument, malformed query, or a
     * sequence the pure layer refuses. */
    YAN_SEMANTIC_INVALID = 1,
    /* The object is already inside a call; no new work was started. */
    YAN_SEMANTIC_BUSY = 2,
    /* A consumer callback returned false and the operation stopped cleanly. */
    YAN_SEMANTIC_STOPPED = 3,
    /* Honest not-implemented / not-supported answer, never a silent fallback. */
    YAN_SEMANTIC_UNSUPPORTED = 4,
    /* Reserved for the facade: broken note header on otherwise legal text. */
    YAN_SEMANTIC_METADATA = 5,
    /* Reserved: value does not fit the declared cap or buffer. */
    YAN_SEMANTIC_TOO_LARGE = 6,
    /* Reserved: the source-wide 8192-segment cap was exceeded. */
    YAN_SEMANTIC_LIMIT = 7,
    /* Reserved: index structure/checksum is bad. */
    YAN_SEMANTIC_CORRUPT = 8,
    /* Reserved: identity changed, an explicit rebuild is required. */
    YAN_SEMANTIC_STALE = 9,
    /* Reserved: no index is present. */
    YAN_SEMANTIC_MISSING = 10,
    /* Reserved: model/runner dependency is missing or unreadable. */
    YAN_SEMANTIC_DEPENDENCY = 11,
    /* Reserved: the inference process answered badly. */
    YAN_SEMANTIC_PROVIDER = 12,
    /* Reserved: a deadline expired. */
    YAN_SEMANTIC_TIMEOUT = 13,
    /* Reserved: an I/O call failed. */
    YAN_SEMANTIC_IO = 14,
    /* Reserved: the operation was cancelled. */
    YAN_SEMANTIC_CANCELLED = 15,
    /* Reserved: output space was insufficient. */
    YAN_SEMANTIC_NOSPACE = 16,
    /* Reserved: an addressed logical file does not exist. */
    YAN_SEMANTIC_NOT_FOUND = 17
} YanSemanticResult;

/* Stable ASCII token for diagnostics and readable Unity failure messages.
 * Never returns NULL. Unused by the pure modules, so an unused static inline
 * definition is harmless; the switch keeps every enumerator explicit for the
 * later CLI mapping. */
static inline const char *yan_semantic_result_name(YanSemanticResult result)
{
    switch (result) {
    case YAN_SEMANTIC_OK: return "OK";
    case YAN_SEMANTIC_INVALID: return "INVALID";
    case YAN_SEMANTIC_BUSY: return "BUSY";
    case YAN_SEMANTIC_STOPPED: return "STOPPED";
    case YAN_SEMANTIC_UNSUPPORTED: return "UNSUPPORTED";
    case YAN_SEMANTIC_METADATA: return "METADATA";
    case YAN_SEMANTIC_TOO_LARGE: return "TOO_LARGE";
    case YAN_SEMANTIC_LIMIT: return "LIMIT";
    case YAN_SEMANTIC_CORRUPT: return "CORRUPT";
    case YAN_SEMANTIC_STALE: return "STALE";
    case YAN_SEMANTIC_MISSING: return "MISSING";
    case YAN_SEMANTIC_DEPENDENCY: return "DEPENDENCY";
    case YAN_SEMANTIC_PROVIDER: return "PROVIDER";
    case YAN_SEMANTIC_TIMEOUT: return "TIMEOUT";
    case YAN_SEMANTIC_IO: return "IO";
    case YAN_SEMANTIC_CANCELLED: return "CANCELLED";
    case YAN_SEMANTIC_NOSPACE: return "NOSPACE";
    case YAN_SEMANTIC_NOT_FOUND: return "NOT_FOUND";
    default: return "UNKNOWN";
    }
}

#endif
