/* 0036-R5-AC1 fixture builder: fill a formatted image with N notes.
 *
 * The Guest step test must mount a real, full image without paying the Host
 * cost of N terminal `create` commands. This probe uses the production YanFS
 * API over a file-backed block device, so the image it leaves behind is exactly
 * what a sequence of creates would have produced. It is a test input builder,
 * not a production tool.
 *
 * Every slot except the one the Guest will reuse holds a small note with a
 * `link: f0000.txt` declaration, one tag, block x and a `[[f0000.txt#x]]`
 * reference to the existing block in slot 0;
 * those are the shapes `backlinks`, `related` and `block backlinks` must walk.
 * Slot N/2 holds 60 plain bytes instead, so the Guest can `rm` it and `create`
 * a same-size replacement. */
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

static int parse_u32(const char *text, uint32_t *out)
{
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || value > UINT32_MAX) return 0;
    *out = (uint32_t)value;
    return 1;
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    uint32_t files = 0;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--image") == 0 && i + 1 < argc) {
            path = argv[++i];
        } else if (strcmp(argv[i], "--files") == 0 && i + 1 < argc) {
            if (!parse_u32(argv[++i], &files)) return 2;
        } else {
            return 2;
        }
    }
    if (path == NULL || files < 63u || files > 4095u ||
        (files + 1u) % 64u != 0u) {
        fprintf(stderr, "usage: %s --image FILE --files N\n", argv[0]);
        return 2;
    }
    const uint32_t directory_blocks = (files + 1u) / 64u;
    Image image = {fopen(path, "r+b")};
    if (image.file == NULL) {
        fprintf(stderr, "yanfs_fill: cannot open '%s'\n", path);
        return 5;
    }
    uint8_t *directory = calloc(directory_blocks, YAN_FS_BLOCK_SIZE);
    uint16_t *name_order = calloc(files, sizeof *name_order);
    YanFs fs = {0};
    YanFsBlockIo io = {&image, capacity, read_block, write_block};
    if (directory == NULL || name_order == NULL ||
        yan_fs_init_directory(&fs, io, directory, directory_blocks,
                              name_order) != YAN_FS_OK ||
        yan_fs_mount(&fs) != YAN_FS_OK) {
        fprintf(stderr, "yanfs_fill: cannot mount '%s'\n", path);
        free(name_order);
        free(directory);
        fclose(image.file);
        return 5;
    }
    char note[] = "@yan-note 1\nlink: f0000.txt\ntag: t\n@end\n@block x\n[[f0000.txt#x]]\n";
    char plain[60];
    memset(plain, 'a', sizeof plain);
    bool clean = true;
    for (uint32_t slot = 0; slot < files; ++slot) {
        char name[16];
        (void)snprintf(name, sizeof name, "f%04" PRIu32 ".txt", slot);
        const uint8_t *bytes = (const uint8_t *)note;
        uint32_t length = (uint32_t)strlen(note);
        if (slot == files / 2u) {
            bytes = (const uint8_t *)plain;
            length = sizeof plain;
        }
        YanFsResult result = yan_fs_create(&fs, name, bytes, length);
        if (result != YAN_FS_OK) {
            fprintf(stderr, "yanfs_fill: create %s failed: %d\n", name,
                    (int)result);
            clean = false;
            break;
        }
    }
    if (yan_fs_unmount(&fs) != YAN_FS_OK) clean = false;
    free(name_order);
    free(directory);
    if (fclose(image.file) != 0) clean = false;
    return clean ? 0 : 5;
}
