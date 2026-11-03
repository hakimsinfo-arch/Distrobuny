/*
 * Distro Buny — boot planning
 *
 * Chooses the boot entry, applies the family adapter for the selected media
 * strategy and validates the result against the image contents.  The plan
 * never claims more than it knows: a plan is PREPARED only when the adapter
 * accepted the strategy and every referenced kernel and initrd exists.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "boot-adapters/adapters.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/log.h"
#include "common/strbuf.h"
#include "common/util.h"

const char *compat_level_name(enum compat_level l)
{
	switch (l) {
	case COMPAT_UNKNOWN: return "unknown";
	case COMPAT_RECOGNIZED: return "recognized";
	case COMPAT_METHOD: return "method-known";
	case COMPAT_PREPARED: return "prepared";
	case COMPAT_HANDOFF: return "handoff";
	case COMPAT_VERIFIED: return "verified";
	}
	return "unknown";
}

const char *media_strategy_name(enum media_strategy s)
{
	switch (s) {
	case MEDIA_VIRTUAL: return "virtual-medium";
	case MEDIA_NATIVE: return "native-iso-file";
	default: return "auto";
	}
}

/* ------------------------------------------------------- entry ranking */

static bool title_has(const struct boot_entry *e, const char *word)
{
	return e->title && str_icontains(e->title, word);
}

static bool args_have(const struct boot_entry *e, const char *needle)
{
	for (size_t i = 0; i < e->args.n; i++)
		if (str_icontains(e->args.v[i], needle))
			return true;
	return false;
}

/* Entries that are not distribution boots at all. */
static bool entry_unusable(const struct boot_entry *e)
{
	if (!e->kernel)
		return true;
	if (str_icontains(e->kernel, "memtest") || str_icontains(e->kernel, "hdt") || str_iends(e->kernel, ".c32") ||
	    str_icontains(e->kernel, "ipxe") || str_icontains(e->kernel, "::"))
		return true;
	if (args_have(e, "archiso_nbd_srv") || args_have(e, "archiso_http_srv") || args_have(e, "archiso_nfs_srv") ||
	    args_have(e, "${pxeserver}") || args_have(e, "pxeboot"))
		return true;
	if (title_has(e, "hard disk") || title_has(e, "firmware") || title_has(e, "boot from") || title_has(e, "memory test"))
		return true;
	return false;
}

static int entry_score(const struct boot_entry *e)
{
	int s = 100;
	if (e->is_default)
		s += 5;
	if (args_have(e, "rd.live.check") || args_have(e, "mediacheck") || title_has(e, "test this media") ||
	    title_has(e, "check") || title_has(e, "verify"))
		s -= 60;
	if (args_have(e, "nomodeset") || title_has(e, "safe graphics") || title_has(e, "basic graphics") ||
	    title_has(e, "compatibility") || title_has(e, "failsafe") || title_has(e, "fallback") ||
	    title_has(e, "legacy hardware"))
		s -= 40;
	if (args_have(e, "speakup") || args_have(e, "accessibility") || title_has(e, "screen reader") ||
	    title_has(e, "braille") || title_has(e, "accessib"))
		s -= 50;
	if (args_have(e, "inst.rescue") || args_have(e, "rescue=1") || title_has(e, "rescue"))
		s -= 70;
	if (args_have(e, "upgrade=1") || title_has(e, "upgrade"))
		s -= 30;
	if (args_have(e, "copytoram") && !args_have(e, "copytoram=auto") && !args_have(e, "copytoram=n"))
		s -= 10;
	if (args_have(e, "rd.live.ram") || args_have(e, "toram") || title_has(e, "(ram)") || title_has(e, "cached"))
		s -= 10;
	if (title_has(e, "nvidia") || title_has(e, "proprietary") || args_have(e, "driver=nonfree"))
		s -= 5;
	if (title_has(e, "debug") || args_have(e, "debug"))
		s -= 20;
	return s;
}

/* Collect usable entries from the preferred source list for a strategy. */
static void candidate_entries(const struct iso_probe *p, enum media_strategy st, struct ptrvec *out)
{
	const struct boot_entries *order_virtual[] = { &p->grub, &p->sdboot, &p->syslinux, &p->loopback };
	const struct boot_entries *order_native[] = { &p->loopback, &p->grub, &p->sdboot, &p->syslinux };
	const struct boot_entries **order = st == MEDIA_NATIVE && p->loopback_native ? order_native : order_virtual;
	for (size_t li = 0; li < 4; li++) {
		const struct boot_entries *l = order[li];
		for (size_t i = 0; i < l->n; i++)
			if (!entry_unusable(l->v[i]))
				ptrvec_push(out, l->v[i]);
		if (out->n)
			return; /* use one consistent source */
	}
}

static int pick_primary(struct ptrvec *cands)
{
	int best = -1, best_score = -1000;
	for (size_t i = 0; i < cands->n; i++) {
		int s = entry_score(cands->v[i]);
		if (s > best_score) {
			best_score = s;
			best = (int)i;
		}
	}
	return best;
}

/* ---------------------------------------------------------- validation */

static char *validate_files(const struct iso_probe *p, const struct boot_entry *e)
{
	if (!e->kernel)
		return xstrdup("the boot entry names no kernel");
	struct iso_file f;
	if (iso_lookup(p->iso, e->kernel, &f) < 0)
		return xasprintf("kernel %s is missing from the image", e->kernel);
	bool is_dir = f.is_dir;
	uint64_t ksize = f.size;
	iso_file_free(&f);
	if (is_dir || ksize < 512 * 1024)
		return xasprintf("kernel %s is not a valid kernel image", e->kernel);
	for (size_t i = 0; i < e->initrds.n; i++) {
		if (iso_lookup(p->iso, e->initrds.v[i], &f) < 0)
			return xasprintf("initrd %s is missing from the image", (char *)e->initrds.v[i]);
		iso_file_free(&f);
	}
	return NULL;
}

/* Replace the placeholder UUID the probe used when the real one was unknown. */
static void substitute_uuid(struct boot_entry *e, const char *uuid)
{
	if (!uuid)
		return;
	for (size_t i = 0; i < e->args.n; i++) {
		char *a = e->args.v[i];
		char *at = strstr(a, "@DATA_UUID@");
		if (!at)
			continue;
		char *n = xasprintf("%.*s%s%s", (int)(at - a), a, uuid, at + strlen("@DATA_UUID@"));
		free(a);
		e->args.v[i] = n;
	}
}

static void add_note(struct boot_plan *plan, const char *fmt, ...) BUNY_PRINTF(2, 3);
static void add_note(struct boot_plan *plan, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	ptrvec_push(&plan->notes, xvasprintf(fmt, ap));
	va_end(ap);
}

/* Try to prepare an entry for a strategy; returns the prepared clone or NULL. */
static struct boot_entry *try_strategy(const struct iso_probe *p, const struct adapter *ad, enum media_strategy st,
				       const struct boot_ctx *c, int variant, struct ptrvec *titles, char **blocker)
{
	struct ptrvec cands = { 0 };
	candidate_entries(p, st, &cands);
	if (!cands.n) {
		*blocker = xstrdup("the image has no usable boot entry");
		free(cands.v);
		return NULL;
	}
	int idx = variant >= 0 && (size_t)variant < cands.n ? variant : pick_primary(&cands);
	if (titles && !titles->n)
		for (size_t i = 0; i < cands.n; i++)
			ptrvec_push(titles, xstrdup(((struct boot_entry *)cands.v[i])->title ? ((struct boot_entry *)cands.v[i])->title : "Boot"));
	struct boot_entry *e = boot_entry_clone(cands.v[idx]);
	free(cands.v);
	substitute_uuid(e, c->data_fs_uuid);
	int rc = st == MEDIA_VIRTUAL ? ad->prepare_virtual(p, e, c, blocker) : ad->prepare_native(p, e, c, blocker);
	if (rc < 0) {
		boot_entry_free(e);
		return NULL;
	}
	return e;
}

/* Does a kickstart partition disks by itself?  A kickstart on the image is
 * read (Fedora IoT's only names its ostree source); one elsewhere (network,
 * initrd) cannot be checked and counts as unattended. */
static bool kickstart_partitions(const struct iso_probe *p, const char *ks)
{
	const char *path = NULL;
	if (str_starts(ks, "cdrom:"))
		path = ks + 6;
	else if (str_starts(ks, "hd:") && strchr(ks + 3, ':'))
		path = strrchr(ks, ':') + 1;
	struct iso_file f;
	if (!path || path[0] != '/' || !p->iso || iso_lookup(p->iso, path, &f) < 0)
		return true;
	size_t len = f.size < 65536 ? (size_t)f.size : 65536;
	char *text = xmalloc(len + 1);
	ssize_t n = iso_pread(p->iso, &f, text, len, 0);
	iso_file_free(&f);
	bool erases = n < 0;
	text[n > 0 ? n : 0] = '\0';
	static const char *cmds[] = { "clearpart", "zerombr", "autopart", "part", "reqpart", NULL };
	for (char *save = NULL, *l = strtok_r(text, "\n", &save); l && !erases; l = strtok_r(NULL, "\n", &save)) {
		l += strspn(l, " \t");
		for (int i = 0; cmds[i] && !erases; i++) {
			size_t k = strlen(cmds[i]);
			erases = !strncmp(l, cmds[i], k) && (l[k] == '\0' || l[k] == ' ' || l[k] == '\t' || l[k] == '\r');
		}
	}
	free(text);
	return erases;
}

/* Unattended installations (Ubuntu autoinstall, kickstart, d-i preseeding) */
/* Is "major.minor..." at most major.minor? */
static bool old_kernel(const char *version, int major, int minor)
{
	int a = 0, b = 0;
	if (!version || sscanf(version, "%d.%d", &a, &b) != 2)
		return false;
	return a < major || (a == major && b <= minor);
}

static bool unattended_install(const struct iso_probe *p, const struct boot_entry *e)
{
	const char *autoi = entry_get_arg(e, "auto");
	const char *crit = entry_get_arg(e, "priority");
	const char *ks = entry_get_arg(e, "inst.ks");
	if (!ks)
		ks = entry_get_arg(e, "ks");
	return entry_has_arg(e, "autoinstall") || (ks && kickstart_partitions(p, ks)) ||
	       entry_has_arg(e, "auto-install/enable") || (autoi && !strcmp(autoi, "true")) ||
	       (crit && !strcmp(crit, "critical") && (entry_has_arg(e, "preseed/file") || entry_has_arg(e, "preseed/url") ||
						       entry_has_arg(e, "url") || entry_has_arg(e, "file")));
}

int plan_build(const struct iso_probe *p, const struct boot_ctx *c, struct boot_plan *plan)
{
	memset(plan, 0, sizeof(*plan));
	plan->variant_index = c->variant;
	if (p->error && !p->iso) {
		plan->level = COMPAT_UNKNOWN;
		plan->reason = xasprintf("the image cannot be read: %s", p->error);
		return -1;
	}
	if (p->error) {
		plan->level = p->id.recognized ? COMPAT_RECOGNIZED : COMPAT_UNKNOWN;
		plan->reason = xstrdup(p->error);
		return -1;
	}
	int score = 0;
	const struct adapter *ad = adapter_detect(p, &score);
	plan->adapter = ad;
	if (!ad) {
		plan->level = p->id.recognized ? COMPAT_RECOGNIZED : COMPAT_UNKNOWN;
		plan->reason = xstrdup("no boot adapter recognises this image's boot configuration");
		return -1;
	}
	plan->level = COMPAT_METHOD;

	/* Evaluate both strategies to report what is possible and why not. */
	char *vb = NULL, *nb = NULL;
	struct boot_entry *ev = try_strategy(p, ad, MEDIA_VIRTUAL, c, c->strategy == MEDIA_VIRTUAL || c->strategy == MEDIA_AUTO ? c->variant : -1,
					      &plan->variant_titles, &vb);
	struct ptrvec native_titles = { 0 };
	struct boot_entry *en = try_strategy(p, ad, MEDIA_NATIVE, c, c->strategy == MEDIA_NATIVE ? c->variant : -1,
					      &native_titles, &nb);
	plan->virtual_possible = ev != NULL;
	plan->native_possible = en != NULL;
	plan->virtual_blocker = vb;
	plan->native_blocker = nb;

	struct boot_entry *chosen = NULL;
	if (c->strategy == MEDIA_NATIVE) {
		chosen = en;
		en = NULL;
		plan->strategy = MEDIA_NATIVE;
		ptrvec_free(&plan->variant_titles, free);
		plan->variant_titles = native_titles;
		native_titles = (struct ptrvec){ 0 };
	} else if (c->strategy == MEDIA_VIRTUAL || ev) {
		chosen = ev;
		ev = NULL;
		plan->strategy = MEDIA_VIRTUAL;
	} else {
		chosen = en;
		en = NULL;
		plan->strategy = MEDIA_NATIVE;
		ptrvec_free(&plan->variant_titles, free);
		plan->variant_titles = native_titles;
		native_titles = (struct ptrvec){ 0 };
	}
	ptrvec_free(&native_titles, free);
	boot_entry_free(ev);
	boot_entry_free(en);

	if (!chosen) {
		const char *why = plan->strategy == MEDIA_NATIVE ? nb : vb;
		if (!why)
			why = vb ? vb : nb;
		plan->reason = xasprintf("%s cannot boot this image: %s", ad->name, why ? why : "no strategy available");
		return -1;
	}

	/* Kernels up to 5.10 (antiX 26.1's) call a UEFI runtime service through a
	 * null pointer when they are started by kexec from a current kernel
	 * (load_uefi_certs, seen in the lab).  Without runtime services they run
	 * normally; an installer then cannot add a firmware boot entry. */
	if (old_kernel(p->id.kernel_version, 5, 10) && !entry_has_arg(chosen, "efi") && !entry_has_arg(chosen, "noefi")) {
		entry_set_arg(chosen, "efi", "noruntime");
		add_note(plan, "this older kernel runs without UEFI runtime services (efi=noruntime)");
	}
	if (c->safe_graphics && !entry_has_arg(chosen, "nomodeset"))
		entry_set_arg(chosen, "nomodeset", NULL);
	if (c->extra_args && *c->extra_args) {
		struct ptrvec extra = { 0 };
		cmdline_split(c->extra_args, &extra);
		for (size_t i = 0; i < extra.n; i++) {
			char *a = extra.v[i];
			char *eq = strchr(a, '=');
			if (eq) {
				*eq = '\0';
				entry_set_arg(chosen, a, eq + 1);
			} else {
				entry_set_arg(chosen, a, NULL);
			}
		}
		ptrvec_free(&extra, free);
	}
	if (c->serial_console) {
		/* Lab mode: mirror the console to the first serial port. */
		entry_remove_arg(chosen, "quiet");
		entry_remove_arg(chosen, "splash");
		entry_remove_arg(chosen, "rhgb");
		/* debian-installer starts one installer per console: two of them load
		 * their components at the same time and corrupt each other's package
		 * database ("corrupted status flag"), so it only gets the serial one */
		if (strcmp(ad->id, ADAPTER_DEBIAN_INSTALLER) != 0)
			entry_add_arg(chosen, "console=tty0");
		entry_add_arg(chosen, "console=ttyS0,115200");
	}
	plan->needs_preinit = plan->strategy == MEDIA_VIRTUAL;
	plan->entry = chosen;
	if (plan->needs_preinit && !strcmp(ad->id, ADAPTER_DEBIAN_INSTALLER)) {
		plan->udeb = debian_installer_loop_udeb(p);
		add_note(plan, "the installer's loop driver comes from the image itself (%s)", plan->udeb);
	}
	/* KIWI OEM self-install images copy themselves onto a disk right away.
	 * Started from the virtual medium they cannot recognise the drive they came
	 * from, so they offer the Distro Buny drive itself as a target. */
	const char *root = entry_get_arg(chosen, "root");
	if (entry_has_arg(chosen, "cdinst") || (root && str_starts(root, "install:")))
		plan->warning = xstrdup("This is a self-installing disk image: it copies itself onto a disk and erases "
					"everything on that disk. Started from Distro Buny it cannot tell which drive it "
					"came from, so it may offer this Distro Buny drive as the target. Check the target "
					"disk carefully before you confirm the installation.");
	/* Unattended installations (Ubuntu autoinstall, kickstart, d-i preseeding)
	 * partition a disk without asking anyone: on someone's PC that can be the
	 * internal disk. */
	else if (unattended_install(p, chosen))
		plan->warning = xstrdup("This image installs a system automatically as soon as it starts: it can erase a "
					"disk of this computer without asking first. Start it only on a computer whose "
					"disks may be erased.");

	char *bad = validate_files(p, chosen);
	if (bad) {
		plan->reason = bad;
		return -1;
	}
	plan->level = COMPAT_PREPARED;
	if (!strcmp(ad->id, ADAPTER_BEST_EFFORT))
		add_note(plan, "Distro Buny does not know this distribution family; it tries the image's own boot entry "
			       "with the virtual medium");
	if (plan->strategy == MEDIA_VIRTUAL) {
		plan->reason = xasprintf("%s; booted through the Distro Buny virtual medium", ad->mechanism);
		if (c->iso_fragmented)
			add_note(plan, "the ISO file is fragmented on disk and will be mapped with device-mapper");
	} else {
		plan->reason = xasprintf("using the distribution's own ISO-file boot support (%s)", ad->name);
		add_note(plan, "requires the distribution's initramfs to read the %s data partition",
			 c->data_fs_type ? c->data_fs_type : "Distro Buny");
	}
	return 0;
}

void plan_free(struct boot_plan *plan)
{
	boot_entry_free(plan->entry);
	ptrvec_free(&plan->variant_titles, free);
	free(plan->virtual_blocker);
	free(plan->native_blocker);
	free(plan->reason);
	ptrvec_free(&plan->notes, free);
	free(plan->warning);
	free(plan->udeb);
	memset(plan, 0, sizeof(*plan));
}
