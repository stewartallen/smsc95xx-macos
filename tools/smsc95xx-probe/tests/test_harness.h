/* SPDX-License-Identifier: GPL-2.0 */
#ifndef TEST_HARNESS_H
#define TEST_HARNESS_H

#include <stdio.h>
#include <inttypes.h>

/* Note: tests_run and tests_failed are file-scope statics. If a second test
 * translation unit is added, it will have its own private copies and TEST_REPORT()
 * will under-report. Link them as extern or move to a shared .c file. */
static int tests_run;
static int tests_failed;

#define CHECK_EQ_U32(actual, expected, what)                                   \
    do {                                                                       \
        uint32_t a_ = (uint32_t)(actual);                                       \
        uint32_t e_ = (uint32_t)(expected);                                     \
        tests_run++;                                                            \
        if (a_ != e_) {                                                         \
            tests_failed++;                                                     \
            printf("FAIL %s:%d  %s: got 0x%08" PRIX32 ", want 0x%08" PRIX32 "\n",\
                   __FILE__, __LINE__, (what), a_, e_);                         \
        }                                                                       \
    } while (0)

#define CHECK_TRUE(cond, what)                                                 \
    do {                                                                       \
        tests_run++;                                                            \
        if (!(cond)) {                                                          \
            tests_failed++;                                                     \
            printf("FAIL %s:%d  %s: expected true\n", __FILE__, __LINE__, (what));\
        }                                                                       \
    } while (0)

#define CHECK_FALSE(cond, what)                                                \
    do {                                                                       \
        tests_run++;                                                            \
        if (cond) {                                                             \
            tests_failed++;                                                     \
            printf("FAIL %s:%d  %s: expected false\n", __FILE__, __LINE__, (what));\
        }                                                                       \
    } while (0)

#define CHECK(cond, what)                                                      \
    do {                                                                       \
        tests_run++;                                                            \
        if (!(cond)) {                                                          \
            tests_failed++;                                                     \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, (what));            \
        }                                                                       \
    } while (0)

#define TEST_REPORT()                                                          \
    do {                                                                       \
        printf("%d checks, %d failed\n", tests_run, tests_failed);              \
        return tests_failed == 0 ? 0 : 1;                                       \
    } while (0)

#endif /* TEST_HARNESS_H */
