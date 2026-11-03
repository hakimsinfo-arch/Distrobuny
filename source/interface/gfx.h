/*
 * Distro Buny — software rendering canvas
 *
 * Pixels are 32-bit BGRA (little-endian XRGB8888 when opaque), premultiplied
 * alpha.  All drawing honours the canvas clip rectangle.  Shapes are
 * anti-aliased with analytic coverage.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BUNY_GFX_H
#define BUNY_GFX_H

#include <stdbool.h>
#include <stdint.h>

typedef uint32_t color_t; /* 0xAARRGGBB, straight (not premultiplied) alpha */

#define RGB(r, g, b) ((color_t)(0xFF000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b)))
#define RGBA(r, g, b, a) ((color_t)(((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b)))
#define HEXC(x) ((color_t)(0xFF000000u | (uint32_t)(x)))

static inline color_t color_alpha(color_t c, float a)
{
	uint32_t al = (uint32_t)((float)(c >> 24) * (a < 0 ? 0 : a > 1 ? 1 : a) + 0.5f);
	return (c & 0x00FFFFFFu) | (al << 24);
}

color_t color_mix(color_t a, color_t b, float t);

struct rect {
	int x, y, w, h;
};

static inline bool rect_contains(struct rect r, int x, int y)
{
	return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}
struct rect rect_intersect(struct rect a, struct rect b);
struct rect rect_union(struct rect a, struct rect b);
static inline bool rect_empty(struct rect r) { return r.w <= 0 || r.h <= 0; }

struct canvas {
	uint32_t *px;
	int w, h;
	int stride; /* in pixels */
	struct rect clip;
	bool owns;
};

struct canvas *canvas_new(int w, int h);
void canvas_free(struct canvas *c);
void canvas_wrap(struct canvas *c, uint32_t *px, int w, int h, int stride);
void canvas_set_clip(struct canvas *c, struct rect r);
void canvas_reset_clip(struct canvas *c);
struct rect canvas_push_clip(struct canvas *c, struct rect r); /* returns previous clip */

void gfx_clear(struct canvas *c, color_t col);
void gfx_fill_rect(struct canvas *c, struct rect r, color_t col);
/* Anti-aliased rounded rectangle with fractional geometry. */
void gfx_fill_rrect(struct canvas *c, float x, float y, float w, float h, float radius, color_t col);
void gfx_stroke_rrect(struct canvas *c, float x, float y, float w, float h, float radius, float width, color_t col);
void gfx_fill_circle(struct canvas *c, float cx, float cy, float r, color_t col);
void gfx_stroke_circle(struct canvas *c, float cx, float cy, float r, float width, color_t col);
/* Vertical gradient. */
void gfx_fill_vgradient(struct canvas *c, struct rect r, color_t top, color_t bottom);
/* Soft drop shadow of a rounded rectangle (cached blur). */
void gfx_shadow(struct canvas *c, float x, float y, float w, float h, float radius, float blur, float dy, color_t col);
/* Thick anti-aliased line segment with round caps. */
void gfx_line(struct canvas *c, float x0, float y0, float x1, float y1, float width, color_t col);
/* Blend an alpha mask (8-bit coverage) tinted with col at (x, y). */
void gfx_blit_mask(struct canvas *c, const uint8_t *mask, int mw, int mh, int mstride, int x, int y, color_t col);
/* Composite a premultiplied BGRA bitmap with global opacity. */
void gfx_blit(struct canvas *c, const uint32_t *src, int sw, int sh, int sstride, int x, int y, float opacity);
/* Copy a canvas region (opaque copy). */
void gfx_copy(struct canvas *dst, int dx, int dy, const struct canvas *src, struct rect sr);
/* Draw a canvas onto another with opacity (premultiplied blend). */
void gfx_draw_canvas(struct canvas *dst, const struct canvas *src, int dx, int dy, float opacity);

/* Pixel blend helper (premultiplied src over dst). */
static inline uint32_t px_over(uint32_t dst, uint32_t src)
{
	uint32_t sa = src >> 24;
	if (sa == 255)
		return src;
	if (sa == 0)
		return dst;
	uint32_t ia = 255 - sa;
	uint32_t rb = ((dst & 0x00FF00FFu) * ia + 0x00800080u);
	rb = ((rb + ((rb >> 8) & 0x00FF00FFu)) >> 8) & 0x00FF00FFu;
	uint32_t ag = (((dst >> 8) & 0x00FF00FFu) * ia + 0x00800080u);
	ag = ((ag + ((ag >> 8) & 0x00FF00FFu))) & 0xFF00FF00u;
	return src + rb + ag;
}

/* Premultiply a straight colour with an extra coverage factor (0..255). */
static inline uint32_t premul(color_t c, uint32_t cov)
{
	uint32_t a = ((c >> 24) * cov + 127) / 255;
	uint32_t r = (((c >> 16) & 0xFF) * a + 127) / 255;
	uint32_t g = (((c >> 8) & 0xFF) * a + 127) / 255;
	uint32_t b = ((c & 0xFF) * a + 127) / 255;
	return (a << 24) | (r << 16) | (g << 8) | b;
}

#endif
