/*
 * Distro Buny — text rendering (stb_truetype)
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "interface/font.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "common/strbuf.h"
#include "common/util.h"

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "third_party/stb_truetype.h"
#pragma GCC diagnostic pop

#define SUBPIXELS 4
#define CACHE_SLOTS 4096

struct glyph {
	unsigned cp;
	int size;       /* pixel size key */
	int sub;        /* sub-pixel bucket */
	struct font *font;
	uint8_t *bmp;
	int w, h, xoff, yoff;
	bool used;
};

struct font {
	stbtt_fontinfo info;
	unsigned char *data;
};

static struct glyph g_cache[CACHE_SLOTS];
static uint8_t g_gamma[256];
static bool g_gamma_ready;

static void gamma_init(void)
{
	/* Slight contrast boost: stb coverage is linear and renders light text thin. */
	for (int i = 0; i < 256; i++)
		g_gamma[i] = (uint8_t)(powf((float)i / 255.0f, 0.80f) * 255.0f + 0.5f);
	g_gamma_ready = true;
}

struct font *font_load(const char *path)
{
	size_t len;
	char *data = read_file(path, 32u << 20, &len);
	if (!data)
		return NULL;
	struct font *f = xcalloc(1, sizeof(*f));
	f->data = (unsigned char *)data;
	if (!stbtt_InitFont(&f->info, f->data, stbtt_GetFontOffsetForIndex(f->data, 0))) {
		free(data);
		free(f);
		return NULL;
	}
	if (!g_gamma_ready)
		gamma_init();
	return f;
}

void font_free(struct font *f)
{
	if (!f)
		return;
	for (int i = 0; i < CACHE_SLOTS; i++)
		if (g_cache[i].used && g_cache[i].font == f) {
			free(g_cache[i].bmp);
			g_cache[i].used = false;
		}
	free(f->data);
	free(f);
}

unsigned utf8_next(const char **sp)
{
	const unsigned char *s = (const unsigned char *)*sp;
	unsigned c = s[0];
	int n = 0;
	if (c < 0x80) {
		*sp += 1;
		return c;
	}
	if ((c & 0xE0) == 0xC0) {
		c &= 0x1F;
		n = 1;
	} else if ((c & 0xF0) == 0xE0) {
		c &= 0x0F;
		n = 2;
	} else if ((c & 0xF8) == 0xF0) {
		c &= 0x07;
		n = 3;
	} else {
		*sp += 1;
		return 0xFFFD;
	}
	for (int i = 1; i <= n; i++) {
		if ((s[i] & 0xC0) != 0x80) {
			*sp += (size_t)i;
			return 0xFFFD;
		}
		c = (c << 6) | (s[i] & 0x3F);
	}
	*sp += (size_t)n + 1;
	return c;
}

static float scale_for(const struct text_style *st)
{
	return stbtt_ScaleForPixelHeight(&st->font->info, st->size);
}

void font_metrics(const struct text_style *st, struct font_metrics *m)
{
	int asc, desc, gap;
	stbtt_GetFontVMetrics(&st->font->info, &asc, &desc, &gap);
	float s = scale_for(st);
	m->ascent = (float)asc * s;
	m->descent = (float)-desc * s;
	m->line_height = (float)(asc - desc + gap) * s;
}

static struct glyph *glyph_get(const struct text_style *st, unsigned cp, int sub)
{
	int size = (int)(st->size * 4.0f + 0.5f); /* quarter-pixel size keys */
	unsigned h = (cp * 2654435761u) ^ ((unsigned)size * 40503u) ^ ((unsigned)sub * 97u) ^ (unsigned)(uintptr_t)st->font;
	for (int probe = 0; probe < 16; probe++) {
		struct glyph *g = &g_cache[(h + (unsigned)probe) % CACHE_SLOTS];
		if (g->used && g->cp == cp && g->size == size && g->sub == sub && g->font == st->font)
			return g;
		if (!g->used) {
			float s = scale_for(st);
			g->bmp = stbtt_GetCodepointBitmapSubpixel(&st->font->info, s, s, (float)sub / SUBPIXELS, 0.0f, (int)cp,
								  &g->w, &g->h, &g->xoff, &g->yoff);
			if (g->bmp)
				for (int i = 0; i < g->w * g->h; i++)
					g->bmp[i] = g_gamma[g->bmp[i]];
			g->cp = cp;
			g->size = size;
			g->sub = sub;
			g->font = st->font;
			g->used = true;
			return g;
		}
	}
	/* cache full in this neighbourhood: evict the home slot */
	struct glyph *g = &g_cache[h % CACHE_SLOTS];
	free(g->bmp);
	g->used = false;
	return glyph_get(st, cp, sub);
}

static float advance(const struct text_style *st, unsigned cp, unsigned next, float s)
{
	int adv, lsb;
	stbtt_GetCodepointHMetrics(&st->font->info, (int)cp, &adv, &lsb);
	float a = (float)adv * s;
	if (next)
		a += (float)stbtt_GetCodepointKernAdvance(&st->font->info, (int)cp, (int)next) * s;
	return a;
}

float text_width(const struct text_style *st, const char *t)
{
	if (!t)
		return 0;
	float s = scale_for(st), w = 0;
	const char *p = t;
	while (*p) {
		unsigned cp = utf8_next(&p);
		const char *q = p;
		unsigned next = *q ? utf8_next(&q) : 0;
		w += advance(st, cp, next, s);
	}
	return w;
}

float text_draw(struct canvas *c, const struct text_style *st, float x, float y, const char *t, color_t col)
{
	if (!t || !st->font)
		return 0;
	float s = scale_for(st);
	float pen = x;
	int by = (int)floorf(y + 0.5f);
	const char *p = t;
	while (*p) {
		unsigned cp = utf8_next(&p);
		const char *q = p;
		unsigned next = *q ? utf8_next(&q) : 0;
		if (cp != ' ' && cp != '\t') {
			float fl = floorf(pen);
			int sub = (int)((pen - fl) * SUBPIXELS) % SUBPIXELS;
			struct glyph *g = glyph_get(st, cp, sub);
			if (g->bmp && g->w > 0)
				gfx_blit_mask(c, g->bmp, g->w, g->h, g->w, (int)fl + g->xoff, by + g->yoff, col);
		}
		pen += advance(st, cp, next, s);
	}
	return pen - x;
}

char *text_ellipsize(const struct text_style *st, const char *t, float max_w)
{
	if (!t)
		return xstrdup("");
	if (text_width(st, t) <= max_w)
		return xstrdup(t);
	const char *ell = "\xE2\x80\xA6";
	float ew = text_width(st, ell);
	struct strbuf sb = STRBUF_INIT;
	const char *p = t;
	float w = 0, s = scale_for(st);
	while (*p) {
		const char *before = p;
		unsigned cp = utf8_next(&p);
		float a = advance(st, cp, 0, s);
		if (w + a + ew > max_w)
			break;
		sb_addn(&sb, before, (size_t)(p - before));
		w += a;
	}
	/* do not end with a space before the ellipsis */
	while (sb.len && sb.buf[sb.len - 1] == ' ')
		sb.buf[--sb.len] = '\0';
	sb_add(&sb, ell);
	return sb_detach(&sb);
}

/* Break text into lines no wider than max_w (break at spaces, or anywhere for long words). */
static int wrap_lines(const struct text_style *st, float max_w, const char *t, struct ptrvec *out)
{
	const char *p = t;
	while (*p) {
		const char *line_start = p, *last_space = NULL, *q = p;
		float w = 0, s = scale_for(st);
		while (*q && *q != '\n') {
			const char *before = q;
			unsigned cp = utf8_next(&q);
			float a = advance(st, cp, 0, s);
			if (w + a > max_w && before > line_start) {
				if (last_space) {
					q = last_space;
				} else {
					q = before;
				}
				break;
			}
			if (cp == ' ')
				last_space = before;
			w += a;
		}
		const char *end = q;
		if (out)
			ptrvec_push(out, xstrndup(line_start, (size_t)(end - line_start)));
		p = end;
		while (*p == ' ')
			p++;
		if (*p == '\n')
			p++;
	}
	return out ? (int)out->n : 0;
}

int text_count_lines(const struct text_style *st, float max_w, const char *t)
{
	struct ptrvec lines = { 0 };
	wrap_lines(st, max_w, t, &lines);
	int n = (int)lines.n;
	ptrvec_free(&lines, free);
	return n;
}

int text_draw_wrapped(struct canvas *c, const struct text_style *st, float x, float y, float max_w, const char *t,
		      color_t col, int max_lines, float line_spacing)
{
	struct ptrvec lines = { 0 };
	wrap_lines(st, max_w, t, &lines);
	struct font_metrics m;
	font_metrics(st, &m);
	int drawn = 0;
	for (size_t i = 0; i < lines.n; i++) {
		if (max_lines && drawn == max_lines - 1 && i + 1 < lines.n) {
			/* last allowed line: append the rest with an ellipsis */
			struct strbuf rest = STRBUF_INIT;
			for (size_t k = i; k < lines.n; k++) {
				if (k > i)
					sb_addc(&rest, ' ');
				sb_add(&rest, lines.v[k]);
			}
			char *e = text_ellipsize(st, sb_str(&rest), max_w);
			text_draw(c, st, x, y + (float)drawn * m.line_height * line_spacing, e, col);
			free(e);
			sb_free(&rest);
			drawn++;
			break;
		}
		text_draw(c, st, x, y + (float)drawn * m.line_height * line_spacing, lines.v[i], col);
		drawn++;
	}
	ptrvec_free(&lines, free);
	return drawn;
}
