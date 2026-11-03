/*
 * Distro Buny — text rendering (stb_truetype)
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BUNY_FONT_H
#define BUNY_FONT_H

#include <stdbool.h>

#include "interface/gfx.h"

struct font;

struct font *font_load(const char *path);
void font_free(struct font *f);

struct text_style {
	struct font *font;
	float size;     /* pixel height (em) */
};

struct font_metrics {
	float ascent, descent, line_height;
};

void font_metrics(const struct text_style *st, struct font_metrics *m);
float text_width(const struct text_style *st, const char *utf8);
/* Draw text with its baseline at y; returns the advance width. */
float text_draw(struct canvas *c, const struct text_style *st, float x, float y, const char *utf8, color_t col);
/* Copy of text shortened with an ellipsis to fit max_w (caller frees). */
char *text_ellipsize(const struct text_style *st, const char *utf8, float max_w);
/* Draw text wrapped to max_w; returns the number of lines drawn.  max_lines 0 = unlimited. */
int text_draw_wrapped(struct canvas *c, const struct text_style *st, float x, float y, float max_w, const char *utf8,
		      color_t col, int max_lines, float line_spacing);
int text_count_lines(const struct text_style *st, float max_w, const char *utf8);
/* Decode one UTF-8 code point; advances *s.  Invalid bytes yield U+FFFD. */
unsigned utf8_next(const char **s);

#endif
