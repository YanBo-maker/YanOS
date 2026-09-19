/* Control transfer paths: conditional branches, integer loops, switch
 * dispatch and indirect calls through function pointers. */
#include "guest.h"

static uint32_t classify(int32_t value)
{
    if (value < -1000) {
        return 1;
    }
    if (value < 0) {
        return 2;
    }
    if (value == 0) {
        return 3;
    }
    if (value < 1000) {
        return 4;
    }
    return 5;
}

static uint32_t add(uint32_t left, uint32_t right)
{
    return left + right;
}

static uint32_t subtract(uint32_t left, uint32_t right)
{
    return left - right;
}

static uint32_t multiply(uint32_t left, uint32_t right)
{
    return left * right;
}

static uint32_t dispatch(uint32_t operation, uint32_t left, uint32_t right)
{
    switch (operation) {
    case 0:
        return add(left, right);
    case 1:
        return subtract(left, right);
    case 2:
        return multiply(left, right);
    default:
        return 0;
    }
}

/* JALR must clear bit zero of the computed target. The target below is odd on
 * purpose: a model that keeps the bit fetches at an unaligned address and the
 * instruction stream diverges, while a correct model lands on `1:`. */
static uint32_t odd_jalr_target(void)
{
    uint32_t result = 0;
    __asm__ volatile ("la t0, 1f\n\t"
                      "ori t0, t0, 1\n\t"
                      "jalr ra, t0, 0\n\t"
                      "li %0, 111\n\t"
                      "j 2f\n\t"
                      "1:\n\t"
                      "li %0, 0x5a\n\t"
                      "2:\n\t"
                      : "+r"(result) : : "t0", "ra");
    return result;
}

int main(void)
{
    const volatile int32_t samples[] = {-5000, -1000, -999, -1, 0, 1, 999, 1000, 5000};
    const uint32_t expected[] = {1, 2, 2, 2, 3, 4, 4, 5, 5};
    for (uint32_t index = 0; index < sizeof samples / sizeof samples[0]; ++index) {
        guest_check(classify(samples[index]) == expected[index], 2);
    }

    /* Nested loops with a signed counter and a step of two. */
    uint32_t total = 0;
    for (int32_t outer = 10; outer > 0; outer -= 2) {
        for (int32_t inner = 0; inner < outer; ++inner) {
            total += (uint32_t)inner;
        }
    }
    guest_check(total == 95, 3);

    /* Wrapping counter: an unsigned increment crossing zero. */
    uint32_t wrapped = 0;
    for (uint32_t value = 0xfffffffeu; value != 3u; value += 1u) {
        wrapped += 1u;
    }
    guest_check(wrapped == 5u, 4);

    /* Indirect calls exercise jalr with several targets. */
    uint32_t (*const table[])(uint32_t, uint32_t) = {add, subtract, multiply};
    for (uint32_t operation = 0; operation < 3; ++operation) {
        guest_check(dispatch(operation, 12, 5) == table[operation](12, 5), 5);
    }
    guest_check(dispatch(7, 12, 5) == 0, 6);

    /* Early exit through a loop guard. */
    uint32_t found = 0;
    for (uint32_t value = 0; value < 1000; ++value) {
        if (value * value > 500) {
            found = value;
            break;
        }
    }
    guest_check(found == 23, 7);

    /* Do-while with an unsigned comparison. */
    uint32_t count = 0;
    do {
        count += 3;
    } while (count < 20u);
    guest_check(count == 21, 8);

    guest_check(odd_jalr_target() == 0x5au, 9);

    guest_finish(1);
    return 0;
}
