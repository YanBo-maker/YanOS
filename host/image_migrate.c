#define _XOPEN_SOURCE 700

#include "image_migrate.h"

#include "note_write.h"
#include "semantic_corpus.h"
#include "semantic_posix.h"
#include "yanfs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define MIG_HEADER_BYTES 64u
#define MIG_ENTRY_BYTES 64u
#define MIG_NAME_BYTES 32u
#define MIG_SIZE_OFFSET 32u
#define MIG_START_OFFSET 36u
#define MIG_COUNT_OFFSET 40u
#define MIG_CRC_OFFSET 60u
#define MIG_COPY_BUFFER 16384u

static uint32_t load_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void store_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static YanImageMigrateResult set_error(char *error, size_t error_size,
                                       YanImageMigrateResult result,
                                       const char *format, ...)
{
    if (error != NULL && error_size > 0u) {
        va_list args;
        va_start(args, format);
        (void)vsnprintf(error, error_size, format, args);
        va_end(args);
    }
    return result;
}

static void append_error(char *error, size_t error_size, const char *format, ...)
{
    if (error == NULL || error_size == 0u) {
        return;
    }
    size_t used = strlen(error);
    if (used + 1u >= error_size) {
        return;
    }
    va_list args;
    va_start(args, format);
    (void)vsnprintf(error + used, error_size - used, format, args);
    va_end(args);
}

static bool pwrite_exact(int fd, const void *in, size_t length, uint64_t offset)
{
    if (offset > (uint64_t)INT64_MAX ||
        length > (size_t)((uint64_t)INT64_MAX - offset)) {
        return false;
    }
    const uint8_t *bytes = in;
    while (length > 0u) {
        ssize_t put = pwrite(fd, bytes, length, (off_t)offset);
        if (put < 0 && errno == EINTR) {
            continue;
        }
        if (put <= 0) {
            return false;
        }
        bytes += (size_t)put;
        length -= (size_t)put;
        offset += (uint64_t)put;
    }
    return true;
}

static bool read_exact(int fd, void *out, size_t length)
{
    uint8_t *bytes = out;
    while (length > 0u) {
        ssize_t got = read(fd, bytes, length);
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            return false;
        }
        bytes += (size_t)got;
        length -= (size_t)got;
    }
    return true;
}

static bool write_exact(int fd, const void *in, size_t length)
{
    const uint8_t *bytes = in;
    while (length > 0u) {
        ssize_t put = write(fd, bytes, length);
        if (put < 0 && errno == EINTR) {
            continue;
        }
        if (put <= 0) {
            return false;
        }
        bytes += (size_t)put;
        length -= (size_t)put;
    }
    return true;
}

static char *path_join(const char *directory, const char *base)
{
    size_t left = strlen(directory);
    size_t right = strlen(base);
    bool slash = left > 0u && directory[left - 1u] != '/';
    if (left > SIZE_MAX - right - 2u) {
        return NULL;
    }
    char *path = malloc(left + right + (slash ? 2u : 1u));
    if (path == NULL) {
        return NULL;
    }
    memcpy(path, directory, left);
    size_t at = left;
    if (slash) {
        path[at++] = '/';
    }
    memcpy(path + at, base, right + 1u);
    return path;
}

static char *append_suffix(const char *text, const char *suffix)
{
    size_t left = strlen(text);
    size_t right = strlen(suffix);
    if (left > SIZE_MAX - right - 1u) {
        return NULL;
    }
    char *out = malloc(left + right + 1u);
    if (out != NULL) {
        memcpy(out, text, left);
        memcpy(out + left, suffix, right + 1u);
    }
    return out;
}

static bool split_target(const char *path, char **parent, char **base)
{
    if (path == NULL || *path == '\0') {
        return false;
    }
    const char *slash = strrchr(path, '/');
    if (slash == path) {
        *parent = strdup("/");
        *base = strdup(slash + 1);
    } else if (slash == NULL) {
        *parent = strdup(".");
        *base = strdup(path);
    } else {
        *parent = strndup(path, (size_t)(slash - path));
        *base = strdup(slash + 1);
    }
    if (*parent == NULL || *base == NULL || **base == '\0' ||
        strcmp(*base, ".") == 0 || strcmp(*base, "..") == 0) {
        free(*parent);
        free(*base);
        *parent = NULL;
        *base = NULL;
        return false;
    }
    return true;
}

static bool path_is_under(const char *candidate, const char *root)
{
    size_t length = strlen(root);
    if (length == 0u) {
        return false;
    }
    if (strcmp(candidate, root) == 0) {
        return true;
    }
    if (strncmp(candidate, root, length) != 0) {
        return false;
    }
    if (strcmp(root, "/") == 0) {
        return candidate[0] == '/';
    }
    return candidate[length] == '/';
}

static uint32_t blocks_for_size(uint32_t size)
{
    uint32_t count = size / YAN_FS_BLOCK_SIZE;
    if ((size % YAN_FS_BLOCK_SIZE) != 0u) {
        ++count;
    }
    return count;
}

static bool files_to_blocks(uint32_t files, uint32_t *directory_blocks)
{
    if (files < 63u || files > 4095u || ((files + 1u) % 64u) != 0u) {
        return false;
    }
    uint32_t blocks = (files + 1u) / 64u;
    if (blocks < 1u || blocks > YAN_FS_MAX_DIR_BLOCKS) {
        return false;
    }
    *directory_blocks = blocks;
    return true;
}

static bool capacity_representable(uint32_t blocks, uint64_t *bytes_out)
{
    uint64_t bytes = (uint64_t)blocks * (uint64_t)YAN_FS_BLOCK_SIZE;
    if (bytes > (uint64_t)INT64_MAX) {
        return false;
    }
    if (sizeof(off_t) < 8u && bytes > (uint64_t)INT32_MAX) {
        return false;
    }
    *bytes_out = bytes;
    return true;
}

static YanImageMigrateResult map_fs(YanFsResult result)
{
    switch (result) {
    case YAN_FS_OK: return YAN_IMAGE_MIGRATE_OK;
    case YAN_FS_CORRUPT: return YAN_IMAGE_MIGRATE_CORRUPT;
    case YAN_FS_UNSUPPORTED: return YAN_IMAGE_MIGRATE_UNSUPPORTED;
    case YAN_FS_NOSPACE: return YAN_IMAGE_MIGRATE_NOSPACE;
    case YAN_FS_BUSY: return YAN_IMAGE_MIGRATE_BUSY;
    case YAN_FS_INVALID: return YAN_IMAGE_MIGRATE_INVALID;
    default: return YAN_IMAGE_MIGRATE_IO;
    }
}

static YanImageMigrateResult map_open(YanImageOpenResult result)
{
    switch (result) {
    case YAN_IMAGE_OPEN_OK: return YAN_IMAGE_MIGRATE_OK;
    case YAN_IMAGE_OPEN_INVALID: return YAN_IMAGE_MIGRATE_INVALID;
    case YAN_IMAGE_OPEN_IO: return YAN_IMAGE_MIGRATE_IO;
    case YAN_IMAGE_OPEN_CORRUPT: return YAN_IMAGE_MIGRATE_CORRUPT;
    case YAN_IMAGE_OPEN_UNSUPPORTED: return YAN_IMAGE_MIGRATE_UNSUPPORTED;
    case YAN_IMAGE_OPEN_BUSY: return YAN_IMAGE_MIGRATE_BUSY;
    case YAN_IMAGE_OPEN_NOT_MOUNTED:
    case YAN_IMAGE_OPEN_FAULTED:
    case YAN_IMAGE_OPEN_PROTOCOL:
        return YAN_IMAGE_MIGRATE_IO;
    }
    return YAN_IMAGE_MIGRATE_IO;
}

static YanImageMigrateResult map_semantic(YanSemanticResult result)
{
    switch (result) {
    case YAN_SEMANTIC_OK: return YAN_IMAGE_MIGRATE_OK;
    case YAN_SEMANTIC_INVALID: return YAN_IMAGE_MIGRATE_INVALID;
    case YAN_SEMANTIC_CORRUPT: return YAN_IMAGE_MIGRATE_CORRUPT;
    case YAN_SEMANTIC_UNSUPPORTED: return YAN_IMAGE_MIGRATE_UNSUPPORTED;
    case YAN_SEMANTIC_IO:
    case YAN_SEMANTIC_DEPENDENCY:
        return YAN_IMAGE_MIGRATE_IO;
    default:
        return YAN_IMAGE_MIGRATE_VERIFY;
    }
}

static YanImageMigrateResult require_absent(int parent, const char *base,
                                            const char *label, char *error,
                                            size_t error_size)
{
    struct stat info;
    if (fstatat(parent, base, &info, AT_SYMLINK_NOFOLLOW) == 0) {
        return set_error(error, error_size, YAN_IMAGE_MIGRATE_EXISTS,
                         "%s already exists", label);
    }
    if (errno != ENOENT) {
        return set_error(error, error_size, YAN_IMAGE_MIGRATE_IO,
                         "cannot inspect %s", label);
    }
    return YAN_IMAGE_MIGRATE_OK;
}

static YanImageMigrateResult scan_source(YanSemanticPosix *old, uint32_t files,
                                         uint64_t *data_blocks, char *error,
                                         size_t error_size)
{
    uint32_t cursor = 0u;
    uint64_t blocks = 0u;
    for (;;) {
        YanFsInfo info;
        YanFsResult result = yan_fs_list(&old->fs, &cursor, &info);
        if (result == YAN_FS_END) {
            break;
        }
        if (result != YAN_FS_OK) {
            return set_error(error, error_size, map_fs(result),
                             "cannot enumerate source image");
        }
        uint32_t slot = cursor - 1u;
        if (slot >= files) {
            return set_error(error, error_size, YAN_IMAGE_MIGRATE_NOSPACE,
                             "requested --files is below the highest used slot");
        }
        blocks += blocks_for_size(info.size_bytes);
    }
    *data_blocks = blocks;
    return YAN_IMAGE_MIGRATE_OK;
}

static YanImageMigrateResult build_directory(YanSemanticPosix *old,
                                             uint32_t files,
                                             uint32_t directory_blocks,
                                             uint32_t capacity_blocks,
                                             uint8_t *directory, char *error,
                                             size_t error_size)
{
    YanFsResult formatted = yan_fs_format_directory(directory, directory_blocks,
                                                    capacity_blocks);
    if (formatted != YAN_FS_OK) {
        return set_error(error, error_size, map_fs(formatted),
                         "cannot format the new directory");
    }
    uint32_t cursor = 0u;
    uint32_t next_block = directory_blocks;
    for (;;) {
        YanFsInfo info;
        YanFsResult result = yan_fs_list(&old->fs, &cursor, &info);
        if (result == YAN_FS_END) {
            break;
        }
        if (result != YAN_FS_OK) {
            return set_error(error, error_size, map_fs(result),
                             "cannot enumerate source image");
        }
        uint32_t slot = cursor - 1u;
        if (slot >= files) {
            return set_error(error, error_size, YAN_IMAGE_MIGRATE_NOSPACE,
                             "requested --files is below the highest used slot");
        }
        uint8_t *entry = directory + MIG_HEADER_BYTES +
                         (size_t)slot * MIG_ENTRY_BYTES;
        memcpy(entry, info.name, MIG_NAME_BYTES);
        store_le32(entry + MIG_SIZE_OFFSET, info.size_bytes);
        uint32_t count = blocks_for_size(info.size_bytes);
        if (count > 0u) {
            if (count > capacity_blocks - next_block) {
                return set_error(error, error_size, YAN_IMAGE_MIGRATE_NOSPACE,
                                 "target has no room for every file");
            }
            store_le32(entry + MIG_START_OFFSET, next_block);
            store_le32(entry + MIG_COUNT_OFFSET, count);
            next_block += count;
        }
    }
    store_le32(directory + MIG_CRC_OFFSET,
               yan_fs_directory_crc(directory, directory_blocks));
    return YAN_IMAGE_MIGRATE_OK;
}

static YanImageMigrateResult copy_image_payload(YanFs *old, int target,
                                                const uint8_t *directory,
                                                uint32_t capacity_blocks,
                                                uint32_t directory_blocks,
                                                char *error, size_t error_size)
{
    uint64_t bytes = 0u;
    if (!capacity_representable(capacity_blocks, &bytes) ||
        ftruncate(target, (off_t)bytes) != 0) {
        return set_error(error, error_size, YAN_IMAGE_MIGRATE_IO,
                         "cannot size the target image");
    }
    for (uint32_t block = 0u; block < directory_blocks; ++block) {
        if (!pwrite_exact(target,
                          directory + (size_t)block * YAN_FS_BLOCK_SIZE,
                          YAN_FS_BLOCK_SIZE,
                          (uint64_t)block * YAN_FS_BLOCK_SIZE)) {
            return set_error(error, error_size, YAN_IMAGE_MIGRATE_IO,
                             "cannot write the target directory");
        }
    }
    uint8_t buffer[YAN_FS_BLOCK_SIZE];
    uint32_t entries = directory_blocks * 64u - 1u;
    for (uint32_t slot = 0u; slot < entries; ++slot) {
        const uint8_t *entry = directory + MIG_HEADER_BYTES +
                               (size_t)slot * MIG_ENTRY_BYTES;
        uint32_t size = load_le32(entry + MIG_SIZE_OFFSET);
        uint32_t start = load_le32(entry + MIG_START_OFFSET);
        uint32_t count = load_le32(entry + MIG_COUNT_OFFSET);
        if (entry[0] == 0u || size == 0u) {
            continue;
        }
        const char *name = (const char *)entry;
        for (uint32_t index = 0u; index < count; ++index) {
            memset(buffer, 0, sizeof buffer);
            uint32_t offset = index * YAN_FS_BLOCK_SIZE;
            uint32_t want = size - offset;
            if (want > YAN_FS_BLOCK_SIZE) {
                want = YAN_FS_BLOCK_SIZE;
            }
            uint32_t got = 0u;
            YanFsResult read = yan_fs_read(old, name, offset, buffer, want, &got);
            if (read != YAN_FS_OK || got != want) {
                return set_error(error, error_size, map_fs(read),
                                 "cannot read source file '%s'", name);
            }
            if (!pwrite_exact(target, buffer, YAN_FS_BLOCK_SIZE,
                              ((uint64_t)start + index) * YAN_FS_BLOCK_SIZE)) {
                return set_error(error, error_size, YAN_IMAGE_MIGRATE_IO,
                                 "cannot write target file '%s'", name);
            }
        }
    }
    return YAN_IMAGE_MIGRATE_OK;
}

/* Public read-only view check: yan_fs_list yields live slots in physical order;
 * each live slot must match the built name/size and every file byte. */
static YanImageMigrateResult verify_target(YanFs *old, YanFs *fresh,
                                           const uint8_t *directory,
                                           uint32_t files, char *error,
                                           size_t error_size)
{
    uint8_t old_buffer[YAN_FS_BLOCK_SIZE];
    uint8_t new_buffer[YAN_FS_BLOCK_SIZE];
    uint32_t cursor = 0u;
    for (uint32_t slot = 0u; slot < files; ++slot) {
        const uint8_t *entry = directory + MIG_HEADER_BYTES +
                               (size_t)slot * MIG_ENTRY_BYTES;
        if (entry[0] == 0u) {
            continue;
        }
        YanFsInfo info;
        YanFsResult listed = yan_fs_list(fresh, &cursor, &info);
        if (listed != YAN_FS_OK || cursor - 1u != slot ||
            memcmp(info.name, entry, MIG_NAME_BYTES) != 0 ||
            info.size_bytes != load_le32(entry + MIG_SIZE_OFFSET)) {
            return set_error(error, error_size, YAN_IMAGE_MIGRATE_VERIFY,
                             "target slot %u does not match the source", slot);
        }
        const char *name = (const char *)entry;
        uint32_t size = info.size_bytes;
        uint64_t offset = 0u;
        while (offset < size) {
            uint32_t want = size - (uint32_t)offset;
            if (want > YAN_FS_BLOCK_SIZE) {
                want = YAN_FS_BLOCK_SIZE;
            }
            uint32_t old_got = 0u;
            uint32_t new_got = 0u;
            YanFsResult old_read = yan_fs_read(old, name, (uint32_t)offset,
                                               old_buffer, want, &old_got);
            YanFsResult new_read = yan_fs_read(fresh, name, (uint32_t)offset,
                                               new_buffer, want, &new_got);
            if (old_read != YAN_FS_OK || new_read != YAN_FS_OK ||
                old_got != want || new_got != want ||
                memcmp(old_buffer, new_buffer, want) != 0) {
                return set_error(error, error_size,
                                 YAN_IMAGE_MIGRATE_VERIFY,
                                 "file '%s' differs after migration", name);
            }
            offset += want;
        }
    }
    YanFsInfo info;
    YanFsResult listed = yan_fs_list(fresh, &cursor, &info);
    if (listed != YAN_FS_END || cursor != files) {
        return set_error(error, error_size, YAN_IMAGE_MIGRATE_VERIFY,
                         "target has an unexpected live slot");
    }
    return YAN_IMAGE_MIGRATE_OK;
}

typedef enum { ASSETS_COPY, ASSETS_VERIFY, ASSETS_REMOVE } AssetMode;

static YanImageMigrateResult close_checked(int fd, const char *what,
                                           YanImageMigrateResult result,
                                           char *error, size_t error_size)
{
    if (close(fd) != 0 && result == YAN_IMAGE_MIGRATE_OK) {
        return set_error(error, error_size, YAN_IMAGE_MIGRATE_IO,
                         "cannot close %s", what);
    }
    return result;
}

/* One byte past the known length must be a true EOF.  A signal can interrupt
 * read before it transfers anything; retry only that case so a tail byte and a
 * real read error keep their existing VERIFY classification. */
static bool probe_asset_eof(int fd)
{
    uint8_t byte;
    for (;;) {
        ssize_t got = read(fd, &byte, 1u);
        if (got < 0 && errno == EINTR) {
            continue;
        }
        return got == 0;
    }
}

static YanImageMigrateResult asset_regular(int source, int target,
                                           const char *name, AssetMode mode,
                                           char *error, size_t error_size)
{
    int in = openat(source, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (in < 0) {
        return set_error(error, error_size, YAN_IMAGE_MIGRATE_IO,
                         "cannot open source asset '%s'", name);
    }
    struct stat source_info;
    if (fstat(in, &source_info) != 0 || !S_ISREG(source_info.st_mode)) {
        (void)close(in);
        return set_error(error, error_size, YAN_IMAGE_MIGRATE_UNSUPPORTED,
                         "source asset '%s' is not a regular file", name);
    }
    YanImageMigrateResult result = YAN_IMAGE_MIGRATE_OK;
    uint8_t left[MIG_COPY_BUFFER];
    uint8_t right[MIG_COPY_BUFFER];
    if (mode == ASSETS_COPY) {
        int out = openat(target, name,
                         O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                         0600);
        if (out < 0) {
            (void)close(in);
            return set_error(error, error_size, YAN_IMAGE_MIGRATE_IO,
                             "cannot create target asset '%s'", name);
        }
        for (;;) {
            ssize_t got = read(in, left, sizeof left);
            if (got < 0 && errno == EINTR) {
                continue;
            }
            if (got < 0) {
                result = set_error(error, error_size, YAN_IMAGE_MIGRATE_IO,
                                   "cannot read source asset '%s'", name);
                break;
            }
            if (got == 0) {
                break;
            }
            if (!write_exact(out, left, (size_t)got)) {
                result = set_error(error, error_size, YAN_IMAGE_MIGRATE_IO,
                                   "cannot write target asset '%s'", name);
                break;
            }
        }
        if (result == YAN_IMAGE_MIGRATE_OK && fsync(out) != 0) {
            result = set_error(error, error_size, YAN_IMAGE_MIGRATE_IO,
                               "cannot flush target asset '%s'", name);
        }
        result = close_checked(in, "source asset", result, error, error_size);
        return close_checked(out, "target asset", result, error, error_size);
    }
    int out = openat(target, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (out < 0) {
        (void)close(in);
        return set_error(error, error_size, YAN_IMAGE_MIGRATE_VERIFY,
                         "target asset '%s' is missing", name);
    }
    struct stat target_info;
    if (fstat(out, &target_info) != 0 || !S_ISREG(target_info.st_mode) ||
        source_info.st_size != target_info.st_size) {
        result = set_error(error, error_size, YAN_IMAGE_MIGRATE_VERIFY,
                           "asset '%s' size or type differs", name);
        goto out;
    }
    uint64_t remaining = (uint64_t)source_info.st_size;
    while (remaining > 0u) {
        size_t chunk = remaining < sizeof left ? (size_t)remaining : sizeof left;
        if (!read_exact(in, left, chunk) || !read_exact(out, right, chunk) ||
            memcmp(left, right, chunk) != 0) {
            result = set_error(error, error_size, YAN_IMAGE_MIGRATE_VERIFY,
                               "asset '%s' bytes differ", name);
            goto out;
        }
        remaining -= chunk;
    }
    if (!probe_asset_eof(in) || !probe_asset_eof(out)) {
        result = set_error(error, error_size, YAN_IMAGE_MIGRATE_VERIFY,
                           "asset '%s' size changed during verification", name);
    }
out:
    result = close_checked(in, "source asset", result, error, error_size);
    return close_checked(out, "target asset", result, error, error_size);
}

static YanImageMigrateResult assets_walk(int source, int target, AssetMode mode,
                                         char *error, size_t error_size)
{
    int origin = mode == ASSETS_REMOVE ? target : source;
    int duplicate = dup(origin);
    if (duplicate < 0) {
        return set_error(error, error_size, YAN_IMAGE_MIGRATE_IO,
                         "cannot scan assets");
    }
    DIR *directory = fdopendir(duplicate);
    if (directory == NULL) {
        (void)close(duplicate);
        return set_error(error, error_size, YAN_IMAGE_MIGRATE_IO,
                         "cannot scan assets");
    }
    YanImageMigrateResult result = YAN_IMAGE_MIGRATE_OK;
    bool clean = true;
    uint64_t source_count = 0u;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (entry == NULL) {
            if (errno != 0 && result == YAN_IMAGE_MIGRATE_OK) {
                result = set_error(error, error_size, YAN_IMAGE_MIGRATE_IO,
                                   "cannot finish scanning assets");
                clean = false;
            }
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        ++source_count;
        struct stat info;
        if (fstatat(origin, entry->d_name, &info, AT_SYMLINK_NOFOLLOW) != 0) {
            result = set_error(error, error_size, YAN_IMAGE_MIGRATE_IO,
                               "cannot inspect asset '%s'", entry->d_name);
            clean = false;
            if (mode == ASSETS_REMOVE) {
                continue;
            }
            break;
        }
        if (S_ISDIR(info.st_mode)) {
            if (mode == ASSETS_REMOVE) {
                int child = openat(target, entry->d_name,
                                   O_RDONLY | O_DIRECTORY | O_NOFOLLOW |
                                       O_CLOEXEC);
                if (child < 0) {
                    clean = false;
                    continue;
                }
                if (assets_walk(child, child, mode, error, error_size) !=
                    YAN_IMAGE_MIGRATE_OK) {
                    clean = false;
                }
                if (close(child) != 0) {
                    clean = false;
                }
                if (unlinkat(target, entry->d_name, AT_REMOVEDIR) != 0 &&
                    errno != ENOENT) {
                    clean = false;
                }
                continue;
            }
            if (mode == ASSETS_COPY) {
                if (mkdirat(target, entry->d_name, 0700) != 0) {
                    result = set_error(error, error_size, YAN_IMAGE_MIGRATE_IO,
                                       "cannot create target asset directory");
                    break;
                }
            } else {
                struct stat target_info;
                if (fstatat(target, entry->d_name, &target_info,
                            AT_SYMLINK_NOFOLLOW) != 0 ||
                    !S_ISDIR(target_info.st_mode)) {
                    result = set_error(error, error_size,
                                       YAN_IMAGE_MIGRATE_VERIFY,
                                       "asset directory '%s' differs",
                                       entry->d_name);
                    break;
                }
            }
            int child_source = openat(source, entry->d_name,
                                      O_RDONLY | O_DIRECTORY | O_NOFOLLOW |
                                          O_CLOEXEC);
            int child_target = openat(target, entry->d_name,
                                      O_RDONLY | O_DIRECTORY | O_NOFOLLOW |
                                          O_CLOEXEC);
            if (child_source < 0 || child_target < 0) {
                if (child_source >= 0) {
                    (void)close(child_source);
                }
                if (child_target >= 0) {
                    (void)close(child_target);
                }
                result = set_error(error, error_size, YAN_IMAGE_MIGRATE_IO,
                                   "cannot open asset directory '%s'",
                                   entry->d_name);
                break;
            }
            result = assets_walk(child_source, child_target, mode, error,
                                 error_size);
            if (result == YAN_IMAGE_MIGRATE_OK && mode == ASSETS_COPY &&
                fsync(child_target) != 0) {
                result = set_error(error, error_size, YAN_IMAGE_MIGRATE_IO,
                                   "cannot flush target asset directory '%s'",
                                   entry->d_name);
            }
            result = close_checked(child_source, "source asset directory",
                                   result, error, error_size);
            result = close_checked(child_target, "target asset directory",
                                   result, error, error_size);
            if (result != YAN_IMAGE_MIGRATE_OK) {
                break;
            }
        } else if (S_ISREG(info.st_mode)) {
            if (mode == ASSETS_REMOVE) {
                if (unlinkat(target, entry->d_name, 0) != 0 &&
                    errno != ENOENT) {
                    clean = false;
                }
            } else {
                result = asset_regular(source, target, entry->d_name, mode,
                                       error, error_size);
                if (result != YAN_IMAGE_MIGRATE_OK) {
                    break;
                }
            }
        } else {
            if (mode == ASSETS_REMOVE) {
                if (unlinkat(target, entry->d_name, 0) != 0 &&
                    errno != ENOENT) {
                    clean = false;
                }
            } else {
                result = set_error(error, error_size,
                                   mode == ASSETS_COPY
                                       ? YAN_IMAGE_MIGRATE_UNSUPPORTED
                                       : YAN_IMAGE_MIGRATE_VERIFY,
                                   "source asset '%s' has an unsupported type",
                                   entry->d_name);
                break;
            }
        }
    }
    if (result == YAN_IMAGE_MIGRATE_OK && mode == ASSETS_VERIFY) {
        int target_duplicate = dup(target);
        DIR *target_directory = target_duplicate < 0
                                    ? NULL
                                    : fdopendir(target_duplicate);
        if (target_directory == NULL) {
            if (target_duplicate >= 0) {
                (void)close(target_duplicate);
            }
            result = set_error(error, error_size, YAN_IMAGE_MIGRATE_IO,
                               "cannot scan target assets");
        } else {
            uint64_t target_count = 0u;
            for (;;) {
                errno = 0;
                struct dirent *entry = readdir(target_directory);
                if (entry == NULL) {
                    if (errno != 0) {
                        result = set_error(error, error_size,
                                           YAN_IMAGE_MIGRATE_IO,
                                           "cannot finish scanning target assets");
                    }
                    break;
                }
                if (strcmp(entry->d_name, ".") != 0 &&
                    strcmp(entry->d_name, "..") != 0) {
                    ++target_count;
                }
            }
            if (result == YAN_IMAGE_MIGRATE_OK &&
                target_count != source_count) {
                result = set_error(error, error_size,
                                   YAN_IMAGE_MIGRATE_VERIFY,
                                   "target assets have a different entry count");
            }
            if (closedir(target_directory) != 0 &&
                result == YAN_IMAGE_MIGRATE_OK) {
                result = set_error(error, error_size, YAN_IMAGE_MIGRATE_IO,
                                   "cannot close the target asset scan");
            }
        }
    }
    if (closedir(directory) != 0 && result == YAN_IMAGE_MIGRATE_OK) {
        result = set_error(error, error_size, YAN_IMAGE_MIGRATE_IO,
                           "cannot close the asset scan");
    }
    if (mode == ASSETS_REMOVE && result == YAN_IMAGE_MIGRATE_OK && !clean) {
        result = set_error(error, error_size, YAN_IMAGE_MIGRATE_IO,
                           "cannot remove target assets");
    }
    return result;
}

static YanImageMigrateResult assets_run(const char *source_path, int parent,
                                        const char *base, AssetMode mode,
                                        dev_t device, ino_t inode, char *error,
                                        size_t error_size)
{
    int source = open(source_path,
                      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int target = openat(parent, base,
                        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat info;
    if (source < 0 || target < 0 || fstat(target, &info) != 0 ||
        (mode == ASSETS_COPY &&
         (info.st_dev != device || info.st_ino != inode))) {
        if (source >= 0) {
            (void)close(source);
        }
        if (target >= 0) {
            (void)close(target);
        }
        return set_error(error, error_size, YAN_IMAGE_MIGRATE_IO,
                         "cannot open assets");
    }
    YanImageMigrateResult result = assets_walk(source, target, mode, error,
                                               error_size);
    if (result == YAN_IMAGE_MIGRATE_OK && mode == ASSETS_COPY &&
        fsync(target) != 0) {
        result = set_error(error, error_size, YAN_IMAGE_MIGRATE_IO,
                           "cannot flush target assets");
    }
    result = close_checked(source, "source assets", result, error, error_size);
    result = close_checked(target, "target assets", result, error, error_size);
    return result;
}

static bool cleanup_assets(int parent, const char *base, dev_t device,
                           ino_t inode)
{
    int directory = openat(parent, base,
                           O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0) {
        return errno == ENOENT;
    }
    struct stat info;
    if (fstat(directory, &info) != 0 || info.st_dev != device ||
        info.st_ino != inode) {
        (void)close(directory);
        return false;
    }
    char ignored[1] = {0};
    bool clean = assets_walk(directory, directory, ASSETS_REMOVE, ignored,
                             sizeof ignored) == YAN_IMAGE_MIGRATE_OK;
    if (close(directory) != 0) {
        clean = false;
    }
    struct stat current;
    if (clean &&
        (fstatat(parent, base, &current, AT_SYMLINK_NOFOLLOW) != 0 ||
         current.st_dev != device || current.st_ino != inode)) {
        clean = false;
    }
    if (clean && unlinkat(parent, base, AT_REMOVEDIR) != 0 && errno != ENOENT) {
        clean = false;
    }
    return clean;
}

static bool unlink_image(int parent, const char *base, dev_t device,
                         ino_t inode)
{
    struct stat info;
    if (fstatat(parent, base, &info, AT_SYMLINK_NOFOLLOW) != 0) {
        return errno == ENOENT;
    }
    if (info.st_dev != device || info.st_ino != inode) {
        return false;
    }
    return unlinkat(parent, base, 0) == 0;
}

static YanImageMigrateResult digest(YanSemanticPosix *posix, uint8_t out[32],
                                    char *error, size_t error_size)
{
    YanSemanticSource source;
    yan_semantic_posix_bind_source(posix, &source);
    YanSemanticResult result = yan_semantic_source_digest(&source, out);
    if (result != YAN_SEMANTIC_OK) {
        return set_error(error, error_size, map_semantic(result),
                         "cannot compute the source identity");
    }
    return YAN_IMAGE_MIGRATE_OK;
}

#define MIG_FAIL(code, ...)                                                   \
    do {                                                                      \
        result = set_error(error, error_size, (code), __VA_ARGS__);            \
        goto fail;                                                            \
    } while (0)
#define MIG_TRY()                                                             \
    do {                                                                      \
        if (result != YAN_IMAGE_MIGRATE_OK) {                                 \
            goto fail;                                                        \
        }                                                                     \
    } while (0)

YanImageMigrateResult yan_image_migrate(const char *from, const char *to,
                                        const YanImageMigrateOptions *options,
                                        char *error, size_t error_size)
{
    if (error != NULL && error_size > 0u) {
        error[0] = '\0';
    }
    if (from == NULL || *from == '\0' || to == NULL || *to == '\0' ||
        options == NULL) {
        return set_error(error, error_size, YAN_IMAGE_MIGRATE_INVALID,
                         "invalid migration arguments");
    }
    uint32_t directory_blocks = 0u;
    if (!files_to_blocks(options->files, &directory_blocks)) {
        return set_error(error, error_size, YAN_IMAGE_MIGRATE_INVALID,
                         "invalid --files");
    }

    YanImageMigrateResult result = YAN_IMAGE_MIGRATE_OK;
    YanSemanticPosix old = {0};
    YanSemanticPosix fresh = {0};
    bool old_open = false;
    bool fresh_open = false;
    int lock_fd = -1;
    int parent = -1;
    int target_fd = -1;
    bool target_created = false;
    bool target_owned = false;
    bool assets_created = false;
    bool assets_owned = false;
    dev_t target_device = 0;
    ino_t target_inode = 0;
    dev_t assets_device = 0;
    ino_t assets_inode = 0;
    uint8_t *directory = NULL;
    char *old_real = NULL;
    char *old_assets = NULL;
    char *parent_text = NULL;
    char *base = NULL;
    char *parent_real = NULL;
    char *target_path = NULL;
    char *assets_base = NULL;
    char *assets_path = NULL;
    bool source_assets = false;

    /* Issue #136: report the syscall's own reason in the message. The migrate
     * error enum has no NOT_FOUND member, so the CODE stays IO -- changing it
     * would alter how callers branch on a failed migration -- but the message
     * now names the cause instead of saying only "cannot lock". A missing
     * source and an unwritable directory produced the same sentence before. */
    int lock_errno = 0;
    YanFsResult locked =
        yan_image_lock_errno(from, LOCK_SH, &lock_fd, &lock_errno);
    if (locked == YAN_FS_BUSY) {
        MIG_FAIL(YAN_IMAGE_MIGRATE_BUSY,
                 "source image is locked by another writer");
    }
    if (locked != YAN_FS_OK) {
        MIG_FAIL(YAN_IMAGE_MIGRATE_IO, "cannot lock the source image: %s",
                 lock_errno != 0 ? strerror(lock_errno) : "unknown error");
    }

    old_real = realpath(from, NULL);
    if (old_real == NULL) {
        MIG_FAIL(YAN_IMAGE_MIGRATE_IO, "cannot resolve the source image");
    }
    old_assets = append_suffix(old_real, ".assets");
    if (old_assets == NULL) {
        MIG_FAIL(YAN_IMAGE_MIGRATE_IO,
                 "cannot allocate the source asset path");
    }
    struct stat source_assets_info;
    if (lstat(old_assets, &source_assets_info) == 0) {
        if (!S_ISDIR(source_assets_info.st_mode)) {
            MIG_FAIL(YAN_IMAGE_MIGRATE_UNSUPPORTED,
                     "source assets is not a directory");
        }
        source_assets = true;
    } else if (errno != ENOENT) {
        MIG_FAIL(YAN_IMAGE_MIGRATE_IO, "cannot inspect source assets");
    }

    YanImageOpenResult opened = yan_semantic_posix_open_image_ex(&old, from);
    old_open = true;
    if (opened != YAN_IMAGE_OPEN_OK) {
        MIG_FAIL(map_open(opened), "cannot mount the source image");
    }
    uint64_t data_blocks = 0u;
    result = scan_source(&old, options->files, &data_blocks, error, error_size);
    MIG_TRY();

    uint32_t capacity_blocks = 0u;
    if (options->blocks_set) {
        if (options->blocks == 0u) {
            MIG_FAIL(YAN_IMAGE_MIGRATE_INVALID, "invalid --blocks");
        }
        capacity_blocks = options->blocks;
    } else {
        int64_t candidate = (int64_t)old.fs.capacity_blocks +
                            ((int64_t)directory_blocks -
                             (int64_t)old.fs.directory_blocks);
        if (candidate < 1 || candidate > (int64_t)UINT32_MAX) {
            MIG_FAIL(YAN_IMAGE_MIGRATE_INVALID,
                     "default --blocks is out of range");
        }
        capacity_blocks = (uint32_t)candidate;
    }
    uint64_t image_bytes = 0u;
    if (!capacity_representable(capacity_blocks, &image_bytes)) {
        MIG_FAIL(YAN_IMAGE_MIGRATE_INVALID,
                 "target capacity is not representable");
    }
    if (capacity_blocks < directory_blocks ||
        (directory_blocks > 1u && capacity_blocks == directory_blocks)) {
        MIG_FAIL(YAN_IMAGE_MIGRATE_NOSPACE,
                 "--blocks is too small for the directory");
    }
    if (data_blocks > (uint64_t)(capacity_blocks - directory_blocks)) {
        MIG_FAIL(YAN_IMAGE_MIGRATE_NOSPACE, "--blocks cannot hold every file");
    }

    if (!split_target(to, &parent_text, &base)) {
        MIG_FAIL(YAN_IMAGE_MIGRATE_INVALID, "invalid target path");
    }
    parent_real = realpath(parent_text, NULL);
    if (parent_real == NULL) {
        MIG_FAIL(YAN_IMAGE_MIGRATE_IO, "cannot resolve the target directory");
    }
    target_path = path_join(parent_real, base);
    assets_base = append_suffix(base, ".assets");
    if (target_path == NULL || assets_base == NULL) {
        MIG_FAIL(YAN_IMAGE_MIGRATE_IO, "cannot allocate the target path");
    }
    assets_path = path_join(parent_real, assets_base);
    if (assets_path == NULL) {
        MIG_FAIL(YAN_IMAGE_MIGRATE_IO,
                 "cannot allocate the target asset path");
    }
    if (path_is_under(target_path, old_assets) ||
        path_is_under(assets_path, old_assets) ||
        strcmp(target_path, old_real) == 0 ||
        strcmp(assets_path, old_assets) == 0) {
        MIG_FAIL(YAN_IMAGE_MIGRATE_INVALID,
                 "target overlaps the source assets");
    }
    parent = open(parent_real, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (parent < 0) {
        MIG_FAIL(YAN_IMAGE_MIGRATE_IO, "cannot open the target directory");
    }
    result = require_absent(parent, base, "target", error, error_size);
    MIG_TRY();
    result = require_absent(parent, assets_base, "target assets", error,
                            error_size);
    MIG_TRY();

    directory = calloc(directory_blocks, YAN_FS_BLOCK_SIZE);
    if (directory == NULL) {
        MIG_FAIL(YAN_IMAGE_MIGRATE_IO,
                 "cannot allocate the target directory");
    }
    result = build_directory(&old, options->files, directory_blocks,
                             capacity_blocks, directory, error, error_size);
    MIG_TRY();

    target_fd = openat(parent, base,
                       O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (target_fd < 0) {
        if (errno == EEXIST) {
            MIG_FAIL(YAN_IMAGE_MIGRATE_EXISTS, "target already exists");
        }
        MIG_FAIL(YAN_IMAGE_MIGRATE_IO, "cannot create the target image");
    }
    target_created = true;
    struct stat created;
    if (fstat(target_fd, &created) != 0) {
        MIG_FAIL(YAN_IMAGE_MIGRATE_IO,
                 "cannot inspect the new target image");
    }
    target_owned = true;
    target_device = created.st_dev;
    target_inode = created.st_ino;

    result = copy_image_payload(&old.fs, target_fd, directory, capacity_blocks,
                                directory_blocks, error, error_size);
    MIG_TRY();
    if (fsync(target_fd) != 0) {
        MIG_FAIL(YAN_IMAGE_MIGRATE_IO, "cannot flush the target image");
    }
    if (close(target_fd) != 0) {
        target_fd = -1;
        MIG_FAIL(YAN_IMAGE_MIGRATE_IO, "cannot close the target image");
    }
    target_fd = -1;

    if (source_assets) {
        if (mkdirat(parent, assets_base, 0700) != 0) {
            if (errno == EEXIST) {
                MIG_FAIL(YAN_IMAGE_MIGRATE_EXISTS,
                         "target assets already exists");
            }
            MIG_FAIL(YAN_IMAGE_MIGRATE_IO, "cannot create target assets");
        }
        assets_created = true;
        struct stat created_assets;
        if (fstatat(parent, assets_base, &created_assets,
                    AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISDIR(created_assets.st_mode)) {
            MIG_FAIL(YAN_IMAGE_MIGRATE_IO,
                     "cannot inspect the new target assets");
        }
        assets_owned = true;
        assets_device = created_assets.st_dev;
        assets_inode = created_assets.st_ino;
        result = assets_run(old_assets, parent, assets_base, ASSETS_COPY,
                            assets_device, assets_inode, error, error_size);
        MIG_TRY();
    }
    if (fsync(parent) != 0) {
        MIG_FAIL(YAN_IMAGE_MIGRATE_IO, "cannot flush the target directory");
    }

    opened = yan_semantic_posix_open_image_ex(&fresh, target_path);
    fresh_open = true;
    if (opened != YAN_IMAGE_OPEN_OK) {
        MIG_FAIL(map_open(opened), "cannot remount the target image");
    }
    if (fresh.fs.capacity_blocks != capacity_blocks ||
        fresh.fs.directory_blocks != directory_blocks ||
        fresh.fs.entry_capacity != options->files) {
        MIG_FAIL(YAN_IMAGE_MIGRATE_VERIFY,
                 "target image geometry differs from the request");
    }
    result = verify_target(&old.fs, &fresh.fs, directory, options->files,
                           error, error_size);
    MIG_TRY();
    uint8_t old_digest[32];
    uint8_t new_digest[32];
    result = digest(&old, old_digest, error, error_size);
    MIG_TRY();
    result = digest(&fresh, new_digest, error, error_size);
    MIG_TRY();
    if (memcmp(old_digest, new_digest, sizeof old_digest) != 0) {
        MIG_FAIL(YAN_IMAGE_MIGRATE_VERIFY,
                 "source identity differs after migration");
    }
    if (source_assets) {
        result = assets_run(old_assets, parent, assets_base, ASSETS_VERIFY,
                            0, 0, error, error_size);
        MIG_TRY();
    } else {
        struct stat extra_assets;
        if (fstatat(parent, assets_base, &extra_assets,
                    AT_SYMLINK_NOFOLLOW) == 0) {
            MIG_FAIL(YAN_IMAGE_MIGRATE_VERIFY,
                     "target assets appeared without a source");
        }
        if (errno != ENOENT) {
            MIG_FAIL(YAN_IMAGE_MIGRATE_IO, "cannot inspect target assets");
        }
    }

    YanSemanticResult closed = yan_semantic_posix_cleanup(&fresh);
    fresh_open = false;
    if (closed != YAN_SEMANTIC_OK) {
        MIG_FAIL(YAN_IMAGE_MIGRATE_IO,
                 "cannot close the verification source");
    }
    closed = yan_semantic_posix_cleanup(&old);
    old_open = false;
    if (closed != YAN_SEMANTIC_OK) {
        MIG_FAIL(YAN_IMAGE_MIGRATE_IO, "cannot close the source image");
    }

    if (close(lock_fd) != 0) {
        lock_fd = -1;
        MIG_FAIL(YAN_IMAGE_MIGRATE_IO, "cannot release the source lock");
    }
    lock_fd = -1;
    if (close(parent) != 0) {
        parent = -1;
        result = set_error(error, error_size, YAN_IMAGE_MIGRATE_IO,
                           "cannot close the target directory");
        parent = open(parent_real, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (parent < 0) {
            if (target_created) {
                append_error(error, error_size,
                             "; residual may remain at %s", target_path);
            }
            if (assets_created) {
                append_error(error, error_size,
                             "; residual may remain at %s", assets_path);
            }
            free(directory);
            free(old_real);
            free(old_assets);
            free(parent_text);
            free(base);
            free(parent_real);
            free(target_path);
            free(assets_base);
            free(assets_path);
            return result;
        }
        goto fail;
    }
    parent = -1;

    free(directory);
    free(old_real);
    free(old_assets);
    free(parent_text);
    free(base);
    free(parent_real);
    free(target_path);
    free(assets_base);
    free(assets_path);
    return YAN_IMAGE_MIGRATE_OK;

fail:
    if (fresh_open) {
        (void)yan_semantic_posix_cleanup(&fresh);
    }
    if (old_open) {
        (void)yan_semantic_posix_cleanup(&old);
    }
    if (target_fd >= 0) {
        (void)close(target_fd);
    }
    if (parent >= 0) {
        bool assets_clean = true;
        bool target_clean = true;
        if (assets_created) {
            assets_clean = assets_owned &&
                           cleanup_assets(parent, assets_base, assets_device,
                                          assets_inode);
        }
        if (target_created) {
            target_clean = target_owned &&
                           unlink_image(parent, base, target_device,
                                        target_inode);
        }
        if (fsync(parent) != 0) {
            append_error(error, error_size,
                         "; cleanup directory flush failed");
        }
        if (!assets_clean) {
            append_error(error, error_size,
                         "; cleanup incomplete, residual may remain at %s",
                         assets_path);
        }
        if (!target_clean) {
            append_error(error, error_size,
                         "; cleanup incomplete, residual may remain at %s",
                         target_path);
        }
        if (close(parent) != 0) {
            append_error(error, error_size,
                         "; cannot close the target directory");
        }
    }
    if (lock_fd >= 0) {
        (void)close(lock_fd);
    }
    free(directory);
    free(old_real);
    free(old_assets);
    free(parent_text);
    free(base);
    free(parent_real);
    free(target_path);
    free(assets_base);
    free(assets_path);
    if (result == YAN_IMAGE_MIGRATE_OK) {
        result = set_error(error, error_size, YAN_IMAGE_MIGRATE_IO,
                           "migration failed");
    }
    return result;
}
#undef MIG_FAIL
#undef MIG_TRY
