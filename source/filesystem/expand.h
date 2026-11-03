/*
 * Distro Buny — growing the data partition to the end of its drive
 *
 * A drive written from an image (balenaEtcher, Rufus, dd) onto a bigger
 * stick keeps the image's size: DISTROBUNY and its exFAT file system end
 * where the image ended and the backup GPT sits in the middle of the stick.
 * Distro Buny images are laid out so that this can be fixed in place
 * (source/python/distrobuny/image/layout.py and exfat.py): DISTROBUNY is the
 * last partition on the disk, its file system reserves room for a larger
 * FAT in front of the cluster heap, and its allocation bitmap takes the last
 * clusters of the heap, right where new clusters are appended.
 *
 * Growing rewrites metadata only: both GPT copies (the backup moves to the
 * end of the disk), the protective MBR's size, FAT entries and bitmap bits
 * of the new clusters, the bitmap's directory entry and the two boot
 * regions.  No file data and no other partition is read for writing.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BUNY_EXPAND_H
#define BUNY_EXPAND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum expand_state {
	EXPAND_NONE,      /* DISTROBUNY already uses the drive (or the gain would be tiny) */
	EXPAND_POSSIBLE,  /* it can grow: expand_run() grows it */
	EXPAND_RESUME,    /* an interrupted expansion (or a damaged copy): expand_run() finishes it */
	EXPAND_REFUSED,   /* the drive is bigger, but DISTROBUNY cannot grow safely: see reason */
};

struct expand_plan {
	enum expand_state state;
	const char *code;          /* short machine-readable reason ("layout", "dirty", ...) */
	char reason[512];          /* for the user (EXPAND_REFUSED) */
	uint64_t disk_sectors;     /* 512-byte sectors */
	uint64_t data_bytes;       /* DISTROBUNY's file system now ... */
	uint64_t new_data_bytes;   /* ... and after growing */
	bool fs_limited;           /* the file system cannot reach the end of the drive */

	/* partition table */
	unsigned part_index;       /* entry of DISTROBUNY */
	uint64_t part_first, part_last, new_part_last;
	bool gpt_current;          /* both copies already describe the new layout */
	bool primary_bad;          /* the primary copy is damaged: rewritten from the backup */
	uint32_t gpt_entries, gpt_entry_size;
	uint8_t *gpt_table;        /* the valid entry array (gpt_entries * gpt_entry_size bytes) */
	uint8_t gpt_header[512];   /* the valid header */

	/* exFAT, in sectors from the start of the partition */
	bool boot_from_backup;     /* main boot region damaged: the backup describes the volume */
	uint8_t boot[12 * 512];    /* the valid boot region */
	unsigned spc_shift;
	uint32_t fat_offset, fat_length, new_fat_length, heap_offset;
	uint64_t vol_sectors, new_vol_sectors;
	uint32_t clusters, new_clusters, used_clusters;
	uint32_t bitmap_first, bitmap_clusters, new_bitmap_clusters;
	uint64_t bitmap_entry;     /* byte offset of the bitmap's directory entry, from the partition start */

	/* tests: simulate a power cut after this many writes (0: off) */
	unsigned stop_after_writes;
	unsigned writes;           /* writes expand_run() issued */
};

/* Inspect the whole-disk device (or disk image) `fd` of `disk_bytes` bytes
 * with `sector_size`-byte logical sectors.  Reads only.  Returns 0 (see
 * p->state) or -1 with errno when the disk cannot be read. */
int expand_plan(int fd, uint64_t disk_bytes, unsigned sector_size, struct expand_plan *p);
/* Carry out an EXPAND_POSSIBLE or EXPAND_RESUME plan.  Returns 0, or -1 with
 * a message in err (errno EINTR: stopped by stop_after_writes). */
int expand_run(int fd, struct expand_plan *p, char *err, size_t errlen);
void expand_plan_free(struct expand_plan *p);
const char *expand_state_name(enum expand_state s);

#endif
