/*
 * Distro Buny — visual theme
 *
 * Colours derive from the official logo's spring green (#9CC66F).  Text and
 * interactive accents use darker shades of the same hue so they meet
 * WCAG AA contrast on the light surfaces.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BUNY_THEME_H
#define BUNY_THEME_H

#include "interface/font.h"
#include "interface/gfx.h"

struct theme {
	color_t bg_top, bg_bottom;   /* window background (very subtle vertical gradient) */
	color_t surface;             /* cards */
	color_t surface_hover;
	color_t surface_alt;         /* secondary panels */
	color_t border;
	color_t shadow;
	color_t text, text2, text3;  /* primary, secondary, tertiary */
	color_t brand;               /* logo green */
	color_t accent;              /* interactive green (darker) */
	color_t accent_text;         /* text on accent */
	color_t accent_soft;         /* selected / tinted backgrounds */
	color_t focus;               /* focus ring */
	color_t ok, warn, danger, info;
	color_t ok_soft, warn_soft, danger_soft;
	color_t scrim;               /* modal backdrop */
	bool dark;
};

void theme_light(struct theme *t);
void theme_dark(struct theme *t);

struct fonts {
	struct font *regular, *medium, *semibold, *bold;
};

int fonts_load(struct fonts *f, const char *dir);

#endif
