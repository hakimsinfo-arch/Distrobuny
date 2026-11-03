/*
 * Distro Buny — firmware and Secure Boot state
 *
 * Reads the standard UEFI global variables through efivarfs.  Distro Buny
 * only reports Secure Boot state; it never changes Secure Boot keys or
 * policy.  The single variable it may write is OsIndications, and only when
 * the user explicitly asks to reboot into firmware setup.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "security/secureboot.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/util.h"

#define EFI_GLOBAL "8be4df61-93ca-11d2-aa0d-00e098032b8c"
#define OS_INDICATIONS_BOOT_TO_FW_UI 0x1ull

/* efivarfs files start with a 4-byte attribute word. */
static int read_var(const char *sysdir, const char *name, uint8_t *buf, size_t len)
{
	char *p = xasprintf("%s/firmware/efi/efivars/%s-" EFI_GLOBAL, sysdir, name);
	int fd = open(p, O_RDONLY | O_CLOEXEC);
	free(p);
	if (fd < 0)
		return -1;
	uint8_t tmp[4 + 64];
	ssize_t r = read(fd, tmp, sizeof(tmp));
	close(fd);
	if (r < 5)
		return -1;
	size_t n = (size_t)r - 4 < len ? (size_t)r - 4 : len;
	memcpy(buf, tmp + 4, n);
	return (int)n;
}

void fw_state_read(const char *sysdir, struct fw_state *st)
{
	memset(st, 0, sizeof(*st));
	char *efi = path_join(sysdir, "firmware/efi");
	struct stat sb;
	st->uefi = stat(efi, &sb) == 0;
	free(efi);
	uint8_t v[8];
	if (st->uefi) {
		if (read_var(sysdir, "SecureBoot", v, 1) == 1)
			st->secure_boot = v[0] ? SB_ENABLED : SB_DISABLED;
		if (read_var(sysdir, "SetupMode", v, 1) == 1)
			st->setup_mode = v[0] != 0;
		uint64_t sup = 0;
		if (read_var(sysdir, "OsIndicationsSupported", v, 8) == 8) {
			memcpy(&sup, v, 8);
			st->fw_ui_supported = (sup & OS_INDICATIONS_BOOT_TO_FW_UI) != 0;
		}
	}
	/* firmware vendor and version from SMBIOS/DMI */
	char *vp = path_join(sysdir, "class/dmi/id/bios_vendor");
	char *vend = read_file(vp, 128, NULL);
	free(vp);
	vp = path_join(sysdir, "class/dmi/id/bios_version");
	char *ver = read_file(vp, 128, NULL);
	free(vp);
	if (vend)
		snprintf(st->fw_vendor, sizeof(st->fw_vendor), "%.40s%s%.20s", str_trim(vend), ver ? " " : "", ver ? str_trim(ver) : "");
	free(vend);
	free(ver);
	char *lp = path_join(sysdir, "kernel/security/lockdown");
	char *ld = read_file(lp, 256, NULL);
	free(lp);
	if (ld) {
		/* format: "none [integrity] confidentiality" — the active mode is bracketed */
		char *a = strchr(ld, '['), *b = a ? strchr(a, ']') : NULL;
		if (a && b)
			snprintf(st->lockdown, sizeof(st->lockdown), "%.*s", (int)(b - a - 1), a + 1);
		free(ld);
	}
}

const char *sb_state_name(enum sb_state s)
{
	return s == SB_ENABLED ? "enabled" : s == SB_DISABLED ? "disabled" : "unknown";
}

#ifndef FS_IOC_GETFLAGS
#define FS_IOC_GETFLAGS _IOR('f', 1, long)
#define FS_IOC_SETFLAGS _IOW('f', 2, long)
#endif
#define FS_IMMUTABLE_FL 0x00000010

int fw_request_setup(const char *efivars_dir)
{
	char *p = xasprintf("%s/OsIndications-" EFI_GLOBAL, efivars_dir);
	uint8_t buf[12];
	uint32_t attrs = 0x7; /* NON_VOLATILE | BOOTSERVICE_ACCESS | RUNTIME_ACCESS */
	uint64_t val = OS_INDICATIONS_BOOT_TO_FW_UI;
	/* preserve other bits if the variable exists, and lift efivarfs's
	 * immutable flag (set on existing variables) through a read-only fd:
	 * opening an immutable file for writing would fail first. */
	int fd = open(p, O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		if (read(fd, buf, sizeof(buf)) == (ssize_t)sizeof(buf)) {
			uint64_t old;
			memcpy(&old, buf + 4, 8);
			val |= old;
		}
		int flags = 0;
		if (ioctl(fd, FS_IOC_GETFLAGS, &flags) == 0 && (flags & FS_IMMUTABLE_FL)) {
			flags &= ~FS_IMMUTABLE_FL;
			ioctl(fd, FS_IOC_SETFLAGS, &flags);
		}
		close(fd);
	}
	memcpy(buf, &attrs, 4);
	memcpy(buf + 4, &val, 8);
	fd = open(p, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
	free(p);
	if (fd < 0)
		return -1;
	ssize_t w = write(fd, buf, sizeof(buf));
	int e = errno;
	close(fd);
	errno = e;
	return w == (ssize_t)sizeof(buf) ? 0 : -1;
}

int kernel_key_subjects(const char *proc_keys, struct ptrvec *out)
{
	char *text = read_file(proc_keys, 1 << 20, NULL);
	if (!text)
		return -1;
	/* "<id> <flags> <usage> <timeout> <perm> <uid> <gid> <type> <description>: <summary>"
	 * e.g. "... asymmetri Canonical Ltd. Master Certificate Authority: ad91…: X509.rsa 2acc2fd0 []" */
	for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
		char type[16];
		int pos = 0;
		if (sscanf(line, "%*s %*s %*s %*s %*s %*s %*s %15s %n", type, &pos) != 1 || strncmp(type, "asymmetri", 9) != 0)
			continue;
		char *d = line + pos;
		char *x = strstr(d, ": X509.");
		if (x)
			*x = '\0';
		char *id = strrchr(d, ':');   /* trailing ": <key id>" */
		if (id && id[1] == ' ' && strspn(id + 2, "0123456789abcdef") == strlen(id + 2))
			*id = '\0';
		bool dup = false;
		for (size_t i = 0; i < out->n && !dup; i++)
			dup = !strcmp(out->v[i], d);
		if (*d && !dup)
			ptrvec_push(out, xstrdup(d));
	}
	free(text);
	return 0;
}
