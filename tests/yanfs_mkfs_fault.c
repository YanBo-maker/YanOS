/* Test-only link wrappers for yan_mkfs.
 *
 * This file is never compiled into the production tool. A second binary links
 * the same yan_mkfs.c with -Wl,--wrap=fwrite,--wrap=fflush,--wrap=fclose,
 * --wrap=fseeko and this translation unit to inject exactly one Host failure
 * chosen by the environment variable YAN_TEST_MKFS_FAULT: "short-write",
 * "flush", "close", "seek" or "metadata-write". The production tool does not
 * read that variable and does not know this file exists.
 *
 * Every wrapper performs the real operation first and only then reports the
 * failure, so a "short write" really moved a partial block, a "seek" really
 * moved the position, a "metadata-write" really wrote 32 directory bytes, and a
 * "flush" or "close" failure happened after the data reached the stream. Each
 * injection prints a stable marker on stderr so the Python driver can prove the
 * fault actually fired instead of trusting the exit code. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

size_t __real_fwrite(const void *pointer, size_t size, size_t count, FILE *stream);
int __real_fflush(FILE *stream);
int __real_fclose(FILE *stream);
int __real_fseeko(FILE *stream, off_t offset, int whence);

/* Set by a successful seek in metadata-write mode, so the next full-block
 * fwrite is recognised as the directory write and not one of the zero blocks. */
static int metadata_seek_seen;

static const char *selected_fault(void)
{
    return getenv("YAN_TEST_MKFS_FAULT");
}

static void announce(const char *mode)
{
    fprintf(stderr, "YAN_TEST_MKFS_FAULT:%s\n", mode);
}

size_t __wrap_fwrite(const void *pointer, size_t size, size_t count, FILE *stream)
{
    const char *mode = selected_fault();
    if (mode != NULL && strcmp(mode, "short-write") == 0 && count > 0u) {
        size_t written = __real_fwrite(pointer, size, count - 1u, stream);
        announce("short-write");
        errno = EIO;
        return written;
    }
    if (mode != NULL && strcmp(mode, "metadata-write") == 0 && metadata_seek_seen &&
        count == 4096u) {
        /* A real short directory write: 32 bytes reach the stream and the short
         * count is what the tool sees, so the image is genuinely incomplete. */
        size_t written = __real_fwrite(pointer, size, 32u, stream);
        metadata_seek_seen = 0;
        announce("metadata-write");
        errno = EIO;
        return written;
    }
    return __real_fwrite(pointer, size, count, stream);
}

int __wrap_fseeko(FILE *stream, off_t offset, int whence)
{
    const char *mode = selected_fault();
    int result = __real_fseeko(stream, offset, whence);
    if (mode != NULL && strcmp(mode, "seek") == 0) {
        /* The real seek happened; only the reported result fails. */
        announce("seek");
        errno = EIO;
        return -1;
    }
    if (mode != NULL && strcmp(mode, "metadata-write") == 0 && result == 0) {
        metadata_seek_seen = 1;
    }
    return result;
}

int __wrap_fflush(FILE *stream)
{
    const char *mode = selected_fault();
    if (mode != NULL && strcmp(mode, "flush") == 0) {
        (void)__real_fflush(stream);
        announce("flush");
        errno = EIO;
        return EOF;
    }
    return __real_fflush(stream);
}

int __wrap_fclose(FILE *stream)
{
    const char *mode = selected_fault();
    if (mode != NULL && strcmp(mode, "close") == 0) {
        (void)__real_fclose(stream);
        announce("close");
        errno = EIO;
        return EOF;
    }
    return __real_fclose(stream);
}
