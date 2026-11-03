/*
 * Distro Buny — read-only exFAT metadata reader
 *
 * Follows the exFAT specification (Microsoft, 2019): boot sector fields,
 * File / Stream Extension / File Name directory entry sets, the FAT and the
 * NoFatChain flag for contiguous allocations.  Everything read from disk is
 * validated before use.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "filesystem/exfat.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common/util.h"

#define EXFAT_EOC 0xFFFFFFFFu
#define EXFAT_BAD 0xFFFFFFF7u
#define MAX_DIR_ENTRIES (1u << 22)

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }

static int rd(struct exfat_vol *v, uint64_t off, void *buf, size_t len)
{
	uint8_t *b = buf;
	while (len) {
		ssize_t r = pread(v->fd, b, len, (off_t)(v->base + off));
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (r == 0) {
			errno = EIO;
			return -1;
		}
		b += r;
		off += (uint64_t)r;
		len -= (size_t)r;
	}
	return 0;
}

int exfat_open(struct exfat_vol *v, int fd, uint64_t base)
{
	memset(v, 0, sizeof(*v));
	v->fd = fd;
	v->base = base;
	uint8_t bs[512];
	if (rd(v, 0, bs, sizeof(bs)) < 0)
		return -1;
	if (memcmp(bs + 3, "EXFAT   ", 8) != 0 || bs[510] != 0x55 || bs[511] != 0xAA) {
		errno = EINVAL;
		return -1;
	}
	v->bps_shift = bs[108];
	v->spc_shift = bs[109];
	if (v->bps_shift < 9 || v->bps_shift > 12 || v->spc_shift > 25 - v->bps_shift) {
		errno = EINVAL;
		return -1;
	}
	v->fat_offset = (uint64_t)le32(bs + 80) << v->bps_shift;
	v->heap_offset = (uint64_t)le32(bs + 88) << v->bps_shift;
	v->cluster_count = le32(bs + 92);
	v->root_cluster = le32(bs + 96);
	v->serial = le32(bs + 100);
	v->cluster_size = 1ull << (v->bps_shift + v->spc_shift);
	v->volume_bytes = le64(bs + 72) << v->bps_shift;
	if (v->root_cluster < 2 || v->root_cluster >= v->cluster_count + 2) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

static bool cluster_valid(const struct exfat_vol *v, uint32_t c)
{
	return c >= 2 && c < v->cluster_count + 2;
}

static uint64_t cluster_offset(const struct exfat_vol *v, uint32_t c)
{
	return v->heap_offset + (uint64_t)(c - 2) * v->cluster_size;
}

static int fat_next(struct exfat_vol *v, uint32_t c, uint32_t *next)
{
	uint8_t e[4];
	if (rd(v, v->fat_offset + (uint64_t)c * 4, e, 4) < 0)
		return -1;
	*next = le32(e);
	return 0;
}

/* Build the cluster runs of an allocation as partition-relative extents. */
static int chain_extents(struct exfat_vol *v, uint32_t first, uint64_t length, bool contiguous, struct file_extent **out,
			 size_t *n)
{
	*out = NULL;
	*n = 0;
	if (length == 0)
		return 0;
	if (!cluster_valid(v, first)) {
		errno = EINVAL;
		return -1;
	}
	uint64_t clusters = (length + v->cluster_size - 1) / v->cluster_size;
	if (contiguous) {
		if (first + clusters > (uint64_t)v->cluster_count + 2) {
			errno = EINVAL;
			return -1;
		}
		*out = xmalloc(sizeof(struct file_extent));
		(*out)[0].offset = cluster_offset(v, first);
		(*out)[0].length = length;
		*n = 1;
		return 0;
	}
	size_t cap = 8;
	struct file_extent *ext = xmalloc(cap * sizeof(*ext));
	uint32_t c = first;
	uint64_t remaining = length;
	for (uint64_t i = 0; i < clusters; i++) {
		if (!cluster_valid(v, c)) {
			free(ext);
			errno = EINVAL;
			return -1;
		}
		uint64_t take = remaining < v->cluster_size ? remaining : v->cluster_size;
		uint64_t off = cluster_offset(v, c);
		if (*n && ext[*n - 1].offset + ext[*n - 1].length == off) {
			ext[*n - 1].length += take;
		} else {
			if (*n == cap) {
				cap *= 2;
				ext = xrealloc(ext, cap * sizeof(*ext));
			}
			ext[*n].offset = off;
			ext[*n].length = take;
			(*n)++;
		}
		remaining -= take;
		if (i + 1 < clusters) {
			uint32_t nx;
			if (fat_next(v, c, &nx) < 0) {
				free(ext);
				return -1;
			}
			if (nx == EXFAT_EOC || nx == EXFAT_BAD) {
				free(ext);
				errno = EINVAL;
				return -1;
			}
			c = nx;
		}
	}
	*out = ext;
	return 0;
}

struct dir_hit {
	uint32_t first_cluster;
	uint64_t data_length;
	bool no_fat_chain;
	bool is_dir;
};

static bool name_eq(const uint16_t *a, size_t alen, const char *utf8)
{
	/* Decode utf8 to UTF-16 and compare case-insensitively for ASCII. */
	size_t i = 0;
	const unsigned char *s = (const unsigned char *)utf8;
	while (*s) {
		uint32_t cp;
		if (*s < 0x80) {
			cp = *s++;
		} else if ((*s & 0xE0) == 0xC0 && s[1]) {
			cp = ((uint32_t)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
			s += 2;
		} else if ((*s & 0xF0) == 0xE0 && s[1] && s[2]) {
			cp = ((uint32_t)(s[0] & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
			s += 3;
		} else {
			return false; /* 4-byte sequences are not used in ISO names we handle */
		}
		if (i >= alen)
			return false;
		uint32_t x = a[i++];
		if (x != cp) {
			uint32_t lx = (x >= 'A' && x <= 'Z') ? x + 32 : x;
			uint32_t lc = (cp >= 'A' && cp <= 'Z') ? cp + 32 : cp;
			if (lx != lc)
				return false;
		}
	}
	return i == alen;
}

/* Search one directory for `name`. */
static int dir_lookup(struct exfat_vol *v, uint32_t first, uint64_t length, bool no_chain, const char *name, struct dir_hit *hit)
{
	struct file_extent *ext;
	size_t next;
	if (length == 0) {
		/* the root directory has no recorded length: walk its FAT chain */
		size_t cap = 8;
		ext = xmalloc(cap * sizeof(*ext));
		next = 0;
		uint32_t c = first;
		for (unsigned guard = 0; guard < 65536; guard++) {
			if (!cluster_valid(v, c))
				break;
			if (next == cap) {
				cap *= 2;
				ext = xrealloc(ext, cap * sizeof(*ext));
			}
			ext[next].offset = cluster_offset(v, c);
			ext[next].length = v->cluster_size;
			next++;
			uint32_t nx;
			if (fat_next(v, c, &nx) < 0 || nx == EXFAT_EOC)
				break;
			c = nx;
		}
	} else if (chain_extents(v, first, length, no_chain, &ext, &next) < 0) {
		return -1;
	}

	uint16_t namebuf[256];
	size_t namelen = 0, want_len = 0;
	int secondary_left = 0;
	struct dir_hit cur = { 0 };
	bool in_set = false;
	unsigned entries = 0;
	uint8_t *buf = xmalloc(65536);
	int rc = -1;
	errno = ENOENT;
	for (size_t x = 0; x < next; x++) {
		for (uint64_t pos = 0; pos < ext[x].length; pos += 65536) {
			size_t chunk = ext[x].length - pos < 65536 ? (size_t)(ext[x].length - pos) : 65536;
			if (rd(v, ext[x].offset + pos, buf, chunk) < 0)
				goto out;
			for (size_t e = 0; e + 32 <= chunk; e += 32) {
				if (++entries > MAX_DIR_ENTRIES)
					goto out;
				const uint8_t *d = buf + e;
				uint8_t type = d[0];
				if (type == 0x00) /* end of directory */
					goto out;
				if (!(type & 0x80)) { /* unused entry */
					in_set = false;
					continue;
				}
				if (type == 0x85) {
					in_set = true;
					secondary_left = d[1];
					memset(&cur, 0, sizeof(cur));
					cur.is_dir = (le16(d + 4) & 0x10) != 0;
					namelen = 0;
					want_len = 0;
					continue;
				}
				if (!in_set)
					continue;
				if (type == 0xC0) {
					cur.no_fat_chain = (d[1] & 0x02) != 0;
					want_len = d[3];
					cur.first_cluster = le32(d + 20);
					cur.data_length = le64(d + 24);
				} else if (type == 0xC1) {
					for (int k = 0; k < 15 && namelen < want_len; k++)
						namebuf[namelen++] = le16(d + 2 + 2 * k);
				}
				if (--secondary_left <= 0) {
					in_set = false;
					if (want_len && namelen == want_len && name_eq(namebuf, namelen, name)) {
						*hit = cur;
						rc = 0;
						goto out;
					}
				}
			}
		}
	}
out:
	free(buf);
	free(ext);
	return rc;
}

int exfat_file_extents(struct exfat_vol *v, const char *path, struct file_extent **ext, size_t *n, uint64_t *size)
{
	char *dup = xstrdup(path);
	char *save = NULL;
	uint32_t dir_first = v->root_cluster;
	uint64_t dir_len = 0;
	bool dir_nochain = false;
	struct dir_hit hit = { 0 };
	bool found_any = false;
	int rc = -1;
	for (char *tok = strtok_r(dup, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
		if (dir_lookup(v, dir_first, dir_len, dir_nochain, tok, &hit) < 0)
			goto out;
		found_any = true;
		dir_first = hit.first_cluster;
		dir_len = hit.data_length;
		dir_nochain = hit.no_fat_chain;
		if (!hit.is_dir) {
			char *rest = strtok_r(NULL, "/", &save);
			if (rest) {
				errno = ENOTDIR;
				goto out;
			}
			break;
		}
	}
	if (!found_any || hit.is_dir) {
		errno = EISDIR;
		goto out;
	}
	if (chain_extents(v, hit.first_cluster, hit.data_length, hit.no_fat_chain, ext, n) < 0)
		goto out;
	*size = hit.data_length;
	rc = 0;
out:
	free(dup);
	return rc;
}
