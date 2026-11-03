/*
 * Distro Buny — identification rules
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "iso-manager/rules.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/log.h"

static const char *src_names[RS_COUNT] = { "label", "file", "info", "treeinfo", "app", "title", "folder" };

static int src_from_name(const char *s)
{
	for (int i = 0; i < RS_COUNT; i++)
		if (strcmp(s, src_names[i]) == 0)
			return i;
	return -1;
}

static void free_patterns(struct rule_pattern *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		regfree(&p[i].re);
		free(p[i].text);
	}
	free(p);
}

void rules_free(struct ruleset *rs)
{
	for (size_t i = 0; i < rs->n; i++) {
		struct rule *r = &rs->v[i];
		free(r->id);
		free(r->name);
		free(r->icon);
		free(r->vendor);
		free(r->variant);
		free(r->description);
		free_patterns(r->match, r->nmatch);
		free_patterns(r->version, r->nversion);
		free_patterns(r->variant_re, r->nvariant_re);
	}
	free(rs->v);
	rs->v = NULL;
	rs->n = 0;
}

static int add_pattern(struct rule_pattern **arr, size_t *n, int src, const char *text, char *err, size_t errlen, int line)
{
	struct rule_pattern p;
	p.src = (enum rule_src)src;
	int rc = regcomp(&p.re, text, REG_EXTENDED | REG_ICASE);
	if (rc != 0) {
		char msg[128];
		regerror(rc, &p.re, msg, sizeof(msg));
		snprintf(err, errlen, "line %d: bad regex '%s': %s", line, text, msg);
		return -1;
	}
	p.text = xstrdup(text);
	*arr = xrealloc(*arr, (*n + 1) * sizeof(struct rule_pattern));
	(*arr)[(*n)++] = p;
	return 0;
}

int rules_load_text(struct ruleset *rs, const char *text, char *err, size_t errlen)
{
	char *dup = xstrdup(text);
	struct rule *cur = NULL;
	int line = 0;
	int rc = 0;
	char *save = NULL;
	for (char *ln = strtok_r(dup, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
		line++;
		char *l = str_trim(ln);
		if (!*l || *l == '#' || *l == ';')
			continue;
		if (*l == '[') {
			char *e = strchr(l, ']');
			if (!e) {
				snprintf(err, errlen, "line %d: unterminated section", line);
				rc = -1;
				break;
			}
			*e = '\0';
			rs->v = xrealloc(rs->v, (rs->n + 1) * sizeof(struct rule));
			cur = &rs->v[rs->n++];
			memset(cur, 0, sizeof(*cur));
			const char *id = l + 1;
			if (str_starts(id, "folder:")) {
				cur->folder = true;
				id += 7;
			}
			cur->id = xstrdup(id);
			continue;
		}
		if (!cur)
			continue;
		char *eq = strchr(l, '=');
		if (!eq)
			continue;
		*eq = '\0';
		char *key = str_trim(l);
		char *val = str_trim(eq + 1);
		if (!strcmp(key, "name")) {
			free(cur->name);
			cur->name = xstrdup(val);
		} else if (!strcmp(key, "icon")) {
			free(cur->icon);
			cur->icon = xstrdup(val);
		} else if (!strcmp(key, "vendor")) {
			free(cur->vendor);
			cur->vendor = xstrdup(val);
		} else if (!strcmp(key, "variant")) {
			free(cur->variant);
			cur->variant = xstrdup(val);
		} else if (!strcmp(key, "rolling")) {
			cur->rolling = !strcmp(val, "yes") || !strcmp(val, "true") || !strcmp(val, "1");
		} else if (!strcmp(key, "description")) {
			free(cur->description);
			cur->description = xstrdup(val);
		} else if (str_starts(key, "match.") || str_starts(key, "version.") || str_starts(key, "variant.")) {
			char *dot = strchr(key, '.');
			int src = src_from_name(dot + 1);
			if (src < 0) {
				snprintf(err, errlen, "line %d: unknown source '%s'", line, dot + 1);
				rc = -1;
				break;
			}
			int r;
			if (key[0] == 'm')
				r = add_pattern(&cur->match, &cur->nmatch, src, val, err, errlen, line);
			else if (key[1] == 'e')
				r = add_pattern(&cur->version, &cur->nversion, src, val, err, errlen, line);
			else
				r = add_pattern(&cur->variant_re, &cur->nvariant_re, src, val, err, errlen, line);
			if (r < 0) {
				rc = -1;
				break;
			}
		}
	}
	free(dup);
	return rc;
}

int rules_load(struct ruleset *rs, const char *path, char *err, size_t errlen)
{
	char *text = read_file(path, 4u << 20, NULL);
	if (!text) {
		snprintf(err, errlen, "cannot read %s", path);
		return -1;
	}
	int rc = rules_load_text(rs, text, err, errlen);
	free(text);
	return rc;
}

static bool pattern_hits(const struct rule_pattern *p, const char *const src[RS_COUNT])
{
	const char *s = src[p->src];
	return s && *s && regexec(&p->re, s, 0, NULL, 0) == 0;
}

const struct rule *rules_match(const struct ruleset *rs, const char *const src[RS_COUNT])
{
	for (size_t i = 0; i < rs->n; i++) {
		const struct rule *r = &rs->v[i];
		if (r->folder)
			continue;
		for (size_t k = 0; k < r->nmatch; k++)
			if (pattern_hits(&r->match[k], src))
				return r;
	}
	return NULL;
}

const struct rule *rules_match_folder(const struct ruleset *rs, const char *folder_name, bool *whole)
{
	*whole = false;
	if (!folder_name || !*folder_name)
		return NULL;
	for (size_t i = 0; i < rs->n; i++) {
		const struct rule *r = &rs->v[i];
		for (size_t k = 0; k < r->nmatch; k++) {
			regmatch_t m[1];
			if (r->match[k].src != RS_FOLDER || regexec(&r->match[k].re, folder_name, 1, m, 0) != 0)
				continue;
			*whole = m[0].rm_so == 0 && (size_t)m[0].rm_eo == strlen(folder_name);
			return r;
		}
	}
	return NULL;
}

char *rules_extract(const struct rule_pattern *pats, size_t n, const char *const src[RS_COUNT])
{
	for (size_t i = 0; i < n; i++) {
		const char *s = src[pats[i].src];
		if (!s || !*s)
			continue;
		regmatch_t m[4];
		if (regexec(&pats[i].re, s, 4, m, 0) != 0)
			continue;
		int g = pats[i].re.re_nsub >= 1 ? 1 : 0;
		if (m[g].rm_so < 0)
			continue;
		char *v = xstrndup(s + m[g].rm_so, (size_t)(m[g].rm_eo - m[g].rm_so));
		char *t = str_trim(v);
		if (!*t) {
			free(v);
			continue;
		}
		char *out = xstrdup(t);
		free(v);
		return out;
	}
	return NULL;
}
