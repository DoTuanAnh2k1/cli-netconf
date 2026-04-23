/*
 * test_common.h — Minimal test harness for cli-netconf.
 *
 * No dependencies beyond stdio/stdlib/string. Each test binary has its
 * own main() that calls TEST_CASE(...) macros. Failure prints file:line +
 * message and increments g_test_fail. Exit code = g_test_fail.
 */
#ifndef TEST_COMMON_H
#define TEST_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ANSI_RED    "\033[31m"
#define ANSI_GREEN  "\033[32m"
#define ANSI_YELLOW "\033[33m"
#define ANSI_DIM    "\033[90m"
#define ANSI_RESET  "\033[0m"

static int g_test_fail = 0;
static int g_test_run  = 0;
static const char *g_test_current = "<none>";

#define TEST_CASE(name)                                                 \
    do {                                                                \
        g_test_current = #name;                                         \
        g_test_run++;                                                   \
        int _fail_before = g_test_fail;                                 \
        fprintf(stderr, ANSI_DIM "  running: %s" ANSI_RESET "\n", #name);\
        _run_##name();                                                  \
        if (g_test_fail == _fail_before)                                \
            fprintf(stderr, ANSI_GREEN "    PASS" ANSI_RESET "\n");     \
        else                                                            \
            fprintf(stderr, ANSI_RED "    FAIL (%d)" ANSI_RESET "\n",   \
                    g_test_fail - _fail_before);                        \
    } while (0)

#define ASSERT_TRUE(cond)                                               \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, ANSI_RED "      [%s:%d] assert failed: %s"  \
                    ANSI_RESET "\n", __FILE__, __LINE__, #cond);        \
            g_test_fail++;                                              \
        }                                                               \
    } while (0)

#define ASSERT_EQ_INT(expected, actual)                                 \
    do {                                                                \
        long _e = (long)(expected);                                     \
        long _a = (long)(actual);                                       \
        if (_e != _a) {                                                 \
            fprintf(stderr, ANSI_RED "      [%s:%d] %s: expected %ld,"  \
                    " got %ld" ANSI_RESET "\n",                         \
                    __FILE__, __LINE__, #actual, _e, _a);               \
            g_test_fail++;                                              \
        }                                                               \
    } while (0)

#define ASSERT_EQ_STR(expected, actual)                                 \
    do {                                                                \
        const char *_e = (expected);                                    \
        const char *_a = (actual);                                      \
        if (!_e || !_a || strcmp(_e, _a) != 0) {                        \
            fprintf(stderr, ANSI_RED "      [%s:%d] %s:" ANSI_RESET     \
                    "\n        expected: \"%s\"\n        actual:   \"%s\"\n", \
                    __FILE__, __LINE__, #actual,                        \
                    _e ? _e : "(null)", _a ? _a : "(null)");            \
            g_test_fail++;                                              \
        }                                                               \
    } while (0)

/* Substring match. Useful for checking ANSI colored output contains certain
 * tokens without pinning to exact full output. */
#define ASSERT_CONTAINS(haystack, needle)                               \
    do {                                                                \
        const char *_h = (haystack);                                    \
        const char *_n = (needle);                                      \
        if (!_h || !_n || !strstr(_h, _n)) {                            \
            fprintf(stderr, ANSI_RED "      [%s:%d] expected string"    \
                    " to contain \"%s\"" ANSI_RESET "\n",               \
                    __FILE__, __LINE__, _n ? _n : "(null)");            \
            fprintf(stderr, "        haystack: <<<%s>>>\n",             \
                    _h ? _h : "(null)");                                \
            g_test_fail++;                                              \
        }                                                               \
    } while (0)

#define ASSERT_NOT_CONTAINS(haystack, needle)                           \
    do {                                                                \
        const char *_h = (haystack);                                    \
        const char *_n = (needle);                                      \
        if (_h && _n && strstr(_h, _n)) {                               \
            fprintf(stderr, ANSI_RED "      [%s:%d] expected string"    \
                    " to NOT contain \"%s\"" ANSI_RESET "\n",           \
                    __FILE__, __LINE__, _n);                            \
            fprintf(stderr, "        haystack: <<<%s>>>\n", _h);        \
            g_test_fail++;                                              \
        }                                                               \
    } while (0)

#define TEST_REPORT_AND_EXIT()                                          \
    do {                                                                \
        fprintf(stderr, "\n");                                          \
        if (g_test_fail == 0) {                                         \
            fprintf(stderr, ANSI_GREEN "  %d tests passed"              \
                    ANSI_RESET "\n", g_test_run);                       \
            return 0;                                                   \
        }                                                               \
        fprintf(stderr, ANSI_RED "  %d failure(s) across %d tests"      \
                ANSI_RESET "\n", g_test_fail, g_test_run);              \
        return 1;                                                       \
    } while (0)

#endif /* TEST_COMMON_H */
