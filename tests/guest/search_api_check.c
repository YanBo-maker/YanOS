/* Freestanding RV32IM probe for the 0025 Search facade over a fake backend.
 *
 * This links only os/search.c, the shared Guest start/memory and this file: no
 * filesystem is needed, because the backend is a fake. It exercises the public
 * facade contract that is not tied to any directory: span validation, pattern
 * validation and aliasing, the match reader's period and holder guards, the
 * busy/stop lifecycle, sticky reader errors, zero-progress detection and a
 * near-UINT32_MAX relative offset. Every check has its own tohost code, and a
 * successful run prints the unique signature SEARCH_API_PASS before finishing
 * with tohost 1, so a compiled-out case cannot look like a pass.
 *
 * Sparse boundaries are probed honestly: the large content length is a value
 * and a static one-byte pointer, not a 4 GiB buffer, and no 4 GiB scan runs.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "guest.h"
#include "platform.h"
#include "search.h"

typedef struct {
    const uint8_t *body;
    uint32_t length;
    uint32_t queries;
    uint32_t reads;
    YanSearchResult read_result;
    bool zero_progress;
} Fake;

static YanSearch *probe_search;
static YanSearchResult probe_result;
static uint32_t probe_length;
static YanSearchResult reentry_result;
static YanSearch *reentry_search;

static YanSearchResult fake_query(void *context, const uint8_t *pattern,
                                  uint32_t pattern_length,
                                  YanSearchMatchFn match, void *match_context)
{
    Fake *self = (Fake *)context;
    (void)pattern;
    (void)pattern_length;
    ++self->queries;
    YanSearchMatch value;
    value.name = "f";
    value.line_number = 1u;
    value.content_length = self->length;
    (void)match(match_context, &value);
    return YAN_SEARCH_OK;
}

static YanSearchResult fake_read(void *context, uint32_t offset,
                                 uint32_t capacity, const uint8_t **bytes,
                                 uint32_t *length)
{
    Fake *self = (Fake *)context;
    ++self->reads;
    if (self->read_result != YAN_SEARCH_OK) {
        return self->read_result;
    }
    if (offset >= self->length) {
        *bytes = NULL;
        *length = 0u;
        return YAN_SEARCH_OK;
    }
    if (self->zero_progress) {
        *bytes = NULL;
        *length = 0u;
        return YAN_SEARCH_OK;
    }
    uint32_t want = 1u; /* one static byte, never a large buffer */
    if (capacity < want) {
        want = capacity;
    }
    *bytes = self->body;
    *length = want;
    return YAN_SEARCH_OK;
}

static YanSearchBackend make_backend(Fake *fake)
{
    YanSearchBackend backend;
    backend.context = fake;
    backend.context_size = sizeof *fake;
    backend.source_context = NULL;
    backend.source_size = 0u;
    backend.query = fake_query;
    backend.read_match = fake_read;
    return backend;
}

static YanSearchResult init_fake(YanSearch *search, Fake *fake)
{
    memset(search, 0, sizeof *search);
    return yan_search_init(search, make_backend(fake));
}

static bool read_once(void *context, const YanSearchMatch *match)
{
    (void)context;
    const uint8_t *bytes = NULL;
    uint32_t length = 0u;
    probe_result = yan_search_read_match(probe_search, 0u, match->content_length,
                                         &bytes, &length);
    probe_length = length;
    return false;
}

static bool holder_alias(void *context, const YanSearchMatch *match)
{
    (void)context;
    uint32_t length = 0u;
    probe_result = yan_search_read_match(
        probe_search, 0u, match->content_length,
        (const uint8_t **)(void *)probe_search, &length);
    return false;
}

static bool ignore_reader(void *context, const YanSearchMatch *match)
{
    (void)context;
    const uint8_t *bytes = NULL;
    uint32_t length = 0u;
    (void)yan_search_read_match(probe_search, 0u, match->content_length, &bytes,
                                &length);
    /* The caller ignores the reader result on purpose. */
    return true;
}

static bool huge_offset(void *context, const YanSearchMatch *match)
{
    (void)context;
    (void)match;
    const uint8_t *bytes = NULL;
    uint32_t length = 0u;
    probe_result = yan_search_read_match(probe_search, 0xFFFFFFFEu, 1u, &bytes,
                                         &length);
    probe_length = length;
    return false;
}

static bool empty_reads(void *context, const YanSearchMatch *match)
{
    (void)context;
    const uint8_t *bytes = NULL;
    uint32_t length = 0u;
    YanSearchResult zero =
        yan_search_read_match(probe_search, 0u, 0u, &bytes, &length);
    YanSearchResult end = yan_search_read_match(
        probe_search, match->content_length, 1u, &bytes, &length);
    probe_result = (zero == YAN_SEARCH_OK && end == YAN_SEARCH_OK &&
                    length == 0u)
                       ? YAN_SEARCH_OK
                       : YAN_SEARCH_PROTOCOL;
    return false;
}

static bool reentry_match(void *context, const YanSearchMatch *match)
{
    (void)context;
    (void)match;
    YanSearchSink inner;
    inner.context = NULL;
    inner.match = read_once;
    inner.chunk = NULL;
    reentry_result =
        yan_search_query(reentry_search, (const uint8_t *)"x", 1u, inner);
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
    static Fake fake;
    static YanSearch search;
    static uint8_t region[64];
    static const uint8_t body[6] = {'n', 'e', 'e', 'd', 'l', 'e'};
    static const uint8_t with_nul[3] = {'a', 0u, 'b'};
    static const uint8_t with_lf[3] = {'a', (uint8_t)'\n', 'b'};
    YanSearchSink sink;
    sink.context = NULL;
    sink.match = read_once;
    sink.chunk = NULL;

    memset(&fake, 0, sizeof fake);
    fake.body = body;
    fake.length = 6u;

    /* init: null, incomplete, malformed spans, aliases, then a valid one. */
    YanSearchBackend backend = make_backend(&fake);
    guest_check(yan_search_init(NULL, backend) == YAN_SEARCH_INVALID, 1u);
    backend.query = NULL;
    guest_check(yan_search_init(&search, backend) == YAN_SEARCH_INVALID, 2u);
    backend = make_backend(&fake);
    backend.read_match = NULL;
    guest_check(yan_search_init(&search, backend) == YAN_SEARCH_INVALID, 3u);
    backend = make_backend(&fake);
    backend.context = NULL;
    backend.context_size = 8u;
    guest_check(yan_search_init(&search, backend) == YAN_SEARCH_INVALID, 4u);
    backend = make_backend(&fake);
    backend.context = (void *)(uintptr_t)0xFFFFFFF0u;
    backend.context_size = 0x20u;
    guest_check(yan_search_init(&search, backend) == YAN_SEARCH_INVALID, 5u);
    backend = make_backend(&fake);
    backend.context = &search;
    backend.context_size = sizeof search;
    guest_check(yan_search_init(&search, backend) == YAN_SEARCH_INVALID, 6u);
    backend = make_backend(&fake);
    backend.source_context = &search;
    backend.source_size = sizeof search;
    guest_check(yan_search_init(&search, backend) == YAN_SEARCH_INVALID, 7u);
    backend = make_backend(&fake);
    backend.context = region;
    backend.context_size = 8u;
    backend.source_context = region + 4;
    backend.source_size = 8u;
    guest_check(yan_search_init(&search, backend) == YAN_SEARCH_INVALID, 8u);
    guest_check(init_fake(&search, &fake) == YAN_SEARCH_OK, 9u);
    probe_search = &search;

    /* query: pattern length, bytes, range wrap and aliases. */
    guest_check(yan_search_query(&search, NULL, 3u, sink) == YAN_SEARCH_INVALID,
                10u);
    guest_check(yan_search_query(&search, body, 0u, sink) == YAN_SEARCH_INVALID,
                11u);
    guest_check(yan_search_query(&search, (const uint8_t *)(uintptr_t)0xFFFFFFF0u,
                                 0x20u, sink) == YAN_SEARCH_INVALID, 12u);
    guest_check(yan_search_query(&search, with_nul, 3u, sink) ==
                    YAN_SEARCH_INVALID, 13u);
    guest_check(yan_search_query(&search, with_lf, 3u, sink) ==
                    YAN_SEARCH_INVALID, 14u);
    guest_check(yan_search_query(&search, (const uint8_t *)&search, 4u, sink) ==
                    YAN_SEARCH_INVALID, 15u);
    guest_check(yan_search_query(&search, (const uint8_t *)&fake, 4u, sink) ==
                    YAN_SEARCH_INVALID, 16u);
    sink.match = NULL;
    guest_check(yan_search_query(&search, body, 6u, sink) == YAN_SEARCH_INVALID,
                17u);
    sink.match = read_once;

    /* reader period, holders and empty reads. */
    {
        const uint8_t *bytes = NULL;
        uint32_t length = 0u;
        guest_check(yan_search_read_match(&search, 0u, 1u, &bytes, &length) ==
                        YAN_SEARCH_INVALID, 18u);
    }
    guest_check(yan_search_query(&search, body, 6u, sink) == YAN_SEARCH_STOPPED,
                19u);
    guest_check(probe_result == YAN_SEARCH_OK && probe_length == 1u, 20u);
    sink.match = holder_alias;
    guest_check(yan_search_query(&search, body, 6u, sink) == YAN_SEARCH_STOPPED,
                21u);
    guest_check(probe_result == YAN_SEARCH_INVALID, 22u);
    sink.match = empty_reads;
    guest_check(yan_search_query(&search, body, 6u, sink) == YAN_SEARCH_STOPPED,
                23u);
    guest_check(probe_result == YAN_SEARCH_OK, 24u);

    /* whole-query reentry from inside a match callback is BUSY. */
    reentry_search = &search;
    reentry_result = YAN_SEARCH_OK;
    sink.match = reentry_match;
    guest_check(yan_search_query(&search, body, 6u, sink) == YAN_SEARCH_STOPPED,
                25u);
    guest_check(reentry_result == YAN_SEARCH_BUSY, 26u);

    /* a sticky reader error wins over a callback that ignores it. */
    fake.read_result = YAN_SEARCH_IO;
    sink.match = ignore_reader;
    guest_check(yan_search_query(&search, body, 6u, sink) == YAN_SEARCH_IO, 27u);
    fake.read_result = YAN_SEARCH_OK;

    /* zero progress before the end is a protocol error, not a stop. */
    fake.zero_progress = true;
    sink.match = ignore_reader;
    guest_check(yan_search_query(&search, body, 6u, sink) ==
                    YAN_SEARCH_PROTOCOL, 28u);
    fake.zero_progress = false;

    /* near UINT32_MAX: the relative-offset subtraction stays bounded and the
     * reader hands back its one static byte; no large buffer or scan. */
    fake.length = 0xFFFFFFFFu;
    sink.match = huge_offset;
    guest_check(yan_search_query(&search, body, 6u, sink) == YAN_SEARCH_STOPPED,
                29u);
    guest_check(probe_result == YAN_SEARCH_OK && probe_length == 1u, 30u);
    fake.length = 6u;

    marker("SEARCH_API_PASS");
    guest_finish(1u);
    return 0;
}
