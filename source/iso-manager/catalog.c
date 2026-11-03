/*
 * Distro Buny — ISO catalog
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "iso-manager/catalog.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/json.h"
#include "common/log.h"

#define INDEX_VERSION 5

struct scan_state {
	struct catalog *cat;
	size_t nodes;
	dev_t dev;
	/* visited directory inodes, to defend against bind-mount/symlink loops */
	ino_t *seen;
	size_t nseen;
};

static struct cat_node *node_new(enum cat_kind k, const char *name, const char *rel)
{
	struct cat_node *n = xcalloc(1, sizeof(*n));
	n->kind = k;
	n->name = xstrdup(name);
	n->rel_path = xstrdup(rel);
	return n;
}

static void node_add(struct cat_node *parent, struct cat_node *child)
{
	parent->children = xrealloc(parent->children, (parent->nchildren + 1) * sizeof(*parent->children));
	parent->children[parent->nchildren++] = child;
	child->parent = parent;
}

static int node_cmp(const void *a, const void *b)
{
	const struct cat_node *x = *(const struct cat_node *const *)a, *y = *(const struct cat_node *const *)b;
	if (x->kind != y->kind)
		return x->kind == CAT_FOLDER ? -1 : 1; /* folders first */
	return natural_cmp(x->name, y->name);
}

/* Reject names that are unsafe to display or to use in paths. */
static bool name_acceptable(const char *name)
{
	if (!name[0] || name[0] == '.')
		return false; /* hidden files, ".distrobuny", "." and ".." */
	size_t len = strlen(name);
	if (len > 255)
		return false;
	for (const unsigned char *p = (const unsigned char *)name; *p; p++)
		if (*p < 0x20 || *p == 0x7f)
			return false; /* control characters */
	return true;
}

static void scan_dir(struct scan_state *st, struct cat_node *dir, const char *abs, int depth)
{
	if (depth > CATALOG_MAX_DEPTH) {
		st->cat->skipped++;
		return;
	}
	DIR *d = opendir(abs);
	if (!d) {
		dir->error = xasprintf("cannot open folder: %s", strerror(errno));
		return;
	}
	struct dirent *de;
	while ((de = readdir(d))) {
		if (!name_acceptable(de->d_name)) {
			if (strcmp(de->d_name, ".") && strcmp(de->d_name, ".."))
				st->cat->skipped++;
			continue;
		}
		if (st->nodes >= CATALOG_MAX_NODES) {
			st->cat->skipped++;
			continue;
		}
		char *child_abs = path_join(abs, de->d_name);
		char *child_rel = dir->rel_path[1] ? xasprintf("%s/%s", dir->rel_path, de->d_name) : xasprintf("/%s", de->d_name);
		struct stat sb;
		if (stat(child_abs, &sb) < 0) {
			/* broken symlink or vanished entry */
			if (str_iends(de->d_name, ".iso")) {
				struct cat_node *n = node_new(CAT_ISO, de->d_name, child_rel);
				n->error = xasprintf("cannot access file: %s", strerror(errno));
				node_add(dir, n);
				st->nodes++;
			}
			free(child_abs);
			free(child_rel);
			continue;
		}
		if (S_ISDIR(sb.st_mode)) {
			bool loop = false;
			for (size_t i = 0; i < st->nseen; i++)
				if (st->seen[i] == sb.st_ino)
					loop = true;
			if (!loop) {
				st->seen = xrealloc(st->seen, (st->nseen + 1) * sizeof(ino_t));
				st->seen[st->nseen++] = sb.st_ino;
				struct cat_node *n = node_new(CAT_FOLDER, de->d_name, child_rel);
				n->mtime = sb.st_mtime;
				node_add(dir, n);
				st->nodes++;
				scan_dir(st, n, child_abs, depth + 1);
			} else {
				st->cat->skipped++;
			}
		} else if (S_ISREG(sb.st_mode) && str_iends(de->d_name, ".iso")) {
			struct cat_node *n = node_new(CAT_ISO, de->d_name, child_rel);
			n->size = (uint64_t)sb.st_size;
			n->mtime = sb.st_mtime;
			if (access(child_abs, R_OK) != 0)
				n->error = xasprintf("file is not readable: %s", strerror(errno));
			else if (sb.st_size < 64 * 1024)
				n->error = xstrdup("file is too small to be a bootable ISO image");
			node_add(dir, n);
			st->nodes++;
		} else {
			st->cat->skipped++;
		}
		free(child_abs);
		free(child_rel);
	}
	closedir(d);
	if (dir->nchildren > 1)
		qsort(dir->children, dir->nchildren, sizeof(*dir->children), node_cmp);
}

static void finalize(struct catalog *cat, struct cat_node *n)
{
	if (n->kind == CAT_ISO) {
		n->iso_count = 1;
		n->subtree_bytes = n->size;
		cat->total_isos++;
		cat->total_bytes += n->size;
		if (!n->display) {
			char *d = name_from_filename(n->name);
			n->display = d;
		}
		if (!n->icon)
			n->icon = xstrdup("generic");
		return;
	}
	for (size_t i = 0; i < n->nchildren; i++) {
		finalize(cat, n->children[i]);
		n->iso_count += n->children[i]->iso_count;
		n->subtree_bytes += n->children[i]->subtree_bytes;
	}
	if (n->parent) {
		const char *icon, *desc;
		char *disp;
		identify_folder(cat->rules, n->name, &icon, &disp, &desc);
		n->display = disp;
		n->icon = xstrdup(icon);
		n->description = desc ? xstrdup(desc) : NULL;
	} else {
		n->display = xstrdup("Home");
		n->icon = xstrdup("home");
	}
}

int catalog_scan(struct catalog *cat, const char *base_dir, const struct ruleset *rules, char *err, size_t errlen)
{
	memset(cat, 0, sizeof(*cat));
	cat->base_dir = xstrdup(base_dir);
	cat->rules = rules;
	cat->root = node_new(CAT_FOLDER, "", "/");
	struct stat sb;
	if (stat(base_dir, &sb) < 0 || !S_ISDIR(sb.st_mode)) {
		snprintf(err, errlen, "%s is not a directory", base_dir);
		return -1;
	}
	struct scan_state st = { cat, 0, sb.st_dev, NULL, 0 };
	st.seen = xmalloc(sizeof(ino_t));
	st.seen[0] = sb.st_ino;
	st.nseen = 1;
	scan_dir(&st, cat->root, base_dir, 0);
	free(st.seen);
	finalize(cat, cat->root);
	return 0;
}

void cat_meta_free(struct cat_meta *m)
{
	if (!m)
		return;
	char **fields[] = { &m->name, &m->variant, &m->version, &m->arch, &m->kernel_version, &m->label, &m->uuid,
			    &m->created, &m->info_line, &m->vendor, &m->icon, &m->adapter, &m->adapter_name, &m->reason,
			    &m->virtual_blocker, &m->native_blocker, &m->error, &m->kernel_signer, &m->warning };
	for (size_t i = 0; i < ARRAY_SIZE(fields); i++)
		free(*fields[i]);
	ptrvec_free(&m->variants, free);
	free(m);
}

static void node_free(struct cat_node *n)
{
	for (size_t i = 0; i < n->nchildren; i++)
		node_free(n->children[i]);
	free(n->children);
	free(n->name);
	free(n->rel_path);
	free(n->display);
	free(n->icon);
	free(n->description);
	free(n->error);
	cat_meta_free(n->meta);
	free(n);
}

void catalog_free(struct catalog *cat)
{
	if (cat->root)
		node_free(cat->root);
	free(cat->base_dir);
	memset(cat, 0, sizeof(*cat));
}

struct cat_node *catalog_find(struct catalog *cat, const char *rel_path)
{
	if (!strcmp(rel_path, "/") || !*rel_path)
		return cat->root;
	struct cat_node *cur = cat->root;
	char *dup = xstrdup(rel_path);
	char *save = NULL;
	for (char *tok = strtok_r(dup, "/", &save); tok && cur; tok = strtok_r(NULL, "/", &save)) {
		struct cat_node *next = NULL;
		for (size_t i = 0; i < cur->nchildren; i++)
			if (!strcmp(cur->children[i]->name, tok))
				next = cur->children[i];
		cur = next;
	}
	free(dup);
	return cur;
}

void catalog_walk(struct cat_node *n, cat_visit_fn fn, void *ctx)
{
	fn(n, ctx);
	for (size_t i = 0; i < n->nchildren; i++)
		catalog_walk(n->children[i], fn, ctx);
}

static char *dup_or_null(const char *s)
{
	return s && *s ? xstrdup(s) : NULL;
}

static void set_display_from_meta(struct cat_node *n)
{
	struct cat_meta *m = n->meta;
	if (!m || !m->recognized)
		return;
	free(n->display);
	struct strbuf sb = STRBUF_INIT;
	sb_add(&sb, m->name);
	if (m->version) {
		sb_addc(&sb, ' ');
		sb_add(&sb, m->version);
	}
	n->display = sb_detach(&sb);
	free(n->icon);
	n->icon = xstrdup(m->icon ? m->icon : "generic");
}

struct cat_meta *catalog_probe_meta(const char *base_dir, const char *rel_path, const struct probe_env *env,
				    const struct boot_ctx *ctx)
{
	char *abs = path_join(base_dir, rel_path);
	struct iso_probe p;
	iso_probe_open(&p, abs, rel_path, env);
	free(abs);

	struct cat_meta *m = xcalloc(1, sizeof(*m));
	m->name = dup_or_null(p.id.name);
	m->variant = dup_or_null(p.id.variant);
	m->version = dup_or_null(p.id.version);
	m->arch = dup_or_null(p.id.arch);
	m->kernel_version = dup_or_null(p.id.kernel_version);
	m->label = dup_or_null(p.info.volume_id);
	m->uuid = dup_or_null(p.info.uuid);
	m->created = dup_or_null(p.info.created);
	m->info_line = dup_or_null(p.id.info_line);
	m->vendor = dup_or_null(p.id.vendor);
	m->icon = dup_or_null(p.id.icon);
	m->recognized = p.id.recognized;
	m->error = dup_or_null(p.error);
	m->kernel_signed = p.id.kernel_signed;
	m->kernel_signer = dup_or_null(p.id.kernel_signer);

	struct boot_ctx defctx = { .variant = -1, .strategy = MEDIA_AUTO, .data_fs_uuid = env ? env->data_fs_uuid : NULL,
				   .data_fs_label = env ? env->data_fs_label : NULL, .data_fs_type = env ? env->data_fs_type : NULL };
	struct boot_plan plan;
	plan_build(&p, ctx ? ctx : &defctx, &plan);
	m->level = plan.level;
	m->strategy = plan.strategy;
	m->reason = dup_or_null(plan.reason);
	m->virtual_possible = plan.virtual_possible;
	m->native_possible = plan.native_possible;
	m->virtual_blocker = dup_or_null(plan.virtual_blocker);
	m->native_blocker = dup_or_null(plan.native_blocker);
	m->warning = dup_or_null(plan.warning);
	if (plan.adapter) {
		m->adapter = xstrdup(plan.adapter->id);
		m->adapter_name = xstrdup(plan.adapter->name);
	}
	for (size_t i = 0; i < plan.variant_titles.n; i++)
		ptrvec_push(&m->variants, xstrdup(plan.variant_titles.v[i]));
	plan_free(&plan);
	iso_probe_close(&p);
	return m;
}

void catalog_set_meta(struct cat_node *n, struct cat_meta *m)
{
	cat_meta_free(n->meta);
	n->meta = m;
	set_display_from_meta(n);
}

int catalog_probe(struct catalog *cat, struct cat_node *n, const struct probe_env *env, const struct boot_ctx *ctx)
{
	if (n->kind != CAT_ISO)
		return -1;
	struct probe_env penv = env ? *env : (struct probe_env){ 0 };
	if (!penv.rules)
		penv.rules = cat->rules;
	catalog_set_meta(n, catalog_probe_meta(cat->base_dir, n->rel_path, &penv, ctx));
	return 0;
}

/* -------------------------------------------------------- index cache */

static void meta_to_json(struct json_writer *w, const struct cat_meta *m)
{
	jw_obj_begin(w);
#define S(k, v)               \
	do {                  \
		jw_key(w, k); \
		jw_str(w, v); \
	} while (0)
	S("name", m->name);
	S("variant", m->variant);
	S("version", m->version);
	S("arch", m->arch);
	S("kernel_version", m->kernel_version);
	S("label", m->label);
	S("uuid", m->uuid);
	S("created", m->created);
	S("info", m->info_line);
	S("vendor", m->vendor);
	S("icon", m->icon);
	S("adapter", m->adapter);
	S("adapter_name", m->adapter_name);
	S("level", compat_level_name(m->level));
	S("strategy", media_strategy_name(m->strategy));
	S("reason", m->reason);
	S("virtual_blocker", m->virtual_blocker);
	S("native_blocker", m->native_blocker);
	S("error", m->error);
	S("kernel_signer", m->kernel_signer);
	S("warning", m->warning);
#undef S
	jw_key(w, "kernel_signed");
	jw_bool(w, m->kernel_signed);
	jw_key(w, "recognized");
	jw_bool(w, m->recognized);
	jw_key(w, "virtual_possible");
	jw_bool(w, m->virtual_possible);
	jw_key(w, "native_possible");
	jw_bool(w, m->native_possible);
	jw_key(w, "variants");
	jw_arr_begin(w);
	for (size_t i = 0; i < m->variants.n; i++)
		jw_str(w, m->variants.v[i]);
	jw_arr_end(w);
	jw_obj_end(w);
}

static void node_to_json(struct json_writer *w, const struct cat_node *n)
{
	jw_obj_begin(w);
	jw_key(w, "kind");
	jw_str(w, n->kind == CAT_FOLDER ? "folder" : "iso");
	jw_key(w, "name");
	jw_str(w, n->name);
	jw_key(w, "path");
	jw_str(w, n->rel_path);
	jw_key(w, "display");
	jw_str(w, n->display);
	jw_key(w, "icon");
	jw_str(w, n->icon);
	if (n->description) {
		jw_key(w, "description");
		jw_str(w, n->description);
	}
	if (n->error) {
		jw_key(w, "error");
		jw_str(w, n->error);
	}
	if (n->kind == CAT_ISO) {
		jw_key(w, "size");
		jw_int(w, (long long)n->size);
		jw_key(w, "mtime");
		jw_int(w, (long long)n->mtime);
		if (n->meta) {
			jw_key(w, "meta");
			meta_to_json(w, n->meta);
		}
	} else {
		jw_key(w, "iso_count");
		jw_int(w, (long long)n->iso_count);
		jw_key(w, "bytes");
		jw_int(w, (long long)n->subtree_bytes);
		jw_key(w, "children");
		jw_arr_begin(w);
		for (size_t i = 0; i < n->nchildren; i++)
			node_to_json(w, n->children[i]);
		jw_arr_end(w);
	}
	jw_obj_end(w);
}

void catalog_to_json(const struct catalog *cat, struct strbuf *sb, bool pretty)
{
	struct json_writer w;
	jw_init(&w, sb, pretty);
	jw_obj_begin(&w);
	jw_key(&w, "format");
	jw_str(&w, "distrobuny-index");
	jw_key(&w, "version");
	jw_int(&w, INDEX_VERSION);
	jw_key(&w, "total_isos");
	jw_int(&w, (long long)cat->total_isos);
	jw_key(&w, "total_bytes");
	jw_int(&w, (long long)cat->total_bytes);
	jw_key(&w, "root");
	node_to_json(&w, cat->root);
	jw_obj_end(&w);
	if (pretty)
		sb_addc(sb, '\n');
}

int catalog_save_index(const struct catalog *cat, const char *path)
{
	struct strbuf sb = STRBUF_INIT;
	catalog_to_json(cat, &sb, false);
	int rc = write_file_atomic(path, sb.buf, sb.len);
	sb_free(&sb);
	return rc;
}

static enum compat_level level_from_name(const char *s)
{
	for (int l = COMPAT_UNKNOWN; l <= COMPAT_VERIFIED; l++)
		if (s && !strcmp(s, compat_level_name((enum compat_level)l)))
			return (enum compat_level)l;
	return COMPAT_UNKNOWN;
}

static char *jdup(const struct json *o, const char *k)
{
	const char *s = json_get_str(o, k, NULL);
	return s ? xstrdup(s) : NULL;
}

static size_t restore(struct catalog *cat, const struct json *jn)
{
	size_t restored = 0;
	const char *kind = json_get_str(jn, "kind", "");
	if (!strcmp(kind, "folder")) {
		const struct json *ch = json_get(jn, "children");
		if (ch && ch->type == JSON_ARRAY)
			for (size_t i = 0; i < ch->n; i++)
				restored += restore(cat, ch->items[i]);
		return restored;
	}
	const struct json *jm = json_get(jn, "meta");
	const char *path = json_get_str(jn, "path", NULL);
	if (!jm || !path)
		return 0;
	struct cat_node *n = catalog_find(cat, path);
	if (!n || n->kind != CAT_ISO || n->meta)
		return 0;
	/* Only trust the cache if the file is unchanged. */
	if ((uint64_t)json_get_num(jn, "size", -1) != n->size || (int64_t)json_get_num(jn, "mtime", -1) != n->mtime)
		return 0;
	struct cat_meta *m = xcalloc(1, sizeof(*m));
	m->name = jdup(jm, "name");
	m->variant = jdup(jm, "variant");
	m->version = jdup(jm, "version");
	m->arch = jdup(jm, "arch");
	m->kernel_version = jdup(jm, "kernel_version");
	m->label = jdup(jm, "label");
	m->uuid = jdup(jm, "uuid");
	m->created = jdup(jm, "created");
	m->info_line = jdup(jm, "info");
	m->vendor = jdup(jm, "vendor");
	m->icon = jdup(jm, "icon");
	m->adapter = jdup(jm, "adapter");
	m->adapter_name = jdup(jm, "adapter_name");
	m->reason = jdup(jm, "reason");
	m->virtual_blocker = jdup(jm, "virtual_blocker");
	m->native_blocker = jdup(jm, "native_blocker");
	m->error = jdup(jm, "error");
	m->kernel_signer = jdup(jm, "kernel_signer");
	m->warning = jdup(jm, "warning");
	m->kernel_signed = json_get_bool(jm, "kernel_signed", false);
	m->level = level_from_name(json_get_str(jm, "level", NULL));
	const char *st = json_get_str(jm, "strategy", "");
	m->strategy = !strcmp(st, "native-iso-file") ? MEDIA_NATIVE : MEDIA_VIRTUAL;
	m->recognized = json_get_bool(jm, "recognized", false);
	m->virtual_possible = json_get_bool(jm, "virtual_possible", false);
	m->native_possible = json_get_bool(jm, "native_possible", false);
	const struct json *vs = json_get(jm, "variants");
	if (vs && vs->type == JSON_ARRAY)
		for (size_t i = 0; i < vs->n; i++)
			if (vs->items[i]->type == JSON_STRING)
				ptrvec_push(&m->variants, xstrdup(vs->items[i]->str));
	n->meta = m;
	set_display_from_meta(n);
	return 1;
}

int catalog_load_index(struct catalog *cat, const char *path)
{
	size_t len;
	char *text = read_file(path, 64u << 20, &len);
	if (!text)
		return -1;
	char err[160];
	struct json *j = json_parse(text, len, err, sizeof(err));
	free(text);
	if (!j) {
		LOGW("index %s is invalid: %s", path, err);
		return -1;
	}
	int restored = -1;
	if (!strcmp(json_get_str(j, "format", ""), "distrobuny-index") && json_get_num(j, "version", 0) == INDEX_VERSION) {
		const struct json *root = json_get(j, "root");
		restored = root ? (int)restore(cat, root) : 0;
	} else {
		LOGW("index %s has an unsupported format; ignoring", path);
	}
	json_free(j);
	return restored;
}
