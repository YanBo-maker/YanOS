/* 0036-R7-AC1 independent term-index probe.
 *
 * Opens one image read-only through the public POSIX adapter, initializes the
 * public in-memory term index, rebuilds it in this process, and prints the
 * index state plus every match (name, line, score, truncation flags and the raw
 * snippet bytes in hex) for each query. The 2026-10-21 clarification requires a
 * fresh-process rebuild comparison, not a claim that the RAM index travelled
 * across the migration or the process boundary.
 *
 * usage: yanfs_migrate_terms_probe IMAGE QUERY [QUERY...]
 * exit: 0 output complete and index READY; 1 any call failed or output error;
 *       2 usage / allocation error.
 */
#define _POSIX_C_SOURCE 200809L

#include "semantic_posix.h"
#include "search_terms_index.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    YanSearchTerms *terms;
    bool ok;
} Capture;

static bool emit_match(void *opaque, const YanSearchTermsMatch *match)
{
    Capture *capture = opaque;
    if (printf("MATCH name=%s line=%" PRIu32 " score=%u left=%u right=%u snippet=",
               match->name, match->line_number, (unsigned)match->score,
               match->left_truncated ? 1u : 0u,
               match->right_truncated ? 1u : 0u) < 0) {
        capture->ok = false;
        return false;
    }
    uint32_t offset = 0u;
    while (offset < match->snippet_length) {
        const uint8_t *bytes = NULL;
        uint32_t length = 0u;
        YanSearchTermsResult read = yan_search_terms_read_snippet(
            capture->terms, offset, match->snippet_length - offset, &bytes,
            &length);
        if (read != YAN_SEARCH_TERMS_OK || bytes == NULL || length == 0u ||
            length > match->snippet_length - offset) {
            capture->ok = false;
            return false;
        }
        for (uint32_t i = 0u; i < length; ++i) {
            if (printf("%02x", (unsigned)bytes[i]) < 0) {
                capture->ok = false;
                return false;
            }
        }
        offset += length;
    }
    if (putchar('\n') == EOF) {
        capture->ok = false;
        return false;
    }
    return true;
}

static void release(YanSemanticPosix *adapter, YanSearchTermsIndex *index,
                    YanSearchTerms *terms, YanFsInfo *entries,
                    uint8_t *eligible)
{
    free(eligible);
    free(entries);
    free(index);
    free(adapter);
    (void)terms;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s IMAGE QUERY [QUERY...]\n", argv[0]);
        return 2;
    }
    int status = 1;
    YanSemanticPosix *adapter = calloc(1u, sizeof *adapter);
    YanSearchTermsIndex *index = calloc(1u, sizeof *index);
    YanSearchTerms terms = {0};
    YanFsInfo *entries = NULL;
    uint8_t *eligible = NULL;
    if (adapter == NULL || index == NULL) {
        fprintf(stderr, "PROBE allocation failed\n");
        release(adapter, index, &terms, entries, eligible);
        return 2;
    }
    adapter->image_fd = adapter->index_fd = -1;

    YanImageOpenResult opened =
        yan_semantic_posix_open_image_ex(adapter, argv[1]);
    if (opened != YAN_IMAGE_OPEN_OK) {
        fprintf(stderr, "PROBE open failed (%d)\n", (int)opened);
        goto done;
    }
    printf("IMAGE capacity=%" PRIu32 " directory_blocks=%" PRIu32
           " entry_capacity=%" PRIu32 "\n",
           adapter->fs.capacity_blocks, adapter->fs.directory_blocks,
           adapter->fs.entry_capacity);
    entries = calloc(adapter->fs.entry_capacity, sizeof *entries);
    eligible = calloc(adapter->fs.entry_capacity, sizeof *eligible);
    if (entries == NULL || eligible == NULL ||
        yan_search_terms_index_init(index, &adapter->fs, entries, eligible) !=
            YAN_SEARCH_TERMS_OK ||
        yan_search_terms_init(&terms, yan_search_terms_index_backend(index)) !=
            YAN_SEARCH_TERMS_OK) {
        fprintf(stderr, "PROBE index init failed\n");
        goto done;
    }
    YanSearchTermsResult rebuilt = yan_search_terms_rebuild(&terms);
    printf("REBUILD result=%d\n", (int)rebuilt);
    if (rebuilt != YAN_SEARCH_TERMS_OK) {
        goto done;
    }
    YanSearchTermsStatus state;
    memset(&state, 0, sizeof state);
    if (yan_search_terms_status(&terms, &state) != YAN_SEARCH_TERMS_OK) {
        fprintf(stderr, "PROBE status failed\n");
        goto done;
    }
    printf("INDEX state=%d source=%d terms=%" PRIu32 " postings=%" PRIu32
           "\n",
           (int)state.state, (int)state.source, state.terms, state.postings);
    if (state.state != YAN_SEARCH_INDEX_READY) {
        fprintf(stderr, "PROBE index is not READY (%d)\n", (int)state.state);
        goto done;
    }

    for (int i = 2; i < argc; ++i) {
        size_t query_size = strlen(argv[i]);
        if (query_size == 0u || query_size > YAN_SEARCH_TERMS_QUERY_MAX) {
            fprintf(stderr, "PROBE invalid query %d\n", i - 2);
            goto done;
        }
        Capture capture = {&terms, true};
        YanSearchTermsSink sink = {&capture, emit_match, NULL};
        YanSearchTermsSummary summary;
        memset(&summary, 0, sizeof summary);
        printf("QUERY text=%s\n", argv[i]);
        YanSearchTermsResult queried = yan_search_terms(
            &terms, (const uint8_t *)argv[i], (uint32_t)query_size, sink,
            &summary);
        printf("SUMMARY result=%d total=%" PRIu64 " shown=%" PRIu32
               " skipped=%" PRIu32 " mode=%d\n",
               (int)queried, summary.total, summary.shown, summary.skipped,
               (int)summary.mode);
        if (queried != YAN_SEARCH_TERMS_OK || !capture.ok) {
            goto done;
        }
    }
    if (ferror(stdout) != 0 || fflush(stdout) != 0) {
        goto done;
    }
    printf("PROBE OK\n");
    status = 0;
done:
    if (yan_semantic_posix_cleanup(adapter) != YAN_SEMANTIC_OK) {
        status = 1;
    }
    release(adapter, index, &terms, entries, eligible);
    return status;
}
