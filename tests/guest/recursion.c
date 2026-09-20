/* Recursion: save/restore of the link register, stack traffic and the deep
 * call chains that exercise every part of the control transfer paths. */
#include "guest.h"

static uint32_t fibonacci(uint32_t index)
{
    if (index < 2) {
        return index;
    }
    return fibonacci(index - 1) + fibonacci(index - 2);
}

static uint32_t ackermann(uint32_t left, uint32_t right)
{
    if (left == 0) {
        return right + 1;
    }
    if (right == 0) {
        return ackermann(left - 1, 1);
    }
    return ackermann(left - 1, ackermann(left, right - 1));
}

static uint32_t gcd(uint32_t left, uint32_t right)
{
    return right == 0 ? left : gcd(right, left % right);
}

static int32_t search(const int32_t *values, int32_t wanted, int32_t low, int32_t high)
{
    if (low > high) {
        return -1;
    }
    const int32_t middle = low + (high - low) / 2;
    if (values[middle] == wanted) {
        return middle;
    }
    return values[middle] < wanted ? search(values, wanted, middle + 1, high)
                                   : search(values, wanted, low, middle - 1);
}

static const int32_t sorted[] = {-100, -7, 0, 3, 5, 9, 11, 42, 99, 1000};

int main(void)
{
    guest_check(fibonacci(0) == 0, 2);
    guest_check(fibonacci(1) == 1, 3);
    guest_check(fibonacci(10) == 55, 4);
    guest_check(fibonacci(16) == 987, 5);

    guest_check(ackermann(0, 5) == 6, 6);
    guest_check(ackermann(1, 3) == 5, 7);
    guest_check(ackermann(2, 3) == 9, 8);
    guest_check(ackermann(3, 3) == 61, 9);

    guest_check(gcd(48, 18) == 6, 10);
    guest_check(gcd(1071, 462) == 21, 11);
    guest_check(gcd(17, 5) == 1, 12);

    for (int32_t index = 0; index < (int32_t)(sizeof sorted / sizeof sorted[0]); ++index) {
        guest_check(search(sorted, sorted[index], 0,
                           (int32_t)(sizeof sorted / sizeof sorted[0]) - 1) == index, 13);
    }
    guest_check(search(sorted, 4, 0, (int32_t)(sizeof sorted / sizeof sorted[0]) - 1) == -1, 14);

    /* Mutually recursive parity check. */
    uint32_t parity = 0;
    for (uint32_t value = 0; value < 20; ++value) {
        parity ^= (fibonacci(value) & 1u);
    }
    guest_check(parity <= 1u, 15);

    guest_finish(1);
    return 0;
}
