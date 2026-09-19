/* Load and store paths: widths, sign extension, endianness, alignment-safe
 * access patterns and the string routines a freestanding Guest needs. */
#include "guest.h"

/* Aligned on purpose: the corpus must never rely on an unaligned access
 * succeeding, because YanCPU traps on those by design. */
static uint8_t bytes[512] __attribute__((aligned(4)));
static uint32_t words[64];
static const char message[] = "yanos difftest corpus";

static uint32_t read_word(const uint8_t *view, size_t at)
{
    return (uint32_t)view[at] | ((uint32_t)view[at + 1] << 8) |
           ((uint32_t)view[at + 2] << 16) | ((uint32_t)view[at + 3] << 24);
}

/* A compiler is free to implement a signed narrow read as a zero-extending
 * load plus shifts, so the four narrow loads are requested explicitly. Without
 * this the corpus would leave the architectural sign extension of lb/lh
 * unobserved. */
static uint32_t load_byte_signed(const uint8_t *view, size_t at)
{
    uint32_t result;
    __asm__ volatile ("lb %0, 0(%1)" : "=r"(result) : "r"(view + at));
    return result;
}

static uint32_t load_byte_unsigned(const uint8_t *view, size_t at)
{
    uint32_t result;
    __asm__ volatile ("lbu %0, 0(%1)" : "=r"(result) : "r"(view + at));
    return result;
}

static uint32_t load_half_signed(const uint8_t *view, size_t at)
{
    uint32_t result;
    __asm__ volatile ("lh %0, 0(%1)" : "=r"(result) : "r"(view + at));
    return result;
}

static uint32_t load_half_unsigned(const uint8_t *view, size_t at)
{
    uint32_t result;
    __asm__ volatile ("lhu %0, 0(%1)" : "=r"(result) : "r"(view + at));
    return result;
}

int main(void)
{
    memset(bytes, 0, sizeof bytes);
    memset(words, 0, sizeof words);

    /* Little endian: the low byte of a word lands at the lowest address. */
    words[0] = 0x11223344u;
    guest_check(((uint8_t *)words)[0] == 0x44, 2);
    guest_check(((uint8_t *)words)[1] == 0x33, 3);
    guest_check(((uint8_t *)words)[2] == 0x22, 4);
    guest_check(((uint8_t *)words)[3] == 0x11, 5);

    /* Sign extension of byte and halfword loads. */
    const int8_t negative_byte = -2;
    const int16_t negative_half = -300;
    words[1] = (uint32_t)(uint8_t)negative_byte;
    guest_check((int32_t)(int8_t)words[1] == -2, 6);
    words[2] = (uint32_t)(uint16_t)negative_half;
    guest_check((int32_t)(int16_t)words[2] == -300, 7);
    guest_check((uint32_t)(int16_t)words[2] != (uint32_t)(uint16_t)negative_half, 8);

    /* Byte wide writes must not disturb their neighbours. */
    for (size_t at = 0; at < sizeof bytes; ++at) {
        bytes[at] = (uint8_t)(at * 7u + 3u);
    }
    bytes[100] = 0x5au;
    guest_check(bytes[99] == (uint8_t)(99u * 7u + 3u), 9);
    guest_check(bytes[101] == (uint8_t)(101u * 7u + 3u), 10);

    /* Reassembling a word from bytes matches the architectural value. */
    for (size_t at = 0; at + 4 <= sizeof bytes; at += 4) {
        const uint32_t assembled = read_word(bytes, at);
        const uint32_t direct =
            ((const uint32_t *)(const void *)bytes)[at / 4];
        guest_check(assembled == direct, 11);
    }

    /* Copying through the supplied memcpy preserves the image. */
    uint8_t copy[512];
    memcpy(copy, bytes, sizeof bytes);
    guest_check(memcmp(copy, bytes, sizeof bytes) == 0, 12);
    copy[17] ^= 0xffu;
    guest_check(memcmp(copy, bytes, sizeof bytes) != 0, 13);
    guest_check(strlen(message) == sizeof message - 1, 14);
    guest_check(message[sizeof message - 1] == '\0', 15);

    /* A word stream read back through a narrow view stays consistent. */
    for (size_t at = 0; at < sizeof words / sizeof words[0]; ++at) {
        words[at] = (uint32_t)at * 0x01010101u;
    }
    const uint8_t *view = (const uint8_t *)(const void *)words;
    for (size_t at = 0; at < sizeof words / sizeof words[0]; ++at) {
        guest_check(read_word(view, at * 4) == words[at], 16);
    }

    /* Narrow loads through a volatile view: these must be real lb/lh/lbu/lhu
     * accesses with the architectural sign extension, not register folding. */
    static int8_t signed_bytes[8];
    static int16_t signed_halves[8];
    volatile int8_t *byte_view = signed_bytes;
    volatile int16_t *half_view = signed_halves;
    volatile uint16_t *unsigned_view = (volatile uint16_t *)(void *)signed_halves;
    for (size_t at = 0; at < 8; ++at) {
        byte_view[at] = (int8_t)(-128 + (int)at * 37);
        half_view[at] = (int16_t)(-30000 + (int)at * 4321);
    }
    for (size_t at = 0; at < 8; ++at) {
        guest_check(byte_view[at] == (int8_t)(-128 + (int)at * 37), 17);
        guest_check(half_view[at] == (int16_t)(-30000 + (int)at * 4321), 18);
        guest_check(unsigned_view[at] == (uint16_t)half_view[at], 19);
        guest_check((int32_t)byte_view[at] ==
                        (int32_t)(int8_t)(-128 + (int)at * 37), 20);
    }

    /* Explicit narrow loads: sign extension is part of the architecture. */
    static uint8_t narrow[8] __attribute__((aligned(4)));
    narrow[0] = 0x00;
    narrow[1] = 0x7f;
    narrow[2] = 0x80;
    narrow[3] = 0xff;
    narrow[4] = 0x01;
    narrow[5] = 0x90;
    narrow[6] = 0x55;
    narrow[7] = 0xaa;
    guest_check(load_byte_signed(narrow, 0) == 0x00000000u, 21);
    guest_check(load_byte_signed(narrow, 1) == 0x0000007fu, 22);
    guest_check(load_byte_signed(narrow, 2) == 0xffffff80u, 23);
    guest_check(load_byte_signed(narrow, 3) == 0xffffffffu, 24);
    guest_check(load_byte_signed(narrow, 5) == 0xffffff90u, 25);
    guest_check(load_byte_unsigned(narrow, 2) == 0x00000080u, 26);
    guest_check(load_byte_unsigned(narrow, 3) == 0x000000ffu, 27);
    guest_check(load_half_signed(narrow, 2) == 0xffffff80u, 28);
    guest_check(load_half_unsigned(narrow, 2) == 0x0000ff80u, 29);
    guest_check(load_half_signed(narrow, 4) == 0xffff9001u, 30);
    guest_check(load_half_unsigned(narrow, 6) == 0x0000aa55u, 31);

    guest_finish(1);
    return 0;
}
