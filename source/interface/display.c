/*
 * Distro Buny — display output (DRM dumb buffers, fbdev, offscreen)
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "interface/display.h"

#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "common/log.h"
#include "common/util.h"

enum backend { BK_DRM, BK_FBDEV, BK_OFFSCREEN };

struct display {
	enum backend bk;
	int fd;
	int w, h;
	uint8_t *map;
	size_t map_len;
	uint32_t pitch;      /* bytes */
	int bpp;
	/* DRM */
	uint32_t fb_id, handle, crtc_id, conn_id;
	struct drm_mode_modeinfo mode;
};

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;
	do
		r = ioctl(fd, req, arg);
	while (r < 0 && (errno == EINTR || errno == EAGAIN));
	return r;
}

/* ------------------------------------------------------------------ DRM */

static int drm_try(struct display *d, const char *path)
{
	int fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return -1;
	struct drm_mode_card_res res = { 0 };
	if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0 || !res.count_connectors || !res.count_crtcs) {
		close(fd);
		return -1;
	}
	uint32_t *conns = xcalloc(res.count_connectors, sizeof(uint32_t));
	uint32_t *crtcs = xcalloc(res.count_crtcs, sizeof(uint32_t));
	uint32_t *encs = xcalloc(res.count_encoders ? res.count_encoders : 1, sizeof(uint32_t));
	uint32_t *fbs = xcalloc(res.count_fbs ? res.count_fbs : 1, sizeof(uint32_t));
	res.connector_id_ptr = (uint64_t)(uintptr_t)conns;
	res.crtc_id_ptr = (uint64_t)(uintptr_t)crtcs;
	res.encoder_id_ptr = (uint64_t)(uintptr_t)encs;
	res.fb_id_ptr = (uint64_t)(uintptr_t)fbs;
	int rc = -1;
	if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0)
		goto out;
	for (uint32_t i = 0; i < res.count_connectors && rc < 0; i++) {
		struct drm_mode_get_connector c = { .connector_id = conns[i] };
		if (xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) < 0 || c.connection != 1 || !c.count_modes)
			continue;
		struct drm_mode_modeinfo *modes = xcalloc(c.count_modes, sizeof(*modes));
		uint32_t *cencs = xcalloc(c.count_encoders ? c.count_encoders : 1, sizeof(uint32_t));
		uint32_t *props = xcalloc(c.count_props ? c.count_props : 1, sizeof(uint32_t));
		uint64_t *pvals = xcalloc(c.count_props ? c.count_props : 1, sizeof(uint64_t));
		c.modes_ptr = (uint64_t)(uintptr_t)modes;
		c.encoders_ptr = (uint64_t)(uintptr_t)cencs;
		c.props_ptr = (uint64_t)(uintptr_t)props;
		c.prop_values_ptr = (uint64_t)(uintptr_t)pvals;
		if (xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) == 0 && c.count_modes) {
			/* prefer the mode currently driven by the encoder's CRTC */
			uint32_t crtc = 0;
			struct drm_mode_modeinfo mode = modes[0];
			for (uint32_t m = 0; m < c.count_modes; m++)
				if (modes[m].type & DRM_MODE_TYPE_PREFERRED) {
					mode = modes[m];
					break;
				}
			if (c.encoder_id) {
				struct drm_mode_get_encoder e = { .encoder_id = c.encoder_id };
				if (xioctl(fd, DRM_IOCTL_MODE_GETENCODER, &e) == 0 && e.crtc_id) {
					crtc = e.crtc_id;
					struct drm_mode_crtc cr = { .crtc_id = crtc };
					if (xioctl(fd, DRM_IOCTL_MODE_GETCRTC, &cr) == 0 && cr.mode_valid)
						mode = cr.mode;
				}
			}
			if (!crtc && c.count_encoders) {
				struct drm_mode_get_encoder e = { .encoder_id = cencs[0] };
				if (xioctl(fd, DRM_IOCTL_MODE_GETENCODER, &e) == 0) {
					for (uint32_t k = 0; k < res.count_crtcs; k++)
						if (e.possible_crtcs & (1u << k)) {
							crtc = crtcs[k];
							break;
						}
				}
			}
			if (crtc) {
				d->crtc_id = crtc;
				d->conn_id = conns[i];
				d->mode = mode;
				rc = 0;
			}
		}
		free(modes);
		free(cencs);
		free(props);
		free(pvals);
	}
	if (rc < 0)
		goto out;
	d->w = d->mode.hdisplay;
	d->h = d->mode.vdisplay;
	struct drm_mode_create_dumb cd = { .width = (uint32_t)d->w, .height = (uint32_t)d->h, .bpp = 32 };
	if (xioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0) {
		rc = -1;
		goto out;
	}
	d->handle = cd.handle;
	d->pitch = cd.pitch;
	d->map_len = cd.size;
	struct drm_mode_fb_cmd fb = { .width = (uint32_t)d->w, .height = (uint32_t)d->h, .pitch = cd.pitch,
				      .bpp = 32, .depth = 24, .handle = cd.handle };
	if (xioctl(fd, DRM_IOCTL_MODE_ADDFB, &fb) < 0) {
		rc = -1;
		goto out;
	}
	d->fb_id = fb.fb_id;
	struct drm_mode_map_dumb md = { .handle = cd.handle };
	if (xioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md) < 0) {
		rc = -1;
		goto out;
	}
	d->map = mmap(NULL, d->map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)md.offset);
	if (d->map == MAP_FAILED) {
		d->map = NULL;
		rc = -1;
		goto out;
	}
	memset(d->map, 0, d->map_len);
	struct drm_mode_crtc set = { .crtc_id = d->crtc_id, .fb_id = d->fb_id,
				     .set_connectors_ptr = (uint64_t)(uintptr_t)&d->conn_id, .count_connectors = 1,
				     .mode = d->mode, .mode_valid = 1 };
	if (xioctl(fd, DRM_IOCTL_MODE_SETCRTC, &set) < 0) {
		LOGW("display: SETCRTC failed: %s", strerror(errno));
		rc = -1;
		goto out;
	}
	d->fd = fd;
	d->bk = BK_DRM;
	d->bpp = 32;
	LOGI("display: DRM %s %dx%d (%s)", path, d->w, d->h, d->mode.name);
out:
	free(conns);
	free(crtcs);
	free(encs);
	free(fbs);
	if (rc < 0)
		close(fd);
	return rc;
}

/* ---------------------------------------------------------------- fbdev */

static int fbdev_try(struct display *d)
{
	int fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return -1;
	struct fb_var_screeninfo v;
	struct fb_fix_screeninfo f;
	if (ioctl(fd, FBIOGET_VSCREENINFO, &v) < 0 || ioctl(fd, FBIOGET_FSCREENINFO, &f) < 0 ||
	    (v.bits_per_pixel != 32 && v.bits_per_pixel != 16)) {
		close(fd);
		return -1;
	}
	d->w = (int)v.xres;
	d->h = (int)v.yres;
	d->pitch = f.line_length;
	d->bpp = (int)v.bits_per_pixel;
	d->map_len = f.smem_len;
	d->map = mmap(NULL, d->map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (d->map == MAP_FAILED) {
		close(fd);
		d->map = NULL;
		return -1;
	}
	d->fd = fd;
	d->bk = BK_FBDEV;
	LOGI("display: fbdev %dx%d %d bpp", d->w, d->h, d->bpp);
	return 0;
}

struct display *display_open(char *err, size_t errlen)
{
	struct display *d = xcalloc(1, sizeof(*d));
	d->fd = -1;
	const char *env = getenv("BUNY_DISPLAY");
	if (env && str_starts(env, "offscreen")) {
		int w = 1920, h = 1080;
		if (sscanf(env, "offscreen:%dx%d", &w, &h) != 2 || w < 320 || h < 240 || w > 8192 || h > 8192) {
			w = 1920;
			h = 1080;
		}
		d->bk = BK_OFFSCREEN;
		d->w = w;
		d->h = h;
		return d;
	}
	for (int i = 0; i < 8; i++) {
		char p[32];
		snprintf(p, sizeof(p), "/dev/dri/card%d", i);
		if (drm_try(d, p) == 0)
			return d;
	}
	if (fbdev_try(d) == 0)
		return d;
	snprintf(err, errlen, "no usable display: neither DRM nor /dev/fb0 is available");
	free(d);
	return NULL;
}

void display_close(struct display *d)
{
	if (!d)
		return;
	if (d->map)
		munmap(d->map, d->map_len);
	if (d->fd >= 0)
		close(d->fd);
	free(d);
}

int display_width(const struct display *d) { return d->w; }
int display_height(const struct display *d) { return d->h; }

const char *display_backend(const struct display *d)
{
	return d->bk == BK_DRM ? "drm" : d->bk == BK_FBDEV ? "fbdev" : "offscreen";
}

void display_present(struct display *d, const struct canvas *frame, const struct rect *damage, int ndamage)
{
	if (d->bk == BK_OFFSCREEN || !d->map)
		return;
	struct drm_clip_rect clips[16];
	int nclips = 0;
	for (int i = 0; i < ndamage; i++) {
		struct rect r = rect_intersect(damage[i], (struct rect){ 0, 0, d->w, d->h });
		if (rect_empty(r))
			continue;
		for (int y = r.y; y < r.y + r.h; y++) {
			const uint32_t *src = frame->px + (size_t)y * frame->stride + r.x;
			uint8_t *dst = d->map + (size_t)y * d->pitch + (size_t)r.x * (size_t)(d->bpp / 8);
			if (d->bpp == 32) {
				memcpy(dst, src, (size_t)r.w * 4);
			} else {
				uint16_t *d16 = (uint16_t *)dst;
				for (int x = 0; x < r.w; x++) {
					uint32_t p = src[x];
					d16[x] = (uint16_t)(((p >> 8) & 0xF800) | ((p >> 5) & 0x07E0) | ((p >> 3) & 0x001F));
				}
			}
		}
		if (nclips < 16)
			clips[nclips++] = (struct drm_clip_rect){ (unsigned short)r.x, (unsigned short)r.y,
								  (unsigned short)(r.x + r.w), (unsigned short)(r.y + r.h) };
	}
	if (d->bk == BK_DRM && nclips) {
		struct drm_mode_fb_dirty_cmd dc = { .fb_id = d->fb_id, .num_clips = (uint32_t)nclips,
						    .clips_ptr = (uint64_t)(uintptr_t)clips };
		ioctl(d->fd, DRM_IOCTL_MODE_DIRTYFB, &dc); /* ENOSYS on drivers without damage tracking */
	}
}

/* ------------------------------------------------------------ PNG writer */

static uint32_t crc_table[256];

static void crc_init(void)
{
	for (uint32_t n = 0; n < 256; n++) {
		uint32_t c = n;
		for (int k = 0; k < 8; k++)
			c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
		crc_table[n] = c;
	}
}

static uint32_t crc32_buf(uint32_t crc, const uint8_t *b, size_t n)
{
	crc = ~crc;
	for (size_t i = 0; i < n; i++)
		crc = crc_table[(crc ^ b[i]) & 0xFF] ^ (crc >> 8);
	return ~crc;
}

static void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static int write_chunk(FILE *f, const char *type, const uint8_t *data, size_t len)
{
	uint8_t hdr[8];
	put_be32(hdr, (uint32_t)len);
	memcpy(hdr + 4, type, 4);
	uint32_t crc = crc32_buf(0, (const uint8_t *)type, 4);
	crc = crc32_buf(crc, data, len);
	uint8_t c[4];
	put_be32(c, crc);
	return fwrite(hdr, 1, 8, f) == 8 && (len == 0 || fwrite(data, 1, len, f) == len) && fwrite(c, 1, 4, f) == 4 ? 0 : -1;
}

/* PNG with uncompressed ("stored") deflate blocks: simple and dependency free. */
int display_screenshot(const struct canvas *frame, const char *path)
{
	crc_init();
	FILE *f = fopen(path, "wb");
	if (!f)
		return -1;
	static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
	fwrite(sig, 1, 8, f);
	uint8_t ihdr[13];
	put_be32(ihdr, (uint32_t)frame->w);
	put_be32(ihdr + 4, (uint32_t)frame->h);
	ihdr[8] = 8;  /* bit depth */
	ihdr[9] = 2;  /* RGB */
	ihdr[10] = 0;
	ihdr[11] = 0;
	ihdr[12] = 0;
	write_chunk(f, "IHDR", ihdr, sizeof(ihdr));
	size_t rowlen = (size_t)frame->w * 3 + 1;
	size_t raw_len = rowlen * (size_t)frame->h;
	uint8_t *raw = xmalloc(raw_len);
	for (int y = 0; y < frame->h; y++) {
		uint8_t *r = raw + (size_t)y * rowlen;
		r[0] = 0;
		for (int x = 0; x < frame->w; x++) {
			uint32_t p = frame->px[(size_t)y * frame->stride + x];
			r[1 + 3 * x] = (uint8_t)(p >> 16);
			r[2 + 3 * x] = (uint8_t)(p >> 8);
			r[3 + 3 * x] = (uint8_t)p;
		}
	}
	size_t nblocks = (raw_len + 65534) / 65535;
	size_t z_len = 2 + raw_len + nblocks * 5 + 4;
	uint8_t *z = xmalloc(z_len);
	size_t o = 0;
	z[o++] = 0x78;
	z[o++] = 0x01;
	uint32_t a = 1, b = 0;
	for (size_t i = 0; i < raw_len; i++) {
		a = (a + raw[i]) % 65521;
		b = (b + a) % 65521;
	}
	for (size_t off = 0; off < raw_len; off += 65535) {
		size_t n = BUNY_MIN((size_t)65535, raw_len - off);
		z[o++] = off + n >= raw_len ? 1 : 0;
		z[o++] = (uint8_t)n;
		z[o++] = (uint8_t)(n >> 8);
		z[o++] = (uint8_t)~n;
		z[o++] = (uint8_t)(~n >> 8);
		memcpy(z + o, raw + off, n);
		o += n;
	}
	put_be32(z + o, (b << 16) | a);
	o += 4;
	int rc = write_chunk(f, "IDAT", z, o);
	rc |= write_chunk(f, "IEND", NULL, 0);
	free(raw);
	free(z);
	if (fclose(f) != 0)
		rc = -1;
	return rc;
}
