/* R4-AC2's Guest output omits scores. Inspect the actual public search seam
 * on the same image so the end-to-end oracle can compare scores as well. */
#include "semantic_posix.h"
#include "search_terms_index.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { YanSearchTerms *terms; bool ok; } Capture;
static bool emit(void *opaque, const YanSearchTermsMatch *match)
{
    Capture *capture = opaque;
    printf("MATCH %s %" PRIu32 " %u %u %u ", match->name,
           match->line_number, (unsigned)match->score,
           (unsigned)match->left_truncated, (unsigned)match->right_truncated);
    uint32_t offset = 0;
    while (offset < match->snippet_length) {
        const uint8_t *bytes = NULL; uint32_t length = 0;
        if (yan_search_terms_read_snippet(capture->terms, offset,
                match->snippet_length - offset, &bytes, &length) != YAN_SEARCH_TERMS_OK || !length) {
            capture->ok = false; return false;
        }
        for (uint32_t i = 0; i < length; ++i) printf("%02x", (unsigned)bytes[i]);
        offset += length;
    }
    puts("");
    return true;
}
int main(int argc, char **argv)
{
    if (argc != 4 || (strcmp(argv[2], "limit") && strcmp(argv[2], "scan"))) return 2;
    int status = 1;
    YanSemanticPosix *adapter = calloc(1, sizeof *adapter);
    YanSearchTermsIndex *index = calloc(1, sizeof *index);
    YanFsInfo *entries = NULL; uint8_t *eligible = NULL;
    if (!adapter || !index) { free(adapter); free(index); return 2; }
    adapter->image_fd = adapter->index_fd = -1;
    if (yan_semantic_posix_open_image(adapter, argv[1]) != YAN_SEMANTIC_OK) goto done;
    entries = calloc(adapter->fs.entry_capacity, sizeof *entries);
    eligible = calloc(adapter->fs.entry_capacity, 1);
    if (!entries || !eligible || yan_search_terms_index_init(index, &adapter->fs,
            entries, eligible) != YAN_SEARCH_TERMS_OK) goto done;
    YanSearchTerms terms = {0};
    if (yan_search_terms_init(&terms, yan_search_terms_index_backend(index)) != YAN_SEARCH_TERMS_OK ||
        yan_search_terms_rebuild(&terms) != YAN_SEARCH_TERMS_INDEX_LIMIT) goto done;
    if (!strcmp(argv[2], "scan") && yan_search_terms_clear(&terms) != YAN_SEARCH_TERMS_OK) goto done;
    Capture capture = {&terms, true};
    YanSearchTermsSink sink = {&capture, emit, NULL};
    YanSearchTermsSummary summary = {0};
    if (yan_search_terms(&terms, (const uint8_t *)argv[3], (uint32_t)strlen(argv[3]),
            sink, &summary) != YAN_SEARCH_TERMS_OK || !capture.ok || summary.mode != YAN_SEARCH_MODE_SCAN) goto done;
    printf("OK total=%" PRIu64 " shown=%" PRIu32 " skipped=%" PRIu32 "\n",
           summary.total, summary.shown, summary.skipped);
    status = ferror(stdout) ? 1 : 0;
done:
    if (yan_semantic_posix_cleanup(adapter) != YAN_SEMANTIC_OK) status = 1;
    free(eligible); free(entries); free(index); free(adapter);
    return status;
}
