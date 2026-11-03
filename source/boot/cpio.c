/*
 * Distro Buny — "newc" cpio writer (initramfs format)
 *
 * Archives are deterministic: inode numbers are sequential, owners are root
 * and timestamps are zero.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "boot/cpio.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int put(struct cpio_writer *w, const void *data, size_t len)
{
	const char *p = data;
	while (len) {
		ssize_t r = write(w->fd, p, len);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			w->error = errno;
			return -1;
		}
		p += r;
		len -= (size_t)r;
		w->off += (uint64_t)r;
	}
	return 0;
}

static int pad4(struct cpio_writer *w)
{
	static const char zero[4];
	size_t n = (size_t)((4 - (w->off & 3)) & 3);
	return n ? put(w, zero, n) : 0;
}

static int header(struct cpio_writer *w, const char *name, uint32_t mode, uint32_t nlink, uint32_t filesize)
{
	char hdr[111];
	size_t namesize = strlen(name) + 1;
	snprintf(hdr, sizeof(hdr), "070701%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X", ++w->ino, mode, 0u, 0u,
		 nlink, 0u, filesize, 0u, 0u, 0u, 0u, (uint32_t)namesize, 0u);
	if (put(w, hdr, 110) < 0 || put(w, name, namesize) < 0 || pad4(w) < 0)
		return -1;
	return 0;
}

void cpio_init(struct cpio_writer *w, int fd)
{
	memset(w, 0, sizeof(*w));
	w->fd = fd;
}

int cpio_add_dir(struct cpio_writer *w, const char *name, uint32_t mode)
{
	return header(w, name, S_IFDIR | (mode & 07777), 2, 0);
}

int cpio_add_file(struct cpio_writer *w, const char *name, uint32_t mode, const void *data, size_t len)
{
	if (len > 0xFFFFFFFFu) {
		w->error = EFBIG;
		return -1;
	}
	if (header(w, name, S_IFREG | (mode & 07777), 1, (uint32_t)len) < 0)
		return -1;
	if (len && put(w, data, len) < 0)
		return -1;
	return pad4(w);
}

int cpio_finish(struct cpio_writer *w)
{
	if (header(w, "TRAILER!!!", 0, 1, 0) < 0)
		return -1;
	return pad4(w);
}
