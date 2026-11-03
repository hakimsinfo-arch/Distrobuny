/*
 * Distro Buny — input (evdev keyboards, mice, touchpads, tablets)
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "interface/input.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "common/log.h"
#include "common/util.h"

#define MAX_DEVS 32
#define BITS_LONG (sizeof(long) * 8)
#define NLONGS(x) (((x) + BITS_LONG - 1) / BITS_LONG)
#define TEST_BIT(b, a) ((a[(b) / BITS_LONG] >> ((b) % BITS_LONG)) & 1)

enum dev_kind { DK_KEYBOARD = 1, DK_MOUSE = 2, DK_TOUCHPAD = 4, DK_ABSOLUTE = 8 };

struct dev {
	int fd;
	char node[64];
	char name[96];
	unsigned kind;
	struct input_absinfo ax, ay;
	bool mt;               /* multitouch protocol B */
	/* touchpad state */
	bool touching;
	int fingers;
	float tx, ty;          /* last touch position (device units) */
	bool have_last;
	uint64_t touch_start;
	float travel;
	int pending_abs_x, pending_abs_y;
	bool abs_dirty;
	float scroll_acc;
};

struct input {
	struct dev devs[MAX_DEVS];
	int ndevs;
	int ino;
	int sw, sh;
	float scale;
	float px, py;
	bool pointer_seen;
	unsigned mods;
};

static uint64_t ms_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static bool open_dev(struct input *in, const char *node)
{
	for (int i = 0; i < in->ndevs; i++)
		if (!strcmp(in->devs[i].node, node))
			return true;
	if (in->ndevs >= MAX_DEVS)
		return false;
	int fd = open(node, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return false;
	unsigned long evbits[NLONGS(EV_CNT)] = { 0 }, keybits[NLONGS(KEY_CNT)] = { 0 };
	unsigned long relbits[NLONGS(REL_CNT)] = { 0 }, absbits[NLONGS(ABS_CNT)] = { 0 };
	unsigned long props[NLONGS(INPUT_PROP_CNT)] = { 0 };
	ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits);
	ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits);
	ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relbits)), relbits);
	ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits);
	ioctl(fd, EVIOCGPROP(sizeof(props)), props);
	struct dev d;
	memset(&d, 0, sizeof(d));
	d.fd = fd;
	snprintf(d.node, sizeof(d.node), "%s", node);
	ioctl(fd, EVIOCGNAME(sizeof(d.name) - 1), d.name);
	if (TEST_BIT(EV_KEY, evbits) && TEST_BIT(KEY_A, keybits) && TEST_BIT(KEY_ENTER, keybits))
		d.kind |= DK_KEYBOARD;
	else if (TEST_BIT(EV_KEY, evbits) && (TEST_BIT(KEY_ENTER, keybits) || TEST_BIT(KEY_UP, keybits)))
		d.kind |= DK_KEYBOARD; /* remotes, keypads */
	if (TEST_BIT(EV_REL, evbits) && TEST_BIT(REL_X, relbits) && TEST_BIT(REL_Y, relbits))
		d.kind |= DK_MOUSE;
	if (TEST_BIT(EV_ABS, evbits) && TEST_BIT(ABS_X, absbits) && TEST_BIT(ABS_Y, absbits)) {
		ioctl(fd, EVIOCGABS(ABS_X), &d.ax);
		ioctl(fd, EVIOCGABS(ABS_Y), &d.ay);
		d.mt = TEST_BIT(ABS_MT_SLOT, absbits);
		bool finger = TEST_BIT(BTN_TOOL_FINGER, keybits);
		bool direct = TEST_BIT(INPUT_PROP_DIRECT, props);
		if (finger && !direct)
			d.kind |= DK_TOUCHPAD;
		else if (d.ax.maximum > d.ax.minimum && d.ay.maximum > d.ay.minimum)
			d.kind |= DK_ABSOLUTE; /* tablets, touchscreens, VM pointers */
	}
	if (!d.kind) {
		close(fd);
		return false;
	}
	in->devs[in->ndevs++] = d;
	LOGI("input: %s \"%s\"%s%s%s%s", node, d.name, d.kind & DK_KEYBOARD ? " keyboard" : "", d.kind & DK_MOUSE ? " mouse" : "",
	     d.kind & DK_TOUCHPAD ? " touchpad" : "", d.kind & DK_ABSOLUTE ? " absolute-pointer" : "");
	return true;
}

static void scan(struct input *in)
{
	DIR *dir = opendir("/dev/input");
	if (!dir)
		return;
	struct dirent *de;
	while ((de = readdir(dir)))
		if (str_starts(de->d_name, "event")) {
			char p[64];
			snprintf(p, sizeof(p), "/dev/input/%.40s", de->d_name);
			open_dev(in, p);
		}
	closedir(dir);
}

struct input *input_open(int screen_w, int screen_h)
{
	struct input *in = xcalloc(1, sizeof(*in));
	in->sw = screen_w;
	in->sh = screen_h;
	in->scale = 1.0f;
	in->px = (float)screen_w / 2;
	in->py = (float)screen_h / 2;
	in->ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (in->ino >= 0)
		inotify_add_watch(in->ino, "/dev/input", IN_CREATE | IN_ATTRIB);
	scan(in);
	return in;
}

void input_close(struct input *in)
{
	if (!in)
		return;
	for (int i = 0; i < in->ndevs; i++)
		close(in->devs[i].fd);
	if (in->ino >= 0)
		close(in->ino);
	free(in);
}

int input_fds(struct input *in, int *fds, int max)
{
	int n = 0;
	if (in->ino >= 0 && n < max)
		fds[n++] = in->ino;
	for (int i = 0; i < in->ndevs && n < max; i++)
		fds[n++] = in->devs[i].fd;
	return n;
}

void input_set_screen(struct input *in, int w, int h, float scale)
{
	in->sw = w;
	in->sh = h;
	in->scale = scale;
	in->px = BUNY_CLAMP(in->px, 0.0f, (float)(w - 1));
	in->py = BUNY_CLAMP(in->py, 0.0f, (float)(h - 1));
}

bool input_has_pointer(const struct input *in)
{
	return in->pointer_seen;
}

void input_pointer(const struct input *in, float *x, float *y)
{
	*x = in->px;
	*y = in->py;
}

int input_device_count(const struct input *in, int *keyboards, int *pointers)
{
	int k = 0, p = 0;
	for (int i = 0; i < in->ndevs; i++) {
		if (in->devs[i].kind & DK_KEYBOARD)
			k++;
		if (in->devs[i].kind & (DK_MOUSE | DK_TOUCHPAD | DK_ABSOLUTE))
			p++;
	}
	if (keyboards)
		*keyboards = k;
	if (pointers)
		*pointers = p;
	return in->ndevs;
}

/* US keyboard layout: (unshifted, shifted) characters for printable keys. */
static unsigned key_char(int code, bool shift)
{
	static const char *rows[] = { "1234567890-=", "qwertyuiop[]", "asdfghjkl;'`", "zxcvbnm,./" };
	static const char *srows[] = { "!@#$%^&*()_+", "QWERTYUIOP{}", "ASDFGHJKL:\"~", "ZXCVBNM<>?" };
	struct { int first; int len; int row; } map[] = {
		{ KEY_1, 12, 0 }, { KEY_Q, 12, 1 }, { KEY_A, 12, 2 }, { KEY_Z, 10, 3 },
	};
	if (code == KEY_SPACE)
		return ' ';
	if (code == KEY_BACKSLASH)
		return shift ? '|' : '\\';
	for (size_t i = 0; i < ARRAY_SIZE(map); i++)
		if (code >= map[i].first && code < map[i].first + map[i].len)
			return (unsigned char)(shift ? srows[map[i].row] : rows[map[i].row])[code - map[i].first];
	return 0;
}

static void push(struct in_event *out, int *n, int max, struct in_event ev)
{
	if (*n < max)
		out[(*n)++] = ev;
}

static void move_pointer(struct input *in, float dx, float dy, struct in_event *out, int *n, int max)
{
	in->px = BUNY_CLAMP(in->px + dx, 0.0f, (float)(in->sw - 1));
	in->py = BUNY_CLAMP(in->py + dy, 0.0f, (float)(in->sh - 1));
	in->pointer_seen = true;
	push(out, n, max, (struct in_event){ .type = IN_MOTION, .x = in->px, .y = in->py });
}

static float accel(float d, float scale)
{
	float a = fabsf(d);
	float f = 1.0f + BUNY_MIN(a, 12.0f) * 0.08f; /* gentle acceleration */
	return d * f * scale;
}

static void handle(struct input *in, struct dev *d, const struct input_event *e, struct in_event *out, int *n, int max)
{
	if (e->type == EV_KEY) {
		int code = e->code;
		if (code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT)
			in->mods = e->value ? in->mods | MOD_SHIFT : in->mods & ~MOD_SHIFT;
		else if (code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL)
			in->mods = e->value ? in->mods | MOD_CTRL : in->mods & ~MOD_CTRL;
		else if (code == KEY_LEFTALT || code == KEY_RIGHTALT)
			in->mods = e->value ? in->mods | MOD_ALT : in->mods & ~MOD_ALT;
		if (code == BTN_LEFT || code == BTN_RIGHT || code == BTN_MIDDLE) {
			if (d->kind & DK_TOUCHPAD && e->value == 0)
				d->travel = 1e9f; /* a physical click is not a tap */
			in->pointer_seen = true;
			push(out, n, max, (struct in_event){ .type = IN_BUTTON, .code = code, .pressed = e->value != 0,
							   .x = in->px, .y = in->py });
			return;
		}
		if (code == BTN_TOUCH || code == BTN_TOOL_FINGER || code == BTN_TOOL_DOUBLETAP || code == BTN_TOOL_TRIPLETAP) {
			if (code == BTN_TOOL_DOUBLETAP)
				d->fingers = e->value ? 2 : 1;
			if (code == BTN_TOUCH && d->kind & DK_TOUCHPAD) {
				if (e->value) {
					d->touching = true;
					d->have_last = false;
					d->touch_start = ms_now();
					d->travel = 0;
				} else {
					d->touching = false;
					uint64_t dur = ms_now() - d->touch_start;
					/* tap-to-click: short touch with little movement */
					if (dur < 200 && d->travel < 8.0f) {
						push(out, n, max, (struct in_event){ .type = IN_BUTTON, .code = BTN_LEFT, .pressed = true, .x = in->px, .y = in->py });
						push(out, n, max, (struct in_event){ .type = IN_BUTTON, .code = BTN_LEFT, .pressed = false, .x = in->px, .y = in->py });
					}
					d->fingers = 0;
				}
			} else if (code == BTN_TOUCH && d->kind & DK_ABSOLUTE) {
				/* touchscreen: touch acts as the left button */
				push(out, n, max, (struct in_event){ .type = IN_BUTTON, .code = BTN_LEFT, .pressed = e->value != 0, .x = in->px, .y = in->py });
			}
			return;
		}
		if (e->value == 0)
			return;
		bool shift = in->mods & MOD_SHIFT;
		push(out, n, max, (struct in_event){ .type = IN_KEY, .code = code, .pressed = true, .repeat = e->value == 2,
						   .mods = in->mods, .ch = (in->mods & (MOD_CTRL | MOD_ALT)) ? 0 : key_char(code, shift) });
	} else if (e->type == EV_REL) {
		if (e->code == REL_X)
			move_pointer(in, accel((float)e->value, in->scale), 0, out, n, max);
		else if (e->code == REL_Y)
			move_pointer(in, 0, accel((float)e->value, in->scale), out, n, max);
		else if (e->code == REL_WHEEL)
			push(out, n, max, (struct in_event){ .type = IN_SCROLL, .dy = (float)-e->value, .x = in->px, .y = in->py });
	} else if (e->type == EV_ABS) {
		if (e->code == ABS_X || e->code == ABS_MT_POSITION_X) {
			if (e->code == ABS_MT_POSITION_X && !d->mt)
				return;
			if (e->code == ABS_X || !(d->kind & DK_TOUCHPAD)) {
				d->pending_abs_x = e->value;
				d->abs_dirty = true;
			}
		} else if (e->code == ABS_Y || e->code == ABS_MT_POSITION_Y) {
			if (e->code == ABS_MT_POSITION_Y && !d->mt)
				return;
			if (e->code == ABS_Y || !(d->kind & DK_TOUCHPAD)) {
				d->pending_abs_y = e->value;
				d->abs_dirty = true;
			}
		}
	} else if (e->type == EV_SYN && e->code == SYN_REPORT && d->abs_dirty) {
		d->abs_dirty = false;
		float spanx = (float)(d->ax.maximum - d->ax.minimum), spany = (float)(d->ay.maximum - d->ay.minimum);
		if (spanx <= 0 || spany <= 0)
			return;
		if (d->kind & DK_ABSOLUTE) {
			float nx = (float)(d->pending_abs_x - d->ax.minimum) / spanx;
			float ny = (float)(d->pending_abs_y - d->ay.minimum) / spany;
			in->px = BUNY_CLAMP(nx * (float)(in->sw - 1), 0.0f, (float)(in->sw - 1));
			in->py = BUNY_CLAMP(ny * (float)(in->sh - 1), 0.0f, (float)(in->sh - 1));
			in->pointer_seen = true;
			push(out, n, max, (struct in_event){ .type = IN_MOTION, .x = in->px, .y = in->py });
		} else if (d->kind & DK_TOUCHPAD && d->touching) {
			float x = (float)d->pending_abs_x, y = (float)d->pending_abs_y;
			if (d->have_last) {
				/* normalise by resolution (units/mm) so speed is device independent */
				float rx = d->ax.resolution > 0 ? (float)d->ax.resolution : spanx / 100.0f;
				float ry = d->ay.resolution > 0 ? (float)d->ay.resolution : spany / 70.0f;
				float dxmm = (x - d->tx) / rx, dymm = (y - d->ty) / ry;
				d->travel += fabsf(dxmm) + fabsf(dymm);
				if (d->fingers >= 2) {
					d->scroll_acc += dymm / 4.0f;
					if (fabsf(d->scroll_acc) >= 1.0f) {
						push(out, n, max, (struct in_event){ .type = IN_SCROLL, .dy = -d->scroll_acc, .x = in->px, .y = in->py });
						d->scroll_acc = 0;
					}
				} else {
					move_pointer(in, accel(dxmm * 4.0f, in->scale), accel(dymm * 4.0f, in->scale), out, n, max);
				}
			}
			d->tx = x;
			d->ty = y;
			d->have_last = true;
		}
	}
}

int input_read(struct input *in, struct in_event *out, int max)
{
	int n = 0;
	if (in->ino >= 0) {
		char buf[4096];
		ssize_t r = read(in->ino, buf, sizeof(buf));
		if (r > 0)
			scan(in); /* new device node (or permissions granted) */
	}
	for (int i = 0; i < in->ndevs; i++) {
		struct dev *d = &in->devs[i];
		struct input_event evs[64];
		for (;;) {
			ssize_t r = read(d->fd, evs, sizeof(evs));
			if (r < 0) {
				if (errno == ENODEV) {
					LOGI("input: %s removed", d->node);
					close(d->fd);
					in->devs[i] = in->devs[--in->ndevs];
					i--;
				}
				break;
			}
			if (r == 0)
				break;
			for (size_t k = 0; k < (size_t)r / sizeof(struct input_event); k++)
				handle(in, d, &evs[k], out, &n, max);
			if ((size_t)r < sizeof(evs))
				break;
		}
	}
	return n;
}
