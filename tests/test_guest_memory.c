#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

/* These names belong to the test-only compilation of the actual Guest source.
 * They deliberately do not replace the host C library's memory routines. */
void *yan_guest_memcpy(void *, const void *, size_t);
void *yan_guest_memmove(void *, const void *, size_t);
void *yan_guest_memset(void *, int, size_t);

#define BUFFER 8256u
static unsigned char actual[BUFFER], expected[BUFFER], source[BUFFER], saved[BUFFER];
static uint64_t memcpy_cases, memmove_cases, memset_cases;

static void fill_buffers(size_t span)
{
    volatile unsigned char *a = actual, *e = expected, *s = source;
    for (size_t i = 0; i < span; ++i) {
        unsigned char value = (unsigned char)((i * 67u + i / 7u + 31u) & 255u);
        a[i] = value; e[i] = value; s[i] = (unsigned char)(value ^ 0xb9u);
    }
}

static void check_bytes(size_t span, const char *operation, size_t n, size_t src, size_t dst)
{
    for (size_t i = 0; i < span; ++i) {
        if (actual[i] != expected[i]) {
            fprintf(stderr, "FAIL %s n=%zu src=%zu dst=%zu byte=%zu got=%u expected=%u\n",
                    operation, n, src, dst, i, (unsigned)actual[i], (unsigned)expected[i]);
            exit(1);
        }
    }
}

static void copy_case(size_t n, size_t src, size_t dst, int move)
{
    size_t span = (src > dst ? src : dst) + n + 16u;
    if (span < 96u) span = 96u;
    if (span > BUFFER) { fputs("FAIL probe span\n", stderr); exit(1); }
    fill_buffers(span);
    volatile unsigned char *snapshot = saved, *oracle = expected;
    const volatile unsigned char *read_from = move ? expected : source;
    /* Snapshot oracle avoids repeating the implementation's direction/word
     * logic. Volatile byte accesses keep this separate from libc lowering. */
    for (size_t i = 0; i < n; ++i) snapshot[i] = read_from[src + i];
    for (size_t i = 0; i < n; ++i) oracle[dst + i] = snapshot[i];
    void *result = move ? yan_guest_memmove(actual + dst, actual + src, n)
                        : yan_guest_memcpy(actual + dst, source + src, n);
    if (result != actual + dst) { fputs("FAIL copy return\n", stderr); exit(1); }
    check_bytes(span, move ? "memmove" : "memcpy", n, src, dst);
    if (move) ++memmove_cases; else ++memcpy_cases;
}

static void set_case(size_t n, size_t dst, int value)
{
    size_t span = dst + n + 16u;
    if (span < 96u) span = 96u;
    fill_buffers(span);
    volatile unsigned char *oracle = expected;
    for (size_t i = 0; i < n; ++i) oracle[dst + i] = (unsigned char)value;
    if (yan_guest_memset(actual + dst, value, n) != actual + dst) {
        fputs("FAIL memset return\n", stderr); exit(1);
    }
    check_bytes(span, "memset", n, 0, dst);
    ++memset_cases;
}

static void pointer_case(void)
{
    unsigned char *a = malloc(32u), *b = malloc(32u);
    if (!a || !b) exit(2);
    for (unsigned i = 0; i < 32u; ++i) a[i] = (unsigned char)i;
    yan_guest_memmove(b, a, 32u);
    for (unsigned i = 0; i < 32u; ++i) if (a[i] != b[i]) exit(1);
    free(a); free(b);
    puts("PASS independent-object memmove");
}

int main(void)
{
    for (size_t n = 0; n <= 256u; ++n) {
        for (size_t src = 0; src < 16u; ++src)
            for (size_t dst = 0; dst < 16u; ++dst)
                copy_case(n, 16u + src, 16u + dst, 1);
        for (size_t src = 0; src < 8u; ++src)
            for (size_t dst = 0; dst < 8u; ++dst)
                copy_case(n, 16u + src, 16u + dst, 0);
        const int values[] = {-1, 0, 1, 127, 128, 255, 256, 0x12345678};
        for (size_t dst = 0; dst < 8u; ++dst)
            for (size_t v = 0; v < sizeof values / sizeof values[0]; ++v)
                set_case(n, 16u + dst, values[v]);
    }
    const size_t large[] = {257u, 511u, 512u, 513u, 1023u, 1024u, 1025u, 4095u, 4096u, 4097u, 8191u, 8192u};
    for (size_t k = 0; k < sizeof large / sizeof large[0]; ++k)
        for (size_t src = 0; src < 8u; ++src)
            for (size_t dst = 0; dst < 8u; ++dst) {
                copy_case(large[k], 16u + src, 16u + dst, 1);
                copy_case(large[k], 16u + src, 16u + dst, 0);
                set_case(large[k], 16u + dst, -1);
            }
    /* Explicit zero-length one-past arguments; all accesses must be skipped. */
    if (yan_guest_memcpy(actual + BUFFER, source + BUFFER, 0) != actual + BUFFER ||
        yan_guest_memmove(actual + BUFFER, source + BUFFER, 0) != actual + BUFFER ||
        yan_guest_memset(actual + BUFFER, 3, 0) != actual + BUFFER) exit(1);
    pointer_case();
    printf("PASS Guest memory: memcpy=%" PRIu64 " memmove=%" PRIu64 " memset=%" PRIu64 "\n",
           memcpy_cases, memmove_cases, memset_cases);
    return 0;
}
