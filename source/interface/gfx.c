/*
 * Distro Buny — software rendering canvas
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "interface/gfx.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "common/util.h"

color_t color_mix(color_t a, color_t b, float t)
{
	if (t <= 0)
		return a;
	if (t >= 1)
		return b;
	uint32_t out = 0;
	for (int s = 0; s < 32; s += 8) {
		float ca = (float)((a >> s) & 0xFF), cb = (float)((b >> s) & 0xFF);
		out |= (uint32_t)(ca + (cb - ca) * t + 0.5f) << s;
	}
	return out;
}

struct rect rect_intersect(struct rect a, struct rect b)
{
	int x0 = BUNY_MAX(a.x, b.x), y0 = BUNY_MAX(a.y, b.y);
	int x1 = BUNY_MIN(a.x + a.w, b.x + b.w), y1 = BUNY_MIN(a.y + a.h, b.y + b.h);
	struct rect r = { x0, y0, x1 - x0, y1 - y0 };
	if (r.w < 0)
		r.w = 0;
	if (r.h < 0)
		r.h = 0;
	return r;
}

struct rect rect_union(struct rect a, struct rect b)
{
	if (rect_empty(a))
		return b;
	if (rect_empty(b))
		return a;
	int x0 = BUNY_MIN(a.x, b.x), y0 = BUNY_MIN(a.y, b.y);
	int x1 = BUNY_MAX(a.x + a.w, b.x + b.w), y1 = BUNY_MAX(a.y + a.h, b.y + b.h);
	return (struct rect){ x0, y0, x1 - x0, y1 - y0 };
}

struct canvas *canvas_new(int w, int h)
{
	struct canvas *c = xcalloc(1, sizeof(*c));
	size_t bytes = (size_t)w * (size_t)h * 4;
	void *p = NULL;
	if (posix_memalign(&p, 64, bytes ? bytes : 64) != 0)
		abort();
	memset(p, 0, bytes);
	c->px = p;
	c->w = w;
	c->h = h;
	c->stride = w;
	c->owns = true;
	c->clip = (struct rect){ 0, 0, w, h };
	return c;
}

void canvas_free(struct canvas *c)
{
	if (!c)
		return;
	if (c->owns)
		free(c->px);
	free(c);
}

void canvas_wrap(struct canvas *c, uint32_t *px, int w, int h, int stride)
{
	memset(c, 0, sizeof(*c));
	c->px = px;
	c->w = w;
	c->h = h;
	c->stride = stride;
	c->clip = (struct rect){ 0, 0, w, h };
}

void canvas_set_clip(struct canvas *c, struct rect r)
{
	c->clip = rect_intersect(r, (struct rect){ 0, 0, c->w, c->h });
}

void canvas_reset_clip(struct canvas *c)
{
	c->clip = (struct rect){ 0, 0, c->w, c->h };
}

struct rect canvas_push_clip(struct canvas *c, struct rect r)
{
	struct rect old = c->clip;
	c->clip = rect_intersect(old, r);
	return old;
}

void gfx_clear(struct canvas *c, color_t col)
{
	gfx_fill_rect(c, (struct rect){ 0, 0, c->w, c->h }, col);
}

void gfx_fill_rect(struct canvas *c, struct rect r, color_t col)
{
	r = rect_intersect(r, c->clip);
	if (rect_empty(r) || !(col >> 24))
		return;
	uint32_t p = premul(col, 255);
	for (int y = r.y; y < r.y + r.h; y++) {
		uint32_t *row = c->px + (size_t)y * c->stride + r.x;
		if ((p >> 24) == 255) {
			for (int x = 0; x < r.w; x++)
				row[x] = p;
		} else {
			for (int x = 0; x < r.w; x++)
				row[x] = px_over(row[x], p);
		}
	}
}

/* Signed distance from (px,py) to a rounded box centred at (cx,cy). */
static inline float sdf_rrect(float px, float py, float cx, float cy, float hw, float hh, float r)
{
	float qx = fabsf(px - cx) - (hw - r);
	float qy = fabsf(py - cy) - (hh - r);
	float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
	float outside = sqrtf(ox * ox + oy * oy);
	float inside = fminf(fmaxf(qx, qy), 0.0f);
	return outside + inside - r;
}

static inline void blend_cov(uint32_t *dst, color_t col, float cov)
{
	if (cov <= 0.0f)
		return;
	uint32_t ci = cov >= 1.0f ? 255u : (uint32_t)(cov * 255.0f + 0.5f);
	*dst = px_over(*dst, premul(col, ci));
}

static void rrect_cov(struct canvas *c, float x, float y, float w, float h, float radius, color_t col, float stroke)
{
	if (w <= 0 || h <= 0 || !(col >> 24))
		return;
	float r = fminf(radius, fminf(w, h) * 0.5f);
	if (r < 0)
		r = 0;
	float cx = x + w * 0.5f, cy = y + h * 0.5f, hw = w * 0.5f, hh = h * 0.5f;
	struct rect bb = { (int)floorf(x) - 1, (int)floorf(y) - 1, (int)ceilf(w) + 3, (int)ceilf(h) + 3 };
	bb = rect_intersect(bb, c->clip);
	if (rect_empty(bb))
		return;
	uint32_t full = premul(col, 255);
	for (int py = bb.y; py < bb.y + bb.h; py++) {
		uint32_t *row = c->px + (size_t)py * c->stride;
		float fy = (float)py + 0.5f;
		/* rows away from the corners: coverage depends on x only */
		bool band = stroke <= 0 && fabsf(fy - cy) <= hh - r - 0.5f;
		if (band) {
			float l = x, rr = x + w;
			for (int px = bb.x; px < bb.x + bb.w; px++) {
				float fx = (float)px + 0.5f;
				float cov = fminf(fminf(fx - l + 0.5f, rr - fx + 0.5f), 1.0f);
				if (cov >= 1.0f && (full >> 24) == 255)
					row[px] = full;
				else
					blend_cov(&row[px], col, cov);
			}
			continue;
		}
		for (int px = bb.x; px < bb.x + bb.w; px++) {
			float d = sdf_rrect((float)px + 0.5f, fy, cx, cy, hw, hh, r);
			float cov = 0.5f - d;
			if (stroke > 0) {
				float inner = 0.5f + d + stroke;
				cov = fminf(cov, inner);
			}
			if (cov <= 0)
				continue;
			if (cov >= 1.0f && (full >> 24) == 255)
				row[px] = full;
			else
				blend_cov(&row[px], col, cov > 1 ? 1 : cov);
		}
	}
}

void gfx_fill_rrect(struct canvas *c, float x, float y, float w, float h, float radius, color_t col)
{
	rrect_cov(c, x, y, w, h, radius, col, 0);
}

void gfx_stroke_rrect(struct canvas *c, float x, float y, float w, float h, float radius, float width, color_t col)
{
	rrect_cov(c, x, y, w, h, radius, col, width);
}

void gfx_fill_circle(struct canvas *c, float cx, float cy, float r, color_t col)
{
	rrect_cov(c, cx - r, cy - r, 2 * r, 2 * r, r, col, 0);
}

void gfx_stroke_circle(struct canvas *c, float cx, float cy, float r, float width, color_t col)
{
	rrect_cov(c, cx - r, cy - r, 2 * r, 2 * r, r, col, width);
}

void gfx_fill_vgradient(struct canvas *c, struct rect r, color_t top, color_t bottom)
{
	/* Ordered (Bayer 4x4) dithering: shallow gradients otherwise show 8-bit steps. */
	static const float bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
	struct rect cr = rect_intersect(r, c->clip);
	if (rect_empty(cr))
		return;
	float c0[3] = { (float)((top >> 16) & 0xFF), (float)((top >> 8) & 0xFF), (float)(top & 0xFF) };
	float c1[3] = { (float)((bottom >> 16) & 0xFF), (float)((bottom >> 8) & 0xFF), (float)(bottom & 0xFF) };
	for (int y = cr.y; y < cr.y + cr.h; y++) {
		float t = r.h > 1 ? (float)(y - r.y) / (float)(r.h - 1) : 0;
		float v[3];
		for (int k = 0; k < 3; k++)
			v[k] = c0[k] + (c1[k] - c0[k]) * t;
		uint32_t *row = c->px + (size_t)y * c->stride;
		for (int x = cr.x; x < cr.x + cr.w; x++) {
			float d = (bayer[y & 3][x & 3] + 0.5f) / 16.0f;
			uint32_t rr = (uint32_t)BUNY_CLAMP(v[0] + d, 0.0f, 255.0f);
			uint32_t gg = (uint32_t)BUNY_CLAMP(v[1] + d, 0.0f, 255.0f);
			uint32_t bb = (uint32_t)BUNY_CLAMP(v[2] + d, 0.0f, 255.0f);
			row[x] = 0xFF000000u | rr << 16 | gg << 8 | bb;
		}
	}
}

/* ------------------------------------------------------------ shadows */

struct shadow_entry {
	int w, h, r, blur;
	uint8_t *mask;
	int mw, mh;
	unsigned stamp;
};

#define SHADOW_CACHE 48
static struct shadow_entry g_shadows[SHADOW_CACHE];
static unsigned g_shadow_stamp;

static void box_blur(uint8_t *buf, int w, int h, int radius, uint8_t *tmp)
{
	if (radius < 1)
		return;
	int div = 2 * radius + 1;
	/* horizontal */
	for (int y = 0; y < h; y++) {
		uint8_t *row = buf + (size_t)y * w;
		int acc = 0;
		for (int x = -radius; x <= radius; x++)
			acc += row[BUNY_CLAMP(x, 0, w - 1)];
		for (int x = 0; x < w; x++) {
			tmp[(size_t)y * w + x] = (uint8_t)(acc / div);
			acc += row[BUNY_MIN(x + radius + 1, w - 1)] - row[BUNY_MAX(x - radius, 0)];
		}
	}
	/* vertical */
	for (int x = 0; x < w; x++) {
		int acc = 0;
		for (int y = -radius; y <= radius; y++)
			acc += tmp[(size_t)BUNY_CLAMP(y, 0, h - 1) * w + x];
		for (int y = 0; y < h; y++) {
			buf[(size_t)y * w + x] = (uint8_t)(acc / div);
			acc += tmp[(size_t)BUNY_MIN(y + radius + 1, h - 1) * w + x] - tmp[(size_t)BUNY_MAX(y - radius, 0) * w + x];
		}
	}
}

static struct shadow_entry *shadow_get(int w, int h, int r, int blur)
{
	struct shadow_entry *victim = &g_shadows[0];
	for (int i = 0; i < SHADOW_CACHE; i++) {
		struct shadow_entry *e = &g_shadows[i];
		if (e->mask && e->w == w && e->h == h && e->r == r && e->blur == blur) {
			e->stamp = ++g_shadow_stamp;
			return e;
		}
		if (!e->mask || e->stamp < victim->stamp)
			victim = e;
	}
	free(victim->mask);
	int pad = blur * 2;
	int mw = w + 2 * pad, mh = h + 2 * pad;
	uint8_t *mask = xcalloc((size_t)mw * mh, 1);
	struct canvas tmpc;
	uint32_t *px = xcalloc((size_t)mw * mh, 4);
	canvas_wrap(&tmpc, px, mw, mh, mw);
	gfx_fill_rrect(&tmpc, (float)pad, (float)pad, (float)w, (float)h, (float)r, RGBA(0, 0, 0, 255));
	for (size_t i = 0; i < (size_t)mw * mh; i++)
		mask[i] = (uint8_t)(px[i] >> 24);
	free(px);
	uint8_t *tmp = xmalloc((size_t)mw * mh);
	int rad = BUNY_MAX(1, blur / 3);
	for (int pass = 0; pass < 3; pass++)
		box_blur(mask, mw, mh, rad, tmp);
	free(tmp);
	*victim = (struct shadow_entry){ w, h, r, blur, mask, mw, mh, ++g_shadow_stamp };
	return victim;
}

void gfx_shadow(struct canvas *c, float x, float y, float w, float h, float radius, float blur, float dy, color_t col)
{
	int iw = (int)(w + 0.5f), ih = (int)(h + 0.5f), ib = (int)(blur + 0.5f);
	if (iw <= 0 || ih <= 0 || !(col >> 24))
		return;
	struct shadow_entry *e = shadow_get(iw, ih, (int)(radius + 0.5f), BUNY_MAX(ib, 1));
	int pad = e->blur * 2;
	gfx_blit_mask(c, e->mask, e->mw, e->mh, e->mw, (int)(x + 0.5f) - pad, (int)(y + dy + 0.5f) - pad, col);
}

/* --------------------------------------------------------------- lines */

void gfx_line(struct canvas *c, float x0, float y0, float x1, float y1, float width, color_t col)
{
	float hw = width * 0.5f;
	struct rect bb = { (int)floorf(fminf(x0, x1) - hw) - 1, (int)floorf(fminf(y0, y1) - hw) - 1, 0, 0 };
	bb.w = (int)ceilf(fmaxf(x0, x1) + hw) + 2 - bb.x;
	bb.h = (int)ceilf(fmaxf(y0, y1) + hw) + 2 - bb.y;
	bb = rect_intersect(bb, c->clip);
	float dx = x1 - x0, dy = y1 - y0;
	float len2 = dx * dx + dy * dy;
	for (int py = bb.y; py < bb.y + bb.h; py++) {
		uint32_t *row = c->px + (size_t)py * c->stride;
		for (int px = bb.x; px < bb.x + bb.w; px++) {
			float fx = (float)px + 0.5f - x0, fy = (float)py + 0.5f - y0;
			float t = len2 > 0 ? (fx * dx + fy * dy) / len2 : 0;
			t = BUNY_CLAMP(t, 0.0f, 1.0f);
			float ex = fx - t * dx, ey = fy - t * dy;
			float d = sqrtf(ex * ex + ey * ey) - hw;
			blend_cov(&row[px], col, BUNY_CLAMP(0.5f - d, 0.0f, 1.0f));
		}
	}
}

/* ------------------------------------------------------------- blits */

void gfx_blit_mask(struct canvas *c, const uint8_t *mask, int mw, int mh, int mstride, int x, int y, color_t col)
{
	struct rect r = rect_intersect((struct rect){ x, y, mw, mh }, c->clip);
	if (rect_empty(r))
		return;
	uint32_t ca = col >> 24;
	for (int py = r.y; py < r.y + r.h; py++) {
		const uint8_t *m = mask + (size_t)(py - y) * mstride + (r.x - x);
		uint32_t *row = c->px + (size_t)py * c->stride + r.x;
		for (int px = 0; px < r.w; px++) {
			uint32_t cov = m[px];
			if (!cov)
				continue;
			uint32_t a = (cov * ca + 127) / 255;
			row[px] = px_over(row[px], premul((col & 0x00FFFFFFu) | (a << 24), 255));
		}
	}
}

void gfx_blit(struct canvas *c, const uint32_t *src, int sw, int sh, int sstride, int x, int y, float opacity)
{
	struct rect r = rect_intersect((struct rect){ x, y, sw, sh }, c->clip);
	if (rect_empty(r) || opacity <= 0.0f)
		return;
	uint32_t op = opacity >= 1.0f ? 256 : (uint32_t)(opacity * 256.0f);
	for (int py = r.y; py < r.y + r.h; py++) {
		const uint32_t *s = src + (size_t)(py - y) * sstride + (r.x - x);
		uint32_t *row = c->px + (size_t)py * c->stride + r.x;
		if (op == 256) {
			for (int px = 0; px < r.w; px++)
				row[px] = px_over(row[px], s[px]);
		} else {
			for (int px = 0; px < r.w; px++) {
				uint32_t v = s[px];
				uint32_t rb = (((v & 0x00FF00FFu) * op) >> 8) & 0x00FF00FFu;
				uint32_t ag = (((v >> 8) & 0x00FF00FFu) * op) & 0xFF00FF00u;
				row[px] = px_over(row[px], rb | ag);
			}
		}
	}
}

void gfx_copy(struct canvas *dst, int dx, int dy, const struct canvas *src, struct rect sr)
{
	sr = rect_intersect(sr, (struct rect){ 0, 0, src->w, src->h });
	struct rect dr = rect_intersect((struct rect){ dx, dy, sr.w, sr.h }, dst->clip);
	if (rect_empty(dr))
		return;
	int ox = dr.x - dx, oy = dr.y - dy;
	for (int y = 0; y < dr.h; y++)
		memcpy(dst->px + (size_t)(dr.y + y) * dst->stride + dr.x,
		       src->px + (size_t)(sr.y + oy + y) * src->stride + sr.x + ox, (size_t)dr.w * 4);
}

void gfx_draw_canvas(struct canvas *dst, const struct canvas *src, int dx, int dy, float opacity)
{
	gfx_blit(dst, src->px, src->w, src->h, src->stride, dx, dy, opacity);
}
