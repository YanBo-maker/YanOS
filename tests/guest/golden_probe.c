/* Deterministic Guest probe for the executor's golden regression.
 *
 * Its only job is to produce the same instruction stream, the same register
 * writeback and the same memory effects on every run, so that yan_run's
 * `--trace` output and `--signature` bytes can be frozen as a baseline and
 * compared byte for byte afterwards (tests/guest/run_golden.sh).
 *
 * Two properties matter more than coverage here:
 *
 *   - it touches no device at all: no UART, transport, CLINT or PLIC access.
 *     A baseline that depended on a device would also depend on whether a
 *     terminal is attached or a transport ring placed, and could not say
 *     anything about the default execution path;
 *   - it is deterministic by construction: the only input is a constant, and
 *     every check is an algebraic identity, so there is no magic expected value
 *     to recompute when the code changes. `tohost = 1` means the probe passed;
 *     a failed check reports its own code (2 and up, deliberately never 1).
 *
 * The probe is deliberately not a device or trap test: those live in
 * terminal_check.c and trap_env.c. Keep it small enough that a human can diff
 * the baseline, and keep it stable: changing this file means regenerating
 * tests/guest/golden/ (see the note there).
 */
#include "guest.h"

#define CHECK_ARITHMETIC UINT32_C(2)
#define CHECK_MEMORY UINT32_C(3)
#define CHECK_BRANCH UINT32_C(4)
#define CHECK_JUMP UINT32_C(5)
#define CHECK_FINAL UINT32_C(6)

/* Read through a volatile object so the compiler cannot fold the whole program
 * into a constant: the trace must contain real arithmetic. It lives in .bss,
 * which is part of the exported signature region, so the initial value is
 * written by the program rather than relying on RAM contents. */
static volatile uint32_t seed;

/* Working data for the load/store checks; .bss, inside the signature region.
 * Kept small on purpose: the loops below are about instruction variety, and
 * every extra repetition only lengthens the frozen trace without adding any. */
static uint32_t words[8];

/* Mixed arithmetic on a value and a round counter: multiply, rotate, xor and
 * add, so the trace exercises the M extension and both shift flavours. */
static uint32_t mix(uint32_t value, uint32_t round)
{
    value ^= round * UINT32_C(0x01000193);
    value = (value << 5) | (value >> 27);
    value += UINT32_C(0x7f4a7c15);
    return value;
}

/* Recursion: exercises the call/return path and the stack. */
static uint32_t fold_recursive(uint32_t depth, uint32_t acc)
{
    if (depth == 0) {
        return acc;
    }
    return fold_recursive(depth - 1, mix(acc, depth));
}

/* The same fold as a loop, so the recursive result has something to be checked
 * against that is not a hard-coded constant. */
static uint32_t fold_iterative(uint32_t depth, uint32_t acc)
{
    for (uint32_t round = depth; round > 0; --round) {
        acc = mix(acc, round);
    }
    return acc;
}

static void arithmetic_checks(uint32_t start, uint32_t *out)
{
    uint32_t acc = start + UINT32_C(0x0f0f0f0f);
    acc ^= start - UINT32_C(0x0f0f0f0f);      /* wraps, and unsigned wrapping is defined */
    acc = (acc << 3) | (acc >> 29);
    acc += (start * UINT32_C(2654435761));    /* mul */
    acc ^= UINT32_C(0x5a5a5a5a);

    /* Division and remainder must agree with the dividend they came from. */
    const uint32_t divisor = UINT32_C(7);
    const uint32_t quotient = start / divisor;
    const uint32_t remainder = start % divisor;
    guest_check(quotient * divisor + remainder == start, CHECK_ARITHMETIC);
    guest_check(remainder < divisor, CHECK_ARITHMETIC);

    /* Signed comparison is a different instruction from unsigned comparison. */
    const int32_t signed_start = (int32_t)start;
    guest_check(signed_start < 0, CHECK_ARITHMETIC);
    guest_check(start > UINT32_C(0x7fffffff), CHECK_ARITHMETIC);

    /* Logical right shift and arithmetic right shift must differ here. */
    const uint32_t logical = start >> 4;
    const uint32_t arithmetic = (uint32_t)(signed_start >> 4);
    guest_check(logical != arithmetic, CHECK_ARITHMETIC);
    guest_check((logical << 4) == (start & UINT32_C(0xfffffff0)), CHECK_ARITHMETIC);

    /* xor/or/and identity, and set-less-than both ways. */
    guest_check(((start ^ acc) ^ acc) == start, CHECK_ARITHMETIC);
    guest_check((start | acc) >= (start & acc), CHECK_ARITHMETIC);
    guest_check((start < acc) == (acc > start), CHECK_ARITHMETIC);

    *out = acc;
}

static void memory_checks(void)
{
    uint8_t *bytes = (uint8_t *)(void *)words;
    for (uint32_t at = 0; at < sizeof(words); ++at) {
        bytes[at] = (uint8_t)(at * 7 + 3);
    }

    /* Word reads must see what the byte stores assembled, which also pins the
     * byte order the signature baseline was taken with. */
    for (uint32_t index = 0; index < (sizeof(words) / sizeof(words[0])); ++index) {
        const uint32_t from_bytes =
            (uint32_t)bytes[index * 4] |
            ((uint32_t)bytes[index * 4 + 1] << 8) |
            ((uint32_t)bytes[index * 4 + 2] << 16) |
            ((uint32_t)bytes[index * 4 + 3] << 24);
        guest_check(words[index] == from_bytes, CHECK_MEMORY);
    }

    /* Signed and unsigned narrow loads extend the same bits differently. */
    volatile int16_t *signed_half = (volatile int16_t *)(void *)words;
    volatile uint16_t *unsigned_half = (volatile uint16_t *)(void *)words;
    volatile int8_t *signed_byte = (volatile int8_t *)(void *)words;
    volatile uint8_t *unsigned_byte = (volatile uint8_t *)(void *)words;

    signed_half[0] = (int16_t)-12345;
    guest_check(signed_half[0] == (int16_t)-12345, CHECK_MEMORY);
    guest_check(unsigned_half[0] == (uint16_t)(int16_t)-12345, CHECK_MEMORY);
    /* Loading the same half word as signed or unsigned differs by the
     * extension: 0xffffcfc7 against 0x0000cfc7. */
    guest_check((uint32_t)unsigned_half[0] < (uint32_t)(int32_t)signed_half[0],
                CHECK_MEMORY);

    signed_byte[1] = (int8_t)-7;
    guest_check(signed_byte[1] == (int8_t)-7, CHECK_MEMORY);
    guest_check(unsigned_byte[1] == (uint8_t)(int8_t)-7, CHECK_MEMORY);
    guest_check((uint32_t)(int32_t)signed_byte[1] == UINT32_C(0xfffffff9),
                CHECK_MEMORY);

    /* Store-then-load through a computed address, and pointer arithmetic. */
    const uint32_t *base = words;
    for (uint32_t index = 0; index < (sizeof(words) / sizeof(words[0])); ++index) {
        guest_check((uintptr_t)(base + index) - (uintptr_t)base == index * 4,
                    CHECK_MEMORY);
    }
}

static void branch_checks(uint32_t start)
{
    /* The same sum accumulated forwards and backwards must agree. */
    uint32_t forwards = 0;
    uint32_t backwards = 0;
    for (uint32_t round = 0; round < 8; ++round) {
        forwards += mix(start, round);
    }
    for (uint32_t round = 8; round-- > 0;) {
        backwards += mix(start, round);
    }
    guest_check(forwards == backwards, CHECK_BRANCH);

    /* A conditional chain: only the branch matching the value may run. */
    uint32_t taken = 0;
    for (uint32_t value = 0; value < 8; ++value) {
        if (value < 3) {
            taken += 1;
        } else if (value == 4) {
            taken += 10;
        } else if (value >= 6) {
            taken += 100;
        }
    }
    guest_check(taken == 3 + 10 + 200, CHECK_BRANCH);
}

static void jump_checks(uint32_t start)
{
    /* jal through a call, and jalr through a function pointer. */
    uint32_t (*const step)(uint32_t, uint32_t) = mix;
    guest_check(step(start, 1) == mix(start, 1), CHECK_JUMP);
    guest_check(fold_recursive(5, start) == fold_iterative(5, start), CHECK_JUMP);
    guest_check(mix(start, 0) != mix(start, 1), CHECK_JUMP);
}

int main(void)
{
    seed = UINT32_C(0x9e3779b9);
    const uint32_t start = seed;

    uint32_t accumulated = 0;
    arithmetic_checks(start, &accumulated);
    memory_checks();
    branch_checks(start);
    jump_checks(start);

    /* One last value derived from everything above, so a change anywhere is
     * visible in the final registers the trace records. */
    guest_check(accumulated != 0, CHECK_FINAL);
    guest_check(mix(accumulated, 3) != mix(accumulated, 4), CHECK_FINAL);

    guest_finish(1);
    return 0;
}
