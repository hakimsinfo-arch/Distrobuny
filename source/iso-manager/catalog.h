/*
 * Distro Buny — ISO catalog
 *
 * The catalog mirrors the data partition's directory tree: directories are
 * categories, *.iso files are candidate images.  Scanning only reads
 * directory entries and stat() results; image metadata is probed lazily (or
 * restored from the index cache, keyed by path, size and mtime), so a drive
 * with hundreds of images opens instantly.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BUNY_CATALOG_H
#define BUNY_CATALOG_H

#include <stdbool.h>
#include <stdint.h>

#include "boot-adapters/adapters.h"
#include "common/strbuf.h"
#include "iso-manager/identify.h"
#include "iso-manager/rules.h"

#define CATALOG_MAX_DEPTH 12
#define CATALOG_MAX_NODES 20000

enum cat_kind { CAT_FOLDER, CAT_ISO };

struct cat_meta {
	char *name, *variant, *version, *arch, *kernel_version;
	char *label, *uuid, *created, *info_line, *vendor, *icon;
	char *adapter, *adapter_name;
	bool kernel_signed;
	char *kernel_signer;
	enum compat_level level;
	enum media_strategy strategy;
	char *reason;
	bool recognized;
	bool virtual_possible, native_possible;
	char *virtual_blocker, *native_blocker;
	char *warning;           /* destructive behaviour to confirm before starting */
	struct ptrvec variants;  /* char* titles */
	char *error;
};

struct cat_node {
	enum cat_kind kind;
	char *name;           /* on-disk name */
	char *rel_path;       /* "/Desktop/ubuntu/kubuntu.iso" */
	char *display;        /* folder display name or ISO display name */
	char *icon;
	char *description;
	struct cat_node *parent;
	struct cat_node **children;
	size_t nchildren;
	uint64_t size;
	int64_t mtime;
	size_t iso_count;     /* folders: images in the subtree */
	uint64_t subtree_bytes;
	struct cat_meta *meta;  /* ISO: NULL until probed or loaded from the index */
	char *error;            /* unreadable entry */
};

struct catalog {
	struct cat_node *root;
	char *base_dir;
	const struct ruleset *rules;
	size_t total_isos;
	uint64_t total_bytes;
	size_t skipped;         /* entries ignored: hidden, non-ISO, limits */
};

int catalog_scan(struct catalog *cat, const char *base_dir, const struct ruleset *rules, char *err, size_t errlen);
void catalog_free(struct catalog *cat);
struct cat_node *catalog_find(struct catalog *cat, const char *rel_path);

/* Probe one ISO node (fills node->meta).  ctx may be NULL for defaults. */
int catalog_probe(struct catalog *cat, struct cat_node *n, const struct probe_env *env, const struct boot_ctx *ctx);
/* Thread-safe variant: compute metadata without touching the tree (safe to
 * call from a worker thread); apply it later with catalog_set_meta(). */
struct cat_meta *catalog_probe_meta(const char *base_dir, const char *rel_path, const struct probe_env *env,
				    const struct boot_ctx *ctx);
void catalog_set_meta(struct cat_node *n, struct cat_meta *m);
void cat_meta_free(struct cat_meta *m);

/* Index cache. */
int catalog_save_index(const struct catalog *cat, const char *path);
int catalog_load_index(struct catalog *cat, const char *path); /* returns number of entries restored */
void catalog_to_json(const struct catalog *cat, struct strbuf *sb, bool pretty);

/* Iterate ISO nodes depth-first. */
typedef void (*cat_visit_fn)(struct cat_node *n, void *ctx);
void catalog_walk(struct cat_node *n, cat_visit_fn fn, void *ctx);

#endif
