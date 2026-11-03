/*
 * Distro Buny — hand-off to the selected distribution (kexec)
 *
 * On Secure Boot systems the running (signed) kernel enforces signature
 * verification of the kernel being loaded; Distro Buny never bypasses it and
 * reports a rejected signature to the user.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "boot/handoff.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/reboot.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "boot/cpio.h"
#include "common/log.h"
#include "common/strbuf.h"

#define CMDLINE_MAX 2047 /* x86 COMMAND_LINE_SIZE - 1 */
#define KEXEC_FILE_NO_INITRAMFS 0x4

static int memfd(const char *name)
{
	return (int)syscall(SYS_memfd_create, name, 1u /* MFD_CLOEXEC */);
}

static int write_all(int fd, const void *buf, size_t len)
{
	const char *p = buf;
	while (len) {
		ssize_t r = write(fd, p, len);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += r;
		len -= (size_t)r;
	}
	return 0;
}

static int pad_to4(int fd)
{
	off_t pos = lseek(fd, 0, SEEK_END);
	static const char zero[4];
	size_t n = (size_t)((4 - (pos & 3)) & 3);
	return n ? write_all(fd, zero, n) : 0;
}

/* ---------------------------------------------------------- prepare */

struct prog {
	handoff_progress_fn cb;
	void *ctx;
	enum handoff_stage stage;
	uint64_t base, total;
};

static void prog_cb(void *ctx, uint64_t done, uint64_t total)
{
	struct prog *p = ctx;
	if (p->cb)
		p->cb(p->ctx, p->stage, p->base + done, p->total ? p->total : total);
}

static uint64_t iso_size_of(struct iso_image *iso, const char *path)
{
	struct iso_file f;
	if (iso_lookup(iso, path, &f) < 0)
		return 0;
	uint64_t s = f.size;
	iso_file_free(&f);
	return s;
}

static int copy_iso_file(struct iso_image *iso, const char *path, int fd, struct prog *pg, char *err, size_t errlen)
{
	struct iso_file f;
	if (iso_lookup(iso, path, &f) < 0) {
		snprintf(err, errlen, "%s is missing from the image", path);
		return -1;
	}
	int rc = iso_copy_to_fd(iso, &f, fd, prog_cb, pg);
	pg->base += f.size;
	iso_file_free(&f);
	if (rc < 0)
		snprintf(err, errlen, "reading %s from the image failed: %s", path, strerror(errno));
	return rc;
}

void handoff_payload_close(struct handoff_payload *p)
{
	if (p->kernel_fd >= 0)
		close(p->kernel_fd);
	if (p->initrd_fd >= 0)
		close(p->initrd_fd);
	free(p->cmdline);
	p->kernel_fd = p->initrd_fd = -1;
	p->cmdline = NULL;
}

/* debian-installer: its initrd has no loop driver, and cdrom-detect only mounts
 * what udev knows as a CD drive or disk.  Add the image's own loop-modules
 * package for the pre-init to unpack, and a udev rule that tags the virtual
 * medium as a CD drive. */
static int append_udeb_overlay(struct iso_image *iso, const char *udeb, int fd, char *err, size_t errlen)
{
	struct iso_file f;
	if (iso_lookup(iso, udeb, &f) < 0 || f.is_dir || f.size > (32u << 20)) {
		snprintf(err, errlen, "the installer's driver package %s cannot be read", udeb);
		return -1;
	}
	size_t size = (size_t)f.size;
	char *buf = xmalloc(size ? size : 1);
	ssize_t n = iso_pread(iso, &f, buf, size, 0);
	iso_file_free(&f);
	if (n != (ssize_t)size) {
		snprintf(err, errlen, "reading %s from the image failed", udeb);
		free(buf);
		return -1;
	}
	char rule[160];
	snprintf(rule, sizeof(rule), "# Distro Buny: the virtual medium is this installer's CD\n"
		 "KERNEL==\"loop%d\", ENV{ID_CDROM}=\"1\", ENV{ID_TYPE}=\"cd\"\n", BUNY_VM_LOOP_INDEX);
	struct cpio_writer w;
	cpio_init(&w, fd);
	cpio_add_dir(&w, "distrobuny", 0755);
	cpio_add_dir(&w, "distrobuny/udebs", 0755);
	cpio_add_file(&w, "distrobuny/udebs/loop-modules.udeb", 0644, buf, (size_t)n);
	cpio_add_dir(&w, "etc", 0755);
	cpio_add_dir(&w, "etc/udev", 0755);
	cpio_add_dir(&w, "etc/udev/rules.d", 0755);
	cpio_add_file(&w, "etc/udev/rules.d/60-distrobuny-medium.rules", 0644, rule, strlen(rule));
	int rc = cpio_finish(&w);
	free(buf);
	if (rc < 0 || w.error || pad_to4(fd) < 0) {
		snprintf(err, errlen, "writing the installer's driver package failed: %s", strerror(w.error ? w.error : errno));
		return -1;
	}
	return 0;
}

int handoff_prepare(struct iso_image *iso, const struct boot_plan *plan, struct handoff_payload *out,
		    handoff_progress_fn cb, void *cb_ctx, char *err, size_t errlen)
{
	memset(out, 0, sizeof(*out));
	out->kernel_fd = out->initrd_fd = -1;
	const struct boot_entry *e = plan->entry;
	if (!e || !e->kernel) {
		snprintf(err, errlen, "the boot plan has no kernel");
		return -1;
	}
	out->kernel_fd = memfd("buny-kernel");
	out->initrd_fd = memfd("buny-initrd");
	if (out->kernel_fd < 0 || out->initrd_fd < 0) {
		snprintf(err, errlen, "memfd_create failed: %s", strerror(errno));
		goto fail;
	}
	struct prog pg = { cb, cb_ctx, HS_KERNEL, 0, iso_size_of(iso, e->kernel) };
	if (copy_iso_file(iso, e->kernel, out->kernel_fd, &pg, err, errlen) < 0)
		goto fail;
	uint64_t itotal = 0;
	for (size_t i = 0; i < e->initrds.n; i++)
		itotal += iso_size_of(iso, e->initrds.v[i]);
	pg = (struct prog){ cb, cb_ctx, HS_INITRD, 0, itotal };
	for (size_t i = 0; i < e->initrds.n; i++) {
		if (copy_iso_file(iso, e->initrds.v[i], out->initrd_fd, &pg, err, errlen) < 0)
			goto fail;
		/* each archive in a concatenated initramfs starts on a 4-byte boundary */
		if (pad_to4(out->initrd_fd) < 0) {
			snprintf(err, errlen, "writing the initrd failed: %s", strerror(errno));
			goto fail;
		}
	}
	if (plan->udeb && append_udeb_overlay(iso, plan->udeb, out->initrd_fd, err, errlen) < 0)
		goto fail;
	out->cmdline = entry_cmdline(e);
	out->needs_preinit = plan->needs_preinit;
	return 0;
fail:
	handoff_payload_close(out);
	return -1;
}

/* ------------------------------------------------------------ medium */

char *handoff_medium_conf(const struct handoff_medium *m, const char *next_init)
{
	struct strbuf sb = STRBUF_INIT;
	sb_add(&sb, "# Distro Buny virtual medium (generated)\nversion=1\n");
	sb_addf(&sb, "label=%s\n", m->iso_label ? m->iso_label : "");
	sb_addf(&sb, "next_init=%s\n", next_init ? next_init : "/init");
	sb_addf(&sb, "fstype=%s\n", m->fstype ? m->fstype : "");
	sb_addf(&sb, "part_start=%llu\n", (unsigned long long)m->part_start);
	sb_addf(&sb, "part_size=%llu\n", (unsigned long long)m->part_size);
	if (m->have_serial)
		sb_addf(&sb, "serial=%08X\n", m->vol_serial);
	sb_addf(&sb, "iso_size=%llu\n", (unsigned long long)m->iso_size);
	sb_addf(&sb, "loop_index=%d\n", BUNY_VM_LOOP_INDEX);
	sb_addf(&sb, "timeout=300\nmode=%s\nverbose=%d\n", m->next == 1 && !m->force_dm ? "loop" : "dm", m->verbose ? 1 : 0);
	for (size_t i = 0; i < m->next; i++)
		sb_addf(&sb, "extent=%llu %llu\n", (unsigned long long)m->ext[i].offset, (unsigned long long)m->ext[i].length);
	return sb_detach(&sb);
}

/* Read `len` bytes at logical offset `off` of the ISO through the extent map. */
static int read_mapped(int fd, const struct handoff_medium *m, uint64_t off, void *buf, size_t len)
{
	uint64_t pos = 0;
	uint8_t *b = buf;
	for (size_t i = 0; i < m->next && len; i++) {
		uint64_t end = pos + m->ext[i].length;
		if (off < end) {
			uint64_t within = off - pos;
			size_t take = (size_t)BUNY_MIN((uint64_t)len, m->ext[i].length - within);
			if (pread(fd, b, take, (off_t)(m->part_base + m->ext[i].offset + within)) != (ssize_t)take)
				return -1;
			b += take;
			off += take;
			len -= take;
		}
		pos = end;
	}
	return len ? -1 : 0;
}

/* Prove the extent map is right before trusting it: the ISO's volume
 * descriptor must be where the map says, carrying the expected label, and
 * the first and last 64 KiB read through the map must equal the same bytes
 * read through the filesystem. */
int handoff_verify_medium(const struct handoff_medium *m, char *err, size_t errlen)
{
	uint64_t total = 0;
	for (size_t i = 0; i < m->next; i++) {
		if (m->ext[i].length % 512 || m->ext[i].offset % 512) {
			snprintf(err, errlen, "the ISO is not sector-aligned on the data partition");
			return -1;
		}
		total += m->ext[i].length;
	}
	if (!m->next || total != m->iso_size) {
		snprintf(err, errlen, "the extent map covers %llu of %llu bytes", (unsigned long long)total,
			 (unsigned long long)m->iso_size);
		return -1;
	}
	int fd = open(m->part_dev, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		snprintf(err, errlen, "cannot open %s: %s", m->part_dev, strerror(errno));
		return -1;
	}
	int ffd = m->iso_file ? open(m->iso_file, O_RDONLY | O_CLOEXEC) : -1;
	enum { CHUNK = 65536 };
	uint8_t *a = xmalloc(CHUNK), *b = xmalloc(CHUNK);
	int rc = 0;
	if (read_mapped(fd, m, 32768, a, 2048) < 0 || memcmp(a + 1, "CD001", 5) != 0) {
		snprintf(err, errlen, "the data partition does not contain the ISO at the mapped location");
		rc = -1;
		goto out;
	}
	char label[33];
	memcpy(label, a + 40, 32);
	label[32] = '\0';
	for (int i = 31; i >= 0 && (label[i] == ' ' || !label[i]); i--)
		label[i] = '\0';
	if (m->iso_label && strcmp(label, m->iso_label) != 0) {
		snprintf(err, errlen, "mapped ISO label \"%s\" differs from \"%s\"", label, m->iso_label);
		rc = -1;
		goto out;
	}
	if (ffd >= 0 && m->iso_size >= 2 * CHUNK) {
		uint64_t offs[2] = { 0, m->iso_size - CHUNK };
		for (int k = 0; k < 2; k++) {
			if (read_mapped(fd, m, offs[k], a, CHUNK) < 0 || pread(ffd, b, CHUNK, (off_t)offs[k]) != CHUNK ||
			    memcmp(a, b, CHUNK) != 0) {
				snprintf(err, errlen, "the on-disk location of the ISO does not match the file (%s)",
					 k ? "end" : "start");
				rc = -1;
				goto out;
			}
		}
	}
out:
	free(a);
	free(b);
	if (ffd >= 0)
		close(ffd);
	close(fd);
	return rc;
}

int handoff_attach_medium(struct handoff_payload *p, const struct handoff_medium *m, const char *preinit_path,
			  char **final_cmdline, char *err, size_t errlen)
{
	*final_cmdline = NULL;
	if (handoff_verify_medium(m, err, errlen) < 0)
		return -1;
	size_t plen;
	char *preinit = read_file(preinit_path, 16u << 20, &plen);
	if (!preinit) {
		snprintf(err, errlen, "the pre-init program %s is missing", preinit_path);
		return -1;
	}
	struct ptrvec args = { 0 };
	cmdline_split(p->cmdline, &args);
	struct boot_entry tmp = { 0 };
	tmp.args = args;
	const char *orig = entry_get_arg(&tmp, "rdinit");
	char *next_init = xstrdup(orig && *orig ? orig : "/init");
	entry_set_arg(&tmp, "rdinit", BUNY_PREINIT_PATH);
	char *cl = entry_cmdline(&tmp);
	ptrvec_free(&tmp.args, free);

	char *conf = handoff_medium_conf(m, next_init);
	free(next_init);
	if (pad_to4(p->initrd_fd) < 0) {
		snprintf(err, errlen, "writing the initrd failed: %s", strerror(errno));
		free(preinit);
		free(conf);
		free(cl);
		return -1;
	}
	struct cpio_writer w;
	cpio_init(&w, p->initrd_fd);
	cpio_add_dir(&w, "distrobuny", 0755);
	cpio_add_dir(&w, "distrobuny/dev", 0755);
	cpio_add_dir(&w, "distrobuny/sys", 0755);
	cpio_add_dir(&w, "distrobuny/modules", 0755);
	cpio_add_file(&w, "distrobuny/preinit", 0755, preinit, plen);
	cpio_add_file(&w, "distrobuny/medium.conf", 0644, conf, strlen(conf));
	int rc = cpio_finish(&w);
	free(preinit);
	free(conf);
	if (rc < 0 || w.error) {
		snprintf(err, errlen, "writing the pre-init overlay failed: %s", strerror(w.error));
		free(cl);
		return -1;
	}
	*final_cmdline = cl;
	return 0;
}

/* -------------------------------------------------------------- load */

static const char *kexec_error(int e)
{
	switch (e) {
	case ENODATA:
		return "Secure Boot is on and this system's kernel is not signed, so the computer will not start it";
	case EKEYREJECTED:
	case EBADMSG:
	case ENOKEY:
	case EKEYREVOKED:
	case EKEYEXPIRED:
		return "Secure Boot: this kernel's signature is not trusted by this computer";
	case EPERM:
		return "the running system is locked down and refused to load this kernel (Secure Boot)";
	case ENOEXEC:
		return "the kernel image format cannot be loaded with kexec";
	case ENOMEM:
		return "not enough memory to load the kernel and initrd";
	case EADDRNOTAVAIL:
		return "no free memory region is large enough for this system's kernel and initrd "
		       "(more RAM is needed, or the drive's boot options must include kho=off)";
	case EBUSY:
		return "another kernel load is in progress";
	case EINVAL:
		return "the kernel rejected the image or its parameters";
	case ENOSYS:
		return "the runtime kernel does not support kexec_file_load";
	default:
		return NULL;
	}
}

int handoff_load(struct handoff_payload *p, const char *cmdline, bool dry_run, char *err, size_t errlen)
{
	if (strlen(cmdline) > CMDLINE_MAX) {
		snprintf(err, errlen, "the kernel command line is too long (%zu bytes)", strlen(cmdline));
		return -1;
	}
	struct stat st;
	if (fstat(p->kernel_fd, &st) < 0 || st.st_size < 1024) {
		snprintf(err, errlen, "the kernel image is empty");
		return -1;
	}
	LOGI("handoff: loading kernel (%lld bytes), cmdline: %s", (long long)st.st_size, cmdline);
	if (dry_run)
		return 0;
	unsigned long flags = 0;
	int ifd = p->initrd_fd;
	if (fstat(ifd, &st) == 0 && st.st_size == 0) {
		flags |= KEXEC_FILE_NO_INITRAMFS;
		ifd = -1;
	}
	lseek(p->kernel_fd, 0, SEEK_SET);
	if (ifd >= 0)
		lseek(ifd, 0, SEEK_SET);
	long r = syscall(SYS_kexec_file_load, p->kernel_fd, ifd, strlen(cmdline) + 1, cmdline, flags);
	if (r < 0) {
		int ke = errno;
		const char *why = kexec_error(ke);
		snprintf(err, errlen, "%s (%s)", why ? why : "loading the kernel failed", strerror(ke));
		errno = ke;
		return -1;
	}
	return 0;
}

int handoff_execute(char *err, size_t errlen)
{
	sync();
	if (reboot(LINUX_REBOOT_CMD_KEXEC) < 0) {
		snprintf(err, errlen, "starting the loaded kernel failed: %s", strerror(errno));
		return -1;
	}
	return 0; /* not reached */
}

static int dump_fd(int fd, const char *dir, const char *name)
{
	char *path = path_join(dir, name);
	int out = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	free(path);
	if (out < 0)
		return -1;
	off_t pos = 0;
	char buf[1 << 16];
	ssize_t r;
	int rc = 0;
	while ((r = pread(fd, buf, sizeof(buf), pos)) > 0) {
		if (write_all(out, buf, (size_t)r) < 0) {
			rc = -1;
			break;
		}
		pos += r;
	}
	close(out);
	return rc;
}

int handoff_dump(const struct handoff_payload *p, const char *cmdline, const char *dir)
{
	if (dump_fd(p->kernel_fd, dir, "kernel") < 0 || dump_fd(p->initrd_fd, dir, "initrd") < 0)
		return -1;
	char *path = path_join(dir, "cmdline");
	int rc = write_file_atomic(path, cmdline, strlen(cmdline));
	free(path);
	return rc;
}
