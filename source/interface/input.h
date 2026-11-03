/*
 * Distro Buny — input (evdev keyboards, mice, touchpads, tablets)
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BUNY_INPUT_H
#define BUNY_INPUT_H

#include <stdbool.h>
#include <stdint.h>

enum in_type { IN_KEY, IN_MOTION, IN_BUTTON, IN_SCROLL };

#define MOD_SHIFT 1u
#define MOD_CTRL 2u
#define MOD_ALT 4u

struct in_event {
	enum in_type type;
	int code;          /* IN_KEY: linux KEY_*; IN_BUTTON: BTN_LEFT/RIGHT/MIDDLE */
	bool pressed;
	bool repeat;
	unsigned mods;
	unsigned ch;       /* IN_KEY: Unicode character typed (US layout), 0 if none */
	float x, y;        /* IN_MOTION/IN_BUTTON: pointer position */
	float dy;          /* IN_SCROLL: lines (positive = down) */
};

struct input;

struct input *input_open(int screen_w, int screen_h);
void input_close(struct input *in);
/* File descriptors to poll (devices + hot-plug watch). */
int input_fds(struct input *in, int *fds, int max);
/* Read pending events; returns the number written to out. */
int input_read(struct input *in, struct in_event *out, int max);
void input_set_screen(struct input *in, int w, int h, float scale);
bool input_has_pointer(const struct input *in);
void input_pointer(const struct input *in, float *x, float *y);
int input_device_count(const struct input *in, int *keyboards, int *pointers);

#endif
