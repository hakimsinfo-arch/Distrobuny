/*
 * Distro Buny — kernel module loading for the runtime
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "boot/modload.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "common/log.h"
#include "common/util.h"

#ifndef MODULE_INIT_COMPRESSED_FILE
#define MODULE_INIT_COMPRESSED_FILE 4
#endif

struct moddep {
	char *name;      /* normalised: '-' -> '_' */
	char *path;      /* relative to moddir */
	char **deps;     /* names */
	size_t ndeps;
	int state;       /* 0 unloaded, 1 loading, 2 loaded, -1 failed */
};

struct alias {
	char *pattern;
	char *module;
};

struct modctx {
	char *dir;
	struct moddep *mods;
	size_t nmods;
	struct alias *aliases;
	size_t naliases;
	char **builtin;
	size_t nbuiltin;
	size_t loaded;
};

static void normalize(char *s)
{
	for (; *s; s++)
		if (*s == '-')
			*s = '_';
}

static char *modname_from_path(const char *path)
{
	const char *b = path_basename(path);
	const char *dot = strstr(b, ".ko");
	char *n = xstrndup(b, dot ? (size_t)(dot - b) : strlen(b));
	normalize(n);
	return n;
}

static struct moddep *find_mod(struct modctx *m, const char *name)
{
	/* linear search is fine for a few hundred modules */
	for (size_t i = 0; i < m->nmods; i++)
		if (!strcmp(m->mods[i].name, name))
			return &m->mods[i];
	return NULL;
}

static bool is_builtin(struct modctx *m, const char *name)
{
	for (size_t i = 0; i < m->nbuiltin; i++)
		if (!strcmp(m->builtin[i], name))
			return true;
	return false;
}

static void load_dep_file(struct modctx *m)
{
	char *p = path_join(m->dir, "modules.dep");
	char *text = read_file(p, 64u << 20, NULL);
	free(p);
	if (!text)
		return;
	char *save = NULL;
	for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char *colon = strchr(line, ':');
		if (!colon)
			continue;
		*colon = '\0';
		m->mods = xrealloc(m->mods, (m->nmods + 1) * sizeof(*m->mods));
		struct moddep *d = &m->mods[m->nmods++];
		memset(d, 0, sizeof(*d));
		d->path = xstrdup(line);
		d->name = modname_from_path(line);
		char *s2 = NULL;
		for (char *tok = strtok_r(colon + 1, " \t", &s2); tok; tok = strtok_r(NULL, " \t", &s2)) {
			d->deps = xrealloc(d->deps, (d->ndeps + 1) * sizeof(char *));
			d->deps[d->ndeps++] = modname_from_path(tok);
		}
	}
	free(text);
}

static void load_alias_file(struct modctx *m)
{
	char *p = path_join(m->dir, "modules.alias");
	char *text = read_file(p, 64u << 20, NULL);
	free(p);
	if (!text)
		return;
	char *save = NULL;
	for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		if (strncmp(line, "alias ", 6) != 0)
			continue;
		char *pat = line + 6;
		char *sp = strchr(pat, ' ');
		if (!sp)
			continue;
		*sp = '\0';
		char *mod = xstrdup(sp + 1);
		normalize(mod);
		m->aliases = xrealloc(m->aliases, (m->naliases + 1) * sizeof(*m->aliases));
		m->aliases[m->naliases].pattern = xstrdup(pat);
		m->aliases[m->naliases].module = mod;
		m->naliases++;
	}
	free(text);
}

static void load_builtin_file(struct modctx *m)
{
	char *p = path_join(m->dir, "modules.builtin");
	char *text = read_file(p, 16u << 20, NULL);
	free(p);
	if (!text)
		return;
	char *save = NULL;
	for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		m->builtin = xrealloc(m->builtin, (m->nbuiltin + 1) * sizeof(char *));
		m->builtin[m->nbuiltin++] = modname_from_path(line);
	}
	free(text);
}

struct modctx *modload_open(const char *moddir)
{
	struct modctx *m = xcalloc(1, sizeof(*m));
	if (moddir) {
		m->dir = xstrdup(moddir);
	} else {
		struct utsname u;
		uname(&u);
		m->dir = xasprintf("/lib/modules/%s", u.release);
	}
	load_dep_file(m);
	load_alias_file(m);
	load_builtin_file(m);
	LOGD("modload: %zu modules, %zu aliases, %zu built in", m->nmods, m->naliases, m->nbuiltin);
	return m;
}

void modload_close(struct modctx *m)
{
	if (!m)
		return;
	for (size_t i = 0; i < m->nmods; i++) {
		free(m->mods[i].name);
		free(m->mods[i].path);
		for (size_t k = 0; k < m->mods[i].ndeps; k++)
			free(m->mods[i].deps[k]);
		free(m->mods[i].deps);
	}
	for (size_t i = 0; i < m->naliases; i++) {
		free(m->aliases[i].pattern);
		free(m->aliases[i].module);
	}
	for (size_t i = 0; i < m->nbuiltin; i++)
		free(m->builtin[i]);
	free(m->mods);
	free(m->aliases);
	free(m->builtin);
	free(m->dir);
	free(m);
}

static int insmod(struct modctx *m, struct moddep *d)
{
	char *full = path_join(m->dir, d->path);
	int fd = open(full, O_RDONLY | O_CLOEXEC);
	free(full);
	if (fd < 0)
		return -1;
	bool compressed = str_ends(d->path, ".zst") || str_ends(d->path, ".xz") || str_ends(d->path, ".gz");
	long r = syscall(SYS_finit_module, fd, "", compressed ? MODULE_INIT_COMPRESSED_FILE : 0);
	int e = errno;
	close(fd);
	if (r < 0 && e != EEXIST) {
		LOGW("modload: %s: %s", d->name, strerror(e));
		errno = e;
		return -1;
	}
	return 0;
}

static int load_rec(struct modctx *m, const char *name, int depth)
{
	if (depth > 32)
		return -1;
	if (is_builtin(m, name))
		return 0;
	struct moddep *d = find_mod(m, name);
	if (!d)
		return -1;
	if (d->state == 2)
		return 0;
	if (d->state == 1 || d->state == -1)
		return d->state == 1 ? 0 : -1; /* cycle guard / known failure */
	d->state = 1;
	for (size_t i = 0; i < d->ndeps; i++) {
		/* modules.dep lists the deepest dependency last */
		load_rec(m, d->deps[d->ndeps - 1 - i], depth + 1);
	}
	if (insmod(m, d) < 0) {
		d->state = -1;
		return -1;
	}
	d->state = 2;
	m->loaded++;
	LOGD("modload: loaded %s", name);
	return 0;
}

int modload_name(struct modctx *m, const char *name)
{
	char *n = xstrdup(name);
	normalize(n);
	int rc = load_rec(m, n, 0);
	free(n);
	return rc;
}

int modload_alias(struct modctx *m, const char *modalias)
{
	int count = 0;
	/* Aliases are grouped by bus prefix ("usb:", "pci:", "hid:" ...): compare
	 * the fixed prefix before running the full pattern match. */
	const char *colon = strchr(modalias, ':');
	size_t plen = colon ? (size_t)(colon - modalias) + 1 : 0;
	for (size_t i = 0; i < m->naliases; i++) {
		const struct alias *a = &m->aliases[i];
		if (plen) {
			bool wild = false;
			for (size_t k = 0; k < plen && a->pattern[k]; k++)
				if (strchr("*?[", a->pattern[k]))
					wild = true;
			if (!wild && strncmp(a->pattern, modalias, plen) != 0)
				continue;
		}
		if (fnmatch(a->pattern, modalias, 0) != 0)
			continue;
		struct moddep *d = find_mod(m, a->module);
		if (d && d->state == 2)
			continue;
		if (modload_name(m, a->module) == 0)
			count++;
	}
	return count;
}

static int walk(struct modctx *m, const char *dir, int depth)
{
	if (depth > 24)
		return 0;
	DIR *d = opendir(dir);
	if (!d)
		return 0;
	int count = 0;
	struct dirent *de;
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;
		if (de->d_type == DT_LNK)
			continue; /* sysfs symlinks would revisit devices */
		char *p = path_join(dir, de->d_name);
		if (!strcmp(de->d_name, "modalias") && de->d_type == DT_REG) {
			char *alias = read_file(p, 4096, NULL);
			if (alias) {
				count += modload_alias(m, str_trim(alias));
				free(alias);
			}
		} else if (de->d_type == DT_DIR) {
			count += walk(m, p, depth + 1);
		}
		free(p);
	}
	closedir(d);
	return count;
}

int modload_coldplug(struct modctx *m, const char *sysdir)
{
	char *root = path_join(sysdir, "devices");
	int total = 0;
	/* Loading a controller driver reveals new devices (USB hubs, disks), so
	 * repeat until a pass loads nothing new. */
	for (int pass = 0; pass < 6; pass++) {
		int n = walk(m, root, 0);
		total += n;
		if (n == 0)
			break;
		usleep(200 * 1000);
	}
	free(root);
	return total;
}

size_t modload_count_loaded(const struct modctx *m)
{
	return m->loaded;
}
