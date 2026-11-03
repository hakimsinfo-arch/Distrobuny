/*
 * Distro Buny — common utilities
 * Copyright (c) 2026 the Distro Buny authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BUNY_UTIL_H
#define BUNY_UTIL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define BUNY_MIN(a, b) ((a) < (b) ? (a) : (b))
#define BUNY_MAX(a, b) ((a) > (b) ? (a) : (b))
#define BUNY_CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
#define BUNY_UNUSED(x) ((void)(x))
#define BUNY_PRINTF(f, a) __attribute__((format(printf, f, a)))

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
char *xasprintf(const char *fmt, ...) BUNY_PRINTF(1, 2);
char *xvasprintf(const char *fmt, va_list ap);

bool str_starts(const char *s, const char *prefix);
bool str_istarts(const char *s, const char *prefix);
bool str_ends(const char *s, const char *suffix);
bool str_iends(const char *s, const char *suffix);
bool str_eq(const char *a, const char *b);
bool str_ieq(const char *a, const char *b);
const char *str_icontains(const char *hay, const char *needle);
char *str_trim(char *s);              /* trims in place, returns s+offset */
void str_lower(char *s);
void str_rstrip_chars(char *s, const char *chars);
bool str_has_space(const char *s);
size_t strlcpy_buny(char *dst, const char *src, size_t n);

/* Natural-order comparison: "file2" < "file10", case-insensitive. */
int natural_cmp(const char *a, const char *b);

/* Human readable byte size, e.g. "4.7 GB" (decimal, like file managers). */
void format_size(uint64_t bytes, char *out, size_t outlen);

/* Monotonic clock in milliseconds / microseconds. */
uint64_t now_ms(void);
uint64_t now_us(void);

/* Read a whole small file (max bytes); returns malloc'd NUL-terminated buffer or NULL. */
char *read_file(const char *path, size_t max, size_t *len_out);
/* Write data atomically: write tmp, fsync, rename, fsync dir. Returns 0 on success. */
int write_file_atomic(const char *path, const void *data, size_t len);

/* Join path components with exactly one '/' between them. */
char *path_join(const char *a, const char *b);
const char *path_basename(const char *p);
char *path_dirname(const char *p);

/* Dynamic pointer vector */
struct ptrvec {
	void **v;
	size_t n, cap;
};
void ptrvec_push(struct ptrvec *pv, void *p);
void ptrvec_free(struct ptrvec *pv, void (*free_fn)(void *));

#endif
