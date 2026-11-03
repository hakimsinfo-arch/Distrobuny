/*
 * Distro Buny — common utilities
 * Copyright (c) 2026 the Distro Buny authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "common/util.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void oom(void)
{
	static const char msg[] = "distrobuny: out of memory\n";
	if (write(2, msg, sizeof(msg) - 1) < 0) {
		/* nothing else we can do */
	}
	abort();
}

void *xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);
	if (!p)
		oom();
	return p;
}

void *xcalloc(size_t n, size_t sz)
{
	void *p = calloc(n ? n : 1, sz ? sz : 1);
	if (!p)
		oom();
	return p;
}

void *xrealloc(void *p, size_t n)
{
	p = realloc(p, n ? n : 1);
	if (!p)
		oom();
	return p;
}

char *xstrdup(const char *s)
{
	char *d = strdup(s ? s : "");
	if (!d)
		oom();
	return d;
}

char *xstrndup(const char *s, size_t n)
{
	char *d = strndup(s ? s : "", n);
	if (!d)
		oom();
	return d;
}

char *xvasprintf(const char *fmt, va_list ap)
{
	char *out = NULL;
	if (vasprintf(&out, fmt, ap) < 0)
		oom();
	return out;
}

char *xasprintf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	char *s = xvasprintf(fmt, ap);
	va_end(ap);
	return s;
}

bool str_starts(const char *s, const char *prefix)
{
	return s && prefix && strncmp(s, prefix, strlen(prefix)) == 0;
}

bool str_istarts(const char *s, const char *prefix)
{
	return s && prefix && strncasecmp(s, prefix, strlen(prefix)) == 0;
}

bool str_ends(const char *s, const char *suffix)
{
	if (!s || !suffix)
		return false;
	size_t a = strlen(s), b = strlen(suffix);
	return a >= b && memcmp(s + a - b, suffix, b) == 0;
}

bool str_iends(const char *s, const char *suffix)
{
	if (!s || !suffix)
		return false;
	size_t a = strlen(s), b = strlen(suffix);
	return a >= b && strcasecmp(s + a - b, suffix) == 0;
}

bool str_eq(const char *a, const char *b)
{
	return a && b && strcmp(a, b) == 0;
}

bool str_ieq(const char *a, const char *b)
{
	return a && b && strcasecmp(a, b) == 0;
}

const char *str_icontains(const char *hay, const char *needle)
{
	if (!hay || !needle)
		return NULL;
	return strcasestr(hay, needle);
}

char *str_trim(char *s)
{
	if (!s)
		return s;
	while (*s && isspace((unsigned char)*s))
		s++;
	size_t n = strlen(s);
	while (n > 0 && isspace((unsigned char)s[n - 1]))
		s[--n] = '\0';
	return s;
}

void str_lower(char *s)
{
	for (; s && *s; s++)
		*s = (char)tolower((unsigned char)*s);
}

void str_rstrip_chars(char *s, const char *chars)
{
	size_t n = strlen(s);
	while (n > 0 && strchr(chars, s[n - 1]))
		s[--n] = '\0';
}

bool str_has_space(const char *s)
{
	for (; s && *s; s++)
		if (isspace((unsigned char)*s))
			return true;
	return false;
}

size_t strlcpy_buny(char *dst, const char *src, size_t n)
{
	size_t len = strlen(src);
	if (n) {
		size_t c = len >= n ? n - 1 : len;
		memcpy(dst, src, c);
		dst[c] = '\0';
	}
	return len;
}

int natural_cmp(const char *a, const char *b)
{
	while (*a && *b) {
		if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
			while (*a == '0')
				a++;
			while (*b == '0')
				b++;
			size_t la = 0, lb = 0;
			while (isdigit((unsigned char)a[la]))
				la++;
			while (isdigit((unsigned char)b[lb]))
				lb++;
			if (la != lb)
				return la < lb ? -1 : 1;
			int c = strncmp(a, b, la);
			if (c)
				return c;
			a += la;
			b += lb;
			continue;
		}
		int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
		if (ca != cb)
			return ca < cb ? -1 : 1;
		a++;
		b++;
	}
	return (*a != 0) - (*b != 0);
}

void format_size(uint64_t bytes, char *out, size_t outlen)
{
	static const char *units[] = { "B", "KB", "MB", "GB", "TB" };
	double v = (double)bytes;
	int u = 0;
	while (v >= 1000.0 && u < 4) {
		v /= 1000.0;
		u++;
	}
	if (u == 0)
		snprintf(out, outlen, "%llu B", (unsigned long long)bytes);
	else if (v >= 100.0)
		snprintf(out, outlen, "%.0f %s", v, units[u]);
	else
		snprintf(out, outlen, "%.1f %s", v, units[u]);
}

uint64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

uint64_t now_ms(void)
{
	return now_us() / 1000ull;
}

char *read_file(const char *path, size_t max, size_t *len_out)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return NULL;
	size_t cap = 4096, len = 0;
	char *buf = xmalloc(cap + 1);
	for (;;) {
		if (len == cap) {
			if (cap >= max)
				break;
			cap = BUNY_MIN(cap * 2, max);
			buf = xrealloc(buf, cap + 1);
		}
		ssize_t r = read(fd, buf + len, cap - len);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			free(buf);
			close(fd);
			return NULL;
		}
		if (r == 0)
			break;
		len += (size_t)r;
	}
	close(fd);
	buf[len] = '\0';
	if (len_out)
		*len_out = len;
	return buf;
}

int write_file_atomic(const char *path, const void *data, size_t len)
{
	char *tmp = xasprintf("%s.tmp.%d", path, (int)getpid());
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0) {
		free(tmp);
		return -1;
	}
	const char *p = data;
	size_t left = len;
	while (left) {
		ssize_t w = write(fd, p, left);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			close(fd);
			unlink(tmp);
			free(tmp);
			return -1;
		}
		p += w;
		left -= (size_t)w;
	}
	if (fsync(fd) < 0 || close(fd) < 0) {
		unlink(tmp);
		free(tmp);
		return -1;
	}
	if (rename(tmp, path) < 0) {
		unlink(tmp);
		free(tmp);
		return -1;
	}
	free(tmp);
	char *dir = path_dirname(path);
	int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dfd >= 0) {
		fsync(dfd);
		close(dfd);
	}
	free(dir);
	return 0;
}

char *path_join(const char *a, const char *b)
{
	if (!a || !*a)
		return xstrdup(b ? b : "");
	if (!b || !*b)
		return xstrdup(a);
	size_t la = strlen(a);
	while (la > 1 && a[la - 1] == '/')
		la--;
	while (*b == '/')
		b++;
	if (la == 1 && a[0] == '/')
		return xasprintf("/%s", b);
	return xasprintf("%.*s/%s", (int)la, a, b);
}

const char *path_basename(const char *p)
{
	const char *s = strrchr(p, '/');
	return s ? s + 1 : p;
}

char *path_dirname(const char *p)
{
	const char *s = strrchr(p, '/');
	if (!s)
		return xstrdup(".");
	if (s == p)
		return xstrdup("/");
	return xstrndup(p, (size_t)(s - p));
}

void ptrvec_push(struct ptrvec *pv, void *p)
{
	if (pv->n == pv->cap) {
		pv->cap = pv->cap ? pv->cap * 2 : 8;
		pv->v = xrealloc(pv->v, pv->cap * sizeof(void *));
	}
	pv->v[pv->n++] = p;
}

void ptrvec_free(struct ptrvec *pv, void (*free_fn)(void *))
{
	if (free_fn)
		for (size_t i = 0; i < pv->n; i++)
			free_fn(pv->v[i]);
	free(pv->v);
	pv->v = NULL;
	pv->n = pv->cap = 0;
}
