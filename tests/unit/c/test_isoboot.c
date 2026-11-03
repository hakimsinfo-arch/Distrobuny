/*
 * Unit tests: ISO 9660 reader, boot configuration parsers, GRUB interpreter,
 * identification, adapters and boot planning.  ISO fixtures are generated at
 * test time with genisoimage (Rock Ridge + Joliet).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "test.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "boot-adapters/adapters.h"
#include "boot-adapters/bootcfg.h"
#include "common/strbuf.h"
#include "filesystem/iso9660.h"
#include "iso-manager/identify.h"
#include "iso-manager/rules.h"

static char g_tmp[256];

static void write_file(const char *rel, const void *data, size_t len)
{
	char *p = xasprintf("%s/%s", g_tmp, rel);
	char *dir = path_dirname(p);
	char *cmd = xasprintf("mkdir -p '%s'", dir);
	if (system(cmd) != 0)
		abort();
	free(cmd);
	free(dir);
	int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0 || write(fd, data, len) != (ssize_t)len)
		abort();
	close(fd);
	free(p);
}

static void write_text(const char *rel, const char *text)
{
	write_file(rel, text, strlen(text));
}

/* A file with a valid x86 boot protocol header, large enough to pass validation. */
static void write_kernel(const char *rel, const char *version)
{
	size_t n = 600 * 1024;
	unsigned char *k = calloc(1, n);
	memcpy(k + 0x202, "HdrS", 4);
	k[0x206] = 0x0f;
	k[0x207] = 0x02; /* protocol 2.15 */
	k[0x236] = 0x01; /* XLF_KERNEL_64 */
	k[0x20e] = 0x00;
	k[0x20f] = 0x02; /* version string at 0x200 + 0x200 */
	strcpy((char *)k + 0x400, version);
	write_file(rel, k, n);
	free(k);
}

static char *make_iso(const char *name, const char *label, const char *tree)
{
	char *iso = xasprintf("%s/%s.iso", g_tmp, name);
	char *cmd = xasprintf("genisoimage -quiet -R -J -joliet-long -V '%s' -o '%s' '%s/%s' 2>/dev/null", label, iso, g_tmp, tree);
	int rc = system(cmd);
	free(cmd);
	if (rc != 0) {
		fprintf(stderr, "genisoimage failed for %s\n", name);
		exit(2);
	}
	return iso;
}

static struct ruleset g_rules;

static void build_fixtures(void)
{
	/* casper-style (Ubuntu) image with a loopback.cfg */
	write_kernel("casper/casper/vmlinuz", "6.9.0-test-generic (buildd@lcy) #1 SMP");
	write_text("casper/casper/initrd", "initrd-placeholder");
	write_text("casper/.disk/info", "Ubuntu 26.04.1 LTS \"Test\" - Release amd64 (20260101)\n");
	write_text("casper/boot/grub/grub.cfg",
		   "set timeout=30\nmenuentry \"Try or Install Ubuntu\" {\n linux /casper/vmlinuz --- quiet splash\n initrd /casper/initrd\n}\n"
		   "menuentry \"Ubuntu (safe graphics)\" {\n linux /casper/vmlinuz nomodeset --- quiet splash\n initrd /casper/initrd\n}\n");
	write_text("casper/boot/grub/loopback.cfg",
		   "menuentry \"Try or Install Ubuntu\" {\n\tlinux\t/casper/vmlinuz iso-scan/filename=${iso_path} --- quiet splash\n\tinitrd\t/casper/initrd\n}\n");
	write_text("casper/Mixed Case Dir/File With Spaces.txt", "hello");
	char *lnk = xasprintf("%s/casper/casper/vmlinuz-link", g_tmp);
	if (symlink("vmlinuz", lnk) != 0)
		perror("symlink");
	free(lnk);

	/* archiso-style image: systemd-boot entries + SYSLINUX with ::/ paths */
	write_kernel("arch/arch/boot/x86_64/vmlinuz-linux", "6.10.1-arch1-1 (linux@archlinux) #1");
	write_text("arch/arch/boot/x86_64/initramfs-linux.img", "x");
	write_text("arch/loader/entries/01-archiso-linux.conf",
		   "title    Arch Linux install medium (x86_64, UEFI)\nlinux    /arch/boot/x86_64/vmlinuz-linux\n"
		   "initrd   /arch/boot/x86_64/initramfs-linux.img\noptions  archisobasedir=arch archisosearchuuid=2026-01-01-00-00-00-00\n");
	write_text("arch/boot/syslinux/syslinux.cfg",
		   "DEFAULT arch\nLABEL arch\nMENU LABEL Arch ^Linux\nLINUX ::/arch/boot/x86_64/vmlinuz-linux\n"
		   "INITRD ::/arch/boot/x86_64/initramfs-linux.img\nAPPEND archisobasedir=arch\n");

	/* dracut live image whose default entry is a media check */
	write_kernel("dracut/images/pxeboot/vmlinuz", "6.11.0-300.fc44.x86_64 (mockbuild@fedora) #1");
	write_text("dracut/images/pxeboot/initrd.img", "x");
	write_text("dracut/boot/grub2/grub.cfg",
		   "set default=\"1\"\nfunction load_video {\n insmod all_video\n}\nload_video\n"
		   "search --no-floppy --set=root -l 'Fedora-WS-Live-44'\n"
		   "menuentry 'Start Fedora' --class fedora {\n linux /images/pxeboot/vmlinuz root=live:CDLABEL=Fedora-WS-Live-44 rd.live.image quiet\n initrd /images/pxeboot/initrd.img\n}\n"
		   "menuentry 'Test this media & start Fedora' {\n linux /images/pxeboot/vmlinuz root=live:CDLABEL=Fedora-WS-Live-44 rd.live.image rd.live.check\n initrd /images/pxeboot/initrd.img\n}\n"
		   "submenu 'Troubleshooting -->' {\n menuentry 'Basic graphics' {\n  linux /images/pxeboot/vmlinuz root=live:CDLABEL=Fedora-WS-Live-44 nomodeset\n  initrd /images/pxeboot/initrd.img\n }\n}\n");
}

static void iso_reader(void)
{
	char err[256];
	char *iso = make_iso("casper", "Ubuntu 26.04.1 LTS amd64", "casper");
	struct iso_image *img = iso_open_path(iso, err, sizeof(err));
	CHECK(img != NULL);
	if (!img)
		return;
	const struct iso_info *info = iso_get_info(img);
	CHECK_STR(info->volume_id, "Ubuntu 26.04.1 LTS amd64");
	CHECK(info->rock_ridge);
	CHECK(info->joliet);
	CHECK(iso_exists(img, "/casper/vmlinuz"));
	CHECK(iso_exists(img, "/CASPER/VMLINUZ"));             /* case-insensitive fallback */
	CHECK(iso_exists(img, "/Mixed Case Dir/File With Spaces.txt"));
	CHECK(!iso_exists(img, "/casper/nothing"));
	char *canon = iso_canonical_path(img, "/mixed case dir/file with spaces.txt");
	CHECK_STR(canon, "/Mixed Case Dir/File With Spaces.txt");
	free(canon);
	struct iso_file f, l;
	CHECK(iso_lookup(img, "/casper/vmlinuz", &f) == 0);
	CHECK(iso_lookup(img, "/casper/vmlinuz-link", &l) == 0); /* Rock Ridge symlink resolved */
	CHECK(f.size == l.size && f.ext[0].lba == l.ext[0].lba);
	iso_file_free(&f);
	iso_file_free(&l);
	char *t = iso_read_text(img, "/.disk/info", 4096, NULL);
	CHECK(t && str_starts(t, "Ubuntu 26.04.1"));
	free(t);
	char *arch, *ver;
	CHECK(kernel_header_info(img, "/casper/vmlinuz", &arch, &ver) == 0);
	CHECK_STR(arch, "x86_64");
	CHECK_STR(ver, "6.9.0-test-generic");
	free(arch);
	free(ver);
	struct ptrvec g = { 0 };
	iso_glob(img, "/casper/vm*", &g);
	CHECK(g.n == 2);
	ptrvec_free(&g, free);
	iso_close(img);
	/* garbage is rejected cleanly */
	char *junk = xasprintf("%s/junk.iso", g_tmp);
	char zero[65536] = { 0 };
	int fd = open(junk, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (write(fd, zero, sizeof(zero)) < 0)
		abort();
	close(fd);
	CHECK(iso_open_path(junk, err, sizeof(err)) == NULL);
	free(junk);
	free(iso);
}

static struct iso_image *open_fixture(const char *name)
{
	char err[256];
	char *p = xasprintf("%s/%s.iso", g_tmp, name);
	struct iso_image *img = iso_open_path(p, err, sizeof(err));
	free(p);
	return img;
}

static void grub_interpreter(void)
{
	struct iso_image *img = open_fixture("casper");
	if (!img)
		return;
	struct grub_eval_ctx ctx = { img, "/Desktop/My ISOs/x.iso", "ABCD-1234", "DISTROBUNY", SRC_GRUB_LOOPBACK, 0 };
	struct boot_entries out = { 0 };
	char err[200];
	const char *script =
		"set base=/casper\n"
		"function mk { menuentry \"$1 entry\" \"$2\" { linux ${base}/vmlinuz arg=$2 \"quoted arg\" ; initrd ${base}/initrd } }\n"
		"mk First alpha\n"
		"if [ -f /casper/vmlinuz -a ! -d /casper/vmlinuz ]; then set found=yes; elif true; then set found=no; fi\n"
		"search --no-floppy --set=dev --file \"${iso_path}\"\n"
		"probe --set uuid --fs-uuid \"$dev\"\n"
		"regexp --set=1:ver '([0-9]+)\\.([0-9]+)' 'release 26.04'\n"
		"for k in /casper/vm*; do set last=$k; done\n"
		"menuentry \"$found $uuid $ver $last\" { linux /casper/vmlinuz img_loop=\"${iso_path}\" }\n"
		"submenu Advanced { menuentry Inner { linux /casper/vmlinuz inner=1 } }\n";
	CHECK(grubcfg_eval_text(&ctx, script, "/test.cfg", &out, err, sizeof(err)) == 0);
	CHECK(out.n == 3);
	if (out.n == 3) {
		CHECK_STR(out.v[0]->title, "First entry");
		CHECK_STR(entry_get_arg(out.v[0], "arg"), "alpha");
		CHECK_STR(out.v[0]->args.v[1], "quoted arg");
		CHECK_STR(out.v[1]->title, "yes ABCD-1234 26 /casper/vmlinuz-link");
		CHECK_STR(entry_get_arg(out.v[1], "img_loop"), "/Desktop/My ISOs/x.iso");
		CHECK(str_ends(out.v[2]->title, "Inner"));
	}
	boot_entries_free(&out);
	/* hostile input: an endless loop is stopped by the step limit */
	struct grub_eval_ctx lim = ctx;
	lim.max_steps = 5000;
	CHECK(grubcfg_eval_text(&lim, "while true; do set x=1; done\nmenuentry never { linux /casper/vmlinuz }", "/loop.cfg", &out, err,
				sizeof(err)) < 0);
	boot_entries_free(&out);
	/* deep recursion through self-sourcing configs terminates */
	CHECK(grubcfg_eval_text(&ctx, "function f { f }\nf\n", "/rec.cfg", &out, err, sizeof(err)) == 0);
	boot_entries_free(&out);
	iso_close(img);
}

static void other_parsers(void)
{
	char *iso = make_iso("arch", "ARCH_202601", "arch");
	free(iso);
	struct iso_image *img = open_fixture("arch");
	if (!img)
		return;
	struct boot_entries sd = { 0 }, sl = { 0 };
	CHECK(sdboot_parse(img, &sd) == 1);
	CHECK_STR(sd.v[0]->kernel, "/arch/boot/x86_64/vmlinuz-linux");
	CHECK_STR(entry_get_arg(sd.v[0], "archisosearchuuid"), "2026-01-01-00-00-00-00");
	CHECK(syslinux_parse(img, &sl) == 1);
	CHECK_STR(sl.v[0]->title, "Arch Linux");
	CHECK(sl.v[0]->is_default);
	boot_entries_free(&sd);
	boot_entries_free(&sl);
	iso_close(img);
}

static void planning(void)
{
	char err[256];
	char *d = make_iso("dracut", "Fedora-WS-Live-44", "dracut");
	struct probe_env env = { &g_rules, "ABCD-1234", "DISTROBUNY", "exfat" };
	struct iso_probe p;
	CHECK(iso_probe_open(&p, d, "/Desktop/Fedora/Fedora-Workstation-Live-44-1.7.x86_64.iso", &env) == 0);
	CHECK(p.id.recognized);
	CHECK_STR(p.id.name, "Fedora Workstation");
	CHECK_STR(p.id.version, "44");
	struct boot_ctx ctx = { .data_fs_uuid = "ABCD-1234", .data_fs_type = "exfat", .variant = -1 };
	struct boot_plan plan;
	CHECK(plan_build(&p, &ctx, &plan) == 0);
	CHECK_STR(plan.adapter->id, "dracut-live");
	CHECK(plan.strategy == MEDIA_VIRTUAL);
	/* the default entry is a media check: the planner must prefer the plain start entry */
	CHECK(!entry_has_arg(plan.entry, "rd.live.check"));
	CHECK(plan.variant_titles.n == 3);
	plan_free(&plan);
	/* native strategy rewrites to iso-scan/filename */
	ctx.strategy = MEDIA_NATIVE;
	CHECK(plan_build(&p, &ctx, &plan) == 0);
	CHECK_STR(entry_get_arg(plan.entry, "iso-scan/filename"), "/Desktop/Fedora/Fedora-Workstation-Live-44-1.7.x86_64.iso");
	plan_free(&plan);
	iso_probe_close(&p);
	free(d);

	/* casper: virtual medium named explicitly; native blocked by the space in the path */
	char *c = xasprintf("%s/casper.iso", g_tmp);
	CHECK(iso_probe_open(&p, c, "/Desktop/My ISOs/ubuntu.iso", &env) == 0);
	CHECK_STR(p.id.rule_id, "ubuntu");
	CHECK_STR(p.id.version, "26.04.1 LTS");
	CHECK(p.loopback_native);
	ctx.strategy = MEDIA_AUTO;
	CHECK(plan_build(&p, &ctx, &plan) == 0);
	CHECK_STR(plan.adapter->id, "casper");
	CHECK_STR(entry_get_arg(plan.entry, "live-media"), BUNY_VM_LOOP_DEV);
	CHECK(!entry_has_arg(plan.entry, "iso-scan/filename"));
	CHECK(!plan.native_possible && plan.native_blocker && strstr(plan.native_blocker, "spaces"));
	plan_free(&plan);
	iso_probe_close(&p);
	free(c);

	/* unreadable input yields an honest UNKNOWN plan */
	char *junk = xasprintf("%s/junk.iso", g_tmp);
	iso_probe_open(&p, junk, "/junk.iso", &env);
	CHECK(plan_build(&p, &ctx, &plan) < 0);
	CHECK(plan.level == COMPAT_UNKNOWN);
	plan_free(&plan);
	iso_probe_close(&p);
	free(junk);
	(void)err;
}

/* debian-installer: the plan carries the image's own loop driver package; the
 * lab gets the text installer; an image without the package stays blocked */
static void debian_installer(void)
{
	static const char cfg[] =
		"menuentry 'Graphical install' {\n linux /install.amd/vmlinuz vga=788 --- quiet\n initrd /install.amd/gtk/initrd.gz\n}\n"
		"menuentry 'Install' {\n linux /install.amd/vmlinuz vga=788 --- quiet\n initrd /install.amd/initrd.gz\n}\n";
	static const char udeb[] = "debian/pool/main/l/linux-signed-amd64/loop-modules-6.12.107+deb13-amd64-di_6.12.107-1_amd64.udeb";
	write_kernel("debian/install.amd/vmlinuz", "6.12.107+deb13-amd64 (debian-kernel@lists.debian.org) #1 SMP Debian 6.12.107-1");
	write_text("debian/install.amd/initrd.gz", "text initrd");
	write_text("debian/install.amd/gtk/initrd.gz", "graphical initrd");
	write_text("debian/boot/grub/grub.cfg", cfg);
	write_text("debian/.disk/info", "Debian GNU/Linux 13.7.0 \"Trixie\" - Official amd64 NETINST");
	write_text(udeb, "!<arch>\n");
	char *iso = make_iso("debian", "Debian 13.7.0 amd64 n", "debian");
	struct probe_env env = { &g_rules, "ABCD-1234", "DISTROBUNY", "exfat" };
	struct iso_probe p;
	CHECK(iso_probe_open(&p, iso, "/Desktop/debian-13.7.0-amd64-netinst.iso", &env) == 0);
	struct boot_ctx ctx = { .data_fs_uuid = "ABCD-1234", .data_fs_type = "exfat", .variant = -1 };
	struct boot_plan plan;
	CHECK(plan_build(&p, &ctx, &plan) == 0);
	CHECK_STR(plan.adapter->id, "debian-installer");
	CHECK(plan.level == COMPAT_PREPARED && plan.needs_preinit);
	CHECK_STR(plan.udeb, udeb + strlen("debian"));
	CHECK_STR(plan.entry->initrds.v[0], "/install.amd/gtk/initrd.gz");
	CHECK(!entry_has_arg(plan.entry, "console"));
	plan_free(&plan);
	/* lab: the text installer on the serial console only, language and keyboard answered */
	ctx.serial_console = true;
	CHECK(plan_build(&p, &ctx, &plan) == 0);
	CHECK_STR(plan.entry->initrds.v[0], "/install.amd/initrd.gz");
	CHECK_STR(entry_get_arg(plan.entry, "console"), "ttyS0,115200");
	CHECK_STR(entry_get_arg(plan.entry, "locale"), "en_US.UTF-8");
	plan_free(&plan);
	iso_probe_close(&p);
	free(iso);

	/* without the loop-modules package there is no way to start it */
	char *path = xasprintf("%s/%s", g_tmp, udeb);
	CHECK(unlink(path) == 0);
	free(path);
	iso = make_iso("debian-nopool", "Debian 13.7.0 amd64 n", "debian");
	CHECK(iso_probe_open(&p, iso, "/Desktop/debian.iso", &env) == 0);
	ctx.serial_console = false;
	CHECK(plan_build(&p, &ctx, &plan) < 0);
	CHECK(plan.level < COMPAT_PREPARED);
	plan_free(&plan);
	iso_probe_close(&p);
	free(iso);
}

static void folder_names(void)
{
	static const struct { const char *folder, *display, *icon; } v[] = {
		{ "open suse", "openSUSE", "opensuse" },      /* whole-name match: canonical name */
		{ "Linux mint", "Linux Mint", "linuxmint" },
		{ "servers", "Server", "cat-server" },
		{ "Ubuntu Family", "Ubuntu Family", "ubuntu" }, /* partial match: the user's name stays */
		{ "Arch experiments", "Arch experiments", "archlinux" },
		{ "my isos", "My isos", "folder" },           /* no rule: first letter capitalised */
	};
	for (size_t i = 0; i < ARRAY_SIZE(v); i++) {
		const char *icon, *desc;
		char *disp;
		identify_folder(&g_rules, v[i].folder, &icon, &disp, &desc);
		CHECK_STR(disp, v[i].display);
		CHECK_STR(icon, v[i].icon);
		free(disp);
	}
}

int main(void)
{
	snprintf(g_tmp, sizeof(g_tmp), "/tmp/buny-test-XXXXXX");
	if (!mkdtemp(g_tmp))
		return 2;
	char err[256];
	if (rules_load(&g_rules, "configuration/distributions/distributions.conf", err, sizeof(err)) < 0) {
		fprintf(stderr, "%s (run from the project root)\n", err);
		return 2;
	}
	build_fixtures();
	TEST(iso_reader);
	TEST(grub_interpreter);
	TEST(other_parsers);
	TEST(planning);
	TEST(debian_installer);
	TEST(folder_names);
	rules_free(&g_rules);
	char *cmd = xasprintf("rm -rf '%s'", g_tmp);
	if (system(cmd) != 0)
		perror("cleanup");
	free(cmd);
	return test_summary("isoboot");
}
