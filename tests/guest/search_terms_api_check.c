/* Freestanding RV32IM probe for the 0026 term-search facade over a fake
 * backend.
 *
 * This links only os/search_terms.c, the shared core and text primitives, the
 * shared Guest start/memory and this file: no filesystem and no index are
 * needed, because the backend is a fake. It exercises the public facade
 * contract that a native 64-bit build cannot prove for RV32: uintptr_t span
 * wraparound, the four-byte snippet output holders and dual-holder aliasing,
 * the canonical-group count, the caller/summary/query/context/source alias
 * guards, the busy management reentry, and the sticky/zero-progress reader
 * rules. Every check has its own tohost code (starting at 2, so code 1 is
 * reserved for the healthy end). A run is accepted only when it both prints the
 * unique signature SEARCH_TERMS_API_PASS and writes tohost 1; the driver
 * requires both, so a run that only prints the marker or only writes tohost 1
 * is not accepted.
 *
 * Sparse boundaries are probed honestly: the near-UINTPTR_MAX values are holder
 * base addresses rejected by span arithmetic, not buffers, and the reader hands
 * back one static byte. The facade and the fake backend are static and
 * zero-initialized.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "guest.h"
#include "platform.h"
#include "search_terms.h"

typedef struct {
    const uint8_t *body;
    uint32_t length; /* snippet length advertised in the match */
    uint32_t queries;
    uint32_t reads;
    uint32_t status_calls;
    YanSearchTermsResult read_result;
    bool zero_progress;
} Fake;

static YanSearchTerms *probe_terms;
static YanSearchTermsResult probe_result;
static uint32_t probe_length;

static Fake fake;
static YanSearchTerms *reentry_terms;
static YanSearchTermsResult reentry_result;
static uint32_t reentry_kind; /* 0 none, 1 status, 2 clear, 3 rebuild, 4 init */
static bool holder_unchanged;

static YanSearchTermsResult fake_read(void *context, uint32_t offset,
                                      uint32_t capacity, const uint8_t **bytes,
                                      uint32_t *length);
static YanSearchTermsBackend make_backend(Fake *fake);

static void run_reentry(void)
{
    switch (reentry_kind) {
    case 1u: {
        /* Fill a holder with a sentinel, keep a byte copy, then require a BUSY
         * answer to leave every byte (padding included) untouched. */
        YanSearchTermsStatus holder;
        holder.state = YAN_SEARCH_INDEX_READY;
        holder.source = YAN_SEARCH_SOURCE_MOUNTED;
        holder.terms = 0xDEADBEEFu;
        holder.postings = 0xFEEDFACEu;
        uint8_t before[sizeof holder];
        memcpy(before, &holder, sizeof holder);
        reentry_result = yan_search_terms_status(reentry_terms, &holder);
        holder_unchanged = memcmp(before, &holder, sizeof holder) == 0;
        break;
    }
    case 2u:
        reentry_result = yan_search_terms_clear(reentry_terms);
        break;
    case 3u:
        reentry_result = yan_search_terms_rebuild(reentry_terms);
        break;
    case 4u:
        reentry_result = yan_search_terms_init(reentry_terms,
                                               make_backend(&fake));
        break;
    default:
        break;
    }
}

static YanSearchTermsResult fake_query(void *context, const uint8_t *query,
                                       uint32_t query_length,
                                       YanSearchTermsMatchFn match,
                                       void *match_context,
                                       YanSearchTermsSummary *summary)
{
    Fake *self = (Fake *)context;
    (void)query;
    (void)query_length;
    ++self->queries;
    YanSearchTermsMatch value;
    value.name = "f";
    value.line_number = 1u;
    value.score = 1u;
    value.snippet_length = self->length;
    value.left_truncated = false;
    value.right_truncated = false;
    (void)match(match_context, &value);
    if (summary != NULL) {
        summary->total = 1u;
        summary->shown = 1u;
        summary->skipped = 0u;
        summary->mode = YAN_SEARCH_MODE_INDEX;
    }
    return YAN_SEARCH_TERMS_OK;
}

static YanSearchTermsResult fake_read(void *context, uint32_t offset,
                                      uint32_t capacity, const uint8_t **bytes,
                                      uint32_t *length)
{
    Fake *self = (Fake *)context;
    ++self->reads;
    if (self->read_result != YAN_SEARCH_TERMS_OK) {
        return self->read_result;
    }
    if (offset >= self->length) {
        *bytes = NULL;
        *length = 0u;
        return YAN_SEARCH_TERMS_OK;
    }
    if (self->zero_progress) {
        *bytes = NULL;
        *length = 0u;
        return YAN_SEARCH_TERMS_OK;
    }
    uint32_t want = 1u; /* one static byte, never a large buffer */
    if (capacity < want) {
        want = capacity;
    }
    *bytes = self->body;
    *length = want;
    return YAN_SEARCH_TERMS_OK;
}

static YanSearchTermsResult fake_status(void *context,
                                        YanSearchTermsStatus *out)
{
    Fake *self = (Fake *)context;
    ++self->status_calls;
    run_reentry();
    if (out != NULL) {
        out->state = YAN_SEARCH_INDEX_READY;
        out->source = YAN_SEARCH_SOURCE_MOUNTED;
        out->terms = 1u;
        out->postings = 1u;
    }
    return YAN_SEARCH_TERMS_OK;
}

static YanSearchTermsResult fake_rebuild(void *context)
{
    (void)context;
    return YAN_SEARCH_TERMS_OK;
}

static YanSearchTermsResult fake_clear(void *context)
{
    (void)context;
    return YAN_SEARCH_TERMS_OK;
}

static YanSearchTermsBackend make_backend(Fake *fake)
{
    YanSearchTermsBackend backend;
    backend.context = fake;
    backend.context_size = sizeof *fake;
    backend.source_context = NULL;
    backend.source_size = 0u;
    backend.query = fake_query;
    backend.read_match = fake_read;
    backend.status = fake_status;
    backend.rebuild = fake_rebuild;
    backend.clear = fake_clear;
    return backend;
}

static bool read_keep(void *context, const YanSearchTermsMatch *match)
{
    (void)context;
    const uint8_t *bytes = NULL;
    uint32_t length = 0u;
    probe_result = yan_search_terms_read_snippet(
        probe_terms, 0u, match->snippet_length, &bytes, &length);
    probe_length = length;
    return true;
}

static bool read_once(void *context, const YanSearchTermsMatch *match)
{
    (void)context;
    const uint8_t *bytes = NULL;
    uint32_t length = 0u;
    probe_result = yan_search_terms_read_snippet(
        probe_terms, 0u, match->snippet_length, &bytes, &length);
    probe_length = length;
    return false;
}

static bool holder_alias(void *context, const YanSearchTermsMatch *match)
{
    (void)context;
    uint32_t length = 0u;
    probe_result = yan_search_terms_read_snippet(
        probe_terms, 0u, match->snippet_length,
        (const uint8_t **)(void *)probe_terms, &length);
    return false;
}

static bool dual_alias(void *context, const YanSearchTermsMatch *match)
{
    (void)context;
    uint32_t slot = 0u;
    probe_result = yan_search_terms_read_snippet(
        probe_terms, 0u, match->snippet_length,
        (const uint8_t **)(void *)&slot, &slot);
    return false;
}

static bool empty_reads(void *context, const YanSearchTermsMatch *match)
{
    (void)context;
    const uint8_t *bytes = NULL;
    uint32_t length = 0u;
    YanSearchTermsResult zero =
        yan_search_terms_read_snippet(probe_terms, 0u, 0u, &bytes, &length);
    YanSearchTermsResult end = yan_search_terms_read_snippet(
        probe_terms, match->snippet_length, 1u, &bytes, &length);
    probe_result = (zero == YAN_SEARCH_TERMS_OK && end == YAN_SEARCH_TERMS_OK &&
                    length == 0u)
                       ? YAN_SEARCH_TERMS_OK
                       : YAN_SEARCH_TERMS_PROTOCOL;
    return false;
}

static bool ignore_reader(void *context, const YanSearchTermsMatch *match)
{
    (void)context;
    const uint8_t *bytes = NULL;
    uint32_t length = 0u;
    (void)yan_search_terms_read_snippet(probe_terms, 0u, match->snippet_length,
                                        &bytes, &length);
    /* The caller ignores the reader result on purpose. */
    return true;
}

static bool holder_wrap(void *context, const YanSearchTermsMatch *match)
{
    (void)context;
    uint32_t length = 0u;
    probe_result = yan_search_terms_read_snippet(
        probe_terms, 0u, match->snippet_length,
        (const uint8_t **)(uintptr_t)0xFFFFFFFEu, &length);
    return false;
}

static bool reentry_match(void *context, const YanSearchTermsMatch *match)
{
    (void)context;
    (void)match;
    YanSearchTermsSink inner;
    inner.context = NULL;
    inner.match = read_once;
    inner.chunk = NULL;
    reentry_result = yan_search_terms(reentry_terms, (const uint8_t *)"x", 1u,
                                      inner, NULL);
    return false;
}

static void marker(const char *text)
{
    while (*text != '\0') {
        while (!yan_os_uart_tx_ready()) {
        }
        (void)yan_os_uart_put((uint8_t)*text);
        ++text;
    }
}

int main(void)
{
    static YanSearchTerms terms;
    static YanSearchTermsStatus status;
    static uint8_t region[64];
    static uint8_t source_region[16];
    static const uint8_t body[6] = {'n', 'e', 'e', 'd', 'l', 'e'};
    static const uint8_t with_nul[3] = {'a', 0u, 'b'};
    static const uint8_t with_lf[3] = {'a', (uint8_t)'\n', 'b'};
    static const uint8_t with_cr[3] = {'a', (uint8_t)'\r', 'b'};
    static const uint8_t with_del[3] = {'a', 0x7Fu, 'b'};
    static const uint8_t bad_utf8[2] = {0xFFu, 0xFEu};
    static const char groups17[] = "a b c d e f g h i j k l m n o p q";
    static const char separators_only[] = " /- ";
    static uint8_t word256[256];
    static uint8_t duplicates[128];
    YanSearchTermsSink sink;
    sink.context = NULL;
    sink.match = read_once;
    sink.chunk = NULL;

    memset(&fake, 0, sizeof fake);
    fake.body = body;
    fake.length = 6u;
    memset(word256, (int)'a', sizeof word256);
    uint32_t duplicate_length = 0u;
    for (uint32_t i = 0u; i < 17u; ++i) {
        if (i > 0u) {
            duplicates[duplicate_length++] = (uint8_t)' ';
        }
        duplicates[duplicate_length++] = (uint8_t)'C';
        duplicates[duplicate_length++] = (uint8_t)'P';
        duplicates[duplicate_length++] = (uint8_t)'U';
    }

    /* init: null, incomplete, malformed spans, aliases, then a valid one. */
    YanSearchTermsBackend backend = make_backend(&fake);
    guest_check(yan_search_terms_init(NULL, backend) == YAN_SEARCH_TERMS_INVALID,
                2u);
    backend.query = NULL;
    guest_check(yan_search_terms_init(&terms, backend) == YAN_SEARCH_TERMS_INVALID,
                3u);
    backend = make_backend(&fake);
    backend.read_match = NULL;
    guest_check(yan_search_terms_init(&terms, backend) == YAN_SEARCH_TERMS_INVALID,
                4u);
    backend = make_backend(&fake);
    backend.context = NULL;
    backend.context_size = 8u;
    guest_check(yan_search_terms_init(&terms, backend) == YAN_SEARCH_TERMS_INVALID,
                5u);
    backend = make_backend(&fake);
    backend.context = (void *)(uintptr_t)0xFFFFFFF0u;
    backend.context_size = 0x20u;
    guest_check(yan_search_terms_init(&terms, backend) == YAN_SEARCH_TERMS_INVALID,
                6u);
    backend = make_backend(&fake);
    backend.context = &terms;
    backend.context_size = sizeof terms;
    guest_check(yan_search_terms_init(&terms, backend) == YAN_SEARCH_TERMS_INVALID,
                7u);
    backend = make_backend(&fake);
    backend.source_context = &terms;
    backend.source_size = sizeof terms;
    guest_check(yan_search_terms_init(&terms, backend) == YAN_SEARCH_TERMS_INVALID,
                8u);
    backend = make_backend(&fake);
    backend.context = region;
    backend.context_size = 8u;
    backend.source_context = region + 4;
    backend.source_size = 8u;
    guest_check(yan_search_terms_init(&terms, backend) == YAN_SEARCH_TERMS_INVALID,
                9u);
    backend = make_backend(&fake);
    backend.source_context = source_region;
    backend.source_size = sizeof source_region;
    guest_check(yan_search_terms_init(&terms, backend) == YAN_SEARCH_TERMS_OK,
                10u);
    probe_terms = &terms;

    /* query: null/empty/limit, wrap, grammar, aliases, required callback. */
    guest_check(yan_search_terms(&terms, NULL, 3u, sink, NULL) ==
                    YAN_SEARCH_TERMS_INVALID, 11u);
    guest_check(yan_search_terms(&terms, body, 0u, sink, NULL) ==
                    YAN_SEARCH_TERMS_INVALID, 12u);
    guest_check(yan_search_terms(&terms, body, YAN_SEARCH_TERMS_QUERY_MAX + 1u,
                                 sink, NULL) == YAN_SEARCH_TERMS_INVALID, 13u);
    guest_check(yan_search_terms(&terms,
                                 (const uint8_t *)(uintptr_t)0xFFFFFFF0u, 0x20u,
                                 sink, NULL) == YAN_SEARCH_TERMS_INVALID, 14u);
    guest_check(yan_search_terms(&terms, with_nul, 3u, sink, NULL) ==
                    YAN_SEARCH_TERMS_INVALID, 15u);
    guest_check(yan_search_terms(&terms, with_lf, 3u, sink, NULL) ==
                    YAN_SEARCH_TERMS_INVALID, 16u);
    guest_check(yan_search_terms(&terms, with_cr, 3u, sink, NULL) ==
                    YAN_SEARCH_TERMS_INVALID, 17u);
    guest_check(yan_search_terms(&terms, with_del, 3u, sink, NULL) ==
                    YAN_SEARCH_TERMS_INVALID, 18u);
    guest_check(yan_search_terms(&terms, bad_utf8, 2u, sink, NULL) ==
                    YAN_SEARCH_TERMS_INVALID, 19u);
    guest_check(yan_search_terms(&terms, (const uint8_t *)groups17,
                                 (uint32_t)strlen(groups17), sink, NULL) ==
                    YAN_SEARCH_TERMS_INVALID, 20u);
    guest_check(yan_search_terms(&terms, (const uint8_t *)separators_only,
                                 (uint32_t)strlen(separators_only), sink,
                                 NULL) == YAN_SEARCH_TERMS_INVALID, 21u);
    guest_check(yan_search_terms(&terms, word256, sizeof word256, sink, NULL) ==
                    YAN_SEARCH_TERMS_INVALID, 22u);
    guest_check(yan_search_terms(&terms, (const uint8_t *)&terms, 4u, sink,
                                 NULL) == YAN_SEARCH_TERMS_INVALID, 23u);
    guest_check(yan_search_terms(&terms, (const uint8_t *)&fake, 4u, sink,
                                 NULL) == YAN_SEARCH_TERMS_INVALID, 24u);
    guest_check(yan_search_terms(&terms, source_region, 4u, sink, NULL) ==
                    YAN_SEARCH_TERMS_INVALID, 25u);
    sink.match = NULL;
    guest_check(yan_search_terms(&terms, body, 6u, sink, NULL) ==
                    YAN_SEARCH_TERMS_INVALID, 26u);
    sink.match = read_once;
    guest_check(yan_search_terms(
                    &terms, body, 6u, sink,
                    (YanSearchTermsSummary *)(void *)&terms) ==
                    YAN_SEARCH_TERMS_INVALID, 27u);
    guest_check(yan_search_terms(
                    &terms, body, 6u, sink,
                    (YanSearchTermsSummary *)(void *)&fake) ==
                    YAN_SEARCH_TERMS_INVALID, 28u);
    guest_check(yan_search_terms(
                    &terms, body, 6u, sink,
                    (YanSearchTermsSummary *)(void *)body) ==
                    YAN_SEARCH_TERMS_INVALID, 29u);
    guest_check(fake.queries == 0u, 30u);

    /* A repeated canonical group is one group and reaches the backend once. */
    guest_check(yan_search_terms(&terms, duplicates, duplicate_length, sink,
                                 NULL) == YAN_SEARCH_TERMS_STOPPED, 31u);
    guest_check(fake.queries == 1u, 32u);

    /* the snippet reader is legal only inside a match callback. */
    {
        const uint8_t *bytes = NULL;
        uint32_t length = 0u;
        guest_check(yan_search_terms_read_snippet(&terms, 0u, 1u, &bytes,
                                                  &length) ==
                        YAN_SEARCH_TERMS_INVALID, 33u);
    }

    /* a valid keep query publishes the summary and the one-byte snippet. */
    {
        YanSearchTermsSummary summary;
        summary.total = 0u;
        summary.shown = 0u;
        summary.skipped = 0u;
        summary.mode = YAN_SEARCH_MODE_SCAN;
        sink.match = read_keep;
        guest_check(yan_search_terms(&terms, body, 6u, sink, &summary) ==
                        YAN_SEARCH_TERMS_OK, 34u);
        guest_check(probe_result == YAN_SEARCH_TERMS_OK && probe_length == 1u,
                    35u);
        guest_check(summary.total == 1u && summary.shown == 1u &&
                        summary.skipped == 0u &&
                        summary.mode == YAN_SEARCH_MODE_INDEX, 36u);
    }

    /* holder aliases and empty reads. */
    sink.match = holder_alias;
    guest_check(yan_search_terms(&terms, body, 6u, sink, NULL) ==
                    YAN_SEARCH_TERMS_STOPPED, 37u);
    guest_check(probe_result == YAN_SEARCH_TERMS_INVALID, 38u);
    sink.match = dual_alias;
    guest_check(yan_search_terms(&terms, body, 6u, sink, NULL) ==
                    YAN_SEARCH_TERMS_STOPPED, 39u);
    guest_check(probe_result == YAN_SEARCH_TERMS_INVALID, 40u);
    sink.match = empty_reads;
    guest_check(yan_search_terms(&terms, body, 6u, sink, NULL) ==
                    YAN_SEARCH_TERMS_STOPPED, 41u);
    guest_check(probe_result == YAN_SEARCH_TERMS_OK, 42u);

    /* a sticky reader IO error wins over a callback that ignores it. */
    fake.read_result = YAN_SEARCH_TERMS_IO;
    sink.match = ignore_reader;
    guest_check(yan_search_terms(&terms, body, 6u, sink, NULL) ==
                    YAN_SEARCH_TERMS_IO, 43u);
    fake.read_result = YAN_SEARCH_TERMS_OK;

    /* zero progress before the end is a protocol error, not a stop. */
    fake.zero_progress = true;
    sink.match = ignore_reader;
    guest_check(yan_search_terms(&terms, body, 6u, sink, NULL) ==
                    YAN_SEARCH_TERMS_PROTOCOL, 44u);
    fake.zero_progress = false;

    /* a failed query leaves the caller's summary holder untouched. */
    {
        YanSearchTermsSummary summary;
        uint8_t before[sizeof summary];
        summary.total = 0xA5A5A5A5u;
        summary.shown = 0x5A5A5A5Au;
        summary.skipped = 0x12345678u;
        summary.mode = YAN_SEARCH_MODE_SCAN;
        memcpy(before, &summary, sizeof summary);
        fake.read_result = YAN_SEARCH_TERMS_IO;
        sink.match = ignore_reader;
        guest_check(yan_search_terms(&terms, body, 6u, sink, &summary) ==
                        YAN_SEARCH_TERMS_IO, 45u);
        guest_check(memcmp(before, &summary, sizeof summary) == 0, 46u);
        fake.read_result = YAN_SEARCH_TERMS_OK;
    }

    /* an optional NULL summary is legal and still delivers the match. */
    sink.match = read_keep;
    probe_result = YAN_SEARCH_TERMS_INVALID;
    guest_check(yan_search_terms(&terms, body, 6u, sink, NULL) ==
                    YAN_SEARCH_TERMS_OK, 47u);
    guest_check(probe_result == YAN_SEARCH_TERMS_OK, 48u);

    /* whole-query reentry from inside a match callback is BUSY. */
    reentry_terms = &terms;
    reentry_result = YAN_SEARCH_TERMS_OK;
    sink.match = reentry_match;
    guest_check(yan_search_terms(&terms, body, 6u, sink, NULL) ==
                    YAN_SEARCH_TERMS_STOPPED, 49u);
    guest_check(reentry_result == YAN_SEARCH_TERMS_BUSY, 50u);

    /* management reentry is BUSY and a BUSY status leaves the holder alone. */
    reentry_terms = &terms;
    reentry_kind = 1u;
    holder_unchanged = false;
    reentry_result = YAN_SEARCH_TERMS_OK;
    guest_check(yan_search_terms_status(&terms, &status) ==
                    YAN_SEARCH_TERMS_OK, 51u);
    guest_check(reentry_result == YAN_SEARCH_TERMS_BUSY, 52u);
    guest_check(holder_unchanged, 53u);

    reentry_kind = 2u;
    reentry_result = YAN_SEARCH_TERMS_OK;
    guest_check(yan_search_terms_status(&terms, &status) ==
                    YAN_SEARCH_TERMS_OK, 54u);
    guest_check(reentry_result == YAN_SEARCH_TERMS_BUSY, 55u);

    reentry_kind = 3u;
    reentry_result = YAN_SEARCH_TERMS_OK;
    guest_check(yan_search_terms_status(&terms, &status) ==
                    YAN_SEARCH_TERMS_OK, 56u);
    guest_check(reentry_result == YAN_SEARCH_TERMS_BUSY, 57u);

    reentry_kind = 4u;
    reentry_result = YAN_SEARCH_TERMS_OK;
    guest_check(yan_search_terms_status(&terms, &status) ==
                    YAN_SEARCH_TERMS_OK, 58u);
    guest_check(reentry_result == YAN_SEARCH_TERMS_BUSY, 59u);
    reentry_kind = 0u;

    /* near UINTPTR_MAX: a summary holder and a snippet output holder whose
     * ranges leave the address space are rejected by pure arithmetic, before
     * any write or backend read. */
    sink.match = read_keep;
    guest_check(yan_search_terms(
                    &terms, body, 6u, sink,
                    (YanSearchTermsSummary *)(uintptr_t)0xFFFFFFFEu) ==
                    YAN_SEARCH_TERMS_INVALID, 60u);
    sink.match = holder_wrap;
    guest_check(yan_search_terms(&terms, body, 6u, sink, NULL) ==
                    YAN_SEARCH_TERMS_STOPPED, 61u);
    guest_check(probe_result == YAN_SEARCH_TERMS_INVALID, 62u);

    marker("SEARCH_TERMS_API_PASS");
    guest_finish(1u);
    return 0;
}
