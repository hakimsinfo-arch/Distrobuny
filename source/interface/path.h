/*
 * Distro Buny — vector paths
 *
 * Paths are flattened to line segments and rasterised with exact signed-area
 * coverage accumulation (each segment deposits the area it covers in every
 * pixel cell; a prefix sum per row yields coverage).  This gives the same
 * quality as font rasterisers and handles holes formed by opposite winding.
 * Used for distribution marks (SVG path data) and interface icons.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BUNY_PATH_H
#define BUNY_PATH_H

#include <stddef.h>
#include <stdint.h>

#include "interface/gfx.h"

struct pt {
	float x, y;
};

struct path {
	struct pt *pts;      /* flattened polyline points */
	int *starts;         /* index of the first point of each subpath */
	size_t npts, nsub, cap, subcap;
	struct pt cur, start;
	float tol;           /* flattening tolerance */
};

void path_init(struct path *p);
void path_free(struct path *p);
void path_move(struct path *p, float x, float y);
void path_line(struct path *p, float x, float y);
void path_quad(struct path *p, float cx, float cy, float x, float y);
void path_cubic(struct path *p, float c1x, float c1y, float c2x, float c2y, float x, float y);
void path_arc_to(struct path *p, float rx, float ry, float rot_deg, int large, int sweep, float x, float y);
void path_close(struct path *p);
void path_circle(struct path *p, float cx, float cy, float r);
void path_rrect(struct path *p, float x, float y, float w, float h, float r);

/* Parse SVG path data ("M10 10 L20 20 ...").  Returns 0 on success. */
int path_parse_svg(struct path *p, const char *d);

/* Rasterise into an 8-bit coverage mask of size w x h, transforming points by
 * scale then offset.  Caller frees *out. */
uint8_t *path_raster(const struct path *p, int w, int h, float scale, float ox, float oy);
/* Fill directly onto a canvas. */
void path_fill(struct canvas *c, const struct path *p, float scale, float ox, float oy, color_t col);
/* Stroke a path (polyline of each subpath) with round joins. */
void path_stroke(struct canvas *c, const struct path *p, float scale, float ox, float oy, float width, color_t col);

#endif
