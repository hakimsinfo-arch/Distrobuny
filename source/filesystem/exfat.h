/*
 * Distro Buny — read-only exFAT metadata reader
 *
 * Maps a file on an exFAT volume to its physical extents (byte ranges
 * relative to the start of the partition).  The Linux exfat driver offers
 * neither FIEMAP nor an efficient FIBMAP, and the virtual medium needs the
 * exact on-disk location of each ISO, so the directory tree and FAT are read
 * directly from the block device.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BUNY_EXFAT_H
#define BUNY_EXFAT_H

#include <stddef.h>
#include <stdint.h>

struct file_extent {
	uint64_t offset;  /* bytes from the start of the partition */
	uint64_t length;  /* bytes */
};

struct exfat_vol {
	int fd;
	uint64_t base;          /* byte offset of the volume within fd */
	unsigned bps_shift;     /* bytes per sector (log2) */
	unsigned spc_shift;     /* sectors per cluster (log2) */
	uint64_t fat_offset;    /* bytes */
	uint64_t heap_offset;   /* bytes */
	uint32_t cluster_count;
	uint32_t root_cluster;
	uint32_t serial;
	uint64_t cluster_size;
	uint64_t volume_bytes;
};

int exfat_open(struct exfat_vol *v, int fd, uint64_t base);
/* Resolve `path` ("/Desktop/x.iso") to extents.  Returns 0, or -1 with errno. */
int exfat_file_extents(struct exfat_vol *v, const char *path, struct file_extent **ext, size_t *n, uint64_t *size);

#endif
