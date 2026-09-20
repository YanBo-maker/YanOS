#include "guest.h"

void *memcpy(void *destination, const void *source, size_t size)
{
    uint8_t *out = destination;
    const uint8_t *in = source;
    for (size_t at = 0; at < size; ++at) {
        out[at] = in[at];
    }
    return destination;
}

void *memset(void *destination, int value, size_t size)
{
    uint8_t *out = destination;
    for (size_t at = 0; at < size; ++at) {
        out[at] = (uint8_t)value;
    }
    return destination;
}

int memcmp(const void *left, const void *right, size_t size)
{
    const uint8_t *a = left;
    const uint8_t *b = right;
    for (size_t at = 0; at < size; ++at) {
        if (a[at] != b[at]) {
            return a[at] < b[at] ? -1 : 1;
        }
    }
    return 0;
}

size_t strlen(const char *text)
{
    size_t length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    return length;
}
