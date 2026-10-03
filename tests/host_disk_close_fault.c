/* Linked only into the test executor. Production tools have no fault switch. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

int __real_fclose(FILE *file);

int __wrap_fclose(FILE *file)
{
    const char *path = getenv("YAN_TEST_DISK_CLOSE_PATH");
    struct stat stream, image;
    const bool inject = path != NULL && fstat(fileno(file), &stream) == 0 &&
                        stat(path, &image) == 0 && stream.st_dev == image.st_dev &&
                        stream.st_ino == image.st_ino;
    const int result = __real_fclose(file);
    if (inject && result == 0) {
        fputs("TEST: disk fclose failure injected\n", stderr);
        errno = EIO;
        return EOF;
    }
    return result;
}
