/*
 * Distro Buny — icons
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "interface/icons.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/log.h"
#include "common/util.h"
#include "interface/path.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* All interface icons are designed on a 24x24 grid with 2-unit strokes. */
struct pen {
	struct canvas *c;
	float x, y, s;  /* origin and scale (size / 24) */
	float w;        /* stroke width in pixels */
	color_t col;
};

static void ln(struct pen *p, float x0, float y0, float x1, float y1)
{
	gfx_line(p->c, p->x + x0 * p->s, p->y + y0 * p->s, p->x + x1 * p->s, p->y + y1 * p->s, p->w, p->col);
}

static void poly(struct pen *p, const float *xy, int n, bool closed)
{
	for (int i = 0; i + 1 < n; i++)
		ln(p, xy[2 * i], xy[2 * i + 1], xy[2 * i + 2], xy[2 * i + 3]);
	if (closed)
		ln(p, xy[2 * n - 2], xy[2 * n - 1], xy[0], xy[1]);
}

static void rrect(struct pen *p, float x, float y, float w, float h, float r)
{
	gfx_stroke_rrect(p->c, p->x + x * p->s - p->w / 2, p->y + y * p->s - p->w / 2, w * p->s + p->w, h * p->s + p->w,
			 r * p->s + p->w / 2, p->w, p->col);
}

static void circle(struct pen *p, float cx, float cy, float r)
{
	gfx_stroke_circle(p->c, p->x + cx * p->s, p->y + cy * p->s, r * p->s + p->w / 2, p->w, p->col);
}

static void dot(struct pen *p, float cx, float cy, float r)
{
	gfx_fill_circle(p->c, p->x + cx * p->s, p->y + cy * p->s, r * p->s, p->col);
}

static void arc(struct pen *p, float cx, float cy, float r, float a0, float a1)
{
	int n = 24;
	float px = cx + r * cosf(a0), py = cy + r * sinf(a0);
	for (int i = 1; i <= n; i++) {
		float a = a0 + (a1 - a0) * (float)i / (float)n;
		float nx = cx + r * cosf(a), ny = cy + r * sinf(a);
		ln(p, px, py, nx, ny);
		px = nx;
		py = ny;
	}
}

void icon_draw(struct canvas *c, enum ui_icon id, float x, float y, float size, color_t col)
{
	struct pen p = { c, x, y, size / 24.0f, fmaxf(1.2f, size * 0.085f), col };
	switch (id) {
	case ICON_BACK: {
		ln(&p, 19, 12, 5, 12);
		float a[] = { 11.5f, 5.5f, 5, 12, 11.5f, 18.5f };
		poly(&p, a, 3, false);
		break;
	}
	case ICON_CHEVRON_RIGHT: {
		float a[] = { 9, 5.5f, 15.5f, 12, 9, 18.5f };
		poly(&p, a, 3, false);
		break;
	}
	case ICON_CHEVRON_LEFT: {
		float a[] = { 15, 5.5f, 8.5f, 12, 15, 18.5f };
		poly(&p, a, 3, false);
		break;
	}
	case ICON_CHEVRON_DOWN: {
		float a[] = { 6, 9, 12, 15, 18, 9 };
		poly(&p, a, 3, false);
		break;
	}
	case ICON_GEAR:
		circle(&p, 12, 12, 3);
		circle(&p, 12, 12, 7);
		for (int i = 0; i < 8; i++) {
			float a = (float)i * (float)M_PI / 4.0f;
			ln(&p, 12 + 7.5f * cosf(a), 12 + 7.5f * sinf(a), 12 + 10.2f * cosf(a), 12 + 10.2f * sinf(a));
		}
		break;
	case ICON_INFO:
		circle(&p, 12, 12, 9.5f);
		ln(&p, 12, 11, 12, 16.5f);
		dot(&p, 12, 7.6f, 1.25f);
		break;
	case ICON_POWER:
		arc(&p, 12, 13, 8, (float)(-M_PI * 0.3), (float)(M_PI * 1.3));
		ln(&p, 12, 3, 12, 11.5f);
		break;
	case ICON_SEARCH:
		circle(&p, 10.5f, 10.5f, 6.5f);
		ln(&p, 15.5f, 15.5f, 20.5f, 20.5f);
		break;
	case ICON_HOME: {
		float roof[] = { 3, 10.5f, 12, 3.5f, 21, 10.5f };
		poly(&p, roof, 3, false);
		float body[] = { 5.5f, 9, 5.5f, 20.5f, 18.5f, 20.5f, 18.5f, 9 };
		poly(&p, body, 4, false);
		float door[] = { 10, 20.5f, 10, 15, 14, 15, 14, 20.5f };
		poly(&p, door, 4, false);
		break;
	}
	case ICON_FOLDER: {
		float f[] = { 3, 6, 9, 6, 11, 8.5f, 21, 8.5f, 21, 19, 3, 19 };
		poly(&p, f, 6, true);
		break;
	}
	case ICON_MONITOR:
		rrect(&p, 2.5f, 3.5f, 19, 12.5f, 2);
		ln(&p, 8, 20.5f, 16, 20.5f);
		ln(&p, 12, 16, 12, 20.5f);
		break;
	case ICON_SERVER:
		rrect(&p, 3, 3, 18, 7, 2);
		rrect(&p, 3, 14, 18, 7, 2);
		dot(&p, 7, 6.5f, 1.2f);
		dot(&p, 7, 17.5f, 1.2f);
		ln(&p, 11, 6.5f, 17, 6.5f);
		ln(&p, 11, 17.5f, 17, 17.5f);
		break;
	case ICON_GAMEPAD:
		rrect(&p, 2, 7, 20, 11, 5.5f);
		ln(&p, 7.5f, 10.5f, 7.5f, 14.5f);
		ln(&p, 5.5f, 12.5f, 9.5f, 12.5f);
		dot(&p, 15.5f, 11.5f, 1.3f);
		dot(&p, 18, 14, 1.3f);
		break;
	case ICON_TERMINAL: {
		rrect(&p, 2.5f, 4, 19, 16, 2);
		float pr[] = { 6.5f, 9, 9.5f, 12, 6.5f, 15 };
		poly(&p, pr, 3, false);
		ln(&p, 12, 15.5f, 17, 15.5f);
		break;
	}
	case ICON_TOOLS:
		ln(&p, 4.5f, 19.5f, 12.5f, 11.5f);
		arc(&p, 15.5f, 8.5f, 4.5f, (float)(M_PI * 0.75), (float)(M_PI * 2.25));
		break;
	case ICON_SHIELD: {
		float s[] = { 12, 2.5f, 19.5f, 5.5f, 19.5f, 11, 17.5f, 16, 12, 21.5f, 6.5f, 16, 4.5f, 11, 4.5f, 5.5f };
		poly(&p, s, 8, true);
		break;
	}
	case ICON_CHIP:
		rrect(&p, 6, 6, 12, 12, 2);
		for (int i = 0; i < 3; i++) {
			float o = 9 + (float)i * 3;
			ln(&p, o, 2.5f, o, 6);
			ln(&p, o, 18, o, 21.5f);
			ln(&p, 2.5f, o, 6, o);
			ln(&p, 18, o, 21.5f, o);
		}
		break;
	case ICON_DRIVE:
		rrect(&p, 2.5f, 7, 19, 10, 2);
		dot(&p, 17, 12, 1.2f);
		ln(&p, 6, 12, 12, 12);
		break;
	case ICON_CHECK: {
		float ck[] = { 5, 12.5f, 10, 17.5f, 19.5f, 7 };
		poly(&p, ck, 3, false);
		break;
	}
	case ICON_WARNING: {
		float t[] = { 12, 3.5f, 21.5f, 20, 2.5f, 20 };
		poly(&p, t, 3, true);
		ln(&p, 12, 9.5f, 12, 13.5f);
		dot(&p, 12, 16.8f, 1.2f);
		break;
	}
	case ICON_CLOSE:
		ln(&p, 6, 6, 18, 18);
		ln(&p, 18, 6, 6, 18);
		break;
	case ICON_RESTART:
		arc(&p, 12, 12, 8, (float)(-M_PI * 0.5), (float)(M_PI * 1.2));
		ln(&p, 12, 4, 15.5f, 2);
		ln(&p, 12, 4, 15.5f, 6.5f);
		break;
	case ICON_DISC:
		circle(&p, 12, 12, 9.5f);
		circle(&p, 12, 12, 2.5f);
		break;
	case ICON_PLAY: {
		struct path pa;
		path_init(&pa);
		path_move(&pa, 8, 5);
		path_line(&pa, 19, 12);
		path_line(&pa, 8, 19);
		path_close(&pa);
		path_fill(c, &pa, p.s, x, y, col);
		path_free(&pa);
		break;
	}
	case ICON_LOCK:
		rrect(&p, 5, 11, 14, 10, 2);
		arc(&p, 12, 8.5f, 4, (float)M_PI, (float)(2 * M_PI));
		ln(&p, 8, 8.5f, 8, 11);
		ln(&p, 16, 8.5f, 16, 11);
		break;
	case ICON_LIST:
		for (int i = 0; i < 3; i++) {
			float yy = 6.5f + (float)i * 5.5f;
			dot(&p, 4.5f, yy, 1.2f);
			ln(&p, 8.5f, yy, 20, yy);
		}
		break;
	case ICON_MEMORY:
		rrect(&p, 2.5f, 7, 19, 9, 1.5f);
		for (int i = 0; i < 4; i++)
			ln(&p, 6 + (float)i * 4, 16, 6 + (float)i * 4, 19);
		break;
	}
}

int icon_from_name(const char *name)
{
	static const struct { const char *n; enum ui_icon i; } map[] = {
		{ "cat-desktop", ICON_MONITOR }, { "cat-server", ICON_SERVER }, { "cat-gaming", ICON_GAMEPAD },
		{ "cat-terminal", ICON_TERMINAL }, { "cat-tools", ICON_TOOLS }, { "cat-security", ICON_SHIELD },
		{ "folder", ICON_FOLDER }, { "home", ICON_HOME },
	};
	for (size_t i = 0; i < ARRAY_SIZE(map); i++)
		if (name && !strcmp(name, map[i].n))
			return (int)map[i].i;
	return -1;
}

/* ------------------------------------------------------- distribution marks */

struct mark_cache {
	int size;
	uint8_t *mask;
};

struct mark {
	char *id;
	bool mono;
	color_t color;
	char *text;
	struct path path;
	struct mark_cache cache[4];
};

struct distro_icons {
	struct mark *v;
	size_t n;
};

struct distro_icons *distro_icons_load(const char *file)
{
	char *text = read_file(file, 4u << 20, NULL);
	if (!text)
		return NULL;
	struct distro_icons *d = xcalloc(1, sizeof(*d));
	char *save = NULL;
	for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		if (line[0] == '#')
			continue;
		char *f[5];
		int nf = 0;
		char *s = line;
		while (nf < 5) {
			f[nf++] = s;
			char *bar = nf < 5 ? strchr(s, '|') : NULL;
			if (!bar)
				break;
			*bar = '\0';
			s = bar + 1;
		}
		if (nf < 5)
			continue;
		struct mark m;
		memset(&m, 0, sizeof(m));
		m.id = xstrdup(f[0]);
		m.mono = !strcmp(f[1], "mono");
		m.color = HEXC((uint32_t)strtoul(f[2], NULL, 16));
		path_init(&m.path);
		if (m.mono) {
			m.text = xstrdup(f[4]);
		} else if (path_parse_svg(&m.path, f[4]) < 0) {
			LOGW("icon %s: invalid path data", m.id);
			free(m.id);
			path_free(&m.path);
			continue;
		}
		d->v = xrealloc(d->v, (d->n + 1) * sizeof(*d->v));
		d->v[d->n++] = m;
	}
	free(text);
	LOGI("loaded %zu distribution marks", d->n);
	return d;
}

void distro_icons_free(struct distro_icons *d)
{
	if (!d)
		return;
	for (size_t i = 0; i < d->n; i++) {
		free(d->v[i].id);
		free(d->v[i].text);
		path_free(&d->v[i].path);
		for (int k = 0; k < 4; k++)
			free(d->v[i].cache[k].mask);
	}
	free(d->v);
	free(d);
}

static struct mark *find_mark(struct distro_icons *d, const char *id)
{
	if (!d || !id)
		return NULL;
	for (size_t i = 0; i < d->n; i++)
		if (!strcmp(d->v[i].id, id))
			return &d->v[i];
	return NULL;
}

bool distro_icon_known(struct distro_icons *d, const char *id)
{
	return find_mark(d, id) != NULL;
}

color_t distro_icon_color(struct distro_icons *d, const char *id, color_t def)
{
	struct mark *m = find_mark(d, id);
	return m ? m->color : def;
}

bool distro_icon_draw(struct distro_icons *d, struct canvas *c, const char *id, float x, float y, float size,
		      struct font *mono_font, float opacity)
{
	struct mark *m = find_mark(d, id);
	if (!m)
		return false;
	color_t col = color_alpha(m->color, opacity);
	/* very dark marks (#000000) would vanish on dark backgrounds: they are
	 * drawn by the caller on a light tile, so keep the official colour */
	if (m->mono) {
		float r = size * 0.24f;
		gfx_fill_rrect(c, x, y, size, size, r, col);
		if (m->text[0] && mono_font) {
			struct text_style st = { mono_font, size * 0.40f };
			float tw = text_width(&st, m->text);
			struct font_metrics fm;
			font_metrics(&st, &fm);
			text_draw(c, &st, x + (size - tw) / 2, y + size / 2 + (fm.ascent - fm.descent) / 2 - fm.descent * 0.15f,
				  m->text, color_alpha(HEXC(0xFFFFFF), opacity));
		}
		return true;
	}
	int isz = (int)(size + 0.5f);
	if (isz < 4)
		return true;
	struct mark_cache *slot = NULL;
	for (int k = 0; k < 4; k++)
		if (m->cache[k].mask && m->cache[k].size == isz)
			slot = &m->cache[k];
	if (!slot) {
		slot = &m->cache[0];
		for (int k = 0; k < 4; k++)
			if (!m->cache[k].mask) {
				slot = &m->cache[k];
				break;
			}
		free(slot->mask);
		/* the 24-unit viewBox maps onto the box; one pixel of margin keeps
		 * the coverage rasteriser's geometry inside the raster */
		float s = (float)(isz - 2) / 24.0f;
		slot->mask = path_raster(&m->path, isz, isz, s, 1.0f, 1.0f);
		slot->size = isz;
	}
	gfx_blit_mask(c, slot->mask, isz, isz, isz, (int)(x + 0.5f), (int)(y + 0.5f), col);
	return true;
}
