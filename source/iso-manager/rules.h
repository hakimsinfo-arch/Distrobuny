/*
 * Distro Buny — identification rules
 *
 * Distribution and folder identification is data driven: the rules live in
 * configuration/distributions/distributions.conf (installed into the runtime
 * at /usr/share/distrobuny/distributions.conf).  Sections are tried in file
 * order; a section matches when any of its match.* patterns matches the
 * corresponding source text.  Patterns are POSIX extended regular
 * expressions evaluated case-insensitively.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BUNY_RULES_H
#define BUNY_RULES_H

#include <regex.h>
#include <stdbool.h>

#include "common/util.h"

/* Sources a rule can match against. */
enum rule_src {
	RS_LABEL = 0,   /* ISO volume label */
	RS_FILE,        /* ISO file name */
	RS_INFO,        /* /.disk/info */
	RS_TREEINFO,    /* .treeinfo "name" */
	RS_APP,         /* ISO application id */
	RS_TITLE,       /* first boot entry title */
	RS_FOLDER,      /* folder name (folder rules only) */
	RS_COUNT
};

struct rule_pattern {
	enum rule_src src;
	regex_t re;
	char *text;
};

struct rule {
	char *id;
	bool folder;            /* [folder:xxx] section */
	char *name;
	char *icon;
	char *vendor;           /* grouping, icon fallback */
	char *variant;          /* literal variant name */
	char *description;
	bool rolling;           /* use the ISO build date when no version matched */
	struct rule_pattern *match;
	size_t nmatch;
	/* version extraction: per source optional regex with group 1 */
	struct rule_pattern *version;
	size_t nversion;
	struct rule_pattern *variant_re; /* group 1 becomes variant */
	size_t nvariant_re;
};

struct ruleset {
	struct rule *v;
	size_t n;
};

int rules_load(struct ruleset *rs, const char *path, char *err, size_t errlen);
int rules_load_text(struct ruleset *rs, const char *text, char *err, size_t errlen);
void rules_free(struct ruleset *rs);

/* Find the first distribution rule matching the given sources (NULL entries skipped). */
const struct rule *rules_match(const struct ruleset *rs, const char *const src[RS_COUNT]);
/* Folder rule for a folder name; *whole is true when the pattern matched the
 * entire name (only then does the rule's display name replace the user's). */
const struct rule *rules_match_folder(const struct ruleset *rs, const char *folder_name, bool *whole);
/* Extract with the rule's regexes; returns malloc'd string or NULL. */
char *rules_extract(const struct rule_pattern *pats, size_t n, const char *const src[RS_COUNT]);

#endif
