/*
 * Distro Buny — distribution family boot adapters
 *
 * Each adapter documents (in `mechanism`) how its family locates the boot
 * medium, detects the family from the image's own boot entries, and adapts
 * a cloned entry for the virtual-medium or native ISO-file strategy.
 * References for each mechanism are listed in documentation/COMPATIBILITY.md.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "boot-adapters/adapters.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/strbuf.h"

/* ------------------------------------------------------------- helpers */

static bool any_entry_arg(const struct iso_probe *p, const char *key)
{
	const struct boot_entries *lists[] = { &p->grub, &p->sdboot, &p->syslinux, &p->loopback };
	for (size_t li = 0; li < ARRAY_SIZE(lists); li++)
		for (size_t i = 0; i < lists[li]->n; i++)
			if (entry_has_arg(lists[li]->v[i], key))
				return true;
	return false;
}

static bool any_arg_prefix(const struct iso_probe *p, const char *prefix)
{
	const struct boot_entries *lists[] = { &p->grub, &p->sdboot, &p->syslinux, &p->loopback };
	size_t pl = strlen(prefix);
	for (size_t li = 0; li < ARRAY_SIZE(lists); li++)
		for (size_t i = 0; i < lists[li]->n; i++)
			for (size_t k = 0; k < lists[li]->v[i]->args.n; k++)
				if (!strncmp(lists[li]->v[i]->args.v[k], prefix, pl))
					return true;
	return false;
}

static bool any_kernel_under(const struct iso_probe *p, const char *dir_prefix)
{
	const struct boot_entries *lists[] = { &p->grub, &p->sdboot, &p->syslinux, &p->loopback };
	size_t pl = strlen(dir_prefix);
	for (size_t li = 0; li < ARRAY_SIZE(lists); li++)
		for (size_t i = 0; i < lists[li]->n; i++) {
			const char *k = lists[li]->v[i]->kernel;
			if (k && !strncasecmp(k, dir_prefix, pl))
				return true;
		}
	return false;
}

static bool path_ok_for_cmdline(const char *path, char **blocker, const char *family)
{
	if (str_has_space(path)) {
		*blocker = xasprintf("the ISO path contains spaces, which the %s initramfs cannot read from the "
				     "kernel command line", family);
		return false;
	}
	return true;
}

static void remove_iso_file_args(struct boot_entry *e)
{
	static const char *keys[] = { "iso-scan/filename", "findiso", "img_dev", "img_loop", "isoboot",
				      "isofrom_device", "isofrom_system", "iso_path", "fromiso", NULL };
	for (int i = 0; keys[i]; i++)
		entry_remove_arg(e, keys[i]);
}

/* =============================================================== casper */
/* Ubuntu and derivatives (Linux Mint, Pop!_OS, Zorin OS, elementary, ...).
 * casper scans block devices for a filesystem containing /casper (or the
 * live-media-path); it deliberately skips loop devices during that scan, but
 * honours live-media=DEVICE.  Native mode uses iso-scan/filename=PATH, which
 * casper reads by word-splitting /proc/cmdline (no spaces possible) and which
 * requires the initramfs to mount the data partition's filesystem. */

static int casper_detect(const struct iso_probe *p)
{
	if (any_entry_arg(p, "boot") && any_arg_prefix(p, "boot=casper"))
		return 90;
	if (any_kernel_under(p, "/casper"))
		return 80;
	return 0;
}

static int casper_virtual(const struct iso_probe *p, struct boot_entry *e, const struct boot_ctx *c, char **blocker)
{
	remove_iso_file_args(e);
	entry_set_arg(e, "live-media", BUNY_VM_LOOP_DEV);
	return 0;
}

static int casper_native(const struct iso_probe *p, struct boot_entry *e, const struct boot_ctx *c, char **blocker)
{
	if (!path_ok_for_cmdline(p->iso_path, blocker, "casper"))
		return -1;
	remove_iso_file_args(e);
	entry_set_arg(e, "iso-scan/filename", p->iso_path);
	return 0;
}

static const struct adapter ad_casper = {
	"casper", "Ubuntu live system (casper)",
	"casper searches disks for its live filesystem; Distro Buny points it at the virtual medium with live-media=",
	casper_detect, casper_virtual, casper_native,
};

/* ====================================================== dracut live (dmsquash) */
/* Fedora live media, Bazzite, Nobara, Void, Gentoo and other dracut-based
 * live images: root=live:CDLABEL=<label> rd.live.image.  dracut waits for
 * /dev/disk/by-label/<label>; udev creates that link for loop devices, so the
 * virtual medium is found like a real USB stick.  Native mode uses dracut's
 * iso-scan/filename module. */

static int dracut_live_detect(const struct iso_probe *p)
{
	if (any_arg_prefix(p, "root=live:"))
		return 85;
	if (any_entry_arg(p, "rd.live.image"))
		return 70;
	return 0;
}

static int dracut_live_virtual(const struct iso_probe *p, struct boot_entry *e, const struct boot_ctx *c, char **blocker)
{
	remove_iso_file_args(e);
	return 0;
}

static int dracut_live_native(const struct iso_probe *p, struct boot_entry *e, const struct boot_ctx *c, char **blocker)
{
	if (!path_ok_for_cmdline(p->iso_path, blocker, "dracut"))
		return -1;
	remove_iso_file_args(e);
	entry_set_arg(e, "iso-scan/filename", p->iso_path);
	return 0;
}

static const struct adapter ad_dracut_live = {
	"dracut-live", "dracut live image (dmsquash-live)",
	"dracut waits for the medium by label (root=live:CDLABEL=...); the virtual medium carries the ISO label",
	dracut_live_detect, dracut_live_virtual, dracut_live_native,
};

/* ============================================================ Anaconda */
/* Fedora installers (Server DVD, Everything, atomic desktops, IoT):
 * inst.stage2=hd:LABEL=<label> (and possibly inst.ks=hd:LABEL=<label>:/file).
 * Anaconda's dracut module waits for the labelled device.  Native mode
 * rewrites the stage2 location to hd:UUID=<data>:<path to ISO>. */

static int anaconda_detect(const struct iso_probe *p)
{
	if (any_arg_prefix(p, "inst.stage2=") || any_arg_prefix(p, "inst.repo="))
		return 88;
	return 0;
}

static int anaconda_virtual(const struct iso_probe *p, struct boot_entry *e, const struct boot_ctx *c, char **blocker)
{
	remove_iso_file_args(e);
	return 0;
}

static int anaconda_native(const struct iso_probe *p, struct boot_entry *e, const struct boot_ctx *c, char **blocker)
{
	const char *ks = entry_get_arg(e, "inst.ks");
	if (ks && strstr(ks, "LABEL=")) {
		*blocker = xstrdup("the installer's kickstart file is addressed by the ISO label, which only exists on a medium");
		return -1;
	}
	if (!path_ok_for_cmdline(p->iso_path, blocker, "Anaconda"))
		return -1;
	if (!c->data_fs_uuid) {
		*blocker = xstrdup("the data partition UUID is unknown");
		return -1;
	}
	char *v = xasprintf("hd:UUID=%s:%s", c->data_fs_uuid, p->iso_path);
	entry_set_arg(e, "inst.stage2", v);
	free(v);
	return 0;
}

static const struct adapter ad_anaconda = {
	"anaconda", "Fedora installer (Anaconda)",
	"Anaconda loads its installer image from inst.stage2=hd:LABEL=...; the virtual medium provides that label",
	anaconda_detect, anaconda_virtual, anaconda_native,
};

/* ============================================================ archiso */
/* Arch Linux and archiso derivatives (EndeavourOS, CachyOS, Omarchy, ...).
 * archiso resolves archisosearchuuid=<ISO UUID> or archisolabel=<label>
 * through udev and polls for rootdelay seconds.  Native mode is archiso's
 * documented img_dev=/img_loop= pair (mkinitcpio parses quoted values). */

static int archiso_detect(const struct iso_probe *p)
{
	if (any_arg_prefix(p, "archisobasedir=") || any_arg_prefix(p, "archisolabel=") ||
	    any_arg_prefix(p, "archisosearchuuid="))
		return 90;
	return 0;
}

static int archiso_virtual(const struct iso_probe *p, struct boot_entry *e, const struct boot_ctx *c, char **blocker)
{
	remove_iso_file_args(e);
	if (!entry_has_arg(e, "archisosearchuuid") && !entry_has_arg(e, "archisolabel") &&
	    !entry_has_arg(e, "archisodevice")) {
		/* loopback-derived entry: restore the medium lookup */
		entry_set_arg(e, "archisosearchuuid", p->info.uuid);
	} else if (!entry_has_arg(e, "archisosearchuuid") && !entry_has_arg(e, "archisodevice")) {
		/* Older archiso (BlackArch 2023) waits for /dev/disk/by-label/<label>,
		 * which its udev does not create for the loop device: name the device. */
		entry_set_arg(e, "archisodevice", BUNY_VM_LOOP_DEV);
	}
	if (!entry_has_arg(e, "rootdelay"))
		entry_set_arg(e, "rootdelay", "30");
	return 0;
}

static int archiso_native(const struct iso_probe *p, struct boot_entry *e, const struct boot_ctx *c, char **blocker)
{
	if (!c->data_fs_uuid) {
		*blocker = xstrdup("the data partition UUID is unknown");
		return -1;
	}
	remove_iso_file_args(e);
	entry_remove_arg(e, "archisosearchuuid");
	entry_remove_arg(e, "archisolabel");
	char *dev = xasprintf("UUID=%s", c->data_fs_uuid);
	entry_set_arg(e, "img_dev", dev);
	entry_set_arg(e, "img_loop", p->iso_path);
	free(dev);
	return 0;
}

static const struct adapter ad_archiso = {
	"archiso", "Arch Linux live medium (archiso)",
	"archiso looks up its medium by ISO UUID or label through udev; the virtual medium carries both",
	archiso_detect, archiso_virtual, archiso_native,
};

/* =============================================================== miso */
/* Manjaro (manjaro-tools "miso"): misobasedir= misolabel=<label>; native
 * mode uses img_dev=/dev/disk/by-uuid/<uuid> img_loop=<path>.  Manjaro's own
 * loopback.cfg splits paths with spaces, so the native entry is rebuilt from
 * the main configuration. */

static int miso_detect(const struct iso_probe *p)
{
	if (any_arg_prefix(p, "misobasedir=") || any_arg_prefix(p, "misolabel="))
		return 92;
	return 0;
}

static int miso_virtual(const struct iso_probe *p, struct boot_entry *e, const struct boot_ctx *c, char **blocker)
{
	remove_iso_file_args(e);
	return 0;
}

static int miso_native(const struct iso_probe *p, struct boot_entry *e, const struct boot_ctx *c, char **blocker)
{
	if (!c->data_fs_uuid) {
		*blocker = xstrdup("the data partition UUID is unknown");
		return -1;
	}
	remove_iso_file_args(e);
	char *dev = xasprintf("/dev/disk/by-uuid/%s", c->data_fs_uuid);
	entry_set_arg(e, "img_dev", dev);
	entry_set_arg(e, "img_loop", p->iso_path);
	free(dev);
	return 0;
}

static const struct adapter ad_miso = {
	"miso", "Manjaro live medium (miso)",
	"Manjaro's miso hooks find the medium by label (misolabel=); the virtual medium carries that label",
	miso_detect, miso_virtual, miso_native,
};

/* =============================================================== KIWI */
/* openSUSE images built with KIWI: Agama/Leap 16 installers (live ISO with
 * the root configuration embedded in the initrd) and OEM self-install images
 * (root=install:CDLABEL=...).  Both locate their medium by label; KIWI also
 * implements iso-scan/filename for ISO-file booting. */

static int kiwi_detect(const struct iso_probe *p)
{
	if (any_arg_prefix(p, "root=install:"))
		return 86;
	if (iso_exists(p->iso, "/boot/x86_64/loader/linux") &&
	    (iso_exists(p->iso, "/LiveOS/squashfs.img") || iso_exists(p->iso, "/config.isoclient")) &&
	    !any_arg_prefix(p, "root=live:"))
		return 60;
	return 0;
}

static int kiwi_virtual(const struct iso_probe *p, struct boot_entry *e, const struct boot_ctx *c, char **blocker)
{
	remove_iso_file_args(e);
	return 0;
}

static int kiwi_native(const struct iso_probe *p, struct boot_entry *e, const struct boot_ctx *c, char **blocker)
{
	if (!path_ok_for_cmdline(p->iso_path, blocker, "KIWI"))
		return -1;
	remove_iso_file_args(e);
	entry_set_arg(e, "iso-scan/filename", p->iso_path);
	return 0;
}

static const struct adapter ad_kiwi = {
	"kiwi", "openSUSE KIWI image",
	"KIWI images locate their medium by label; the virtual medium carries that label",
	kiwi_detect, kiwi_virtual, kiwi_native,
};

/* ============================================================= linuxrc */
/* openSUSE installation DVDs (Tumbleweed, MicroOS): linuxrc searches the
 * system's block devices for the installation repository.  The virtual
 * medium is named explicitly with install=hd:/?device=loop200. */

static int linuxrc_detect(const struct iso_probe *p)
{
	if (iso_exists(p->iso, "/boot/x86_64/loader/linux") && !iso_exists(p->iso, "/LiveOS") &&
	    (iso_exists(p->iso, "/media.1") || iso_exists(p->iso, "/.treeinfo") || iso_exists(p->iso, "/content")) &&
	    !any_arg_prefix(p, "root="))
		return 75;
	return 0;
}

static int linuxrc_virtual(const struct iso_probe *p, struct boot_entry *e, const struct boot_ctx *c, char **blocker)
{
	remove_iso_file_args(e);
	entry_set_arg(e, "install", "hd:/?device=loop200");
	return 0;
}

static int linuxrc_native(const struct iso_probe *p, struct boot_entry *e, const struct boot_ctx *c, char **blocker)
{
	*blocker = xstrdup("linuxrc ISO-file installation is not implemented by Distro Buny yet");
	return -1;
}

static const struct adapter ad_linuxrc = {
	"linuxrc", "openSUSE installer (linuxrc)",
	"linuxrc is told to install from the virtual medium with install=hd:/?device=loop200",
	linuxrc_detect, linuxrc_virtual, linuxrc_native,
};

/* ===================================================== Debian installer */
/* debian-installer (netinst/DVD): cdrom-detect mounts what udev knows as a CD
 * drive or disk.  Its initrd ships no loop driver and no iso-scan, but the
 * image's pool carries the driver for its own kernel as a udeb
 * (loop-modules-<release>-di).  The plan hands that package to the pre-init,
 * which unpacks it with the installer's udpkg and loads the driver, and tags
 * the virtual medium as a CD drive, so cdrom-detect finds it like a real one. */

static int debian_installer_detect(const struct iso_probe *p)
{
	if (any_kernel_under(p, "/install.amd") || any_kernel_under(p, "/install/") || iso_exists(p->iso, "/install.amd"))
		return 85;
	return 0;
}

char *debian_installer_loop_udeb(const struct iso_probe *p)
{
	if (!p->id.kernel_version || !*p->id.kernel_version)
		return NULL;
	/* the version string starts with the release ("6.12.107+deb13-amd64 (debian-kernel@...) #1 ...") */
	size_t n = strcspn(p->id.kernel_version, " ");
	char *pat = xasprintf("/pool/main/l/*/loop-modules-%.*s-di_*.udeb", (int)n, p->id.kernel_version);
	struct ptrvec found = { 0 };
	iso_glob(p->iso, pat, &found);
	free(pat);
	char *udeb = found.n ? xstrdup(found.v[0]) : NULL;
	ptrvec_free(&found, free);
	return udeb;
}

static int debian_installer_virtual(const struct iso_probe *p, struct boot_entry *e, const struct boot_ctx *c,
				    char **blocker)
{
	char *udeb = debian_installer_loop_udeb(p);
	if (!udeb) {
		*blocker = xstrdup("the Debian installer's initrd has no loop driver, and the image carries no loop-modules "
				   "package for its kernel");
		return -1;
	}
	free(udeb);
	if (c->serial_console) {
		/* lab (serial console only): the text installer, since the graphical one
		 * needs a framebuffer console; and the language and keyboard answered,
		 * which come before the medium search */
		for (size_t i = 0; i < e->initrds.n; i++) {
			char *v = e->initrds.v[i];
			if (str_ends(v, "/gtk/initrd.gz")) {
				char *text = xasprintf("%.*sinitrd.gz", (int)(strlen(v) - strlen("gtk/initrd.gz")), v);
				if (iso_exists(p->iso, text)) {
					free(v);
					e->initrds.v[i] = text;
				} else {
					free(text);
				}
			}
		}
		entry_set_arg(e, "locale", "en_US.UTF-8");
		entry_set_arg(e, "keymap", "us");
	}
	return 0;
}

static int debian_installer_native(const struct iso_probe *p, struct boot_entry *e, const struct boot_ctx *c,
				   char **blocker)
{
	*blocker = xstrdup("the Debian installer CD initrd does not include iso-scan or a driver for the data partition");
	return -1;
}

static const struct adapter ad_debian_installer = {
	ADAPTER_DEBIAN_INSTALLER, "Debian installer (d-i)",
	"debian-installer finds the virtual medium as a CD drive, with the loop driver from the image's own pool",
	debian_installer_detect, debian_installer_virtual, debian_installer_native,
};

/* ========================================================= Debian live */
/* live-boot (Debian live, Kali, Tails, ...): boot=live; findiso=PATH for ISO
 * files, live-media=DEVICE to name the medium explicitly. */

static int debian_live_detect(const struct iso_probe *p)
{
	if (any_arg_prefix(p, "boot=live"))
		return 88;
	return 0;
}

static int debian_live_virtual(const struct iso_probe *p, struct boot_entry *e, const struct boot_ctx *c, char **blocker)
{
	remove_iso_file_args(e);
	entry_set_arg(e, "live-media", BUNY_VM_LOOP_DEV);
	return 0;
}

static int debian_live_native(const struct iso_probe *p, struct boot_entry *e, const struct boot_ctx *c, char **blocker)
{
	if (!path_ok_for_cmdline(p->iso_path, blocker, "live-boot"))
		return -1;
	remove_iso_file_args(e);
	entry_set_arg(e, "findiso", p->iso_path);
	return 0;
}

static const struct adapter ad_debian_live = {
	"debian-live", "Debian live system (live-boot)",
	"live-boot is pointed at the virtual medium with live-media=",
	debian_live_detect, debian_live_virtual, debian_live_native,
};

/* =============================================================== NixOS */
/* NixOS installer: stage 1 mounts the medium listed in its fstab by label
 * (root=fstab); findiso=PATH is NixOS's documented ISO-file option. */

static int nixos_detect(const struct iso_probe *p)
{
	if (any_arg_prefix(p, "init=/nix/store/") || iso_exists(p->iso, "/nix-store.squashfs"))
		return 90;
	return 0;
}

static int nixos_virtual(const struct iso_probe *p, struct boot_entry *e, const struct boot_ctx *c, char **blocker)
{
	remove_iso_file_args(e);
	return 0;
}

static int nixos_native(const struct iso_probe *p, struct boot_entry *e, const struct boot_ctx *c, char **blocker)
{
	if (!path_ok_for_cmdline(p->iso_path, blocker, "NixOS stage-1"))
		return -1;
	remove_iso_file_args(e);
	entry_set_arg(e, "findiso", p->iso_path);
	return 0;
}

static const struct adapter ad_nixos = {
	"nixos", "NixOS installer",
	"NixOS stage 1 mounts its medium by label; the virtual medium carries that label",
	nixos_detect, nixos_virtual, nixos_native,
};

/* ================================================================ Guix */
/* GNU Guix System installer: the Guile initrd scans /proc/partitions for the
 * root file system UUID given by root=; loop devices are listed there too. */

static int guix_detect(const struct iso_probe *p)
{
	if (any_arg_prefix(p, "gnu.system=") || any_kernel_under(p, "/gnu/store/"))
		return 90;
	return 0;
}

static int guix_virtual(const struct iso_probe *p, struct boot_entry *e, const struct boot_ctx *c, char **blocker)
{
	remove_iso_file_args(e);
	return 0;
}

static int guix_native(const struct iso_probe *p, struct boot_entry *e, const struct boot_ctx *c, char **blocker)
{
	*blocker = xstrdup("the Guix initrd has no ISO-file boot option");
	return -1;
}

static const struct adapter ad_guix = {
	"guix", "GNU Guix System installer",
	"Guix's initrd finds the medium by file-system UUID; the virtual medium carries the ISO UUID",
	guix_detect, guix_virtual, guix_native,
};

/* ============================================================== generic */
/* Unknown families whose loopback.cfg already references ${iso_path}: use the
 * distributor's own ISO-file recipe (native only). */

static int generic_detect(const struct iso_probe *p)
{
	if (p->loopback_native)
		return 10;
	return 0;
}

static int generic_virtual(const struct iso_probe *p, struct boot_entry *e, const struct boot_ctx *c, char **blocker)
{
	*blocker = xstrdup("unknown distribution family: the medium lookup method is not known");
	return -1;
}

static int generic_native(const struct iso_probe *p, struct boot_entry *e, const struct boot_ctx *c, char **blocker)
{
	if (!path_ok_for_cmdline(p->iso_path, blocker, "distribution's"))
		return -1;
	return 0;
}

static const struct adapter ad_generic = {
	"generic-loopback", "Generic loopback.cfg image",
	"uses the image's own loopback.cfg recipe for booting from an ISO file",
	generic_detect, generic_virtual, generic_native,
};

/* ================================================================ antiX */
/* antiX and MX Linux (antiX "live-init").  Its device scan skips every
 * /dev/loop* device, but bdev=<device> names the boot device directly.  A
 * from= filter would hide the loop device again, and fromiso= is the ISO-file
 * mode, so both are removed. */

static int antix_detect(const struct iso_probe *p)
{
	if (any_kernel_under(p, "/antiX/") && iso_exists(p->iso, "/antiX/linuxfs"))
		return 88;
	return 0;
}

static int antix_virtual(const struct iso_probe *p, struct boot_entry *e, const struct boot_ctx *c, char **blocker)
{
	remove_iso_file_args(e);
	entry_remove_arg(e, "from");
	entry_remove_arg(e, "fromiso");
	entry_remove_arg(e, "buuid");
	entry_remove_arg(e, "blabel");
	entry_set_arg(e, "bdev", BUNY_VM_LOOP_DEV);
	return 0;
}

static int antix_native(const struct iso_probe *p, struct boot_entry *e, const struct boot_ctx *c, char **blocker)
{
	*blocker = xstrdup("antiX's ISO-file mode (fromiso=) needs a file system its initrd can read; use the virtual medium");
	return -1;
}

static const struct adapter ad_antix = {
	"antix", "antiX / MX Linux live system (live-init)",
	"live-init is pointed at the virtual medium with bdev=/dev/loop200 (its own device scan skips loop devices)",
	antix_detect, antix_virtual, antix_native,
};

/* ============================================================ Slackware */
/* The Slackware installer runs from its initrd and only needs the DVD for the
 * packages.  setup's automatic search tries real CD drives only (its USB
 * search skips loop devices), but "manual" -> "custom" takes any device:
 * the menu tells the user to type the virtual medium's name there. */

static int slackware_detect(const struct iso_probe *p)
{
	if (any_kernel_under(p, "/kernels/") && (iso_is_dir(p->iso, "/slackware64") || iso_is_dir(p->iso, "/slackware")))
		return 80;
	return 0;
}

static int slackware_virtual(const struct iso_probe *p, struct boot_entry *e, const struct boot_ctx *c, char **blocker)
{
	remove_iso_file_args(e);
	return 0;
}

static int slackware_native(const struct iso_probe *p, struct boot_entry *e, const struct boot_ctx *c, char **blocker)
{
	*blocker = xstrdup("the Slackware installer has no ISO-file mode");
	return -1;
}

static const struct adapter ad_slackware = {
	ADAPTER_SLACKWARE, "Slackware installer",
	"setup installs from the virtual medium when given its name (CD/DVD, manual, custom: /dev/loop200)",
	slackware_detect, slackware_virtual, slackware_native,
};

/* ========================================================== best effort */
/* A 64-bit Linux image of a family none of the adapters above knows (antiX,
 * Alpine, Puppy, Slax, ...): start its own boot entry unchanged with the
 * virtual medium attached.  The medium carries the image's volume label and
 * UUID, which is how most initramfs implementations find their files; one
 * that only looks at CD drives stops at its own prompt.  Lowest score: any
 * adapter that knows the family wins, and the menu asks before starting. */

static int best_effort_detect(const struct iso_probe *p)
{
	if (!p->id.arch || strcmp(p->id.arch, "x86_64") != 0)
		return 0;
	const struct boot_entries *lists[] = { &p->grub, &p->sdboot, &p->syslinux, &p->loopback };
	for (size_t li = 0; li < ARRAY_SIZE(lists); li++)
		for (size_t i = 0; i < lists[li]->n; i++)
			if (lists[li]->v[i]->kernel && lists[li]->v[i]->initrds.n)
				return 1;
	return 0;
}

static int best_effort_virtual(const struct iso_probe *p, struct boot_entry *e, const struct boot_ctx *c, char **blocker)
{
	remove_iso_file_args(e);
	return 0;
}

static int best_effort_native(const struct iso_probe *p, struct boot_entry *e, const struct boot_ctx *c, char **blocker)
{
	*blocker = xstrdup("this family's own ISO-file support is not known");
	return -1;
}

static const struct adapter ad_best_effort = {
	ADAPTER_BEST_EFFORT, "Unknown Linux family (best effort)",
	"the virtual medium carries the image's own label and UUID, which most systems use to find their files",
	best_effort_detect, best_effort_virtual, best_effort_native,
};

/* ---------------------------------------------------------------- table */

static const struct adapter *const all_adapters[] = {
	&ad_miso,     &ad_archiso, &ad_casper,  &ad_debian_live, &ad_anaconda, &ad_dracut_live, &ad_kiwi,
	&ad_linuxrc,  &ad_nixos,   &ad_guix,    &ad_antix,   &ad_slackware, &ad_debian_installer, &ad_generic, &ad_best_effort,
};

const struct adapter *const *adapters_all(size_t *n)
{
	*n = ARRAY_SIZE(all_adapters);
	return all_adapters;
}

const struct adapter *adapter_detect(const struct iso_probe *p, int *score_out)
{
	const struct adapter *best = NULL;
	int best_score = 0;
	for (size_t i = 0; i < ARRAY_SIZE(all_adapters); i++) {
		int s = all_adapters[i]->detect(p);
		if (s > best_score) {
			best_score = s;
			best = all_adapters[i];
		}
	}
	if (score_out)
		*score_out = best_score;
	return best;
}

const struct adapter *adapter_by_id(const char *id)
{
	for (size_t i = 0; i < ARRAY_SIZE(all_adapters); i++)
		if (!strcmp(all_adapters[i]->id, id))
			return all_adapters[i];
	return NULL;
}
