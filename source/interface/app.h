/*
 * Distro Buny — interface application state
 *
 * The interface is a stack of screens (Home, folder browser, image details,
 * boot progress, settings, system information, search, log) with modal
 * dialogs on top.  Home also offers to grow the drive's data partition when
 * the drive was written from an image onto a bigger stick.  Every frame each screen lays out its interactive items;
 * the same item list drives rendering, pointer hit-testing and spatial
 * keyboard navigation, so what is drawn is exactly what can be activated.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BUNY_APP_H
#define BUNY_APP_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "boot-adapters/adapters.h"
#include "common/json.h"
#include "interface/display.h"
#include "interface/font.h"
#include "interface/icons.h"
#include "interface/image.h"
#include "interface/input.h"
#include "interface/theme.h"
#include "iso-manager/catalog.h"
#include "iso-manager/rules.h"

enum screen_kind { SCR_HOME, SCR_FOLDER, SCR_DETAIL, SCR_BOOT, SCR_SETTINGS, SCR_INFO, SCR_SEARCH, SCR_LOG, SCR_NODATA };

enum item_kind {
	IT_CATEGORY, IT_FOLDER, IT_ISO, IT_BACK, IT_CRUMB, IT_BUTTON, IT_SETTING, IT_RESULT, IT_DIALOG, IT_TEXTFIELD,
};

enum action {
	ACT_NONE, ACT_SETTINGS, ACT_INFO, ACT_POWER, ACT_SEARCH, ACT_BOOT, ACT_OPTIONS, ACT_RETRY, ACT_BACK,
	ACT_LOG, ACT_REBOOT, ACT_POWEROFF, ACT_FIRMWARE, ACT_CANCEL, ACT_VARIANT, ACT_TOGGLE_SAFE, ACT_STRATEGY,
	ACT_LAST_BOOT, ACT_RESCAN, ACT_DISMISS, ACT_CONFIRM_BOOT, ACT_EXPAND, ACT_CONFIRM_EXPAND,
};

struct item {
	enum item_kind kind;
	struct rect r;
	struct cat_node *node;
	enum action action;
	int index;
	bool disabled;
	bool scrolls;          /* belongs to the scrolling content area */
	const void *key;       /* stable identity for animations */
};

#define MAX_ITEMS 512
#define MAX_DEPTH 16

struct screen {
	enum screen_kind kind;
	struct cat_node *node;
	int focus;             /* index into items, -1 none */
	const void *focus_key; /* survives relayout */
	float scroll, scroll_target, content_h, view_h;
};

enum dialog_kind { DLG_NONE, DLG_POWER, DLG_OPTIONS, DLG_ERROR, DLG_INFO, DLG_CONFIRM, DLG_EXPAND, DLG_BUSY };

struct dialog {
	enum dialog_kind kind;
	char title[128];
	char message[1024];
	int focus;
	float appear;          /* animation 0..1 */
	uint64_t opened_ms;
	bool retry;            /* error dialog offers Retry */
	bool firmware;         /* error dialog offers "firmware settings" (Secure Boot) */
};

struct settings {
	int theme;             /* 0 light, 1 dark */
	bool animations;
	int scale_pct;         /* 0 = automatic */
	bool show_unsupported;
	bool remember_last;
	int strategy;          /* enum media_strategy */
	bool show_details;     /* show boot parameters on the detail screen */
};

enum boot_phase { BP_IDLE, BP_PREPARING, BP_LOADING, BP_READY, BP_HANDOFF, BP_FAILED, BP_DONE };

struct boot_job {
	pthread_t thread;
	bool running;
	atomic_int phase;              /* enum boot_phase */
	atomic_int stage;              /* enum handoff_stage */
	atomic_ullong done, total;
	char error[1024];
	int error_code;                /* errno-style code from the runtime, 0 if none */
	char rel_path[1024];
	char title[256];
	char cmdline[2048];
	char adapter[96];
	enum media_strategy strategy;
	int variant;
	bool safe_graphics;
	bool dry_run_only;             /* no privileged init available (development) */
	uint64_t ready_ms;
};

/* Growing the data partition: the privileged request runs on a thread while
 * a dialog that cannot be dismissed asks not to unplug the drive. */
enum expand_phase { EX_IDLE, EX_RUNNING, EX_FINISHED };

struct expand_job {
	pthread_t thread;
	bool running;
	atomic_int phase;              /* enum expand_phase */
	int status;                    /* errno-style code from the runtime, 0 on success */
	char text[1024];
};

struct anim_slot {
	const void *key;
	float value;                   /* hover/focus highlight 0..1 */
	uint64_t seen;
};

struct app {
	struct display *disp;
	struct canvas *frame;          /* what is on screen */
	struct canvas *scene;          /* off-screen render target for transitions */
	struct canvas *prev;           /* snapshot of the previous screen */
	struct canvas *bg;             /* cached background */
	struct input *in;
	struct fonts fonts;
	struct theme th;
	struct image *logo;
	struct distro_icons *icons;
	float s;                       /* layout scale (1.0 = 1080p) */
	int W, H;

	struct ruleset rules;
	struct catalog cat;
	bool have_catalog;
	struct json *runtime;          /* runtime.json */
	struct json *compat;           /* lab compatibility records */
	char data_dir[512];
	char share_dir[512];
	char esp_dir[512];
	char data_uuid[40], data_fs[16], data_label[64];
	int ipc_fd;
	bool lab;

	struct screen stack[MAX_DEPTH];
	int depth;
	struct item items[MAX_ITEMS];
	int nitems;
	struct dialog dlg;
	struct item ditems[16];
	int nditems;

	/* animation */
	bool transitioning;
	uint64_t trans_start;
	int trans_dir;
	float focus_rect[4], focus_from[4], focus_to[4];
	uint64_t focus_anim_start;
	bool focus_visible;
	struct anim_slot anims[64];
	uint64_t now;                  /* frame time (ms) */
	bool dirty;
	bool keyboard_mode;            /* last interaction came from the keyboard */
	float px, py;
	bool pointer_visible;
	uint64_t pointer_ms;
	const void *hover_key;
	bool pressing;

	/* search */
	char query[128];

	/* toasts */
	char toast[256];
	uint64_t toast_until;

	struct settings set;
	struct boot_job boot;

	/* background probing */
	pthread_t probe_thread;
	pthread_mutex_t lock;
	int wake_fd;                   /* eventfd: workers wake the main loop */
	struct probe_result *results;  /* worker -> main thread queue */
	bool probe_running;
	atomic_bool probe_stop;        /* stop probing (the data partition is about to change) */
	atomic_bool quit;
	size_t probed;

	struct expand_job expand;

	/* last boot attempt (from the ESP) */
	char last_boot_path[1024];
	char last_boot_title[256];

	/* lab/automation */
	char last_event[512];
	uint64_t start_ms;
	bool autoboot_done;
};

/* app.c */
int app_init(struct app *a);
void app_run(struct app *a);
void app_free(struct app *a);
void app_emit(struct app *a, const char *fmt, ...) BUNY_PRINTF(2, 3);
/* "possible" or "refused" when Home should mention the drive's unused space, else NULL. */
const char *expand_offer(struct app *a);
/* The image's family is unknown: it is started best effort, after a confirmation. */
bool is_best_effort(const struct cat_meta *m);

/* render.c */
void render_frame(struct app *a);
void layout_screen(struct app *a, struct screen *sc);
void layout_dialog(struct app *a);
struct text_style ts(struct app *a, struct font *f, float px);
float S(struct app *a, float v);
const char *iso_title(struct cat_node *n);
/* The empty-state title a screen shows instead of images, or NULL. */
const char *empty_hint(struct app *a, const struct screen *sc);
const char *compat_label(struct app *a, struct cat_node *n, color_t *fg, color_t *bg);
bool secure_boot_on(struct app *a);
bool sb_kernel_trusted(struct app *a, const struct cat_meta *m);

#endif
