/*
 * Distro Buny — ISO 9660 reader
 *
 * Reads ISO 9660 images directly from a file descriptor (no loop mount and no
 * privileges required).  Supports Rock Ridge (NM, SL, CE continuation),
 * Joliet (UCS-2), multi-extent files (> 4 GiB), symlink resolution and
 * case-insensitive path fallback.  All on-disk values are treated as
 * untrusted: every length and offset is bounds-checked against the image.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BUNY_ISO9660_H
#define BUNY_ISO9660_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include "common/util.h"

#define ISO_SECTOR 2048u

struct iso_extent {
	uint64_t lba;  /* in 2048-byte sectors */
	uint64_t len;  /* bytes */
};

struct iso_node {
	char *name;              /* UTF-8, no version suffix */
	bool is_dir;
	bool is_symlink;
	char *link_target;       /* Rock Ridge SL target, if symlink */
	uint64_t size;           /* total bytes over all extents */
	uint32_t next;
	struct iso_extent *ext;
};

struct iso_dir {
	struct iso_node *nodes;
	size_t n;
};

struct iso_info {
	char volume_id[33];
	char system_id[33];
	char volume_set_id[129];
	char publisher_id[129];
	char preparer_id[129];
	char application_id[129];
	char uuid[24];           /* "YYYY-MM-DD-HH-MM-SS-cc" like GRUB/blkid */
	char created[20];        /* "YYYY-MM-DD HH:MM:SS" or "" */
	uint64_t volume_bytes;   /* volume space size * block size */
	bool rock_ridge;
	bool joliet;
	bool el_torito;
};

struct iso_image;

/* Open an ISO located at byte offset `base` of `fd`; `size` is the image size. */
struct iso_image *iso_open(int fd, uint64_t base, uint64_t size, char *err, size_t errlen);
struct iso_image *iso_open_path(const char *path, char *err, size_t errlen);
void iso_close(struct iso_image *img);

const struct iso_info *iso_get_info(const struct iso_image *img);

/* A resolved file: an owned copy of the node's extent list. */
struct iso_file {
	bool is_dir;
	uint64_t size;
	uint32_t next;
	struct iso_extent *ext;
};

/* Resolve a path (absolute, '/'-separated).  Follows symlinks.  On success
 * fills *out (release with iso_file_free) and returns 0; -1 if not found. */
int iso_lookup(struct iso_image *img, const char *path, struct iso_file *out);
void iso_file_free(struct iso_file *f);
bool iso_exists(struct iso_image *img, const char *path);
bool iso_is_dir(struct iso_image *img, const char *path);

/* List a directory.  The returned pointer is owned by the image's directory
 * cache and is only valid until the next call into this module. */
const struct iso_dir *iso_list(struct iso_image *img, const char *path);

/* Canonical (on-image) spelling of a path found via case-insensitive match. */
char *iso_canonical_path(struct iso_image *img, const char *path);

ssize_t iso_pread(struct iso_image *img, const struct iso_file *f, void *buf, size_t len, uint64_t off);

/* Read a whole small file into a NUL-terminated buffer (NULL if missing or too big). */
char *iso_read_text(struct iso_image *img, const char *path, size_t max, size_t *len_out);

/* Copy a file into an open fd; progress callback may be NULL.  Returns 0 on success. */
typedef void (*iso_progress_fn)(void *ctx, uint64_t done, uint64_t total);
int iso_copy_to_fd(struct iso_image *img, const struct iso_file *f, int outfd, iso_progress_fn cb, void *ctx);

/* Expand a glob pattern ('*' and '?' within path components) into paths. */
void iso_glob(struct iso_image *img, const char *pattern, struct ptrvec *out);

/* Simple fnmatch-like matcher used by iso_glob (no brackets). */
bool iso_wildmatch(const char *pat, const char *str, bool icase);

#endif
