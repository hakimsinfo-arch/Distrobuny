/*
 * Distro Buny — growable string buffer
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "common/strbuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sb_reserve(struct strbuf *sb, size_t extra)
{
	size_t need = sb->len + extra + 1;
	if (need <= sb->cap)
		return;
	size_t cap = sb->cap ? sb->cap : 64;
	while (cap < need)
		cap *= 2;
	sb->buf = xrealloc(sb->buf, cap);
	sb->cap = cap;
}

void sb_addn(struct strbuf *sb, const char *s, size_t n)
{
	sb_reserve(sb, n);
	memcpy(sb->buf + sb->len, s, n);
	sb->len += n;
	sb->buf[sb->len] = '\0';
}

void sb_add(struct strbuf *sb, const char *s)
{
	if (s)
		sb_addn(sb, s, strlen(s));
}

void sb_addc(struct strbuf *sb, char c)
{
	sb_addn(sb, &c, 1);
}

void sb_vaddf(struct strbuf *sb, const char *fmt, va_list ap)
{
	char small[256];
	va_list cp;
	va_copy(cp, ap);
	int n = vsnprintf(small, sizeof(small), fmt, cp);
	va_end(cp);
	if (n <= 0)
		return;
	if ((size_t)n < sizeof(small)) {
		sb_addn(sb, small, (size_t)n);
		return;
	}
	sb_reserve(sb, (size_t)n);
	vsnprintf(sb->buf + sb->len, (size_t)n + 1, fmt, ap);
	sb->len += (size_t)n;
}

void sb_addf(struct strbuf *sb, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	sb_vaddf(sb, fmt, ap);
	va_end(ap);
}

const char *sb_str(struct strbuf *sb)
{
	return sb->buf ? sb->buf : "";
}

char *sb_detach(struct strbuf *sb)
{
	char *s = sb->buf ? sb->buf : xstrdup("");
	sb->buf = NULL;
	sb->len = sb->cap = 0;
	return s;
}

void sb_reset(struct strbuf *sb)
{
	sb->len = 0;
	if (sb->buf)
		sb->buf[0] = '\0';
}

void sb_free(struct strbuf *sb)
{
	free(sb->buf);
	sb->buf = NULL;
	sb->len = sb->cap = 0;
}
