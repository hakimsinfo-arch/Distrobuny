/*
 * Distro Buny — visual theme
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "interface/theme.h"

#include <stdlib.h>

#include "common/log.h"
#include "common/util.h"

void theme_light(struct theme *t)
{
	*t = (struct theme){
		.bg_top = HEXC(0xF7FAF3),
		.bg_bottom = HEXC(0xEEF3E8),
		.surface = HEXC(0xFFFFFF),
		.surface_hover = HEXC(0xFBFDF8),
		.surface_alt = HEXC(0xF3F7EE),
		.border = HEXC(0xE1E8D8),
		.shadow = RGBA(38, 58, 24, 30),
		.text = HEXC(0x1D2817),
		.text2 = HEXC(0x56634E),
		.text3 = HEXC(0x88937F),
		.brand = HEXC(0x9CC66F),
		.accent = HEXC(0x4E8A2C),
		.accent_text = HEXC(0xFFFFFF),
		.accent_soft = HEXC(0xEAF4DE),
		.focus = HEXC(0x6FAA42),
		.ok = HEXC(0x3E8E2F),
		.warn = HEXC(0xB7791F),
		.danger = HEXC(0xC2413B),
		.info = HEXC(0x3B7EA8),
		.ok_soft = HEXC(0xE6F3DF),
		.warn_soft = HEXC(0xFBF0DC),
		.danger_soft = HEXC(0xF9E3E1),
		.scrim = RGBA(20, 28, 16, 110),
		.dark = false,
	};
}

void theme_dark(struct theme *t)
{
	*t = (struct theme){
		.bg_top = HEXC(0x151B12),
		.bg_bottom = HEXC(0x10150D),
		.surface = HEXC(0x1E2619),
		.surface_hover = HEXC(0x242E1F),
		.surface_alt = HEXC(0x1A2116),
		.border = HEXC(0x2E3A27),
		.shadow = RGBA(0, 0, 0, 90),
		.text = HEXC(0xE9F0E2),
		.text2 = HEXC(0xA9B6A0),
		.text3 = HEXC(0x76836E),
		.brand = HEXC(0x9CC66F),
		.accent = HEXC(0x8CC45A),
		.accent_text = HEXC(0x10170C),
		.accent_soft = HEXC(0x26341D),
		.focus = HEXC(0xA8D47C),
		.ok = HEXC(0x7FC46A),
		.warn = HEXC(0xE0AE55),
		.danger = HEXC(0xEE7D73),
		.info = HEXC(0x7DB8DE),
		.ok_soft = HEXC(0x23331D),
		.warn_soft = HEXC(0x3A3020),
		.danger_soft = HEXC(0x3D2422),
		.scrim = RGBA(0, 0, 0, 150),
		.dark = true,
	};
}

int fonts_load(struct fonts *f, const char *dir)
{
	struct {
		struct font **slot;
		const char *file;
	} want[] = {
		{ &f->regular, "Inter-Regular.ttf" },
		{ &f->medium, "Inter-Medium.ttf" },
		{ &f->semibold, "Inter-SemiBold.ttf" },
		{ &f->bold, "Inter-Bold.ttf" },
	};
	int missing = 0;
	for (size_t i = 0; i < ARRAY_SIZE(want); i++) {
		char *p = path_join(dir, want[i].file);
		*want[i].slot = font_load(p);
		if (!*want[i].slot) {
			LOGW("font %s could not be loaded", p);
			missing++;
		}
		free(p);
	}
	/* fall back to any loaded face so text always renders */
	struct font *any = f->regular ? f->regular : f->medium ? f->medium : f->semibold ? f->semibold : f->bold;
	if (!any)
		return -1;
	for (size_t i = 0; i < ARRAY_SIZE(want); i++)
		if (!*want[i].slot)
			*want[i].slot = any;
	return missing ? 1 : 0;
}
