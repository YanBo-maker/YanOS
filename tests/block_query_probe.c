#include "block_query.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Native probe for the independent Python oracle. It prints only stable,
 * line-oriented fields; multi-byte page text is hex so CRLF/NUL handling cannot
 * be confused with terminal encoding. Exit status is 0 whenever a result or an
 * ERROR token was printed; 2 is reserved for probe usage errors. */
static YanBlockQuery query;
static YanBlockQueryList list_result;
static YanBlockQueryRefs refs_result;
static YanBlockQueryBacklinks backlinks_result;
static YanBlockQueryRead read_result;

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static bool parse_expected(const char *text, uint8_t out[32])
{
    if (text == NULL || strlen(text) != 64) return false;
    for (unsigned i = 0; i < 32; ++i) {
        int high = hex_value(text[2 * i]);
        int low = hex_value(text[2 * i + 1]);
        if (high < 0 || low < 0) return false;
        out[i] = (uint8_t)((high << 4) | low);
    }
    return true;
}
static void print_list(void)
{
    printf("LIST %llu %u\n", (unsigned long long)list_result.total,
           (unsigned)list_result.shown);
    for (uint32_t i = 0; i < list_result.shown; ++i) {
        const YanBlockDecl *block = &list_result.blocks[i];
        printf("BLOCK %s %u %u %u %u %u\n", block->id, block->marker_line,
               block->first_line, block->last_line, block->start, block->end);
    }
}
static void print_read(void)
{
    printf("READ %u %u %u %u %u %u %u %u %u %u %u\n",
           read_result.block_start, read_result.block_end,
           read_result.block_first_line, read_result.block_last_line,
           read_result.page_start, read_result.page_end,
           read_result.page_first_line, read_result.page_last_line,
           read_result.complete ? 1u : 0u, read_result.next_offset,
           read_result.text_length);
    printf("TEXT ");
    if (read_result.text_length == 0) printf("-");
    for (uint32_t i = 0; i < read_result.text_length; ++i)
        printf("%02x", read_result.text[i]);
    printf("\n");
}
static void print_refs(void)
{
    printf("REFS %llu %u\n", (unsigned long long)refs_result.total,
           (unsigned)refs_result.shown);
    for (uint32_t i = 0; i < refs_result.shown; ++i) {
        const YanBlockRow *row = &refs_result.rows[i];
        const YanBlockReference *reference = &row->reference;
        printf("REF %s %s %u %u %u %s %s %s\n", row->source,
               reference->has_source_id ? reference->source_id : "NONE",
               reference->line, reference->start, reference->end,
               reference->target.name, reference->target.id,
               yan_block_target_name(row->state));
    }
}
static void print_backlinks(void)
{
    printf("BACKLINKS %llu %u %u %s\n",
           (unsigned long long)backlinks_result.total,
           (unsigned)backlinks_result.shown, (unsigned)backlinks_result.skipped,
           yan_block_target_name(backlinks_result.target_state));
    for (uint32_t i = 0; i < backlinks_result.shown; ++i) {
        const YanBlockRow *row = &backlinks_result.rows[i];
        const YanBlockReference *reference = &row->reference;
        printf("BACKLINK %s %s %u %u %u\n", row->source,
               reference->has_source_id ? reference->source_id : "NONE",
               reference->line, reference->start, reference->end);
    }
}
int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s IMAGE list|refs NAME [EXPECTED]\n"
                        "       %s IMAGE read NAME ID OFFSET [EXPECTED]\n"
                        "       %s IMAGE backlinks NAME ID [EXPECTED]\n",
                argv[0], argv[0], argv[0]);
        return 2;
    }
    const char *operation = argv[2];
    const char *name = argv[3];
    const char *id = NULL, *expected_text = NULL;
    uint32_t offset = 0;
    if (strcmp(operation, "read") == 0) {
        if (argc < 6) return 2;
        id = argv[4];
        char *end = NULL;
        unsigned long value = strtoul(argv[5], &end, 10);
        if (end == argv[5] || *end != '\0' || value > UINT32_MAX) return 2;
        offset = (uint32_t)value;
        expected_text = argc > 6 ? argv[6] : NULL;
    } else if (strcmp(operation, "backlinks") == 0) {
        if (argc < 5) return 2;
        id = argv[4];
        expected_text = argc > 5 ? argv[5] : NULL;
    } else if (strcmp(operation, "list") == 0 || strcmp(operation, "refs") == 0) {
        expected_text = argc > 4 ? argv[4] : NULL;
    } else {
        return 2;
    }
    uint8_t expected[32];
    bool has_expected = expected_text != NULL;
    if (has_expected && !parse_expected(expected_text, expected)) return 2;

    YanSemanticPosix adapter;
    memset(&adapter, 0, sizeof adapter);
    adapter.image_fd = adapter.index_fd = -1;
    YanSemanticResult opened = yan_semantic_posix_open_image(&adapter, argv[1]);
    if (opened != YAN_SEMANTIC_OK) {
        printf("ERROR %s\n", opened == YAN_SEMANTIC_INVALID ? "INVALID" :
               opened == YAN_SEMANTIC_CORRUPT ? "CORRUPT" : "IO");
        yan_semantic_posix_cleanup(&adapter);
        return 0;
    }
    memset(&query, 0, sizeof query);
    YanBlockQueryResult result = yan_block_query_init(&query, &adapter, NULL);
    if (result != YAN_BLOCK_QUERY_OK) {
        printf("ERROR %s\n", yan_block_query_result_name(result));
        yan_semantic_posix_cleanup(&adapter);
        return 0;
    }
    uint8_t source_digest[32] = {0};
    if (strcmp(operation, "list") == 0) {
        result = yan_block_query_list(&query, name,
            has_expected ? expected : NULL, &list_result);
    } else if (strcmp(operation, "refs") == 0) {
        result = yan_block_query_refs(&query, name,
            has_expected ? expected : NULL, &refs_result);
    } else if (strcmp(operation, "read") == 0) {
        result = yan_block_query_read(&query, name, id, offset,
            has_expected ? expected : NULL, &read_result);
    } else {
        result = yan_block_query_backlinks(&query, name, id,
            has_expected ? expected : NULL, &backlinks_result);
    }
    if (result == YAN_BLOCK_QUERY_OK)
        memcpy(source_digest, query.source_sha256, sizeof source_digest);
    YanSemanticResult closed = yan_semantic_posix_cleanup(&adapter);
    if (result == YAN_BLOCK_QUERY_OK && closed != YAN_SEMANTIC_OK) {
        printf("ERROR IO\n");
        return 0;
    }
    if (result != YAN_BLOCK_QUERY_OK) {
        printf("ERROR %s\n", yan_block_query_result_name(result));
        return 0;
    }
    if (strcmp(operation, "list") == 0) print_list();
    else if (strcmp(operation, "refs") == 0) print_refs();
    else if (strcmp(operation, "read") == 0) print_read();
    else print_backlinks();
    printf("SOURCE ");
    for (unsigned i = 0; i < sizeof source_digest; ++i)
        printf("%02x", source_digest[i]);
    printf("\n");
    return 0;
}
