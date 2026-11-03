/*
 * Distro Buny — ISO 9660 reader
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "filesystem/iso9660.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/log.h"
#include "common/strbuf.h"

#define SECTOR_CACHE 64
#define DIR_CACHE 64
#define MAX_DIR_BYTES (64u << 20)     /* refuse absurd directory sizes */
#define MAX_SYMLINK_DEPTH 8
#define MAX_CE_CHAIN 16

struct sector_slot {
	uint64_t lba;
	bool valid;
	uint8_t data[ISO_SECTOR];
};

struct dir_cache_slot {
	uint64_t lba;      /* key: first sector of the directory extent */
	struct iso_dir dir;
	bool valid;
	uint64_t stamp;
};

struct iso_image {
	int fd;
	bool own_fd;
	uint64_t base, size;
	struct iso_info info;
	struct iso_extent root;         /* root directory extent */
	bool use_joliet;                /* names from the Joliet SVD tree */
	struct iso_extent joliet_root;
	unsigned susp_skip;             /* SUSP skip bytes from SP entry */
	struct sector_slot sectors[SECTOR_CACHE];
	struct dir_cache_slot dirs[DIR_CACHE];
	uint64_t stamp;
};

/* ------------------------------------------------------------------ I/O */

static int raw_read(struct iso_image *img, uint64_t off, void *buf, size_t len)
{
	if (off > img->size || len > img->size - off)
		return -1;
	uint8_t *p = buf;
	while (len) {
		ssize_t r = pread(img->fd, p, len, (off_t)(img->base + off));
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (r == 0)
			return -1;
		p += r;
		off += (uint64_t)r;
		len -= (size_t)r;
	}
	return 0;
}

static const uint8_t *get_sector(struct iso_image *img, uint64_t lba)
{
	struct sector_slot *s = &img->sectors[lba % SECTOR_CACHE];
	if (s->valid && s->lba == lba)
		return s->data;
	if (raw_read(img, lba * ISO_SECTOR, s->data, ISO_SECTOR) < 0) {
		s->valid = false;
		return NULL;
	}
	s->valid = true;
	s->lba = lba;
	return s->data;
}

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static void copy_achars(char *dst, const uint8_t *src, size_t n)
{
	memcpy(dst, src, n);
	dst[n] = '\0';
	/* trim trailing spaces and NULs */
	size_t l = n;
	while (l > 0 && (dst[l - 1] == ' ' || dst[l - 1] == '\0'))
		dst[--l] = '\0';
	/* replace non-printable characters to keep labels safe for display/cmdlines */
	for (size_t i = 0; i < l; i++)
		if ((unsigned char)dst[i] < 0x20 || (unsigned char)dst[i] == 0x7f)
			dst[i] = '_';
}

static bool date_nonzero(const uint8_t *d)
{
	for (int i = 0; i < 16; i++)
		if (d[i] != '0' && d[i] != 0)
			return true;
	return false;
}

static void format_uuid(char *out, const uint8_t *d)
{
	/* d: "YYYYMMDDHHMMSScc" ASCII digits */
	snprintf(out, 24, "%.4s-%.2s-%.2s-%.2s-%.2s-%.2s-%.2s", d, d + 4, d + 6, d + 8, d + 10, d + 12, d + 14);
	for (char *p = out; *p; p++)
		if (!((*p >= '0' && *p <= '9') || *p == '-'))
			*p = '0';
}

/* ------------------------------------------------------------- names */

static void ucs2be_to_utf8(struct strbuf *sb, const uint8_t *p, size_t n)
{
	for (size_t i = 0; i + 1 < n; i += 2) {
		uint32_t cp = (uint32_t)p[i] << 8 | p[i + 1];
		if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < n) {
			uint32_t lo = (uint32_t)p[i + 2] << 8 | p[i + 3];
			if (lo >= 0xDC00 && lo <= 0xDFFF) {
				cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
				i += 2;
			}
		}
		if (cp == 0)
			break;
		char b[4];
		if (cp < 0x80) {
			sb_addc(sb, (char)cp);
		} else if (cp < 0x800) {
			b[0] = (char)(0xC0 | (cp >> 6));
			b[1] = (char)(0x80 | (cp & 0x3F));
			sb_addn(sb, b, 2);
		} else if (cp < 0x10000) {
			b[0] = (char)(0xE0 | (cp >> 12));
			b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
			b[2] = (char)(0x80 | (cp & 0x3F));
			sb_addn(sb, b, 3);
		} else {
			b[0] = (char)(0xF0 | (cp >> 18));
			b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
			b[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
			b[3] = (char)(0x80 | (cp & 0x3F));
			sb_addn(sb, b, 4);
		}
	}
}

static void strip_version(char *s, bool lower)
{
	char *semi = strrchr(s, ';');
	if (semi)
		*semi = '\0';
	size_t n = strlen(s);
	if (n > 1 && s[n - 1] == '.')
		s[n - 1] = '\0';
	if (lower) {
		/* Plain ISO 9660 names are upper case; present them lower-cased like
		 * Linux's isofs "map=normal" does, lookups are case-insensitive anyway. */
		for (char *p = s; *p; p++)
			if (*p >= 'A' && *p <= 'Z')
				*p = (char)(*p - 'A' + 'a');
	}
}

/* ------------------------------------------------------- Rock Ridge */

struct rr_result {
	struct strbuf name;
	bool have_name;
	struct strbuf link;
	bool have_link;
	bool link_continue;   /* last SL component had CONTINUE flag */
};

static void rr_parse_area(struct iso_image *img, const uint8_t *su, size_t len, struct rr_result *rr, int depth);

static void rr_parse_sl(const uint8_t *e, size_t elen, struct rr_result *rr)
{
	/* e points at the SL entry; component records start at offset 5 */
	size_t pos = 5;
	while (pos + 2 <= elen) {
		uint8_t flags = e[pos];
		uint8_t clen = e[pos + 1];
		if (pos + 2 + clen > elen)
			break;
		const char *comp = (const char *)e + pos + 2;
		if (rr->have_link && !rr->link_continue && rr->link.len && rr->link.buf[rr->link.len - 1] != '/')
			sb_addc(&rr->link, '/');
		if (flags & 0x02)
			sb_add(&rr->link, ".");
		else if (flags & 0x04)
			sb_add(&rr->link, "..");
		else if (flags & 0x08) {
			sb_reset(&rr->link);
			sb_addc(&rr->link, '/');
		} else
			sb_addn(&rr->link, comp, clen);
		rr->have_link = true;
		rr->link_continue = (flags & 0x01) != 0;
		pos += 2u + clen;
	}
}

static void rr_parse_area(struct iso_image *img, const uint8_t *su, size_t len, struct rr_result *rr, int depth)
{
	size_t pos = 0;
	while (pos + 4 <= len) {
		const uint8_t *e = su + pos;
		uint8_t elen = e[2];
		if (elen < 4 || pos + elen > len)
			break;
		if (e[0] == 'N' && e[1] == 'M' && elen >= 5) {
			uint8_t flags = e[4];
			if (!(flags & 0x06)) {
				sb_addn(&rr->name, (const char *)e + 5, (size_t)elen - 5);
				rr->have_name = true;
			}
		} else if (e[0] == 'S' && e[1] == 'L' && elen >= 5) {
			rr_parse_sl(e, elen, rr);
		} else if (e[0] == 'C' && e[1] == 'E' && elen >= 28 && depth < MAX_CE_CHAIN) {
			uint64_t blk = le32(e + 4), off = le32(e + 12), clen = le32(e + 20);
			if (clen > 0 && clen <= 65536 && off < ISO_SECTOR * 32u) {
				uint8_t *buf = xmalloc(clen);
				if (raw_read(img, blk * ISO_SECTOR + off, buf, clen) == 0)
					rr_parse_area(img, buf, clen, rr, depth + 1);
				free(buf);
			}
		} else if (e[0] == 'S' && e[1] == 'T') {
			break;
		}
		pos += elen;
	}
}

/* ------------------------------------------------------ directories */

static void free_dir(struct iso_dir *d)
{
	for (size_t i = 0; i < d->n; i++) {
		free(d->nodes[i].name);
		free(d->nodes[i].link_target);
		free(d->nodes[i].ext);
	}
	free(d->nodes);
	d->nodes = NULL;
	d->n = 0;
}

/* Parse one directory extent into a node array. */
static int parse_dir(struct iso_image *img, const struct iso_extent *ex, bool joliet, struct iso_dir *out)
{
	out->nodes = NULL;
	out->n = 0;
	if (ex->len == 0 || ex->len > MAX_DIR_BYTES)
		return -1;
	size_t cap = 0;
	uint64_t nsect = (ex->len + ISO_SECTOR - 1) / ISO_SECTOR;
	struct iso_node *pending = NULL; /* multi-extent accumulation */

	for (uint64_t s = 0; s < nsect; s++) {
		const uint8_t *sec = get_sector(img, ex->lba + s);
		if (!sec) {
			free_dir(out);
			return -1;
		}
		uint8_t copy[ISO_SECTOR];
		memcpy(copy, sec, ISO_SECTOR); /* cache slot may be reused during CE reads */
		size_t pos = 0;
		while (pos < ISO_SECTOR) {
			const uint8_t *r = copy + pos;
			uint8_t rlen = r[0];
			if (rlen == 0)
				break; /* padding to end of sector */
			if (rlen < 34 || pos + rlen > ISO_SECTOR)
				break;
			uint8_t namelen = r[32];
			if (33u + namelen > rlen)
				break;
			uint8_t flags = r[25];
			uint64_t lba = le32(r + 2);
			uint64_t dlen = le32(r + 10);
			pos += rlen;

			/* skip "." and ".." */
			if (namelen == 1 && (r[33] == 0 || r[33] == 1))
				continue;

			struct rr_result rr = { STRBUF_INIT, false, STRBUF_INIT, false, false };
			if (!joliet && img->info.rock_ridge) {
				size_t su_off = 33u + namelen + ((namelen & 1) ? 0u : 1u) + img->susp_skip;
				if (su_off < rlen)
					rr_parse_area(img, r + su_off, (size_t)rlen - su_off, &rr, 0);
			}

			char *name;
			if (rr.have_name) {
				name = sb_detach(&rr.name);
			} else if (joliet) {
				struct strbuf sb = STRBUF_INIT;
				ucs2be_to_utf8(&sb, r + 33, namelen);
				name = sb_detach(&sb);
				strip_version(name, false);
			} else {
				name = xstrndup((const char *)r + 33, namelen);
				strip_version(name, true);
			}
			sb_free(&rr.name);

			if (!name[0] || strchr(name, '/')) {
				/* names containing '/' cannot be addressed; ignore safely */
				free(name);
				sb_free(&rr.link);
				continue;
			}

			/* Continuation of a multi-extent file: same name as pending. */
			if (pending && strcmp(pending->name, name) == 0) {
				pending->ext = xrealloc(pending->ext, (pending->next + 1) * sizeof(struct iso_extent));
				pending->ext[pending->next].lba = lba;
				pending->ext[pending->next].len = dlen;
				pending->next++;
				pending->size += dlen;
				free(name);
				sb_free(&rr.link);
				if (!(flags & 0x80))
					pending = NULL;
				continue;
			}
			pending = NULL;

			if (out->n == cap) {
				cap = cap ? cap * 2 : 32;
				out->nodes = xrealloc(out->nodes, cap * sizeof(struct iso_node));
			}
			struct iso_node *nd = &out->nodes[out->n++];
			memset(nd, 0, sizeof(*nd));
			nd->name = name;
			nd->is_dir = (flags & 0x02) != 0;
			nd->ext = xmalloc(sizeof(struct iso_extent));
			nd->ext[0].lba = lba;
			nd->ext[0].len = dlen;
			nd->next = 1;
			nd->size = dlen;
			if (rr.have_link) {
				nd->is_symlink = true;
				nd->link_target = sb_detach(&rr.link);
			}
			sb_free(&rr.link);
			if (flags & 0x80)
				pending = nd;
		}
	}
	return 0;
}

static const struct iso_dir *get_dir(struct iso_image *img, const struct iso_extent *ex)
{
	struct dir_cache_slot *victim = NULL;
	for (int i = 0; i < DIR_CACHE; i++) {
		struct dir_cache_slot *s = &img->dirs[i];
		if (s->valid && s->lba == ex->lba) {
			s->stamp = ++img->stamp;
			return &s->dir;
		}
		if (!victim || !s->valid || (victim->valid && s->stamp < victim->stamp))
			victim = s;
	}
	if (victim->valid)
		free_dir(&victim->dir);
	victim->valid = false;
	if (parse_dir(img, ex, img->use_joliet, &victim->dir) < 0)
		return NULL;
	victim->valid = true;
	victim->lba = ex->lba;
	victim->stamp = ++img->stamp;
	return &victim->dir;
}

/* ----------------------------------------------------------- opening */

struct iso_image *iso_open(int fd, uint64_t base, uint64_t size, char *err, size_t errlen)
{
	if (err && errlen)
		err[0] = '\0';
	struct iso_image *img = xcalloc(1, sizeof(*img));
	img->fd = fd;
	img->base = base;
	img->size = size;

	bool have_pvd = false;
	for (uint64_t lba = 16; lba < 16 + 64; lba++) {
		uint8_t vd[ISO_SECTOR];
		if (raw_read(img, lba * ISO_SECTOR, vd, sizeof(vd)) < 0)
			break;
		if (memcmp(vd + 1, "CD001", 5) != 0)
			break;
		uint8_t type = vd[0];
		if (type == 255)
			break;
		if (type == 0 && memcmp(vd + 7, "EL TORITO SPECIFICATION", 23) == 0)
			img->info.el_torito = true;
		if (type == 1 && !have_pvd) {
			have_pvd = true;
			copy_achars(img->info.system_id, vd + 8, 32);
			copy_achars(img->info.volume_id, vd + 40, 32);
			copy_achars(img->info.volume_set_id, vd + 190, 128);
			copy_achars(img->info.publisher_id, vd + 318, 128);
			copy_achars(img->info.preparer_id, vd + 446, 128);
			copy_achars(img->info.application_id, vd + 574, 128);
			uint16_t bs = le16(vd + 128);
			if (bs != ISO_SECTOR) {
				snprintf(err, errlen, "unsupported logical block size %u", bs);
				free(img);
				return NULL;
			}
			img->info.volume_bytes = (uint64_t)le32(vd + 80) * bs;
			const uint8_t *root = vd + 156;
			img->root.lba = le32(root + 2);
			img->root.len = le32(root + 10);
			const uint8_t *cdate = vd + 813, *mdate = vd + 830;
			format_uuid(img->info.uuid, date_nonzero(mdate) ? mdate : cdate);
			if (date_nonzero(cdate))
				snprintf(img->info.created, sizeof(img->info.created), "%.4s-%.2s-%.2s %.2s:%.2s:%.2s",
					 cdate, cdate + 4, cdate + 6, cdate + 8, cdate + 10, cdate + 12);
		} else if (type == 2) {
			/* Joliet escape sequences at offset 88 */
			const uint8_t *esc = vd + 88;
			if (esc[0] == '%' && esc[1] == '/' && (esc[2] == '@' || esc[2] == 'C' || esc[2] == 'E')) {
				img->info.joliet = true;
				const uint8_t *root = vd + 156;
				img->joliet_root.lba = le32(root + 2);
				img->joliet_root.len = le32(root + 10);
			}
		}
	}
	if (!have_pvd) {
		snprintf(err, errlen, "no ISO 9660 primary volume descriptor");
		free(img);
		return NULL;
	}

	/* Rock Ridge detection: SP entry in the root's "." record. */
	const uint8_t *sec = get_sector(img, img->root.lba);
	if (sec) {
		uint8_t rlen = sec[0];
		uint8_t namelen = sec[32];
		size_t su_off = 33u + namelen + ((namelen & 1) ? 0u : 1u);
		if (rlen >= 34 && su_off + 7 <= rlen) {
			const uint8_t *su = sec + su_off;
			if (su[0] == 'S' && su[1] == 'P' && su[4] == 0xBE && su[5] == 0xEF) {
				img->susp_skip = su[6];
				/* Confirm RR by scanning for RR/NM/PX entries in the root record. */
				img->info.rock_ridge = true;
			}
		}
	}
	if (!img->info.rock_ridge && img->info.joliet) {
		img->use_joliet = true;
		img->root = img->joliet_root;
	}
	return img;
}

struct iso_image *iso_open_path(const char *path, char *err, size_t errlen)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		snprintf(err, errlen, "cannot open: %s", strerror(errno));
		return NULL;
	}
	struct stat st;
	if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
		snprintf(err, errlen, "not a regular file");
		close(fd);
		return NULL;
	}
	struct iso_image *img = iso_open(fd, 0, (uint64_t)st.st_size, err, errlen);
	if (!img) {
		close(fd);
		return NULL;
	}
	img->own_fd = true;
	return img;
}

void iso_close(struct iso_image *img)
{
	if (!img)
		return;
	for (int i = 0; i < DIR_CACHE; i++)
		if (img->dirs[i].valid)
			free_dir(&img->dirs[i].dir);
	if (img->own_fd)
		close(img->fd);
	free(img);
}

const struct iso_info *iso_get_info(const struct iso_image *img)
{
	return &img->info;
}

/* ------------------------------------------------------------ lookup */

static const struct iso_node *find_in_dir(const struct iso_dir *d, const char *name)
{
	for (size_t i = 0; i < d->n; i++)
		if (strcmp(d->nodes[i].name, name) == 0)
			return &d->nodes[i];
	for (size_t i = 0; i < d->n; i++)
		if (strcasecmp(d->nodes[i].name, name) == 0)
			return &d->nodes[i];
	return NULL;
}

/* Normalise path into components, resolving "." and "..". */
static size_t split_path(const char *path, char ***comps_out)
{
	char *dup = xstrdup(path);
	char **comps = NULL;
	size_t n = 0, cap = 0;
	for (char *tok = strtok(dup, "/"); tok; tok = strtok(NULL, "/")) {
		if (strcmp(tok, ".") == 0 || !*tok)
			continue;
		if (strcmp(tok, "..") == 0) {
			if (n > 0)
				free(comps[--n]);
			continue;
		}
		if (n == cap) {
			cap = cap ? cap * 2 : 8;
			comps = xrealloc(comps, cap * sizeof(char *));
		}
		comps[n++] = xstrdup(tok);
	}
	free(dup);
	*comps_out = comps;
	return n;
}

static void free_comps(char **c, size_t n)
{
	for (size_t i = 0; i < n; i++)
		free(c[i]);
	free(c);
}

/* Walks the path; on success copies the final node's metadata into *out.
 * Directory extents are copied to locals before each get_dir() call because
 * get_dir() may evict the cache slot that holds the current node. */
static int lookup_depth(struct iso_image *img, const char *path, struct iso_file *out, struct strbuf *canon, int depth)
{
	if (depth > MAX_SYMLINK_DEPTH)
		return -1;
	char **comps;
	size_t n = split_path(path, &comps);
	struct iso_extent cur_ext = img->root;
	bool cur_is_dir = true;
	struct iso_file result = { true, img->root.len, 0, NULL };
	if (canon)
		sb_reset(canon);
	for (size_t i = 0; i < n; i++) {
		if (!cur_is_dir)
			goto fail;
		const struct iso_dir *d = get_dir(img, &cur_ext);
		if (!d)
			goto fail;
		const struct iso_node *nd = find_in_dir(d, comps[i]);
		if (!nd)
			goto fail;
		if (nd->is_symlink && nd->link_target) {
			struct strbuf np = STRBUF_INIT;
			if (nd->link_target[0] != '/') {
				for (size_t k = 0; k < i; k++) {
					sb_addc(&np, '/');
					sb_add(&np, comps[k]);
				}
				sb_addc(&np, '/');
			}
			sb_add(&np, nd->link_target);
			for (size_t k = i + 1; k < n; k++) {
				sb_addc(&np, '/');
				sb_add(&np, comps[k]);
			}
			free_comps(comps, n);
			free(result.ext);
			int rc = lookup_depth(img, sb_str(&np), out, canon, depth + 1);
			sb_free(&np);
			return rc;
		}
		if (canon) {
			sb_addc(canon, '/');
			sb_add(canon, nd->name);
		}
		cur_is_dir = nd->is_dir;
		cur_ext = nd->ext[0];
		if (i + 1 == n) {
			result.is_dir = nd->is_dir;
			result.size = nd->size;
			result.next = nd->next;
			result.ext = xmalloc(nd->next * sizeof(struct iso_extent));
			memcpy(result.ext, nd->ext, nd->next * sizeof(struct iso_extent));
		}
	}
	free_comps(comps, n);
	if (!result.ext) { /* the root itself */
		result.next = 1;
		result.ext = xmalloc(sizeof(struct iso_extent));
		result.ext[0] = img->root;
	}
	*out = result;
	return 0;
fail:
	free_comps(comps, n);
	free(result.ext);
	return -1;
}

int iso_lookup(struct iso_image *img, const char *path, struct iso_file *out)
{
	memset(out, 0, sizeof(*out));
	return lookup_depth(img, path, out, NULL, 0);
}

void iso_file_free(struct iso_file *f)
{
	free(f->ext);
	f->ext = NULL;
	f->next = 0;
}

bool iso_exists(struct iso_image *img, const char *path)
{
	struct iso_file f;
	if (iso_lookup(img, path, &f) < 0)
		return false;
	iso_file_free(&f);
	return true;
}

bool iso_is_dir(struct iso_image *img, const char *path)
{
	struct iso_file f;
	if (iso_lookup(img, path, &f) < 0)
		return false;
	bool d = f.is_dir;
	iso_file_free(&f);
	return d;
}

char *iso_canonical_path(struct iso_image *img, const char *path)
{
	struct strbuf sb = STRBUF_INIT;
	struct iso_file f;
	if (lookup_depth(img, path, &f, &sb, 0) < 0) {
		sb_free(&sb);
		return NULL;
	}
	iso_file_free(&f);
	if (sb.len == 0)
		sb_addc(&sb, '/');
	return sb_detach(&sb);
}

const struct iso_dir *iso_list(struct iso_image *img, const char *path)
{
	struct iso_file f;
	if (iso_lookup(img, path, &f) < 0)
		return NULL;
	struct iso_extent ex = f.ext[0];
	bool dir = f.is_dir;
	iso_file_free(&f);
	return dir ? get_dir(img, &ex) : NULL;
}

ssize_t iso_pread(struct iso_image *img, const struct iso_file *node, void *buf, size_t len, uint64_t off)
{
	if (off >= node->size)
		return 0;
	if (len > node->size - off)
		len = (size_t)(node->size - off);
	size_t done = 0;
	uint64_t ext_start = 0;
	for (uint32_t i = 0; i < node->next && done < len; i++) {
		const struct iso_extent *e = &node->ext[i];
		uint64_t ext_end = ext_start + e->len;
		uint64_t cur = off + done;
		if (cur < ext_end) {
			uint64_t within = cur - ext_start;
			size_t take = (size_t)BUNY_MIN((uint64_t)(len - done), e->len - within);
			if (raw_read(img, e->lba * ISO_SECTOR + within, (uint8_t *)buf + done, take) < 0)
				return -1;
			done += take;
		}
		ext_start = ext_end;
	}
	return (ssize_t)done;
}

char *iso_read_text(struct iso_image *img, const char *path, size_t max, size_t *len_out)
{
	struct iso_file f;
	if (iso_lookup(img, path, &f) < 0)
		return NULL;
	if (f.is_dir || f.size > max) {
		iso_file_free(&f);
		return NULL;
	}
	char *buf = xmalloc((size_t)f.size + 1);
	ssize_t r = iso_pread(img, &f, buf, (size_t)f.size, 0);
	uint64_t want = f.size;
	iso_file_free(&f);
	if (r < 0 || (uint64_t)r != want) {
		free(buf);
		return NULL;
	}
	buf[r] = '\0';
	if (len_out)
		*len_out = (size_t)r;
	return buf;
}

int iso_copy_to_fd(struct iso_image *img, const struct iso_file *node, int outfd, iso_progress_fn cb, void *ctx)
{
	const size_t chunk = 1u << 20;
	uint8_t *buf = xmalloc(chunk);
	uint64_t off = 0;
	int rc = 0;
	while (off < node->size) {
		ssize_t r = iso_pread(img, node, buf, chunk, off);
		if (r <= 0) {
			rc = -1;
			break;
		}
		size_t w = 0;
		while (w < (size_t)r) {
			ssize_t x = write(outfd, buf + w, (size_t)r - w);
			if (x < 0) {
				if (errno == EINTR)
					continue;
				rc = -1;
				goto out;
			}
			w += (size_t)x;
		}
		off += (uint64_t)r;
		if (cb)
			cb(ctx, off, node->size);
	}
out:
	free(buf);
	return rc;
}

bool iso_wildmatch(const char *pat, const char *str, bool icase)
{
	/* iterative '*' / '?' matcher with backtracking */
	const char *star = NULL, *ss = NULL;
	while (*str) {
		char pc = *pat, sc = *str;
		if (icase) {
			if (pc >= 'A' && pc <= 'Z')
				pc = (char)(pc - 'A' + 'a');
			if (sc >= 'A' && sc <= 'Z')
				sc = (char)(sc - 'A' + 'a');
		}
		if (*pat == '*') {
			star = pat++;
			ss = str;
		} else if (*pat == '?' || (pc == sc && *pat)) {
			pat++;
			str++;
		} else if (star) {
			pat = star + 1;
			str = ++ss;
		} else {
			return false;
		}
	}
	while (*pat == '*')
		pat++;
	return *pat == '\0';
}

static void glob_rec(struct iso_image *img, const char *base, char **comps, size_t i, size_t n, struct ptrvec *out)
{
	if (i == n) {
		ptrvec_push(out, xstrdup(*base ? base : "/"));
		return;
	}
	bool wild = strpbrk(comps[i], "*?") != NULL;
	if (!wild) {
		char *p = xasprintf("%s/%s", base, comps[i]);
		struct iso_file f;
		if (iso_lookup(img, p, &f) == 0) {
			bool ok = i + 1 == n || f.is_dir;
			iso_file_free(&f);
			if (ok)
				glob_rec(img, p, comps, i + 1, n, out);
		}
		free(p);
		return;
	}
	const struct iso_dir *d = iso_list(img, *base ? base : "/");
	if (!d)
		return;
	/* copy names first: recursion may evict this directory from the cache */
	size_t cnt = d->n;
	char **names = xcalloc(cnt ? cnt : 1, sizeof(char *));
	bool *isdir = xcalloc(cnt ? cnt : 1, sizeof(bool));
	for (size_t k = 0; k < cnt; k++) {
		names[k] = xstrdup(d->nodes[k].name);
		isdir[k] = d->nodes[k].is_dir || d->nodes[k].is_symlink;
	}
	for (size_t k = 0; k < cnt; k++) {
		if (!iso_wildmatch(comps[i], names[k], false))
			continue;
		if (i + 1 < n && !isdir[k])
			continue;
		char *p = xasprintf("%s/%s", base, names[k]);
		glob_rec(img, p, comps, i + 1, n, out);
		free(p);
	}
	for (size_t k = 0; k < cnt; k++)
		free(names[k]);
	free(names);
	free(isdir);
}

void iso_glob(struct iso_image *img, const char *pattern, struct ptrvec *out)
{
	char **comps;
	size_t n = split_path(pattern, &comps);
	size_t before = out->n;
	glob_rec(img, "", comps, 0, n, out);
	/* sort new results for deterministic behaviour */
	for (size_t a = before; a < out->n; a++)
		for (size_t b = a + 1; b < out->n; b++)
			if (strcmp(out->v[a], out->v[b]) > 0) {
				void *t = out->v[a];
				out->v[a] = out->v[b];
				out->v[b] = t;
			}
	free_comps(comps, n);
}
