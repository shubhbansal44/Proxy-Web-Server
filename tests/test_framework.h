#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int tests_run;
extern int tests_passed;
extern int tests_failed;
extern const char* current_test_name;

#define TEST_SUITE(name) \
    current_test_name = #name; \
    printf("Running test suite: %s\n", current_test_name);

#define TEST_ASSERT_EQ(expected, actual) \
    do { \
        tests_run++; \
        long long e = (long long)(expected); \
        long long a = (long long)(actual); \
        if (e == a) { \
            tests_passed++; \
        } else { \
            tests_failed++; \
            printf("FAIL: %s:%d: expected %lld, got %lld\n", __FILE__, __LINE__, e, a); \
        } \
    } while (0)

#define TEST_ASSERT_STR_EQ(expected, actual) \
    do { \
        tests_run++; \
        const char* e = (expected); \
        const char* a = (actual); \
        if (e != NULL && a != NULL && strcmp(e, a) == 0) { \
            tests_passed++; \
        } else if (e == NULL && a == NULL) { \
            tests_passed++; \
        } else { \
            tests_failed++; \
            printf("FAIL: %s:%d: expected \"%s\", got \"%s\"\n", __FILE__, __LINE__, e ? e : "NULL", a ? a : "NULL"); \
        } \
    } while (0)

#define TEST_ASSERT_NE(expected, actual) \
    do { \
        tests_run++; \
        long long e = (long long)(expected); \
        long long a = (long long)(actual); \
        if (e != a) { \
            tests_passed++; \
        } else { \
            tests_failed++; \
            printf("FAIL: %s:%d: expected %lld and %lld to be different\n", __FILE__, __LINE__, e, a); \
        } \
    } while (0)

#define TEST_ASSERT(condition) \
    do { \
        tests_run++; \
        if (condition) { \
            tests_passed++; \
        } else { \
            tests_failed++; \
            printf("FAIL: %s:%d: condition '%s' was false\n", __FILE__, __LINE__, #condition); \
        } \
    } while (0)

#define TEST_REPORT() \
    do { \
        printf("\n-----------------------------------\n"); \
        printf("Tests run:    %d\n", tests_run); \
        printf("Tests passed: %d\n", tests_passed); \
        printf("Tests failed: %d\n", tests_failed); \
        printf("-----------------------------------\n"); \
        if (tests_failed > 0) exit(1); \
    } while (0)

#ifdef TEST_FRAMEWORK_IMPL
int tests_run = 0;
int tests_passed = 0;
int tests_failed = 0;
const char* current_test_name = "";
#endif

#endif
