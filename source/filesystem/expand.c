/*
 * Distro Buny — growing the data partition to the end of its drive
 *
 * See expand.h for what is changed.  The exFAT structures follow the exFAT
 * specification (Microsoft, 2019), the partition table the UEFI
 * specification (chapter 5).  Everything read from the disk is validated
 * before it is used; anything unexpected refuses the expansion with a reason
 * instead of guessing.
 *
 * Crash safety.  Every step is idempotent and derived from what is on the
 * disk, and the order makes every intermediate state either harmless or
 * recognisable:
 *
 *   1. GPT: the backup copy is written at the new end of the disk first (an
 *      area nothing uses), then the primary copy, entries before header.
 *      Until the primary header is written the old table stays valid; a torn
 *      primary is detected by its CRCs and rebuilt from the new backup.  A
 *      partition larger than its file system is valid.
 *   2. exFAT, marked VolumeDirty for the whole step so other systems check
 *      the volume if the step is interrupted: FAT entries and bitmap bits of
 *      the new clusters (beyond the old volume: unused until the end), then
 *      the bitmap's FAT link and directory entry, then the backup boot
 *      region, then the main boot region with VolumeDirty cleared, written
 *      as one 6 KiB write.  A torn main region fails its checksum and the
 *      (already new) backup describes the volume.
 *
 * expand_plan() recognises the interrupted states (a damaged primary GPT or
 * main boot region, or a partition already larger than its volume) as
 * EXPAND_RESUME, and expand_run() finishes them with the same steps.  A
 * dirty volume is refused unless an expansion is in progress.  File data,
 * other partitions and clusters in use are never written.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "filesystem/expand.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "common/util.h"
#include "filesystem/blockdev.h"

#define SECTOR 512u
#define MIB_SECTORS 2048u
#define GPT_ENTRY 128u
#define EXFAT_EOC 0xFFFFFFFFu
#define EXFAT_MAX_CLUSTERS 0xFFFFFFF5u
#define VOLUME_DIRTY 0x0002u
#define MIN_GAIN (64ull << 20)    /* smaller gains are not worth offering */
#define CHUNK (1u << 20)
#define MAX_ROOT_CLUSTERS 64u

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

static int pread_full(int fd, void *buf, size_t len, uint64_t off)
{
	uint8_t *b = buf;
	while (len) {
		ssize_t r = pread(fd, b, len, (off_t)off);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0) {
			if (r == 0)
				errno = EIO;
			return -1;
		}
		b += r;
		off += (uint64_t)r;
		len -= (size_t)r;
	}
	return 0;
}

static int pwrite_full(int fd, const void *buf, size_t len, uint64_t off)
{
	const uint8_t *b = buf;
	while (len) {
		ssize_t r = pwrite(fd, b, len, (off_t)off);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0) {
			if (r == 0)
				errno = EIO;
			return -1;
		}
		b += r;
		off += (uint64_t)r;
		len -= (size_t)r;
	}
	return 0;
}

static uint32_t crc32_ieee(const uint8_t *b, size_t n)
{
	uint32_t c = 0xFFFFFFFFu;
	for (size_t i = 0; i < n; i++) {
		c ^= b[i];
		for (int k = 0; k < 8; k++)
			c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
	}
	return ~c;
}

const char *expand_state_name(enum expand_state s)
{
	switch (s) {
	case EXPAND_NONE: return "none";
	case EXPAND_POSSIBLE: return "possible";
	case EXPAND_RESUME: return "resume";
	case EXPAND_REFUSED: return "refused";
	}
	return "?";
}

void expand_plan_free(struct expand_plan *p)
{
	free(p->gpt_table);
	p->gpt_table = NULL;
}

static int refuse(struct expand_plan *p, const char *code, const char *fmt, ...) BUNY_PRINTF(3, 4);
static int refuse(struct expand_plan *p, const char *code, const char *fmt, ...)
{
	p->state = EXPAND_REFUSED;
	p->code = code;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(p->reason, sizeof(p->reason), fmt, ap);
	va_end(ap);
	return 0;
}

/* ------------------------------------------------------------------ GPT */

static uint32_t entry_sectors(uint32_t n)
{
	return (n * GPT_ENTRY + SECTOR - 1) / SECTOR;
}

/* Read and check the GPT header at `lba` and its entry array. */
static int gpt_load(int fd, uint64_t lba, uint64_t disk_sectors, uint8_t hdr[512], uint8_t **table, uint32_t *n)
{
	*table = NULL;
	if (pread_full(fd, hdr, SECTOR, lba * SECTOR) < 0 || memcmp(hdr, "EFI PART", 8) != 0)
		return -1;
	uint32_t hsize = le32(hdr + 12);
	if (hsize < 92 || hsize > SECTOR || le64(hdr + 24) != lba)
		return -1;
	uint8_t tmp[512];
	memcpy(tmp, hdr, hsize);
	put32(tmp + 16, 0);
	if (crc32_ieee(tmp, hsize) != le32(hdr + 16))
		return -1;
	*n = le32(hdr + 80);
	uint64_t ent_lba = le64(hdr + 72);
	if (le32(hdr + 84) != GPT_ENTRY || *n == 0 || *n > 1024 || ent_lba + entry_sectors(*n) > disk_sectors)
		return -1;
	size_t len = (size_t)*n * GPT_ENTRY;
	*table = xmalloc(len);
	if (pread_full(fd, *table, len, ent_lba * SECTOR) < 0 || crc32_ieee(*table, len) != le32(hdr + 88)) {
		free(*table);
		*table = NULL;
		return -1;
	}
	return 0;
}

static bool entry_used(const uint8_t *e)
{
	for (int i = 0; i < 16; i++)
		if (e[i])
			return true;
	return false;
}

static bool entry_named(const uint8_t *e, const char *name)
{
	size_t i = 0;
	for (; name[i]; i++)
		if (le16(e + 56 + 2 * i) != (unsigned char)name[i])
			return false;
	return i == 36 || le16(e + 56 + 2 * i) == 0;
}

/* A copy of `orig` describing a table at `entries_lba` with this header at `my_lba`. */
static void make_header(uint8_t out[512], const uint8_t *orig, uint64_t my_lba, uint64_t alt_lba, uint64_t entries_lba,
			uint64_t last_usable, uint32_t entries_crc)
{
	memcpy(out, orig, SECTOR);
	uint32_t hsize = le32(orig + 12);
	put64(out + 24, my_lba);
	put64(out + 32, alt_lba);
	put64(out + 48, last_usable);
	put64(out + 72, entries_lba);
	put32(out + 88, entries_crc);
	put32(out + 16, 0);
	put32(out + 16, crc32_ieee(out, hsize));
}

/* ---------------------------------------------------------------- exFAT */

static uint32_t boot_checksum(const uint8_t *r)
{
	uint32_t cs = 0;
	for (size_t i = 0; i < 11 * SECTOR; i++) {
		if (i == 106 || i == 107 || i == 112) /* VolumeFlags and PercentInUse */
			continue;
		cs = ((cs << 31) | (cs >> 1)) + r[i];
	}
	return cs;
}

static bool is_exfat(const uint8_t *r)
{
	return memcmp(r + 3, "EXFAT   ", 8) == 0;
}

/* A complete, consistent boot region of a volume that fits `part_sectors`. */
static bool boot_region_ok(const uint8_t *r, uint64_t part_sectors)
{
	if (!is_exfat(r) || r[510] != 0x55 || r[511] != 0xAA)
		return false;
	for (int i = 11; i < 64; i++) /* MustBeZero */
		if (r[i])
			return false;
	uint32_t cs = boot_checksum(r);
	for (size_t i = 0; i < SECTOR; i += 4)
		if (le32(r + 11 * SECTOR + i) != cs)
			return false;
	uint64_t vol = le64(r + 72);
	uint32_t fo = le32(r + 80), fl = le32(r + 84), ho = le32(r + 88), cc = le32(r + 92), root = le32(r + 96);
	unsigned spc = r[109];
	/* revision 1.x, 512-byte sectors (Distro Buny images), clusters up to 32 MiB, one FAT */
	if (r[105] != 1 || r[108] != 9 || spc > 16 || r[110] != 1)
		return false;
	if (fo < 24 || fl == 0 || (uint64_t)fo + fl > ho || cc == 0 || cc > EXFAT_MAX_CLUSTERS)
		return false;
	if (((uint64_t)cc + 2) * 4 > (uint64_t)fl * SECTOR || ho + ((uint64_t)cc << spc) > vol || vol > part_sectors)
		return false;
	return root >= 2 && root <= (uint64_t)cc + 1;
}

struct vol {
	int fd;
	uint64_t base;      /* partition start, bytes */
	uint64_t fat;       /* FAT start, bytes from the disk start */
	uint64_t heap;      /* cluster heap start, bytes from the disk start */
	uint64_t csize;
	uint32_t clusters;
};

static uint64_t cluster_at(const struct vol *v, uint32_t c)
{
	return v->heap + (uint64_t)(c - 2) * v->csize;
}

static int fat_get(const struct vol *v, uint32_t c, uint32_t *out)
{
	uint8_t e[4];
	if (pread_full(v->fd, e, 4, v->fat + (uint64_t)c * 4) < 0)
		return -1;
	*out = le32(e);
	return 0;
}

/* Find the allocation bitmap's directory entry in the root directory. */
static int find_bitmap(const struct vol *v, uint32_t root, uint32_t *first, uint64_t *len, uint64_t *entry_off)
{
	uint8_t *buf = xmalloc(v->csize);
	int rc = -1;
	uint32_t c = root;
	for (unsigned k = 0; k < MAX_ROOT_CLUSTERS && c >= 2 && c <= (uint64_t)v->clusters + 1; k++) {
		if (pread_full(v->fd, buf, v->csize, cluster_at(v, c)) < 0)
			break;
		for (uint64_t i = 0; i < v->csize; i += 32) {
			if (buf[i] == 0x00) /* end of directory */
				goto out;
			if (buf[i] == 0x81 && !(buf[i + 1] & 1)) {
				*first = le32(buf + i + 20);
				*len = le64(buf + i + 24);
				*entry_off = cluster_at(v, c) + i - v->base;
				rc = 0;
				goto out;
			}
		}
		if (fat_get(v, c, &c) < 0)
			break;
	}
out:
	free(buf);
	return rc;
}

static uint32_t popcount_bits(const uint8_t *b, uint64_t nbits)
{
	uint64_t n = 0;
	for (uint64_t i = 0; i < nbits / 8; i++)
		n += (uint64_t)__builtin_popcount(b[i]);
	if (nbits % 8)
		n += (uint64_t)__builtin_popcount(b[nbits / 8] & ((1u << (nbits % 8)) - 1));
	return (uint32_t)n;
}

/* ----------------------------------------------------------------- plan */

int expand_plan(int fd, uint64_t disk_bytes, unsigned sector_size, struct expand_plan *p)
{
	memset(p, 0, sizeof(*p));
	p->state = EXPAND_NONE;
	p->code = "none";
	p->disk_sectors = disk_bytes / SECTOR;
	uint64_t D = p->disk_sectors;
	if (D < 8 * MIB_SECTORS) {
		errno = EINVAL;
		return -1;
	}
	if (sector_size != SECTOR)
		return refuse(p, "sector-size", "This drive uses %u-byte sectors; Distro Buny drives use 512-byte sectors.", sector_size);

	/* ---- partition table */
	if (gpt_load(fd, 1, D, p->gpt_header, &p->gpt_table, &p->gpt_entries) < 0) {
		if (gpt_load(fd, D - 1, D, p->gpt_header, &p->gpt_table, &p->gpt_entries) < 0)
			return refuse(p, "gpt", "The partition table of this drive is damaged or missing.");
		p->primary_bad = true;
	}
	p->gpt_entry_size = GPT_ENTRY;
	uint32_t es = entry_sectors(p->gpt_entries);
	uint64_t first_usable = le64(p->gpt_header + 40);
	int idx = -1, found = 0;
	uint64_t max_end = 0;
	for (uint32_t i = 0; i < p->gpt_entries; i++) {
		const uint8_t *e = p->gpt_table + (size_t)i * GPT_ENTRY;
		if (!entry_used(e))
			continue;
		char type[37];
		guid_to_str(e, type);
		if (!strcasecmp(type, GPT_TYPE_MS_BASIC_DATA) && entry_named(e, "DISTROBUNY")) {
			idx = (int)i;
			found++;
		}
		if (le64(e + 40) > max_end)
			max_end = le64(e + 40);
	}
	if (found != 1)
		return refuse(p, "no-data", "This drive has no single DISTROBUNY partition.");
	const uint8_t *de = p->gpt_table + (size_t)idx * GPT_ENTRY;
	p->part_index = (unsigned)idx;
	p->part_first = le64(de + 32);
	p->part_last = le64(de + 40);
	if (first_usable < 2 + es || p->part_first < first_usable || p->part_last < p->part_first || max_end >= D - 1 - es)
		return refuse(p, "gpt", "The partition table of this drive is damaged.");
	uint64_t last_usable = D - 2 - es;
	uint64_t target_last = (last_usable + 1) / MIB_SECTORS * MIB_SECTORS - 1;
	if (target_last < p->part_last)
		target_last = p->part_last;
	bool unused = target_last > max_end && (target_last - max_end) * SECTOR >= MIN_GAIN;
	uint64_t part_sectors = p->part_last - p->part_first + 1;

	/* ---- file system */
	uint64_t base = p->part_first * SECTOR;
	uint8_t backup[12 * SECTOR];
	if (pread_full(fd, p->boot, sizeof(p->boot), base) < 0 || pread_full(fd, backup, sizeof(backup), base + 12 * SECTOR) < 0)
		return -1;
	if (!boot_region_ok(p->boot, part_sectors)) {
		if (!boot_region_ok(backup, part_sectors)) {
			if (!unused)
				return 0;
			if (!is_exfat(p->boot) && !is_exfat(backup))
				return refuse(p, "not-exfat", "DISTROBUNY does not hold an exFAT file system (was it formatted again?). "
					      "Only Distro Buny's exFAT file system can grow.");
			return refuse(p, "damaged", "The file system on DISTROBUNY is damaged. Let a Windows, macOS or Linux "
				      "computer check and repair it first.");
		}
		memcpy(p->boot, backup, sizeof(p->boot));
		p->boot_from_backup = true;
	}
	const uint8_t *bs = p->boot;
	p->vol_sectors = le64(bs + 72);
	p->fat_offset = le32(bs + 80);
	p->fat_length = le32(bs + 84);
	p->heap_offset = le32(bs + 88);
	p->clusters = le32(bs + 92);
	p->spc_shift = bs[109];
	p->data_bytes = p->new_data_bytes = p->vol_sectors * SECTOR;
	uint64_t csize = (uint64_t)SECTOR << p->spc_shift;
	/* The partition table step of an expansion is done: a partition larger than
	 * its volume (only an expansion leaves that on a Distro Buny drive).  UEFI
	 * firmware may already have rebuilt a torn primary GPT from the new backup
	 * by the time the runtime looks, so this alone means "interrupted". */
	bool table_grown = part_sectors > p->vol_sectors;
	bool dirty = !p->boot_from_backup && (le16(bs + 106) & VOLUME_DIRTY);
	bool interrupted = p->primary_bad || p->boot_from_backup || table_grown;
	if (!unused && !interrupted && !table_grown)
		return 0;

	/* ---- layout: nothing may follow DISTROBUNY */
	for (uint32_t i = 0; i < p->gpt_entries; i++) {
		const uint8_t *e = p->gpt_table + (size_t)i * GPT_ENTRY;
		if ((int)i != idx && entry_used(e) && le64(e + 40) >= p->part_first)
			return refuse(p, "layout", "On this drive the boot partition lies behind DISTROBUNY (an older Distro Buny "
				      "image), so DISTROBUNY cannot grow. To use the whole stick, copy your ISO files off it, "
				      "write the current starter image again and expand it then, or create the drive with buny-install.");
	}
	if (dirty && !table_grown)
		return refuse(p, "dirty", "DISTROBUNY was not ejected safely the last time it was used. Connect the drive to "
			      "a Windows, macOS or Linux computer, let it check the drive, eject it safely, and try again.");

	/* ---- the new size */
	struct vol v = { fd, base, base + (uint64_t)p->fat_offset * SECTOR, base + (uint64_t)p->heap_offset * SECTOR, csize, p->clusters };
	uint32_t bm_first;
	uint64_t bm_len, entry_off;
	if (find_bitmap(&v, le32(bs + 96), &bm_first, &bm_len, &entry_off) < 0)
		return refuse(p, "damaged", "The file system on DISTROBUNY has no readable allocation bitmap.");
	uint64_t N = p->clusters;
	uint64_t need0 = (N + 7) / 8;
	uint32_t k0 = (uint32_t)((need0 + csize - 1) / csize);
	if (bm_first < 2 || (uint64_t)bm_first + k0 - 1 > N + 1)
		return refuse(p, "damaged", "The allocation bitmap of DISTROBUNY lies outside the volume.");
	bool at_end = (uint64_t)bm_first + k0 - 1 == N + 1;

	uint64_t new_vol = target_last - p->part_first + 1;
	uint64_t m_disk = new_vol > p->heap_offset ? (new_vol - p->heap_offset) >> p->spc_shift : 0;
	uint64_t m = m_disk;
	uint64_t fat_cap = (uint64_t)(p->heap_offset - p->fat_offset) * SECTOR / 4 - 2;
	if (m > fat_cap)
		m = fat_cap;
	if (m > EXFAT_MAX_CLUSTERS)
		m = EXFAT_MAX_CLUSTERS;
	if (!at_end && m > (uint64_t)k0 * csize * 8) /* the bitmap can only grow inside its clusters */
		m = (uint64_t)k0 * csize * 8;
	if (m < N)
		m = N;
	if (m < m_disk) {
		p->fs_limited = true;
		uint64_t need = p->heap_offset + (m << p->spc_shift);
		new_vol = (need + MIB_SECTORS - 1) / MIB_SECTORS * MIB_SECTORS;
		if (new_vol > target_last - p->part_first + 1)
			new_vol = target_last - p->part_first + 1;
	}
	if (new_vol < part_sectors) /* never shrink the partition */
		new_vol = part_sectors;
	uint64_t need1 = (m + 7) / 8;
	uint32_t k1 = (uint32_t)((need1 + csize - 1) / csize);
	if (bm_len != need0 && !(table_grown && bm_len == need1))
		return refuse(p, "damaged", "The allocation bitmap of DISTROBUNY has an unexpected size.");
	/* the bitmap's clusters form a chain of consecutive clusters */
	for (uint32_t i = 0; i < k0; i++) {
		uint32_t next;
		if (fat_get(&v, bm_first + i, &next) < 0)
			return -1;
		bool last = i + 1 == k0;
		bool ok = last ? next == EXFAT_EOC || (table_grown && k1 > k0 && at_end && next == N + 2) : next == bm_first + i + 1;
		if (!ok)
			return refuse(p, "damaged", "The allocation bitmap of DISTROBUNY is not stored in one piece.");
	}
	uint8_t *bm = xmalloc(need0);
	if (pread_full(fd, bm, need0, cluster_at(&v, bm_first)) < 0) {
		free(bm);
		return -1;
	}
	for (uint32_t c = bm_first; c < bm_first + k0; c++)
		if (!(bm[(c - 2) / 8] & (1u << ((c - 2) % 8)))) {
			free(bm);
			return refuse(p, "damaged", "The allocation bitmap of DISTROBUNY does not mark its own clusters in use.");
		}
	p->used_clusters = popcount_bits(bm, N);
	free(bm);

	p->new_clusters = (uint32_t)m;
	p->new_vol_sectors = new_vol;
	p->new_part_last = p->part_first + new_vol - 1;
	p->new_fat_length = p->fat_length;
	uint64_t fl = ((m + 2) * 4 + SECTOR - 1) / SECTOR;
	if (fl > p->new_fat_length)
		p->new_fat_length = (uint32_t)fl;
	p->bitmap_first = bm_first;
	p->bitmap_clusters = k0;
	p->new_bitmap_clusters = k1;
	p->bitmap_entry = entry_off;
	p->new_data_bytes = new_vol * SECTOR;
	p->gpt_current = !p->primary_bad && le64(p->gpt_header + 32) == D - 1 && le64(p->gpt_header + 48) == last_usable &&
			 p->part_last == p->new_part_last;

	if (interrupted) {
		p->state = EXPAND_RESUME;
		p->code = "resume";
	} else if ((m - N) * csize >= MIN_GAIN || (table_grown && m > N)) {
		p->state = EXPAND_POSSIBLE;
		p->code = p->fs_limited ? "possible-limited" : "possible";
	} else if (unused) {
		/* the file system already has the largest size it can reach (2 TiB, or a
		 * plain volume's FAT or bitmap is full): nothing the user can do */
		p->code = "fs-limit";
	}
	return 0;
}

/* ------------------------------------------------------------------ run */

struct writer {
	int fd;
	unsigned writes, stop;
	char *err;
	size_t errlen;
};

/* Write `len` bytes at `off` unless they are already there; every write is
 * flushed to the device before the next one starts (ordering). */
static int put(struct writer *w, uint64_t off, const void *buf, size_t len)
{
	uint8_t *cur = xmalloc(len);
	bool same = pread_full(w->fd, cur, len, off) == 0 && memcmp(cur, buf, len) == 0;
	free(cur);
	if (same)
		return 0;
	if (w->stop && w->writes >= w->stop) {
		snprintf(w->err, w->errlen, "stopped after %u writes (simulated power cut)", w->writes);
		errno = EINTR;
		return -1;
	}
	if (pwrite_full(w->fd, buf, len, off) < 0 || fsync(w->fd) < 0) {
		int e = errno;
		snprintf(w->err, w->errlen, "writing %zu bytes at offset %llu failed: %s", len, (unsigned long long)off, strerror(e));
		errno = e;
		return -1;
	}
	w->writes++;
	return 0;
}

static int run_gpt(struct writer *w, struct expand_plan *p)
{
	uint64_t D = p->disk_sectors;
	uint32_t es = entry_sectors(p->gpt_entries);
	size_t tlen = (size_t)p->gpt_entries * GPT_ENTRY;
	uint8_t *t = xcalloc(es, SECTOR);
	memcpy(t, p->gpt_table, tlen);
	put64(t + (size_t)p->part_index * GPT_ENTRY + 40, p->new_part_last);
	uint32_t tcrc = crc32_ieee(t, tlen);
	uint8_t prim[512], back[512];
	make_header(prim, p->gpt_header, 1, D - 1, 2, D - 2 - es, tcrc);
	make_header(back, p->gpt_header, D - 1, 1, D - 1 - es, D - 2 - es, tcrc);
	int rc = -1;
	if (put(w, (D - 1 - es) * SECTOR, t, (size_t)es * SECTOR) < 0 || put(w, (D - 1) * SECTOR, back, SECTOR) < 0 ||
	    put(w, 2 * SECTOR, t, (size_t)es * SECTOR) < 0 || put(w, SECTOR, prim, SECTOR) < 0)
		goto out;
	memcpy(p->gpt_table, t, tlen);
	memcpy(p->gpt_header, prim, SECTOR);
	p->part_last = p->new_part_last;
	p->gpt_current = true;
	p->primary_bad = false;
	rc = 0;
out:
	free(t);
	return rc;
}

/* The protective MBR covers the whole disk (up to its 32-bit limit). */
static int run_pmbr(struct writer *w, const struct expand_plan *p)
{
	uint8_t mbr[512];
	if (pread_full(w->fd, mbr, sizeof(mbr), 0) < 0)
		return -1;
	if (mbr[510] != 0x55 || mbr[511] != 0xAA || mbr[446 + 4] != 0xEE)
		return 0;
	uint64_t size = p->disk_sectors - 1;
	put32(mbr + 446 + 12, size > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)size);
	return put(w, 0, mbr, sizeof(mbr));
}

/* Rewrite [from, to) of a region at `start`, chunk by chunk, with fill(). */
static int rewrite(struct writer *w, uint64_t start, uint64_t from, uint64_t to,
		   void (*fill)(const struct expand_plan *, uint64_t pos, uint8_t *buf, size_t len), const struct expand_plan *p)
{
	uint8_t *buf = xmalloc(CHUNK);
	int rc = 0;
	for (uint64_t pos = from / SECTOR * SECTOR; pos < to && rc == 0; pos += CHUNK) {
		size_t len = (size_t)((to + SECTOR - 1) / SECTOR * SECTOR - pos);
		if (len > CHUNK)
			len = CHUNK;
		if (pread_full(w->fd, buf, len, start + pos) < 0) {
			snprintf(w->err, w->errlen, "reading at offset %llu failed: %s", (unsigned long long)(start + pos), strerror(errno));
			rc = -1;
			break;
		}
		fill(p, pos, buf, len);
		rc = put(w, start + pos, buf, len);
	}
	free(buf);
	return rc;
}

/* FAT entries of the new clusters: free, except the bitmap's new clusters,
 * which continue its chain. */
static void fill_fat(const struct expand_plan *p, uint64_t pos, uint8_t *buf, size_t len)
{
	uint64_t N = p->clusters, M = p->new_clusters, ext = p->new_bitmap_clusters - p->bitmap_clusters;
	uint64_t lo = pos / 4 > N + 2 ? pos / 4 : N + 2, hi = (pos + len) / 4 < M + 2 ? (pos + len) / 4 : M + 2;
	for (uint64_t c = lo; c < hi; c++) {
		uint32_t want = 0;
		if (c < N + 2 + ext)
			want = c == N + 1 + ext ? EXFAT_EOC : (uint32_t)(c + 1);
		put32(buf + (c * 4 - pos), want);
	}
}

/* Bitmap bits of the new clusters (and the unused tail of its last cluster). */
static void fill_bitmap(const struct expand_plan *p, uint64_t pos, uint8_t *buf, size_t len)
{
	uint64_t N = p->clusters, ext = p->new_bitmap_clusters - p->bitmap_clusters;
	for (size_t i = 0; i < len; i++) {
		uint64_t bit0 = (pos + i) * 8;
		if (bit0 + 8 <= N)
			continue;
		uint8_t b = buf[i];
		for (unsigned k = 0; k < 8; k++) {
			uint64_t bit = bit0 + k;
			if (bit < N)
				continue;
			if (bit < N + ext)
				b |= (uint8_t)(1u << k);
			else
				b &= (uint8_t)~(1u << k);
		}
		buf[i] = b;
	}
}

static int put_u32(struct writer *w, uint64_t off, uint32_t v)
{
	uint8_t s[512];
	uint64_t sec = off / SECTOR * SECTOR;
	if (pread_full(w->fd, s, sizeof(s), sec) < 0)
		return -1;
	put32(s + (off - sec), v);
	return put(w, sec, s, sizeof(s));
}

static int put_u64(struct writer *w, uint64_t off, uint64_t v)
{
	uint8_t s[512];
	uint64_t sec = off / SECTOR * SECTOR;
	if (pread_full(w->fd, s, sizeof(s), sec) < 0)
		return -1;
	put64(s + (off - sec), v);
	return put(w, sec, s, sizeof(s));
}

static int run_exfat(struct writer *w, struct expand_plan *p)
{
	uint64_t base = p->part_first * SECTOR;
	uint64_t csize = (uint64_t)SECTOR << p->spc_shift;
	uint64_t N = p->clusters, M = p->new_clusters;
	uint32_t ext = p->new_bitmap_clusters - p->bitmap_clusters;

	/* VolumeDirty for the duration (not part of the boot checksum) */
	if (!p->boot_from_backup && !(le16(p->boot + 106) & VOLUME_DIRTY)) {
		uint8_t s0[512];
		memcpy(s0, p->boot, sizeof(s0));
		put16(s0 + 106, le16(s0 + 106) | VOLUME_DIRTY);
		if (put(w, base, s0, sizeof(s0)) < 0)
			return -1;
	}
	uint64_t fat = base + (uint64_t)p->fat_offset * SECTOR;
	uint64_t bitmap = base + (uint64_t)p->heap_offset * SECTOR + (uint64_t)(p->bitmap_first - 2) * csize;
	if (rewrite(w, fat, (N + 2) * 4, (M + 2) * 4, fill_fat, p) < 0 ||
	    rewrite(w, bitmap, N / 8, (uint64_t)p->new_bitmap_clusters * csize, fill_bitmap, p) < 0)
		return -1;
	if (ext && put_u32(w, fat + (uint64_t)(p->bitmap_first + p->bitmap_clusters - 1) * 4, (uint32_t)(N + 2)) < 0)
		return -1;
	if (put_u64(w, base + p->bitmap_entry + 24, (M + 7) / 8) < 0)
		return -1;

	/* the boot regions: backup first, then main (clean) in one write */
	uint8_t r[12 * SECTOR];
	memcpy(r, p->boot, sizeof(r));
	put64(r + 72, p->new_vol_sectors);
	put32(r + 84, p->new_fat_length);
	put32(r + 92, (uint32_t)M);
	put16(r + 106, le16(r + 106) & ~VOLUME_DIRTY);
	uint64_t used = (uint64_t)p->used_clusters + ext;
	r[112] = (uint8_t)(used * 100 / M);
	uint32_t cs = boot_checksum(r);
	for (size_t i = 0; i < SECTOR; i += 4)
		put32(r + 11 * SECTOR + i, cs);
	if (put(w, base + 12 * SECTOR, r, sizeof(r)) < 0 || put(w, base, r, sizeof(r)) < 0)
		return -1;
	memcpy(p->boot, r, sizeof(r));
	p->boot_from_backup = false;
	return 0;
}

int expand_run(int fd, struct expand_plan *p, char *err, size_t errlen)
{
	snprintf(err, errlen, "%s", "");
	if (p->state != EXPAND_POSSIBLE && p->state != EXPAND_RESUME) {
		snprintf(err, errlen, "%s", p->state == EXPAND_REFUSED ? p->reason : "DISTROBUNY already uses the whole drive");
		errno = EINVAL;
		return -1;
	}
	struct writer w = { fd, 0, p->stop_after_writes, err, errlen };
	int rc = 0;
	if ((!p->gpt_current && run_gpt(&w, p) < 0) || run_pmbr(&w, p) < 0 || run_exfat(&w, p) < 0) {
		if (!err[0])
			snprintf(err, errlen, "%s", strerror(errno));
		rc = -1;
	}
	p->writes = w.writes;
	if (rc < 0)
		return -1;
	p->clusters = p->new_clusters;
	p->vol_sectors = p->new_vol_sectors;
	p->data_bytes = p->new_data_bytes;
	p->state = EXPAND_NONE;
	p->code = "done";
	return 0;
}
