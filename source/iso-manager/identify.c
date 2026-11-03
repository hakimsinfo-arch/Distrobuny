/*
 * Distro Buny — ISO probing and identification
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "iso-manager/identify.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "common/log.h"
#include "common/strbuf.h"

static const char *loopback_cfgs[] = { "/boot/grub/loopback.cfg", "/boot/grub2/loopback.cfg", NULL };
static const char *grub_cfgs[] = { "/boot/grub/grub.cfg", "/boot/grub2/grub.cfg", "/EFI/BOOT/grub.cfg",
				   "/EFI/boot/grub.cfg", "/boot/grub/x86_64-efi/grub.cfg", NULL };

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

int kernel_header_info(struct iso_image *iso, const char *kernel_path, char **arch, char **version)
{
	*arch = NULL;
	*version = NULL;
	struct iso_file f;
	if (iso_lookup(iso, kernel_path, &f) < 0)
		return -1;
	uint8_t hdr[1024];
	ssize_t n = iso_pread(iso, &f, hdr, sizeof(hdr), 0);
	int rc = -1;
	if (n >= 0x240 && memcmp(hdr + 0x202, "HdrS", 4) == 0) {
		uint16_t proto = rd16(hdr + 0x206);
		uint16_t xload = proto >= 0x020c ? rd16(hdr + 0x236) : 0;
		*arch = xstrdup((xload & 1) ? "x86_64" : "i386");
		/* kernel_version: offset of a NUL-terminated string, relative to 0x200 */
		uint16_t kv = rd16(hdr + 0x20e);
		if (kv) {
			char buf[160] = "";
			ssize_t r = iso_pread(iso, &f, buf, sizeof(buf) - 1, (uint64_t)kv + 0x200);
			if (r > 0) {
				buf[r] = '\0';
				char *sp = strchr(buf, ' ');
				if (sp)
					*sp = '\0';
				bool ok = buf[0] != '\0';
				for (char *c = buf; *c; c++)
					if (!isprint((unsigned char)*c))
						ok = false;
				if (ok)
					*version = xstrdup(buf);
			}
		}
		rc = 0;
	} else if (n >= 0x40 && hdr[0] == 'M' && hdr[1] == 'Z') {
		uint32_t pe = rd32(hdr + 0x3c);
		uint8_t pehdr[8];
		if (pe < 4096 && iso_pread(iso, &f, pehdr, sizeof(pehdr), pe) == (ssize_t)sizeof(pehdr) &&
		    memcmp(pehdr, "PE\0\0", 4) == 0) {
			uint16_t mach = rd16(pehdr + 4);
			*arch = xstrdup(mach == 0x8664 ? "x86_64" : mach == 0xaa64 ? "aarch64" : mach == 0x14c ? "i386" : "unknown");
			rc = 0;
		}
	}
	iso_file_free(&f);
	return rc;
}

bool kernel_signature_info(struct iso_image *iso, const char *kernel_path, char **signer)
{
	*signer = NULL;
	struct iso_file f;
	if (iso_lookup(iso, kernel_path, &f) < 0)
		return false;
	bool sig = false;
	uint8_t hdr[512];
	if (iso_pread(iso, &f, hdr, sizeof(hdr), 0) == (ssize_t)sizeof(hdr) && hdr[0] == 'M' && hdr[1] == 'Z') {
		uint32_t pe = rd32(hdr + 0x3c);
		uint8_t ph[256];
		if (pe < 4096 && iso_pread(iso, &f, ph, sizeof(ph), pe) == (ssize_t)sizeof(ph) && !memcmp(ph, "PE\0\0", 4)) {
			uint16_t magic = rd16(ph + 24);
			/* data directory 4 (certificate table): PE32+ at optional header + 144, PE32 at + 128 */
			size_t dd = magic == 0x20b ? 24 + 112 + 32 : 24 + 96 + 32;
			uint32_t cert_off = rd32(ph + dd), cert_size = rd32(ph + dd + 4);
			if (cert_size > 8 && cert_off < f.size && cert_size <= 65536) {
				sig = true;
				uint8_t *blob = xmalloc(cert_size);
				if (iso_pread(iso, &f, blob, cert_size, cert_off) == (ssize_t)cert_size) {
					/* Certificate subjects carry the Common Name attribute: OID
					 * 2.5.4.3 (06 03 55 04 03) followed by a string (UTF8String,
					 * PrintableString, T61String or BMPString-free ASCII).  The
					 * signing certificate's CN mentions "Signing"/"Signer";
					 * otherwise report the issuing CA. */
					char best[128] = "", ca[128] = "";
					for (size_t i = 0; i + 7 < cert_size; i++) {
						if (memcmp(blob + i, "\x06\x03\x55\x04\x03", 5) != 0)
							continue;
						uint8_t tag = blob[i + 5], len = blob[i + 6];
						if ((tag != 0x0C && tag != 0x13 && tag != 0x14) || len >= 0x80 || i + 7 + len > cert_size)
							continue;
						char s[128];
						size_t n = len < sizeof(s) - 1 ? len : sizeof(s) - 1;
						memcpy(s, blob + i + 7, n);
						s[n] = '\0';
						if (str_icontains(s, "sign"))
							snprintf(best, sizeof(best), "%s", s);
						else if (!ca[0])
							snprintf(ca, sizeof(ca), "%s", s);
					}
					if (best[0] && ca[0] && strcmp(best, ca) != 0) {
						char both[300];
						snprintf(both, sizeof(both), "%s (issued by %s)", best, ca);
						snprintf(best, sizeof(best), "%.127s", both);
					} else if (!best[0]) {
						snprintf(best, sizeof(best), "%s", ca);
					}
					if (best[0])
						*signer = xstrdup(best);
				}
				free(blob);
			}
		}
	}
	iso_file_free(&f);
	return sig;
}

static void parse_treeinfo(struct iso_probe *p)
{
	const char *paths[] = { "/.treeinfo", "/treeinfo", "/install/.treeinfo", NULL };
	for (int i = 0; paths[i]; i++) {
		char *t = iso_read_text(p->iso, paths[i], 256 * 1024, NULL);
		if (!t)
			continue;
		char section[64] = "";
		char *save = NULL;
		for (char *ln = strtok_r(t, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
			char *l = str_trim(ln);
			if (*l == '[') {
				snprintf(section, sizeof(section), "%s", l);
				continue;
			}
			char *eq = strchr(l, '=');
			if (!eq)
				continue;
			*eq = '\0';
			char *k = str_trim(l), *v = str_trim(eq + 1);
			bool gen = !strcmp(section, "[general]") || !strcmp(section, "[release]");
			if (!gen)
				continue;
			if (!strcmp(k, "name") && !p->tree_name)
				p->tree_name = xstrdup(v);
			else if (!strcmp(k, "version") && !p->tree_version)
				p->tree_version = xstrdup(v);
			else if (!strcmp(k, "variant") && !p->tree_variant)
				p->tree_variant = xstrdup(v);
			else if (!strcmp(k, "arch") && !p->tree_arch)
				p->tree_arch = xstrdup(v);
		}
		free(t);
		return;
	}
}

static bool entries_reference(const struct boot_entries *l, const char *needle)
{
	for (size_t i = 0; i < l->n; i++)
		for (size_t k = 0; k < l->v[i]->args.n; k++)
			if (strstr(l->v[i]->args.v[k], needle))
				return true;
	return false;
}

char *name_from_filename(const char *file_name)
{
	char *s = xstrdup(file_name);
	char *dot = strrchr(s, '.');
	if (dot && !strcasecmp(dot, ".iso"))
		*dot = '\0';
	for (char *c = s; *c; c++)
		if (*c == '_' || *c == '-')
			*c = ' ';
	/* capitalise words that start with a letter */
	bool start = true;
	for (char *c = s; *c; c++) {
		if (start && islower((unsigned char)*c))
			*c = (char)toupper((unsigned char)*c);
		start = *c == ' ';
	}
	char *t = xstrdup(str_trim(s));
	free(s);
	return t;
}

static void identify(struct iso_probe *p, const struct probe_env *env)
{
	struct iso_identity *id = &p->id;
	const char *first_title = NULL;
	if (p->grub.n)
		first_title = p->grub.v[0]->title;
	else if (p->sdboot.n)
		first_title = p->sdboot.v[0]->title;
	else if (p->syslinux.n)
		first_title = p->syslinux.v[0]->title;

	char *info_first = NULL;
	if (p->disk_info) {
		info_first = xstrdup(p->disk_info);
		char *nl = strchr(info_first, '\n');
		if (nl)
			*nl = '\0';
		id->info_line = xstrdup(str_trim(info_first));
	}
	const char *src[RS_COUNT] = { 0 };
	src[RS_LABEL] = p->info.volume_id;
	src[RS_FILE] = p->file_name;
	src[RS_INFO] = id->info_line;
	src[RS_TREEINFO] = p->tree_name;
	src[RS_APP] = p->info.application_id;
	src[RS_TITLE] = first_title;

	const struct rule *r = env && env->rules ? rules_match(env->rules, src) : NULL;
	if (r) {
		id->recognized = true;
		id->rule_id = xstrdup(r->id);
		id->name = xstrdup(r->name ? r->name : r->id);
		id->icon = xstrdup(r->icon ? r->icon : (r->vendor ? r->vendor : "generic"));
		id->vendor = r->vendor ? xstrdup(r->vendor) : NULL;
		char *v = rules_extract(r->variant_re, r->nvariant_re, src);
		if (v) {
			/* "xfce" -> "Xfce", "nvidia" -> "NVIDIA" */
			if (!strcasecmp(v, "nvidia") || !strcasecmp(v, "kde") || !strcasecmp(v, "lxqt")) {
				for (char *c = v; *c; c++)
					*c = (char)toupper((unsigned char)*c);
			} else if (islower((unsigned char)v[0])) {
				v[0] = (char)toupper((unsigned char)v[0]);
			}
			id->variant = v;
		} else if (r->variant) {
			id->variant = xstrdup(r->variant);
		}
		id->version = rules_extract(r->version, r->nversion, src);
		if (!id->version && r->rolling && p->info.created[0]) {
			/* rolling release: the image build date is the meaningful version */
			id->version = xasprintf("%.4s.%.2s.%.2s", p->info.created, p->info.created + 5, p->info.created + 8);
		}
	} else {
		id->name = name_from_filename(p->file_name);
		id->icon = xstrdup("generic");
	}
	if (!id->version && p->tree_version)
		id->version = xstrdup(p->tree_version);
	free(info_first);
}

static void eval_grub(struct iso_probe *p, const struct probe_env *env, const char *cfg, bool with_iso_path,
		      struct boot_entries *out)
{
	struct grub_eval_ctx ctx = {
		.iso = p->iso,
		.iso_path = with_iso_path ? p->iso_path : NULL,
		.data_fs_uuid = env && env->data_fs_uuid ? env->data_fs_uuid : "@DATA_UUID@",
		.data_fs_label = env && env->data_fs_label ? env->data_fs_label : "DISTROBUNY",
		.source = with_iso_path ? SRC_GRUB_LOOPBACK : SRC_GRUB,
		.max_steps = 0,
	};
	char err[200];
	if (grubcfg_eval(&ctx, cfg, out, err, sizeof(err)) < 0)
		LOGW("%s: evaluating %s: %s", p->file_name, cfg, err);
}

int iso_probe_open(struct iso_probe *p, const char *host_path, const char *iso_path, const struct probe_env *env)
{
	memset(p, 0, sizeof(*p));
	p->host_path = xstrdup(host_path);
	p->iso_path = xstrdup(iso_path ? iso_path : host_path);
	p->file_name = xstrdup(path_basename(host_path));
	struct stat st;
	if (stat(host_path, &st) == 0)
		p->file_size = (uint64_t)st.st_size;
	char err[256];
	p->iso = iso_open_path(host_path, err, sizeof(err));
	if (!p->iso) {
		p->error = xstrdup(err);
		p->id.name = name_from_filename(p->file_name);
		p->id.icon = xstrdup("generic");
		return -1;
	}
	p->info = *iso_get_info(p->iso);
	if (p->info.volume_bytes > p->file_size + ISO_SECTOR * 300u && p->file_size) {
		/* A volume larger than the file means a truncated download. */
		p->error = xasprintf("image is truncated: volume is %llu bytes but the file has %llu",
				     (unsigned long long)p->info.volume_bytes, (unsigned long long)p->file_size);
	}
	p->disk_info = iso_read_text(p->iso, "/.disk/info", 4096, NULL);
	parse_treeinfo(p);

	/* Loopback (ISO-file aware) configuration. */
	for (int i = 0; loopback_cfgs[i]; i++) {
		if (!iso_exists(p->iso, loopback_cfgs[i]))
			continue;
		eval_grub(p, env, loopback_cfgs[i], true, &p->loopback);
		p->loopback_cfg = xstrdup(loopback_cfgs[i]);
		break;
	}
	/* Main configuration as GRUB sees it when booting the real medium. */
	for (int i = 0; grub_cfgs[i]; i++) {
		if (!iso_exists(p->iso, grub_cfgs[i]))
			continue;
		struct boot_entries tmp = { 0 };
		eval_grub(p, env, grub_cfgs[i], false, &tmp);
		if (!tmp.n) {
			boot_entries_free(&tmp);
			continue;
		}
		p->grub = tmp;
		p->grub_cfg = xstrdup(grub_cfgs[i]);
		/* Some images (NixOS, openSUSE Agama) implement ISO-file booting in
		 * their main config, keyed on ${iso_path}; evaluate that mode too. */
		if (!p->loopback.n) {
			struct boot_entries lb = { 0 };
			eval_grub(p, env, grub_cfgs[i], true, &lb);
			if (entries_reference(&lb, p->iso_path)) {
				p->loopback = lb;
				p->loopback_cfg = xstrdup(grub_cfgs[i]);
			} else {
				boot_entries_free(&lb);
			}
		}
		break;
	}
	sdboot_parse(p->iso, &p->sdboot);
	syslinux_parse(p->iso, &p->syslinux);
	/* ISO-file awareness: the evaluated entries carry the path, or (when GRUB
	 * word-splits an unquoted ${iso_path} containing spaces) the config at
	 * least references the variable. */
	p->loopback_native = entries_reference(&p->loopback, p->iso_path);
	if (!p->loopback_native && p->loopback_cfg) {
		char *text = iso_read_text(p->iso, p->loopback_cfg, 2u << 20, NULL);
		p->loopback_native = text && strstr(text, "iso_path") && p->loopback.n > 0;
		free(text);
	}

	identify(p, env);

	/* Architecture and kernel version from the first kernel we can find. */
	struct boot_entries *lists[] = { &p->grub, &p->sdboot, &p->syslinux, &p->loopback };
	for (size_t li = 0; li < ARRAY_SIZE(lists) && !p->id.arch; li++)
		for (size_t i = 0; i < lists[li]->n && !p->id.arch; i++)
			if (lists[li]->v[i]->kernel && kernel_header_info(p->iso, lists[li]->v[i]->kernel, &p->id.arch, &p->id.kernel_version) == 0)
				p->id.kernel_signed = kernel_signature_info(p->iso, lists[li]->v[i]->kernel, &p->id.kernel_signer);
	return 0;
}

void iso_probe_close(struct iso_probe *p)
{
	free(p->host_path);
	free(p->iso_path);
	free(p->file_name);
	if (p->iso)
		iso_close(p->iso);
	free(p->disk_info);
	free(p->tree_name);
	free(p->tree_version);
	free(p->tree_variant);
	free(p->tree_arch);
	boot_entries_free(&p->loopback);
	boot_entries_free(&p->grub);
	boot_entries_free(&p->sdboot);
	boot_entries_free(&p->syslinux);
	free(p->loopback_cfg);
	free(p->grub_cfg);
	struct iso_identity *id = &p->id;
	free(id->rule_id);
	free(id->name);
	free(id->variant);
	free(id->version);
	free(id->icon);
	free(id->vendor);
	free(id->arch);
	free(id->kernel_version);
	free(id->kernel_signer);
	free(id->info_line);
	free(p->error);
	memset(p, 0, sizeof(*p));
}

void identify_folder(const struct ruleset *rules, const char *folder_name, const char **icon, char **display_name,
		     const char **description)
{
	bool whole = false;
	const struct rule *r = rules ? rules_match_folder(rules, folder_name, &whole) : NULL;
	*icon = r && r->icon ? r->icon : "folder";
	*description = r ? r->description : NULL;
	/* "open suse" becomes "openSUSE", but "Ubuntu Family" stays the user's
	 * name: a rule only renames a folder whose whole name it matches. */
	if (r && r->name && whole) {
		*display_name = xstrdup(r->name);
	} else {
		char *n = xstrdup(folder_name);
		if (islower((unsigned char)n[0]))
			n[0] = (char)toupper((unsigned char)n[0]);
		*display_name = n;
	}
}
