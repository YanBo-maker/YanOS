/* yan_migrate: the thin 0036-R7 command line for the offline image migration
 * library. Argument errors exit 2 before the library runs. Every runtime
 * failure exits nonzero with a stable prefix, and success is printed only after
 * yan_image_migrate returned OK (all writes, fsyncs, closes, remounts and
 * comparisons done). */
#define _POSIX_C_SOURCE 200809L

#include "image_migrate.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static void usage(FILE *out)
{
    fputs("usage: yan_migrate --from OLD --to NEW --files N [--blocks B]\n", out);
    fputs("  --from OLD   existing YanFS image (read-only source)\n", out);
    fputs("  --to NEW     new image path (must not already exist)\n", out);
    fputs("  --files N    directory capacity, 63..4095 and 64*D-1\n", out);
    fputs("  --blocks B   new capacity in 4096-byte blocks (optional)\n", out);
}

static bool parse_u32(const char *text, uint32_t *out)
{
    if (text == NULL || *text == '\0') {
        return false;
    }
    uint64_t value = 0;
    for (const char *cursor = text; *cursor != '\0'; ++cursor) {
        if (*cursor < '0' || *cursor > '9') {
            return false;
        }
        value = value * 10u + (uint64_t)(*cursor - '0');
        if (value > UINT32_MAX) {
            return false;
        }
    }
    if (value == 0u) {
        return false;
    }
    *out = (uint32_t)value;
    return true;
}

static bool valid_files(uint32_t files)
{
    return files >= 63u && files <= 4095u && ((files + 1u) % 64u) == 0u;
}

static int report_failure(YanImageMigrateResult result, const char *error)
{
    const char *name = "IO";
    int code = 8;
    switch (result) {
    case YAN_IMAGE_MIGRATE_OK: name = "OK"; code = 0; break;
    case YAN_IMAGE_MIGRATE_INVALID: name = "INVALID"; code = 2; break;
    case YAN_IMAGE_MIGRATE_EXISTS: name = "EXISTS"; code = 3; break;
    case YAN_IMAGE_MIGRATE_BUSY: name = "BUSY"; code = 4; break;
    case YAN_IMAGE_MIGRATE_NOSPACE: name = "NOSPACE"; code = 5; break;
    case YAN_IMAGE_MIGRATE_CORRUPT: name = "CORRUPT"; code = 6; break;
    case YAN_IMAGE_MIGRATE_UNSUPPORTED: name = "UNSUPPORTED"; code = 7; break;
    case YAN_IMAGE_MIGRATE_IO: name = "IO"; code = 8; break;
    case YAN_IMAGE_MIGRATE_VERIFY: name = "VERIFY"; code = 9; break;
    }
    fprintf(stderr, "yan_migrate: %s: %s\n", name,
            error[0] != '\0' ? error : "migration failed");
    return code;
}

int main(int argc, char **argv)
{
    const char *from = NULL;
    const char *to = NULL;
    const char *files_text = NULL;
    const char *blocks_text = NULL;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--from") == 0) {
            if (from != NULL || i + 1 >= argc) {
                usage(stderr);
                return 2;
            }
            from = argv[++i];
        } else if (strcmp(argv[i], "--to") == 0) {
            if (to != NULL || i + 1 >= argc) {
                usage(stderr);
                return 2;
            }
            to = argv[++i];
        } else if (strcmp(argv[i], "--files") == 0) {
            if (files_text != NULL || i + 1 >= argc) {
                usage(stderr);
                return 2;
            }
            files_text = argv[++i];
        } else if (strcmp(argv[i], "--blocks") == 0) {
            if (blocks_text != NULL || i + 1 >= argc) {
                usage(stderr);
                return 2;
            }
            blocks_text = argv[++i];
        } else {
            usage(stderr);
            return 2;
        }
    }
    if (from == NULL || to == NULL || files_text == NULL ||
        *from == '\0' || *to == '\0') {
        usage(stderr);
        return 2;
    }

    uint32_t files = 0;
    if (!parse_u32(files_text, &files) || !valid_files(files)) {
        fprintf(stderr, "yan_migrate: invalid --files '%s'\n", files_text);
        return 2;
    }
    YanImageMigrateOptions options = {0};
    options.files = files;
    if (blocks_text != NULL) {
        if (!parse_u32(blocks_text, &options.blocks)) {
            fprintf(stderr, "yan_migrate: invalid --blocks '%s'\n", blocks_text);
            return 2;
        }
        options.blocks_set = true;
    }

    char error[2 * PATH_MAX + 512];
    YanImageMigrateResult result =
        yan_image_migrate(from, to, &options, error, sizeof error);
    if (result != YAN_IMAGE_MIGRATE_OK) {
        return report_failure(result, error);
    }
    printf("yan_migrate: migrated %s to %s (%u files)\n", from, to, files);
    return 0;
}
