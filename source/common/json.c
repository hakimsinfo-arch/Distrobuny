/*
 * Distro Buny — small JSON reader/writer
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "common/json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSON_MAX_DEPTH 48

struct parser {
	const char *start, *p, *end;
	char *err;
	size_t errlen;
	int depth;
};

static void perr(struct parser *ps, const char *msg)
{
	if (ps->err && ps->errlen && !ps->err[0])
		snprintf(ps->err, ps->errlen, "%s at offset %ld", msg, (long)(ps->p - ps->start));
}

static void skip_ws(struct parser *ps)
{
	while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
		ps->p++;
}

static struct json *jnew(enum json_type t)
{
	struct json *j = xcalloc(1, sizeof(*j));
	j->type = t;
	return j;
}

static void put_utf8(struct strbuf *sb, uint32_t cp)
{
	char b[4];
	if (cp < 0x80) {
		sb_addc(sb, (char)cp);
	} else if (cp < 0x800) {
		b[0] = (char)(0xC0 | (cp >> 6));
		b[1] = (char)(0x80 | (cp & 0x3F));
		sb_addn(sb, b, 2);
	} else if (cp < 0x10000) {
		b[0] = (char)(0xE0 | (cp >> 12));
		b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		b[2] = (char)(0x80 | (cp & 0x3F));
		sb_addn(sb, b, 3);
	} else {
		b[0] = (char)(0xF0 | (cp >> 18));
		b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
		b[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
		b[3] = (char)(0x80 | (cp & 0x3F));
		sb_addn(sb, b, 4);
	}
}

static int hex4(const char *s, uint32_t *out)
{
	uint32_t v = 0;
	for (int i = 0; i < 4; i++) {
		char c = s[i];
		v <<= 4;
		if (c >= '0' && c <= '9')
			v |= (uint32_t)(c - '0');
		else if (c >= 'a' && c <= 'f')
			v |= (uint32_t)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F')
			v |= (uint32_t)(c - 'A' + 10);
		else
			return -1;
	}
	*out = v;
	return 0;
}

static char *parse_string_raw(struct parser *ps)
{
	if (ps->p >= ps->end || *ps->p != '"') {
		perr(ps, "expected string");
		return NULL;
	}
	ps->p++;
	struct strbuf sb = STRBUF_INIT;
	while (ps->p < ps->end && *ps->p != '"') {
		unsigned char c = (unsigned char)*ps->p;
		if (c < 0x20) {
			perr(ps, "control character in string");
			sb_free(&sb);
			return NULL;
		}
		if (c == '\\') {
			ps->p++;
			if (ps->p >= ps->end)
				break;
			char e = *ps->p++;
			switch (e) {
			case '"': sb_addc(&sb, '"'); break;
			case '\\': sb_addc(&sb, '\\'); break;
			case '/': sb_addc(&sb, '/'); break;
			case 'b': sb_addc(&sb, '\b'); break;
			case 'f': sb_addc(&sb, '\f'); break;
			case 'n': sb_addc(&sb, '\n'); break;
			case 'r': sb_addc(&sb, '\r'); break;
			case 't': sb_addc(&sb, '\t'); break;
			case 'u': {
				uint32_t cp;
				if (ps->end - ps->p < 4 || hex4(ps->p, &cp) < 0) {
					perr(ps, "bad \\u escape");
					sb_free(&sb);
					return NULL;
				}
				ps->p += 4;
				if (cp >= 0xD800 && cp <= 0xDBFF && ps->end - ps->p >= 6 && ps->p[0] == '\\' && ps->p[1] == 'u') {
					uint32_t lo;
					if (hex4(ps->p + 2, &lo) == 0 && lo >= 0xDC00 && lo <= 0xDFFF) {
						cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
						ps->p += 6;
					}
				}
				put_utf8(&sb, cp);
				break;
			}
			default:
				perr(ps, "bad escape");
				sb_free(&sb);
				return NULL;
			}
			continue;
		}
		sb_addc(&sb, (char)c);
		ps->p++;
	}
	if (ps->p >= ps->end) {
		perr(ps, "unterminated string");
		sb_free(&sb);
		return NULL;
	}
	ps->p++; /* closing quote */
	return sb_detach(&sb);
}

static struct json *parse_value(struct parser *ps);

static struct json *parse_array(struct parser *ps)
{
	struct json *j = jnew(JSON_ARRAY);
	size_t cap = 0;
	ps->p++;
	skip_ws(ps);
	if (ps->p < ps->end && *ps->p == ']') {
		ps->p++;
		return j;
	}
	for (;;) {
		struct json *v = parse_value(ps);
		if (!v) {
			json_free(j);
			return NULL;
		}
		if (j->n == cap) {
			cap = cap ? cap * 2 : 8;
			j->items = xrealloc(j->items, cap * sizeof(*j->items));
		}
		j->items[j->n++] = v;
		skip_ws(ps);
		if (ps->p < ps->end && *ps->p == ',') {
			ps->p++;
			continue;
		}
		if (ps->p < ps->end && *ps->p == ']') {
			ps->p++;
			return j;
		}
		perr(ps, "expected , or ]");
		json_free(j);
		return NULL;
	}
}

static struct json *parse_object(struct parser *ps)
{
	struct json *j = jnew(JSON_OBJECT);
	size_t cap = 0;
	ps->p++;
	skip_ws(ps);
	if (ps->p < ps->end && *ps->p == '}') {
		ps->p++;
		return j;
	}
	for (;;) {
		skip_ws(ps);
		char *key = parse_string_raw(ps);
		if (!key) {
			json_free(j);
			return NULL;
		}
		skip_ws(ps);
		if (ps->p >= ps->end || *ps->p != ':') {
			free(key);
			perr(ps, "expected :");
			json_free(j);
			return NULL;
		}
		ps->p++;
		struct json *v = parse_value(ps);
		if (!v) {
			free(key);
			json_free(j);
			return NULL;
		}
		if (j->n == cap) {
			cap = cap ? cap * 2 : 8;
			j->items = xrealloc(j->items, cap * sizeof(*j->items));
			j->keys = xrealloc(j->keys, cap * sizeof(*j->keys));
		}
		j->keys[j->n] = key;
		j->items[j->n++] = v;
		skip_ws(ps);
		if (ps->p < ps->end && *ps->p == ',') {
			ps->p++;
			continue;
		}
		if (ps->p < ps->end && *ps->p == '}') {
			ps->p++;
			return j;
		}
		perr(ps, "expected , or }");
		json_free(j);
		return NULL;
	}
}

static struct json *parse_value(struct parser *ps)
{
	skip_ws(ps);
	if (ps->p >= ps->end) {
		perr(ps, "unexpected end");
		return NULL;
	}
	if (++ps->depth > JSON_MAX_DEPTH) {
		perr(ps, "nesting too deep");
		return NULL;
	}
	struct json *r = NULL;
	char c = *ps->p;
	if (c == '{') {
		r = parse_object(ps);
	} else if (c == '[') {
		r = parse_array(ps);
	} else if (c == '"') {
		char *s = parse_string_raw(ps);
		if (s) {
			r = jnew(JSON_STRING);
			r->str = s;
		}
	} else if (ps->end - ps->p >= 4 && !memcmp(ps->p, "true", 4)) {
		ps->p += 4;
		r = jnew(JSON_BOOL);
		r->b = true;
	} else if (ps->end - ps->p >= 5 && !memcmp(ps->p, "false", 5)) {
		ps->p += 5;
		r = jnew(JSON_BOOL);
	} else if (ps->end - ps->p >= 4 && !memcmp(ps->p, "null", 4)) {
		ps->p += 4;
		r = jnew(JSON_NULL);
	} else if (c == '-' || (c >= '0' && c <= '9')) {
		char tmp[64];
		size_t n = 0;
		while (ps->p < ps->end && n < sizeof(tmp) - 1 &&
		       (strchr("+-.eE", *ps->p) || (*ps->p >= '0' && *ps->p <= '9')))
			tmp[n++] = *ps->p++;
		tmp[n] = '\0';
		char *endp;
		double v = strtod(tmp, &endp);
		if (*endp || !isfinite(v)) {
			perr(ps, "bad number");
		} else {
			r = jnew(JSON_NUMBER);
			r->num = v;
		}
	} else {
		perr(ps, "unexpected character");
	}
	ps->depth--;
	return r;
}

struct json *json_parse(const char *text, size_t len, char *err, size_t errlen)
{
	struct parser ps = { text, text, text + len, err, errlen, 0 };
	if (err && errlen)
		err[0] = '\0';
	struct json *j = parse_value(&ps);
	if (!j)
		return NULL;
	skip_ws(&ps);
	if (ps.p != ps.end) {
		perr(&ps, "trailing data");
		json_free(j);
		return NULL;
	}
	return j;
}

void json_free(struct json *j)
{
	if (!j)
		return;
	for (size_t i = 0; i < j->n; i++) {
		json_free(j->items[i]);
		if (j->keys)
			free(j->keys[i]);
	}
	free(j->items);
	free(j->keys);
	free(j->str);
	free(j);
}

const struct json *json_get(const struct json *obj, const char *key)
{
	if (!obj || obj->type != JSON_OBJECT)
		return NULL;
	for (size_t i = 0; i < obj->n; i++)
		if (strcmp(obj->keys[i], key) == 0)
			return obj->items[i];
	return NULL;
}

const char *json_get_str(const struct json *obj, const char *key, const char *def)
{
	const struct json *v = json_get(obj, key);
	return v && v->type == JSON_STRING ? v->str : def;
}

double json_get_num(const struct json *obj, const char *key, double def)
{
	const struct json *v = json_get(obj, key);
	return v && v->type == JSON_NUMBER ? v->num : def;
}

bool json_get_bool(const struct json *obj, const char *key, bool def)
{
	const struct json *v = json_get(obj, key);
	return v && v->type == JSON_BOOL ? v->b : def;
}

/* ---------------------------------------------------------------- writer */

void json_escape(struct strbuf *sb, const char *s)
{
	sb_addc(sb, '"');
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		switch (*p) {
		case '"': sb_add(sb, "\\\""); break;
		case '\\': sb_add(sb, "\\\\"); break;
		case '\n': sb_add(sb, "\\n"); break;
		case '\r': sb_add(sb, "\\r"); break;
		case '\t': sb_add(sb, "\\t"); break;
		default:
			if (*p < 0x20)
				sb_addf(sb, "\\u%04x", *p);
			else
				sb_addc(sb, (char)*p);
		}
	}
	sb_addc(sb, '"');
}

void jw_init(struct json_writer *w, struct strbuf *sb, bool pretty)
{
	memset(w, 0, sizeof(*w));
	w->sb = sb;
	w->pretty = pretty;
}

static void jw_indent(struct json_writer *w)
{
	if (!w->pretty)
		return;
	sb_addc(w->sb, '\n');
	for (int i = 0; i < w->depth; i++)
		sb_add(w->sb, "  ");
}

/* Called before an array element or an object key: separator + indentation. */
static void jw_item(struct json_writer *w)
{
	if (w->need_comma[w->depth])
		sb_addc(w->sb, ',');
	w->need_comma[w->depth] = true;
	jw_indent(w);
}

/* Values inside objects follow their key directly; array values need jw_item. */
static void jw_value_prefix(struct json_writer *w)
{
	if (w->depth > 0 && !w->in_obj[w->depth])
		jw_item(w);
}

static void jw_push(struct json_writer *w, bool is_obj)
{
	if (w->depth < JSON_WRITER_MAX_DEPTH - 1)
		w->depth++;
	w->need_comma[w->depth] = false;
	w->in_obj[w->depth] = is_obj;
}

void jw_obj_begin(struct json_writer *w)
{
	jw_value_prefix(w);
	sb_addc(w->sb, '{');
	jw_push(w, true);
}

void jw_obj_end(struct json_writer *w)
{
	bool had = w->need_comma[w->depth];
	w->depth--;
	if (had)
		jw_indent(w);
	sb_addc(w->sb, '}');
}

void jw_arr_begin(struct json_writer *w)
{
	jw_value_prefix(w);
	sb_addc(w->sb, '[');
	jw_push(w, false);
}

void jw_arr_end(struct json_writer *w)
{
	bool had = w->need_comma[w->depth];
	w->depth--;
	if (had)
		jw_indent(w);
	sb_addc(w->sb, ']');
}

void jw_key(struct json_writer *w, const char *key)
{
	jw_item(w);
	json_escape(w->sb, key);
	sb_add(w->sb, w->pretty ? ": " : ":");
}

void jw_str(struct json_writer *w, const char *s)
{
	jw_value_prefix(w);
	if (s)
		json_escape(w->sb, s);
	else
		sb_add(w->sb, "null");
}

void jw_num(struct json_writer *w, double v)
{
	jw_value_prefix(w);
	if (v == (double)(long long)v && fabs(v) < 9e15)
		sb_addf(w->sb, "%lld", (long long)v);
	else
		sb_addf(w->sb, "%.17g", v);
}

void jw_int(struct json_writer *w, long long v)
{
	jw_value_prefix(w);
	sb_addf(w->sb, "%lld", v);
}

void jw_bool(struct json_writer *w, bool v)
{
	jw_value_prefix(w);
	sb_add(w->sb, v ? "true" : "false");
}

void jw_null(struct json_writer *w)
{
	jw_value_prefix(w);
	sb_add(w->sb, "null");
}
