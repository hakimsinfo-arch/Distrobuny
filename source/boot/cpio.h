/*
 * Distro Buny — "newc" cpio writer (initramfs format)
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BUNY_CPIO_H
#define BUNY_CPIO_H

#include <stddef.h>
#include <stdint.h>

struct cpio_writer {
	int fd;
	uint64_t off;
	uint32_t ino;
	int error;
};

void cpio_init(struct cpio_writer *w, int fd);
int cpio_add_dir(struct cpio_writer *w, const char *name, uint32_t mode);
int cpio_add_file(struct cpio_writer *w, const char *name, uint32_t mode, const void *data, size_t len);
/* Write the trailer and pad the archive to a 4-byte boundary. */
int cpio_finish(struct cpio_writer *w);

#endif
