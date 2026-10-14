#define _POSIX_C_SOURCE 200809L
#include "semantic_llama.h"
#include "semantic_posix.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t cancelled;
static void cancel(int signal_number) { (void)signal_number; cancelled = 1; }
typedef struct {
    const char *command, *image, *index, *model, *runner, *query;
    unsigned startup, request;
} Options;
static bool seconds(const char *text, unsigned *out)
{
    unsigned value = 0;
    if (!*text) return false;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p < '0' || *p > '9' || value > 3600 / 10) return false;
        value = value * 10 + (*p - '0'); if (value > 3600) return false;
    }
    if (!value) return false;
    *out = value; return true;
}
static bool parse(int argc, char **argv, Options *o)
{
    if (argc < 2) return false;
    memset(o, 0, sizeof *o); o->startup = o->request = 120; o->command = argv[1];
    if (strcmp(o->command, "rebuild") && strcmp(o->command, "search") &&
        strcmp(o->command, "status")) return false;
    unsigned seen = 0;
    for (int i = 2; i < argc; i += 2) {
        if (i + 1 >= argc) return false;
        unsigned bit;
        const char **destination = NULL;
        if (!strcmp(argv[i], "--disk-image")) { bit = 1; destination = &o->image; }
        else if (!strcmp(argv[i], "--index")) { bit = 2; destination = &o->index; }
        else if (!strcmp(argv[i], "--model")) { bit = 4; destination = &o->model; }
        else if (!strcmp(argv[i], "--runner")) { bit = 8; destination = &o->runner; }
        else if (!strcmp(argv[i], "--query")) { bit = 16; destination = &o->query; }
        else if (!strcmp(argv[i], "--startup-timeout")) bit = 32;
        else if (!strcmp(argv[i], "--request-timeout")) bit = 64;
        else return false;
        if (seen & bit) return false;
        seen |= bit;
        if (destination) { *destination = argv[i+1]; if (!**destination) return false; }
        else if (!seconds(argv[i+1], bit == 32 ? &o->startup : &o->request)) return false;
    }
    if ((seen & 15) != 15) return false;
    if (!strcmp(o->command, "search"))
        return o->query && yan_semantic_query_valid((const uint8_t *)o->query, strlen(o->query));
    return o->query == NULL;
}
static int error(YanSemanticResult r)
{
    fprintf(stderr, "ERROR %s\n", r == YAN_SEMANTIC_STOPPED ? "OUTPUT" : yan_semantic_result_name(r));
    switch (r) {
    case YAN_SEMANTIC_INVALID: case YAN_SEMANTIC_BUSY: return 2;
    case YAN_SEMANTIC_MISSING: case YAN_SEMANTIC_STALE: case YAN_SEMANTIC_CORRUPT:
    case YAN_SEMANTIC_METADATA: case YAN_SEMANTIC_LIMIT: return 3;
    case YAN_SEMANTIC_DEPENDENCY: case YAN_SEMANTIC_PROVIDER: case YAN_SEMANTIC_TIMEOUT:
    case YAN_SEMANTIC_UNSUPPORTED: return 4;
    default: return 5;
    }
}
static bool escaped_excerpt(const uint8_t *bytes, uint32_t length)
{
    uint32_t count = 0, i = 0;
    while (i < length && count++ < 160) {
        uint8_t b = bytes[i];
        uint32_t width = b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
        if (width > length - i) return false; /* defensive, core prevalidated */
        if (b == '\\') { if (fputs("\\\\", stdout) == EOF) return false; }
        else if (b < 32 || b == 127) {
            if (fprintf(stdout, "\\x%02X", (unsigned)b) < 0) return false;
        } else if (fwrite(bytes + i, 1, width, stdout) != width) return false;
        i += width;
    }
    return true;
}
static bool row(void *opaque, const YanSemanticRow *r)
{
    (void)opaque;
    if (cancelled) return false;
    if (printf("MATCH %s lines=%u-%u bytes=%u-%u score=%.6f ", r->provenance.name,
        r->provenance.first_line, r->provenance.last_line, r->provenance.start,
        r->provenance.end, r->score) < 0 || !escaped_excerpt(r->raw, r->raw_length) ||
        putchar('\n') == EOF || fflush(stdout) != 0) return false;
    return true;
}
int main(int argc, char **argv)
{
    Options options;
    if (!parse(argc, argv, &options)) { fputs("ERROR USAGE\n", stderr); return 2; }
    struct sigaction action;
    memset(&action, 0, sizeof action); action.sa_handler = cancel; sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL)) return error(YAN_SEMANTIC_IO);
    action.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &action, NULL)) return error(YAN_SEMANTIC_IO);
    YanSemanticPosix *adapter = calloc(1, sizeof *adapter);
    if (!adapter) return error(YAN_SEMANTIC_IO);
    adapter->image_fd = adapter->index_fd = -1;
    YanSemanticIdentity identity = {{0},{0},{0}};
    YanSemanticResult result = yan_semantic_posix_open(adapter, options.image,
        options.index, options.model, options.runner, &identity);
    YanSemanticLlama llama = {0}; llama.owned_pid = -1;
    if (result == YAN_SEMANTIC_OK) result = yan_semantic_llama_init(&llama,
        options.model, options.runner, options.startup, options.request, &cancelled);
    if (result == YAN_SEMANTIC_OK) result = yan_semantic_llama_config(&llama, identity.config);
    YanSemanticSource source; YanSemanticStore store; YanSemanticProvider provider;
    YanSemantic context = {0}; YanSemanticSummary summary;
    yan_semantic_posix_bind(adapter, &source, &store);
    yan_semantic_llama_bind(&llama, &provider);
    if (result == YAN_SEMANTIC_OK) result = yan_semantic_init(&context, &source,
        &provider, &store, &identity, yan_semantic_posix_close_source);
    if (result == YAN_SEMANTIC_OK && cancelled) result = YAN_SEMANTIC_CANCELLED;
    if (result == YAN_SEMANTIC_OK) {
        if (!strcmp(options.command, "rebuild")) result = yan_semantic_rebuild(&context, &summary);
        else if (!strcmp(options.command, "status")) result = yan_semantic_status(&context, &summary);
        else result = yan_semantic_search(&context, (const uint8_t *)options.query,
            strlen(options.query), row, NULL, &summary);
    }
    YanSemanticResult closed = yan_semantic_posix_cleanup(adapter);
    if (result == YAN_SEMANTIC_OK) result = closed;
    YanSemanticResult stopped = yan_semantic_llama_stop(&llama);
    if (result == YAN_SEMANTIC_OK) result = stopped;
    free(adapter);
    if (cancelled && (result == YAN_SEMANTIC_OK || result == YAN_SEMANTIC_STOPPED))
        result = YAN_SEMANTIC_CANCELLED;
    if (result != YAN_SEMANTIC_OK) return error(result);
    int printed;
    if (!strcmp(options.command, "rebuild"))
        printed = printf("OK rebuild segments=%u skipped=%u\n", summary.segments, summary.skipped);
    else if (!strcmp(options.command, "search"))
        printed = printf("OK search shown=%u segments=%u skipped=%u\n", summary.shown, summary.segments, summary.skipped);
    else if (summary.state == YAN_SEMANTIC_INDEX_READY)
        printed = printf("STATUS READY segments=%u skipped=%u\n", summary.segments, summary.skipped);
    else {
        const char *name = summary.state == YAN_SEMANTIC_INDEX_MISSING ? "MISSING" :
            summary.state == YAN_SEMANTIC_INDEX_STALE ? "STALE" : "CORRUPT";
        printed = printf("STATUS %s\n", name);
    }
    if (printed < 0 || fflush(stdout) != 0) return error(YAN_SEMANTIC_STOPPED);
    return 0;
}
