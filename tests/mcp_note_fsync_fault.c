/* LD_PRELOAD fault: fail only directory fsyncs so note_write completes its
 * rename and reports committed:true. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int fsync(int fd)
{
    static int (*real_fsync)(int);
    if (!real_fsync) {
        void *symbol = dlsym(RTLD_NEXT, "fsync");
        memcpy(&real_fsync, &symbol, sizeof real_fsync);
    }
    struct stat info;
    if (getenv("YAN_TEST_DIR_FSYNC_FAULT") && fstat(fd, &info) == 0 &&
        S_ISDIR(info.st_mode)) {
        errno = EIO;
        return -1;
    }
    return real_fsync(fd);
}
