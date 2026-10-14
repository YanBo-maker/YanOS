/* Binary stdin avoids shell quoting and lets the independent Python oracle
 * send embedded NUL and arbitrary UTF-8 fragments. This is a test adapter. */
#include "semantic_text.h"
#include "semantic_sha256.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

static uint8_t bytes[65536];
static YanSemanticChunker chunker;
static bool emit(void *opaque, const YanSemanticChunk *chunk)
{
    (void)opaque;
    YanSemanticSha256 h = {0}; uint8_t hash[32];
    if (yan_semantic_sha256_init(&h) != YAN_SEMANTIC_OK ||
        yan_semantic_sha256_update(&h, chunk->body, chunk->body_length) != YAN_SEMANTIC_OK ||
        yan_semantic_sha256_final(&h, hash) != YAN_SEMANTIC_OK) return false;
    printf("ROW %u %u %u %u %u ", chunk->start, chunk->end, chunk->first_line,
           chunk->last_line, chunk->scalar_count);
    for (unsigned i = 0; i < 32; ++i) printf("%02x", hash[i]);
    putchar('\n'); return true;
}
static bool u32(uint32_t *value)
{
    uint8_t b[4]; if (fread(b, 1, 4, stdin) != 4) return false;
    *value = 0; for (unsigned i = 0; i < 4; ++i) *value |= (uint32_t)b[i] << (8 * i);
    return true;
}
int main(int argc, char **argv)
{
#ifdef _WIN32
    if (_setmode(_fileno(stdin), _O_BINARY) == -1) return 1;
#endif
    uint32_t n, feed, offset, line;
    if (argc != 2 || !u32(&n) || !u32(&feed) || !u32(&offset) || !u32(&line) ||
        n > sizeof bytes || feed == 0 || fread(bytes, 1, n, stdin) != n ||
        fgetc(stdin) != EOF) return 1;
    YanSemanticResult r = YAN_SEMANTIC_OK;
    if (strcmp(argv[1], "chunk") == 0) {
        YanSemanticChunkSink sink = {NULL, emit};
        r = yan_semantic_chunker_init(&chunker, offset, line);
        for (uint32_t i = 0; r == YAN_SEMANTIC_OK && i < n;) {
            uint32_t amount = n - i; if (amount > feed) amount = feed;
            r = yan_semantic_chunker_feed(&chunker, bytes + i, amount, &sink); i += amount;
        }
        if (r == YAN_SEMANTIC_OK) r = yan_semantic_chunker_finish(&chunker, &sink);
    } else if (strcmp(argv[1], "sha") == 0) {
        YanSemanticSha256 hash = {0}; uint8_t digest[32];
        r = yan_semantic_sha256_init(&hash);
        for (uint32_t i = 0; r == YAN_SEMANTIC_OK && i < n;) {
            uint32_t amount = n - i; if (amount > feed) amount = feed;
            r = yan_semantic_sha256_update(&hash, bytes + i, amount); i += amount;
        }
        if (r == YAN_SEMANTIC_OK) r = yan_semantic_sha256_final(&hash, digest);
        if (r == YAN_SEMANTIC_OK) {
            printf("HASH "); for (unsigned i = 0; i < 32; ++i) printf("%02x", digest[i]); putchar('\n');
        }
    } else if (strcmp(argv[1], "text") == 0) {
        YanSemanticText text = {0}; r = yan_semantic_text_init(&text);
        for (uint32_t i = 0; r == YAN_SEMANTIC_OK && i < n;) {
            uint32_t amount = n - i; if (amount > feed) amount = feed;
            r = yan_semantic_text_update(&text, bytes + i, amount); i += amount;
        }
        if (r == YAN_SEMANTIC_OK) r = yan_semantic_text_finish(&text);
    } else if (strcmp(argv[1], "query") == 0) {
        r = yan_semantic_query_valid(bytes, n) ? YAN_SEMANTIC_OK : YAN_SEMANTIC_INVALID;
    } else return 1;
    printf("RESULT %d\n", (int)r); return ferror(stdout) ? 1 : 0;
}
