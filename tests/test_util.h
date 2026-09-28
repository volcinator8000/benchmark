#ifndef TEST_UTIL_H
#define TEST_UTIL_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0, tests_failed = 0;

#define CHECK(cond) do { tests_run++; if (!(cond)) { tests_failed++; \
    fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_STR(a, b) do { tests_run++; const char *_a = (a), *_b = (b); \
    if (!_a || strcmp(_a, _b) != 0) { tests_failed++; \
    fprintf(stderr, "  FAIL %s:%d: got \"%s\" expected \"%s\"\n", __FILE__, __LINE__, _a ? _a : "(null)", _b); } } while (0)
#define TEST_MAIN_END() do { printf("%s: %d checks, %d failed\n", __FILE__, tests_run, tests_failed); \
    return tests_failed ? 1 : 0; } while (0)
#endif
