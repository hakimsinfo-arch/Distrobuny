/*
 * buny-init — PID 1 of the Distro Buny runtime
 *
 * Responsibilities (everything that needs privileges):
 *   - mount the kernel API file systems and set up logging;
 *   - load drivers for present and hot-plugged hardware;
 *   - find the Distro Buny drive (ESP named by GRUB, data partition beside it)
 *     and mount the data partition read-only;
 *   - grow the data partition to the end of a bigger stick when the user asks
 *     (and finish an expansion a power cut interrupted) — filesystem/expand.c;
 *   - start the graphical interface as an unprivileged user and supervise it;
 *   - serve the interface's requests: hand-off (kexec), power actions and
 *     whitelisted state writes on the ESP.
 *
 * Kernel command line options (set by GRUB):
 *   buny.esp=<PARTUUID>   ESP the runtime was loaded from
 *   buny.lab=1            lab mode: machine-readable events on the serial console
 *   buny.autoboot=<path>  boot this ISO without user interaction (tests)
 *   buny.dryrun=1         validate hand-offs but never kexec (tests)
 *   buny.medium=dm        attach the virtual medium through device-mapper (tests)
 *   buny.shell=1          start a root shell instead of the interface (debugging)
 *   buny.debug=1          verbose logging
 *   buny.expandcrash=N    lab only: cut the power after N writes of a drive expansion
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/blkpg.h>
#include <linux/kd.h>
#include <linux/netlink.h>
#include <linux/vt.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/klog.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "boot-adapters/bootcfg.h"
#include "boot/extents.h"
#include "boot/handoff.h"
#include "boot/ipc.h"
#include "boot/modload.h"
#include "common/json.h"
#include "common/log.h"
#include "common/strbuf.h"
#include "filesystem/blockdev.h"
#include "filesystem/expand.h"
#include "security/secureboot.h"

#ifndef BUNY_VERSION
#define BUNY_VERSION "dev"
#endif

#define UI_UID 100
#define UI_GID 100
#define RUN_DIR "/run/distrobuny"
#define DATA_MNT RUN_DIR "/data"
#define ESP_MNT RUN_DIR "/esp"
#define PREINIT "/usr/lib/distrobuny/preinit"
#define UI_BIN "/usr/bin/buny-ui"
#define FIND_TIMEOUT_MS 20000

struct opts {
	char esp_partuuid[40];
	char autoboot[1024];
	bool lab, dryrun, shell, debug, medium_dm;
	unsigned expand_crash;
};

static struct opts O;
static struct modctx *MODS;
static struct blk_dev DATA, ESP;
static bool HAVE_DATA, HAVE_ESP;
static int SERIAL = -1;
static pid_t UI_PID = -1;
static int UI_SOCK = -1;
static int UI_LOG = -1;
static int UI_RESTARTS;
static struct handoff_payload LOADED = { -1, -1, NULL, false };
static bool KERNEL_LOADED;

/* What the menu needs to know about growing the data partition (runtime.json "expand"). */
static struct {
	enum expand_state state;
	char code[24];
	uint64_t disk_bytes, data_bytes, new_data_bytes;
	bool limited, done;
	char reason[512];
} XP;

/* ------------------------------------------------------------ logging */

static void serial_line(const char *s)
{
	if (SERIAL >= 0) {
		if (write(SERIAL, s, strlen(s)) < 0 || write(SERIAL, "\r\n", 2) < 0) {
			/* serial console gone */
		}
	}
}

/* Machine-readable lab events: "BUNY-EVENT key=value ..." on the serial line. */
static void lab_event(const char *fmt, ...) BUNY_PRINTF(1, 2);
static void lab_event(const char *fmt, ...)
{
	if (!O.lab)
		return;
	va_list ap;
	va_start(ap, fmt);
	char *m = xvasprintf(fmt, ap);
	va_end(ap);
	char *line = xasprintf("BUNY-EVENT %s", m);
	serial_line(line);
	free(line);
	free(m);
}

static void setup_logging(void)
{
	int kmsg = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
	log_set_tag("buny-init");
	log_set_stderr(false);
	if (kmsg >= 0)
		log_set_extra_fd(kmsg, true);
	log_set_level(O.debug ? LOG_LVL_DEBUG : LOG_LVL_INFO);
	if (O.lab) {
		SERIAL = open("/dev/ttyS0", O_WRONLY | O_NOCTTY | O_CLOEXEC);
		struct termios t;
		if (SERIAL >= 0 && tcgetattr(SERIAL, &t) == 0) {
			cfsetospeed(&t, B115200);
			cfsetispeed(&t, B115200);
			t.c_oflag |= OPOST | ONLCR;
			tcsetattr(SERIAL, TCSANOW, &t);
		}
	}
}

/* ------------------------------------------------------------ setup */

static void must_mount(const char *src, const char *dst, const char *type, unsigned long flags, const char *data)
{
	mkdir(dst, 0755);
	if (mount(src, dst, type, flags, data) < 0 && errno != EBUSY)
		LOGE("mount %s on %s failed: %s", type, dst, strerror(errno));
}

static void mount_api(void)
{
	must_mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
	must_mount("sysfs", "/sys", "sysfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
	must_mount("devtmpfs", "/dev", "devtmpfs", MS_NOSUID, "mode=0755");
	must_mount("tmpfs", "/run", "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755,size=64m");
	must_mount("tmpfs", "/tmp", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777,size=32m");
	mkdir("/dev/pts", 0755);
	must_mount("devpts", "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC, "gid=5,mode=620,ptmxmode=000");
	if (access("/sys/kernel/security", F_OK) == 0)
		must_mount("securityfs", "/sys/kernel/security", "securityfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
	if (access("/sys/firmware/efi/efivars", F_OK) == 0)
		must_mount("efivarfs", "/sys/firmware/efi/efivars", "efivarfs", MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_RDONLY, NULL);
	mkdir(RUN_DIR, 0755);
}

static void parse_options(const char *cl)
{
	struct ptrvec args = { 0 };
	cmdline_split(cl, &args);
	for (size_t i = 0; i < args.n; i++) {
		const char *a = args.v[i];
		if (str_starts(a, "buny.esp="))
			snprintf(O.esp_partuuid, sizeof(O.esp_partuuid), "%.39s", a + 9);
		else if (str_starts(a, "buny.autoboot="))
			snprintf(O.autoboot, sizeof(O.autoboot), "%.1023s", a + 14);
		else if (!strcmp(a, "buny.lab=1"))
			O.lab = true;
		else if (!strcmp(a, "buny.dryrun=1"))
			O.dryrun = true;
		else if (!strcmp(a, "buny.medium=dm"))
			O.medium_dm = true; /* lab: exercise the fragmented-file path */
		else if (!strcmp(a, "buny.shell=1"))
			O.shell = true;
		else if (!strcmp(a, "buny.debug=1"))
			O.debug = true;
		else if (str_starts(a, "buny.expandcrash="))
			O.expand_crash = (unsigned)strtoul(a + 17, NULL, 10);
	}
	ptrvec_free(&args, free);
	/* PARTUUIDs compare case-insensitively; store lower case */
	str_lower(O.esp_partuuid);
}

static void parse_cmdline(void)
{
	char *cl = read_file("/proc/cmdline", 8192, NULL);
	if (cl) {
		parse_options(cl);
		free(cl);
	}
}

/* Lab automation inside QEMU: extra buny.* options from the fw_cfg item
 * "opt/distrobuny/cmdline".  fw_cfg exists only in virtual
 * machines, so this channel cannot be used on real hardware. */
#define FW_CFG_OPTS "/sys/firmware/qemu_fw_cfg/by_name/opt/distrobuny/cmdline/raw"
static void parse_fw_cfg(void)
{
	if (MODS)
		modload_name(MODS, "qemu_fw_cfg");
	char *t = read_file(FW_CFG_OPTS, 4096, NULL);
	if (!t)
		return;
	/* only Distro Buny options are honoured */
	struct ptrvec args = { 0 };
	cmdline_split(t, &args);
	struct strbuf sb = STRBUF_INIT;
	for (size_t i = 0; i < args.n; i++)
		if (str_starts(args.v[i], "buny.")) {
			sb_addc(&sb, '"');
			sb_add(&sb, args.v[i]);
			sb_add(&sb, "\" ");
		}
	ptrvec_free(&args, free);
	free(t);
	if (sb.len) {
		bool was_lab = O.lab;
		parse_options(sb.buf);
		if (O.lab && !was_lab)
			setup_logging();
		LOGI("applied virtual-machine lab options");
	}
	sb_free(&sb);
}

static int open_uevent_socket(void)
{
	int s = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);
	if (s < 0)
		return -1;
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK, .nl_groups = 1 };
	if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		close(s);
		return -1;
	}
	int sz = 1 << 20;
	setsockopt(s, SOL_SOCKET, SO_RCVBUFFORCE, &sz, sizeof(sz));
	return s;
}

/* Give the interface access to display and input device nodes. */
static void grant_ui_devices(void)
{
	const char *dirs[] = { "/dev/dri", "/dev/input", NULL };
	for (int i = 0; dirs[i]; i++) {
		DIR *d = opendir(dirs[i]);
		if (!d)
			continue;
		struct dirent *de;
		while ((de = readdir(d))) {
			if (de->d_name[0] == '.')
				continue;
			char *p = path_join(dirs[i], de->d_name);
			struct stat st;
			if (stat(p, &st) == 0 && S_ISCHR(st.st_mode) && st.st_uid != UI_UID) {
				if (chown(p, UI_UID, UI_GID) < 0 || chmod(p, 0600) < 0)
					LOGW("cannot grant %s: %s", p, strerror(errno));
			}
			free(p);
		}
		closedir(d);
	}
	struct stat st;
	if (stat("/dev/fb0", &st) == 0 && st.st_uid != UI_UID && chown("/dev/fb0", UI_UID, UI_GID) == 0)
		chmod("/dev/fb0", 0600);
}

static void handle_uevents(int s)
{
	char buf[8192];
	for (;;) {
		ssize_t n = recv(s, buf, sizeof(buf) - 1, 0);
		if (n <= 0)
			break;
		buf[n] = '\0';
		const char *action = NULL, *modalias = NULL, *subsystem = NULL;
		for (char *p = buf; p < buf + n; p += strlen(p) + 1) {
			if (str_starts(p, "ACTION="))
				action = p + 7;
			else if (str_starts(p, "MODALIAS="))
				modalias = p + 9;
			else if (str_starts(p, "SUBSYSTEM="))
				subsystem = p + 10;
		}
		if (!action || strcmp(action, "add") != 0)
			continue;
		if (modalias && MODS)
			modload_alias(MODS, modalias);
		if (subsystem && (!strcmp(subsystem, "input") || !strcmp(subsystem, "drm") || !strcmp(subsystem, "graphics")))
			grant_ui_devices();
	}
}

/* ------------------------------------------------------- boot drive */

static bool find_boot_drive(void)
{
	struct blk_dev *v;
	size_t n;
	if (blk_enumerate("/sys", "/dev", &v, &n) < 0)
		return false;
	const struct blk_dev *esp = O.esp_partuuid[0] ? blk_find_by_partuuid(v, n, O.esp_partuuid) : NULL;
	const struct blk_dev *data = NULL;
	if (esp) {
		/* data partition: same disk, GPT name DISTROBUNY (or filesystem label) */
		for (size_t i = 0; i < n; i++)
			if (!strcmp(v[i].disk, esp->disk) && (!strcmp(v[i].partlabel, "DISTROBUNY") || !strcmp(v[i].label, "DISTROBUNY")))
				data = &v[i];
	}
	if (!data) {
		/* fallback: any partition labelled DISTROBUNY (e.g. booted without buny.esp) */
		for (size_t i = 0; i < n && !data; i++)
			if (v[i].partno && (!strcmp(v[i].partlabel, "DISTROBUNY") || !strcmp(v[i].label, "DISTROBUNY")))
				data = &v[i];
		if (data && !esp)
			for (size_t i = 0; i < n; i++)
				if (!strcmp(v[i].disk, data->disk) && !strcasecmp(v[i].parttype, GPT_TYPE_ESP))
					esp = &v[i];
	}
	if (data) {
		DATA = *data;
		HAVE_DATA = true;
	}
	if (esp) {
		ESP = *esp;
		HAVE_ESP = true;
	}
	free(v);
	return HAVE_DATA;
}

static int mount_fs(const struct blk_dev *d, const char *where, bool ro)
{
	mkdir(where, 0755);
	char opts[160];
	const char *type = d->fstype;
	if (!strcmp(type, "exfat") || !strcmp(type, "vfat")) {
		modload_name(MODS, type);
		modload_name(MODS, "nls_utf8");
		modload_name(MODS, "nls_cp437");
		modload_name(MODS, "nls_iso8859_1");
		snprintf(opts, sizeof(opts), "uid=%d,gid=%d,fmask=0133,dmask=0022,iocharset=utf8", UI_UID, UI_GID);
		if (!strcmp(type, "vfat"))
			strcat(opts, ",shortname=mixed");
	} else if (!strcmp(type, "ntfs")) {
		type = "ntfs3";
		modload_name(MODS, "ntfs3");
		snprintf(opts, sizeof(opts), "uid=%d,gid=%d,fmask=0133,dmask=0022", UI_UID, UI_GID);
	} else {
		opts[0] = '\0';
	}
	unsigned long flags = MS_NOSUID | MS_NODEV | MS_NOEXEC | (ro ? MS_RDONLY : 0);
	if (mount(d->devpath, where, type, flags, opts[0] ? opts : NULL) < 0) {
		LOGE("mounting %s (%s) on %s failed: %s", d->devpath, type, where, strerror(errno));
		return -1;
	}
	LOGI("mounted %s (%s \"%s\") on %s%s", d->devpath, d->fstype, d->label, where, ro ? " read-only" : "");
	return 0;
}

/* ---------------------------------------------------- drive expansion */

static char *disk_path(const struct blk_dev *d)
{
	return xasprintf("/dev/%s", d->disk);
}

/* Plan growing DATA's partition.  fd: the whole disk, opened by the caller. */
static int plan_expansion(int fd, struct expand_plan *p, char *err, size_t errlen)
{
	uint64_t bytes = 0;
	int ssz = 0;
	if (ioctl(fd, BLKGETSIZE64, &bytes) < 0 || ioctl(fd, BLKSSZGET, &ssz) < 0) {
		snprintf(err, errlen, "cannot read the size of the drive: %s", strerror(errno));
		return -1;
	}
	if (expand_plan(fd, bytes, (unsigned)ssz, p) < 0) {
		snprintf(err, errlen, "cannot read the drive: %s", strerror(errno));
		return -1;
	}
	if (p->state != EXPAND_NONE && DATA.readonly && p->state != EXPAND_REFUSED) {
		p->state = EXPAND_REFUSED;
		p->code = "read-only";
		snprintf(p->reason, sizeof(p->reason), "%s", "The drive is write-protected (check the lock switch of an SD card).");
	}
	/* Only ever write to the drive this computer started from: GRUB named its
	 * ESP, and DISTROBUNY must sit on the same disk.  A DISTROBUNY partition
	 * found any other way (another stick, a copied disk) is left alone. */
	bool booted = O.esp_partuuid[0] && HAVE_ESP && !strcasecmp(ESP.partuuid, O.esp_partuuid) && !strcmp(ESP.disk, DATA.disk);
	if ((p->state == EXPAND_POSSIBLE || p->state == EXPAND_RESUME) && !booted) {
		p->state = EXPAND_REFUSED;
		p->code = "not-boot-drive";
		snprintf(p->reason, sizeof(p->reason), "%s", "This computer was not started from this drive's boot partition, "
			 "so Distro Buny does not change it. Start the computer from the drive itself to expand it.");
	}
	return 0;
}

static void remember_plan(const struct expand_plan *p)
{
	XP.state = p->state;
	snprintf(XP.code, sizeof(XP.code), "%s", p->code);
	XP.disk_bytes = p->disk_sectors * 512;
	XP.data_bytes = p->data_bytes;
	XP.new_data_bytes = p->new_data_bytes;
	XP.limited = p->fs_limited;
	snprintf(XP.reason, sizeof(XP.reason), "%s", p->state == EXPAND_REFUSED ? p->reason : "");
	lab_event("expand-plan state=%s code=%s disk=%llu data=%llu new=%llu", expand_state_name(p->state), p->code,
		  (unsigned long long)XP.disk_bytes, (unsigned long long)XP.data_bytes, (unsigned long long)XP.new_data_bytes);
}

/* Grow (or finish growing) DATA's partition; the data partition must not be
 * mounted.  Tells the kernel the new partition size and re-reads DATA.
 * lab_cut: honour buny.expandcrash (the expansion the user asked for). */
static int run_expansion(int fd, struct expand_plan *p, bool lab_cut, char *err, size_t errlen)
{
	if (lab_cut && O.lab && O.expand_crash)
		p->stop_after_writes = O.expand_crash;
	LOGI("growing %s from %llu to %llu bytes%s", DATA.devpath, (unsigned long long)p->data_bytes,
	     (unsigned long long)p->new_data_bytes, p->state == EXPAND_RESUME ? " (finishing an interrupted expansion)" : "");
	lab_event("expand-start state=%s new=%llu", expand_state_name(p->state), (unsigned long long)p->new_data_bytes);
	uint64_t t0 = now_ms();
	if (expand_run(fd, p, err, errlen) < 0) {
		if (errno == EINTR && p->stop_after_writes) {
			/* lab: a power cut in the middle of the expansion */
			lab_event("expand-power-cut writes=%u", p->writes);
			reboot(RB_AUTOBOOT);
		}
		LOGE("growing the data partition failed: %s", err);
		lab_event("expand-failed reason=\"%s\"", err);
		return -1;
	}
	struct blkpg_partition bp = {
		.start = (long long)(p->part_first * 512),
		.length = (long long)((p->new_part_last - p->part_first + 1) * 512),
		.pno = DATA.partno,
	};
	struct blkpg_ioctl_arg arg = { .op = BLKPG_RESIZE_PARTITION, .datalen = sizeof(bp), .data = &bp };
	if (ioctl(fd, BLKPG, &arg) < 0 && ioctl(fd, BLKRRPART) < 0)
		LOGW("the kernel did not take the new partition size (%s); it applies after a restart", strerror(errno));
	int pf = open(DATA.devpath, O_RDONLY | O_CLOEXEC);
	if (pf >= 0) {
		ioctl(pf, BLKFLSBUF); /* nothing cached from before may be used again */
		close(pf);
	}
	ioctl(fd, BLKFLSBUF);
	find_boot_drive();
	XP.state = EXPAND_NONE;
	snprintf(XP.code, sizeof(XP.code), "done");
	XP.data_bytes = p->new_data_bytes;
	XP.done = true;
	LOGI("the data partition now has %llu bytes (%u writes, %llu ms)", (unsigned long long)p->new_data_bytes, p->writes,
	     (unsigned long long)(now_ms() - t0));
	lab_event("expand-done data=%llu partition=%llu writes=%u ms=%llu", (unsigned long long)p->new_data_bytes,
		  (unsigned long long)(DATA.size * 512), p->writes, (unsigned long long)(now_ms() - t0));
	return 0;
}

/* lab: the kernel's complaints about partition tables (a GPT whose backup is
 * not at the end of the disk, as on a stick bigger than the image) */
static void lab_kernel_gpt_messages(void)
{
	if (!O.lab)
		return;
	int len = klogctl(10, NULL, 0); /* SYSLOG_ACTION_SIZE_BUFFER */
	if (len <= 0)
		return;
	char *buf = xmalloc((size_t)len + 1);
	int n = klogctl(3, buf, len); /* SYSLOG_ACTION_READ_ALL */
	if (n > 0) {
		buf[n] = '\0';
		for (char *save = NULL, *l = strtok_r(buf, "\n", &save); l; l = strtok_r(NULL, "\n", &save))
			if (strstr(l, "GPT:"))
				lab_event("kernel-gpt message=\"%s\"", strstr(l, "GPT:"));
	}
	free(buf);
}

/* At start-up, before the data partition is mounted: finish an interrupted
 * expansion (the user confirmed it before the power cut), and find out
 * whether the menu can offer one. */
static void check_expansion(void)
{
	lab_kernel_gpt_messages();
	char *dev = disk_path(&DATA);
	int fd = open(dev, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		fd = open(dev, O_RDONLY | O_CLOEXEC);
	free(dev);
	if (fd < 0)
		return;
	struct expand_plan p;
	char err[512];
	if (plan_expansion(fd, &p, err, sizeof(err)) == 0) {
		remember_plan(&p);
		if (p.state == EXPAND_RESUME && run_expansion(fd, &p, false, err, sizeof(err)) == 0) {
			expand_plan_free(&p);
			if (plan_expansion(fd, &p, err, sizeof(err)) == 0)
				remember_plan(&p);
			XP.done = true;
		}
		expand_plan_free(&p);
	} else {
		LOGW("drive expansion: %s", err);
	}
	close(fd);
}

static uint64_t uptime_ms(void)
{
	char *s = read_file("/proc/uptime", 64, NULL);
	double up = s ? strtod(s, NULL) : 0;
	free(s);
	return (uint64_t)(up * 1000.0);
}

/* --------------------------------------------------------- runtime info */

static void write_runtime_info(uint64_t boot_ms)
{
	struct strbuf sb = STRBUF_INIT;
	struct json_writer w;
	jw_init(&w, &sb, true);
	struct utsname u;
	uname(&u);
	struct fw_state fw;
	fw_state_read("/sys", &fw);
	struct sysinfo si;
	sysinfo(&si);
	jw_obj_begin(&w);
	jw_key(&w, "version");
	jw_str(&w, BUNY_VERSION);
	jw_key(&w, "kernel");
	jw_str(&w, u.release);
	jw_key(&w, "boot_mode");
	jw_str(&w, fw.uefi ? "uefi" : "bios");
	jw_key(&w, "secure_boot");
	jw_str(&w, sb_state_name(fw.secure_boot));
	jw_key(&w, "setup_mode");
	jw_bool(&w, fw.setup_mode);
	jw_key(&w, "lockdown");
	jw_str(&w, fw.lockdown);
	jw_key(&w, "firmware_setup_supported");
	jw_bool(&w, fw.fw_ui_supported);
	jw_key(&w, "firmware_vendor");
	jw_str(&w, fw.fw_vendor);
	jw_key(&w, "memory");
	jw_int(&w, (long long)si.totalram * si.mem_unit);
	jw_key(&w, "cpus");
	jw_int(&w, sysconf(_SC_NPROCESSORS_ONLN));
	char *cpuinfo = read_file("/proc/cpuinfo", 1 << 20, NULL);
	const char *model = cpuinfo ? strstr(cpuinfo, "model name") : NULL;
	if (model) {
		const char *c = strchr(model, ':');
		const char *nl = c ? strchr(c, '\n') : NULL;
		if (c && nl) {
			char *m = xstrndup(c + 2, (size_t)(nl - c - 2));
			jw_key(&w, "cpu");
			jw_str(&w, m);
			free(m);
		}
	}
	free(cpuinfo);
	jw_key(&w, "lab");
	jw_bool(&w, O.lab);
	jw_key(&w, "dry_run");
	jw_bool(&w, O.dryrun);
	jw_key(&w, "autoboot");
	jw_str(&w, O.autoboot[0] ? O.autoboot : NULL);
	jw_key(&w, "boot_ms");
	jw_int(&w, (long long)boot_ms);
	jw_key(&w, "data");
	if (HAVE_DATA) {
		struct statvfs vs;
		bool st = statvfs(DATA_MNT, &vs) == 0;
		jw_obj_begin(&w);
		jw_key(&w, "device");
		jw_str(&w, DATA.devpath);
		jw_key(&w, "fs");
		jw_str(&w, DATA.fstype);
		jw_key(&w, "uuid");
		jw_str(&w, DATA.uuid);
		jw_key(&w, "label");
		jw_str(&w, DATA.label);
		jw_key(&w, "mount");
		jw_str(&w, DATA_MNT);
		jw_key(&w, "size");
		jw_int(&w, (long long)(DATA.size * 512));
		jw_key(&w, "free");
		jw_int(&w, st ? (long long)vs.f_bavail * (long long)vs.f_frsize : -1);
		jw_key(&w, "disk_model");
		jw_str(&w, DATA.model);
		jw_key(&w, "removable");
		jw_bool(&w, DATA.removable);
		jw_obj_end(&w);
	} else {
		jw_null(&w);
	}
	jw_key(&w, "esp");
	jw_str(&w, HAVE_ESP ? ESP_MNT : NULL);
	jw_key(&w, "expand");
	jw_obj_begin(&w);
	jw_key(&w, "state");
	jw_str(&w, XP.done && XP.state == EXPAND_NONE ? "done" : expand_state_name(XP.state));
	jw_key(&w, "code");
	jw_str(&w, XP.code[0] ? XP.code : "none");
	jw_key(&w, "disk_bytes");
	jw_int(&w, (long long)XP.disk_bytes);
	jw_key(&w, "data_bytes");
	jw_int(&w, (long long)XP.data_bytes);
	jw_key(&w, "new_data_bytes");
	jw_int(&w, (long long)XP.new_data_bytes);
	jw_key(&w, "limited");
	jw_bool(&w, XP.limited);
	jw_key(&w, "reason");
	jw_str(&w, XP.reason);
	jw_obj_end(&w);
	/* which signing authorities this kernel trusts: lets the menu predict
	 * under Secure Boot which systems can be started */
	struct ptrvec keys = { 0 };
	if (kernel_key_subjects("/proc/keys", &keys) == 0) {
		jw_key(&w, "trusted_keys");
		jw_arr_begin(&w);
		for (size_t i = 0; i < keys.n; i++)
			jw_str(&w, keys.v[i]);
		jw_arr_end(&w);
		lab_event("trusted-keys count=%zu", keys.n);
	}
	ptrvec_free(&keys, free);
	jw_obj_end(&w);
	sb_addc(&sb, '\n');
	write_file_atomic(RUN_DIR "/runtime.json", sb.buf, sb.len);
	sb_free(&sb);
}

/* ---------------------------------------------------------- console */

static void console_graphics(bool on)
{
	int fd = open("/dev/tty1", O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (fd < 0)
		return;
	ioctl(fd, KDSETMODE, on ? KD_GRAPHICS : KD_TEXT);
	close(fd);
	/* keep kernel messages off the display while the interface runs */
	klogctl(8, NULL, on ? 1 : 4);
}

/* ---------------------------------------------------------- the UI */

static void spawn_ui(void)
{
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) < 0) {
		LOGE("socketpair: %s", strerror(errno));
		return;
	}
	int lp[2];
	if (pipe2(lp, O_CLOEXEC) < 0) {
		close(sv[0]);
		close(sv[1]);
		return;
	}
	grant_ui_devices();
	console_graphics(true);
	pid_t pid = fork();
	if (pid == 0) {
		/* child: drop to the unprivileged interface user */
		dup2(sv[1], 3);
		dup2(lp[1], 2);
		int null = open("/dev/null", O_RDWR);
		if (null >= 0) {
			dup2(null, 0);
			dup2(null, 1);
		}
		if (setgroups(0, NULL) < 0 || setgid(UI_GID) < 0 || setuid(UI_UID) < 0)
			_exit(126);
		prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
		char *envp[] = { "HOME=/tmp", "PATH=/usr/bin:/bin", "BUNY_IPC_FD=3", "BUNY_RUNTIME=" RUN_DIR "/runtime.json",
				 "BUNY_DATA=" DATA_MNT, "BUNY_SHARE=/usr/share/distrobuny", NULL };
		char *argv[] = { UI_BIN, NULL };
		execve(UI_BIN, argv, envp);
		_exit(127);
	}
	close(sv[1]);
	close(lp[1]);
	if (pid < 0) {
		LOGE("fork: %s", strerror(errno));
		close(sv[0]);
		close(lp[0]);
		return;
	}
	UI_PID = pid;
	UI_SOCK = sv[0];
	UI_LOG = lp[0];
	fcntl(UI_LOG, F_SETFL, O_NONBLOCK);
	LOGI("interface started (pid %d)", pid);
	lab_event("ui-started pid=%d", pid);
}

static void forward_ui_log(void)
{
	static char pending[4096];
	static size_t plen;
	char buf[2048];
	ssize_t n;
	while ((n = read(UI_LOG, buf, sizeof(buf))) > 0) {
		for (ssize_t i = 0; i < n; i++) {
			if (buf[i] == '\n' || plen == sizeof(pending) - 1) {
				pending[plen] = '\0';
				if (str_starts(pending, "BUNY-EVENT "))
					serial_line(pending); /* lab events from the interface */
				else if (plen)
					log_msg(LOG_LVL_INFO, "ui: %s", pending);
				plen = 0;
			} else {
				pending[plen++] = buf[i];
			}
		}
	}
}

static void reply(int status, const char *fmt, ...) BUNY_PRINTF(2, 3);
static void reply(int status, const char *fmt, ...)
{
	struct ipc_msg r;
	memset(&r, 0, sizeof(r));
	r.type = IPC_REPLY;
	r.status = status;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(r.text, sizeof(r.text), fmt, ap);
	va_end(ap);
	ipc_send(UI_SOCK, &r, NULL, 0);
}

/* Resolve the ISO path inside the data mount, refusing anything that escapes it. */
static char *resolve_iso(const char *rel)
{
	if (rel[0] != '/' || strstr(rel, "/../") || str_ends(rel, "/..") || !str_iends(rel, ".iso"))
		return NULL;
	char *p = xasprintf(DATA_MNT "%s", rel);
	char *real = realpath(p, NULL);
	free(p);
	if (!real || !str_starts(real, DATA_MNT "/")) {
		free(real);
		return NULL;
	}
	return real;
}

static void do_boot(struct ipc_msg *m, int *fds, int nfds)
{
	if (nfds != 2) {
		reply(EINVAL, "kernel and initrd were not provided");
		goto close_fds;
	}
	char *iso = resolve_iso(m->path);
	if (!iso) {
		reply(EACCES, "the requested image is not on the Distro Buny data partition");
		goto close_fds;
	}
	handoff_payload_close(&LOADED);
	KERNEL_LOADED = false;
	LOADED.kernel_fd = fds[0];
	LOADED.initrd_fd = fds[1];
	LOADED.cmdline = xstrdup(m->text);
	LOADED.needs_preinit = (m->arg & IPC_BOOT_NEEDS_MEDIUM) != 0;
	char err[512];
	char *cmdline = NULL;
	lab_event("boot-request iso=\"%s\" medium=%d", m->path, LOADED.needs_preinit);
	if (LOADED.needs_preinit) {
		struct file_extent *ext = NULL;
		size_t next = 0;
		uint64_t size = 0;
		if (file_extents(DATA.devpath, DATA.fstype, iso, m->path, &ext, &next, &size, err, sizeof(err)) < 0) {
			reply(EIO, "%s", err);
			free(iso);
			return;
		}
		struct handoff_medium med = {
			.part_dev = DATA.devpath, .part_base = 0, .part_start = DATA.start, .part_size = DATA.size,
			.fstype = DATA.fstype, .vol_serial = DATA.vol_serial,
			.have_serial = !strcmp(DATA.fstype, "exfat") || !strcmp(DATA.fstype, "vfat"),
			.ext = ext, .next = next, .iso_size = size, .iso_label = m->label, .iso_file = iso, .verbose = O.lab,
			.force_dm = O.medium_dm,
		};
		int rc = handoff_attach_medium(&LOADED, &med, PREINIT, &cmdline, err, sizeof(err));
		free(ext);
		if (rc < 0) {
			reply(EIO, "%s", err);
			free(iso);
			return;
		}
		LOGI("virtual medium prepared for %s (%zu extent(s))", m->path, next);
	} else {
		cmdline = xstrdup(LOADED.cmdline);
	}
	free(iso);
	bool dry = O.dryrun || (m->arg & IPC_BOOT_DRY_RUN);
	if (handoff_load(&LOADED, cmdline, dry, err, sizeof(err)) < 0) {
		int e = errno ? errno : EPERM;
		lab_event("boot-load-failed errno=%d reason=\"%s\"", e, err);
		reply(e, "%s", err);
		free(cmdline);
		return;
	}
	lab_event("boot-loaded dry_run=%d cmdline=\"%s\"", dry, cmdline);
	free(cmdline);
	KERNEL_LOADED = !dry;
	reply(0, dry ? "validated (dry run)" : "kernel loaded");
	return;
close_fds:
	for (int i = 0; i < nfds; i++)
		close(fds[i]);
}

static void unmount_all(void)
{
	umount2(DATA_MNT, MNT_DETACH);
	umount2(ESP_MNT, MNT_DETACH);
	sync();
}

static void do_execute(void)
{
	if (!KERNEL_LOADED) {
		reply(ENOENT, "no kernel has been loaded");
		return;
	}
	lab_event("handoff");
	LOGI("handing off to the loaded kernel");
	unmount_all();
	char err[256];
	handoff_execute(err, sizeof(err));
	reply(EIO, "%s", err);
}

static void do_power(uint32_t action)
{
	lab_event("power action=%u", action);
	if (action == POWER_FIRMWARE) {
		mount("efivarfs", "/sys/firmware/efi/efivars", NULL, MS_REMOUNT | MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
		if (fw_request_setup("/sys/firmware/efi/efivars") < 0) {
			reply(errno, "this firmware does not accept a request to open its setup (%s)", strerror(errno));
			return;
		}
	}
	unmount_all();
	reboot(action == POWER_OFF ? RB_POWER_OFF : RB_AUTOBOOT);
	reply(errno, "power action failed: %s", strerror(errno));
}

static void do_save(struct ipc_msg *m)
{
	static const char *allowed[] = { "settings.json", "last-boot.json", NULL };
	bool ok = false;
	for (int i = 0; allowed[i]; i++)
		if (!strcmp(m->path, allowed[i]))
			ok = true;
	if (!ok || !HAVE_ESP) {
		reply(EACCES, "state file not allowed");
		return;
	}
	if (mount(NULL, ESP_MNT, NULL, MS_REMOUNT | MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) < 0) {
		reply(errno, "cannot write to the boot partition: %s", strerror(errno));
		return;
	}
	mkdir(ESP_MNT "/distrobuny", 0755);
	mkdir(ESP_MNT "/distrobuny/state", 0755);
	char *p = xasprintf(ESP_MNT "/distrobuny/state/%s", m->path);
	int rc = write_file_atomic(p, m->text, strnlen(m->text, sizeof(m->text)));
	int e = errno;
	free(p);
	sync();
	mount(NULL, ESP_MNT, NULL, MS_REMOUNT | MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
	if (rc < 0)
		reply(e, "saving failed: %s", strerror(e));
	else
		reply(0, "saved");
}

/* The menu asked to grow the data partition (after the user confirmed). */
static void do_expand(void)
{
	if (!HAVE_DATA) {
		reply(ENOENT, "the data partition was not found");
		return;
	}
	char *dev = disk_path(&DATA);
	int fd = open(dev, O_RDWR | O_CLOEXEC);
	free(dev);
	if (fd < 0) {
		reply(errno, "cannot open the drive for writing: %s", strerror(errno));
		return;
	}
	struct expand_plan p;
	char err[512];
	if (plan_expansion(fd, &p, err, sizeof(err)) < 0) {
		reply(EIO, "%s", err);
		close(fd);
		return;
	}
	remember_plan(&p);
	if (p.state != EXPAND_POSSIBLE && p.state != EXPAND_RESUME) {
		reply(p.state == EXPAND_REFUSED ? EPERM : EALREADY, "%s",
		      p.state == EXPAND_REFUSED ? p.reason : "DISTROBUNY already uses the whole drive.");
		expand_plan_free(&p);
		close(fd);
		return;
	}
	/* nothing may use the old file system while it grows */
	handoff_payload_close(&LOADED);
	KERNEL_LOADED = false;
	int rc = -1;
	for (int i = 0; i < 20 && (rc = umount2(DATA_MNT, 0)) < 0 && errno == EBUSY; i++)
		usleep(100 * 1000);
	if (rc < 0 && errno != EINVAL) {
		reply(EBUSY, "The drive is busy. Try again in a moment.");
		expand_plan_free(&p);
		close(fd);
		return;
	}
	rc = run_expansion(fd, &p, true, err, sizeof(err));
	expand_plan_free(&p);
	close(fd);
	if (mount_fs(&DATA, DATA_MNT, true) < 0)
		HAVE_DATA = false;
	write_runtime_info(uptime_ms());
	if (rc < 0)
		reply(EIO, "%s", err);
	else
		reply(0, "%llu", (unsigned long long)(DATA.size * 512));
}

static void handle_ipc(void)
{
	struct ipc_msg m;
	int fds[BUNY_IPC_MAX_FDS];
	int nfds = 0;
	if (ipc_recv(UI_SOCK, &m, fds, &nfds) < 0) {
		close(UI_SOCK);
		UI_SOCK = -1;
		return;
	}
	switch (m.type) {
	case IPC_BOOT:
		do_boot(&m, fds, nfds);
		return;
	case IPC_EXECUTE:
		do_execute();
		break;
	case IPC_POWER:
		do_power(m.arg);
		break;
	case IPC_SAVE:
		do_save(&m);
		break;
	case IPC_EXPAND:
		do_expand();
		break;
	case IPC_RESCAN:
		if (!HAVE_DATA && find_boot_drive()) {
			if (mount_fs(&DATA, DATA_MNT, true) < 0)
				HAVE_DATA = false;
			if (HAVE_ESP && mount_fs(&ESP, ESP_MNT, true) < 0)
				HAVE_ESP = false;
			write_runtime_info(uptime_ms());
		}
		reply(HAVE_DATA ? 0 : ENOENT, HAVE_DATA ? "data partition available" : "data partition not found");
		break;
	default:
		reply(EINVAL, "unknown request");
	}
	for (int i = 0; i < nfds; i++)
		close(fds[i]);
}

static void emergency_shell(const char *why)
{
	console_graphics(false);
	LOGE("starting emergency shell: %s", why);
	fprintf(stderr, "\nDistro Buny: %s\nStarting a shell. Type 'reboot -f' to restart.\n\n", why);
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		int fd = open(O.lab ? "/dev/ttyS0" : "/dev/tty1", O_RDWR);
		if (fd >= 0) {
			ioctl(fd, TIOCSCTTY, 1);
			dup2(fd, 0);
			dup2(fd, 1);
			dup2(fd, 2);
		}
		char *argv[] = { "/bin/sh", NULL };
		char *envp[] = { "PATH=/bin:/sbin:/usr/bin:/usr/sbin", "HOME=/", "TERM=linux", NULL };
		execve("/bin/sh", argv, envp);
		_exit(127);
	}
}

int main(void)
{
	if (getpid() != 1) {
		fprintf(stderr, "buny-init must run as PID 1 of the Distro Buny runtime\n");
		return 1;
	}
	umask(022);
	mount_api();
	int con = open("/dev/console", O_RDWR | O_NOCTTY);
	if (con >= 0) {
		dup2(con, 0);
		dup2(con, 1);
		dup2(con, 2);
		if (con > 2)
			close(con);
	}
	parse_cmdline();
	setup_logging();
	LOGI("Distro Buny runtime %s starting", BUNY_VERSION);
	lab_event("init version=%s uptime_ms=%llu", BUNY_VERSION, (unsigned long long)uptime_ms());
	if (sethostname("distrobuny", 10) < 0)
		LOGW("sethostname: %s", strerror(errno));

	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGCHLD);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGINT);
	sigprocmask(SIG_BLOCK, &mask, NULL);
	int sfd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);

	int uev = open_uevent_socket();
	MODS = modload_open(NULL);
	uint64_t t0 = now_ms();
	int n = modload_coldplug(MODS, "/sys");
	parse_fw_cfg();
	LOGI("cold-plug loaded %d driver module(s) in %llu ms", n, (unsigned long long)(now_ms() - t0));
	lab_event("coldplug modules=%d ms=%llu", n, (unsigned long long)(now_ms() - t0));

	/* Wait for the boot drive; USB storage may take a few seconds to appear. */
	uint64_t deadline = now_ms() + FIND_TIMEOUT_MS;
	while (!find_boot_drive() && now_ms() < deadline) {
		struct pollfd pf = { uev, POLLIN, 0 };
		poll(&pf, 1, 250);
		if (uev >= 0)
			handle_uevents(uev);
	}
	if (HAVE_DATA) {
		check_expansion();
		lab_event("data-partition device=%s fs=%s uuid=%s size=%llu", DATA.devpath, DATA.fstype, DATA.uuid,
			  (unsigned long long)(DATA.size * 512));
		if (mount_fs(&DATA, DATA_MNT, true) < 0)
			HAVE_DATA = false;
	} else {
		LOGE("the Distro Buny data partition was not found");
		lab_event("data-partition missing");
	}
	if (HAVE_ESP && mount_fs(&ESP, ESP_MNT, true) < 0)
		HAVE_ESP = false;
	write_runtime_info(uptime_ms());

	if (O.shell) {
		emergency_shell("debug shell requested (buny.shell=1)");
	} else {
		spawn_ui();
	}

	for (;;) {
		struct pollfd pf[4];
		int np = 0;
		pf[np++] = (struct pollfd){ sfd, POLLIN, 0 };
		if (uev >= 0)
			pf[np++] = (struct pollfd){ uev, POLLIN, 0 };
		if (UI_SOCK >= 0)
			pf[np++] = (struct pollfd){ UI_SOCK, POLLIN, 0 };
		if (UI_LOG >= 0)
			pf[np++] = (struct pollfd){ UI_LOG, POLLIN, 0 };
		if (poll(pf, (nfds_t)np, -1) < 0 && errno != EINTR)
			continue;
		for (int i = 0; i < np; i++) {
			if (!pf[i].revents)
				continue;
			if (pf[i].fd == uev)
				handle_uevents(uev);
			else if (pf[i].fd == UI_SOCK)
				handle_ipc();
			else if (pf[i].fd == UI_LOG) {
				forward_ui_log();
				if (pf[i].revents & POLLHUP) {
					close(UI_LOG);
					UI_LOG = -1;
				}
			} else if (pf[i].fd == sfd) {
				struct signalfd_siginfo si;
				while (read(sfd, &si, sizeof(si)) == (ssize_t)sizeof(si)) {
				}
				int st;
				pid_t p;
				while ((p = waitpid(-1, &st, WNOHANG)) > 0) {
					if (p != UI_PID)
						continue;
					UI_PID = -1;
					if (UI_SOCK >= 0) {
						close(UI_SOCK);
						UI_SOCK = -1;
					}
					LOGW("interface exited (status %d)", st);
					lab_event("ui-exited status=%d", st);
					if (++UI_RESTARTS <= 3) {
						spawn_ui();
					} else {
						emergency_shell("the interface failed repeatedly");
					}
				}
			}
		}
	}
}
