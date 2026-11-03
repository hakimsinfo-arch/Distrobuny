/*
 * Distro Buny — boot entries, systemd-boot and SYSLINUX parsers
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "boot-adapters/bootcfg.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/log.h"
#include "common/strbuf.h"

#define MAX_TEXT (1u << 20)

/* ------------------------------------------------------------- entries */

void boot_entry_free(struct boot_entry *e)
{
	if (!e)
		return;
	free(e->title);
	free(e->id);
	free(e->kernel);
	ptrvec_free(&e->initrds, free);
	ptrvec_free(&e->args, free);
	free(e->efi);
	free(e->config);
	free(e);
}

struct boot_entry *boot_entry_clone(const struct boot_entry *e)
{
	struct boot_entry *c = xcalloc(1, sizeof(*c));
	c->title = e->title ? xstrdup(e->title) : NULL;
	c->id = e->id ? xstrdup(e->id) : NULL;
	c->kernel = e->kernel ? xstrdup(e->kernel) : NULL;
	for (size_t i = 0; i < e->initrds.n; i++)
		ptrvec_push(&c->initrds, xstrdup(e->initrds.v[i]));
	for (size_t i = 0; i < e->args.n; i++)
		ptrvec_push(&c->args, xstrdup(e->args.v[i]));
	c->efi = e->efi ? xstrdup(e->efi) : NULL;
	c->config = e->config ? xstrdup(e->config) : NULL;
	c->source = e->source;
	c->order = e->order;
	c->is_default = e->is_default;
	return c;
}

void boot_entries_free(struct boot_entries *list)
{
	for (size_t i = 0; i < list->n; i++)
		boot_entry_free(list->v[i]);
	free(list->v);
	list->v = NULL;
	list->n = 0;
}

void boot_entries_add(struct boot_entries *list, struct boot_entry *e)
{
	list->v = xrealloc(list->v, (list->n + 1) * sizeof(*list->v));
	list->v[list->n++] = e;
}

/* --------------------------------------------------------- arguments */

static bool arg_matches(const char *arg, const char *key)
{
	size_t kl = strlen(key);
	return strncmp(arg, key, kl) == 0 && (arg[kl] == '\0' || arg[kl] == '=');
}

const char *entry_get_arg(const struct boot_entry *e, const char *key)
{
	for (size_t i = 0; i < e->args.n; i++) {
		const char *a = e->args.v[i];
		if (arg_matches(a, key)) {
			const char *eq = strchr(a, '=');
			return eq ? eq + 1 : "";
		}
	}
	return NULL;
}

bool entry_has_arg(const struct boot_entry *e, const char *key)
{
	return entry_get_arg(e, key) != NULL;
}

static void insert_arg(struct boot_entry *e, char *arg)
{
	/* Insert before a "---" or "--" separator, otherwise append.  The kernel
	 * hands everything after "--" to init, and installers copy what follows
	 * "---" to the installed system: kernel parameters belong in front. */
	for (size_t i = 0; i < e->args.n; i++) {
		if (!strcmp(e->args.v[i], "---") || !strcmp(e->args.v[i], "--")) {
			ptrvec_push(&e->args, NULL);
			memmove(&e->args.v[i + 1], &e->args.v[i], (e->args.n - 1 - i) * sizeof(void *));
			e->args.v[i] = arg;
			return;
		}
	}
	ptrvec_push(&e->args, arg);
}

void entry_set_arg(struct boot_entry *e, const char *key, const char *value)
{
	char *nv = value ? xasprintf("%s=%s", key, value) : xstrdup(key);
	for (size_t i = 0; i < e->args.n; i++) {
		if (arg_matches(e->args.v[i], key)) {
			free(e->args.v[i]);
			e->args.v[i] = nv;
			return;
		}
	}
	insert_arg(e, nv);
}

void entry_add_arg(struct boot_entry *e, const char *arg)
{
	insert_arg(e, xstrdup(arg));
}

void entry_remove_arg(struct boot_entry *e, const char *key)
{
	size_t w = 0;
	for (size_t i = 0; i < e->args.n; i++) {
		if (arg_matches(e->args.v[i], key)) {
			free(e->args.v[i]);
			continue;
		}
		e->args.v[w++] = e->args.v[i];
	}
	e->args.n = w;
}

void entry_add_arg_front(struct boot_entry *e, const char *arg)
{
	ptrvec_push(&e->args, NULL);
	memmove(&e->args.v[1], &e->args.v[0], (e->args.n - 1) * sizeof(void *));
	e->args.v[0] = xstrdup(arg);
}

char *entry_cmdline(const struct boot_entry *e)
{
	struct strbuf sb = STRBUF_INIT;
	for (size_t i = 0; i < e->args.n; i++) {
		const char *a = e->args.v[i];
		if (!a[0])
			continue;
		if (sb.len)
			sb_addc(&sb, ' ');
		bool space = false;
		for (const char *p = a; *p; p++)
			if (isspace((unsigned char)*p))
				space = true;
		/* Same convention as GRUB's grub_create_loader_cmdline: wrap the whole
		 * argument in double quotes when it contains whitespace. */
		if (space)
			sb_addc(&sb, '"');
		for (const char *p = a; *p; p++) {
			if (*p == '"' || *p == '\\')
				sb_addc(&sb, '\\');
			sb_addc(&sb, *p);
		}
		if (space)
			sb_addc(&sb, '"');
	}
	return sb_detach(&sb);
}

void cmdline_split(const char *line, struct ptrvec *out)
{
	const char *p = line;
	struct strbuf cur = STRBUF_INIT;
	while (*p) {
		while (*p && isspace((unsigned char)*p))
			p++;
		if (!*p)
			break;
		bool inq = false;
		while (*p && (inq || !isspace((unsigned char)*p))) {
			if (*p == '"') {
				inq = !inq;
				p++;
				continue;
			}
			if (*p == '\\' && p[1] && (p[1] == '"' || p[1] == '\\')) {
				sb_addc(&cur, p[1]);
				p += 2;
				continue;
			}
			sb_addc(&cur, *p++);
		}
		ptrvec_push(out, sb_detach(&cur));
	}
	sb_free(&cur);
}

/* -------------------------------------------------------- systemd-boot */

static int sdboot_parse_file(struct iso_image *iso, const char *path, const char *def, struct boot_entries *out)
{
	char *text = iso_read_text(iso, path, MAX_TEXT, NULL);
	if (!text)
		return -1;
	struct boot_entry *e = xcalloc(1, sizeof(*e));
	e->source = SRC_SDBOOT;
	e->config = xstrdup(path);
	char *save = NULL;
	for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char *l = str_trim(line);
		if (!*l || *l == '#')
			continue;
		char *sp = l;
		while (*sp && !isspace((unsigned char)*sp))
			sp++;
		char *val = *sp ? str_trim(sp + 1) : sp;
		*sp = '\0';
		if (!strcmp(l, "title")) {
			free(e->title);
			e->title = xstrdup(val);
		} else if (!strcmp(l, "linux")) {
			free(e->kernel);
			e->kernel = grub_path_normalize(val);
		} else if (!strcmp(l, "initrd")) {
			ptrvec_push(&e->initrds, grub_path_normalize(val));
		} else if (!strcmp(l, "options")) {
			cmdline_split(val, &e->args);
		} else if (!strcmp(l, "efi")) {
			free(e->efi);
			e->efi = grub_path_normalize(val);
		}
	}
	free(text);
	if (!e->kernel && !e->efi) {
		boot_entry_free(e);
		return -1;
	}
	if (!e->title)
		e->title = xstrdup(path_basename(path));
	const char *base = path_basename(path);
	if (def && *def && iso_wildmatch(def, base, true))
		e->is_default = true;
	e->order = (int)out->n;
	boot_entries_add(out, e);
	return 0;
}

int sdboot_parse(struct iso_image *iso, struct boot_entries *out)
{
	const struct iso_dir *d = iso_list(iso, "/loader/entries");
	if (!d)
		return 0;
	struct ptrvec names = { 0 };
	for (size_t i = 0; i < d->n; i++)
		if (!d->nodes[i].is_dir && str_iends(d->nodes[i].name, ".conf"))
			ptrvec_push(&names, xstrdup(d->nodes[i].name));
	/* loader.conf "default" pattern */
	char *def = NULL;
	char *lc = iso_read_text(iso, "/loader/loader.conf", 65536, NULL);
	if (lc) {
		char *save = NULL;
		for (char *line = strtok_r(lc, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
			char *l = str_trim(line);
			if (str_starts(l, "default") && isspace((unsigned char)l[7]))
				def = xstrdup(str_trim(l + 8));
		}
		free(lc);
	}
	/* systemd-boot sorts entries by file name (sort-key aside) */
	for (size_t a = 0; a < names.n; a++)
		for (size_t b = a + 1; b < names.n; b++)
			if (natural_cmp(names.v[a], names.v[b]) > 0) {
				void *t = names.v[a];
				names.v[a] = names.v[b];
				names.v[b] = t;
			}
	int count = 0;
	for (size_t i = 0; i < names.n; i++) {
		char *p = xasprintf("/loader/entries/%s", (char *)names.v[i]);
		if (sdboot_parse_file(iso, p, def, out) == 0)
			count++;
		free(p);
	}
	free(def);
	ptrvec_free(&names, free);
	return count;
}

/* ------------------------------------------------------------ SYSLINUX */

static const char *syslinux_cfgs[] = {
	"/isolinux/isolinux.cfg", "/boot/isolinux/isolinux.cfg", "/boot/syslinux/syslinux.cfg",
	"/syslinux/syslinux.cfg", "/syslinux.cfg",               "/boot/x86_64/loader/isolinux.cfg",
	"/isolinux.cfg",          NULL,
};

struct sl_state {
	struct iso_image *iso;
	struct boot_entries *out;
	struct boot_entry *cur;
	char *cfgdir;
	char *def;
	int depth;
};

static char *sl_path(struct sl_state *st, const char *p)
{
	if (p[0] == '/')
		return grub_path_normalize(p);
	char *j = path_join(st->cfgdir, p);
	char *n = grub_path_normalize(j);
	free(j);
	return n;
}

static void sl_finish(struct sl_state *st)
{
	struct boot_entry *e = st->cur;
	st->cur = NULL;
	if (!e)
		return;
	if (!e->kernel || str_iends(e->kernel, ".c32") || str_iends(e->kernel, ".bin") || str_iends(e->kernel, ".0")) {
		boot_entry_free(e);
		return;
	}
	if (st->def && e->id && strcmp(st->def, e->id) == 0)
		e->is_default = true;
	e->order = (int)st->out->n;
	boot_entries_add(st->out, e);
}

static void sl_parse(struct sl_state *st, const char *path);

static void sl_line(struct sl_state *st, char *l)
{
	char *sp = l;
	while (*sp && !isspace((unsigned char)*sp))
		sp++;
	char *val = *sp ? str_trim(sp + 1) : sp;
	*sp = '\0';
	char *kw = l;
	if (!strcasecmp(kw, "label")) {
		sl_finish(st);
		st->cur = xcalloc(1, sizeof(struct boot_entry));
		st->cur->source = SRC_SYSLINUX;
		st->cur->id = xstrdup(val);
		st->cur->title = xstrdup(val);
	} else if (!strcasecmp(kw, "default")) {
		free(st->def);
		st->def = xstrdup(val);
	} else if (!strcasecmp(kw, "include") || !strcasecmp(kw, "config")) {
		char *p = sl_path(st, val);
		char *first = strtok(p, " \t");
		if (first)
			sl_parse(st, first);
		free(p);
	} else if (!st->cur) {
		return;
	} else if (!strcasecmp(kw, "kernel") || !strcasecmp(kw, "linux")) {
		free(st->cur->kernel);
		st->cur->kernel = sl_path(st, val);
	} else if (!strcasecmp(kw, "initrd")) {
		struct ptrvec parts = { 0 };
		char *dup = xstrdup(val);
		for (char *t = strtok(dup, ","); t; t = strtok(NULL, ","))
			ptrvec_push(&st->cur->initrds, sl_path(st, str_trim(t)));
		free(dup);
		ptrvec_free(&parts, free);
	} else if (!strcasecmp(kw, "append")) {
		struct ptrvec args = { 0 };
		cmdline_split(val, &args);
		for (size_t i = 0; i < args.n; i++) {
			char *a = args.v[i];
			if (!strncmp(a, "initrd=", 7)) {
				char *dup = xstrdup(a + 7);
				for (char *t = strtok(dup, ","); t; t = strtok(NULL, ","))
					ptrvec_push(&st->cur->initrds, sl_path(st, t));
				free(dup);
				free(a);
			} else {
				ptrvec_push(&st->cur->args, a);
			}
		}
		free(args.v);
	} else if (!strcasecmp(kw, "menu")) {
		char *sp2 = val;
		while (*sp2 && !isspace((unsigned char)*sp2))
			sp2++;
		if (!strncasecmp(val, "label", 5) && *sp2) {
			free(st->cur->title);
			char *t = xstrdup(str_trim(sp2));
			/* drop "^" hotkey markers */
			char *w = t;
			for (char *r = t; *r; r++)
				if (*r != '^')
					*w++ = *r;
			*w = '\0';
			st->cur->title = t;
		} else if (!strncasecmp(val, "default", 7)) {
			st->cur->is_default = true;
		}
	}
}

static void sl_parse(struct sl_state *st, const char *path)
{
	if (st->depth > 8)
		return;
	char *text = iso_read_text(st->iso, path, MAX_TEXT, NULL);
	if (!text)
		return;
	st->depth++;
	char *save = NULL;
	for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char *l = str_trim(line);
		if (!*l || *l == '#')
			continue;
		sl_line(st, l);
	}
	st->depth--;
	free(text);
}

int syslinux_parse(struct iso_image *iso, struct boot_entries *out)
{
	for (int i = 0; syslinux_cfgs[i]; i++) {
		if (!iso_exists(iso, syslinux_cfgs[i]))
			continue;
		struct sl_state st = { iso, out, NULL, path_dirname(syslinux_cfgs[i]), NULL, 0 };
		size_t before = out->n;
		sl_parse(&st, syslinux_cfgs[i]);
		sl_finish(&st);
		for (size_t k = before; k < out->n; k++) {
			free(out->v[k]->config);
			out->v[k]->config = xstrdup(syslinux_cfgs[i]);
		}
		free(st.cfgdir);
		free(st.def);
		if (out->n > before)
			return (int)(out->n - before);
	}
	return 0;
}
