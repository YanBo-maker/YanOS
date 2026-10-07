/* Host fixture driver for the independent term-search reference.
 * Input uses little-endian lengths followed by raw bytes. Windows stdin must
 * be binary: CR and Ctrl-Z are legitimate bytes in a source file. All output
 * is ASCII, with snippet bytes encoded as hex while the callback owns them. */
#include "yanfs.h"
#include "search_terms.h"
#include "search_terms_linear.h"
#include "search_terms_index.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#define BLOCKS 2048u
static uint8_t medium[BLOCKS][4096];
static YanFs fs;
static uint64_t read_calls;
static uint64_t write_calls;
static YanSearchTermsLinear linear;
static YanSearchTermsIndex index_context;
static YanSearchTerms facade;

static YanFsIoResult capacity(void *context, uint64_t *out)
{
    (void)context;
    *out = BLOCKS;
    return YAN_FS_IO_OK;
}

static YanFsIoResult read_block(void *context, uint32_t lba, uint8_t out[4096])
{
    (void)context;
    ++read_calls;
    if (lba >= BLOCKS) {
        return YAN_FS_IO_ERROR;
    }
    memcpy(out, medium[lba], 4096);
    return YAN_FS_IO_OK;
}

static YanFsIoResult write_block(void *context, uint32_t lba,
                                 const uint8_t in[4096])
{
    (void)context;
    ++write_calls;
    if (lba >= BLOCKS) {
        return YAN_FS_IO_ERROR;
    }
    memcpy(medium[lba], in, 4096);
    return YAN_FS_IO_OK;
}

static uint32_t get32(void)
{
    uint8_t bytes[4];
    if (fread(bytes, 1, 4, stdin) != 4) {
        fputs("fixture EOF\n", stderr);
        exit(90);
    }
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void get_bytes(void *out, size_t length)
{
    if (length != 0 && fread(out, 1, length, stdin) != length) {
        fputs("fixture short\n", stderr);
        exit(91);
    }
}

static bool match(void *context, const YanSearchTermsMatch *value)
{
    (void)context;
    printf("M\t%s\t%u\t%u\t%u\t%u\t", value->name,
           (unsigned)value->line_number, (unsigned)value->score,
           (unsigned)value->left_truncated, (unsigned)value->right_truncated);
    uint32_t offset = 0u;
    while (offset < value->snippet_length) {
        const uint8_t *bytes = NULL;
        uint32_t length = 0u;
        YanSearchTermsResult result = yan_search_terms_read_snippet(
            &facade, offset, 640u, &bytes, &length);
        if (result != YAN_SEARCH_TERMS_OK || bytes == NULL || length == 0u ||
            length > value->snippet_length - offset) {
            fprintf(stderr, "snippet error %d\n", (int)result);
            exit(92);
        }
        for (uint32_t i = 0u; i < length; ++i) {
            printf("%02x", (unsigned)bytes[i]);
        }
        offset += length;
    }
    putchar('\n');
    return true;
}

static bool warm_match(void *context, const YanSearchTermsMatch *value)
{
    (void)context;
    (void)value;
    return true;
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    if (_setmode(_fileno(stdin), _O_BINARY) == -1) {
        return 88;
    }
#endif
    bool repeat = argc == 2 && strcmp(argv[1], "index-repeat") == 0;
    bool indexed = repeat || (argc == 2 && strcmp(argv[1], "index") == 0);
    if (argc != 2 || (!indexed && strcmp(argv[1], "scan") != 0)) {
        return 89;
    }
    uint32_t query_length = get32();
    if (query_length > 1023u) {
        return 93;
    }
    uint8_t query[1023];
    get_bytes(query, query_length);
    uint32_t count = get32();
    if (count > 63u) {
        return 94;
    }
    if (yan_fs_format_metadata(medium[0], BLOCKS) != YAN_FS_OK) {
        return 95;
    }
    YanFsBlockIo io = {NULL, capacity, read_block, write_block};
    if (yan_fs_init(&fs, io) != YAN_FS_OK || yan_fs_mount(&fs) != YAN_FS_OK) {
        return 96;
    }
    uint32_t next_slot = 0u;
    char holes[63][32];
    uint32_t hole_count = 0u;
    for (uint32_t i = 0u; i < count; ++i) {
        uint32_t slot = get32();
        uint32_t name_length = get32();
        uint32_t length = get32();
        if (slot >= 63u || slot < next_slot || name_length == 0u ||
            name_length > 31u || length > 4000000u) {
            return 97;
        }
        char name[32];
        get_bytes(name, name_length);
        name[name_length] = '\0';
        while (next_slot < slot) {
            (void)snprintf(holes[hole_count], 32, "_hole%u", (unsigned)next_slot);
            if (yan_fs_create(&fs, holes[hole_count], NULL, 0u) != YAN_FS_OK) {
                return 98;
            }
            ++hole_count;
            ++next_slot;
        }
        uint8_t *data = length != 0u ? malloc(length) : NULL;
        if (length != 0u && data == NULL) {
            return 99;
        }
        get_bytes(data, length);
        YanFsResult result = yan_fs_create(&fs, name, data, length);
        free(data);
        if (result != YAN_FS_OK) {
            return 100;
        }
        ++next_slot;
    }
    for (uint32_t i = 0u; i < hole_count; ++i) {
        if (yan_fs_remove(&fs, holes[i]) != YAN_FS_OK) {
            return 101;
        }
    }
    YanSearchTermsBackend backend;
    if (indexed) {
        if (yan_search_terms_index_init(&index_context, &fs) != YAN_SEARCH_TERMS_OK) {
            return 102;
        }
        backend = yan_search_terms_index_backend(&index_context);
    } else {
        if (yan_search_terms_linear_init(&linear, &fs) != YAN_SEARCH_TERMS_OK) {
            return 102;
        }
        backend = yan_search_terms_linear_backend(&linear);
    }
    if (yan_search_terms_init(&facade, backend) != YAN_SEARCH_TERMS_OK) {
        return 103;
    }
    YanSearchTermsSummary summary = {0};
    YanSearchTermsSink sink = {NULL, match, NULL};
    if (repeat) {
        YanSearchTermsSink warm = {NULL, warm_match, NULL};
        YanSearchTermsResult warmed = yan_search_terms(
            &facade, query, query_length, warm, NULL);
        if (warmed != YAN_SEARCH_TERMS_OK && warmed != YAN_SEARCH_TERMS_INVALID) {
            return 104;
        }
    }
    uint64_t before_reads = read_calls;
    uint64_t before_writes = write_calls;
    YanSearchTermsResult result = yan_search_terms(
        &facade, query, query_length, sink, &summary);
    printf("Q\t%d\t%d\t%llu\t%u\t%u\t%llu\t%llu\n", (int)result,
           (int)summary.mode, (unsigned long long)summary.total,
           (unsigned)summary.shown, (unsigned)summary.skipped,
           (unsigned long long)(read_calls - before_reads),
           (unsigned long long)(write_calls - before_writes));
    return 0;
}
