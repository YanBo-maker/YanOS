#define _POSIX_C_SOURCE 200809L
#include "host_disk.h"

#include <limits.h>
#include <sys/stat.h>

bool yan_host_disk_open(YanHostDisk *disk, const char *path)
{
    if (disk == NULL || path == NULL || disk->file != NULL) return false;
    FILE *file = fopen(path, "r+b");
    if (file == NULL) return false;
    struct stat info;
    long bytes = -1;
    if (fstat(fileno(file), &info) == 0 && S_ISREG(info.st_mode) &&
        fseek(file, 0, SEEK_END) == 0) bytes = ftell(file);
    if (bytes <= 0 || (uint64_t)bytes % YAN_HOST_BLOCK_BLOCK_SIZE != 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        (void)fclose(file);
        return false;
    }
    *disk = (YanHostDisk){.file = file, .bytes = (uint64_t)bytes};
    return true;
}

bool yan_host_disk_close(YanHostDisk *disk)
{
    if (disk == NULL) return false;
    const bool ok = disk->file == NULL || fclose(disk->file) == 0;
    *disk = (YanHostDisk){0};
    return ok;
}

bool yan_host_disk_same_file(const YanHostDisk *disk, const char *path)
{
    struct stat image, other;
    return disk != NULL && disk->file != NULL && path != NULL &&
           fstat(fileno(disk->file), &image) == 0 && stat(path, &other) == 0 &&
           image.st_dev == other.st_dev && image.st_ino == other.st_ino;
}

static bool position(YanHostDisk *disk, uint64_t offset, size_t length)
{
    return disk != NULL && disk->file != NULL && !disk->failed &&
           offset <= disk->bytes && length <= disk->bytes - offset &&
           offset <= LONG_MAX && fseek(disk->file, (long)offset, SEEK_SET) == 0;
}

static bool read_disk(void *context, uint64_t offset, uint8_t *data, size_t length)
{
    YanHostDisk *disk = context;
    const bool ok = data != NULL && position(disk, offset, length) &&
                    fread(data, 1, length, disk->file) == length;
    if (!ok && disk != NULL) disk->failed = true;
    return ok;
}

static bool write_disk(void *context, uint64_t offset, const uint8_t *data, size_t length)
{
    YanHostDisk *disk = context;
    /* fflush gives cross-process visibility before the success response. It
     * does not promise power-loss durability or atomic replacement of a block.
     * Poison a failed backend: retries must not turn uncertain writes into a
     * later success without reopening and checking the image. */
    const bool ok = data != NULL && position(disk, offset, length) &&
                    fwrite(data, 1, length, disk->file) == length &&
                    fflush(disk->file) == 0;
    if (!ok && disk != NULL) disk->failed = true;
    return ok;
}

YanHostBlockBackend yan_host_disk_backend(YanHostDisk *disk)
{
    return (YanHostBlockBackend){.context = disk, .read = read_disk, .write = write_disk};
}
