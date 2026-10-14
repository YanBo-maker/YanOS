#define _POSIX_C_SOURCE 200809L
#include "semantic_posix.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static YanFsIoResult capacity(void *opaque, uint64_t *blocks)
{
    YanSemanticPosix *p = opaque;
    *blocks = p->image_size / 4096;
    return YAN_FS_IO_OK;
}
static bool exact_read(int fd, uint64_t offset, uint8_t *out, size_t n)
{
    if (offset > INT64_MAX || n > (uint64_t)INT64_MAX - offset) return false;
    while (n) {
        ssize_t got = pread(fd, out, n, (off_t)offset);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return false;
        n -= (size_t)got; out += got; offset += (uint64_t)got;
    }
    return true;
}
static YanFsIoResult read_block(void *opaque, uint32_t lba, uint8_t *bytes)
{
    YanSemanticPosix *p = opaque;
    return exact_read(p->image_fd, (uint64_t)lba * 4096, bytes, 4096) ?
        YAN_FS_IO_OK : YAN_FS_IO_ERROR;
}
static YanFsIoResult refuse_write(void *opaque, uint32_t lba, const uint8_t *bytes)
{
    (void)opaque; (void)lba; (void)bytes;
    return YAN_FS_IO_ERROR;
}
static YanSemanticResult map_fs(YanFsResult r)
{
    if (r == YAN_FS_OK) return YAN_SEMANTIC_OK;
    if (r == YAN_FS_END) return YAN_SEMANTIC_MISSING;
    if (r == YAN_FS_CORRUPT || r == YAN_FS_UNSUPPORTED) return YAN_SEMANTIC_CORRUPT;
    return YAN_SEMANTIC_IO;
}
static YanSemanticResult next_file(void *opaque, uint32_t *cursor, YanSemanticFile *out)
{
    YanSemanticPosix *p = opaque;
    YanFsInfo info;
    YanFsResult r = yan_fs_list(&p->fs, cursor, &info);
    if (r != YAN_FS_OK) return map_fs(r);
    memset(out, 0, sizeof *out);
    memcpy(out->name, info.name, 32); out->size = info.size_bytes;
    out->slot = *cursor - 1;
    return YAN_SEMANTIC_OK;
}
static YanSemanticResult read_file(void *opaque, const char *name, uint32_t offset,
                                    uint8_t *out, uint32_t n)
{
    YanSemanticPosix *p = opaque; uint32_t got = 0;
    YanFsResult r = yan_fs_read(&p->fs, name, offset, out, n, &got);
    if (r != YAN_FS_OK) return map_fs(r);
    return got == n ? YAN_SEMANTIC_OK : YAN_SEMANTIC_IO;
}
static bool aliases_input(YanSemanticPosix *p, const struct stat *st)
{
    for (unsigned i = 0; i < 3; ++i)
        if (p->input_dev[i] == st->st_dev && p->input_ino[i] == st->st_ino) return true;
    return false;
}
static YanSemanticResult hash_fd(int fd, uint8_t *out)
{
    YanSemanticSha256 hash = {0}; uint8_t bytes[16384]; uint64_t offset = 0;
    YanSemanticResult r = yan_semantic_sha256_init(&hash);
    if (r != YAN_SEMANTIC_OK) return r;
    for (;;) {
        ssize_t n = pread(fd, bytes, sizeof bytes, (off_t)offset);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return YAN_SEMANTIC_DEPENDENCY;
        if (n == 0) break;
        r = yan_semantic_sha256_update(&hash, bytes, (size_t)n);
        if (r != YAN_SEMANTIC_OK) return r;
        offset += (uint64_t)n;
    }
    return yan_semantic_sha256_final(&hash, out);
}
YanSemanticResult yan_semantic_posix_open(YanSemanticPosix *p, const char *image,
    const char *index, const char *model, const char *runner, YanSemanticIdentity *identity)
{
    if (!p || !image || !index || !model || !runner || !identity ||
        !*image || !*index || !*model || !*runner) return YAN_SEMANTIC_INVALID;
    memset(p, 0, sizeof *p); p->image_fd = p->index_fd = -1; p->index_path = index;
    const char *paths[] = {image, model, runner}; struct stat input[3], target;
    bool present[3];
    for (unsigned i = 0; i < 3; ++i) {
        present[i] = stat(paths[i], &input[i]) == 0;
        if (present[i]) {
            p->input_dev[i] = input[i].st_dev; p->input_ino[i] = input[i].st_ino;
            for (unsigned j = 0; j < i; ++j)
                if (present[j] && input[i].st_dev == input[j].st_dev &&
                    input[i].st_ino == input[j].st_ino) return YAN_SEMANTIC_INVALID;
        }
    }
    /* Alias checking precedes opening/hash/provider, including existing links. */
    if (stat(index, &target) == 0 && aliases_input(p, &target)) return YAN_SEMANTIC_INVALID;
    p->image_fd = open(image, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (p->image_fd < 0) return YAN_SEMANTIC_IO;
    if (fstat(p->image_fd, &target) != 0) return YAN_SEMANTIC_IO;
    if (!S_ISREG(target.st_mode) || target.st_size < 4096 || target.st_size % 4096 ||
        (uint64_t)target.st_size / 4096 > UINT32_MAX) return YAN_SEMANTIC_CORRUPT;
    p->image_size = (uint64_t)target.st_size;
    YanFsBlockIo io = {p, capacity, read_block, refuse_write};
    YanSemanticResult r = map_fs(yan_fs_init(&p->fs, io));
    if (r == YAN_SEMANTIC_OK) r = map_fs(yan_fs_mount(&p->fs));
    if (r != YAN_SEMANTIC_OK) return r;
    for (unsigned i = 1; i < 3; ++i) {
        int fd = open(paths[i], O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0) return YAN_SEMANTIC_DEPENDENCY;
        if (fstat(fd, &target) != 0 || !S_ISREG(target.st_mode)) {
            close(fd); return YAN_SEMANTIC_DEPENDENCY;
        }
        r = hash_fd(fd, i == 1 ? identity->model : identity->runner);
        if (close(fd) != 0 && r == YAN_SEMANTIC_OK) r = YAN_SEMANTIC_DEPENDENCY;
        if (r != YAN_SEMANTIC_OK) return r;
    }
    return YAN_SEMANTIC_OK;
}
YanSemanticResult yan_semantic_posix_close_source(void *opaque)
{
    YanSemanticPosix *p = opaque; YanSemanticResult r = YAN_SEMANTIC_OK;
    if (p->image_fd >= 0) {
        if (close(p->image_fd) != 0) r = YAN_SEMANTIC_IO;
        p->image_fd = -1;
    }
    return r;
}
static YanSemanticResult open_index(void *opaque, uint64_t *size)
{
    YanSemanticPosix *p = opaque; struct stat st;
    p->index_fd = open(p->index_path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (p->index_fd < 0) return errno == ENOENT ? YAN_SEMANTIC_MISSING : YAN_SEMANTIC_IO;
    YanSemanticResult r = YAN_SEMANTIC_OK;
    if (fstat(p->index_fd, &st) != 0) r = YAN_SEMANTIC_IO;
    else if (!S_ISREG(st.st_mode) || st.st_size < 0) r = YAN_SEMANTIC_CORRUPT;
    else if (aliases_input(p, &st)) r = YAN_SEMANTIC_INVALID;
    if (r != YAN_SEMANTIC_OK) {
        close(p->index_fd); p->index_fd = -1; return r;
    }
    *size = (uint64_t)st.st_size; return YAN_SEMANTIC_OK;
}
static YanSemanticResult read_index(void *opaque, uint64_t offset, uint8_t *out, size_t n)
{
    YanSemanticPosix *p = opaque;
    return exact_read(p->index_fd, offset, out, n) ? YAN_SEMANTIC_OK : YAN_SEMANTIC_IO;
}
static YanSemanticResult close_index(void *opaque)
{
    YanSemanticPosix *p = opaque; int fd = p->index_fd; p->index_fd = -1;
    return fd < 0 || close(fd) == 0 ? YAN_SEMANTIC_OK : YAN_SEMANTIC_IO;
}
static void discard_index(void *opaque)
{
    YanSemanticPosix *p = opaque;
    if (p->temporary) { fclose(p->temporary); p->temporary = NULL; }
    if (p->owns_temporary && p->temporary_path) unlink(p->temporary_path);
    p->owns_temporary = false;
    free(p->temporary_path); p->temporary_path = NULL;
}
static YanSemanticResult begin_index(void *opaque)
{
    YanSemanticPosix *p = opaque;
    size_t length = strlen(p->index_path);
    if (length > SIZE_MAX - 12) return YAN_SEMANTIC_INVALID;
    p->temporary_path = malloc(length + 12);
    if (!p->temporary_path) return YAN_SEMANTIC_IO;
    memcpy(p->temporary_path, p->index_path, length);
    memcpy(p->temporary_path + length, ".tmp.XXXXXX", 12);
    int fd = mkstemp(p->temporary_path);
    if (fd < 0) { discard_index(p); return YAN_SEMANTIC_IO; }
    p->owns_temporary = true;
    struct stat st;
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 || fstat(fd, &st) != 0 || aliases_input(p, &st)) {
        close(fd); discard_index(p); return YAN_SEMANTIC_IO;
    }
    p->temporary = fdopen(fd, "wb");
    if (!p->temporary) { close(fd); discard_index(p); return YAN_SEMANTIC_IO; }
    return YAN_SEMANTIC_OK;
}
static YanSemanticResult write_index(void *opaque, const uint8_t *bytes, size_t n)
{
    YanSemanticPosix *p = opaque;
    return p->temporary && fwrite(bytes, 1, n, p->temporary) == n ?
        YAN_SEMANTIC_OK : YAN_SEMANTIC_IO;
}
static YanSemanticResult finish_index(void *opaque)
{
    YanSemanticPosix *p = opaque;
    if (!p->temporary) return YAN_SEMANTIC_IO;
    bool failed = fflush(p->temporary) != 0;
    if (fclose(p->temporary) != 0) failed = true;
    p->temporary = NULL;
    return failed ? YAN_SEMANTIC_IO : YAN_SEMANTIC_OK;
}
static YanSemanticResult publish_index(void *opaque)
{
    YanSemanticPosix *p = opaque;
    if (p->temporary || !p->owns_temporary || !p->temporary_path) return YAN_SEMANTIC_IO;
    if (rename(p->temporary_path, p->index_path) != 0) return YAN_SEMANTIC_IO;
    p->owns_temporary = false;
    free(p->temporary_path); p->temporary_path = NULL;
    return YAN_SEMANTIC_OK;
}
void yan_semantic_posix_bind(YanSemanticPosix *p, YanSemanticSource *source, YanSemanticStore *store)
{
    *source = (YanSemanticSource){p, sizeof *p, next_file, read_file};
    *store = (YanSemanticStore){p, sizeof *p, open_index, read_index, close_index,
        begin_index, write_index, finish_index, publish_index, discard_index};
}
YanSemanticResult yan_semantic_posix_cleanup(YanSemanticPosix *p)
{
    YanSemanticResult r = yan_semantic_posix_close_source(p);
    YanSemanticResult index = close_index(p);
    discard_index(p);
    return r == YAN_SEMANTIC_OK ? index : r;
}
