/*
 * Distro Buny — virtual medium pre-init
 *
 * This small static program is appended (in its own cpio archive) to the
 * initramfs of the distribution being booted and started with
 * rdinit=/distrobuny/preinit.  It must never prevent the distribution from
 * booting, so it does the minimum synchronously:
 *
 *   1. read /distrobuny/medium.conf (written by the Distro Buny runtime),
 *   2. fork a helper process,
 *   3. exec the distribution's own init (normally /init) immediately.
 *
 * The helper runs in a private mount namespace (it never touches the
 * distribution's /dev, /sys or /proc mounts) and:
 *
 *   - waits for the Distro Buny data partition to appear (the distribution's
 *     own udev loads USB/SATA/NVMe drivers), identified by start sector,
 *     size and filesystem serial;
 *   - makes sure the loop (or device-mapper) driver is available;
 *   - attaches the ISO's on-disk extents read-only as /dev/loop200
 *     (contiguous file) or /dev/mapper/distrobuny-medium (fragmented file);
 *   - verifies the ISO 9660 volume descriptor through the new device.
 *
 * The kernel then announces a block device carrying the ISO's own label and
 * UUID, and the distribution finds it exactly as it would find a DVD or a
 * USB stick written with its image.  Progress is reported on /dev/kmsg.
 *
 * Dependencies: libc only.  Written for reliability over cleverness.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/dm-ioctl.h>
#include <linux/loop.h>
#include <sched.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CONF_PATH "/distrobuny/medium.conf"
#define BASE "/distrobuny"
#define DEVDIR BASE "/dev"
#define SYSDIR BASE "/sys"
#define MAX_EXTENTS 4096
#define DM_NAME "distrobuny-medium"

#ifndef MODULE_INIT_COMPRESSED_FILE
#define MODULE_INIT_COMPRESSED_FILE 4
#endif

struct extent {
	uint64_t off, len;
};

struct conf {
	char label[64];
	char next_init[256];
	char fstype[16];
	uint64_t part_start, part_size; /* 512-byte sectors */
	uint32_t serial;
	bool have_serial;
	uint64_t iso_size;
	int loop_index;
	int timeout;
	bool force_dm;
	bool verbose;
	struct extent ext[MAX_EXTENTS];
	int next;
};

static struct conf C;
static int kmsg = -1;
static int console = -1; /* lab mode: mirror messages to the console */

/* Log one line to the kernel log (priority 5, "notice"). */
static void say(const char *fmt, ...)
{
	if (kmsg < 0)
		return;
	char msg[480], line[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	int n = snprintf(line, sizeof(line), "<5>distrobuny-medium: %s\n", msg);
	if (n > 0 && write(kmsg, line, (size_t)n) < 0) {
		/* the kernel log is the only sink; nothing else to do */
	}
	if (console >= 0 && n > 3 && write(console, line + 3, (size_t)n - 3) < 0) {
		/* best effort */
	}
}

static uint64_t ms_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void sleep_ms(unsigned ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
	while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {
	}
}

/* ------------------------------------------------------------ config */

static void trim(char *s)
{
	size_t n = strlen(s);
	while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' '))
		s[--n] = '\0';
}

static int load_conf(void)
{
	FILE *f = fopen(CONF_PATH, "re");
	if (!f)
		return -1;
	memset(&C, 0, sizeof(C));
	snprintf(C.next_init, sizeof(C.next_init), "/init");
	C.loop_index = 200;
	C.timeout = 300;
	char line[512];
	while (fgets(line, sizeof(line), f)) {
		trim(line);
		char *eq = strchr(line, '=');
		if (!eq || line[0] == '#')
			continue;
		*eq = '\0';
		const char *k = line, *v = eq + 1;
		if (!strcmp(k, "label"))
			snprintf(C.label, sizeof(C.label), "%.63s", v);
		else if (!strcmp(k, "next_init"))
			snprintf(C.next_init, sizeof(C.next_init), "%.255s", v);
		else if (!strcmp(k, "fstype"))
			snprintf(C.fstype, sizeof(C.fstype), "%.15s", v);
		else if (!strcmp(k, "part_start"))
			C.part_start = strtoull(v, NULL, 10);
		else if (!strcmp(k, "part_size"))
			C.part_size = strtoull(v, NULL, 10);
		else if (!strcmp(k, "serial")) {
			C.serial = (uint32_t)strtoul(v, NULL, 16);
			C.have_serial = true;
		} else if (!strcmp(k, "iso_size"))
			C.iso_size = strtoull(v, NULL, 10);
		else if (!strcmp(k, "loop_index"))
			C.loop_index = atoi(v);
		else if (!strcmp(k, "timeout"))
			C.timeout = atoi(v);
		else if (!strcmp(k, "mode"))
			C.force_dm = !strcmp(v, "dm");
		else if (!strcmp(k, "verbose"))
			C.verbose = !strcmp(v, "1");
		else if (!strcmp(k, "extent") && C.next < MAX_EXTENTS) {
			unsigned long long o, l;
			if (sscanf(v, "%llu %llu", &o, &l) == 2) {
				C.ext[C.next].off = o;
				C.ext[C.next].len = l;
				C.next++;
			}
		}
	}
	fclose(f);
	return C.next > 0 && C.part_size > 0 ? 0 : -1;
}

/* ----------------------------------------------------- partition lookup */

static int read_u64(const char *path, uint64_t *out)
{
	char buf[64];
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	ssize_t r = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (r <= 0)
		return -1;
	buf[r] = '\0';
	*out = strtoull(buf, NULL, 10);
	return 0;
}

static int read_devnum(const char *path, unsigned *maj, unsigned *min)
{
	char buf[64];
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	ssize_t r = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (r <= 0)
		return -1;
	buf[r] = '\0';
	return sscanf(buf, "%u:%u", maj, min) == 2 ? 0 : -1;
}

static bool verify_fs(const char *dev)
{
	int fd = open(dev, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	uint8_t bs[512];
	bool ok = pread(fd, bs, sizeof(bs), 0) == (ssize_t)sizeof(bs);
	close(fd);
	if (!ok)
		return false;
	if (!C.have_serial)
		return true;
	uint32_t serial;
	if (!strcmp(C.fstype, "exfat")) {
		if (memcmp(bs + 3, "EXFAT   ", 8) != 0)
			return false;
		memcpy(&serial, bs + 100, 4);
	} else if (!strcmp(C.fstype, "vfat")) {
		memcpy(&serial, bs + (memcmp(bs + 82, "FAT32", 5) == 0 ? 67 : 39), 4);
	} else {
		return true;
	}
	return serial == C.serial;
}

/* Find the data partition: returns 0 and its node path, major, minor. */
static int find_partition(char *devpath, size_t len, unsigned *maj, unsigned *min)
{
	DIR *d = opendir(SYSDIR "/class/block");
	if (!d)
		return -1;
	struct dirent *de;
	int rc = -1;
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.' || !strncmp(de->d_name, "loop", 4) || !strncmp(de->d_name, "dm-", 3) ||
		    !strncmp(de->d_name, "ram", 3))
			continue;
		char p[512];
		uint64_t start, size;
		snprintf(p, sizeof(p), SYSDIR "/class/block/%s/start", de->d_name);
		if (read_u64(p, &start) < 0 || start != C.part_start)
			continue;
		snprintf(p, sizeof(p), SYSDIR "/class/block/%s/size", de->d_name);
		if (read_u64(p, &size) < 0 || size != C.part_size)
			continue;
		snprintf(p, sizeof(p), SYSDIR "/class/block/%s/dev", de->d_name);
		if (read_devnum(p, maj, min) < 0)
			continue;
		snprintf(devpath, len, DEVDIR "/%.200s", de->d_name);
		if (access(devpath, F_OK) != 0)
			mknod(devpath, S_IFBLK | 0600, makedev(*maj, *min));
		if (!verify_fs(devpath)) {
			say("%s matches the geometry but not the filesystem serial; skipping", de->d_name);
			continue;
		}
		rc = 0;
		break;
	}
	closedir(d);
	return rc;
}

/* ------------------------------------------------------ kernel modules */

static int run_modprobe(const char *mod)
{
	static const char *cands[] = { "/usr/sbin/modprobe", "/sbin/modprobe", "/usr/bin/modprobe", "/bin/modprobe", NULL };
	for (int i = 0; cands[i]; i++) {
		if (access(cands[i], X_OK) != 0)
			continue;
		pid_t pid = fork();
		if (pid == 0) {
			int null = open(DEVDIR "/null", O_RDWR);
			if (null >= 0) {
				dup2(null, 1);
				dup2(null, 2);
			}
			execl(cands[i], "modprobe", "-q", mod, (char *)NULL);
			_exit(127);
		}
		int st;
		if (pid > 0 && waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0)
			return 0;
	}
	return -1;
}

/* Run the first of `cands` that exists with argv; 0 if it exited with status 0. */
static int run_tool(const char *const *cands, char *const argv[])
{
	static char *const envp[] = { "PATH=/usr/sbin:/usr/bin:/sbin:/bin", NULL };
	for (int i = 0; cands[i]; i++) {
		if (access(cands[i], X_OK) != 0)
			continue;
		pid_t pid = fork();
		if (pid == 0) {
			int null = open(DEVDIR "/null", O_RDWR);
			if (null >= 0) {
				dup2(null, 1);
				dup2(null, 2);
			}
			execve(cands[i], argv, envp);
			_exit(127);
		}
		int st;
		return pid > 0 && waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
	}
	return -1;
}

static int insmod_path(const char *path)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	size_t l = strlen(path);
	bool compressed = (l > 3 && !strcmp(path + l - 3, ".xz")) || (l > 4 && !strcmp(path + l - 4, ".zst")) ||
			  (l > 3 && !strcmp(path + l - 3, ".gz"));
	int rc = (int)syscall(SYS_finit_module, fd, "", compressed ? MODULE_INIT_COMPRESSED_FILE : 0);
	int e = errno;
	close(fd);
	if (rc < 0 && e == EEXIST)
		return 0;
	if (rc < 0 && compressed) {
		/* a kernel without in-kernel module decompression: the distribution's insmod unpacks it */
		static const char *const cands[] = { "/sbin/insmod", "/usr/sbin/insmod", "/bin/insmod", "/usr/bin/insmod", NULL };
		char *const argv[] = { "insmod", (char *)path, NULL };
		if (run_tool(cands, argv) == 0)
			return 0;
	}
	errno = e;
	return rc;
}

/* debian-installer: Distro Buny put the image's own driver packages (udebs)
 * into BASE/udebs; extract their files with the installer's BusyBox into
 * BASE/modules, where ensure_driver() looks first.  Never into /: the udeb's
 * "./lib/" entry would replace the installer's /lib -> usr/lib link, and every
 * program started afterwards fails to find its libraries (its components then
 * failed to load "for unknown reasons"). */
static void unpack_udebs(void)
{
	static bool done;
	if (done)
		return;
	done = true;
	DIR *d = opendir(BASE "/udebs");
	if (!d)
		return;
	static const char *const cands[] = { "/bin/sh", NULL };
	struct dirent *de;
	while ((de = readdir(d))) {
		size_t l = strlen(de->d_name);
		if (l < 6 || strcmp(de->d_name + l - 5, ".udeb") != 0)
			continue;
		char p[512];
		snprintf(p, sizeof(p), BASE "/udebs/%s", de->d_name);
		char *const argv[] = { "sh", "-c", "ar -p \"$0\" data.tar.xz | xzcat | tar -xf - -C " BASE "/modules", p, NULL };
		say(run_tool(cands, argv) == 0 ? "unpacked %s" : "could not unpack %s", de->d_name);
	}
	closedir(d);
}

/* Find "<name>.ko*" under a directory tree (depth-limited). */
static int find_module(const char *dir, const char *name, char *out, size_t outlen, int depth)
{
	if (depth > 8)
		return -1;
	DIR *d = opendir(dir);
	if (!d)
		return -1;
	struct dirent *de;
	int rc = -1;
	size_t nl = strlen(name);
	while (rc < 0 && (de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;
		char p[1024];
		snprintf(p, sizeof(p), "%s/%s", dir, de->d_name);
		if (!strncmp(de->d_name, name, nl) && !strncmp(de->d_name + nl, ".ko", 3)) {
			snprintf(out, outlen, "%s", p);
			rc = 0;
		} else if (de->d_type == DT_DIR) {
			rc = find_module(p, name, out, outlen, depth + 1);
		}
	}
	closedir(d);
	return rc;
}

static void ensure_driver(const char *probe_path, const char *mod, const char *alt_name)
{
	if (access(probe_path, F_OK) == 0)
		return;
	char path[1024];
	/* 1) module injected by Distro Buny from the image itself */
	if (find_module(BASE "/modules", mod, path, sizeof(path), 0) == 0 && insmod_path(path) == 0)
		goto done;
	/* 2) the distribution's own modprobe */
	if (run_modprobe(mod) == 0)
		goto done;
	/* 3) the module file shipped in the distribution's initramfs */
	struct utsname u;
	if (uname(&u) == 0) {
		char base[512];
		snprintf(base, sizeof(base), "/lib/modules/%s", u.release);
		if (find_module(base, alt_name ? alt_name : mod, path, sizeof(path), 0) == 0 && insmod_path(path) == 0)
			goto done;
		snprintf(base, sizeof(base), "/usr/lib/modules/%s", u.release);
		if (find_module(base, alt_name ? alt_name : mod, path, sizeof(path), 0) == 0 && insmod_path(path) == 0)
			goto done;
	}
	say("could not load the %s driver", mod);
	return;
done:
	say("loaded %s driver", mod);
	for (int i = 0; i < 50 && access(probe_path, F_OK) != 0; i++)
		sleep_ms(20);
}

/* ------------------------------------------------------------- attach */

static bool verify_iso(const char *dev)
{
	int fd = open(dev, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	uint8_t vd[2048];
	bool ok = pread(fd, vd, sizeof(vd), 32768) == (ssize_t)sizeof(vd);
	close(fd);
	if (!ok || memcmp(vd + 1, "CD001", 5) != 0)
		return false;
	if (!C.label[0])
		return true;
	char label[33];
	memcpy(label, vd + 40, 32);
	label[32] = '\0';
	for (int i = 31; i >= 0 && (label[i] == ' ' || label[i] == '\0'); i--)
		label[i] = '\0';
	return strcmp(label, C.label) == 0;
}

/* /dev/loop<index>: a read-only window [off, off+len) of `backing`. */
static int attach_loop(const char *backing, uint64_t off, uint64_t len)
{
	unpack_udebs();
	ensure_driver(DEVDIR "/loop-control", "loop", NULL);
	int ctl = open(DEVDIR "/loop-control", O_RDWR | O_CLOEXEC);
	if (ctl < 0) {
		say("loop-control unavailable: %s", strerror(errno));
		return -1;
	}
	int r = ioctl(ctl, LOOP_CTL_ADD, C.loop_index);
	close(ctl);
	if (r < 0 && errno != EEXIST) {
		say("cannot create loop%d: %s", C.loop_index, strerror(errno));
		return -1;
	}
	char loopdev[64];
	snprintf(loopdev, sizeof(loopdev), DEVDIR "/loop%d", C.loop_index);
	for (int i = 0; i < 100 && access(loopdev, F_OK) != 0; i++)
		sleep_ms(10);
	int lfd = open(loopdev, O_RDWR | O_CLOEXEC);
	int bfd = open(backing, O_RDONLY | O_CLOEXEC);
	if (lfd < 0 || bfd < 0) {
		say("cannot open %s or %s: %s", loopdev, backing, strerror(errno));
		if (lfd >= 0)
			close(lfd);
		if (bfd >= 0)
			close(bfd);
		return -1;
	}
	struct loop_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.fd = (uint32_t)bfd;
	cfg.info.lo_offset = off;
	cfg.info.lo_sizelimit = len;
	cfg.info.lo_flags = LO_FLAGS_READ_ONLY;
	snprintf((char *)cfg.info.lo_file_name, LO_NAME_SIZE, "distrobuny:%.52s", C.label);
	r = ioctl(lfd, LOOP_CONFIGURE, &cfg);
	if (r < 0 && (errno == EINVAL || errno == ENOTTY)) {
		/* kernels before 5.8 */
		r = ioctl(lfd, LOOP_SET_FD, bfd);
		if (r == 0) {
			struct loop_info64 info;
			memset(&info, 0, sizeof(info));
			info.lo_offset = off;
			info.lo_sizelimit = len;
			snprintf((char *)info.lo_file_name, LO_NAME_SIZE, "distrobuny:%.52s", C.label);
			r = ioctl(lfd, LOOP_SET_STATUS64, &info);
		}
	}
	int e = errno;
	close(bfd);
	close(lfd);
	if (r < 0) {
		say("loop setup failed: %s", strerror(e));
		return -1;
	}
	if (!verify_iso(loopdev)) {
		say("loop%d does not contain the expected ISO (%s); detaching", C.loop_index, C.label);
		lfd = open(loopdev, O_RDWR | O_CLOEXEC);
		if (lfd >= 0) {
			ioctl(lfd, LOOP_CLR_FD, 0);
			close(lfd);
		}
		return -1;
	}
	say("attached /dev/loop%d: \"%s\" (%llu bytes at offset %llu)", C.loop_index, C.label,
	    (unsigned long long)len, (unsigned long long)off);
	return 0;
}

static void dm_init(struct dm_ioctl *io, size_t size, const char *name)
{
	memset(io, 0, size);
	io->version[0] = DM_VERSION_MAJOR;
	io->version[1] = 0;
	io->version[2] = 0;
	io->data_size = (uint32_t)size;
	io->data_start = sizeof(struct dm_ioctl);
	snprintf(io->name, sizeof(io->name), "%s", name);
}

static int attach_dm(unsigned maj, unsigned min)
{
	ensure_driver(DEVDIR "/mapper/control", "dm-mod", "dm_mod");
	if (access(DEVDIR "/mapper/control", F_OK) != 0) {
		/* devtmpfs names it "mapper/control" once dm is loaded */
		say("device-mapper is unavailable");
		return -1;
	}
	int ctl = open(DEVDIR "/mapper/control", O_RDWR | O_CLOEXEC);
	if (ctl < 0)
		return -1;
	size_t size = sizeof(struct dm_ioctl) + (size_t)C.next * (sizeof(struct dm_target_spec) + 64) + 64;
	struct dm_ioctl *io = calloc(1, size);
	if (!io) {
		close(ctl);
		return -1;
	}
	dm_init(io, sizeof(struct dm_ioctl), DM_NAME);
	if (ioctl(ctl, DM_DEV_CREATE, io) < 0 && errno != EBUSY) {
		say("DM_DEV_CREATE failed: %s", strerror(errno));
		goto fail;
	}
	dm_init(io, size, DM_NAME);
	io->target_count = (uint32_t)C.next;
	io->flags = DM_READONLY_FLAG;
	char *p = (char *)io + sizeof(struct dm_ioctl);
	uint64_t sector = 0;
	for (int i = 0; i < C.next; i++) {
		struct dm_target_spec *sp = (struct dm_target_spec *)p;
		memset(sp, 0, sizeof(*sp));
		sp->sector_start = sector;
		sp->length = C.ext[i].len / 512;
		snprintf(sp->target_type, sizeof(sp->target_type), "linear");
		char *params = p + sizeof(*sp);
		int pl = snprintf(params, 64, "%u:%u %llu", maj, min, (unsigned long long)(C.ext[i].off / 512));
		size_t step = sizeof(*sp) + (size_t)pl + 1;
		step = (step + 7) & ~(size_t)7;
		sp->next = (uint32_t)step;
		sector += sp->length;
		p += step;
	}
	if (ioctl(ctl, DM_TABLE_LOAD, io) < 0) {
		say("DM_TABLE_LOAD failed: %s", strerror(errno));
		goto fail;
	}
	dm_init(io, sizeof(struct dm_ioctl), DM_NAME);
	if (ioctl(ctl, DM_DEV_SUSPEND, io) < 0) { /* without SUSPEND flag: resume */
		say("DM resume failed: %s", strerror(errno));
		goto fail;
	}
	unsigned dmaj = major(io->dev), dmin = minor(io->dev);
	mkdir(DEVDIR "/mapper", 0755);
	char node[128];
	snprintf(node, sizeof(node), DEVDIR "/mapper/" DM_NAME);
	unlink(node);
	/* devtmpfs is shared by every mount, so this node is visible to the distribution too */
	mknod(node, S_IFBLK | 0600, makedev(dmaj, dmin));
	free(io);
	close(ctl);
	if (!verify_iso(node)) {
		say("device-mapper medium does not contain the expected ISO (%s)", C.label);
		return -1;
	}
	say("assembled /dev/mapper/%s from %d extents", DM_NAME, C.next);
	/* Present it as /dev/loop<index> like a contiguous file, so every adapter
	 * (some name /dev/loop200 directly) and udev's label probing see the
	 * same kind of device either way. */
	return attach_loop(node, 0, sector * 512);
fail:
	free(io);
	close(ctl);
	return -1;
}

/* The kernel announces the configured loop device once ("change").  A
 * distribution's udev that is still starting can miss that event and then
 * never creates /dev/disk/by-label/<label> (seen in the lab with Void and
 * BlackArch, intermittently).  Repeat the announcement for a while: udev
 * handles a repeated event idempotently (it re-probes a read-only device). */
static void reannounce(void)
{
	static const unsigned at_ms[] = { 500, 1500, 3000, 6000, 10000, 20000 };
	char path[128];
	snprintf(path, sizeof(path), SYSDIR "/block/loop%d/uevent", C.loop_index);
	uint64_t t0 = ms_now();
	for (size_t i = 0; i < sizeof(at_ms) / sizeof(at_ms[0]); i++) {
		uint64_t elapsed = ms_now() - t0;
		if (at_ms[i] > elapsed)
			sleep_ms((unsigned)(at_ms[i] - elapsed));
		int fd = open(path, O_WRONLY | O_CLOEXEC);
		if (fd < 0)
			return;
		if (write(fd, "change", 6) < 0) {
			/* the device is gone: nothing left to announce */
		}
		close(fd);
	}
}

static void helper(void)
{
	if (unshare(CLONE_NEWNS) < 0)
		say("unshare failed: %s", strerror(errno));
	mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);
	mkdir(DEVDIR, 0755);
	mkdir(SYSDIR, 0755);
	if (mount("devtmpfs", DEVDIR, "devtmpfs", MS_NOSUID, "mode=0755") < 0) {
		say("cannot mount devtmpfs: %s", strerror(errno));
		_exit(1);
	}
	if (kmsg < 0)
		kmsg = open(DEVDIR "/kmsg", O_WRONLY | O_CLOEXEC);
	if (C.verbose)
		console = open(DEVDIR "/console", O_WRONLY | O_NOCTTY | O_CLOEXEC);
	if (mount("sysfs", SYSDIR, "sysfs", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL) < 0) {
		say("cannot mount sysfs: %s", strerror(errno));
		_exit(1);
	}
	/* modprobe expects /proc and /sys at their usual places: provide them
	 * in this private namespace only. */
	mount("proc", "/proc", "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL);
	mount("sysfs", "/sys", "sysfs", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL);

	say("waiting for the Distro Buny data partition (start %llu, %llu sectors) to attach \"%s\"",
	    (unsigned long long)C.part_start, (unsigned long long)C.part_size, C.label);
	uint64_t deadline = ms_now() + (uint64_t)C.timeout * 1000;
	char dev[256];
	unsigned maj = 0, min = 0;
	unsigned delay = 25;
	while (find_partition(dev, sizeof(dev), &maj, &min) < 0) {
		if (ms_now() > deadline) {
			say("data partition not found within %d s; giving up", C.timeout);
			_exit(1);
		}
		sleep_ms(delay);
		if (delay < 250)
			delay += 25;
	}
	say("data partition is %s", dev + strlen(DEVDIR) + 1);
	int rc = (C.next == 1 && !C.force_dm) ? attach_loop(dev, C.ext[0].off, C.ext[0].len) : attach_dm(maj, min);
	if (rc == 0)
		reannounce();
	_exit(rc == 0 ? 0 : 1);
}

int main(int argc, char **argv)
{
	kmsg = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
	if (kmsg < 0) {
		/* /dev may not be populated yet in the bare initramfs */
		mkdir("/dev", 0755);
		if (mount("devtmpfs", "/dev", "devtmpfs", MS_NOSUID, "mode=0755") == 0) {
			kmsg = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
			/* leave /dev as the distribution expects to find it: unmounted */
			if (kmsg >= 0)
				umount2("/dev", MNT_DETACH);
		}
	}
	bool have_conf = load_conf() == 0;
	if (!have_conf)
		say("no usable medium configuration; continuing without the virtual medium");
	if (have_conf) {
		pid_t pid = fork();
		if (pid == 0) {
			/* systemd spares initrd processes whose argv[0] starts with '@' */
			if (argc > 0 && argv[0][0])
				argv[0][0] = '@';
			prctl(PR_SET_NAME, "buny-medium", 0, 0, 0);
			setsid();
			helper();
			_exit(0);
		}
		if (pid < 0)
			say("fork failed: %s", strerror(errno));
	}
	char *next = have_conf ? C.next_init : (char *)"/init";
	if (kmsg >= 0)
		close(kmsg);
	argv[0] = next;
	execv(next, argv);
	/* fall back to the usual initramfs entry points */
	static const char *fallback[] = { "/init", "/sbin/init", "/usr/lib/systemd/systemd", "/linuxrc", NULL };
	for (int i = 0; fallback[i]; i++) {
		argv[0] = (char *)fallback[i];
		execv(fallback[i], argv);
	}
	return 127;
}
