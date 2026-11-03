/*
 * Distro Buny — physical extents of files on the data partition
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BUNY_EXTENTS_H
#define BUNY_EXTENTS_H

#include <stddef.h>
#include <stdint.h>

#include "filesystem/exfat.h"

/*
 * Map a file to partition-relative byte extents.
 *   part_dev  block device of the partition holding the file (e.g. /dev/sdb1)
 *   fstype    filesystem type as probed ("exfat", "ext4", "vfat", "ntfs", ...)
 *   mnt_path  the file's path in the mounted filesystem (for FIEMAP)
 *   fs_path   the file's path relative to the filesystem root (for exFAT)
 * Returns 0 on success.  `err` receives a human-readable reason on failure.
 */
int file_extents(const char *part_dev, const char *fstype, const char *mnt_path, const char *fs_path,
		 struct file_extent **ext, size_t *n, uint64_t *size, char *err, size_t errlen);

#endif
