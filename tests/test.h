#ifndef YAN_TEST_H
#define YAN_TEST_H

#include <stdio.h>
#include <stdlib.h>

/* Keep checks active in Release builds, where assert may be disabled. */
#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);   \
            return EXIT_FAILURE;                                             \
        }                                                                    \
    } while (0)

#define RUN_TEST(test)                                                        \
    do {                                                                     \
        if (test() != EXIT_SUCCESS) {                                         \
            return EXIT_FAILURE;                                             \
        }                                                                    \
        puts(#test ": PASS");                                                \
    } while (0)

#endif
