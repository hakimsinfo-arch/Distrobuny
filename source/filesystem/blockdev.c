/*
 * Distro Buny — block device discovery and filesystem probing
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "filesystem/blockdev.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "common/util.h"

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }

static int pread_full(int fd, void *buf, size_t len, uint64_t off)
{
	uint8_t *b = buf;
	while (len) {
		ssize_t r = pread(fd, b, len, (off_t)off);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (r == 0)
			return -1;
		b += r;
		off += (uint64_t)r;
		len -= (size_t)r;
	}
	return 0;
}

void guid_to_str(const uint8_t g[16], char out[37])
{
	snprintf(out, 37, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", le32(g), le16(g + 4), le16(g + 6), g[8], g[9],
		 g[10], g[11], g[12], g[13], g[14], g[15]);
}

static void utf16le_to_utf8(const uint8_t *src, size_t nchars, char *out, size_t outlen)
{
	size_t o = 0;
	for (size_t i = 0; i < nchars; i++) {
		uint32_t c = le16(src + 2 * i);
		if (c == 0)
			break;
		if (c < 0x80) {
			if (o + 1 >= outlen)
				break;
			out[o++] = (char)c;
		} else if (c < 0x800) {
			if (o + 2 >= outlen)
				break;
			out[o++] = (char)(0xC0 | (c >> 6));
			out[o++] = (char)(0x80 | (c & 0x3F));
		} else {
			if (o + 3 >= outlen)
				break;
			out[o++] = (char)(0xE0 | (c >> 12));
			out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
			out[o++] = (char)(0x80 | (c & 0x3F));
		}
	}
	out[o] = '\0';
}

static void copy_label(char *dst, size_t dstlen, const uint8_t *src, size_t n)
{
	size_t l = n < dstlen - 1 ? n : dstlen - 1;
	memcpy(dst, src, l);
	dst[l] = '\0';
	str_rstrip_chars(dst, " \0");
	for (char *c = dst; *c; c++)
		if (!isprint((unsigned char)*c))
			*c = '_';
}

static int probe_exfat(int fd, const uint8_t *bs, struct blk_dev *d)
{
	if (memcmp(bs + 3, "EXFAT   ", 8) != 0)
		return -1;
	snprintf(d->fstype, sizeof(d->fstype), "exfat");
	d->vol_serial = le32(bs + 100);
	snprintf(d->uuid, sizeof(d->uuid), "%04X-%04X", d->vol_serial >> 16, d->vol_serial & 0xFFFF);
	/* Volume label lives in the root directory (entry type 0x83). */
	unsigned bps_shift = bs[108], spc_shift = bs[109];
	if (bps_shift < 9 || bps_shift > 12 || spc_shift > 25 - bps_shift)
		return 0;
	uint64_t heap = (uint64_t)le32(bs + 88) << bps_shift;
	uint32_t root = le32(bs + 96);
	uint64_t csize = 1ull << (bps_shift + spc_shift);
	if (root < 2)
		return 0;
	uint8_t ent[32 * 64];
	size_t take = csize < sizeof(ent) ? (size_t)csize : sizeof(ent);
	if (pread_full(fd, ent, take, heap + (uint64_t)(root - 2) * csize) < 0)
		return 0;
	for (size_t i = 0; i + 32 <= take; i += 32) {
		uint8_t type = ent[i];
		if (type == 0x00)
			break;
		if (type == 0x83) {
			uint8_t n = ent[i + 1];
			if (n > 11)
				n = 11;
			utf16le_to_utf8(ent + i + 2, n, d->label, sizeof(d->label));
			break;
		}
	}
	return 0;
}

static int probe_fat(const uint8_t *bs, struct blk_dev *d)
{
	if (bs[510] != 0x55 || bs[511] != 0xAA)
		return -1;
	const uint8_t *serial, *label;
	if (!memcmp(bs + 82, "FAT32   ", 8)) {
		serial = bs + 67;
		label = bs + 71;
	} else if (!memcmp(bs + 54, "FAT16   ", 8) || !memcmp(bs + 54, "FAT12   ", 8) || !memcmp(bs + 54, "FAT     ", 8)) {
		serial = bs + 39;
		label = bs + 43;
	} else {
		return -1;
	}
	snprintf(d->fstype, sizeof(d->fstype), "vfat");
	d->vol_serial = le32(serial);
	snprintf(d->uuid, sizeof(d->uuid), "%04X-%04X", d->vol_serial >> 16, d->vol_serial & 0xFFFF);
	copy_label(d->label, sizeof(d->label), label, 11);
	if (!strcmp(d->label, "NO NAME"))
		d->label[0] = '\0';
	return 0;
}

static int probe_ntfs(const uint8_t *bs, struct blk_dev *d)
{
	if (memcmp(bs + 3, "NTFS    ", 8) != 0)
		return -1;
	snprintf(d->fstype, sizeof(d->fstype), "ntfs");
	snprintf(d->uuid, sizeof(d->uuid), "%016llX", (unsigned long long)le64(bs + 72));
	return 0;
}

static int probe_ext(int fd, struct blk_dev *d)
{
	uint8_t sb[1024];
	if (pread_full(fd, sb, sizeof(sb), 1024) < 0 || le16(sb + 56) != 0xEF53)
		return -1;
	uint32_t incompat = le32(sb + 96);
	snprintf(d->fstype, sizeof(d->fstype), (incompat & 0x40) ? "ext4" : "ext2");
	/* ext UUIDs are big-endian byte strings, not mixed-endian GUIDs */
	snprintf(d->uuid, sizeof(d->uuid), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", sb[104],
		 sb[105], sb[106], sb[107], sb[108], sb[109], sb[110], sb[111], sb[112], sb[113], sb[114], sb[115], sb[116],
		 sb[117], sb[118], sb[119]);
	copy_label(d->label, sizeof(d->label), sb + 120, 16);
	return 0;
}

static int probe_iso(int fd, struct blk_dev *d)
{
	uint8_t vd[2048];
	if (pread_full(fd, vd, sizeof(vd), 32768) < 0 || memcmp(vd + 1, "CD001", 5) != 0 || vd[0] != 1)
		return -1;
	snprintf(d->fstype, sizeof(d->fstype), "iso9660");
	copy_label(d->label, sizeof(d->label), vd + 40, 32);
	return 0;
}

int blk_probe_fs(int fd, struct blk_dev *d)
{
	uint8_t bs[512];
	d->fstype[0] = d->label[0] = d->uuid[0] = '\0';
	if (pread_full(fd, bs, sizeof(bs), 0) < 0)
		return -1;
	if (probe_exfat(fd, bs, d) == 0 || probe_ntfs(bs, d) == 0 || probe_fat(bs, d) == 0)
		return 0;
	if (probe_ext(fd, d) == 0 || probe_iso(fd, d) == 0)
		return 0;
	return -1;
}

static int read_sys_u64(const char *dir, const char *attr, uint64_t *out)
{
	char *p = path_join(dir, attr);
	char *s = read_file(p, 64, NULL);
	free(p);
	if (!s)
		return -1;
	*out = strtoull(s, NULL, 10);
	free(s);
	return 0;
}

static void read_sys_str(const char *dir, const char *attr, char *out, size_t outlen)
{
	char *p = path_join(dir, attr);
	char *s = read_file(p, 256, NULL);
	free(p);
	out[0] = '\0';
	if (s) {
		snprintf(out, outlen, "%s", str_trim(s));
		free(s);
	}
}

int gpt_read(int fd, struct gpt_entry **out, size_t *nout)
{
	*out = NULL;
	*nout = 0;
	uint8_t hdr[512];
	unsigned lbs = 512;
	if (pread_full(fd, hdr, sizeof(hdr), 512) < 0 || memcmp(hdr, "EFI PART", 8) != 0) {
		/* 4Kn disks keep the header at byte 4096 */
		if (pread_full(fd, hdr, sizeof(hdr), 4096) < 0 || memcmp(hdr, "EFI PART", 8) != 0)
			return -1;
		lbs = 4096;
	}
	uint64_t ent_lba = le64(hdr + 72);
	uint32_t nent = le32(hdr + 80), esz = le32(hdr + 84);
	if (esz < 128 || esz > 1024 || nent > 1024)
		return -1;
	uint8_t *ents = xmalloc((size_t)nent * esz);
	if (pread_full(fd, ents, (size_t)nent * esz, ent_lba * lbs) < 0) {
		free(ents);
		return -1;
	}
	struct gpt_entry *v = NULL;
	size_t n = 0;
	static const uint8_t zero[16];
	for (uint32_t i = 0; i < nent; i++) {
		const uint8_t *e = ents + (size_t)i * esz;
		if (!memcmp(e, zero, 16))
			continue;
		v = xrealloc(v, (n + 1) * sizeof(*v));
		guid_to_str(e, v[n].type);
		guid_to_str(e + 16, v[n].guid);
		utf16le_to_utf8(e + 56, 36, v[n].name, sizeof(v[n].name));
		v[n].first_lba = le64(e + 32) * (lbs / 512);
		v[n].last_lba = (le64(e + 40) + 1) * (lbs / 512) - 1;
		n++;
	}
	free(ents);
	*out = v;
	*nout = n;
	return 0;
}

/* Fill GPT metadata for every partition of one disk. */
static void gpt_annotate(const char *devdir, struct blk_dev *v, size_t n, const char *disk)
{
	char *path = xasprintf("%s/%s", devdir, disk);
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	free(path);
	if (fd < 0)
		return;
	struct gpt_entry *ents;
	size_t ne;
	if (gpt_read(fd, &ents, &ne) == 0) {
		for (size_t i = 0; i < ne; i++)
			for (size_t k = 0; k < n; k++) {
				if (strcmp(v[k].disk, disk) || v[k].partno == 0 || v[k].start != ents[i].first_lba)
					continue;
				snprintf(v[k].parttype, sizeof(v[k].parttype), "%s", ents[i].type);
				snprintf(v[k].partuuid, sizeof(v[k].partuuid), "%s", ents[i].guid);
				snprintf(v[k].partlabel, sizeof(v[k].partlabel), "%s", ents[i].name);
			}
		free(ents);
	}
	close(fd);
}

int blk_enumerate(const char *sysdir, const char *devdir, struct blk_dev **out, size_t *nout)
{
	*out = NULL;
	*nout = 0;
	char *cls = xasprintf("%s/class/block", sysdir);
	DIR *d = opendir(cls);
	if (!d) {
		free(cls);
		return -1;
	}
	struct blk_dev *v = NULL;
	size_t n = 0;
	struct dirent *de;
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;
		if (strlen(de->d_name) >= sizeof(((struct blk_dev *)0)->name))
			continue; /* not a kernel block device name */
		/* skip pseudo devices that can never hold Distro Buny */
		if (str_starts(de->d_name, "ram") || str_starts(de->d_name, "zram") || str_starts(de->d_name, "loop") ||
		    str_starts(de->d_name, "dm-") || str_starts(de->d_name, "md") || str_starts(de->d_name, "sr"))
			continue;
		char *dir = xasprintf("%s/%s", cls, de->d_name);
		struct blk_dev b;
		memset(&b, 0, sizeof(b));
		snprintf(b.name, sizeof(b.name), "%.39s", de->d_name);
		snprintf(b.devpath, sizeof(b.devpath), "%.20s/%.39s", devdir, b.name);
		char devnum[32];
		read_sys_str(dir, "dev", devnum, sizeof(devnum));
		sscanf(devnum, "%u:%u", &b.major, &b.minor);
		uint64_t pn = 0;
		if (read_sys_u64(dir, "partition", &pn) == 0) {
			b.partno = (int)pn;
			read_sys_u64(dir, "start", &b.start);
			/* parent disk: resolve ../ of the sysfs device link */
			char *link = xasprintf("%s/..", dir);
			char *real = realpath(link, NULL);
			if (real)
				snprintf(b.disk, sizeof(b.disk), "%.39s", path_basename(real));
			free(real);
			free(link);
		} else {
			snprintf(b.disk, sizeof(b.disk), "%s", b.name);
		}
		read_sys_u64(dir, "size", &b.size);
		uint64_t ro = 0, rm = 0;
		read_sys_u64(dir, "ro", &ro);
		b.readonly = ro != 0;
		char *diskdir = xasprintf("%s/%s", cls, b.disk);
		read_sys_u64(diskdir, "removable", &rm);
		b.removable = rm != 0;
		char vendor[32], model[48];
		read_sys_str(diskdir, "device/vendor", vendor, sizeof(vendor));
		read_sys_str(diskdir, "device/model", model, sizeof(model));
		snprintf(b.model, sizeof(b.model), "%.15s%s%.46s", vendor, vendor[0] ? " " : "", model);
		free(diskdir);
		free(dir);
		if (b.size == 0)
			continue;
		int fd = open(b.devpath, O_RDONLY | O_CLOEXEC);
		if (fd >= 0) {
			blk_probe_fs(fd, &b);
			close(fd);
		}
		v = xrealloc(v, (n + 1) * sizeof(*v));
		v[n++] = b;
	}
	closedir(d);
	free(cls);
	/* annotate partitions with GPT names/types */
	for (size_t i = 0; i < n; i++)
		if (v[i].partno == 0)
			gpt_annotate(devdir, v, n, v[i].name);
	*out = v;
	*nout = n;
	return 0;
}

const struct blk_dev *blk_find_by_partuuid(const struct blk_dev *v, size_t n, const char *partuuid)
{
	for (size_t i = 0; i < n; i++)
		if (v[i].partuuid[0] && !strcasecmp(v[i].partuuid, partuuid))
			return &v[i];
	return NULL;
}

const struct blk_dev *blk_find_by_label(const struct blk_dev *v, size_t n, const char *fstype, const char *label)
{
	for (size_t i = 0; i < n; i++)
		if ((!fstype || !strcmp(v[i].fstype, fstype)) && !strcmp(v[i].label, label))
			return &v[i];
	return NULL;
}
