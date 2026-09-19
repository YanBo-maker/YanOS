#ifndef YAN_HOST_FILE_H
#define YAN_HOST_FILE_H

#include <stddef.h>
#include <stdint.h>

/* Reads a whole file into a newly allocated buffer. Returns NULL on any
 * failure; the caller owns the buffer and frees it. */
uint8_t *yan_host_read_file(const char *path, size_t *size);

#endif
