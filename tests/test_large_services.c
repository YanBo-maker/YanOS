/* 0036-R4/R6: build the real N=1023 image used by the large-capacity
 * acceptance test.
 *
 * This is a test input builder, not a production tool.  It formats nothing
 * itself: `yan_mkfs --files 1023` creates the v2 directory and this program
 * only calls the production YanFS API (`yan_fs_init_directory`, `yan_fs_mount`,
 * `yan_fs_create`, `yan_fs_list`, `yan_fs_read`) over a file-backed block
 * device.  The image it leaves behind is therefore exactly the image a
 * sequence of production creates would have produced, including slot order,
 * extents and CRCs.
 *
 * Slot layout (must stay in lockstep with tests/run_large_capacity.py):
 *
 *   0..999    n0000.txt .. n0999.txt   link: a.txt and [[a.txt#x]]
 *   1000      a.txt                    declares block x
 *   1001      shared.txt               declares block anchor
 *   1002      relsrc.txt               tag: sharedtag (related source)
 *   1003      big.txt                  filler; --filler-words / --huge-body-bytes
 *   1004..1021 x1004.txt .. x1021.txt  plain fillers
 *   1022      slot1022.txt             unique word/tag, link, block, reference
 *
 * --filler-words only sizes a candidate body; --huge-body-bytes makes the
 * semantic chunker exceed its 8192-segment cap.  Neither option counts term
 * occurrences: the acceptance driver calibrates and asserts the real posting
 * count from the product's own `rebuild` / `index status` output.
 *
 * After writing, the program unmounts, remounts and walks the directory with
 * the independent listing API: 1023 entries, physical slot order, and the
 * exact slot-1022 content.  It prints one machine-readable summary line so the
 * Python driver can record the fixture facts it verifies independently.
 */
#define _POSIX_C_SOURCE 200809L

#include "yanfs.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

typedef struct {
    FILE *file;
} Image;

static YanFsIoResult capacity(void *context, uint64_t *blocks)
{
    Image *image = context;
    if (fseeko(image->file, 0, SEEK_END) != 0) return YAN_FS_IO_ERROR;
    off_t size = ftello(image->file);
    if (size <= 0 || size % (off_t)YAN_FS_BLOCK_SIZE != 0) {
        return YAN_FS_IO_ERROR;
    }
    *blocks = (uint64_t)size / YAN_FS_BLOCK_SIZE;
    return YAN_FS_IO_OK;
}

static YanFsIoResult read_block(void *context, uint32_t lba, uint8_t out[4096])
{
    Image *image = context;
    if (fseeko(image->file, (off_t)lba * YAN_FS_BLOCK_SIZE, SEEK_SET) != 0) {
        return YAN_FS_IO_ERROR;
    }
    return fread(out, 1, YAN_FS_BLOCK_SIZE, image->file) == YAN_FS_BLOCK_SIZE
               ? YAN_FS_IO_OK
               : YAN_FS_IO_ERROR;
}

static YanFsIoResult write_block(void *context, uint32_t lba,
                                 const uint8_t data[4096])
{
    Image *image = context;
    if (fseeko(image->file, (off_t)lba * YAN_FS_BLOCK_SIZE, SEEK_SET) != 0) {
        return YAN_FS_IO_ERROR;
    }
    return fwrite(data, 1, YAN_FS_BLOCK_SIZE, image->file) == YAN_FS_BLOCK_SIZE
               ? YAN_FS_IO_OK
               : YAN_FS_IO_ERROR;
}

static bool parse_u32(const char *text, uint32_t *out)
{
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || value > UINT32_MAX) return false;
    *out = (uint32_t)value;
    return true;
}

static bool parse_u64(const char *text, uint64_t *out)
{
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    if (end == text || *end != '\0') return false;
    *out = (uint64_t)value;
    return true;
}

static const char LINKER[] = "@yan-note 1\nlink: a.txt\n@end\n[[a.txt#x]]\n";
static const char A_TXT[] = "@yan-note 1\n@end\n@block x\nanchor body\n";
static const char SHARED_TXT[] =
    "@yan-note 1\n@end\n@block anchor\nshared anchor body\n";
static const char RELSRC[] =
    "@yan-note 1\ntag: sharedtag\n@end\nrelated source\n";
static const char FILLER[] = "@yan-note 1\n@end\nfiller\n";
static const char BIG_HEADER[] = "@yan-note 1\n@end\n";
static const char SPECIAL[] =
    "@yan-note 1\n"
    "tag: slottag1022\n"
    "tag: sharedtag\n"
    "link: shared.txt\n"
    "@end\n"
    "@block zslot\n"
    "slotlastuniqueword body of the final slot notes query [[shared.txt#anchor]]\n";

/* `count` four-character vocabulary words (w000..w255) separated by one space
 * and terminated by one LF. */
static char *filler_body(uint32_t count)
{
    if (count > (UINT32_MAX - 2u) / 5u) return NULL;
    size_t length = (size_t)count * 5u + 1u;
    char *body = malloc(length + 1u);
    if (body == NULL) return NULL;
    size_t at = 0;
    for (uint32_t i = 0; i < count; ++i) {
        (void)snprintf(body + at, 5u, "w%03u", i % 256u);
        at += 4u;
        body[at++] = ' ';
    }
    body[at++] = '\n';
    body[at] = '\0';
    return body;
}

static char *huge_body(uint64_t count)
{
    if (count > SIZE_MAX - 2u) return NULL;
    size_t length = (size_t)count;
    char *body = malloc(length + 2u);
    if (body == NULL) return NULL;
    memset(body, 'a', length);
    body[length] = '\n';
    body[length + 1u] = '\0';
    return body;
}

static void slot_name(uint32_t slot, char out[16])
{
    if (slot < 1000u) {
        (void)snprintf(out, 16u, "n%04" PRIu32 ".txt", slot);
    } else if (slot >= 1004u && slot <= 1021u) {
        (void)snprintf(out, 16u, "x%04" PRIu32 ".txt", slot);
    } else if (slot == 1000u) {
        (void)snprintf(out, 16u, "a.txt");
    } else if (slot == 1001u) {
        (void)snprintf(out, 16u, "shared.txt");
    } else if (slot == 1002u) {
        (void)snprintf(out, 16u, "relsrc.txt");
    } else if (slot == 1003u) {
        (void)snprintf(out, 16u, "big.txt");
    } else {
        (void)snprintf(out, 16u, "slot1022.txt");
    }
}

static const char *fixed_content(uint32_t slot, size_t *length)
{
    if (slot < 1000u) {
        *length = sizeof LINKER - 1u;
        return LINKER;
    }
    if (slot >= 1004u && slot <= 1021u) {
        *length = sizeof FILLER - 1u;
        return FILLER;
    }
    if (slot == 1000u) {
        *length = sizeof A_TXT - 1u;
        return A_TXT;
    }
    if (slot == 1001u) {
        *length = sizeof SHARED_TXT - 1u;
        return SHARED_TXT;
    }
    if (slot == 1002u) {
        *length = sizeof RELSRC - 1u;
        return RELSRC;
    }
    if (slot == 1022u) {
        *length = sizeof SPECIAL - 1u;
        return SPECIAL;
    }
    return NULL;
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    uint32_t files = 0;
    uint64_t filler_words = 0;
    uint64_t huge_bytes = 0;
    bool has_words = false, has_huge = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--image") == 0 && i + 1 < argc) {
            path = argv[++i];
        } else if (strcmp(argv[i], "--files") == 0 && i + 1 < argc) {
            if (!parse_u32(argv[++i], &files)) return 2;
        } else if (strcmp(argv[i], "--filler-words") == 0 && i + 1 < argc) {
            if (!parse_u64(argv[++i], &filler_words)) return 2;
            has_words = true;
        } else if (strcmp(argv[i], "--huge-body-bytes") == 0 && i + 1 < argc) {
            if (!parse_u64(argv[++i], &huge_bytes)) return 2;
            has_huge = true;
        } else {
            return 2;
        }
    }
    if (path == NULL || files != 1023u || (has_words && has_huge) ||
        filler_words > UINT32_MAX) {
        fprintf(stderr,
                "usage: %s --image FILE --files 1023 "
                "[--filler-words N | --huge-body-bytes N]\n",
                argv[0]);
        return 2;
    }

    const uint32_t directory_blocks = (files + 1u) / 64u;
    Image image = {fopen(path, "r+b")};
    if (image.file == NULL) {
        fprintf(stderr, "large_fixture: cannot open '%s'\n", path);
        return 5;
    }
    uint8_t *directory = calloc(directory_blocks, YAN_FS_BLOCK_SIZE);
    uint16_t *name_order = calloc(files, sizeof *name_order);
    YanFs fs = {0};
    YanFsBlockIo io = {&image, capacity, read_block, write_block};
    char *big = NULL;
    int status = 5;
    if (directory == NULL || name_order == NULL ||
        yan_fs_init_directory(&fs, io, directory, directory_blocks,
                              name_order) != YAN_FS_OK ||
        yan_fs_mount(&fs) != YAN_FS_OK) {
        fprintf(stderr, "large_fixture: cannot mount '%s'\n", path);
        goto done;
    }

    /* `--filler-words` sizes the candidate body only.  The product's own
     * `index status` posts the real posting count; this builder never claims a
     * token count and never stands in for the indexer. */
    if (has_huge || has_words) {
        char *body = has_huge ? huge_body(huge_bytes) : filler_body((uint32_t)filler_words);
        if (body == NULL) goto done;
        size_t length = strlen(body);
        big = malloc(sizeof BIG_HEADER - 1u + length + 1u);
        if (big == NULL) {
            free(body);
            goto done;
        }
        memcpy(big, BIG_HEADER, sizeof BIG_HEADER - 1u);
        memcpy(big + sizeof BIG_HEADER - 1u, body, length + 1u);
        free(body);
    }

    for (uint32_t slot = 0; slot < files; ++slot) {
        char name[16];
        const char *content = NULL;
        size_t length = 0;
        slot_name(slot, name);
        if (slot == 1003u) {
            if (big != NULL) {
                content = big;
                length = strlen(big);
            } else {
                content = FILLER;
                length = sizeof FILLER - 1u;
            }
        } else {
            content = fixed_content(slot, &length);
        }
        if (content == NULL) goto done;
        YanFsResult result = yan_fs_create(&fs, name,
                                           (const uint8_t *)content,
                                           (uint32_t)length);
        if (result != YAN_FS_OK) {
            fprintf(stderr, "large_fixture: create %s failed: %d\n", name,
                    (int)result);
            goto done;
        }
    }

    /* Remount and walk the directory through the product listing API. */
    if (yan_fs_unmount(&fs) != YAN_FS_OK || yan_fs_mount(&fs) != YAN_FS_OK) {
        fprintf(stderr, "large_fixture: remount failed\n");
        goto done;
    }
    uint32_t cursor = 0, seen = 0;
    for (;;) {
        YanFsInfo info;
        YanFsResult result = yan_fs_list(&fs, &cursor, &info);
        if (result == YAN_FS_END) break;
        if (result != YAN_FS_OK || seen >= files || cursor != seen + 1u) {
            fprintf(stderr, "large_fixture: list failed at %u: %d\n", seen,
                    (int)result);
            goto done;
        }
        char expected[16];
        slot_name(seen, expected);
        if (strcmp(info.name, expected) != 0) {
            fprintf(stderr, "large_fixture: slot %u is %s not %s\n", seen,
                    info.name, expected);
            goto done;
        }
        ++seen;
    }
    if (seen != files || cursor != files) {
        fprintf(stderr, "large_fixture: listed %u of %u\n", seen, files);
        goto done;
    }
    YanFsInfo info;
    if (yan_fs_stat(&fs, "slot1022.txt", &info) != YAN_FS_OK ||
        info.size_bytes != sizeof SPECIAL - 1u) {
        fprintf(stderr, "large_fixture: slot 1022 metadata differs\n");
        goto done;
    }
    uint8_t *readback = malloc(sizeof SPECIAL);
    uint32_t got = 0;
    if (readback == NULL ||
        yan_fs_read(&fs, "slot1022.txt", 0, readback,
                    (uint32_t)(sizeof SPECIAL - 1u), &got) != YAN_FS_OK ||
        got != sizeof SPECIAL - 1u ||
        memcmp(readback, SPECIAL, sizeof SPECIAL - 1u) != 0) {
        fprintf(stderr, "large_fixture: slot 1022 content differs\n");
        free(readback);
        goto done;
    }
    free(readback);
    if (yan_fs_unmount(&fs) != YAN_FS_OK) goto done;
    printf("OK fixture files=%" PRIu32 " filler_words=%" PRIu64
           " slot1022=slot1022.txt huge_bytes=%" PRIu64 "\n",
           files, has_words ? filler_words : 0u,
           has_huge ? huge_bytes : 0u);
    status = 0;

done:
    free(big);
    free(name_order);
    free(directory);
    if (fclose(image.file) != 0 && status == 0) status = 5;
    return status;
}
