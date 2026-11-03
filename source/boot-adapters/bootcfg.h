/*
 * Distro Buny — boot configuration model and parsers
 *
 * A distribution image describes how it boots in its own configuration files
 * (GRUB grub.cfg / loopback.cfg, systemd-boot loader entries, ISOLINUX).  These
 * parsers turn them into a uniform list of boot entries — kernel, initrds and
 * arguments as paths inside the ISO — which the family adapters then refine.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BUNY_BOOTCFG_H
#define BUNY_BOOTCFG_H

#include <stdbool.h>

#include "common/util.h"
#include "filesystem/iso9660.h"

enum entry_source {
	SRC_GRUB_LOOPBACK = 0, /* loopback.cfg evaluated with iso_path (designed for ISO-file boot) */
	SRC_GRUB,              /* the image's main grub.cfg */
	SRC_SDBOOT,            /* systemd-boot loader entries */
	SRC_SYSLINUX,          /* isolinux/syslinux configuration */
	SRC_SYNTHETIC,         /* constructed by an adapter */
};

struct boot_entry {
	char *title;
	char *id;
	char *kernel;           /* ISO path, normalised ("/casper/vmlinuz") */
	struct ptrvec initrds;  /* char*: ISO paths, in load order */
	struct ptrvec args;     /* char*: kernel arguments, unquoted */
	char *efi;              /* chainload target (EFI binary) if any */
	char *config;           /* config file this entry came from */
	enum entry_source source;
	int order;
	bool is_default;
};

struct boot_entries {
	struct boot_entry **v;
	size_t n;
};

/* Context used while evaluating GRUB scripts from inside an ISO. */
struct grub_eval_ctx {
	struct iso_image *iso;
	const char *iso_path;       /* path of the ISO on the data partition, or NULL */
	const char *data_fs_uuid;   /* filesystem UUID of the data partition, or placeholder */
	const char *data_fs_label;
	enum entry_source source;
	unsigned max_steps;         /* 0 = default */
};

void boot_entry_free(struct boot_entry *e);
struct boot_entry *boot_entry_clone(const struct boot_entry *e);
void boot_entries_free(struct boot_entries *list);
void boot_entries_add(struct boot_entries *list, struct boot_entry *e);

/* Evaluate a GRUB configuration file located at `cfg_path` inside the ISO. */
int grubcfg_eval(const struct grub_eval_ctx *ctx, const char *cfg_path, struct boot_entries *out, char *err, size_t errlen);
/* Evaluate GRUB script text (used by tests). */
int grubcfg_eval_text(const struct grub_eval_ctx *ctx, const char *text, const char *name, struct boot_entries *out, char *err, size_t errlen);

int sdboot_parse(struct iso_image *iso, struct boot_entries *out);
int syslinux_parse(struct iso_image *iso, struct boot_entries *out);

/* Argument helpers. */
const char *entry_get_arg(const struct boot_entry *e, const char *key);  /* value of key=..., "" for bare flag, NULL if absent */
bool entry_has_arg(const struct boot_entry *e, const char *key);
void entry_set_arg(struct boot_entry *e, const char *key, const char *value); /* replace or append; value NULL => bare */
void entry_remove_arg(struct boot_entry *e, const char *key);                /* removes key and key=... */
void entry_add_arg(struct boot_entry *e, const char *arg);       /* kernel parameter, kept before "--"/"---" */
void entry_add_arg_front(struct boot_entry *e, const char *arg);
/* Serialise args to a kernel command line, quoting arguments with spaces like GRUB does. */
char *entry_cmdline(const struct boot_entry *e);
/* Split a command line into arguments, honouring double quotes. */
void cmdline_split(const char *line, struct ptrvec *out);

/* Normalise "($root)/a//b" or "(loop)/x" to "/a/b". */
char *grub_path_normalize(const char *p);

#endif
