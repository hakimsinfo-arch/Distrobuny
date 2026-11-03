/*
 * Distro Buny — images
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "interface/image.h"

#include <stdlib.h>
#include <string.h>

#include "common/util.h"

static uint32_t rd32(const unsigned char *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

struct image *image_load(const char *path)
{
	size_t len;
	unsigned char *d = (unsigned char *)read_file(path, 64u << 20, &len);
	if (!d)
		return NULL;
	if (len < 12 || memcmp(d, "BIMG", 4) != 0 || rd32(d + 4) != 1) {
		free(d);
		return NULL;
	}
	uint32_t n = rd32(d + 8);
	if (n == 0 || n > 16) {
		free(d);
		return NULL;
	}
	struct image *im = xcalloc(1, sizeof(*im));
	im->levels = xcalloc(n, sizeof(struct image_level));
	size_t off = 12;
	for (uint32_t i = 0; i < n; i++) {
		if (off + 8 > len)
			goto bad;
		uint32_t w = rd32(d + off), h = rd32(d + off + 4);
		off += 8;
		if (w == 0 || h == 0 || w > 8192 || h > 8192 || off + (size_t)w * h * 4 > len)
			goto bad;
		im->levels[i].w = (int)w;
		im->levels[i].h = (int)h;
		im->levels[i].px = xmalloc((size_t)w * h * 4);
		memcpy(im->levels[i].px, d + off, (size_t)w * h * 4);
		off += (size_t)w * h * 4;
		im->nlevels++;
	}
	free(d);
	return im;
bad:
	free(d);
	image_free(im);
	return NULL;
}

void image_free(struct image *im)
{
	if (!im)
		return;
	for (int i = 0; i < im->nlevels; i++)
		free(im->levels[i].px);
	free(im->levels);
	free(im->cpx);
	free(im);
}

static uint32_t sample(const struct image_level *l, float fx, float fy)
{
	fx = BUNY_CLAMP(fx, 0.0f, (float)(l->w - 1));
	fy = BUNY_CLAMP(fy, 0.0f, (float)(l->h - 1));
	int x0 = (int)fx, y0 = (int)fy;
	int x1 = BUNY_MIN(x0 + 1, l->w - 1), y1 = BUNY_MIN(y0 + 1, l->h - 1);
	float tx = fx - (float)x0, ty = fy - (float)y0;
	uint32_t p00 = l->px[(size_t)y0 * l->w + x0], p10 = l->px[(size_t)y0 * l->w + x1];
	uint32_t p01 = l->px[(size_t)y1 * l->w + x0], p11 = l->px[(size_t)y1 * l->w + x1];
	uint32_t out = 0;
	for (int s = 0; s < 32; s += 8) {
		float a = (float)((p00 >> s) & 0xFF) * (1 - tx) + (float)((p10 >> s) & 0xFF) * tx;
		float b = (float)((p01 >> s) & 0xFF) * (1 - tx) + (float)((p11 >> s) & 0xFF) * tx;
		out |= (uint32_t)(a * (1 - ty) + b * ty + 0.5f) << s;
	}
	return out;
}

void image_draw(struct canvas *c, struct image *im, int x, int y, int w, int h, float opacity)
{
	if (!im || w <= 0 || h <= 0)
		return;
	if (im->cw != w || im->ch != h || !im->cpx) {
		/* choose the smallest level that is at least the target size */
		const struct image_level *src = &im->levels[0];
		for (int i = 0; i < im->nlevels; i++)
			if (im->levels[i].w >= w && im->levels[i].h >= h && im->levels[i].w < src->w)
				src = &im->levels[i];
		free(im->cpx);
		im->cpx = xmalloc((size_t)w * h * 4);
		float sx = (float)src->w / (float)w, sy = (float)src->h / (float)h;
		/* box-filter when shrinking by more than 2x, bilinear otherwise */
		int bx = sx > 2 ? (int)sx : 1, byy = sy > 2 ? (int)sy : 1;
		for (int py = 0; py < h; py++)
			for (int px = 0; px < w; px++) {
				if (bx == 1 && byy == 1) {
					im->cpx[(size_t)py * w + px] = sample(src, ((float)px + 0.5f) * sx - 0.5f, ((float)py + 0.5f) * sy - 0.5f);
					continue;
				}
				uint32_t acc[4] = { 0 };
				int n = 0;
				for (int ky = 0; ky < byy; ky++)
					for (int kx = 0; kx < bx; kx++) {
						int sxp = BUNY_MIN((int)((float)px * sx) + kx, src->w - 1);
						int syp = BUNY_MIN((int)((float)py * sy) + ky, src->h - 1);
						uint32_t v = src->px[(size_t)syp * src->w + sxp];
						for (int k = 0; k < 4; k++)
							acc[k] += (v >> (8 * k)) & 0xFF;
						n++;
					}
				uint32_t out = 0;
				for (int k = 0; k < 4; k++)
					out |= ((acc[k] + (uint32_t)n / 2) / (uint32_t)n) << (8 * k);
				im->cpx[(size_t)py * w + px] = out;
			}
		im->cw = w;
		im->ch = h;
	}
	gfx_blit(c, im->cpx, w, h, w, x, y, opacity);
}
