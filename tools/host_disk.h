#ifndef YAN_HOST_DISK_H
#define YAN_HOST_DISK_H

#include <stdio.h>
#include "host_block.h"

typedef struct {
    FILE *file;
    uint64_t bytes;
    bool failed;
} YanHostDisk;

/* Open an existing, nonempty regular image in update mode. Never create or
 * truncate it. Single-writer use only; the owner must not modify the image
 * concurrently. This C17 backend rejects sizes not representable by long. */
bool yan_host_disk_open(YanHostDisk *disk, const char *path);
/* Close errors are Host errors. Safe on an empty object. */
bool yan_host_disk_close(YanHostDisk *disk);
YanHostBlockBackend yan_host_disk_backend(YanHostDisk *disk);
/* Protect an attached image from trace/signature output aliases. */
bool yan_host_disk_same_file(const YanHostDisk *disk, const char *path);

#endif
