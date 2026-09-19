#ifndef GUEST_H
#define GUEST_H

#include <stddef.h>
#include <stdint.h>

/* Defined by start.S; the validation tools find it through the symbol table. */
extern volatile uint32_t tohost;

/* Ending the run with 1 means "the Guest reached its final check". */
static inline void guest_finish(uint32_t code)
{
    tohost = code;
    for (;;) {
    }
}

/* Reports the first failed expectation; the code identifies the check. */
static inline void guest_check(int condition, uint32_t code)
{
    if (!condition) {
        guest_finish(code);
    }
}

/* A freestanding Guest build has no C library, so the few routines the
 * compiler may emit calls to are provided by the corpus itself. */
void *memcpy(void *destination, const void *source, size_t size);
void *memset(void *destination, int value, size_t size);
int memcmp(const void *left, const void *right, size_t size);
size_t strlen(const char *text);

#endif
