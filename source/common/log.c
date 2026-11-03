/*
 * Distro Buny — logging
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "common/log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RING_LINES 512

static enum log_level g_level = LOG_LVL_INFO;
static const char *g_tag = "buny";
static int g_extra_fd = -1;
static bool g_extra_kmsg;
static bool g_stderr = true;
static char *g_ring[RING_LINES];
static size_t g_ring_head, g_ring_count;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

void log_set_level(enum log_level lvl) { g_level = lvl; }
void log_set_tag(const char *tag) { g_tag = tag; }
void log_set_stderr(bool enabled) { g_stderr = enabled; }

void log_set_extra_fd(int fd, bool kmsg_format)
{
	g_extra_fd = fd;
	g_extra_kmsg = kmsg_format;
}

static const char *lvl_name(enum log_level l)
{
	switch (l) {
	case LOG_LVL_DEBUG: return "debug";
	case LOG_LVL_INFO: return "info";
	case LOG_LVL_WARN: return "warn";
	default: return "error";
	}
}

void log_msg(enum log_level lvl, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	char *msg = xvasprintf(fmt, ap);
	va_end(ap);

	pthread_mutex_lock(&g_lock);
	/* Always keep everything (including debug) in the ring for diagnostics. */
	char *line = xasprintf("[%s] %s", lvl_name(lvl), msg);
	size_t idx = (g_ring_head + g_ring_count) % RING_LINES;
	if (g_ring_count == RING_LINES) {
		free(g_ring[g_ring_head]);
		idx = g_ring_head;
		g_ring_head = (g_ring_head + 1) % RING_LINES;
	} else {
		g_ring_count++;
	}
	g_ring[idx] = line;

	if (lvl >= g_level) {
		if (g_stderr)
			fprintf(stderr, "%s: %s: %s\n", g_tag, lvl_name(lvl), msg);
		if (g_extra_fd >= 0) {
			char *out;
			if (g_extra_kmsg) {
				/* kmsg priority: 3 err, 4 warn, 6 info, 7 debug */
				int pri = lvl == LOG_LVL_ERROR ? 3 : lvl == LOG_LVL_WARN ? 4 : lvl == LOG_LVL_INFO ? 6 : 7;
				out = xasprintf("<%d>%s: %s\n", pri, g_tag, msg);
			} else {
				out = xasprintf("%s: %s: %s\r\n", g_tag, lvl_name(lvl), msg);
			}
			if (write(g_extra_fd, out, strlen(out)) < 0) {
				/* sink unavailable: ignore */
			}
			free(out);
		}
	}
	pthread_mutex_unlock(&g_lock);
	free(msg);
}

size_t log_ring_snapshot(char ***lines_out)
{
	pthread_mutex_lock(&g_lock);
	size_t n = g_ring_count;
	char **lines = xcalloc(n ? n : 1, sizeof(char *));
	for (size_t i = 0; i < n; i++)
		lines[i] = xstrdup(g_ring[(g_ring_head + i) % RING_LINES]);
	pthread_mutex_unlock(&g_lock);
	*lines_out = lines;
	return n;
}

void log_ring_free(char **lines, size_t n)
{
	for (size_t i = 0; i < n; i++)
		free(lines[i]);
	free(lines);
}
