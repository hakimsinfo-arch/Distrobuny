/*
 * Distro Buny — logging
 *
 * Messages go to stderr (host tools), optionally to /dev/kmsg and the serial
 * console (runtime), and always into an in-memory ring that the interface's
 * diagnostics screen can display.  Log lines never include file contents or
 * personal data beyond file names the user placed on the drive.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BUNY_LOG_H
#define BUNY_LOG_H

#include "common/util.h"

enum log_level { LOG_LVL_DEBUG = 0, LOG_LVL_INFO, LOG_LVL_WARN, LOG_LVL_ERROR };

void log_set_level(enum log_level lvl);
void log_set_tag(const char *tag);
/* Additional sink file descriptor (e.g. serial console or /dev/kmsg). -1 to disable. */
void log_set_extra_fd(int fd, bool kmsg_format);
void log_set_stderr(bool enabled);
void log_msg(enum log_level lvl, const char *fmt, ...) BUNY_PRINTF(2, 3);

/* Copy of the last N log lines (newest last).  Returns number of lines. */
size_t log_ring_snapshot(char ***lines_out);
void log_ring_free(char **lines, size_t n);

#define LOGD(...) log_msg(LOG_LVL_DEBUG, __VA_ARGS__)
#define LOGI(...) log_msg(LOG_LVL_INFO, __VA_ARGS__)
#define LOGW(...) log_msg(LOG_LVL_WARN, __VA_ARGS__)
#define LOGE(...) log_msg(LOG_LVL_ERROR, __VA_ARGS__)

#endif
