/* Freestanding memory helpers for the production Guest image.
 *
 * A production build links no C library, so the routines the compiler may emit
 * calls to (aggregate copies and zeroing survive -fno-builtin; the build also
 * passes it so these loops are not turned back into calls to themselves) live
 * here. They depend on nothing outside the freestanding headers.
 *
 * 0036-R8: a copy moves 32-bit words once both ends are aligned and falls back
 * to single bytes for the unaligned head and tail. Every 4 KiB block transfer
 * and every 32-byte name move is therefore one quarter of the instructions.
 * The word type is may_alias because the byte buffers it addresses are not
 * declared as uint32_t arrays. */
#include "memory.h"
#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
typedef uint32_t yan_memory_word __attribute__((may_alias));
#else
typedef uint32_t yan_memory_word;
#endif

static void copy_forward(uint8_t *out, const uint8_t *in, size_t size)
{
    while (size > 0u && ((uintptr_t)out % 4u != 0u ||
                         (uintptr_t)in % 4u != 0u)) {
        *out++ = *in++;
        --size;
    }
    while (size >= 32u) {
        yan_memory_word *o = (yan_memory_word *)(void *)out;
        const yan_memory_word *i = (const yan_memory_word *)(const void *)in;
        o[0] = i[0];
        o[1] = i[1];
        o[2] = i[2];
        o[3] = i[3];
        o[4] = i[4];
        o[5] = i[5];
        o[6] = i[6];
        o[7] = i[7];
        out += 32u;
        in += 32u;
        size -= 32u;
    }
    while (size >= 4u) {
        *(yan_memory_word *)(void *)out =
            *(const yan_memory_word *)(const void *)in;
        out += 4u;
        in += 4u;
        size -= 4u;
    }
    while (size > 0u) {
        *out++ = *in++;
        --size;
    }
}

static void copy_backward(uint8_t *out, const uint8_t *in, size_t size)
{
    uint8_t *out_end = out + size;
    const uint8_t *in_end = in + size;
    while (size > 0u && ((uintptr_t)out_end % 4u != 0u ||
                         (uintptr_t)in_end % 4u != 0u)) {
        *--out_end = *--in_end;
        --size;
    }
    while (size >= 32u) {
        out_end -= 32u;
        in_end -= 32u;
        yan_memory_word *o = (yan_memory_word *)(void *)out_end;
        const yan_memory_word *i = (const yan_memory_word *)(const void *)in_end;
        o[7] = i[7];
        o[6] = i[6];
        o[5] = i[5];
        o[4] = i[4];
        o[3] = i[3];
        o[2] = i[2];
        o[1] = i[1];
        o[0] = i[0];
        size -= 32u;
    }
    while (size >= 4u) {
        out_end -= 4u;
        in_end -= 4u;
        *(yan_memory_word *)(void *)out_end =
            *(const yan_memory_word *)(const void *)in_end;
        size -= 4u;
    }
    while (size > 0u) {
        *--out_end = *--in_end;
        --size;
    }
}

void *memcpy(void *destination, const void *source, size_t size)
{
    copy_forward(destination, source, size);
    return destination;
}

void *memmove(void *destination, const void *source, size_t size)
{
    uint8_t *out = destination;
    const uint8_t *in = source;
    if (out == in || size == 0u) {
        return destination;
    }
    if ((uintptr_t)out < (uintptr_t)in) {
        copy_forward(out, in, size);
    } else {
        copy_backward(out, in, size);
    }
    return destination;
}

void *memset(void *destination, int value, size_t size)
{
    uint8_t *out = destination;
    const uint8_t byte = (uint8_t)value;
    while (size > 0u && (uintptr_t)out % 4u != 0u) {
        *out++ = byte;
        --size;
    }
    const yan_memory_word word =
        (yan_memory_word)byte | ((yan_memory_word)byte << 8) |
        ((yan_memory_word)byte << 16) | ((yan_memory_word)byte << 24);
    while (size >= 32u) {
        yan_memory_word *o = (yan_memory_word *)(void *)out;
        o[0] = word;
        o[1] = word;
        o[2] = word;
        o[3] = word;
        o[4] = word;
        o[5] = word;
        o[6] = word;
        o[7] = word;
        out += 32u;
        size -= 32u;
    }
    while (size >= 4u) {
        *(yan_memory_word *)(void *)out = word;
        out += 4u;
        size -= 4u;
    }
    while (size > 0u) {
        *out++ = byte;
        --size;
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
