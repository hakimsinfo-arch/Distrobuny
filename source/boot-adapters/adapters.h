/*
 * Distro Buny — boot adapters and boot plans
 *
 * A boot adapter understands how one family of distribution images locates
 * its live or installation medium at boot.  Given the entries parsed from the
 * image's own configuration, it produces a boot plan: kernel, initrds and
 * command line, plus the media strategy:
 *
 *   MEDIA_VIRTUAL  Distro Buny attaches the ISO's on-disk extents as a
 *                  read-only loop (or device-mapper) block device from a
 *                  tiny pre-init that runs inside the distribution's own
 *                  initramfs.  The distribution then finds "its CD" by label
 *                  or UUID exactly as if booting the real medium.  No
 *                  filesystem driver for the data partition is needed in the
 *                  distribution's initramfs and file names may contain spaces.
 *
 *   MEDIA_NATIVE   The distribution's own ISO-file boot parameter
 *                  (iso-scan/filename, img_loop, findiso, inst.stage2=hd:...)
 *                  is used.  Requires the distribution's initramfs to read the
 *                  data partition's filesystem and usually a path without
 *                  spaces.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BUNY_ADAPTERS_H
#define BUNY_ADAPTERS_H

#include "boot-adapters/bootcfg.h"
#include "iso-manager/identify.h"

/* Loop device index reserved for the virtual medium. */
#define BUNY_VM_LOOP_INDEX 200
#define BUNY_VM_LOOP_DEV "/dev/loop200"
#define BUNY_PREINIT_PATH "/distrobuny/preinit"

enum media_strategy { MEDIA_AUTO = 0, MEDIA_VIRTUAL, MEDIA_NATIVE };

/* Compatibility levels, in increasing order of confidence. */
enum compat_level {
	COMPAT_UNKNOWN = 0,   /* not recognised and no boot method */
	COMPAT_RECOGNIZED,    /* distribution recognised, no usable boot method */
	COMPAT_METHOD,        /* boot method known, but the plan could not be prepared */
	COMPAT_PREPARED,      /* kernel, initrd and parameters prepared */
	COMPAT_HANDOFF,       /* hand-off to the kernel was initiated (runtime only) */
	COMPAT_VERIFIED,      /* boot verified in the Distro Buny test lab */
};

struct boot_ctx {
	const char *data_fs_uuid;
	const char *data_fs_label;
	const char *data_fs_type;   /* "exfat", "vfat", "ext4", "ntfs", or NULL if unknown */
	enum media_strategy strategy;
	int variant;                /* -1 = adapter default, otherwise index into plan variants */
	const char *extra_args;     /* user-supplied extra kernel arguments */
	bool safe_graphics;         /* add nomodeset */
	bool serial_console;        /* add console=ttyS0 (lab/testing) */
	bool iso_fragmented;        /* ISO is not contiguous on disk (virtual medium uses dm-linear) */
};

struct boot_plan {
	const struct adapter *adapter;
	enum media_strategy strategy;
	enum compat_level level;
	struct boot_entry *entry;       /* final kernel, initrds, arguments */
	struct ptrvec variant_titles;   /* char*: selectable boot entries */
	int variant_index;
	bool needs_preinit;             /* virtual medium: pre-init must be injected */
	bool virtual_possible, native_possible;
	char *virtual_blocker;          /* why the virtual strategy cannot be used */
	char *native_blocker;           /* why the native strategy cannot be used */
	char *reason;                   /* explanation of the final decision / failure */
	char *warning;                  /* the system does something destructive the user must know first */
	struct ptrvec notes;            /* char*: informational notes for the user */
	char *udeb;                     /* Debian installer: the image's loop driver package (ISO path); the
					 * pre-init unpacks it, and the virtual medium is tagged as a CD drive */
};

struct adapter {
	const char *id;
	const char *name;               /* human readable family name */
	const char *mechanism;          /* one-line description of how the family finds its medium */
	int (*detect)(const struct iso_probe *p);
	/* Adjust a cloned entry for a strategy.  Return 0 on success, or -1 and set *blocker. */
	int (*prepare_virtual)(const struct iso_probe *p, struct boot_entry *e, const struct boot_ctx *c, char **blocker);
	int (*prepare_native)(const struct iso_probe *p, struct boot_entry *e, const struct boot_ctx *c, char **blocker);
};

/* The fallback for 64-bit Linux images of an unknown family (the menu asks before starting one). */
#define ADAPTER_BEST_EFFORT "best-effort"
/* Slackware: the user names the virtual medium in setup (the menu says how). */
#define ADAPTER_SLACKWARE "slackware"
/* debian-installer: started with the loop driver package from the image itself. */
#define ADAPTER_DEBIAN_INSTALLER "debian-installer"

/* The image's loop-modules udeb for its installer kernel, or NULL (caller frees). */
char *debian_installer_loop_udeb(const struct iso_probe *p);

const struct adapter *const *adapters_all(size_t *n);
const struct adapter *adapter_detect(const struct iso_probe *p, int *score);
const struct adapter *adapter_by_id(const char *id);

/* Build a boot plan.  Always fills *plan; returns 0 if level >= COMPAT_PREPARED. */
int plan_build(const struct iso_probe *p, const struct boot_ctx *ctx, struct boot_plan *plan);
void plan_free(struct boot_plan *plan);

const char *compat_level_name(enum compat_level l);
const char *media_strategy_name(enum media_strategy s);

#endif
