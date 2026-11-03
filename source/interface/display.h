/*
 * Distro Buny — display output
 *
 * Backends, tried in order:
 *   drm        /dev/dri/card*: dumb buffer on the firmware framebuffer (simpledrm),
 *              with DIRTYFB damage flushing;
 *   fbdev      /dev/fb0 (legacy fbcon devices);
 *   offscreen  in-memory surface for host development and screenshots
 *              (BUNY_DISPLAY=offscreen:WIDTHxHEIGHT).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BUNY_DISPLAY_H
#define BUNY_DISPLAY_H

#include <stddef.h>

#include "interface/gfx.h"

struct display;

struct display *display_open(char *err, size_t errlen);
void display_close(struct display *d);
int display_width(const struct display *d);
int display_height(const struct display *d);
const char *display_backend(const struct display *d);
/* Copy damaged areas of `frame` (same size as the display) to the screen. */
void display_present(struct display *d, const struct canvas *frame, const struct rect *damage, int ndamage);
/* Write the last presented frame as a PNG-free portable pixmap (screenshots). */
int display_screenshot(const struct canvas *frame, const char *path);

#endif
