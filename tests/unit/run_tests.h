/** @file run_tests.h
 *  @brief shared macros for the assert-based unit test runner. */
#ifndef RUN_TESTS_H
#define RUN_TESTS_H

#include <stdio.h>

extern int g_failures;

/** Define a test function (non-static; registered from run_tests.c). */
#define TEST_CASE(fn) void fn(void)

/** @note TEST_ASSERT returns from the current test function on failure. */
#define TEST_ASSERT(cond, ...)                                                                     \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                   \
            fprintf(stderr, __VA_ARGS__);                                                          \
            fprintf(stderr, "\n");                                                                 \
            g_failures++;                                                                          \
            return;                                                                                \
        }                                                                                          \
    } while (0)

void test_register(const char* name, void (*fn)(void));

#endif /* RUN_TESTS_H */
