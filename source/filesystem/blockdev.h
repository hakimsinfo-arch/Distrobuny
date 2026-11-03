/*
 * Distro Buny — block device discovery and filesystem probing
 *
 * Used by the runtime to find its own boot drive (ESP and data partition)
 * and to describe the data partition's geometry to the virtual-medium
 * pre-init.  Probing reads superblocks directly; nothing is mounted.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BUNY_BLOCKDEV_H
#define BUNY_BLOCKDEV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* GPT partition type GUIDs (lower case, as printed by blkid/sgdisk). */
#define GPT_TYPE_ESP "c12a7328-f81f-11d2-ba4b-00a0c93ec93b"
#define GPT_TYPE_MS_BASIC_DATA "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7"
#define GPT_TYPE_LINUX_DATA "0fc63daf-8483-4772-8e79-3d69d8477de4"

struct blk_dev {
	char name[40];        /* "sdb1" */
	char disk[40];        /* "sdb" (whole disk), same as name for disks */
	char devpath[64];     /* "/dev/sdb1" */
	unsigned major, minor;
	int partno;           /* 0 for whole disks */
	uint64_t start;       /* partition start, 512-byte sectors from disk start */
	uint64_t size;        /* size in 512-byte sectors */
	bool removable;
	bool readonly;
	char model[64];       /* disk model/vendor, if available */
	/* filesystem probe */
	char fstype[16];      /* exfat, vfat, ext4, ntfs, iso9660, "" */
	char label[64];
	char uuid[40];
	uint32_t vol_serial;  /* exFAT/FAT volume serial */
	/* GPT information (partitions only) */
	char partuuid[40];
	char parttype[40];
	char partlabel[80];
};

/* Enumerate /sys/class/block.  `devdir` is normally "/dev". */
int blk_enumerate(const char *sysdir, const char *devdir, struct blk_dev **out, size_t *n);
/* Probe the filesystem on an open device (fd); fills fstype/label/uuid/vol_serial. */
int blk_probe_fs(int fd, struct blk_dev *d);
const struct blk_dev *blk_find_by_partuuid(const struct blk_dev *v, size_t n, const char *partuuid);
const struct blk_dev *blk_find_by_label(const struct blk_dev *v, size_t n, const char *fstype, const char *label);

/* GPT entries read directly from a disk or disk image. */
struct gpt_entry {
	char type[37];
	char guid[37];
	char name[80];
	uint64_t first_lba, last_lba;   /* in 512-byte units */
};
int gpt_read(int fd, struct gpt_entry **out, size_t *n);

/* Format a little-endian GUID as text. */
void guid_to_str(const uint8_t g[16], char out[37]);

#endif
