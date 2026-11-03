/*
 * Distro Buny — minimal unit test harness
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BUNY_TEST_H
#define BUNY_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int t_failures, t_checks;
static const char *t_current = "";

#define CHECK(cond)                                                                                 \
	do {                                                                                        \
		t_checks++;                                                                         \
		if (!(cond)) {                                                                      \
			t_failures++;                                                               \
			fprintf(stderr, "  FAIL %s:%d [%s]: %s\n", __FILE__, __LINE__, t_current, #cond); \
		}                                                                                   \
	} while (0)

#define CHECK_STR(a, b)                                                                                       \
	do {                                                                                                  \
		const char *_a = (a), *_b = (b);                                                              \
		t_checks++;                                                                                   \
		if (!_a || !_b || strcmp(_a, _b) != 0) {                                                      \
			t_failures++;                                                                         \
			fprintf(stderr, "  FAIL %s:%d [%s]: %s == \"%s\", expected \"%s\"\n", __FILE__, __LINE__, \
				t_current, #a, _a ? _a : "(null)", _b ? _b : "(null)");                       \
		}                                                                                             \
	} while (0)

#define TEST(fn)                                       \
	do {                                           \
		t_current = #fn;                       \
		int before = t_failures;               \
		fn();                                  \
		printf("  %s %s\n", t_failures == before ? "ok  " : "FAIL", #fn); \
	} while (0)

static int test_summary(const char *suite)
{
	printf("%s: %d checks, %d failures\n", suite, t_checks, t_failures);
	return t_failures ? 1 : 0;
}

#endif
