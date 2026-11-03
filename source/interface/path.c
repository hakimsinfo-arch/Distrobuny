/*
 * Distro Buny — vector paths and coverage rasteriser
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "interface/path.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "common/util.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void path_init(struct path *p)
{
	memset(p, 0, sizeof(*p));
	p->tol = 0.05f;
}

void path_free(struct path *p)
{
	free(p->pts);
	free(p->starts);
	memset(p, 0, sizeof(*p));
}

static void add_pt(struct path *p, float x, float y)
{
	if (p->npts == p->cap) {
		p->cap = p->cap ? p->cap * 2 : 64;
		p->pts = xrealloc(p->pts, p->cap * sizeof(struct pt));
	}
	p->pts[p->npts++] = (struct pt){ x, y };
	p->cur = (struct pt){ x, y };
}

void path_move(struct path *p, float x, float y)
{
	if (p->nsub == p->subcap) {
		p->subcap = p->subcap ? p->subcap * 2 : 8;
		p->starts = xrealloc(p->starts, p->subcap * sizeof(int));
	}
	p->starts[p->nsub++] = (int)p->npts;
	add_pt(p, x, y);
	p->start = p->cur;
}

void path_line(struct path *p, float x, float y)
{
	if (!p->nsub)
		path_move(p, p->cur.x, p->cur.y);
	add_pt(p, x, y);
}

void path_close(struct path *p)
{
	if (p->nsub && (p->cur.x != p->start.x || p->cur.y != p->start.y))
		add_pt(p, p->start.x, p->start.y);
	p->cur = p->start;
}

void path_quad(struct path *p, float cx, float cy, float x, float y)
{
	struct pt a = p->cur;
	float dd = fabsf(a.x - 2 * cx + x) + fabsf(a.y - 2 * cy + y);
	int n = (int)ceilf(sqrtf(dd / (p->tol * 8.0f) + 1.0f));
	n = BUNY_CLAMP(n, 1, 64);
	for (int i = 1; i <= n; i++) {
		float t = (float)i / (float)n, u = 1 - t;
		path_line(p, u * u * a.x + 2 * u * t * cx + t * t * x, u * u * a.y + 2 * u * t * cy + t * t * y);
	}
}

void path_cubic(struct path *p, float c1x, float c1y, float c2x, float c2y, float x, float y)
{
	struct pt a = p->cur;
	float dd = fabsf(a.x - 2 * c1x + c2x) + fabsf(a.y - 2 * c1y + c2y) + fabsf(c1x - 2 * c2x + x) +
		   fabsf(c1y - 2 * c2y + y);
	int n = (int)ceilf(sqrtf(dd / (p->tol * 4.0f) + 1.0f));
	n = BUNY_CLAMP(n, 1, 96);
	for (int i = 1; i <= n; i++) {
		float t = (float)i / (float)n, u = 1 - t;
		float b0 = u * u * u, b1 = 3 * u * u * t, b2 = 3 * u * t * t, b3 = t * t * t;
		path_line(p, b0 * a.x + b1 * c1x + b2 * c2x + b3 * x, b0 * a.y + b1 * c1y + b2 * c2y + b3 * y);
	}
}

/* SVG elliptical arc (implementation notes, SVG 1.1 appendix F.6). */
void path_arc_to(struct path *p, float rx, float ry, float rot_deg, int large, int sweep, float x, float y)
{
	float x0 = p->cur.x, y0 = p->cur.y;
	if (rx == 0 || ry == 0 || (x0 == x && y0 == y)) {
		path_line(p, x, y);
		return;
	}
	rx = fabsf(rx);
	ry = fabsf(ry);
	double phi = rot_deg * M_PI / 180.0, cp = cos(phi), sp = sin(phi);
	double dx = (x0 - x) / 2.0, dy = (y0 - y) / 2.0;
	double x1p = cp * dx + sp * dy, y1p = -sp * dx + cp * dy;
	double lam = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
	if (lam > 1) {
		rx *= (float)sqrt(lam);
		ry *= (float)sqrt(lam);
	}
	double num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
	double den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
	double co = den > 0 ? sqrt(fmax(0.0, num / den)) : 0;
	if (large == sweep)
		co = -co;
	double cxp = co * rx * y1p / ry, cyp = -co * ry * x1p / rx;
	double cx = cp * cxp - sp * cyp + (x0 + x) / 2.0, cy = sp * cxp + cp * cyp + (y0 + y) / 2.0;
	double ux = (x1p - cxp) / rx, uy = (y1p - cyp) / ry, vx = (-x1p - cxp) / rx, vy = (-y1p - cyp) / ry;
	double th1 = atan2(uy, ux);
	double dth = atan2(ux * vy - uy * vx, ux * vx + uy * vy);
	if (!sweep && dth > 0)
		dth -= 2 * M_PI;
	else if (sweep && dth < 0)
		dth += 2 * M_PI;
	int n = (int)ceil(fabs(dth) * fmax(rx, ry) / (p->tol * 20.0) + 4);
	n = BUNY_CLAMP(n, 4, 128);
	for (int i = 1; i <= n; i++) {
		double t = th1 + dth * i / n;
		double ex = rx * cos(t), ey = ry * sin(t);
		path_line(p, (float)(cp * ex - sp * ey + cx), (float)(sp * ex + cp * ey + cy));
	}
}

void path_circle(struct path *p, float cx, float cy, float r)
{
	path_move(p, cx + r, cy);
	path_arc_to(p, r, r, 0, 0, 1, cx - r, cy);
	path_arc_to(p, r, r, 0, 0, 1, cx + r, cy);
	path_close(p);
}

void path_rrect(struct path *p, float x, float y, float w, float h, float r)
{
	r = fminf(r, fminf(w, h) / 2);
	path_move(p, x + r, y);
	path_line(p, x + w - r, y);
	path_arc_to(p, r, r, 0, 0, 1, x + w, y + r);
	path_line(p, x + w, y + h - r);
	path_arc_to(p, r, r, 0, 0, 1, x + w - r, y + h);
	path_line(p, x + r, y + h);
	path_arc_to(p, r, r, 0, 0, 1, x, y + h - r);
	path_line(p, x, y + r);
	path_arc_to(p, r, r, 0, 0, 1, x + r, y);
	path_close(p);
}

/* ------------------------------------------------------- SVG path data */

struct svgp {
	const char *s;
};

static void skip_sep(struct svgp *s)
{
	while (*s->s && (isspace((unsigned char)*s->s) || *s->s == ','))
		s->s++;
}

static int read_num(struct svgp *s, float *out)
{
	skip_sep(s);
	const char *start = s->s;
	const char *q = s->s;
	if (*q == '+' || *q == '-')
		q++;
	bool digits = false, dot = false;
	while (isdigit((unsigned char)*q) || (*q == '.' && !dot)) {
		if (*q == '.')
			dot = true;
		else
			digits = true;
		q++;
	}
	if (!digits)
		return -1;
	if (*q == 'e' || *q == 'E') {
		const char *e = q + 1;
		if (*e == '+' || *e == '-')
			e++;
		if (isdigit((unsigned char)*e)) {
			q = e;
			while (isdigit((unsigned char)*q))
				q++;
		}
	}
	char buf[64];
	size_t n = (size_t)(q - start);
	if (n >= sizeof(buf))
		return -1;
	memcpy(buf, start, n);
	buf[n] = '\0';
	*out = strtof(buf, NULL);
	s->s = q;
	return 0;
}

static int read_flag(struct svgp *s, int *out)
{
	skip_sep(s);
	if (*s->s != '0' && *s->s != '1')
		return -1;
	*out = *s->s - '0';
	s->s++;
	return 0;
}

int path_parse_svg(struct path *p, const char *d)
{
	struct svgp s = { d };
	char cmd = 0;
	struct pt ctrl = { 0, 0 };   /* last control point for S/T */
	char lastcmd = 0;
	int guard = 0;
	while (*s.s && guard++ < 200000) {
		skip_sep(&s);
		if (!*s.s)
			break;
		if (isalpha((unsigned char)*s.s)) {
			cmd = *s.s++;
		} else if (!cmd) {
			return -1;
		}
		bool rel = islower((unsigned char)cmd);
		float ox = rel ? p->cur.x : 0, oy = rel ? p->cur.y : 0;
		float a[7];
		switch (toupper((unsigned char)cmd)) {
		case 'M':
			if (read_num(&s, &a[0]) || read_num(&s, &a[1]))
				return -1;
			path_move(p, ox + a[0], oy + a[1]);
			cmd = rel ? 'l' : 'L'; /* subsequent pairs are lineto */
			break;
		case 'L':
			if (read_num(&s, &a[0]) || read_num(&s, &a[1]))
				return -1;
			path_line(p, ox + a[0], oy + a[1]);
			break;
		case 'H':
			if (read_num(&s, &a[0]))
				return -1;
			path_line(p, ox + a[0], p->cur.y);
			break;
		case 'V':
			if (read_num(&s, &a[0]))
				return -1;
			path_line(p, p->cur.x, oy + a[0]);
			break;
		case 'C':
			for (int i = 0; i < 6; i++)
				if (read_num(&s, &a[i]))
					return -1;
			path_cubic(p, ox + a[0], oy + a[1], ox + a[2], oy + a[3], ox + a[4], oy + a[5]);
			ctrl = (struct pt){ ox + a[2], oy + a[3] };
			break;
		case 'S': {
			for (int i = 0; i < 4; i++)
				if (read_num(&s, &a[i]))
					return -1;
			struct pt c1 = p->cur;
			if (lastcmd == 'C' || lastcmd == 'S')
				c1 = (struct pt){ 2 * p->cur.x - ctrl.x, 2 * p->cur.y - ctrl.y };
			path_cubic(p, c1.x, c1.y, ox + a[0], oy + a[1], ox + a[2], oy + a[3]);
			ctrl = (struct pt){ ox + a[0], oy + a[1] };
			break;
		}
		case 'Q':
			for (int i = 0; i < 4; i++)
				if (read_num(&s, &a[i]))
					return -1;
			path_quad(p, ox + a[0], oy + a[1], ox + a[2], oy + a[3]);
			ctrl = (struct pt){ ox + a[0], oy + a[1] };
			break;
		case 'T': {
			if (read_num(&s, &a[0]) || read_num(&s, &a[1]))
				return -1;
			struct pt c = p->cur;
			if (lastcmd == 'Q' || lastcmd == 'T')
				c = (struct pt){ 2 * p->cur.x - ctrl.x, 2 * p->cur.y - ctrl.y };
			path_quad(p, c.x, c.y, ox + a[0], oy + a[1]);
			ctrl = c;
			break;
		}
		case 'A': {
			int la, sw;
			if (read_num(&s, &a[0]) || read_num(&s, &a[1]) || read_num(&s, &a[2]) || read_flag(&s, &la) ||
			    read_flag(&s, &sw) || read_num(&s, &a[3]) || read_num(&s, &a[4]))
				return -1;
			path_arc_to(p, a[0], a[1], a[2], la, sw, ox + a[3], oy + a[4]);
			break;
		}
		case 'Z':
			path_close(p);
			break;
		default:
			return -1;
		}
		lastcmd = (char)toupper((unsigned char)cmd);
	}
	return 0;
}

/* --------------------------------------------------------- rasteriser */

/* Accumulate the signed area of one line segment into the cell buffer
 * (algorithm after font-rs / libart's "area + cover" accumulation). */
static void accumulate_line(float *acc, int w, int h, float x0, float y0, float x1, float y1)
{
	if (y0 == y1)
		return;
	float dir = 1.0f;
	if (y0 > y1) {
		dir = -1.0f;
		float t;
		t = x0; x0 = x1; x1 = t;
		t = y0; y0 = y1; y1 = t;
	}
	float dxdy = (x1 - x0) / (y1 - y0);
	float x = x0;
	if (y0 < 0) {
		x -= y0 * dxdy;
		y0 = 0;
	}
	if (y1 > (float)h)
		y1 = (float)h;
	if (y0 >= y1)
		return;
	int ystart = (int)y0;
	int yend = (int)ceilf(y1);
	for (int y = ystart; y < yend; y++) {
		float linestart = (float)y;
		float dy = fminf((float)(y + 1), y1) - fmaxf(linestart, y0);
		float xnext = x + dxdy * dy;
		float d = dy * dir;
		float xa = fminf(x, xnext), xb = fmaxf(x, xnext);
		float *row = acc + (size_t)y * (size_t)(w + 2);
		if (xb < 0) {
			row[0] += d;
		} else {
			/* callers keep geometry inside [0, w); clamp rounding noise */
			if (xa < 0)
				xa = 0;
			int x0i = (int)xa;
			float x0floor = (float)x0i;
			int x1i = (int)ceilf(xb);
			if (x0i >= w) {
				row[w] += d; /* entirely to the right: no effect inside */
			} else if (x1i <= x0i + 1) {
				float xmf = 0.5f * (xa + xb) - x0floor;
				row[x0i] += d - d * xmf;
				row[x0i + 1] += d * xmf;
			} else {
				float s = 1.0f / (xb - xa);
				float x0f = xa - x0floor;
				float a0 = 0.5f * s * (1.0f - x0f) * (1.0f - x0f);
				float x1f = xb - (float)x1i + 1.0f;
				float am = 0.5f * s * x1f * x1f;
				row[x0i] += d * a0;
				if (x1i == x0i + 2) {
					row[x0i + 1] += d * (1.0f - a0 - am);
				} else {
					float a1 = s * (1.5f - x0f);
					row[x0i + 1] += d * (a1 - a0);
					for (int xi = x0i + 2; xi < x1i - 1 && xi <= w; xi++)
						row[xi] += d * s;
					float a2 = a1 + (float)(x1i - x0i - 3) * s;
					if (x1i - 1 <= w)
						row[x1i - 1] += d * (1.0f - a2 - am);
				}
				if (x1i <= w)
					row[x1i] += d * am;
			}
		}
		x = xnext;
	}
}

uint8_t *path_raster(const struct path *p, int w, int h, float scale, float ox, float oy)
{
	if (w <= 0 || h <= 0)
		return NULL;
	size_t aw = (size_t)w + 2;
	float *acc = xcalloc(aw * (size_t)h + 4, sizeof(float));
	for (size_t s = 0; s < p->nsub; s++) {
		size_t a = (size_t)p->starts[s];
		size_t b = s + 1 < p->nsub ? (size_t)p->starts[s + 1] : p->npts;
		if (b - a < 2)
			continue;
		for (size_t i = a; i + 1 < b; i++)
			accumulate_line(acc, w, h, p->pts[i].x * scale + ox, p->pts[i].y * scale + oy,
					p->pts[i + 1].x * scale + ox, p->pts[i + 1].y * scale + oy);
		/* implicitly close every subpath */
		accumulate_line(acc, w, h, p->pts[b - 1].x * scale + ox, p->pts[b - 1].y * scale + oy,
				p->pts[a].x * scale + ox, p->pts[a].y * scale + oy);
	}
	uint8_t *mask = xmalloc((size_t)w * (size_t)h);
	for (int y = 0; y < h; y++) {
		float sum = 0;
		float *row = acc + (size_t)y * aw;
		for (int x = 0; x < w; x++) {
			sum += row[x];
			float v = fabsf(sum);
			if (v > 1.0f)
				v = 1.0f;
			mask[(size_t)y * w + x] = (uint8_t)(v * 255.0f + 0.5f);
		}
	}
	free(acc);
	return mask;
}

void path_fill(struct canvas *c, const struct path *p, float scale, float ox, float oy, color_t col)
{
	/* rasterise only the clipped bounding box */
	if (!p->npts)
		return;
	float minx = p->pts[0].x, maxx = minx, miny = p->pts[0].y, maxy = miny;
	for (size_t i = 1; i < p->npts; i++) {
		minx = fminf(minx, p->pts[i].x);
		maxx = fmaxf(maxx, p->pts[i].x);
		miny = fminf(miny, p->pts[i].y);
		maxy = fmaxf(maxy, p->pts[i].y);
	}
	struct rect bb = { (int)floorf(minx * scale + ox) - 1, (int)floorf(miny * scale + oy) - 1, 0, 0 };
	bb.w = (int)ceilf(maxx * scale + ox) + 2 - bb.x;
	bb.h = (int)ceilf(maxy * scale + oy) + 2 - bb.y;
	if (rect_empty(rect_intersect(bb, c->clip)))
		return;
	/* Rasterise the whole shape (geometry must lie inside the raster for exact
	 * coverage); the blit applies the canvas clip. */
	uint8_t *m = path_raster(p, bb.w, bb.h, scale, ox - (float)bb.x, oy - (float)bb.y);
	gfx_blit_mask(c, m, bb.w, bb.h, bb.w, bb.x, bb.y, col);
	free(m);
}

void path_stroke(struct canvas *c, const struct path *p, float scale, float ox, float oy, float width, color_t col)
{
	for (size_t s = 0; s < p->nsub; s++) {
		size_t a = (size_t)p->starts[s];
		size_t b = s + 1 < p->nsub ? (size_t)p->starts[s + 1] : p->npts;
		for (size_t i = a; i + 1 < b; i++)
			gfx_line(c, p->pts[i].x * scale + ox, p->pts[i].y * scale + oy, p->pts[i + 1].x * scale + ox,
				 p->pts[i + 1].y * scale + oy, width, col);
	}
}
