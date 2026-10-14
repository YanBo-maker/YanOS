#include "semantic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

typedef struct {
    uint32_t count;
    float vectors[64][1024];
    uint8_t index[256 + 63 * 4160];
    size_t index_size;
    bool present;
} Fixture;
static Fixture fixture;
static YanSemanticResult next(void *p, uint32_t *cursor, YanSemanticFile *out)
{
    Fixture *f = p;
    if (*cursor >= f->count) return YAN_SEMANTIC_MISSING;
    memset(out, 0, sizeof *out);
    uint32_t i = f->count - 1 - *cursor;
    snprintf(out->name, sizeof out->name, "doc%02u", i);
    out->slot = (*cursor)++; out->size = 5; return YAN_SEMANTIC_OK;
}
static YanSemanticResult read_source(void *p, const char *name, uint32_t off, uint8_t *out, uint32_t n)
{
    (void)p;
    if (off > 5 || n > 5 - off) return YAN_SEMANTIC_IO;
    memcpy(out, name + off, n); return YAN_SEMANTIC_OK;
}
static YanSemanticResult okay(void *p) { (void)p; return YAN_SEMANTIC_OK; }
static YanSemanticResult embed(void *p, const uint8_t *input, size_t n, float *out)
{
    Fixture *f = p; uint32_t i = f->count;
    if (n == 5 && !memcmp(input, "doc", 3))
        i = (uint32_t)(input[3] - '0') * 10 + (uint32_t)(input[4] - '0');
    if (i > f->count) return YAN_SEMANTIC_PROVIDER;
    memcpy(out, f->vectors[i], sizeof f->vectors[0]); return YAN_SEMANTIC_OK;
}
static YanSemanticResult open_read(void *p, uint64_t *n)
{
    Fixture *f = p; if (!f->present) return YAN_SEMANTIC_MISSING;
    *n = f->index_size; return YAN_SEMANTIC_OK;
}
static YanSemanticResult read_index(void *p, uint64_t off, uint8_t *out, size_t n)
{
    Fixture *f = p;
    if (off > f->index_size || n > f->index_size - off) return YAN_SEMANTIC_IO;
    memcpy(out, f->index + off, n); return YAN_SEMANTIC_OK;
}
static YanSemanticResult begin(void *p)
{ ((Fixture *)p)->index_size = 0; return YAN_SEMANTIC_OK; }
static YanSemanticResult write_index(void *p, const uint8_t *in, size_t n)
{
    Fixture *f = p;
    if (n > sizeof f->index - f->index_size) return YAN_SEMANTIC_IO;
    memcpy(f->index + f->index_size, in, n); f->index_size += n; return YAN_SEMANTIC_OK;
}
static YanSemanticResult publish(void *p)
{ ((Fixture *)p)->present = true; return YAN_SEMANTIC_OK; }
static void discard(void *p) { (void)p; }
static bool row(void *p, const YanSemanticRow *r)
{
    (void)p;
    printf("ROW %s %.17g %u %u %u %u\n", r->provenance.name, r->score,
           r->provenance.start, r->provenance.end, r->provenance.first_line, r->provenance.last_line);
    return !ferror(stdout);
}
static bool u32(uint32_t *value)
{
    uint8_t b[4]; if (fread(b, 1, 4, stdin) != 4) return false;
    *value = 0; for (unsigned i = 0; i < 4; ++i) *value |= (uint32_t)b[i] << (8 * i);
    return true;
}
int main(void)
{
#ifdef _WIN32
    if (_setmode(_fileno(stdin), _O_BINARY) == -1) return 1;
#endif
    switch (sizeof(float)) { case 4: break; default: return 1; }
    if (!u32(&fixture.count) || fixture.count > 63) return 1;
    for (uint32_t i = 0; i <= fixture.count; ++i)
        for (unsigned j = 0; j < 1024; ++j) {
            uint32_t bits; if (!u32(&bits)) return 1;
            memcpy(&fixture.vectors[i][j], &bits, 4);
        }
    if (fgetc(stdin) != EOF) return 1;
    YanSemanticSource source = {&fixture, sizeof fixture, next, read_source};
    YanSemanticProvider provider = {&fixture, sizeof fixture, okay, embed, okay};
    YanSemanticStore store = {&fixture, sizeof fixture, open_read, read_index, okay,
        begin, write_index, okay, publish, discard};
    YanSemanticIdentity identity = {{0},{0},{0}};
    YanSemantic context = {0}; YanSemanticSummary summary;
    YanSemanticResult r = yan_semantic_init(&context, &source, &provider, &store, &identity, okay);
    if (r == YAN_SEMANTIC_OK) r = yan_semantic_rebuild(&context, &summary);
    if (r == YAN_SEMANTIC_OK)
        r = yan_semantic_search(&context, (const uint8_t *)"query", 5, row, NULL, &summary);
    printf("RESULT %s\n", yan_semantic_result_name(r));
    return ferror(stdout) ? 1 : 0;
}
