/*
 * vmc_test.h — minimal dependency-free unit test harness.
 * SPDX-License-Identifier: MIT
 */
#ifndef VMC_TEST_H
#define VMC_TEST_H

#include <stdio.h>
#include <stdlib.h>

static int test_failures = 0;
static int test_checks = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        test_checks++;                                                     \
        if (!(cond)) {                                                     \
            test_failures++;                                               \
            fprintf(stderr, "  FAIL %s:%d: %s\n",                          \
                    __FILE__, __LINE__, #cond);                            \
        }                                                                  \
    } while (0)

#define CHECK_EQ(a, b)                                                     \
    do {                                                                   \
        test_checks++;                                                     \
        long long _a = (long long)(a);                                     \
        long long _b = (long long)(b);                                     \
        if (_a != _b) {                                                    \
            test_failures++;                                               \
            fprintf(stderr, "  FAIL %s:%d: %s == %s (%lld != %lld)\n",     \
                    __FILE__, __LINE__, #a, #b, _a, _b);                   \
        }                                                                  \
    } while (0)

#define CHECK_STR_EQ(a, b)                                                 \
    do {                                                                   \
        test_checks++;                                                     \
        const char *_a = (a);                                              \
        const char *_b = (b);                                              \
        if (!_a || !_b || strcmp(_a, _b) != 0) {                           \
            test_failures++;                                               \
            fprintf(stderr, "  FAIL %s:%d: %s == %s (\"%s\" != \"%s\")\n", \
                    __FILE__, __LINE__, #a, #b, _a ? _a : "(null)",        \
                    _b ? _b : "(null)");                                   \
        }                                                                  \
    } while (0)

#define TEST_RUN(fn)                                                       \
    do {                                                                   \
        fprintf(stdout, "[ RUN  ] %s\n", #fn);                             \
        fn();                                                              \
        fprintf(stdout, "[ DONE ] %s\n", #fn);                             \
    } while (0)

#define TEST_SUMMARY()                                                     \
    do {                                                                   \
        if (test_failures == 0) {                                          \
            fprintf(stdout, "OK (%d checks)\n", test_checks);              \
            return 0;                                                      \
        } else {                                                           \
            fprintf(stdout, "FAILED (%d/%d checks)\n",                     \
                    test_failures, test_checks);                           \
            return 1;                                                      \
        }                                                                  \
    } while (0)

#endif /* VMC_TEST_H */
