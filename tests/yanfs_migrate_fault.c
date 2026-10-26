/* 0036-R7 fault-injection driver for the real migration library.
 *
 * This translation unit is linked only into a test executable built from a
 * frozen snapshot of the production sources:
 *
 *   cc ... tests/yanfs_migrate_fault.c <snapshot sources except tools/yan_migrate.c>
 *      -Wl,--wrap=open -Wl,--wrap=openat -Wl,--wrap=pwrite -Wl,--wrap=pread
 *      -Wl,--wrap=read -Wl,--wrap=write -Wl,--wrap=fsync -Wl,--wrap=close
 *      -Wl,--wrap=mkdirat -Wl,--wrap=unlinkat -lm
 *
 * MIG_EOF_FAULT selects one deterministic injection at the post-copy one-byte
 * EOF probe: a single or double EINTR, an extra tail byte, or EIO.  The role
 * comes from the actual OLD.assets / NEW.assets directory tree, and the exact
 * asset basename comes from MIG_EOF_ASSET; the probe must read exactly one
 * byte at offset == fstat(st_size).  The marker includes kind, role, offset,
 * size and attempt.  MIG_FAULT selects exactly one injection point.  Every injection prints a
 * FAULT line with after_new=1 once the O_EXCL target image exists, so a test
 * cannot mistake an argument failure for a post-creation cleanup failure.
 * MIG_FAULT=audit performs no injection and logs every open/openat, which the
 * read-only-source check uses to prove OLD is never opened for writing.
 * MIG_FAIL_CLEANUP=1 additionally fails the cleanup unlink of the target so
 * the G5 "explicit nonzero plus residual path" rule can be exercised.
 *
 * usage: yanfs_migrate_fault --from OLD --to NEW --files N [--blocks B]
 *
 * The driver never changes the production sources and never swallows an error:
 * the library result is printed and mapped to a nonzero exit by the CLI table.
 */
#define _XOPEN_SOURCE 700

#include "image_migrate.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FAULT_ASSET_FDS 64

static const char *g_kind = "";
static const char *g_target_base = NULL;
static const char *g_assets_base = NULL;
static bool g_target_created = false;
static bool g_assets_created = false;
static bool g_injected = false;
static bool g_fail_cleanup = false;
static bool g_fail_cleanup_assets = false;
static char *g_old_real = NULL;
static int g_old_fd = -1;
static int g_target_fd = -1;
static int g_verify_fd = -1;
static int g_parent_fd = -1;
static bool g_parent_tracked = false;
static bool g_verify_opened = false;
static ino_t g_source_assets_ino = 0;
static ino_t g_assets_ino = 0;
static const char *g_secondary = NULL;
static bool g_secondary_done = false;
static dev_t g_target_device = 0;
static ino_t g_target_inode = 0;
static uint64_t g_directory_bytes = 0;
static int g_asset_fds[FAULT_ASSET_FDS];

/* Post-copy EOF probe control.  The role is attached to the directory tree
 * rooted at the actual OLD.assets and NEW.assets paths opened by migration, so
 * the wrapper never has to guess from a global read count. */
#define EOF_TRACKED_FDS 128

typedef enum {
    EOF_ROLE_NONE = 0,
    EOF_ROLE_SOURCE,
    EOF_ROLE_TARGET
} EofRole;

typedef struct {
    int fd;
    EofRole role;
    bool directory;
    char name[256];
} EofTrackedFd;

static const char *g_eof_kind = NULL;
static const char *g_eof_asset = NULL;
static char *g_source_assets_path = NULL;
static int g_target_parent_fd = -1;
static EofTrackedFd g_eof_fds[EOF_TRACKED_FDS];
static int g_eof_attempt = 0;

static EofTrackedFd *eof_tracked(int fd)
{
    if (fd < 0) {
        return NULL;
    }
    for (size_t i = 0; i < EOF_TRACKED_FDS; ++i) {
        if (g_eof_fds[i].fd == fd) {
            return &g_eof_fds[i];
        }
    }
    return NULL;
}

static void eof_forget(int fd)
{
    EofTrackedFd *tracked = eof_tracked(fd);
    if (tracked != NULL) {
        tracked->fd = -1;
        tracked->role = EOF_ROLE_NONE;
        tracked->directory = false;
        tracked->name[0] = '\0';
    }
}

static void eof_remember(int fd, EofRole role, bool directory,
                         const char *name)
{
    if (fd < 0 || role == EOF_ROLE_NONE) {
        return;
    }
    eof_forget(fd);
    for (size_t i = 0; i < EOF_TRACKED_FDS; ++i) {
        if (g_eof_fds[i].fd < 0) {
            g_eof_fds[i].fd = fd;
            g_eof_fds[i].role = role;
            g_eof_fds[i].directory = directory;
            if (name == NULL) {
                g_eof_fds[i].name[0] = '\0';
            } else {
                (void)snprintf(g_eof_fds[i].name,
                               sizeof g_eof_fds[i].name, "%s", name);
            }
            return;
        }
    }
}

static EofRole eof_role_of(int fd)
{
    EofTrackedFd *tracked = eof_tracked(fd);
    return tracked == NULL ? EOF_ROLE_NONE : tracked->role;
}

static EofRole eof_expected_role(void)
{
    if (g_eof_kind == NULL) {
        return EOF_ROLE_NONE;
    }
    if (strncmp(g_eof_kind, "asset_eof_source_", 17u) == 0) {
        return EOF_ROLE_SOURCE;
    }
    if (strncmp(g_eof_kind, "asset_eof_target_", 17u) == 0) {
        return EOF_ROLE_TARGET;
    }
    return EOF_ROLE_NONE;
}

static const char *eof_action(void)
{
    return g_eof_kind == NULL ? "" : g_eof_kind + 17;
}

static void eof_marker(const EofTrackedFd *tracked, off_t offset, off_t size)
{
    fprintf(stderr,
            "FAULT kind=%s role=%s asset=%s phase=verify count=1 "
            "offset=%lld size=%lld attempt=%d after_new=%d\n",
            g_eof_kind,
            tracked->role == EOF_ROLE_SOURCE ? "source" : "target",
            tracked->name, (long long)offset, (long long)size,
            g_eof_attempt, g_target_created ? 1 : 0);
}

static bool eof_action_known(void)
{
    const char *action = eof_action();
    return strcmp(action, "eintr1") == 0 ||
           strcmp(action, "eintr2") == 0 ||
           strcmp(action, "eintr_tail") == 0 ||
           strcmp(action, "eintr_eio") == 0 ||
           strcmp(action, "tail") == 0 ||
           strcmp(action, "eio") == 0;
}

static void mark_new_created(int fd)
{
    struct stat info;
    if (fstat(fd, &info) != 0) {
        return;
    }
    g_target_fd = fd;
    g_target_created = true;
    g_target_device = info.st_dev;
    g_target_inode = info.st_ino;
    fprintf(stderr, "EVENT new_created inode=%llu\n",
            (unsigned long long)info.st_ino);
}

static bool is_asset_fd(int fd)
{
    for (size_t i = 0; i < FAULT_ASSET_FDS; ++i) {
        if (g_asset_fds[i] == fd) {
            return true;
        }
    }
    return false;
}

static void remember_asset_fd(int fd)
{
    for (size_t i = 0; i < FAULT_ASSET_FDS; ++i) {
        if (g_asset_fds[i] <= 0) {
            g_asset_fds[i] = fd;
            return;
        }
    }
}

static bool inject(const char *kind)
{
    if (g_kind == NULL || strcmp(g_kind, kind) != 0 || g_injected) {
        return false;
    }
    g_injected = true;
    fprintf(stderr, "FAULT kind=%s after_new=%d\n", kind,
            g_target_created ? 1 : 0);
    errno = EIO;
    return true;
}

/* ---------- wrappers ---------- */

int __real_open(const char *path, int flags, ...);
int __wrap_open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if ((flags & O_CREAT) != 0) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, mode_t);
        va_end(args);
    }
    if (g_kind != NULL && strcmp(g_kind, "audit") == 0) {
        fprintf(stderr, "EVENT open path=%s flags=%d\n", path, flags);
    }
    if (strcmp(g_kind, "verify_open") == 0 && g_target_created && !g_injected) {
        struct stat info;
        if (stat(path, &info) == 0 && info.st_dev == g_target_device &&
            info.st_ino == g_target_inode) {
            (void)inject("verify_open");
            return -1;
        }
    }
    int fd = (flags & O_CREAT) != 0 ? __real_open(path, flags, mode)
                                    : __real_open(path, flags);
    if (fd >= 0 && g_kind != NULL && strcmp(g_kind, "audit") == 0 &&
        g_old_real != NULL) {
        char *resolved = realpath(path, NULL);
        if (resolved != NULL && strcmp(resolved, g_old_real) == 0) {
            g_old_fd = fd;
            fprintf(stderr, "EVENT old_fd=%d\n", fd);
        }
        free(resolved);
    }
    if (fd >= 0 && g_eof_kind != NULL && (flags & O_DIRECTORY) != 0 &&
        g_source_assets_path != NULL &&
        strcmp(path, g_source_assets_path) == 0) {
        eof_remember(fd, EOF_ROLE_SOURCE, true, "");
    }
    if (fd >= 0 && !g_parent_tracked && (flags & O_DIRECTORY) != 0) {
        g_parent_tracked = true;
        g_parent_fd = fd;
        fprintf(stderr, "EVENT parent_opened fd=%d\n", fd);
    }
    if (fd >= 0 && g_target_created && g_verify_fd < 0) {
        struct stat info;
        if (fstat(fd, &info) == 0 && info.st_dev == g_target_device &&
            info.st_ino == g_target_inode) {
            g_verify_fd = fd;
            g_verify_opened = true;
            fprintf(stderr, "EVENT verify_opened fd=%d\n", fd);
        }
    }
    return fd;
}

int __real_openat(int dirfd, const char *name, int flags, ...);
int __wrap_openat(int dirfd, const char *name, int flags, ...)
{
    mode_t mode = 0;
    if ((flags & O_CREAT) != 0) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, mode_t);
        va_end(args);
    }
    if (g_kind != NULL && strcmp(g_kind, "audit") == 0) {
        fprintf(stderr, "EVENT openat dirfd=%d name=%s flags=%d\n", dirfd, name,
                flags);
    }
    int fd = (flags & O_CREAT) != 0
                 ? __real_openat(dirfd, name, flags, mode)
                 : __real_openat(dirfd, name, flags);
    if (fd >= 0 && g_eof_kind != NULL) {
        EofRole parent_role = eof_role_of(dirfd);
        if ((flags & O_DIRECTORY) != 0) {
            if (parent_role != EOF_ROLE_NONE) {
                eof_remember(fd, parent_role, true, "");
            } else if (dirfd == g_target_parent_fd && g_assets_base != NULL &&
                       strcmp(name, g_assets_base) == 0) {
                eof_remember(fd, EOF_ROLE_TARGET, true, "");
            }
        } else if (parent_role != EOF_ROLE_NONE) {
            eof_remember(fd, parent_role, false, name);
        }
    }
    if (fd >= 0 && g_target_base != NULL && strcmp(name, g_target_base) == 0 &&
        (flags & O_CREAT) != 0 && (flags & O_EXCL) != 0 &&
        (flags & O_NOFOLLOW) == 0 && !g_target_created) {
        mark_new_created(fd);
    } else if (fd >= 0 && g_assets_created && (flags & O_CREAT) != 0) {
        remember_asset_fd(fd);
    }
    return fd;
}

int __real_mkdirat(int dirfd, const char *name, mode_t mode);
int __wrap_mkdirat(int dirfd, const char *name, mode_t mode)
{
    int result = __real_mkdirat(dirfd, name, mode);
    if (result == 0 && g_assets_base != NULL &&
        strcmp(name, g_assets_base) == 0) {
        struct stat info;
        g_assets_created = true;
        g_target_parent_fd = dirfd;
        if (fstatat(dirfd, name, &info, AT_SYMLINK_NOFOLLOW) == 0) {
            g_assets_ino = info.st_ino;
        }
        fprintf(stderr, "EVENT assets_created\n");
    }
    return result;
}

static ino_t directory_inode(DIR *directory)
{
    int fd = dirfd(directory);
    struct stat info;
    return fd >= 0 && fstat(fd, &info) == 0 ? info.st_ino : 0;
}

struct dirent *__real_readdir(DIR *directory);
struct dirent *__wrap_readdir(DIR *directory)
{
    ino_t inode = directory_inode(directory);
    if (g_kind != NULL && strcmp(g_kind, "readdir_copy") == 0 &&
        !g_injected && !g_verify_opened && inode != 0 &&
        inode == g_source_assets_ino) {
        (void)inject("readdir_copy");
        errno = EIO;
        return NULL;
    }
    if (g_kind != NULL && strcmp(g_kind, "readdir_verify_target") == 0 &&
        !g_injected && g_verify_opened && inode != 0 &&
        inode == g_assets_ino) {
        (void)inject("readdir_verify_target");
        errno = EIO;
        return NULL;
    }
    if (g_secondary != NULL && strcmp(g_secondary, "readdir_remove") == 0 &&
        !g_secondary_done && g_target_created && g_injected &&
        inode != 0 && inode == g_assets_ino) {
        g_secondary_done = true;
        fprintf(stderr, "FAULT kind=readdir_remove after_new=%d\n",
                g_target_created ? 1 : 0);
        errno = EIO;
        return NULL;
    }
    return __real_readdir(directory);
}

ssize_t __real_pwrite(int fd, const void *bytes, size_t length, off_t offset);
ssize_t __wrap_pwrite(int fd, const void *bytes, size_t length, off_t offset)
{
    if (g_kind != NULL && strcmp(g_kind, "audit") == 0 && fd == g_old_fd) {
        fprintf(stderr, "EVENT old_write kind=pwrite\n");
    }
    if (strcmp(g_kind, "dir_write") == 0 && g_target_created &&
        fd == g_target_fd) {
        (void)inject("dir_write");
        return -1;
    }
    if (strcmp(g_kind, "data_write") == 0 && g_target_created &&
        fd == g_target_fd && (uint64_t)offset >= g_directory_bytes) {
        (void)inject("data_write");
        return -1;
    }
    ssize_t put = __real_pwrite(fd, bytes, length, offset);
    if (g_kind != NULL && strcmp(g_kind, "errno_eintr") == 0 && put > 0) {
        errno = EINTR;
    }
    return put;
}

ssize_t __real_pread(int fd, void *bytes, size_t length, off_t offset);
ssize_t __wrap_pread(int fd, void *bytes, size_t length, off_t offset)
{
    if (strcmp(g_kind, "verify_read") == 0 && g_verify_fd >= 0 &&
        fd == g_verify_fd) {
        (void)inject("verify_read");
        return -1;
    }
    return __real_pread(fd, bytes, length, offset);
}

ssize_t __real_read(int fd, void *bytes, size_t length);
ssize_t __wrap_read(int fd, void *bytes, size_t length)
{
    if (g_eof_kind != NULL && g_verify_opened && length == 1u) {
        EofTrackedFd *tracked = eof_tracked(fd);
        if (tracked != NULL && !tracked->directory &&
            tracked->role == eof_expected_role() && g_eof_asset != NULL &&
            strcmp(tracked->name, g_eof_asset) == 0) {
            struct stat info;
            off_t offset = lseek(fd, 0, SEEK_CUR);
            if (offset >= 0 && fstat(fd, &info) == 0 &&
                offset == info.st_size) {
                ++g_eof_attempt;
                const char *action = eof_action();
                if (strcmp(action, "eintr1") == 0 && g_eof_attempt == 1) {
                    eof_marker(tracked, offset, info.st_size);
                    errno = EINTR;
                    return -1;
                }
                if (strcmp(action, "eintr2") == 0 && g_eof_attempt <= 2) {
                    eof_marker(tracked, offset, info.st_size);
                    errno = EINTR;
                    return -1;
                }
                if (strcmp(action, "eintr_tail") == 0) {
                    eof_marker(tracked, offset, info.st_size);
                    if (g_eof_attempt == 1) {
                        errno = EINTR;
                        return -1;
                    }
                    ((unsigned char *)bytes)[0] = 0x5Au;
                    return 1;
                }
                if (strcmp(action, "eintr_eio") == 0) {
                    eof_marker(tracked, offset, info.st_size);
                    if (g_eof_attempt == 1) {
                        errno = EINTR;
                        return -1;
                    }
                    errno = EIO;
                    return -1;
                }
                if (strcmp(action, "tail") == 0) {
                    eof_marker(tracked, offset, info.st_size);
                    ((unsigned char *)bytes)[0] = 0x5Au;
                    return 1;
                }
                if (strcmp(action, "eio") == 0) {
                    eof_marker(tracked, offset, info.st_size);
                    errno = EIO;
                    return -1;
                }
            }
        }
    }
    return __real_read(fd, bytes, length);
}

ssize_t __real_write(int fd, const void *bytes, size_t length);
ssize_t __wrap_write(int fd, const void *bytes, size_t length)
{
    if (g_kind != NULL && strcmp(g_kind, "audit") == 0 && fd == g_old_fd) {
        fprintf(stderr, "EVENT old_write kind=write\n");
    }
    if (strcmp(g_kind, "asset_file_write") == 0 && is_asset_fd(fd)) {
        (void)inject("asset_file_write");
        return -1;
    }
    ssize_t put = __real_write(fd, bytes, length);
    if (g_kind != NULL && strcmp(g_kind, "errno_eintr") == 0 && put > 0) {
        errno = EINTR;
    }
    return put;
}

int __real_fsync(int fd);
int __wrap_fsync(int fd)
{
    if (strcmp(g_kind, "image_fsync") == 0 && g_target_created &&
        fd == g_target_fd) {
        (void)inject("image_fsync");
        return -1;
    }
    if (strcmp(g_kind, "asset_dir_fsync") == 0 && g_assets_created &&
        !g_injected) {
        struct stat info;
        if (fstat(fd, &info) == 0 && S_ISDIR(info.st_mode)) {
            (void)inject("asset_dir_fsync");
            return -1;
        }
    }
    return __real_fsync(fd);
}

int __real_close(int fd);
int __wrap_close(int fd)
{
    eof_forget(fd);
    /* Linux may already have closed the descriptor when close reports an
     * error.  Reproduce that model: perform the real close first, then
     * inject EIO for the selected descriptor. */
    int closed = __real_close(fd);
    if (closed != 0) {
        return closed;
    }
    if (strcmp(g_kind, "target_close") == 0 && g_target_created &&
        fd == g_target_fd) {
        (void)inject("target_close");
        g_target_fd = -1;
        return -1;
    }
    if (strcmp(g_kind, "parent_close") == 0 && g_parent_fd >= 0 &&
        fd == g_parent_fd) {
        (void)inject("parent_close");
        g_parent_fd = -1;
        return -1;
    }
    if (strcmp(g_kind, "verify_close") == 0 && g_verify_fd >= 0 &&
        fd == g_verify_fd) {
        (void)inject("verify_close");
        g_verify_fd = -1;
        return -1;
    }
    return 0;
}

int __real_unlinkat(int dirfd, const char *name, int flags);
int __wrap_unlinkat(int dirfd, const char *name, int flags)
{
    if (g_fail_cleanup && g_target_created && g_target_base != NULL &&
        strcmp(name, g_target_base) == 0) {
        g_fail_cleanup = false;
        fprintf(stderr, "FAULT kind=cleanup_unlink after_new=%d\n",
                g_target_created ? 1 : 0);
        errno = EACCES;
        return -1;
    }
    if (g_fail_cleanup_assets && g_assets_created && g_assets_base != NULL &&
        strcmp(name, g_assets_base) == 0) {
        g_fail_cleanup_assets = false;
        fprintf(stderr, "FAULT kind=cleanup_assets_unlink after_new=%d\n",
                g_target_created ? 1 : 0);
        errno = EACCES;
        return -1;
    }
    return __real_unlinkat(dirfd, name, flags);
}

/* ---------- driver ---------- */

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

static int exit_code(YanImageMigrateResult result)
{
    switch (result) {
    case YAN_IMAGE_MIGRATE_OK: return 0;
    case YAN_IMAGE_MIGRATE_INVALID: return 2;
    case YAN_IMAGE_MIGRATE_EXISTS: return 3;
    case YAN_IMAGE_MIGRATE_BUSY: return 4;
    case YAN_IMAGE_MIGRATE_NOSPACE: return 5;
    case YAN_IMAGE_MIGRATE_CORRUPT: return 6;
    case YAN_IMAGE_MIGRATE_UNSUPPORTED: return 7;
    case YAN_IMAGE_MIGRATE_IO: return 8;
    case YAN_IMAGE_MIGRATE_VERIFY: return 9;
    }
    return 8;
}

int main(int argc, char **argv)
{
    const char *from = NULL;
    const char *to = NULL;
    const char *files_text = NULL;
    const char *blocks_text = NULL;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--from") == 0 && i + 1 < argc && from == NULL) {
            from = argv[++i];
        } else if (strcmp(argv[i], "--to") == 0 && i + 1 < argc && to == NULL) {
            to = argv[++i];
        } else if (strcmp(argv[i], "--files") == 0 && i + 1 < argc &&
                   files_text == NULL) {
            files_text = argv[++i];
        } else if (strcmp(argv[i], "--blocks") == 0 && i + 1 < argc &&
                   blocks_text == NULL) {
            blocks_text = argv[++i];
        } else {
            fprintf(stderr, "MIGFAULT usage error\n");
            return 2;
        }
    }
    uint32_t files = 0;
    if (from == NULL || to == NULL || files_text == NULL ||
        !parse_u32(files_text, &files)) {
        fprintf(stderr, "MIGFAULT usage error\n");
        return 2;
    }
    YanImageMigrateOptions options = {0};
    options.files = files;
    if (blocks_text != NULL) {
        if (!parse_u32(blocks_text, &options.blocks)) {
            fprintf(stderr, "MIGFAULT usage error\n");
            return 2;
        }
        options.blocks_set = true;
    }

    const char *slash = strrchr(to, '/');
    g_target_base = slash == NULL ? to : slash + 1;
    size_t base_length = strlen(g_target_base);
    char *assets = malloc(base_length + sizeof ".assets");
    if (assets == NULL) {
        return 2;
    }
    memcpy(assets, g_target_base, base_length);
    memcpy(assets + base_length, ".assets", sizeof ".assets");
    g_assets_base = assets;
    g_directory_bytes = (uint64_t)((files + 1u) / 64u) * 4096u;
    for (size_t i = 0; i < FAULT_ASSET_FDS; ++i) {
        g_asset_fds[i] = -1;
    }
    const char *kind = getenv("MIG_FAULT");
    g_kind = kind == NULL ? "" : kind;
    const char *cleanup = getenv("MIG_FAIL_CLEANUP");
    g_fail_cleanup = cleanup != NULL && strcmp(cleanup, "1") == 0;
    const char *cleanup_assets = getenv("MIG_FAIL_CLEANUP_ASSETS");
    g_fail_cleanup_assets = cleanup_assets != NULL &&
                            strcmp(cleanup_assets, "1") == 0;
    const char *secondary = getenv("MIG_SECONDARY");
    g_secondary = secondary == NULL ? NULL : secondary;
    for (size_t i = 0; i < EOF_TRACKED_FDS; ++i) {
        g_eof_fds[i].fd = -1;
    }
    const char *eof_kind = getenv("MIG_EOF_FAULT");
    g_eof_kind = eof_kind == NULL || *eof_kind == '\0' ? NULL : eof_kind;
    g_eof_asset = getenv("MIG_EOF_ASSET");
    if (g_eof_kind != NULL &&
        (g_eof_asset == NULL || *g_eof_asset == '\0')) {
        fprintf(stderr, "MIGFAULT missing MIG_EOF_ASSET\n");
        return 2;
    }
    if (g_eof_kind != NULL &&
        (eof_expected_role() == EOF_ROLE_NONE || !eof_action_known())) {
        fprintf(stderr, "MIGFAULT unknown MIG_EOF_FAULT=%s\n", g_eof_kind);
        return 2;
    }
    if (strcmp(g_kind, "audit") == 0) {
        g_old_real = realpath(from, NULL);
    }
    char *old_real = realpath(from, NULL);
    if (old_real != NULL) {
        size_t old_len = strlen(old_real);
        char *source_assets = malloc(old_len + sizeof ".assets");
        if (source_assets != NULL) {
            memcpy(source_assets, old_real, old_len);
            memcpy(source_assets + old_len, ".assets", sizeof ".assets");
            struct stat info;
            if (lstat(source_assets, &info) == 0 && S_ISDIR(info.st_mode)) {
                g_source_assets_ino = info.st_ino;
            }
            g_source_assets_path = source_assets;
        }
        free(old_real);
    }
    fprintf(stderr, "MIGFAULT start kind=%s eof_kind=%s\n", g_kind,
            g_eof_kind == NULL ? "" : g_eof_kind);

    char error[512];
    YanImageMigrateResult result =
        yan_image_migrate(from, to, &options, error, sizeof error);
    fprintf(stderr, "MIGFAULT result=%d error=%s\n", (int)result, error);
    free(g_old_real);
    free(g_source_assets_path);
    free(assets);
    if (result == YAN_IMAGE_MIGRATE_OK) {
        return 0;
    }
    return exit_code(result);
}
