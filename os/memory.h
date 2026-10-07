#ifndef YAN_OS_MEMORY_H
#define YAN_OS_MEMORY_H

#include <stddef.h>

/* Guest definitions live in memory.c; native callers link the C library.
 * Declaring them here keeps freestanding code off hosted-only headers. */
void *memcpy(void *destination, const void *source, size_t size);
void *memset(void *destination, int value, size_t size);
int memcmp(const void *left, const void *right, size_t size);
size_t strlen(const char *text);

#endif
