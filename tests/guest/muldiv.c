/* RV32M multiplication and division, including the corner cases C cannot
 * express (division by zero, INT32_MIN / -1, high halves of a product). */
#include "guest.h"

static uint32_t high_signed(uint32_t left, uint32_t right)
{
    uint32_t result;
    __asm__ ("mulh %0, %1, %2" : "=r"(result) : "r"(left), "r"(right));
    return result;
}

static uint32_t high_signed_unsigned(uint32_t left, uint32_t right)
{
    uint32_t result;
    __asm__ ("mulhsu %0, %1, %2" : "=r"(result) : "r"(left), "r"(right));
    return result;
}

static uint32_t high_unsigned(uint32_t left, uint32_t right)
{
    uint32_t result;
    __asm__ ("mulhu %0, %1, %2" : "=r"(result) : "r"(left), "r"(right));
    return result;
}

static uint32_t divide_signed(uint32_t left, uint32_t right)
{
    uint32_t result;
    __asm__ ("div %0, %1, %2" : "=r"(result) : "r"(left), "r"(right));
    return result;
}

static uint32_t divide_unsigned(uint32_t left, uint32_t right)
{
    uint32_t result;
    __asm__ ("divu %0, %1, %2" : "=r"(result) : "r"(left), "r"(right));
    return result;
}

static uint32_t remainder_signed(uint32_t left, uint32_t right)
{
    uint32_t result;
    __asm__ ("rem %0, %1, %2" : "=r"(result) : "r"(left), "r"(right));
    return result;
}

static uint32_t remainder_unsigned(uint32_t left, uint32_t right)
{
    uint32_t result;
    __asm__ ("remu %0, %1, %2" : "=r"(result) : "r"(left), "r"(right));
    return result;
}

int main(void)
{
    const volatile uint32_t factors[] = {0u, 1u, 2u, 3u, 7u, 0xffffu, 0x10000u,
                                         0x7fffffffu, 0x80000000u, 0xffffffffu,
                                         0x12345678u, 0xdeadbeefu};
    const uint32_t count = (uint32_t)(sizeof factors / sizeof factors[0]);

    /* The low product agrees with a full width 64-bit product, and the
     * unsigned high half is that product's upper word. */
    for (uint32_t left = 0; left < count; ++left) {
        for (uint32_t right = 0; right < count; ++right) {
            const uint32_t a = factors[left];
            const uint32_t b = factors[right];
            const uint64_t product = (uint64_t)a * (uint64_t)b;
            guest_check((uint32_t)product == (a * b), 2);
            guest_check(high_unsigned(a, b) == (uint32_t)(product >> 32), 3);
        }
    }

    /* Hand-checked signed high halves. */
    guest_check(high_signed(0x80000000u, 0x80000000u) == 0x40000000u, 5);
    guest_check(high_signed(0xffffffffu, 0xffffffffu) == 0u, 6);
    guest_check(high_signed_unsigned(0xffffffffu, 0xffffffffu) == 0xffffffffu, 7);
    guest_check(high_unsigned(0xffffffffu, 0xffffffffu) == 0xfffffffeu, 8);

    /* Truncating division identities, evaluated in wrapping 32-bit
     * arithmetic so the check itself cannot overflow. */
    for (uint32_t left = 0; left < count; ++left) {
        for (uint32_t right = 0; right < count; ++right) {
            const uint32_t a = factors[left];
            const uint32_t b = factors[right];
            const int32_t signed_a = (int32_t)a;
            const int32_t signed_b = (int32_t)b;
            if (b != 0) {
                guest_check(divide_unsigned(a, b) * b + remainder_unsigned(a, b) == a, 9);
            }
            if (b != 0 && !(a == 0x80000000u && b == 0xffffffffu)) {
                guest_check(divide_signed(a, b) * b + remainder_signed(a, b) == a, 10);
            }
            if (signed_b > 0) {
                guest_check(remainder_signed(a, b) == (uint32_t)(signed_a % signed_b), 11);
            }
        }
    }

    /* Architecturally defined division by zero. */
    guest_check(divide_unsigned(12345u, 0u) == 0xffffffffu, 12);
    guest_check(divide_signed(12345u, 0u) == 0xffffffffu, 13);
    guest_check(remainder_unsigned(12345u, 0u) == 12345u, 14);
    guest_check(remainder_signed(12345u, 0u) == 12345u, 15);
    guest_check(divide_signed(0x80000000u, 0xffffffffu) == 0x80000000u, 16);
    guest_check(remainder_signed(0x80000000u, 0xffffffffu) == 0u, 17);

    /* A dependent multiply/divide chain. */
    uint32_t accumulator = 1u;
    for (uint32_t round = 1; round <= 24; ++round) {
        accumulator = accumulator * round + 7u;
        if (accumulator > 1000000u) {
            accumulator = divide_unsigned(accumulator, 3u);
        }
    }
    guest_check(accumulator != 0u, 18);

    guest_finish(1);
    return 0;
}
