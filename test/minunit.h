#ifndef MINUNIT_H
#define MINUNIT_H

#include <assert.h>

/*
 * Based on: http://www.jera.com/techinfo/jtns/jtn002.html
 */

#define MU_ASSERT(test, message)                                               \
    do {                                                                       \
        if (!(test))                                                           \
            assert(0 && message); \
    } while (0)
#define MU_RUN_TEST(test)                                                      \
    do {                                                                       \
        char *message = test();                                                \
        mu_tests_run++;                                                        \
        if (message)                                                           \
            return message;                                                    \
    } while (0)

#define MU_TEST_CASE(name) static char *name(void)
#define MU_TEST_SUITE(name) static char *name(void)

extern int mu_tests_run;

#endif /* !MINUNIT_H */
