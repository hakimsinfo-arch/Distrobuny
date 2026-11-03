/*
 * Distro Buny — physical extents of files on the data partition
 *
 * exFAT is read directly (see filesystem/exfat.c).  Other filesystems use the
 * FIEMAP ioctl; extents that are not plain mapped data (inline, encoded,
 * unwritten or unknown) are rejected because a block device mapping of them
 * would not reproduce the file's contents.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "boot/extents.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/util.h"

#define FIEMAP_BATCH 256

static int fiemap_extents(const char *mnt_path, struct file_extent **out, size_t *n, uint64_t *size, char *err, size_t errlen)
{
	int fd = open(mnt_path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		snprintf(err, errlen, "cannot open %s: %s", mnt_path, strerror(errno));
		return -1;
	}
	struct stat st;
	fstat(fd, &st);
	*size = (uint64_t)st.st_size;
	size_t cap = 16, cnt = 0;
	struct file_extent *ext = xmalloc(cap * sizeof(*ext));
	size_t fmsz = sizeof(struct fiemap) + FIEMAP_BATCH * sizeof(struct fiemap_extent);
	struct fiemap *fm = xcalloc(1, fmsz);
	uint64_t start = 0, mapped = 0; /* mapped: bytes covered so far, to detect holes */
	bool last = false;
	int rc = 0;
	while (!last && start < *size) {
		memset(fm, 0, fmsz);
		fm->fm_start = start;
		fm->fm_length = FIEMAP_MAX_OFFSET;
		fm->fm_flags = FIEMAP_FLAG_SYNC;
		fm->fm_extent_count = FIEMAP_BATCH;
		if (ioctl(fd, FS_IOC_FIEMAP, fm) < 0) {
			snprintf(err, errlen, "this filesystem cannot report file extents (FIEMAP: %s)", strerror(errno));
			rc = -1;
			break;
		}
		if (fm->fm_mapped_extents == 0)
			break;
		for (unsigned i = 0; i < fm->fm_mapped_extents; i++) {
			struct fiemap_extent *fe = &fm->fm_extents[i];
			const unsigned bad = FIEMAP_EXTENT_UNKNOWN | FIEMAP_EXTENT_DELALLOC | FIEMAP_EXTENT_ENCODED |
					     FIEMAP_EXTENT_DATA_ENCRYPTED | FIEMAP_EXTENT_NOT_ALIGNED | FIEMAP_EXTENT_DATA_INLINE |
					     FIEMAP_EXTENT_DATA_TAIL | FIEMAP_EXTENT_UNWRITTEN;
			if (fe->fe_flags & bad) {
				snprintf(err, errlen, "file has extents that cannot be mapped (flags 0x%x)", fe->fe_flags);
				rc = -1;
				goto out;
			}
			if (fe->fe_logical != mapped) {
				snprintf(err, errlen, "file is sparse (hole at %llu)", (unsigned long long)mapped);
				rc = -1;
				goto out;
			}
			uint64_t len = fe->fe_length;
			if (fe->fe_logical + len > *size)
				len = *size - fe->fe_logical;
			if (cnt && ext[cnt - 1].offset + ext[cnt - 1].length == fe->fe_physical) {
				ext[cnt - 1].length += len;
			} else {
				if (cnt == cap) {
					cap *= 2;
					ext = xrealloc(ext, cap * sizeof(*ext));
				}
				ext[cnt].offset = fe->fe_physical;
				ext[cnt].length = len;
				cnt++;
			}
			mapped += len;
			start = fe->fe_logical + fe->fe_length;
			if (fe->fe_flags & FIEMAP_EXTENT_LAST)
				last = true;
		}
	}
out:
	free(fm);
	close(fd);
	if (rc < 0) {
		free(ext);
		return -1;
	}
	uint64_t total = 0;
	for (size_t k = 0; k < cnt; k++)
		total += ext[k].length;
	if (total != *size) {
		snprintf(err, errlen, "extents cover %llu of %llu bytes", (unsigned long long)total, (unsigned long long)*size);
		free(ext);
		return -1;
	}
	*out = ext;
	*n = cnt;
	return 0;
}

int file_extents(const char *part_dev, const char *fstype, const char *mnt_path, const char *fs_path,
		 struct file_extent **ext, size_t *n, uint64_t *size, char *err, size_t errlen)
{
	*ext = NULL;
	*n = 0;
	if (fstype && !strcmp(fstype, "exfat")) {
		int fd = open(part_dev, O_RDONLY | O_CLOEXEC);
		if (fd < 0) {
			snprintf(err, errlen, "cannot open %s: %s", part_dev, strerror(errno));
			return -1;
		}
		struct exfat_vol v;
		int rc = exfat_open(&v, fd, 0);
		if (rc == 0)
			rc = exfat_file_extents(&v, fs_path, ext, n, size);
		if (rc < 0)
			snprintf(err, errlen, "cannot map %s on the exFAT volume: %s", fs_path, strerror(errno));
		close(fd);
		/* Cross-check against the size the kernel driver reports. */
		struct stat st;
		if (rc == 0 && mnt_path && stat(mnt_path, &st) == 0 && (uint64_t)st.st_size != *size) {
			snprintf(err, errlen, "exFAT metadata disagrees with the mounted file size");
			free(*ext);
			*ext = NULL;
			return -1;
		}
		return rc;
	}
	return fiemap_extents(mnt_path, ext, n, size, err, errlen);
}
