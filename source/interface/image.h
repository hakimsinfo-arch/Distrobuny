/*
 * Distro Buny — images (BIMG, see source/python/distrobuny/build/assets.py)
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BUNY_IMAGE_H
#define BUNY_IMAGE_H

#include "interface/gfx.h"

struct image_level {
	int w, h;
	uint32_t *px; /* premultiplied BGRA */
};

struct image {
	struct image_level *levels;
	int nlevels;
	/* last scaled rendition (cache) */
	int cw, ch;
	uint32_t *cpx;
};

struct image *image_load(const char *path);
void image_free(struct image *im);
/* Draw scaled to w x h (high quality: picks the closest larger level, then bilinear). */
void image_draw(struct canvas *c, struct image *im, int x, int y, int w, int h, float opacity);

#endif
