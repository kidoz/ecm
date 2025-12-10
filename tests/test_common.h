#ifndef TEST_COMMON_H
#define TEST_COMMON_H

/*
 * Shared test framework macros for ECM unit tests
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test counters - must be defined in each test file */
static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name)                           \
    do {                                     \
        tests_run++;                         \
        printf("  Testing: %s ... ", #name); \
        fflush(stdout);                      \
    } while (0)

#define PASS()            \
    do {                  \
        tests_passed++;   \
        printf("PASS\n"); \
    } while (0)

#define FAIL(msg)                  \
    do {                           \
        printf("FAIL: %s\n", msg); \
    } while (0)

#define ASSERT_EQ(expected, actual)                                                \
    do {                                                                           \
        if ((expected) != (actual)) {                                              \
            printf("FAIL: expected %d, got %d\n", (int)(expected), (int)(actual)); \
            return;                                                                \
        }                                                                          \
    } while (0)

#define ASSERT_EQ_MSG(expected, actual, msg)                                                 \
    do {                                                                                     \
        if ((expected) != (actual)) {                                                        \
            printf("FAIL: %s (expected %d, got %d)\n", msg, (int)(expected), (int)(actual)); \
            return;                                                                          \
        }                                                                                    \
    } while (0)

#define ASSERT_TRUE(cond)                      \
    do {                                       \
        if (!(cond)) {                         \
            printf("FAIL: condition false\n"); \
            return;                            \
        }                                      \
    } while (0)

#define ASSERT_FALSE(cond)                    \
    do {                                      \
        if ((cond)) {                         \
            printf("FAIL: condition true\n"); \
            return;                           \
        }                                     \
    } while (0)

#define ASSERT_NOT_NULL(ptr)                   \
    do {                                       \
        if ((ptr) == nullptr) {                \
            printf("FAIL: pointer is null\n"); \
            return;                            \
        }                                      \
    } while (0)

#define ASSERT_NULL(ptr)                        \
    do {                                        \
        if ((ptr) != nullptr) {                 \
            printf("FAIL: pointer not null\n"); \
            return;                             \
        }                                       \
    } while (0)

#define ASSERT_MEM_EQ(expected, actual, size)            \
    do {                                                 \
        if (memcmp((expected), (actual), (size)) != 0) { \
            printf("FAIL: memory mismatch\n");           \
            return;                                      \
        }                                                \
    } while (0)

/* Print test suite header */
#define TEST_SUITE_BEGIN(name)          \
    do {                                \
        printf("=== %s ===\n\n", name); \
    } while (0)

/* Print test category */
#define TEST_CATEGORY(name)    \
    do {                       \
        printf("%s:\n", name); \
    } while (0)

/* Print test results */
#define TEST_SUITE_END()                                                            \
    do {                                                                            \
        printf("\n=== Results: %d/%d tests passed ===\n", tests_passed, tests_run); \
        return (tests_passed == tests_run) ? 0 : 1;                                 \
    } while (0)

#endif /* TEST_COMMON_H */
