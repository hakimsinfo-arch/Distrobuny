/*
 * Distro Buny — growable string buffer
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BUNY_STRBUF_H
#define BUNY_STRBUF_H

#include <stdarg.h>
#include <stddef.h>

#include "common/util.h"

struct strbuf {
	char *buf;
	size_t len, cap;
};

#define STRBUF_INIT { NULL, 0, 0 }

void sb_reserve(struct strbuf *sb, size_t extra);
void sb_addn(struct strbuf *sb, const char *s, size_t n);
void sb_add(struct strbuf *sb, const char *s);
void sb_addc(struct strbuf *sb, char c);
void sb_addf(struct strbuf *sb, const char *fmt, ...) BUNY_PRINTF(2, 3);
void sb_vaddf(struct strbuf *sb, const char *fmt, va_list ap);
const char *sb_str(struct strbuf *sb); /* never NULL */
char *sb_detach(struct strbuf *sb);    /* caller frees; sb reset */
void sb_reset(struct strbuf *sb);
void sb_free(struct strbuf *sb);

#endif
