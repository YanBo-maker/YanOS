#define _XOPEN_SOURCE 700

#include "note_write.h"

#include "editor.h"
#include "note_blocks_format.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    int source;
    int target;
    uint32_t blocks;
} ImageCopy;

static YanFsIoResult copy_capacity(void *context, uint64_t *blocks)
{
    *blocks = ((const ImageCopy *)context)->blocks;
    return YAN_FS_IO_OK;
}

static YanFsIoResult copy_read(void *context, uint32_t lba, uint8_t out[4096])
{
    const ImageCopy *copy = context;
    return pread(copy->source, out, YAN_FS_BLOCK_SIZE,
                 (off_t)lba * YAN_FS_BLOCK_SIZE) == (ssize_t)YAN_FS_BLOCK_SIZE
               ? YAN_FS_IO_OK : YAN_FS_IO_ERROR;
}

static YanFsIoResult copy_write(void *context, uint32_t lba, const uint8_t in[4096])
{
    const ImageCopy *copy = context;
    return pwrite(copy->target, in, YAN_FS_BLOCK_SIZE,
                  (off_t)lba * YAN_FS_BLOCK_SIZE) == (ssize_t)YAN_FS_BLOCK_SIZE
               ? YAN_FS_IO_OK : YAN_FS_IO_ERROR;
}

YanFsResult yan_image_lock(const char *image, int operation, int *fd)
{
    if (image == NULL || fd == NULL ||
        (operation != LOCK_EX && operation != LOCK_SH)) {
        return YAN_FS_INVALID;
    }
    char *resolved = realpath(image, NULL);
    if (resolved == NULL) return YAN_FS_IO;
    size_t length = strlen(resolved);
    char *path = malloc(length + sizeof ".lock");
    if (path == NULL) {
        free(resolved);
        return YAN_FS_IO;
    }
    memcpy(path, resolved, length);
    memcpy(path + length, ".lock", sizeof ".lock");
    free(resolved);
    int lock_fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    free(path);
    if (lock_fd < 0) return YAN_FS_IO;
    if (flock(lock_fd, operation | LOCK_NB) != 0) {
        int error = errno;
        (void)close(lock_fd);
        return error == EWOULDBLOCK ? YAN_FS_BUSY : YAN_FS_IO;
    }
    *fd = lock_fd;
    return YAN_FS_OK;
}

YanFsResult yan_note_write_check(const char *name, const uint8_t *text,
                                 uint32_t length)
{
    if (name == NULL) return YAN_FS_INVALID;
    size_t name_length = strlen(name);
    if (name_length > YAN_FS_NAME_MAX ||
        !yan_note_name_valid((const uint8_t *)name, (uint32_t)name_length)) {
        return YAN_FS_INVALID;
    }
    if (length > YAN_EDITOR_CAPACITY || (text == NULL && length != 0)) {
        return YAN_FS_INVALID;
    }
    YanBlockTextValidator validator;
    if (yan_block_text_init(&validator) != YAN_BLOCK_OK ||
        (length != 0 && yan_block_text_feed(&validator, text, length) != YAN_BLOCK_OK) ||
        yan_block_text_finish(&validator) != YAN_BLOCK_OK) {
        return YAN_FS_INVALID;
    }
    return YAN_FS_OK;
}

/* mkstemp needs the suffix at the end; the directory is unchanged and the
 * base name still starts with the image name. */
static char *temporary_template(const char *target)
{
    size_t size = strlen(target) + sizeof ".XXXXXX";
    char *path = malloc(size);
    if (path != NULL) snprintf(path, size, "%s.XXXXXX", target);
    return path;
}

static YanFsResult copy_image(int source, int target, uint32_t blocks)
{
    uint8_t buffer[YAN_FS_BLOCK_SIZE];
    for (uint32_t lba = 0; lba < blocks; ++lba) {
        if (pread(source, buffer, sizeof buffer, (off_t)lba * YAN_FS_BLOCK_SIZE) !=
                (ssize_t)sizeof buffer ||
            write(target, buffer, sizeof buffer) != (ssize_t)sizeof buffer) {
            return YAN_FS_IO;
        }
    }
    return YAN_FS_OK;
}

static int fsync_parent(const char *target)
{
    char *path = strdup(target);
    if (path == NULL) return -1;
    int fd = open(dirname(path), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    free(path);
    if (fd < 0) return -1;
    int result = fsync(fd);
    (void)close(fd);
    return result;
}

static YanFsResult write_image(const char *target, bool replace,
                               const char *name, const uint8_t *text,
                               uint32_t length, bool *committed)
{
    YanFsResult result = YAN_FS_OK;
    int source = -1;
    int temporary = -1;
    char *temporary_path = NULL;
    struct stat info;
    source = open(target, O_RDONLY | O_CLOEXEC);
    if (source < 0) {
        result = YAN_FS_IO;
        goto out;
    }
    if (fstat(source, &info) != 0 || info.st_size <= 0 ||
        info.st_size % YAN_FS_BLOCK_SIZE != 0 ||
        (uint64_t)info.st_size / YAN_FS_BLOCK_SIZE > UINT32_MAX) {
        result = YAN_FS_CORRUPT;
        goto out;
    }
    uint32_t blocks = (uint32_t)((uint64_t)info.st_size / YAN_FS_BLOCK_SIZE);
    temporary_path = temporary_template(target);
    if (temporary_path == NULL) {
        result = YAN_FS_IO;
        goto out;
    }
    temporary = mkstemp(temporary_path);
    if (temporary < 0) {
        result = YAN_FS_IO;
        goto out;
    }
    /* Keep the image's permissions; mkstemp creates the copy as 0600. */
    if (fchmod(temporary, info.st_mode & 07777) != 0) {
        result = YAN_FS_IO;
        goto out;
    }
    result = copy_image(source, temporary, blocks);
    if (result != YAN_FS_OK) goto out;

    ImageCopy copy = {source, temporary, blocks};
    YanFsBlockIo io = {&copy, copy_capacity, copy_read, copy_write};
    YanFs fs = {0};
    uint8_t scratch[YAN_FS_BLOCK_SIZE];
    uint8_t *directory = NULL;
    uint16_t *name_order = NULL;
    uint32_t directory_blocks = 0u;
    result = yan_fs_probe(&io, scratch, &directory_blocks);
    if (result == YAN_FS_OK) {
        directory = calloc(directory_blocks, YAN_FS_BLOCK_SIZE);
        name_order = calloc(directory_blocks * 64u, sizeof *name_order);
        if (directory == NULL || name_order == NULL) {
            result = YAN_FS_IO;
        } else {
            result = yan_fs_init_directory(&fs, io, directory,
                                           directory_blocks, name_order);
        }
    }
    if (result == YAN_FS_OK) result = yan_fs_mount(&fs);
    if (result == YAN_FS_OK) {
        result = replace ? yan_fs_replace(&fs, name, text, length)
                         : yan_fs_create(&fs, name, text, length);
    }
    free(name_order);
    free(directory);
    if (result != YAN_FS_OK) goto out;

    if (fsync(temporary) != 0) {
        result = YAN_FS_IO;
        goto out;
    }
    if (close(temporary) != 0) {
        temporary = -1;
        result = YAN_FS_IO;
        goto out;
    }
    temporary = -1;
    if (rename(temporary_path, target) != 0) {
        result = YAN_FS_IO;
        goto out;
    }
    free(temporary_path);
    temporary_path = NULL;
    *committed = true;
    if (fsync_parent(target) != 0) result = YAN_FS_IO;

out:
    if (temporary >= 0) (void)close(temporary);
    if (temporary_path != NULL) {
        (void)unlink(temporary_path);
        free(temporary_path);
    }
    if (source >= 0) (void)close(source);
    return result;
}

YanFsResult yan_note_write(const char *image, bool replace, const char *name,
                           const uint8_t *text, uint32_t length, bool *committed)
{
    if (committed == NULL) return YAN_FS_INVALID;
    *committed = false;
    YanFsResult result = yan_note_write_check(name, text, length);
    if (result != YAN_FS_OK) return result;
    char *target = image == NULL ? NULL : realpath(image, NULL);
    if (target == NULL) return YAN_FS_IO;
    int lock_fd = -1;
    result = yan_image_lock(target, LOCK_EX, &lock_fd);
    if (result == YAN_FS_OK) {
        result = write_image(target, replace, name, text, length, committed);
        (void)close(lock_fd);
    }
    free(target);
    return result;
}

YanFsResult yan_note_write_locked(const char *image, bool replace,
                                  const char *name, const uint8_t *text,
                                  uint32_t length, bool *committed)
{
    if (committed == NULL) return YAN_FS_INVALID;
    *committed = false;
    YanFsResult result = yan_note_write_check(name, text, length);
    if (result != YAN_FS_OK) return result;
    char *target = image == NULL ? NULL : realpath(image, NULL);
    if (target == NULL) return YAN_FS_IO;
    result = write_image(target, replace, name, text, length, committed);
    free(target);
    return result;
}
