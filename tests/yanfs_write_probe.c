/* 0036-R2-AC4 write-sequence probe. Loads an image into memory, runs one fixed
 * create/replace/remove/rename/copy sequence through yan_fs_init (the v1 path)
 * and writes the image back. tests/test_yanfs_write_sha.py compares the
 * result with the SHA-256 the same probe produced against 41b83b5's
 * os/yanfs.c, so the v1 write path stays byte-identical. */
#include "yanfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *disk;
static uint64_t disk_blocks;

static YanFsIoResult cap(void *c, uint64_t *out) { (void)c; *out = disk_blocks; return YAN_FS_IO_OK; }
static YanFsIoResult rd(void *c, uint32_t lba, uint8_t out[4096])
{
    (void)c;
    if (lba >= disk_blocks) return YAN_FS_IO_ERROR;
    memcpy(out, disk + (size_t)lba * 4096u, 4096u);
    return YAN_FS_IO_OK;
}
static YanFsIoResult wr(void *c, uint32_t lba, const uint8_t data[4096])
{
    (void)c;
    if (lba >= disk_blocks) return YAN_FS_IO_ERROR;
    memcpy(disk + (size_t)lba * 4096u, data, 4096u);
    return YAN_FS_IO_OK;
}

static YanFs fs;
static uint8_t big[9000];

#define STEP(call) do { YanFsResult r_ = (call); if (r_ != YAN_FS_OK) { \
    fprintf(stderr, "step failed: %s -> %d\n", #call, (int)r_); return 1; } } while (0)

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: yanfs_write_probe IMAGE\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (f == NULL) return 2;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    disk = malloc((size_t)size);
    if (disk == NULL || fread(disk, 1u, (size_t)size, f) != (size_t)size) return 2;
    fclose(f);
    disk_blocks = (uint64_t)size / 4096u;
    for (size_t i = 0; i < sizeof big; ++i) big[i] = (uint8_t)(i * 7u + 3u);

    STEP(yan_fs_init(&fs, (YanFsBlockIo){NULL, cap, rd, wr}));
    STEP(yan_fs_mount(&fs));
    STEP(yan_fs_create(&fs, "a.txt", (const uint8_t *)"alpha\n", 6u));
    STEP(yan_fs_create(&fs, "b.txt", big, sizeof big));
    STEP(yan_fs_create(&fs, "empty", NULL, 0u));
    STEP(yan_fs_create(&fs, "c.txt", (const uint8_t *)"gamma", 5u));
    STEP(yan_fs_replace(&fs, "a.txt", big, 5000u));
    STEP(yan_fs_remove(&fs, "c.txt"));
    STEP(yan_fs_rename(&fs, "b.txt", "bee.txt"));
    STEP(yan_fs_copy(&fs, "bee.txt", "b2.txt"));
    STEP(yan_fs_create(&fs, "d.txt", (const uint8_t *)"delta", 5u));
    STEP(yan_fs_replace(&fs, "empty", (const uint8_t *)"now full", 8u));
    STEP(yan_fs_remove(&fs, "a.txt"));
    STEP(yan_fs_copy(&fs, "d.txt", "e.txt"));
    STEP(yan_fs_unmount(&fs));

    f = fopen(argv[1], "wb");
    if (f == NULL || fwrite(disk, 1u, (size_t)size, f) != (size_t)size || fclose(f) != 0) return 2;
    free(disk);
    return 0;
}
