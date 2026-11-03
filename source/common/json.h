/*
 * Distro Buny — small JSON reader/writer
 *
 * The reader builds a DOM for small documents (settings, catalog index,
 * compatibility records).  Input is untrusted: depth and size are bounded and
 * every malformed construct is rejected with an error message.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BUNY_JSON_H
#define BUNY_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common/strbuf.h"

enum json_type { JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT };

struct json {
	enum json_type type;
	bool b;
	double num;
	char *str;                 /* JSON_STRING */
	struct json **items;       /* JSON_ARRAY / JSON_OBJECT values */
	char **keys;               /* JSON_OBJECT keys */
	size_t n;
};

struct json *json_parse(const char *text, size_t len, char *err, size_t errlen);
void json_free(struct json *j);
const struct json *json_get(const struct json *obj, const char *key);
const char *json_get_str(const struct json *obj, const char *key, const char *def);
double json_get_num(const struct json *obj, const char *key, double def);
bool json_get_bool(const struct json *obj, const char *key, bool def);

/* Writer helpers: produce compact or indented JSON into a strbuf. */
#define JSON_WRITER_MAX_DEPTH 32
struct json_writer {
	struct strbuf *sb;
	int depth;
	bool need_comma[JSON_WRITER_MAX_DEPTH];
	bool in_obj[JSON_WRITER_MAX_DEPTH];
	bool pretty;
};
void jw_init(struct json_writer *w, struct strbuf *sb, bool pretty);
void jw_obj_begin(struct json_writer *w);
void jw_obj_end(struct json_writer *w);
void jw_arr_begin(struct json_writer *w);
void jw_arr_end(struct json_writer *w);
void jw_key(struct json_writer *w, const char *key);
void jw_str(struct json_writer *w, const char *s); /* NULL -> null */
void jw_num(struct json_writer *w, double v);
void jw_int(struct json_writer *w, long long v);
void jw_bool(struct json_writer *w, bool v);
void jw_null(struct json_writer *w);
void json_escape(struct strbuf *sb, const char *s);

#endif
