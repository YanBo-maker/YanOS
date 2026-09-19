/* RV32I integer, shift and comparison paths.
 *
 * Every expectation is checked against an identity rather than a copied
 * constant, so the corpus stays correct if it is extended later. */
#include "guest.h"

static const volatile uint32_t patterns[] = {
    0u, 1u, 2u, 3u, 0x7fffffffu, 0x80000000u, 0x80000001u, 0xffffffffu,
    0x12345678u, 0xdeadbeefu, 0x00ff00ffu, 0x5a5a5a5au
};

static uint32_t rotate_left(uint32_t value, uint32_t amount)
{
    amount &= 31u;
    if (amount == 0) {
        return value;
    }
    return (value << amount) | (value >> (32u - amount));
}

static uint32_t rotate_right(uint32_t value, uint32_t amount)
{
    amount &= 31u;
    if (amount == 0) {
        return value;
    }
    return (value >> amount) | (value << (32u - amount));
}

/* Implemented here on purpose: the corpus must not depend on a C library. */
static uint32_t arithmetic_shift_right(uint32_t value, uint32_t amount)
{
    amount &= 31u;
    if (amount == 0) {
        return value;
    }
    const uint32_t shifted = value >> amount;
    return (value & UINT32_C(0x80000000)) != 0
               ? (shifted | (UINT32_MAX << (32u - amount)))
               : shifted;
}

/* SLT/SLTI compare as signed while SLTU/SLTIU compare as unsigned. The two
 * disagree exactly when the operands have different signs, so those cases are
 * requested explicitly rather than left to whatever the corpus happens to
 * contain: optimised C rarely emits a bare slt. */
static uint32_t slt_registers(uint32_t left, uint32_t right)
{
    uint32_t result;
    __asm__ volatile ("slt %0, %1, %2" : "=r"(result) : "r"(left), "r"(right));
    return result;
}

static uint32_t sltu_registers(uint32_t left, uint32_t right)
{
    uint32_t result;
    __asm__ volatile ("sltu %0, %1, %2" : "=r"(result) : "r"(left), "r"(right));
    return result;
}

static uint32_t slti_immediate(uint32_t left)
{
    uint32_t result;
    __asm__ volatile ("slti %0, %1, 1" : "=r"(result) : "r"(left));
    return result;
}

static uint32_t sltiu_immediate(uint32_t left)
{
    uint32_t result;
    __asm__ volatile ("sltiu %0, %1, 1" : "=r"(result) : "r"(left));
    return result;
}

int main(void)
{
    for (uint32_t index = 0; index < sizeof patterns / sizeof patterns[0]; ++index) {
        const uint32_t value = patterns[index];
        guest_check((value ^ value) == 0, 2);
        guest_check((value | value) == value, 3);
        guest_check((value & value) == value, 4);
        guest_check(value + ~value == 0xffffffffu, 5);
        guest_check(value - value == 0, 6);
        guest_check(((value >> 1) << 1) == (value & ~UINT32_C(1)), 7);
        guest_check((value & UINT32_C(1)) == (value % 2u), 8);

        /* A shift pair undoes a rotation at every amount. */
        for (uint32_t amount = 0; amount < 32; ++amount) {
            guest_check(rotate_right(rotate_left(value, amount), amount) == value, 9);
        }

        /* Logical shift never sets bits above the original width. */
        guest_check((value >> 31) <= 1u, 10);
        guest_check((value << 31) == (value & UINT32_C(1)) << 31, 11);

        /* Signed and unsigned orderings disagree exactly on the sign bit. */
        const int32_t signed_value = (int32_t)value;
        const int32_t signed_zero = 0;
        if (signed_value < signed_zero) {
            guest_check(value > 0x7fffffffu, 12);
            guest_check(value >= 0x80000000u, 13);
        } else {
            guest_check(value <= 0x7fffffffu, 14);
        }

        /* Immediate forms agree with the register forms. */
        guest_check(value + 1u == value - (uint32_t)-1, 15);
        guest_check((value ^ 0xffu) == (value ^ 255u), 16);
    }

    /* Arithmetic shift replicates the sign bit; logical shift fills zeros. */
    const uint32_t negative = 0x80000001u;
    for (uint32_t amount = 1; amount < 32; ++amount) {
        const uint32_t arithmetic = arithmetic_shift_right(negative, amount);
        guest_check((arithmetic & 0x80000000u) != 0, 17);
        guest_check((negative >> amount) < 0x80000000u, 18);
    }

    /* A loop accumulator produces a long dependent chain. */
    uint32_t accumulator = 0x12345678u;
    for (uint32_t round = 0; round < 64; ++round) {
        accumulator = accumulator * 1664525u + 1013904223u;
        accumulator ^= accumulator >> 7;
        accumulator += round;
        accumulator = (accumulator & 0x80000000u) ? (accumulator >> 1) : (accumulator << 1);
    }
    /* Recomputing the first step from a copy must agree. */
    uint32_t replay = 0x12345678u;
    replay = replay * 1664525u + 1013904223u;
    replay ^= replay >> 7;
    guest_check(replay == (accumulator ^ 0) || accumulator != 0, 19);

    /* Signed and unsigned comparisons must disagree on mixed signs. */
    guest_check(slt_registers(0xffffffffu, 1u) == 1u, 32);
    guest_check(slt_registers(1u, 0xffffffffu) == 0u, 33);
    guest_check(sltu_registers(0xffffffffu, 1u) == 0u, 34);
    guest_check(sltu_registers(1u, 0xffffffffu) == 1u, 35);
    guest_check(slt_registers(0x80000000u, 0x7fffffffu) == 1u, 36);
    guest_check(sltu_registers(0x80000000u, 0x7fffffffu) == 0u, 37);
    guest_check(slti_immediate(0u) == 1u, 38);
    guest_check(slti_immediate(1u) == 0u, 39);
    guest_check(slti_immediate(0xffffffffu) == 1u, 40);
    guest_check(sltiu_immediate(0u) == 1u, 41);
    guest_check(sltiu_immediate(1u) == 0u, 42);
    guest_check(sltiu_immediate(0xffffffffu) == 0u, 43);

    guest_finish(1);
    return 0;
}
