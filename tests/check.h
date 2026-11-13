#ifndef TESTS_CHECK_H
#define TESTS_CHECK_H

#include <stdio.h>

static int g_checks, g_failures;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { g_failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define REPORT(name) (printf("%s: %d checks, %d failures\n", name, g_checks, g_failures), g_failures ? 1 : 0)

#endif /* TESTS_CHECK_H */
