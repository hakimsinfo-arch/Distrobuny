/*
 * Distro Buny — hand-off to the selected distribution (kexec)
 *
 * The hand-off is split across the runtime's privilege boundary:
 *
 *   handoff_prepare()  runs in the unprivileged interface: reads the kernel
 *                      and initrds out of the ISO into memory files and builds
 *                      the kernel command line.
 *   handoff_load()     runs in the privileged init: maps the ISO's on-disk
 *                      extents, appends the virtual-medium overlay, and calls
 *                      kexec_file_load(2).
 *   handoff_execute()  flushes and reboots into the loaded kernel.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BUNY_HANDOFF_H
#define BUNY_HANDOFF_H

#include <stdbool.h>
#include <stdint.h>

#include "boot-adapters/adapters.h"
#include "filesystem/exfat.h"

/* Where the ISO lives, for the virtual medium. */
struct handoff_medium {
	const char *part_dev;        /* data partition device (or disk image file) */
	uint64_t part_base;          /* byte offset of the partition within part_dev (0 for a partition node) */
	uint64_t part_start;         /* 512-byte sectors from the start of the disk */
	uint64_t part_size;          /* 512-byte sectors */
	const char *fstype;
	uint32_t vol_serial;
	bool have_serial;
	struct file_extent *ext;
	size_t next;
	uint64_t iso_size;
	const char *iso_label;
	const char *iso_file;        /* the ISO through the mounted filesystem, for verification (optional) */
	bool verbose;                /* pre-init mirrors its messages to the console (lab mode) */
	bool force_dm;               /* use the device-mapper path even for one extent (tests) */
};

enum handoff_stage { HS_KERNEL, HS_INITRD, HS_MEDIUM, HS_LOAD, HS_DONE };

typedef void (*handoff_progress_fn)(void *ctx, enum handoff_stage stage, uint64_t done, uint64_t total);

struct handoff_payload {
	int kernel_fd;
	int initrd_fd;
	char *cmdline;               /* without the pre-init hook */
	bool needs_preinit;
};

int handoff_prepare(struct iso_image *iso, const struct boot_plan *plan, struct handoff_payload *out,
		    handoff_progress_fn cb, void *cb_ctx, char *err, size_t errlen);
void handoff_payload_close(struct handoff_payload *p);

/* Verify the extent map against the ISO (label, and data if iso_file is set). */
int handoff_verify_medium(const struct handoff_medium *m, char *err, size_t errlen);
/* Append the pre-init overlay to the initrd and return the final command line. */
int handoff_attach_medium(struct handoff_payload *p, const struct handoff_medium *m, const char *preinit_path,
			  char **final_cmdline, char *err, size_t errlen);
/* Load with kexec_file_load.  `dry_run` validates without loading. */
int handoff_load(struct handoff_payload *p, const char *cmdline, bool dry_run, char *err, size_t errlen);
int handoff_execute(char *err, size_t errlen);

/* Render the medium.conf the pre-init reads. */
char *handoff_medium_conf(const struct handoff_medium *m, const char *next_init);
/* Write the payload to files (testing and diagnostics). */
int handoff_dump(const struct handoff_payload *p, const char *cmdline, const char *dir);

#endif
