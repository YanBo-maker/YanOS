/* 0036-R7 F2: fault wrapper around the real tools/yan_migrate.c main.
 *
 * This file deliberately has no main(). It is linked with the unmodified
 * production CLI and host/image_migrate.c:
 *
 *   cc ... tests/yanfs_migrate_cli_fault.c tools/yan_migrate.c \
 *      host/image_migrate.c <deps> -Wl,--wrap=open -Wl,--wrap=close \
 *      -Wl,--wrap=unlinkat
 *
 * YANOS_CLI_FAULT=target-unlink:
 *   the final parent close really closes fd, then reports EIO; the parent
 *   reopens successfully; the target unlink fails. The CLI must keep exit 8
 *   and print the full residual NEW path (731-byte legal path in the test).
 *
 * YANOS_CLI_FAULT=parent-reopen:
 *   the final parent close really closes fd, then reports EIO; the parent
 *   reopen and both cleanup unlinks fail. NEW and NEW.assets stay; the CLI
 *   must print both full residual paths.
 *
 * YANOS_CLI_TARGET_BASE / YANOS_CLI_ASSETS_BASE carry the exact basenames so
 * the wrapper can fail only this run's objects.
 */
#define _XOPEN_SOURCE 700

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *g_mode;
static const char *g_target_base;
static const char *g_assets_base;
static int g_parent_fd = -1;
static bool g_parent_tracked;
static bool g_parent_closed;

static void load(void)
{
    if (g_mode == NULL) {
        g_mode = getenv("YANOS_CLI_FAULT");
        g_target_base = getenv("YANOS_CLI_TARGET_BASE");
        g_assets_base = getenv("YANOS_CLI_ASSETS_BASE");
    }
}

static bool mode_is(const char *text)
{
    load();
    return g_mode != NULL && strcmp(g_mode, text) == 0;
}

int __real_open(const char *path, int flags, ...);
int __wrap_open(const char *path, int flags, ...)
{
    load();
    mode_t mode = 0;
    if ((flags & O_CREAT) != 0) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, mode_t);
        va_end(args);
    }
    if (mode_is("parent-reopen") && g_parent_closed &&
        (flags & O_DIRECTORY) != 0) {
        fprintf(stderr, "EVENT parent-reopen-failed path=%s\n", path);
        errno = EIO;
        return -1;
    }
    int fd = (flags & O_CREAT) != 0 ? __real_open(path, flags, mode)
                                    : __real_open(path, flags);
    if (fd >= 0 && !g_parent_tracked && (flags & O_DIRECTORY) != 0) {
        g_parent_tracked = true;
        g_parent_fd = fd;
        fprintf(stderr, "EVENT parent-fd=%d\n", fd);
    }
    return fd;
}

int __real_close(int fd);
int __wrap_close(int fd)
{
    load();
    int closed = __real_close(fd);
    if (closed == 0 && g_parent_fd >= 0 && fd == g_parent_fd &&
        !g_parent_closed) {
        g_parent_closed = true;
        fprintf(stderr, "EVENT parent-close-eio\n");
        errno = EIO;
        return -1;
    }
    return closed;
}

int __real_unlinkat(int dirfd, const char *name, int flags);
int __wrap_unlinkat(int dirfd, const char *name, int flags)
{
    load();
    bool target = g_target_base != NULL && strcmp(name, g_target_base) == 0;
    bool assets = g_assets_base != NULL && strcmp(name, g_assets_base) == 0;
    if ((target && (mode_is("target-unlink") || mode_is("parent-reopen"))) ||
        (assets && mode_is("parent-reopen"))) {
        fprintf(stderr, "EVENT unlink-failed name=%s\n", name);
        errno = EACCES;
        return -1;
    }
    return __real_unlinkat(dirfd, name, flags);
}
