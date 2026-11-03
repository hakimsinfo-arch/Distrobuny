/*
 * Distro Buny — sandboxed GRUB script interpreter
 *
 * Evaluates the subset of GRUB's scripting language used by distribution boot
 * menus: variables and expansion, quoting, if/elif/else, for (with globbing
 * against the ISO), while/until, functions, menuentry/submenu, source and
 * configfile, search, probe, regexp, test/[ ].  Nothing is executed on the
 * host: every command is interpreted against an in-memory model in which the
 * ISO is the device "isodev" and the Distro Buny data partition is "datadev".
 * Menu entry bodies are evaluated afterwards to capture their linux/initrd
 * commands, exactly as GRUB evaluates them when an entry is selected.
 *
 * Input is untrusted, so recursion depth, total steps, variable sizes and
 * entry counts are bounded.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "boot-adapters/bootcfg.h"

#include <ctype.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/log.h"
#include "common/strbuf.h"

#define MAX_DEPTH 24
#define DEFAULT_MAX_STEPS 200000
#define MAX_ENTRIES 512
#define MAX_VAR_LEN 65536
#define MAX_CFG_SIZE (2u << 20)

/* ================================================================ lexer */

enum part_kind { P_LIT, P_VAR };

struct part {
	enum part_kind kind;
	bool quoted;
	char *text;
};

struct word {
	struct part *parts;
	size_t n;
	bool any_quote; /* contained quotes: expands to a word even when empty */
};

enum tok_kind { T_WORD, T_SEP, T_LBRACE, T_RBRACE, T_EOF };

struct token {
	enum tok_kind kind;
	struct word w;
	int line;
};

struct lexer {
	const char *s;
	size_t i, n;
	int line;
};

static void word_add(struct word *w, enum part_kind k, bool quoted, const char *text, size_t len)
{
	/* merge consecutive literal parts with the same quoting */
	if (k == P_LIT && w->n && w->parts[w->n - 1].kind == P_LIT && w->parts[w->n - 1].quoted == quoted) {
		struct part *p = &w->parts[w->n - 1];
		size_t ol = strlen(p->text);
		p->text = xrealloc(p->text, ol + len + 1);
		memcpy(p->text + ol, text, len);
		p->text[ol + len] = '\0';
		return;
	}
	w->parts = xrealloc(w->parts, (w->n + 1) * sizeof(struct part));
	w->parts[w->n].kind = k;
	w->parts[w->n].quoted = quoted;
	w->parts[w->n].text = xstrndup(text, len);
	w->n++;
}

static void word_free(struct word *w)
{
	for (size_t i = 0; i < w->n; i++)
		free(w->parts[i].text);
	free(w->parts);
	w->parts = NULL;
	w->n = 0;
}

static bool is_special(char c)
{
	return c == '{' || c == '}' || c == ';' || c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
	       c == '\'' || c == '"' || c == '\\' || c == '$' || c == '|' || c == '&' || c == '<' || c == '>';
}

/* Parse "$name", "${name}", "$1", "$?" ... at lx->s[lx->i] == '$'. */
static bool lex_var(struct lexer *lx, struct word *w, bool quoted)
{
	size_t i = lx->i + 1;
	if (i >= lx->n)
		return false;
	if (lx->s[i] == '{') {
		size_t j = i + 1;
		while (j < lx->n && lx->s[j] != '}' && lx->s[j] != '\n')
			j++;
		if (j >= lx->n || lx->s[j] != '}')
			return false;
		word_add(w, P_VAR, quoted, lx->s + i + 1, j - i - 1);
		lx->i = j + 1;
		return true;
	}
	if (strchr("?#@*", lx->s[i])) {
		word_add(w, P_VAR, quoted, lx->s + i, 1);
		lx->i = i + 1;
		return true;
	}
	size_t j = i;
	if (isdigit((unsigned char)lx->s[j])) {
		while (j < lx->n && isdigit((unsigned char)lx->s[j]))
			j++;
	} else {
		while (j < lx->n && (isalnum((unsigned char)lx->s[j]) || lx->s[j] == '_'))
			j++;
	}
	if (j == i)
		return false;
	word_add(w, P_VAR, quoted, lx->s + i, j - i);
	lx->i = j;
	return true;
}

static int lex_next(struct lexer *lx, struct token *t)
{
	memset(t, 0, sizeof(*t));
	for (;;) {
		while (lx->i < lx->n && (lx->s[lx->i] == ' ' || lx->s[lx->i] == '\t' || lx->s[lx->i] == '\r'))
			lx->i++;
		if (lx->i + 1 < lx->n && lx->s[lx->i] == '\\' && lx->s[lx->i + 1] == '\n') {
			lx->i += 2;
			lx->line++;
			continue;
		}
		if (lx->i < lx->n && lx->s[lx->i] == '#') {
			while (lx->i < lx->n && lx->s[lx->i] != '\n')
				lx->i++;
			continue;
		}
		break;
	}
	t->line = lx->line;
	if (lx->i >= lx->n) {
		t->kind = T_EOF;
		return 0;
	}
	char c = lx->s[lx->i];
	if (c == '\n' || c == ';') {
		if (c == '\n')
			lx->line++;
		lx->i++;
		t->kind = T_SEP;
		return 0;
	}
	if (c == '{') {
		lx->i++;
		t->kind = T_LBRACE;
		return 0;
	}
	if (c == '}') {
		lx->i++;
		t->kind = T_RBRACE;
		return 0;
	}
	t->kind = T_WORD;
	struct word *w = &t->w;
	while (lx->i < lx->n) {
		c = lx->s[lx->i];
		if (c == '\'') {
			size_t j = lx->i + 1;
			while (j < lx->n && lx->s[j] != '\'')
				j++;
			if (j >= lx->n)
				return -1;
			word_add(w, P_LIT, true, lx->s + lx->i + 1, j - lx->i - 1);
			w->any_quote = true;
			for (size_t k = lx->i; k < j; k++)
				if (lx->s[k] == '\n')
					lx->line++;
			lx->i = j + 1;
		} else if (c == '"') {
			w->any_quote = true;
			lx->i++;
			bool closed = false;
			while (lx->i < lx->n) {
				char d = lx->s[lx->i];
				if (d == '"') {
					lx->i++;
					closed = true;
					break;
				}
				if (d == '\\' && lx->i + 1 < lx->n && strchr("$\"\\\n", lx->s[lx->i + 1])) {
					if (lx->s[lx->i + 1] != '\n')
						word_add(w, P_LIT, true, lx->s + lx->i + 1, 1);
					lx->i += 2;
					continue;
				}
				if (d == '$' && lex_var(lx, w, true))
					continue;
				if (d == '\n')
					lx->line++;
				word_add(w, P_LIT, true, &d, 1);
				lx->i++;
			}
			if (!closed)
				return -1;
			if (w->n == 0)
				word_add(w, P_LIT, true, "", 0);
		} else if (c == '\\') {
			if (lx->i + 1 < lx->n) {
				if (lx->s[lx->i + 1] == '\n') {
					lx->line++;
				} else {
					word_add(w, P_LIT, true, lx->s + lx->i + 1, 1);
				}
				lx->i += 2;
			} else {
				lx->i++;
			}
		} else if (c == '$') {
			if (!lex_var(lx, w, false)) {
				word_add(w, P_LIT, false, "$", 1);
				lx->i++;
			}
		} else if (c == '|' || c == '&' || c == '<' || c == '>') {
			/* unsupported operators in GRUB: keep literally */
			word_add(w, P_LIT, false, &c, 1);
			lx->i++;
		} else if (is_special(c)) {
			break;
		} else {
			size_t j = lx->i;
			while (j < lx->n && !is_special(lx->s[j]))
				j++;
			word_add(w, P_LIT, false, lx->s + lx->i, j - lx->i);
			lx->i = j;
		}
	}
	return 0;
}

/* ================================================================ parser */

enum node_kind { N_CMD, N_IF, N_FOR, N_WHILE, N_UNTIL, N_FUNC, N_MENU, N_SUBMENU };

struct node;
struct nodelist {
	struct node **v;
	size_t n;
};

struct node {
	enum node_kind kind;
	int line;
	struct word *words; /* CMD argv; MENU/SUBMENU header; FOR var+list; FUNC name */
	size_t nwords;
	struct nodelist body;
	/* if/elif chains */
	struct nodelist *conds;
	struct nodelist *bodies;
	size_t nbranch;
	struct nodelist else_body;
	bool has_else;
};

struct parser {
	struct lexer lx;
	struct token cur;
	bool have;
	char err[160];
};

static void node_free(struct node *nd);

static void nodelist_free(struct nodelist *l)
{
	for (size_t i = 0; i < l->n; i++)
		node_free(l->v[i]);
	free(l->v);
	l->v = NULL;
	l->n = 0;
}

static void node_free(struct node *nd)
{
	if (!nd)
		return;
	for (size_t i = 0; i < nd->nwords; i++)
		word_free(&nd->words[i]);
	free(nd->words);
	nodelist_free(&nd->body);
	for (size_t i = 0; i < nd->nbranch; i++) {
		if (nd->conds)
			nodelist_free(&nd->conds[i]);
		if (nd->bodies) /* while/until loops have conditions only */
			nodelist_free(&nd->bodies[i]);
	}
	free(nd->conds);
	free(nd->bodies);
	nodelist_free(&nd->else_body);
	free(nd);
}

static void nodelist_push(struct nodelist *l, struct node *nd)
{
	l->v = xrealloc(l->v, (l->n + 1) * sizeof(struct node *));
	l->v[l->n++] = nd;
}

static struct token *peek(struct parser *p)
{
	if (!p->have) {
		if (lex_next(&p->lx, &p->cur) < 0) {
			snprintf(p->err, sizeof(p->err), "line %d: unterminated quote", p->lx.line);
			p->cur.kind = T_EOF;
		}
		p->have = true;
	}
	return &p->cur;
}

static void consume(struct parser *p)
{
	p->have = false; /* ownership of word moved out or freed by caller */
}

static void drop(struct parser *p)
{
	word_free(&p->cur.w);
	p->have = false;
}

/* A word that is a single unquoted literal equal to kw. */
static bool word_is(const struct word *w, const char *kw)
{
	return w->n == 1 && w->parts[0].kind == P_LIT && !w->parts[0].quoted && strcmp(w->parts[0].text, kw) == 0;
}

static bool tok_is_kw(struct parser *p, const char *kw)
{
	struct token *t = peek(p);
	return t->kind == T_WORD && word_is(&t->w, kw);
}

static void skip_seps(struct parser *p)
{
	while (peek(p)->kind == T_SEP)
		consume(p);
}

typedef bool (*stop_fn)(struct parser *p);

static struct node *parse_statement(struct parser *p);

static void parse_list(struct parser *p, struct nodelist *out, stop_fn stop)
{
	for (;;) {
		skip_seps(p);
		struct token *t = peek(p);
		if (t->kind == T_EOF || p->err[0])
			return;
		if (stop(p))
			return;
		if (t->kind == T_RBRACE)
			return;
		struct node *nd = parse_statement(p);
		if (!nd)
			return;
		nodelist_push(out, nd);
	}
}

static bool stop_then(struct parser *p) { return tok_is_kw(p, "then"); }
static bool stop_if_body(struct parser *p) { return tok_is_kw(p, "elif") || tok_is_kw(p, "else") || tok_is_kw(p, "fi"); }
static bool stop_fi(struct parser *p) { return tok_is_kw(p, "fi"); }
static bool stop_do(struct parser *p) { return tok_is_kw(p, "do"); }
static bool stop_done(struct parser *p) { return tok_is_kw(p, "done"); }
static bool stop_never(struct parser *p) { BUNY_UNUSED(p); return false; }

static bool expect_kw(struct parser *p, const char *kw)
{
	skip_seps(p);
	if (!tok_is_kw(p, kw)) {
		if (!p->err[0])
			snprintf(p->err, sizeof(p->err), "line %d: expected '%s'", peek(p)->line, kw);
		return false;
	}
	drop(p);
	return true;
}

static bool parse_block(struct parser *p, struct nodelist *body)
{
	skip_seps(p);
	if (peek(p)->kind != T_LBRACE) {
		if (!p->err[0])
			snprintf(p->err, sizeof(p->err), "line %d: expected '{'", peek(p)->line);
		return false;
	}
	consume(p);
	parse_list(p, body, stop_never);
	if (peek(p)->kind != T_RBRACE) {
		if (!p->err[0])
			snprintf(p->err, sizeof(p->err), "line %d: expected '}'", peek(p)->line);
		return false;
	}
	consume(p);
	return true;
}

static void push_word(struct node *nd, struct word w)
{
	nd->words = xrealloc(nd->words, (nd->nwords + 1) * sizeof(struct word));
	nd->words[nd->nwords++] = w;
}

static struct node *parse_statement(struct parser *p)
{
	struct token *t = peek(p);
	struct node *nd = xcalloc(1, sizeof(*nd));
	nd->line = t->line;
	if (t->kind == T_LBRACE) {
		/* bare block: treat as inline list */
		nd->kind = N_CMD;
		consume(p);
		parse_list(p, &nd->body, stop_never);
		if (peek(p)->kind == T_RBRACE)
			consume(p);
		return nd;
	}
	if (t->kind != T_WORD) {
		free(nd);
		consume(p);
		return NULL;
	}
	if (word_is(&t->w, "if")) {
		drop(p);
		nd->kind = N_IF;
		for (;;) {
			nd->conds = xrealloc(nd->conds, (nd->nbranch + 1) * sizeof(struct nodelist));
			nd->bodies = xrealloc(nd->bodies, (nd->nbranch + 1) * sizeof(struct nodelist));
			memset(&nd->conds[nd->nbranch], 0, sizeof(struct nodelist));
			memset(&nd->bodies[nd->nbranch], 0, sizeof(struct nodelist));
			nd->nbranch++;
			parse_list(p, &nd->conds[nd->nbranch - 1], stop_then);
			if (!expect_kw(p, "then"))
				return nd;
			parse_list(p, &nd->bodies[nd->nbranch - 1], stop_if_body);
			skip_seps(p);
			if (tok_is_kw(p, "elif")) {
				drop(p);
				continue;
			}
			if (tok_is_kw(p, "else")) {
				drop(p);
				nd->has_else = true;
				parse_list(p, &nd->else_body, stop_fi);
			}
			expect_kw(p, "fi");
			return nd;
		}
	}
	if (word_is(&t->w, "for")) {
		drop(p);
		nd->kind = N_FOR;
		t = peek(p);
		if (t->kind != T_WORD) {
			snprintf(p->err, sizeof(p->err), "line %d: bad for", t->line);
			return nd;
		}
		push_word(nd, t->w);
		consume(p);
		if (!tok_is_kw(p, "in")) {
			snprintf(p->err, sizeof(p->err), "line %d: expected 'in'", peek(p)->line);
			return nd;
		}
		drop(p);
		while (peek(p)->kind == T_WORD) {
			push_word(nd, p->cur.w);
			consume(p);
		}
		if (!expect_kw(p, "do"))
			return nd;
		parse_list(p, &nd->body, stop_done);
		expect_kw(p, "done");
		return nd;
	}
	if (word_is(&t->w, "while") || word_is(&t->w, "until")) {
		nd->kind = word_is(&t->w, "while") ? N_WHILE : N_UNTIL;
		drop(p);
		nd->conds = xcalloc(1, sizeof(struct nodelist));
		nd->nbranch = 1;
		parse_list(p, &nd->conds[0], stop_do);
		if (!expect_kw(p, "do"))
			return nd;
		parse_list(p, &nd->body, stop_done);
		expect_kw(p, "done");
		return nd;
	}
	if (word_is(&t->w, "function")) {
		drop(p);
		nd->kind = N_FUNC;
		t = peek(p);
		if (t->kind != T_WORD) {
			snprintf(p->err, sizeof(p->err), "line %d: bad function", t->line);
			return nd;
		}
		push_word(nd, t->w);
		consume(p);
		parse_block(p, &nd->body);
		return nd;
	}
	if (word_is(&t->w, "menuentry") || word_is(&t->w, "submenu")) {
		nd->kind = word_is(&t->w, "menuentry") ? N_MENU : N_SUBMENU;
		drop(p);
		while (peek(p)->kind == T_WORD) {
			push_word(nd, p->cur.w);
			consume(p);
		}
		parse_block(p, &nd->body);
		return nd;
	}
	/* simple command */
	nd->kind = N_CMD;
	while (peek(p)->kind == T_WORD) {
		push_word(nd, p->cur.w);
		consume(p);
	}
	/* "cmd args {" is not valid GRUB; leave braces to the caller. */
	return nd;
}

static struct nodelist *parse_script(const char *text, size_t len, char *err, size_t errlen)
{
	struct parser p;
	memset(&p, 0, sizeof(p));
	p.lx.s = text;
	p.lx.n = len;
	p.lx.line = 1;
	struct nodelist *l = xcalloc(1, sizeof(*l));
	for (;;) {
		parse_list(&p, l, stop_never);
		if (p.err[0])
			break;
		struct token *t = peek(&p);
		if (t->kind == T_EOF)
			break;
		/* stray '}' or keyword at top level: skip it and continue */
		if (t->kind == T_WORD)
			drop(&p);
		else
			consume(&p);
	}
	if (p.have && p.cur.kind == T_WORD)
		word_free(&p.cur.w);
	if (p.err[0] && err)
		snprintf(err, errlen, "%s", p.err);
	return l;
}

/* ============================================================ interpreter */

struct var {
	char *name, *value;
};

struct env {
	struct var *v;
	size_t n;
};

struct func {
	char *name;
	struct node *def; /* borrowed from a parsed script kept alive */
};

struct pending_menu {
	char *title;
	char *id;
	struct ptrvec args;   /* positional: title + extra args */
	struct node *body;    /* borrowed */
	char *config;
	bool top_level;
};

struct capture {
	char *kernel;
	struct ptrvec initrds;
	struct ptrvec args;
	char *efi;
	bool have_linux;
};

struct interp {
	const struct grub_eval_ctx *ctx;
	struct env env;
	struct ptrvec positional; /* stack of struct ptrvec* */
	struct func *funcs;
	size_t nfuncs;
	struct ptrvec scripts;    /* struct nodelist*: parsed scripts kept alive for menu bodies */
	struct ptrvec names;      /* char*: config file names referenced while evaluating */
	struct pending_menu *menus;
	size_t nmenus;
	struct ptrvec submenu_prefix; /* char* */
	struct capture *cap;      /* non-NULL while evaluating an entry body */
	const char *cur_config;
	unsigned steps, max_steps;
	int depth;
	int status;
	bool aborted;
	bool brk;
	char err[200];
};

static const char *env_get(struct env *e, const char *name)
{
	for (size_t i = 0; i < e->n; i++)
		if (strcmp(e->v[i].name, name) == 0)
			return e->v[i].value;
	return NULL;
}

static void env_set(struct env *e, const char *name, const char *value)
{
	size_t vl = strlen(value);
	char *val = vl > MAX_VAR_LEN ? xstrndup(value, MAX_VAR_LEN) : xstrdup(value);
	for (size_t i = 0; i < e->n; i++)
		if (strcmp(e->v[i].name, name) == 0) {
			free(e->v[i].value);
			e->v[i].value = val;
			return;
		}
	e->v = xrealloc(e->v, (e->n + 1) * sizeof(struct var));
	e->v[e->n].name = xstrdup(name);
	e->v[e->n].value = val;
	e->n++;
}

static void env_unset(struct env *e, const char *name)
{
	for (size_t i = 0; i < e->n; i++)
		if (strcmp(e->v[i].name, name) == 0) {
			free(e->v[i].name);
			free(e->v[i].value);
			e->v[i] = e->v[--e->n];
			return;
		}
}

static void env_free(struct env *e)
{
	for (size_t i = 0; i < e->n; i++) {
		free(e->v[i].name);
		free(e->v[i].value);
	}
	free(e->v);
	e->v = NULL;
	e->n = 0;
}

static void env_copy(struct env *dst, const struct env *src)
{
	dst->v = xcalloc(src->n ? src->n : 1, sizeof(struct var));
	dst->n = src->n;
	for (size_t i = 0; i < src->n; i++) {
		dst->v[i].name = xstrdup(src->v[i].name);
		dst->v[i].value = xstrdup(src->v[i].value);
	}
}

static struct ptrvec *cur_positional(struct interp *in)
{
	return in->positional.n ? in->positional.v[in->positional.n - 1] : NULL;
}

static const char *lookup_var(struct interp *in, const char *name, struct strbuf *tmp)
{
	struct ptrvec *pos = cur_positional(in);
	if (isdigit((unsigned char)name[0])) {
		long idx = strtol(name, NULL, 10);
		if (idx == 0)
			return "";
		if (pos && (size_t)idx <= pos->n)
			return pos->v[idx - 1];
		return "";
	}
	if (strcmp(name, "?") == 0) {
		sb_reset(tmp);
		sb_addf(tmp, "%d", in->status);
		return sb_str(tmp);
	}
	if (strcmp(name, "#") == 0) {
		sb_reset(tmp);
		sb_addf(tmp, "%zu", pos ? pos->n : 0);
		return sb_str(tmp);
	}
	if (strcmp(name, "@") == 0 || strcmp(name, "*") == 0) {
		sb_reset(tmp);
		for (size_t i = 0; pos && i < pos->n; i++) {
			if (i)
				sb_addc(tmp, ' ');
			sb_add(tmp, pos->v[i]);
		}
		return sb_str(tmp);
	}
	const char *v = env_get(&in->env, name);
	return v ? v : "";
}

/* Expand a word into zero or more strings.  Like GRUB, the value of an
 * unquoted variable is split into separate words at whitespace. */
static void expand_word(struct interp *in, const struct word *w, struct ptrvec *out)
{
	struct strbuf cur = STRBUF_INIT, tmp = STRBUF_INIT;
	bool have = w->any_quote; /* current word exists even if empty */
#define FLUSH()                                          \
	do {                                             \
		if (have || cur.len)                     \
			ptrvec_push(out, sb_detach(&cur)); \
		have = false;                            \
	} while (0)
	for (size_t i = 0; i < w->n; i++) {
		const struct part *pt = &w->parts[i];
		if (pt->kind == P_LIT) {
			sb_add(&cur, pt->text);
			have = true;
			continue;
		}
		const char *val = lookup_var(in, pt->text, &tmp);
		if (pt->quoted) {
			sb_add(&cur, val);
			have = true;
			continue;
		}
		const char *s = val;
		if (*s && isspace((unsigned char)*s))
			FLUSH();
		bool first = true;
		for (;;) {
			while (*s && isspace((unsigned char)*s))
				s++;
			if (!*s)
				break;
			const char *st = s;
			while (*s && !isspace((unsigned char)*s))
				s++;
			if (!first)
				FLUSH();
			sb_addn(&cur, st, (size_t)(s - st));
			have = true;
			first = false;
		}
		size_t vl = strlen(val);
		if (vl && isspace((unsigned char)val[vl - 1]))
			FLUSH();
	}
	FLUSH();
#undef FLUSH
	sb_free(&cur);
	sb_free(&tmp);
}

static void expand_words(struct interp *in, const struct word *w, size_t n, struct ptrvec *out)
{
	for (size_t i = 0; i < n; i++)
		expand_word(in, &w[i], out);
}

/* True if the word has an unquoted '*' or '?' (glob candidate). */
static bool word_has_glob(const struct word *w)
{
	for (size_t i = 0; i < w->n; i++)
		if (w->parts[i].kind == P_LIT && !w->parts[i].quoted && strpbrk(w->parts[i].text, "*?"))
			return true;
	return false;
}

/* Split "(dev)/path" into device and normalised path. */
static void split_device(const char *arg, char **dev, char **path)
{
	*dev = NULL;
	const char *p = arg;
	if (*p == '(') {
		const char *e = strchr(p, ')');
		if (e) {
			*dev = xstrndup(p + 1, (size_t)(e - p - 1));
			p = e + 1;
		}
	}
	*path = grub_path_normalize(p);
}

char *grub_path_normalize(const char *p)
{
	if (*p == '(') {
		const char *e = strchr(p, ')');
		if (e)
			p = e + 1;
	}
	struct strbuf sb = STRBUF_INIT;
	sb_addc(&sb, '/');
	for (const char *s = p; *s; s++) {
		if (*s == '/' && sb.len && sb.buf[sb.len - 1] == '/')
			continue;
		sb_addc(&sb, *s);
	}
	if (sb.len > 1 && sb.buf[sb.len - 1] == '/')
		sb.buf[--sb.len] = '\0';
	return sb_detach(&sb);
}

static bool dev_is_data(const char *dev)
{
	return dev && strcmp(dev, "datadev") == 0;
}

/* ------------------------------------------------------------ test [ ] */

struct texpr {
	struct interp *in;
	char **a;
	size_t n, i;
};

static bool path_test(struct interp *in, const char *op, const char *arg)
{
	char *dev, *path;
	split_device(arg, &dev, &path);
	bool r = false;
	if (dev_is_data(dev)) {
		const char *ip = in->ctx->iso_path;
		r = ip && strcmp(op, "-d") != 0 && strcmp(path, ip) == 0;
	} else {
		struct iso_file f;
		if (iso_lookup(in->ctx->iso, path, &f) == 0) {
			if (!strcmp(op, "-d"))
				r = f.is_dir;
			else if (!strcmp(op, "-f"))
				r = !f.is_dir;
			else if (!strcmp(op, "-s"))
				r = f.size > 0;
			else
				r = true;
			iso_file_free(&f);
		}
	}
	free(dev);
	free(path);
	return r;
}

static bool texpr_or(struct texpr *t);

static bool texpr_primary(struct texpr *t)
{
	if (t->i >= t->n)
		return false;
	const char *a = t->a[t->i];
	if (!strcmp(a, "(")) {
		t->i++;
		bool r = texpr_or(t);
		if (t->i < t->n && !strcmp(t->a[t->i], ")"))
			t->i++;
		return r;
	}
	if (!strcmp(a, "!")) {
		t->i++;
		return !texpr_primary(t);
	}
	/* binary operator: needs "lhs op rhs" remaining */
	if (t->n - t->i >= 3) {
		const char *op = t->a[t->i + 1];
		static const char *binops[] = { "=", "==", "!=", "<", ">", "<=", ">=", "-eq", "-ne", "-lt",
						"-le", "-gt", "-ge", "-pgt", "-plt", "-nt", "-ot", NULL };
		for (int k = 0; binops[k]; k++) {
			if (strcmp(op, binops[k]) != 0)
				continue;
			const char *l = a, *r = t->a[t->i + 2];
			t->i += 3;
			if (!strcmp(op, "=") || !strcmp(op, "=="))
				return strcmp(l, r) == 0;
			if (!strcmp(op, "!="))
				return strcmp(l, r) != 0;
			if (!strcmp(op, "<"))
				return strcmp(l, r) < 0;
			if (!strcmp(op, ">"))
				return strcmp(l, r) > 0;
			if (!strcmp(op, "<="))
				return strcmp(l, r) <= 0;
			if (!strcmp(op, ">="))
				return strcmp(l, r) >= 0;
			if (!strcmp(op, "-nt") || !strcmp(op, "-ot"))
				return false;
			long long x = strtoll(l, NULL, 10), y = strtoll(r, NULL, 10);
			if (!strcmp(op, "-eq"))
				return x == y;
			if (!strcmp(op, "-ne"))
				return x != y;
			if (!strcmp(op, "-lt") || !strcmp(op, "-plt"))
				return x < y;
			if (!strcmp(op, "-le"))
				return x <= y;
			if (!strcmp(op, "-gt") || !strcmp(op, "-pgt"))
				return x > y;
			return x >= y; /* -ge */
		}
	}
	static const char *unops[] = { "-n", "-z", "-e", "-f", "-d", "-s", "-L", NULL };
	for (int k = 0; unops[k]; k++) {
		if (strcmp(a, unops[k]) == 0 && t->i + 1 < t->n) {
			const char *arg = t->a[t->i + 1];
			t->i += 2;
			if (!strcmp(a, "-n"))
				return arg[0] != '\0';
			if (!strcmp(a, "-z"))
				return arg[0] == '\0';
			if (!strcmp(a, "-L"))
				return false;
			return path_test(t->in, a, arg);
		}
	}
	t->i++;
	return a[0] != '\0';
}

static bool texpr_and(struct texpr *t)
{
	bool r = texpr_primary(t);
	while (t->i < t->n && !strcmp(t->a[t->i], "-a")) {
		t->i++;
		bool r2 = texpr_primary(t);
		r = r && r2;
	}
	return r;
}

static bool texpr_or(struct texpr *t)
{
	bool r = texpr_and(t);
	while (t->i < t->n && !strcmp(t->a[t->i], "-o")) {
		t->i++;
		bool r2 = texpr_and(t);
		r = r || r2;
	}
	return r;
}

/* ------------------------------------------------------------ commands */

static int run_list(struct interp *in, struct nodelist *l);
static int run_script_file(struct interp *in, const char *path);

static int set_assignment(struct interp *in, const char *arg)
{
	const char *eq = strchr(arg, '=');
	if (!eq)
		return -1;
	char *name = xstrndup(arg, (size_t)(eq - arg));
	bool ok = name[0] != '\0';
	for (char *c = name; *c; c++)
		if (!isalnum((unsigned char)*c) && *c != '_')
			ok = false;
	if (ok)
		env_set(&in->env, name, eq + 1);
	free(name);
	return ok ? 0 : -1;
}

static bool is_assignment_word(const char *s)
{
	if (!(isalpha((unsigned char)*s) || *s == '_'))
		return false;
	for (; *s && *s != '='; s++)
		if (!isalnum((unsigned char)*s) && *s != '_')
			return false;
	return *s == '=';
}

/* Parse "--set=VAR", "--set VAR", "-s VAR" style options.  Returns index of next arg. */
static const char *opt_value(char **argv, size_t argc, size_t *i, const char *lng, const char *shrt)
{
	const char *a = argv[*i];
	size_t ll = strlen(lng);
	if (strncmp(a, lng, ll) == 0 && a[ll] == '=')
		return a + ll + 1;
	if ((strcmp(a, lng) == 0 || (shrt && strcmp(a, shrt) == 0)) && *i + 1 < argc) {
		(*i)++;
		return argv[*i];
	}
	return NULL;
}

static int cmd_search(struct interp *in, char **argv, size_t argc)
{
	const char *setvar = "root";
	enum { BY_FILE, BY_LABEL, BY_UUID } mode = BY_FILE;
	const char *target = NULL;
	for (size_t i = 1; i < argc; i++) {
		const char *v;
		if ((v = opt_value(argv, argc, &i, "--set", "-s"))) {
			setvar = v;
			continue;
		}
		const char *a = argv[i];
		if (!strcmp(a, "-f") || !strcmp(a, "--file"))
			mode = BY_FILE;
		else if (!strcmp(a, "-l") || !strcmp(a, "--label"))
			mode = BY_LABEL;
		else if (!strcmp(a, "-u") || !strcmp(a, "--fs-uuid"))
			mode = BY_UUID;
		else if (!strncmp(a, "--hint", 6)) {
			if (!strchr(a, '=') && i + 1 < argc)
				i++;
		} else if (a[0] == '-')
			continue;
		else
			target = a;
	}
	if (!target)
		return 1;
	const char *dev = "isodev";
	const struct iso_info *info = iso_get_info(in->ctx->iso);
	if (mode == BY_FILE) {
		char *d, *p;
		split_device(target, &d, &p);
		if (in->ctx->iso_path && strcmp(p, in->ctx->iso_path) == 0)
			dev = "datadev";
		else if (!iso_exists(in->ctx->iso, p)) {
			free(d);
			free(p);
			return 1;
		}
		free(d);
		free(p);
	} else if (mode == BY_LABEL) {
		if (in->ctx->data_fs_label && strcmp(target, in->ctx->data_fs_label) == 0 && strcmp(target, info->volume_id) != 0)
			dev = "datadev";
	} else {
		if (in->ctx->data_fs_uuid && strcasecmp(target, in->ctx->data_fs_uuid) == 0)
			dev = "datadev";
	}
	env_set(&in->env, setvar, dev);
	return 0;
}

static int cmd_probe(struct interp *in, char **argv, size_t argc)
{
	const char *setvar = NULL, *what = NULL, *device = NULL;
	for (size_t i = 1; i < argc; i++) {
		const char *v;
		if ((v = opt_value(argv, argc, &i, "--set", "-s"))) {
			setvar = v;
			continue;
		}
		const char *a = argv[i];
		if (!strcmp(a, "-u") || !strcmp(a, "--fs-uuid"))
			what = "uuid";
		else if (!strcmp(a, "-l") || !strcmp(a, "--label"))
			what = "label";
		else if (!strcmp(a, "-f") || !strcmp(a, "--fs"))
			what = "fs";
		else if (!strcmp(a, "--part-uuid"))
			what = "partuuid";
		else if (!strcmp(a, "-d") || !strcmp(a, "--driver"))
			what = "driver";
		else if (a[0] != '-')
			device = a;
	}
	if (!what || !device)
		return 1;
	char *dev = xstrdup(device);
	if (dev[0] == '(') {
		char *e = strchr(dev, ')');
		if (e)
			*e = '\0';
		memmove(dev, dev + 1, strlen(dev));
	}
	bool data = strcmp(dev, "datadev") == 0;
	free(dev);
	const struct iso_info *info = iso_get_info(in->ctx->iso);
	const char *val;
	if (!strcmp(what, "uuid"))
		val = data ? (in->ctx->data_fs_uuid ? in->ctx->data_fs_uuid : "") : info->uuid;
	else if (!strcmp(what, "label"))
		val = data ? (in->ctx->data_fs_label ? in->ctx->data_fs_label : "") : info->volume_id;
	else if (!strcmp(what, "fs"))
		val = data ? "exfat" : "iso9660";
	else if (!strcmp(what, "driver"))
		val = data ? "efidisk" : "loopback";
	else
		val = "";
	if (setvar)
		env_set(&in->env, setvar, val);
	return 0;
}

static int cmd_regexp(struct interp *in, char **argv, size_t argc)
{
	struct { int group; const char *var; } sets[8];
	int nsets = 0;
	const char *rx = NULL, *str = NULL;
	for (size_t i = 1; i < argc; i++) {
		const char *v;
		if ((v = opt_value(argv, argc, &i, "--set", "-s"))) {
			if (nsets < 8) {
				const char *colon = strchr(v, ':');
				if (colon && isdigit((unsigned char)v[0])) {
					sets[nsets].group = atoi(v);
					sets[nsets].var = colon + 1;
				} else {
					sets[nsets].group = 1;
					sets[nsets].var = v;
				}
				nsets++;
			}
			continue;
		}
		if (!rx)
			rx = argv[i];
		else if (!str)
			str = argv[i];
	}
	if (!rx || !str)
		return 1;
	regex_t re;
	if (regcomp(&re, rx, REG_EXTENDED) != 0)
		return 1;
	regmatch_t m[10];
	int rc = regexec(&re, str, 10, m, 0) == 0 ? 0 : 1;
	if (rc == 0) {
		for (int k = 0; k < nsets; k++) {
			int g = sets[k].group;
			if (g < 0 || g > 9 || (size_t)g > re.re_nsub)
				g = re.re_nsub ? 1 : 0;
			if (m[g].rm_so >= 0) {
				char *val = xstrndup(str + m[g].rm_so, (size_t)(m[g].rm_eo - m[g].rm_so));
				env_set(&in->env, sets[k].var, val);
				free(val);
			}
		}
	}
	regfree(&re);
	return rc;
}

static void capture_reset(struct capture *c)
{
	free(c->kernel);
	c->kernel = NULL;
	ptrvec_free(&c->args, free);
	ptrvec_free(&c->initrds, free);
	free(c->efi);
	c->efi = NULL;
	c->have_linux = false;
}

static struct func *find_func(struct interp *in, const char *name)
{
	for (size_t i = 0; i < in->nfuncs; i++)
		if (strcmp(in->funcs[i].name, name) == 0)
			return &in->funcs[i];
	return NULL;
}

static int call_with_positional(struct interp *in, struct nodelist *body, char **argv, size_t argc)
{
	struct ptrvec *pos = xcalloc(1, sizeof(*pos));
	for (size_t i = 0; i < argc; i++)
		ptrvec_push(pos, xstrdup(argv[i]));
	ptrvec_push(&in->positional, pos);
	int rc = run_list(in, body);
	in->positional.n--;
	ptrvec_free(pos, free);
	free(pos);
	return rc;
}

static void register_menu(struct interp *in, struct node *nd, char **argv, size_t argc)
{
	/* Entries defined while an entry body runs belong to a nested menu that
	 * GRUB would only show after selection; they are not part of this menu. */
	if (in->nmenus >= MAX_ENTRIES || in->cap)
		return;
	const char *title = NULL;
	const char *id = NULL;
	struct ptrvec extra = { 0 };
	for (size_t i = 0; i < argc; i++) {
		const char *a = argv[i];
		if (!strncmp(a, "--", 2)) {
			const char *eq = strchr(a, '=');
			bool takes_value = !strncmp(a, "--class", 7) || !strncmp(a, "--users", 7) ||
					   !strncmp(a, "--hotkey", 8) || !strncmp(a, "--id", 4) || !strncmp(a, "--source", 8);
			if (!strncmp(a, "--id", 4)) {
				if (eq)
					id = eq + 1;
				else if (i + 1 < argc)
					id = argv[i + 1];
			}
			if (takes_value && !eq)
				i++;
			continue;
		}
		if (!title)
			title = a;
		else
			ptrvec_push(&extra, (void *)a);
	}
	struct pending_menu *m;
	in->menus = xrealloc(in->menus, (in->nmenus + 1) * sizeof(*in->menus));
	m = &in->menus[in->nmenus++];
	memset(m, 0, sizeof(*m));
	struct strbuf t = STRBUF_INIT;
	for (size_t i = 0; i < in->submenu_prefix.n; i++) {
		sb_add(&t, in->submenu_prefix.v[i]);
		sb_add(&t, " › ");
	}
	sb_add(&t, title ? title : "(untitled)");
	m->title = sb_detach(&t);
	m->id = id ? xstrdup(id) : NULL;
	ptrvec_push(&m->args, xstrdup(title ? title : ""));
	for (size_t i = 0; i < extra.n; i++)
		ptrvec_push(&m->args, xstrdup(extra.v[i]));
	free(extra.v);
	m->body = nd;
	m->config = xstrdup(in->cur_config ? in->cur_config : "");
	m->top_level = in->submenu_prefix.n == 0;
}

static int run_command(struct interp *in, struct node *nd)
{
	struct ptrvec av = { 0 };
	expand_words(in, nd->words, nd->nwords, &av);
	int rc = 0;
	if (av.n == 0) {
		/* bare block (parsed as N_CMD with body) */
		if (nd->body.n)
			rc = run_list(in, &nd->body);
		ptrvec_free(&av, free);
		return rc;
	}
	char **argv = (char **)av.v;
	size_t argc = av.n;
	const char *cmd = argv[0];
	bool negate = false;
	if (!strcmp(cmd, "!") && argc > 1) {
		negate = true;
		argv++;
		argc--;
		cmd = argv[0];
	}

	if (is_assignment_word(cmd)) {
		rc = set_assignment(in, cmd) == 0 ? 0 : 1;
	} else if (!strcmp(cmd, "set")) {
		for (size_t i = 1; i < argc; i++) {
			if (strchr(argv[i], '='))
				set_assignment(in, argv[i]);
			else
				env_set(&in->env, argv[i], "");
		}
	} else if (!strcmp(cmd, "unset")) {
		for (size_t i = 1; i < argc; i++)
			env_unset(&in->env, argv[i]);
	} else if (!strcmp(cmd, "[") || !strcmp(cmd, "test")) {
		size_t n = argc - 1;
		if (!strcmp(cmd, "[") && n > 0 && !strcmp(argv[argc - 1], "]"))
			n--;
		struct texpr t = { in, argv + 1, n, 0 };
		rc = texpr_or(&t) ? 0 : 1;
	} else if (!strcmp(cmd, "true") || !strcmp(cmd, ":")) {
		rc = 0;
	} else if (!strcmp(cmd, "false")) {
		rc = 1;
	} else if (!strcmp(cmd, "search") || !strcmp(cmd, "search.file") || !strcmp(cmd, "search.fs_label") ||
		   !strcmp(cmd, "search.fs_uuid")) {
		if (!strcmp(cmd, "search.fs_label") || !strcmp(cmd, "search.fs_uuid") || !strcmp(cmd, "search.file")) {
			/* search.X TARGET [VAR] */
			const char *mode = !strcmp(cmd, "search.file") ? "-f" : !strcmp(cmd, "search.fs_label") ? "-l" : "-u";
			char *nargv[6] = { (char *)"search", (char *)mode, argc > 1 ? argv[1] : (char *)"",
					   (char *)"--set", argc > 2 ? argv[2] : (char *)"root", NULL };
			rc = cmd_search(in, nargv, 5);
		} else {
			rc = cmd_search(in, argv, argc);
		}
	} else if (!strcmp(cmd, "probe")) {
		rc = cmd_probe(in, argv, argc);
	} else if (!strcmp(cmd, "regexp")) {
		rc = cmd_regexp(in, argv, argc);
	} else if (!strcmp(cmd, "source") || !strcmp(cmd, ".") || !strcmp(cmd, "configfile") ||
		   !strcmp(cmd, "extract_entries_source") || !strcmp(cmd, "extract_entries_configfile")) {
		if (argc > 1)
			rc = run_script_file(in, argv[1]);
	} else if (!strcmp(cmd, "linux") || !strcmp(cmd, "linuxefi") || !strcmp(cmd, "linux16") ||
		   !strcmp(cmd, "kernel")) {
		if (in->cap && argc > 1) {
			free(in->cap->kernel);
			ptrvec_free(&in->cap->args, free);
			in->cap->kernel = grub_path_normalize(argv[1]);
			for (size_t i = 2; i < argc; i++)
				ptrvec_push(&in->cap->args, xstrdup(argv[i]));
			in->cap->have_linux = true;
		}
	} else if (!strcmp(cmd, "initrd") || !strcmp(cmd, "initrdefi") || !strcmp(cmd, "initrd16")) {
		if (in->cap) {
			ptrvec_free(&in->cap->initrds, free);
			for (size_t i = 1; i < argc; i++) {
				if (argv[i][0] == '-')
					continue; /* --nounzip etc. */
				ptrvec_push(&in->cap->initrds, grub_path_normalize(argv[i]));
			}
		}
	} else if (!strcmp(cmd, "chainloader")) {
		if (in->cap && argc > 1) {
			size_t i = 1;
			while (i < argc && argv[i][0] == '-')
				i++;
			if (i < argc) {
				free(in->cap->efi);
				in->cap->efi = grub_path_normalize(argv[i]);
			}
		}
	} else if (!strcmp(cmd, "cpuid")) {
		rc = 0; /* x86-64 with long mode and PAE */
	} else if (!strcmp(cmd, "eval")) {
		struct strbuf sb = STRBUF_INIT;
		for (size_t i = 1; i < argc; i++) {
			if (i > 1)
				sb_addc(&sb, ' ');
			sb_add(&sb, argv[i]);
		}
		char perr[160] = "";
		struct nodelist *l = parse_script(sb.buf ? sb.buf : "", sb.len, perr, sizeof(perr));
		ptrvec_push(&in->scripts, l);
		rc = run_list(in, l);
		sb_free(&sb);
	} else if (!strcmp(cmd, "return")) {
		rc = argc > 1 ? atoi(argv[1]) : in->status;
		in->brk = true;
	} else if (!strcmp(cmd, "break") || !strcmp(cmd, "exit")) {
		in->brk = true;
	} else if (!strcmp(cmd, "smbios") || !strcmp(cmd, "videoinfo") || !strcmp(cmd, "hwmatch") ||
		   !strcmp(cmd, "keystatus")) {
		rc = 1;
	} else {
		struct func *f = find_func(in, cmd);
		if (f) {
			if (in->depth < MAX_DEPTH) {
				in->depth++;
				rc = call_with_positional(in, &f->def->body, argv + 1, argc - 1);
				in->depth--;
				in->brk = false;
			}
		} else {
			/* Display, module and terminal commands have no effect on the
			 * boot model: insmod, loadfont, terminal_output, gfxmode, echo,
			 * export, play, sleep, background_image, loopback, ... */
			rc = 0;
		}
	}
	ptrvec_free(&av, free);
	return negate ? !rc : rc;
}

static int run_node(struct interp *in, struct node *nd)
{
	if (in->aborted)
		return 1;
	if (++in->steps > in->max_steps) {
		in->aborted = true;
		snprintf(in->err, sizeof(in->err), "step limit exceeded");
		return 1;
	}
	switch (nd->kind) {
	case N_CMD:
		in->status = run_command(in, nd);
		break;
	case N_IF: {
		size_t b;
		for (b = 0; b < nd->nbranch; b++) {
			int c = run_list(in, &nd->conds[b]);
			if (in->aborted)
				return 1;
			if (c == 0) {
				in->status = run_list(in, &nd->bodies[b]);
				break;
			}
		}
		if (b == nd->nbranch && nd->has_else)
			in->status = run_list(in, &nd->else_body);
		break;
	}
	case N_FOR: {
		if (nd->nwords < 1)
			break;
		struct ptrvec var = { 0 };
		expand_word(in, &nd->words[0], &var);
		struct ptrvec items = { 0 };
		for (size_t i = 1; i < nd->nwords; i++) {
			if (word_has_glob(&nd->words[i])) {
				struct ptrvec raw = { 0 };
				expand_word(in, &nd->words[i], &raw);
				for (size_t k = 0; k < raw.n; k++) {
					char *dev, *path;
					split_device(raw.v[k], &dev, &path);
					size_t before = items.n;
					iso_glob(in->ctx->iso, path, &items);
					if (items.n == before) /* GRUB leaves unmatched patterns as-is */
						ptrvec_push(&items, xstrdup(raw.v[k]));
					free(dev);
					free(path);
				}
				ptrvec_free(&raw, free);
			} else {
				expand_word(in, &nd->words[i], &items);
			}
		}
		for (size_t i = 0; i < items.n && !in->aborted; i++) {
			if (var.n)
				env_set(&in->env, var.v[0], items.v[i]);
			in->status = run_list(in, &nd->body);
			if (in->brk) {
				in->brk = false;
				break;
			}
		}
		ptrvec_free(&items, free);
		ptrvec_free(&var, free);
		break;
	}
	case N_WHILE:
	case N_UNTIL:
		for (int guard = 0; guard < 10000 && !in->aborted; guard++) {
			int c = run_list(in, &nd->conds[0]);
			if ((nd->kind == N_WHILE && c != 0) || (nd->kind == N_UNTIL && c == 0))
				break;
			in->status = run_list(in, &nd->body);
			if (in->brk) {
				in->brk = false;
				break;
			}
		}
		break;
	case N_FUNC: {
		struct ptrvec nm = { 0 };
		expand_word(in, &nd->words[0], &nm);
		if (nm.n) {
			struct func *f = find_func(in, nm.v[0]);
			if (!f) {
				in->funcs = xrealloc(in->funcs, (in->nfuncs + 1) * sizeof(struct func));
				f = &in->funcs[in->nfuncs++];
				f->name = xstrdup(nm.v[0]);
			}
			f->def = nd;
		}
		ptrvec_free(&nm, free);
		in->status = 0;
		break;
	}
	case N_MENU: {
		struct ptrvec av = { 0 };
		expand_words(in, nd->words, nd->nwords, &av);
		register_menu(in, nd, (char **)av.v, av.n);
		ptrvec_free(&av, free);
		in->status = 0;
		break;
	}
	case N_SUBMENU: {
		struct ptrvec av = { 0 };
		expand_words(in, nd->words, nd->nwords, &av);
		const char *title = "Submenu";
		for (size_t i = 0; i < av.n; i++)
			if (strncmp(av.v[i], "--", 2) != 0) {
				title = av.v[i];
				break;
			}
		if (in->depth < MAX_DEPTH) {
			in->depth++;
			ptrvec_push(&in->submenu_prefix, xstrdup(title));
			run_list(in, &nd->body);
			free(in->submenu_prefix.v[--in->submenu_prefix.n]);
			in->depth--;
		}
		ptrvec_free(&av, free);
		in->status = 0;
		break;
	}
	}
	return in->status;
}

static int run_list(struct interp *in, struct nodelist *l)
{
	int rc = 0;
	for (size_t i = 0; i < l->n && !in->aborted && !in->brk; i++)
		rc = run_node(in, l->v[i]);
	return rc;
}

static int run_text(struct interp *in, const char *text, size_t len, const char *name)
{
	char perr[160] = "";
	struct nodelist *l = parse_script(text, len, perr, sizeof(perr));
	if (perr[0])
		LOGD("grubcfg: %s: %s (evaluating what parsed)", name, perr);
	ptrvec_push(&in->scripts, l);
	const char *saved = in->cur_config;
	in->cur_config = name;
	int rc = run_list(in, l);
	in->cur_config = saved;
	return rc;
}

static int run_script_file(struct interp *in, const char *arg)
{
	if (in->depth >= MAX_DEPTH)
		return 1;
	char *dev, *path;
	split_device(arg, &dev, &path);
	free(dev);
	size_t len;
	char *text = iso_read_text(in->ctx->iso, path, MAX_CFG_SIZE, &len);
	if (!text) {
		free(path);
		return 1;
	}
	in->depth++;
	/* source keeps the caller's positional parameters */
	char *name = xstrdup(path);
	ptrvec_push(&in->names, name); /* cur_config points here while the file runs */
	int rc = run_text(in, text, len, name);
	in->depth--;
	free(text);
	free(path);
	return rc;
}

static void init_env(struct interp *in, const char *cfg_path)
{
	struct env *e = &in->env;
	env_set(e, "root", "isodev");
	char *dir = path_dirname(cfg_path);
	char *prefix = xasprintf("(isodev)%s", dir);
	env_set(e, "prefix", prefix);
	env_set(e, "cmdpath", "(isodev)/EFI/BOOT");
	free(prefix);
	free(dir);
	env_set(e, "grub_platform", "efi");
	env_set(e, "grub_cpu", "x86_64");
	env_set(e, "lang", "en_US");
	env_set(e, "feature_default_font_path", "y");
	env_set(e, "feature_all_video_module", "y");
	env_set(e, "feature_menuentry_id", "y");
	env_set(e, "feature_menuentry_options", "y");
	env_set(e, "feature_200_final", "y");
	env_set(e, "feature_nativedisk_cmd", "y");
	env_set(e, "feature_timeout_style", "y");
	if (in->ctx->iso_path)
		env_set(e, "iso_path", in->ctx->iso_path);
}

static bool is_noise_entry(const struct capture *c)
{
	return !c->have_linux && !c->efi;
}

static void finish_entries(struct interp *in, struct boot_entries *out)
{
	const char *def = env_get(&in->env, "default");
	int top_index = 0;
	for (size_t i = 0; i < in->nmenus && !in->aborted; i++) {
		struct pending_menu *m = &in->menus[i];
		struct capture cap = { 0 };
		struct env saved = in->env;
		env_copy(&in->env, &saved);
		in->cap = &cap;
		in->brk = false;
		call_with_positional(in, &m->body->body, (char **)m->args.v, m->args.n);
		in->cap = NULL;
		m = &in->menus[i]; /* the array may have been reallocated */
		env_free(&in->env);
		in->env = saved;
		in->brk = false;

		int my_top = m->top_level ? top_index++ : -1;
		if (is_noise_entry(&cap)) {
			capture_reset(&cap);
			continue;
		}
		struct boot_entry *e = xcalloc(1, sizeof(*e));
		e->title = xstrdup(m->title);
		e->id = m->id ? xstrdup(m->id) : NULL;
		e->kernel = cap.kernel;
		cap.kernel = NULL;
		e->initrds = cap.initrds;
		e->args = cap.args;
		e->efi = cap.efi;
		e->config = xstrdup(m->config);
		e->source = in->ctx->source;
		e->order = (int)out->n;
		if (def && *def) {
			char *endp;
			long idx = strtol(def, &endp, 10);
			if (*endp == '\0' && my_top >= 0 && idx == my_top)
				e->is_default = true;
			else if (*endp != '\0' && ((e->id && !strcmp(def, e->id)) || !strcmp(def, m->title)))
				e->is_default = true;
		}
		boot_entries_add(out, e);
	}
}

static void interp_free(struct interp *in)
{
	env_free(&in->env);
	for (size_t i = 0; i < in->nfuncs; i++)
		free(in->funcs[i].name);
	free(in->funcs);
	for (size_t i = 0; i < in->nmenus; i++) {
		free(in->menus[i].title);
		free(in->menus[i].id);
		free(in->menus[i].config);
		ptrvec_free(&in->menus[i].args, free);
	}
	free(in->menus);
	for (size_t i = 0; i < in->scripts.n; i++) {
		nodelist_free(in->scripts.v[i]);
		free(in->scripts.v[i]);
	}
	free(in->scripts.v);
	ptrvec_free(&in->names, free);
	ptrvec_free(&in->submenu_prefix, free);
	ptrvec_free(&in->positional, NULL);
}

static int eval_common(const struct grub_eval_ctx *ctx, const char *text, size_t len, const char *name,
		       struct boot_entries *out, char *err, size_t errlen)
{
	struct interp in;
	memset(&in, 0, sizeof(in));
	in.ctx = ctx;
	in.max_steps = ctx->max_steps ? ctx->max_steps : DEFAULT_MAX_STEPS;
	init_env(&in, name);
	struct ptrvec *top = xcalloc(1, sizeof(*top));
	ptrvec_push(&in.positional, top);
	run_text(&in, text, len, name);
	finish_entries(&in, out);
	if (err && errlen)
		snprintf(err, errlen, "%s", in.err);
	int rc = in.aborted ? -1 : 0;

	ptrvec_free(top, free);
	free(top);
	interp_free(&in);
	return rc;
}

int grubcfg_eval_text(const struct grub_eval_ctx *ctx, const char *text, const char *name, struct boot_entries *out,
		      char *err, size_t errlen)
{
	return eval_common(ctx, text, strlen(text), name, out, err, errlen);
}

int grubcfg_eval(const struct grub_eval_ctx *ctx, const char *cfg_path, struct boot_entries *out, char *err, size_t errlen)
{
	size_t len;
	char *text = iso_read_text(ctx->iso, cfg_path, MAX_CFG_SIZE, &len);
	if (!text) {
		if (err && errlen)
			snprintf(err, errlen, "cannot read %s", cfg_path);
		return -1;
	}
	char *canon = iso_canonical_path(ctx->iso, cfg_path);
	int rc = eval_common(ctx, text, len, canon ? canon : cfg_path, out, err, errlen);
	free(canon);
	free(text);
	return rc;
}
