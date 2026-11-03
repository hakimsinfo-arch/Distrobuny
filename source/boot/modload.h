/*
 * Distro Buny — kernel module loading for the runtime
 *
 * A small replacement for modprobe/udev coldplug: modules are matched to
 * devices through modules.alias, dependencies come from modules.dep, and
 * modules are loaded with finit_module(2) (compressed modules are
 * decompressed by the kernel).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BUNY_MODLOAD_H
#define BUNY_MODLOAD_H

#include <stdbool.h>
#include <stddef.h>

struct modctx;

struct modctx *modload_open(const char *moddir); /* NULL: /lib/modules/$(uname -r) */
void modload_close(struct modctx *m);
/* Load a module (and its dependencies) by name.  Returns 0 if loaded or built in. */
int modload_name(struct modctx *m, const char *name);
/* Load every module whose alias matches the modalias string.  Returns count loaded. */
int modload_alias(struct modctx *m, const char *modalias);
/* Walk /sys/devices and load modules for every device.  Returns count loaded. */
int modload_coldplug(struct modctx *m, const char *sysdir);
size_t modload_count_loaded(const struct modctx *m);

#endif
