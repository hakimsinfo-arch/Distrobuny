/*
 * Distro Buny — interface layout and rendering
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/log.h"
#include "boot/handoff.h"
#include "common/strbuf.h"
#include "interface/app.h"
#include "interface/path.h"

#ifndef BUNY_VERSION
#define BUNY_VERSION "dev"
#endif

float S(struct app *a, float v)
{
	return v * a->s;
}

struct text_style ts(struct app *a, struct font *f, float px)
{
	return (struct text_style){ f, px * a->s };
}

static float ease_out(float t)
{
	t = BUNY_CLAMP(t, 0.0f, 1.0f);
	return 1.0f - powf(1.0f - t, 3.0f);
}

static float margin(struct app *a)
{
	float max_content = S(a, 1440);
	return fmaxf(S(a, 56), ((float)a->W - max_content) / 2.0f);
}

static const void *item_key(const struct item *it)
{
	if (it->key)
		return it->key;
	if (it->node)
		return it->node;
	return (const void *)(uintptr_t)(0x10000 + (unsigned)it->kind * 4096 + (unsigned)it->action * 64 + (unsigned)it->index);
}

static void add_item(struct app *a, struct item it)
{
	if (a->nitems >= MAX_ITEMS)
		return;
	if (!it.key)
		it.key = item_key(&it);
	a->items[a->nitems++] = it;
}

static float anim_value(struct app *a, const void *key)
{
	for (int i = 0; i < 64; i++)
		if (a->anims[i].key == key)
			return a->anims[i].value;
	return 0;
}

const char *iso_title(struct cat_node *n)
{
	return n->display ? n->display : n->name;
}

/* --------------------------------------------------------- compatibility */

static const struct json *compat_record(struct app *a, struct cat_node *n)
{
	if (!a->compat || !n->meta || !n->meta->label)
		return NULL;
	const struct json *recs = json_get(a->compat, "images");
	if (!recs || recs->type != JSON_ARRAY)
		return NULL;
	/* a lab result only counts for the same firmware situation: booting with
	 * Secure Boot on is a different test from booting with it off */
	bool sb = secure_boot_on(a);
	for (size_t i = 0; i < recs->n; i++) {
		const struct json *r = recs->items[i];
		const char *label = json_get_str(r, "label", NULL);
		double size = json_get_num(r, "size", -1);
		if (label && !strcmp(label, n->meta->label) && (uint64_t)size == n->size &&
		    json_get_bool(r, "secure_boot", false) == sb)
			return r;
	}
	return NULL;
}

const char *compat_label(struct app *a, struct cat_node *n, color_t *fg, color_t *bg)
{
	if (n->error) {
		*fg = a->th.danger;
		*bg = a->th.danger_soft;
		return "Unreadable";
	}
	if (!n->meta) {
		*fg = a->th.text2;
		*bg = a->th.surface_alt;
		return "Checking\xE2\x80\xA6";
	}
	if (n->meta->error) {
		*fg = a->th.danger;
		*bg = a->th.danger_soft;
		return "Damaged";
	}
	if (n->meta->level >= COMPAT_PREPARED && secure_boot_on(a) && !sb_kernel_trusted(a, n->meta)) {
		*fg = a->th.warn;
		*bg = a->th.warn_soft;
		return "Needs Secure Boot off";
	}
	if (n->meta->warning && n->meta->level >= COMPAT_PREPARED) {
		*fg = a->th.warn;
		*bg = a->th.warn_soft;
		return "Erases a disk";
	}
	const struct json *rec = compat_record(a, n);
	const char *res = rec ? json_get_str(rec, "result", "") : "";
	if (rec && !strcmp(res, "pass")) {
		*fg = a->th.ok;
		*bg = a->th.ok_soft;
		return "Verified";
	}
	if (rec && !strcmp(res, "fail")) {
		*fg = a->th.danger;
		*bg = a->th.danger_soft;
		return "Known issue";
	}
	if (n->meta->level >= COMPAT_PREPARED && is_best_effort(n->meta)) {
		*fg = a->th.warn;
		*bg = a->th.warn_soft;
		return "Best effort";
	}
	if (n->meta->level >= COMPAT_PREPARED) {
		*fg = a->th.accent;
		*bg = a->th.accent_soft;
		return "Ready";
	}
	*fg = a->th.warn;
	*bg = a->th.warn_soft;
	return "Not supported";
}

/* ---------------------------------------------------------------- widgets */

static void draw_text_center(struct app *a, struct text_style st, float cx, float baseline, const char *t, color_t col)
{
	float w = text_width(&st, t);
	text_draw(a->frame, &st, cx - w / 2, baseline, t, col);
}

static void draw_text_fit(struct app *a, struct text_style st, float x, float baseline, float maxw, const char *t, color_t col)
{
	char *e = text_ellipsize(&st, t, maxw);
	text_draw(a->frame, &st, x, baseline, e, col);
	free(e);
}

static void card(struct app *a, struct rect r, float radius, float lift, bool emphasise)
{
	struct theme *t = &a->th;
	float y = (float)r.y - lift * S(a, 3);
	gfx_shadow(a->frame, (float)r.x, y, (float)r.w, (float)r.h, radius, S(a, 18 + 10 * lift), S(a, 6 + 6 * lift),
		   color_alpha(t->shadow, 0.9f + 0.6f * lift));
	color_t fill = color_mix(t->surface, t->surface_hover, lift);
	gfx_fill_rrect(a->frame, (float)r.x, y, (float)r.w, (float)r.h, radius, fill);
	color_t border = color_mix(t->border, t->brand, emphasise ? 0.85f : lift * 0.7f);
	gfx_stroke_rrect(a->frame, (float)r.x, y, (float)r.w, (float)r.h, radius, S(a, 1.2f), border);
}

static void pill(struct app *a, float x, float y, const char *label, color_t fg, color_t bg, float size)
{
	struct text_style st = ts(a, a->fonts.semibold, size);
	struct font_metrics m;
	font_metrics(&st, &m);
	float w = text_width(&st, label) + S(a, 20), h = m.ascent + m.descent + S(a, 10);
	gfx_fill_rrect(a->frame, x, y, w, h, h / 2, bg);
	text_draw(a->frame, &st, x + S(a, 10), y + S(a, 5) + m.ascent, label, fg);
}

static float pill_width(struct app *a, const char *label, float size)
{
	struct text_style st = ts(a, a->fonts.semibold, size);
	return text_width(&st, label) + S(a, 20);
}

static void icon_tile(struct app *a, struct cat_node *n, float x, float y, float size)
{
	const char *icon = n && n->icon ? n->icon : "generic";
	int ui = icon_from_name(icon);
	if (ui >= 0 || !distro_icon_known(a->icons, icon)) {
		gfx_fill_rrect(a->frame, x, y, size, size, size * 0.28f, a->th.accent_soft);
		icon_draw(a->frame, ui >= 0 ? (enum ui_icon)ui : (n && n->kind == CAT_FOLDER ? ICON_FOLDER : ICON_DISC),
			  x + size * 0.22f, y + size * 0.22f, size * 0.56f, a->th.accent);
		return;
	}
	color_t brand = distro_icon_color(a->icons, icon, a->th.accent);
	/* tile tinted with the brand colour; marks drawn in their official colour */
	color_t tint = a->th.dark ? color_mix(a->th.surface, HEXC(0xFFFFFF), 0.92f) : color_mix(HEXC(0xFFFFFF), brand, 0.10f);
	gfx_fill_rrect(a->frame, x, y, size, size, size * 0.28f, tint);
	float m = size * 0.20f;
	distro_icon_draw(a->icons, a->frame, icon, x + m, y + m, size - 2 * m, a->fonts.bold, 1.0f);
}

static void draw_background(struct app *a)
{
	if (!a->bg || a->bg->w != a->W || a->bg->h != a->H) {
		canvas_free(a->bg);
		a->bg = canvas_new(a->W, a->H);
		gfx_fill_vgradient(a->bg, (struct rect){ 0, 0, a->W, a->H }, a->th.bg_top, a->th.bg_bottom);
	}
	gfx_copy(a->frame, 0, 0, a->bg, (struct rect){ 0, 0, a->W, a->H });
}

/* ------------------------------------------------------------- header */

static void layout_header(struct app *a, struct screen *sc, bool search_button)
{
	float M = margin(a);
	add_item(a, (struct item){ .kind = IT_BACK, .r = { (int)M, (int)S(a, 30), (int)S(a, 46), (int)S(a, 46) }, .action = ACT_BACK });
	/* breadcrumbs: Home › Desktop › ubuntu */
	struct text_style st = ts(a, a->fonts.medium, 15);
	float x = M + S(a, 64);
	struct cat_node *chain[MAX_DEPTH];
	int nchain = 0;
	for (struct cat_node *n = sc->node; n && nchain < MAX_DEPTH; n = n->parent)
		chain[nchain++] = n;
	for (int i = nchain - 1; i >= 0; i--) {
		const char *label = i == nchain - 1 ? "Home" : (chain[i]->kind == CAT_ISO ? iso_title(chain[i]) : chain[i]->display);
		float w = text_width(&st, label);
		if (i > 0)
			add_item(a, (struct item){ .kind = IT_CRUMB, .r = { (int)x - (int)S(a, 6), (int)S(a, 38), (int)(w + S(a, 12)), (int)S(a, 30) },
						   .node = chain[i], .index = nchain - 1 - i, .key = (const void *)((uintptr_t)chain[i] + 1) });
		x += w + S(a, 30);
	}
	if (search_button)
		add_item(a, (struct item){ .kind = IT_BUTTON, .action = ACT_SEARCH,
					   .r = { (int)((float)a->W - M - S(a, 46)), (int)S(a, 30), (int)S(a, 46), (int)S(a, 46) } });
}

static void draw_round_button(struct app *a, const struct item *it, enum ui_icon icon)
{
	float h = anim_value(a, it->key);
	float x = (float)it->r.x, y = (float)it->r.y, w = (float)it->r.w;
	gfx_shadow(a->frame, x, y, w, w, w / 2, S(a, 10), S(a, 3), color_alpha(a->th.shadow, 0.8f));
	gfx_fill_circle(a->frame, x + w / 2, y + w / 2, w / 2, color_mix(a->th.surface, a->th.accent_soft, h));
	gfx_stroke_circle(a->frame, x + w / 2, y + w / 2, w / 2, S(a, 1.2f), color_mix(a->th.border, a->th.brand, h));
	float is = w * 0.46f;
	icon_draw(a->frame, icon, x + (w - is) / 2, y + (w - is) / 2, is, color_mix(a->th.text, a->th.accent, h));
}

static void draw_header(struct app *a, struct screen *sc, const char *title, const char *subtitle)
{
	struct theme *t = &a->th;
	float M = margin(a);
	struct text_style crumb = ts(a, a->fonts.medium, 15);
	/* breadcrumb text (items carry the hit areas) */
	float x = M + S(a, 64);
	struct cat_node *chain[MAX_DEPTH];
	int nchain = 0;
	for (struct cat_node *n = sc->node; n && nchain < MAX_DEPTH; n = n->parent)
		chain[nchain++] = n;
	if (sc->kind == SCR_SETTINGS || sc->kind == SCR_INFO || sc->kind == SCR_SEARCH || sc->kind == SCR_LOG)
		nchain = 0;
	if (nchain == 0) {
		const char *one[] = { "Home", title };
		for (int i = 0; i < 2; i++) {
			color_t col = i == 1 ? t->text : t->text2;
			x += text_draw(a->frame, &crumb, x, S(a, 58), one[i], col);
			if (i == 0) {
				icon_draw(a->frame, ICON_CHEVRON_RIGHT, x + S(a, 8), S(a, 46), S(a, 14), t->text3);
				x += S(a, 30);
			}
		}
	}
	for (int i = nchain - 1; i >= 0; i--) {
		const char *label = i == nchain - 1 ? "Home" : (chain[i]->kind == CAT_ISO ? iso_title(chain[i]) : chain[i]->display);
		float h = 0;
		for (int k = 0; k < a->nitems; k++)
			if (a->items[k].kind == IT_CRUMB && a->items[k].node == chain[i])
				h = anim_value(a, a->items[k].key);
		color_t col = i == 0 ? t->text : color_mix(t->text2, t->accent, h);
		float w = text_draw(a->frame, &crumb, x, S(a, 58), label, col);
		if (h > 0.01f && i > 0)
			gfx_fill_rect(a->frame, (struct rect){ (int)x, (int)S(a, 62), (int)w, (int)fmaxf(1, S(a, 1.5f)) }, color_alpha(t->accent, h));
		x += w;
		if (i > 0) {
			icon_draw(a->frame, ICON_CHEVRON_RIGHT, x + S(a, 8), S(a, 46), S(a, 14), t->text3);
			x += S(a, 30);
		}
	}
	for (int i = 0; i < a->nitems; i++) {
		if (a->items[i].kind == IT_BACK)
			draw_round_button(a, &a->items[i], ICON_BACK);
		else if (a->items[i].kind == IT_BUTTON && a->items[i].action == ACT_SEARCH && !a->items[i].scrolls)
			draw_round_button(a, &a->items[i], ICON_SEARCH);
	}
	struct text_style tt = ts(a, a->fonts.bold, 36);
	draw_text_fit(a, tt, M, S(a, 132), (float)a->W - 2 * M, title, t->text);
	if (subtitle) {
		struct text_style st = ts(a, a->fonts.regular, 17);
		draw_text_fit(a, st, M, S(a, 162), (float)a->W - 2 * M, subtitle, t->text2);
	}
}

static float content_top(struct app *a)
{
	return S(a, 196);
}

/* --------------------------------------------------------- empty states */

/* A new drive (the starter image, a fresh install) holds no images: Home and
 * empty folders say how to add some instead of showing an empty grid. */
static const char *const add_steps[] = {
	"Connect this drive to any Windows, macOS or Linux computer.",
	"Copy .iso files into the Desktop or Servers folder of the DISTROBUNY drive.",
	"Start from the drive again: the new images appear in this menu automatically.",
};
#define NSTEPS (int)(sizeof(add_steps) / sizeof(add_steps[0]))

const char *empty_hint(struct app *a, const struct screen *sc)
{
	if (!a->have_catalog)
		return NULL;
	if (sc->kind == SCR_HOME && a->cat.total_isos == 0)
		return "No ISO images on this drive yet";
	if (sc->kind == SCR_FOLDER && sc->node && sc->node->iso_count == 0)
		return "No ISO images in this folder yet";
	return NULL;
}

static float add_steps_height(struct app *a)
{
	return S(a, 84) + S(a, 38) * (float)NSTEPS;
}

/* folder: the folder the user is looking at ("/Desktop/Ubuntu"), or NULL on Home */
static void draw_add_steps(struct app *a, float y, const char *title, const char *folder)
{
	struct theme *t = &a->th;
	float w = fminf(S(a, 760), (float)a->W - 2 * margin(a)), h = add_steps_height(a);
	float x = ((float)a->W - w) / 2, pad = S(a, 32);
	card(a, (struct rect){ (int)x, (int)y, (int)w, (int)h }, S(a, 22), 0, false);
	struct text_style tt = ts(a, a->fonts.semibold, 19), st = ts(a, a->fonts.regular, 16), nt = ts(a, a->fonts.semibold, 14);
	icon_draw(a->frame, ICON_INFO, x + pad, y + S(a, 25), S(a, 24), t->accent);
	draw_text_fit(a, tt, x + pad + S(a, 36), y + S(a, 44), w - 2 * pad - S(a, 36), title, t->text);
	char line[512];
	for (int i = 0; i < NSTEPS; i++) {
		if (i == 1 && folder)
			snprintf(line, sizeof(line), "Copy .iso files into the folder %s on the DISTROBUNY drive.", folder + 1);
		else
			snprintf(line, sizeof(line), "%s", add_steps[i]);
		float r = S(a, 13), cy = y + S(a, 82) + S(a, 38) * (float)i;
		gfx_fill_circle(a->frame, x + pad + r - S(a, 1), cy, r, t->accent_soft);
		char num[8];
		snprintf(num, sizeof(num), "%d", i + 1);
		draw_text_center(a, nt, x + pad + r - S(a, 1), cy + S(a, 5), num, t->accent);
		draw_text_fit(a, st, x + pad + S(a, 36), cy + S(a, 6), w - 2 * pad - S(a, 36), line, t->text2);
	}
}

/* ---------------------------------------------------------------- HOME */

static void home_categories(struct app *a, struct cat_node **out, int *n, bool *other)
{
	*n = 0;
	*other = false;
	if (!a->have_catalog)
		return;
	for (size_t i = 0; i < a->cat.root->nchildren && *n < 12; i++) {
		struct cat_node *c = a->cat.root->children[i];
		if (c->kind == CAT_FOLDER)
			out[(*n)++] = c;
		else
			*other = true;
	}
}

/* Home is laid out as one block (logo, title, cards) centred above the footer. */
static float home_logo_size(struct app *a)
{
	return fminf(fmaxf((float)a->H * 0.15f, S(a, 96)), S(a, 180));
}

static float home_top(struct app *a, int rows, float card_h, float gap)
{
	float block = home_logo_size(a) + S(a, 128) + (float)rows * card_h + (float)(rows - 1) * gap;
	if (a->last_boot_path[0] && a->set.remember_last)
		block += S(a, 76);
	if (expand_offer(a))
		block += S(a, 76);
	if (a->have_catalog && a->cat.total_isos == 0)
		block += S(a, 28) + add_steps_height(a);
	float avail = (float)a->H - S(a, 150);
	return fmaxf((float)a->H * 0.05f, (avail - block) / 2);
}

static void layout_home(struct app *a, struct screen *sc)
{
	float W = (float)a->W, H = (float)a->H;
	struct cat_node *cats[12];
	int n;
	bool other;
	home_categories(a, cats, &n, &other);
	int total = n + (other ? 1 : 0);
	float cw = S(a, 340), ch = S(a, 196), gap = S(a, 28);
	float avail = W - 2 * margin(a);
	int per_row = BUNY_MAX(1, (int)((avail + gap) / (cw + gap)));
	if (total < per_row)
		per_row = BUNY_MAX(total, 1);
	int rows = BUNY_MAX(1, (total + per_row - 1) / per_row);
	if (rows > 1)
		ch = S(a, 150);
	float y0 = home_top(a, rows, ch, gap) + home_logo_size(a) + S(a, 128);
	for (int i = 0; i < total; i++) {
		int row = i / per_row, col = i % per_row;
		int in_row = BUNY_MIN(per_row, total - row * per_row);
		float rw = (float)in_row * cw + (float)(in_row - 1) * gap;
		float x = (W - rw) / 2 + (float)col * (cw + gap);
		float y = y0 + (float)row * (ch + gap);
		struct item it = { .kind = IT_CATEGORY, .r = { (int)x, (int)y, (int)cw, (int)ch } };
		if (i < n)
			it.node = cats[i];
		else
			it.node = a->cat.root, it.key = (const void *)((uintptr_t)a->cat.root + 2);
		add_item(a, it);
	}
	float by = y0 + (float)rows * (ch + gap) + S(a, 6);
	if (a->last_boot_path[0] && a->set.remember_last) {
		float bw = S(a, 560), bh = S(a, 58);
		add_item(a, (struct item){ .kind = IT_BUTTON, .action = ACT_LAST_BOOT, .r = { (int)((W - bw) / 2), (int)by, (int)bw, (int)bh } });
		by += S(a, 76);
	}
	if (expand_offer(a)) {
		/* the drive was written from an image onto a bigger stick */
		float bw = fminf(S(a, 560), W - 2 * margin(a)), bh = S(a, 58);
		add_item(a, (struct item){ .kind = IT_BUTTON, .action = ACT_EXPAND, .r = { (int)((W - bw) / 2), (int)by, (int)bw, (int)bh } });
	}
	/* footer buttons */
	const struct { enum action act; const char *label; } btns[] = {
		{ ACT_SEARCH, "Search" }, { ACT_SETTINGS, "Settings" }, { ACT_INFO, "System info" }, { ACT_POWER, "Power" },
	};
	struct text_style st = ts(a, a->fonts.semibold, 16);
	float bw[4], tot = 0, bgap = S(a, 14);
	for (int i = 0; i < 4; i++) {
		bw[i] = text_width(&st, btns[i].label) + S(a, 74);
		tot += bw[i] + (i ? bgap : 0);
	}
	float x = (W - tot) / 2, fy = H - S(a, 118);
	for (int i = 0; i < 4; i++) {
		add_item(a, (struct item){ .kind = IT_BUTTON, .action = btns[i].act, .index = i, .r = { (int)x, (int)fy, (int)bw[i], (int)S(a, 50) } });
		x += bw[i] + bgap;
	}
	(void)sc;
}

static enum ui_icon action_icon(enum action act)
{
	switch (act) {
	case ACT_SEARCH: return ICON_SEARCH;
	case ACT_SETTINGS: return ICON_GEAR;
	case ACT_INFO: return ICON_INFO;
	case ACT_POWER: return ICON_POWER;
	case ACT_LOG: return ICON_LIST;
	case ACT_RETRY: case ACT_RESCAN: return ICON_RESTART;
	case ACT_BOOT: return ICON_PLAY;
	case ACT_OPTIONS: return ICON_LIST;
	case ACT_CANCEL: return ICON_CLOSE;
	default: return ICON_CHEVRON_RIGHT;
	}
}

static void draw_pill_button(struct app *a, const struct item *it, const char *label, bool primary)
{
	struct theme *t = &a->th;
	float h = anim_value(a, it->key);
	float x = (float)it->r.x, y = (float)it->r.y - h * S(a, 1.5f), w = (float)it->r.w, hh = (float)it->r.h;
	if (it->disabled) {
		gfx_fill_rrect(a->frame, x, y, w, hh, hh / 2, t->surface_alt);
		gfx_stroke_rrect(a->frame, x, y, w, hh, hh / 2, S(a, 1.2f), t->border);
	} else if (primary) {
		gfx_shadow(a->frame, x, y, w, hh, hh / 2, S(a, 14), S(a, 5), color_alpha(t->accent, 0.35f + 0.25f * h));
		gfx_fill_rrect(a->frame, x, y, w, hh, hh / 2, color_mix(t->accent, color_mix(t->accent, t->brand, 0.5f), h * 0.6f));
	} else {
		gfx_shadow(a->frame, x, y, w, hh, hh / 2, S(a, 10), S(a, 3), color_alpha(t->shadow, 0.7f + h));
		gfx_fill_rrect(a->frame, x, y, w, hh, hh / 2, color_mix(t->surface, t->accent_soft, h));
		gfx_stroke_rrect(a->frame, x, y, w, hh, hh / 2, S(a, 1.2f), color_mix(t->border, t->brand, h));
	}
	color_t fg = it->disabled ? t->text3 : primary ? t->accent_text : color_mix(t->text, t->accent, h);
	struct text_style st = ts(a, a->fonts.semibold, primary ? 18 : 16);
	struct font_metrics m;
	font_metrics(&st, &m);
	float is = S(a, primary ? 20 : 18);
	float tw = text_width(&st, label);
	float cx = x + (w - (is + S(a, 10) + tw)) / 2;
	icon_draw(a->frame, action_icon(it->action), cx, y + (hh - is) / 2, is, fg);
	text_draw(a->frame, &st, cx + is + S(a, 10), y + hh / 2 + (m.ascent - m.descent) / 2, label, fg);
}

/* Distinct distribution marks found below a folder, most frequent first. */
static void collect_marks(struct cat_node *n, const char **ids, int *counts, int *nids, int max)
{
	if (n->kind == CAT_ISO) {
		const char *id = n->meta && n->meta->icon ? n->meta->icon : n->icon;
		if (!id || !strcmp(id, "generic"))
			return;
		for (int i = 0; i < *nids; i++)
			if (!strcmp(ids[i], id)) {
				counts[i]++;
				return;
			}
		if (*nids < max) {
			ids[*nids] = id;
			counts[(*nids)++] = 1;
		}
		return;
	}
	for (size_t i = 0; i < n->nchildren; i++)
		collect_marks(n->children[i], ids, counts, nids, max);
}

static void draw_mark_stack(struct app *a, struct cat_node *n, float right, float y, float size, int max)
{
	const char *ids[64];
	int counts[64], nids = 0;
	collect_marks(n, ids, counts, &nids, 64);
	/* sort by frequency (stable for equal counts) */
	for (int i = 0; i < nids; i++)
		for (int k = i + 1; k < nids; k++)
			if (counts[k] > counts[i]) {
				int tc = counts[i];
				counts[i] = counts[k];
				counts[k] = tc;
				const char *ti = ids[i];
				ids[i] = ids[k];
				ids[k] = ti;
			}
	int shown = BUNY_MIN(nids, max);
	float step = size * 0.72f;
	float x = right - size - step * (float)(shown - 1);
	for (int i = 0; i < shown; i++) {
		float cx = x + (float)i * step + size / 2, cy = y + size / 2;
		gfx_fill_circle(a->frame, cx, cy, size / 2 + S(a, 2.5f), a->th.surface);
		color_t brand = distro_icon_color(a->icons, ids[i], a->th.accent);
		gfx_fill_circle(a->frame, cx, cy, size / 2, a->th.dark ? color_mix(a->th.surface, HEXC(0xFFFFFF), 0.92f)
									: color_mix(HEXC(0xFFFFFF), brand, 0.12f));
		float m = size * 0.22f;
		distro_icon_draw(a->icons, a->frame, ids[i], x + (float)i * step + m, y + m, size - 2 * m, a->fonts.bold, 1.0f);
	}
	if (nids > max) {
		char more[16];
		snprintf(more, sizeof(more), "+%d", nids - max);
		struct text_style st = ts(a, a->fonts.semibold, 13);
		float tw = text_width(&st, more);
		text_draw(a->frame, &st, x - tw - S(a, 10), y + size / 2 + S(a, 5), more, a->th.text3);
	}
}

static void draw_category_card(struct app *a, const struct item *it)
{
	struct theme *t = &a->th;
	float h = anim_value(a, it->key);
	bool other = it->node == a->cat.root;
	card(a, it->r, S(a, 26), h, false);
	float x = (float)it->r.x, y = (float)it->r.y - h * S(a, 3), w = (float)it->r.w, hh = (float)it->r.h;
	float pad = S(a, 28), ic = S(a, 64);
	if (other) {
		gfx_fill_rrect(a->frame, x + pad, y + pad, ic, ic, ic * 0.3f, t->accent_soft);
		icon_draw(a->frame, ICON_DISC, x + pad + ic * 0.2f, y + pad + ic * 0.2f, ic * 0.6f, t->accent);
	} else {
		icon_tile(a, it->node, x + pad, y + pad, ic);
	}
	if (!other)
		draw_mark_stack(a, it->node, x + w - pad, y + pad + (ic - S(a, 34)) / 2, S(a, 34), 5);
	const char *title = other ? "Other images" : it->node->display;
	size_t count = 0;
	uint64_t bytes = 0;
	if (other) {
		for (size_t i = 0; i < a->cat.root->nchildren; i++)
			if (a->cat.root->children[i]->kind == CAT_ISO) {
				count++;
				bytes += a->cat.root->children[i]->size;
			}
	} else {
		count = it->node->iso_count;
		bytes = it->node->subtree_bytes;
	}
	struct text_style tt = ts(a, a->fonts.bold, hh > S(a, 170) ? 28 : 24);
	struct text_style sub = ts(a, a->fonts.medium, 15);
	float ty = y + hh - pad - S(a, 30);
	draw_text_fit(a, tt, x + pad, ty, w - 2 * pad - S(a, 30), title, t->text);
	char line[96], size[32];
	format_size(bytes, size, sizeof(size));
	snprintf(line, sizeof(line), "%zu image%s \xC2\xB7 %s", count, count == 1 ? "" : "s", size);
	text_draw(a->frame, &sub, x + pad, ty + S(a, 28), line, t->text2);
	icon_draw(a->frame, ICON_CHEVRON_RIGHT, x + w - pad - S(a, 22), ty - S(a, 18), S(a, 22), color_mix(t->text3, t->accent, h));
}

/* "12.8 GB of this drive are not used yet   Use the whole drive >" */
static void draw_expand_banner(struct app *a, const struct item *it)
{
	struct theme *t = &a->th;
	float h = anim_value(a, it->key);
	card(a, it->r, S(a, 18), h, false);
	const struct json *xp = json_get(a->runtime, "expand");
	bool possible = !strcmp(expand_offer(a), "possible");
	uint64_t now = (uint64_t)json_get_num(xp, "data_bytes", 0), grown = (uint64_t)json_get_num(xp, "new_data_bytes", 0);
	char gain[32], line[200];
	format_size(grown > now ? grown - now : 0, gain, sizeof(gain));
	if (possible)
		snprintf(line, sizeof(line), "%s of this drive are not used yet", gain);
	else
		snprintf(line, sizeof(line), "%s", "Part of this drive is not used");
	struct text_style st = ts(a, a->fonts.medium, 15), bt = ts(a, a->fonts.semibold, 16);
	float x = (float)it->r.x + S(a, 22), y = (float)it->r.y - h * S(a, 3);
	const char *cta = possible ? "Use the whole drive" : "Why?";
	float cw = text_width(&bt, cta);
	icon_draw(a->frame, ICON_DRIVE, x, y + S(a, 18), S(a, 22), possible ? t->accent : t->text3);
	draw_text_fit(a, st, x + S(a, 36), y + S(a, 35), (float)it->r.w - cw - S(a, 120), line, t->text2);
	text_draw(a->frame, &bt, (float)(it->r.x + it->r.w) - cw - S(a, 46), y + S(a, 35), cta, color_mix(t->accent, t->text, h * 0.3f));
	icon_draw(a->frame, ICON_CHEVRON_RIGHT, (float)(it->r.x + it->r.w) - S(a, 38), y + S(a, 19), S(a, 20), t->accent);
}

static void draw_home(struct app *a, struct screen *sc)
{
	struct theme *t = &a->th;
	float W = (float)a->W, H = (float)a->H;
	float logo = home_logo_size(a);
	float ly = 0;
	for (int i = 0; i < a->nitems; i++)
		if (a->items[i].kind == IT_CATEGORY) {
			ly = (float)a->items[i].r.y - S(a, 128) - logo;
			break;
		}
	if (ly <= 0)
		ly = H * 0.12f;
	if (a->logo)
		image_draw(a->frame, a->logo, (int)((W - logo) / 2), (int)ly, (int)logo, (int)logo, 1.0f);
	struct text_style title = ts(a, a->fonts.bold, 50);
	draw_text_center(a, title, W / 2, ly + logo + S(a, 66), "Distro Buny", t->text);
	struct text_style tag = ts(a, a->fonts.regular, 20);
	draw_text_center(a, tag, W / 2, ly + logo + S(a, 102), "Your Linux universe", t->text2);

	for (int i = 0; i < a->nitems; i++) {
		struct item *it = &a->items[i];
		if (it->kind == IT_CATEGORY) {
			draw_category_card(a, it);
		} else if (it->kind == IT_BUTTON && it->action == ACT_LAST_BOOT) {
			float h = anim_value(a, it->key);
			card(a, it->r, S(a, 18), h, false);
			struct text_style st = ts(a, a->fonts.medium, 15);
			struct text_style bt = ts(a, a->fonts.semibold, 16);
			float x = (float)it->r.x + S(a, 22), y = (float)it->r.y - h * S(a, 3);
			icon_draw(a->frame, ICON_RESTART, x, y + S(a, 18), S(a, 22), t->accent);
			char line[400];
			snprintf(line, sizeof(line), "Last started: %s", a->last_boot_title);
			draw_text_fit(a, st, x + S(a, 36), y + S(a, 35), (float)it->r.w - S(a, 230), line, t->text2);
			const char *cta = "Start again";
			float cw = text_width(&bt, cta);
			text_draw(a->frame, &bt, (float)(it->r.x + it->r.w) - cw - S(a, 46), y + S(a, 35), cta, color_mix(t->accent, t->text, h * 0.3f));
			icon_draw(a->frame, ICON_CHEVRON_RIGHT, (float)(it->r.x + it->r.w) - S(a, 38), y + S(a, 19), S(a, 20), t->accent);
		} else if (it->kind == IT_BUTTON && it->action == ACT_EXPAND) {
			draw_expand_banner(a, it);
		} else if (it->kind == IT_BUTTON) {
			static const char *labels[] = { "Search", "Settings", "System info", "Power" };
			draw_pill_button(a, it, labels[it->index], false);
		}
	}
	if (!a->have_catalog) {
		struct text_style st = ts(a, a->fonts.medium, 18);
		draw_text_center(a, st, W / 2, H * 0.62f, "No Distro Buny data partition was found.", t->danger);
	} else if (a->cat.total_isos == 0) {
		/* below the category cards (and the "Last started" button) */
		float bottom = ly + logo + S(a, 100);
		for (int i = 0; i < a->nitems; i++)
			if (a->items[i].kind == IT_CATEGORY ||
			    (a->items[i].kind == IT_BUTTON && (a->items[i].action == ACT_LAST_BOOT || a->items[i].action == ACT_EXPAND)))
				bottom = fmaxf(bottom, (float)(a->items[i].r.y + a->items[i].r.h));
		draw_add_steps(a, bottom + S(a, 28), empty_hint(a, sc), NULL);
	}
	struct text_style foot = ts(a, a->fonts.medium, 13);
	text_draw(a->frame, &foot, margin(a), H - S(a, 30), "Distro Buny " BUNY_VERSION, t->text3);
	const char *hint = "\xE2\x86\x90 \xE2\x86\x91 \xE2\x86\x92 \xE2\x86\x93  move    Enter  open    Esc  back    /  search";
	float hw = text_width(&foot, hint);
	text_draw(a->frame, &foot, W - margin(a) - hw, H - S(a, 30), hint, t->text3);
	(void)sc;
}

/* ------------------------------------------------------------- FOLDER */

static bool visible_child(struct app *a, struct cat_node *c)
{
	if (c->kind == CAT_FOLDER)
		return c->iso_count > 0 || a->set.show_unsupported;
	if (a->set.show_unsupported || !c->meta)
		return true;
	return c->meta->level >= COMPAT_PREPARED && !c->meta->error;
}

static void layout_grid(struct app *a, struct screen *sc, struct cat_node **nodes, int n, float top)
{
	float M = margin(a), W = (float)a->W;
	float gap = S(a, 22), minw = S(a, 330), th = S(a, 132);
	float avail = W - 2 * M;
	int cols = BUNY_MAX(1, (int)((avail + gap) / (minw + gap)));
	float tw = (avail - (float)(cols - 1) * gap) / (float)cols;
	for (int i = 0; i < n; i++) {
		int r = i / cols, c = i % cols;
		float x = M + (float)c * (tw + gap);
		float y = top + (float)r * (th + gap) - sc->scroll;
		add_item(a, (struct item){ .kind = nodes[i]->kind == CAT_FOLDER ? IT_FOLDER : IT_ISO, .node = nodes[i],
					   .r = { (int)x, (int)y, (int)tw, (int)th }, .scrolls = true });
	}
	int rows = (n + cols - 1) / cols;
	sc->content_h = (float)rows * (th + gap);
	sc->view_h = (float)a->H - top - S(a, 40);
}

static void layout_folder(struct app *a, struct screen *sc)
{
	layout_header(a, sc, true);
	struct cat_node *nodes[MAX_ITEMS];
	int n = 0;
	for (size_t i = 0; i < sc->node->nchildren && n < MAX_ITEMS - 32; i++)
		if (visible_child(a, sc->node->children[i]))
			nodes[n++] = sc->node->children[i];
	layout_grid(a, sc, nodes, n, content_top(a));
}

static void draw_tile(struct app *a, const struct item *it)
{
	struct theme *t = &a->th;
	struct cat_node *nd = it->node;
	float h = anim_value(a, it->key);
	card(a, it->r, S(a, 22), h, false);
	float x = (float)it->r.x, y = (float)it->r.y - h * S(a, 3), w = (float)it->r.w, hh = (float)it->r.h;
	float pad = S(a, 20), ic = S(a, 60);
	icon_tile(a, nd, x + pad, y + (hh - ic) / 2, ic);
	float tx = x + pad + ic + S(a, 18), tw = w - (tx - x) - pad;
	struct text_style t1 = ts(a, a->fonts.semibold, 19);
	struct text_style t2 = ts(a, a->fonts.regular, 15);
	struct text_style t3 = ts(a, a->fonts.medium, 13);
	char l2[256] = "", l3[128] = "", size[32];
	format_size(nd->kind == CAT_FOLDER ? nd->subtree_bytes : nd->size, size, sizeof(size));
	if (nd->kind == CAT_FOLDER) {
		snprintf(l2, sizeof(l2), "%zu image%s", nd->iso_count, nd->iso_count == 1 ? "" : "s");
		snprintf(l3, sizeof(l3), "%s", size);
		draw_text_fit(a, t1, tx, y + S(a, 52), tw - S(a, 26), nd->display, t->text);
		icon_draw(a->frame, ICON_CHEVRON_RIGHT, x + w - pad - S(a, 20), y + hh / 2 - S(a, 10), S(a, 20), color_mix(t->text3, t->accent, h));
	} else {
		const char *name = nd->meta && nd->meta->recognized && nd->meta->name ? nd->meta->name : iso_title(nd);
		struct cat_meta *m = nd->meta;
		if (m && m->recognized) {
			if (m->version && m->variant)
				snprintf(l2, sizeof(l2), "%s \xC2\xB7 %s", m->version, m->variant);
			else
				snprintf(l2, sizeof(l2), "%s", m->version ? m->version : m->variant ? m->variant : "");
		} else {
			snprintf(l2, sizeof(l2), "%s", nd->name);
		}
		snprintf(l3, sizeof(l3), "%s%s%s", size, m && m->arch ? " \xC2\xB7 " : "", m && m->arch ? m->arch : "");
		draw_text_fit(a, t1, tx, y + S(a, 42), tw, name, t->text);
	}
	draw_text_fit(a, t2, tx, y + (nd->kind == CAT_FOLDER ? S(a, 78) : S(a, 68)), tw - S(a, 24), l2, t->text2);
	if (nd->kind == CAT_ISO) {
		color_t fg, bg;
		const char *label = compat_label(a, nd, &fg, &bg);
		float pw = pill_width(a, label, 12);
		pill(a, tx, y + S(a, 84), label, fg, bg, 12);
		text_draw(a->frame, &t3, tx + pw + S(a, 10), y + S(a, 101), l3, t->text3);
	} else {
		text_draw(a->frame, &t3, tx, y + S(a, 100), l3, t->text3);
	}
}

static void draw_scroll_indicator(struct app *a, struct screen *sc, float top)
{
	if (sc->content_h <= sc->view_h + 1)
		return;
	float track = sc->view_h;
	float frac = sc->view_h / sc->content_h;
	float pos = sc->scroll / (sc->content_h - sc->view_h);
	float bh = fmaxf(track * frac, S(a, 40));
	float by = top + (track - bh) * BUNY_CLAMP(pos, 0.0f, 1.0f);
	gfx_fill_rrect(a->frame, (float)a->W - S(a, 18), by, S(a, 5), bh, S(a, 2.5f), color_alpha(a->th.text3, 0.5f));
}

static void draw_folder(struct app *a, struct screen *sc)
{
	char sub[160], size[32];
	format_size(sc->node->subtree_bytes, size, sizeof(size));
	snprintf(sub, sizeof(sub), "%zu image%s \xC2\xB7 %s%s%s", sc->node->iso_count, sc->node->iso_count == 1 ? "" : "s", size,
		 sc->node->description ? " \xC2\xB7 " : "", sc->node->description ? sc->node->description : "");
	draw_header(a, sc, sc->node == a->cat.root ? "Other images" : sc->node->display, sub);
	float top = content_top(a) - S(a, 14);
	struct rect old = canvas_push_clip(a->frame, (struct rect){ 0, (int)top, a->W, a->H - (int)top });
	for (int i = 0; i < a->nitems; i++)
		if (a->items[i].kind == IT_FOLDER || a->items[i].kind == IT_ISO)
			draw_tile(a, &a->items[i]);
	a->frame->clip = old;
	int shown = 0;
	for (int i = 0; i < a->nitems; i++)
		if (a->items[i].kind == IT_FOLDER || a->items[i].kind == IT_ISO)
			shown++;
	if (sc->node->iso_count == 0) {
		/* below the tiles of any (empty) sub-folders */
		draw_add_steps(a, content_top(a) + sc->content_h - sc->scroll + S(a, 10), empty_hint(a, sc), sc->node->rel_path);
	} else if (!shown) {
		struct text_style st = ts(a, a->fonts.medium, 17);
		draw_text_center(a, st, (float)a->W / 2, content_top(a) + S(a, 80),
				 "No bootable images here (unsupported images are hidden in Settings).", a->th.text2);
	}
	draw_scroll_indicator(a, sc, content_top(a));
}

/* ------------------------------------------------------------- DETAIL */

static void layout_detail(struct app *a, struct screen *sc)
{
	layout_header(a, sc, false);
	float M = margin(a);
	float top = content_top(a) + S(a, 6);
	bool wide = (float)a->W > S(a, 1100);
	float leftw = wide ? S(a, 420) : (float)a->W - 2 * M;
	float by = top + S(a, 250);
	bool ok = sc->node->meta && sc->node->meta->level >= COMPAT_PREPARED && !sc->node->meta->error && !sc->node->error;
	add_item(a, (struct item){ .kind = IT_BUTTON, .action = ACT_BOOT, .disabled = !ok,
				   .r = { (int)M, (int)by, (int)leftw, (int)S(a, 62) } });
	add_item(a, (struct item){ .kind = IT_BUTTON, .action = ACT_OPTIONS, .disabled = !ok,
				   .r = { (int)M, (int)(by + S(a, 78)), (int)leftw, (int)S(a, 52) } });
}

static void info_row(struct app *a, float x, float *y, float w, const char *label, const char *value, color_t vcol)
{
	if (!value || !*value)
		return;
	struct text_style lt = ts(a, a->fonts.medium, 14);
	struct text_style vt = ts(a, a->fonts.regular, 16);
	text_draw(a->frame, &lt, x, *y, label, a->th.text3);
	float lw = S(a, 170);
	int lines = text_draw_wrapped(a->frame, &vt, x + lw, *y, w - lw, value, vcol, 4, 1.15f);
	struct font_metrics m;
	font_metrics(&vt, &m);
	*y += S(a, 16) + (float)BUNY_MAX(lines, 1) * m.line_height * 1.15f;
}

static void draw_detail(struct app *a, struct screen *sc)
{
	struct theme *t = &a->th;
	struct cat_node *n = sc->node;
	struct cat_meta *m = n->meta;
	const char *name = m && m->recognized && m->name ? m->name : iso_title(n);
	char sub[256] = "";
	if (m && m->recognized)
		snprintf(sub, sizeof(sub), "%s%s%s", m->version ? m->version : "", m->version && m->variant ? " \xC2\xB7 " : "",
			 m->variant ? m->variant : "");
	draw_header(a, sc, name, sub[0] ? sub : n->name);

	float M = margin(a);
	float top = content_top(a) + S(a, 6);
	bool wide = (float)a->W > S(a, 1100);
	float leftw = wide ? S(a, 420) : (float)a->W - 2 * M;
	/* identity card */
	struct rect idc = { (int)M, (int)top, (int)leftw, (int)S(a, 226) };
	card(a, idc, S(a, 26), 0, false);
	float ic = S(a, 96);
	icon_tile(a, n, M + S(a, 26), top + S(a, 26), ic);
	color_t fg, bg;
	const char *status = compat_label(a, n, &fg, &bg);
	pill(a, M + S(a, 26) + ic + S(a, 18), top + S(a, 30), status, fg, bg, 14);
	struct text_style big = ts(a, a->fonts.semibold, 20);
	struct text_style small = ts(a, a->fonts.regular, 15);
	char size[32];
	format_size(n->size, size, sizeof(size));
	draw_text_fit(a, big, M + S(a, 26) + ic + S(a, 18), top + S(a, 92), leftw - ic - S(a, 70), size, t->text);
	draw_text_fit(a, small, M + S(a, 26) + ic + S(a, 18), top + S(a, 118), leftw - ic - S(a, 70),
		      m && m->arch ? m->arch : "architecture unknown", t->text2);
	const char *method = m && m->adapter_name ? m->adapter_name : "No boot method";
	text_draw_wrapped(a->frame, &small, M + S(a, 26), top + S(a, 162), leftw - S(a, 52), method, t->text2, 2, 1.1f);

	for (int i = 0; i < a->nitems; i++) {
		struct item *it = &a->items[i];
		if (it->kind != IT_BUTTON)
			continue;
		if (it->action == ACT_BOOT) {
			char label[160];
			snprintf(label, sizeof(label), "Start %s", name);
			struct text_style st = ts(a, a->fonts.semibold, 18);
			char *e = text_ellipsize(&st, label, (float)it->r.w - S(a, 90));
			draw_pill_button(a, it, e, true);
			free(e);
		} else if (it->action == ACT_OPTIONS) {
			draw_pill_button(a, it, "Boot options", false);
		}
	}

	/* details card */
	float rx = wide ? M + leftw + S(a, 28) : M;
	float ry = wide ? top : top + S(a, 400);
	float rw = wide ? (float)a->W - M - rx : leftw;
	struct rect dc = { (int)rx, (int)ry, (int)rw, (int)((float)a->H - ry - S(a, 48)) };
	card(a, dc, S(a, 26), 0, false);
	struct rect old = canvas_push_clip(a->frame, (struct rect){ dc.x, dc.y, dc.w, dc.h - (int)S(a, 12) });
	float x = rx + S(a, 30), y = ry + S(a, 46), w = rw - S(a, 60);
	struct text_style h2 = ts(a, a->fonts.semibold, 19);
	text_draw(a->frame, &h2, x, y, "Image details", t->text);
	y += S(a, 38);
	char tmp[1200];
	info_row(a, x, &y, w, "File", n->name, t->text);
	char *dir = path_dirname(n->rel_path);
	info_row(a, x, &y, w, "Location", dir, t->text);
	free(dir);
	if (m) {
		info_row(a, x, &y, w, "Distribution", m->recognized ? m->name : "Not recognised", t->text);
		info_row(a, x, &y, w, "Version", m->version, t->text);
		info_row(a, x, &y, w, "Edition", m->variant, t->text);
		info_row(a, x, &y, w, "Kernel", m->kernel_version, t->text);
		info_row(a, x, &y, w, "Volume label", m->label, t->text);
		info_row(a, x, &y, w, "Built", m->created, t->text);
		info_row(a, x, &y, w, "Boot method", m->adapter_name, t->text);
		if (m->adapter && !strcmp(m->adapter, ADAPTER_SLACKWARE) && m->level >= COMPAT_PREPARED)
			info_row(a, x, &y, w, "Note", "When setup asks for the source, choose \"Install from a Slackware CD or DVD\", then "
				 "\"manual\" and \"custom\", and type /dev/loop200 (Distro Buny's virtual DVD).", t->warn);
		if (is_best_effort(m) && m->level >= COMPAT_PREPARED)
			info_row(a, x, &y, w, "Note", "Distro Buny does not know this distribution family yet and tries the image's own "
				 "boot entry. If the system stops with an error, restart the computer.", t->warn);
		bool refused = secure_boot_on(a) && !sb_kernel_trusted(a, m);
		snprintf(tmp, sizeof(tmp), "%s%s", m->kernel_signed ? (m->kernel_signer ? m->kernel_signer : "Signed") : "Not signed",
			 refused && m->kernel_signed ? " \xE2\x80\x94 not trusted by this computer's Secure Boot keys" : "");
		info_row(a, x, &y, w, "Kernel signature", tmp, refused ? t->warn : t->text);
		if (m->level >= COMPAT_PREPARED) {
			snprintf(tmp, sizeof(tmp), "%s", m->strategy == MEDIA_NATIVE ? "Native ISO-file boot" :
				 "Distro Buny virtual medium (the ISO is presented to the system as its own disc)");
			info_row(a, x, &y, w, "Media", tmp, t->text);
		}
		if (m->warning)
			info_row(a, x, &y, w, "Warning", m->warning, t->danger);
		const struct json *rec = compat_record(a, n);
		if (rec) {
			snprintf(tmp, sizeof(tmp), "%s on %s: %s", !strcmp(json_get_str(rec, "result", ""), "pass") ? "Booted successfully" : "Failed",
				 json_get_str(rec, "date", "?"), json_get_str(rec, "evidence", ""));
			info_row(a, x, &y, w, "Lab test", tmp, t->text);
		} else {
			info_row(a, x, &y, w, "Lab test", "This exact image has not been tested in the Distro Buny lab.", t->text2);
		}
		if (m->error)
			info_row(a, x, &y, w, "Problem", m->error, t->danger);
		else if (m->level < COMPAT_PREPARED)
			info_row(a, x, &y, w, "Why not", m->reason, t->warn);
		if (a->set.show_details && m->virtual_blocker && m->level >= COMPAT_PREPARED)
			info_row(a, x, &y, w, "Virtual medium", m->virtual_blocker, t->text2);
		if (a->set.show_details && m->native_blocker)
			info_row(a, x, &y, w, "Native boot", m->native_blocker, t->text2);
	} else if (n->error) {
		info_row(a, x, &y, w, "Problem", n->error, t->danger);
	}
	a->frame->clip = old;
}

/* --------------------------------------------------------------- BOOT */

static void layout_boot(struct app *a, struct screen *sc)
{
	int phase = atomic_load(&a->boot.phase);
	float W = (float)a->W, H = (float)a->H;
	if (phase == BP_PREPARING || phase == BP_LOADING) {
		float bw = S(a, 220);
		add_item(a, (struct item){ .kind = IT_BUTTON, .action = ACT_CANCEL, .r = { (int)((W - bw) / 2), (int)(H / 2 + S(a, 150)), (int)bw, (int)S(a, 50) } });
	}
	(void)sc;
}

static const char *stage_text(int stage)
{
	switch (stage) {
	case HS_KERNEL: return "Reading the kernel";
	case HS_INITRD: return "Reading the initial RAM disk";
	case HS_MEDIUM: return "Preparing the virtual medium";
	case HS_LOAD: return "Loading the kernel";
	default: return "Starting";
	}
}

static void draw_boot(struct app *a, struct screen *sc)
{
	struct theme *t = &a->th;
	float W = (float)a->W, H = (float)a->H;
	int phase = atomic_load(&a->boot.phase);
	float cw = S(a, 640), ch = S(a, 300);
	struct rect r = { (int)((W - cw) / 2), (int)((H - ch) / 2 - S(a, 30)), (int)cw, (int)ch };
	card(a, r, S(a, 28), 0, false);
	float ic = S(a, 72);
	icon_tile(a, sc->node, (W - ic) / 2, (float)r.y + S(a, 30), ic);
	struct text_style tt = ts(a, a->fonts.semibold, 24);
	char title[300];
	snprintf(title, sizeof(title), "%s %s", phase >= BP_READY ? "Starting" : "Preparing", a->boot.title);
	char *e = text_ellipsize(&tt, title, cw - S(a, 60));
	draw_text_center(a, tt, W / 2, (float)r.y + S(a, 146), e, t->text);
	free(e);
	struct text_style st = ts(a, a->fonts.regular, 16);
	const char *step = phase == BP_PREPARING ? "Analysing the image" : phase == BP_READY || phase == BP_HANDOFF ? "Handing over to the system\xE2\x80\xA6" :
			   stage_text(atomic_load(&a->boot.stage));
	draw_text_center(a, st, W / 2, (float)r.y + S(a, 182), step, t->text2);
	/* progress bar */
	float bx = (float)r.x + S(a, 60), bw = cw - S(a, 120), by = (float)r.y + S(a, 212), bh = S(a, 10);
	gfx_fill_rrect(a->frame, bx, by, bw, bh, bh / 2, t->surface_alt);
	uint64_t done = atomic_load(&a->boot.done), total = atomic_load(&a->boot.total);
	float frac = phase >= BP_READY ? 1.0f : total ? (float)done / (float)total : 0;
	if (phase == BP_PREPARING) {
		/* indeterminate: a sliding segment */
		float p = fmodf((float)a->now / 1100.0f, 1.0f);
		float seg = bw * 0.28f;
		float sx = bx - seg + (bw + seg) * p;
		struct rect old = canvas_push_clip(a->frame, (struct rect){ (int)bx, (int)by, (int)bw, (int)bh + 1 });
		gfx_fill_rrect(a->frame, sx, by, seg, bh, bh / 2, t->brand);
		a->frame->clip = old;
	} else {
		gfx_fill_rrect(a->frame, bx, by, fmaxf(bh, bw * BUNY_CLAMP(frac, 0.0f, 1.0f)), bh, bh / 2, t->accent);
	}
	if (total && phase == BP_LOADING) {
		char a1[32], a2[32], line[96];
		format_size(done, a1, sizeof(a1));
		format_size(total, a2, sizeof(a2));
		snprintf(line, sizeof(line), "%s of %s", a1, a2);
		struct text_style sm = ts(a, a->fonts.medium, 13);
		draw_text_center(a, sm, W / 2, by + S(a, 36), line, t->text3);
	}
	for (int i = 0; i < a->nitems; i++)
		if (a->items[i].action == ACT_CANCEL)
			draw_pill_button(a, &a->items[i], "Cancel", false);
}

/* ------------------------------------------------------------ SETTINGS */

static const char *setting_title[] = { "Appearance", "Animations", "Interface size", "Show unsupported images",
				       "Remember the last started system", "Boot media", "Technical details" };
static const char *setting_desc[] = {
	"Light or dark colour theme.",
	"Smooth transitions between screens.",
	"Scale text and controls for very large or small displays.",
	"List images that Distro Buny recognises but cannot start yet.",
	"Offer a one-click restart of the system you started last time.",
	"How images are presented to the system that is started.",
	"Show boot parameters and medium diagnostics on image pages.",
};
#define NSETTINGS 7

static const char *setting_value(struct app *a, int i, char *buf, size_t len)
{
	switch (i) {
	case 0: return a->set.theme ? "Dark" : "Light";
	case 1: return a->set.animations ? "On" : "Off";
	case 2:
		if (!a->set.scale_pct)
			return "Automatic";
		snprintf(buf, len, "%d%%", a->set.scale_pct);
		return buf;
	case 3: return a->set.show_unsupported ? "Shown" : "Hidden";
	case 4: return a->set.remember_last ? "On" : "Off";
	case 5: return a->set.strategy == MEDIA_VIRTUAL ? "Virtual medium" : a->set.strategy == MEDIA_NATIVE ? "Native ISO file" : "Automatic";
	case 6: return a->set.show_details ? "Shown" : "Hidden";
	}
	return "";
}

static void layout_settings(struct app *a, struct screen *sc)
{
	layout_header(a, sc, false);
	float M = margin(a), w = fminf((float)a->W - 2 * M, S(a, 980));
	float x = ((float)a->W - w) / 2, y = content_top(a) - sc->scroll;
	for (int i = 0; i < NSETTINGS; i++) {
		add_item(a, (struct item){ .kind = IT_SETTING, .index = i, .r = { (int)x, (int)y, (int)w, (int)S(a, 84) }, .scrolls = true });
		y += S(a, 96);
	}
	sc->content_h = (float)NSETTINGS * S(a, 96);
	sc->view_h = (float)a->H - content_top(a) - S(a, 40);
}

static void draw_settings(struct app *a, struct screen *sc)
{
	struct theme *t = &a->th;
	draw_header(a, sc, "Settings", "Saved on the Distro Buny drive and applied immediately.");
	struct rect old = canvas_push_clip(a->frame, (struct rect){ 0, (int)(content_top(a) - S(a, 14)), a->W, a->H });
	for (int i = 0; i < a->nitems; i++) {
		struct item *it = &a->items[i];
		if (it->kind != IT_SETTING)
			continue;
		float h = anim_value(a, it->key);
		card(a, it->r, S(a, 20), h * 0.6f, false);
		float x = (float)it->r.x + S(a, 26), y = (float)it->r.y - h * S(a, 1.8f);
		struct text_style t1 = ts(a, a->fonts.semibold, 18), t2 = ts(a, a->fonts.regular, 14);
		text_draw(a->frame, &t1, x, y + S(a, 36), setting_title[it->index], t->text);
		text_draw(a->frame, &t2, x, y + S(a, 60), setting_desc[it->index], t->text2);
		char buf[32];
		const char *v = setting_value(a, it->index, buf, sizeof(buf));
		struct text_style vt = ts(a, a->fonts.semibold, 15);
		float vw = text_width(&vt, v) + S(a, 64);
		float vx = (float)(it->r.x + it->r.w) - vw - S(a, 22), vy = y + S(a, 22);
		gfx_fill_rrect(a->frame, vx, vy, vw, S(a, 40), S(a, 20), color_mix(t->surface_alt, t->accent_soft, h));
		icon_draw(a->frame, ICON_CHEVRON_LEFT, vx + S(a, 12), vy + S(a, 11), S(a, 18), color_alpha(t->text3, 0.8f));
		icon_draw(a->frame, ICON_CHEVRON_RIGHT, vx + vw - S(a, 30), vy + S(a, 11), S(a, 18), color_alpha(t->text3, 0.8f));
		draw_text_center(a, vt, vx + vw / 2, vy + S(a, 26), v, color_mix(t->text, t->accent, h));
	}
	a->frame->clip = old;
}

/* --------------------------------------------------------------- INFO */

static void layout_info(struct app *a, struct screen *sc)
{
	layout_header(a, sc, false);
	float M = margin(a);
	add_item(a, (struct item){ .kind = IT_BUTTON, .action = ACT_LOG, .r = { (int)((float)a->W - M - S(a, 220)), (int)S(a, 102), (int)S(a, 220), (int)S(a, 50) } });
}

static void kv(struct app *a, float x, float *y, float w, const char *k, const char *v)
{
	struct text_style kt = ts(a, a->fonts.medium, 14), vt = ts(a, a->fonts.regular, 16);
	text_draw(a->frame, &kt, x, *y, k, a->th.text3);
	draw_text_fit(a, vt, x + S(a, 150), *y, w - S(a, 150), v && *v ? v : "\xE2\x80\x94", a->th.text);
	*y += S(a, 30);
}

static void info_card(struct app *a, float x, float y, float w, float h, enum ui_icon icon, const char *title)
{
	card(a, (struct rect){ (int)x, (int)y, (int)w, (int)h }, S(a, 22), 0, false);
	gfx_fill_rrect(a->frame, x + S(a, 24), y + S(a, 22), S(a, 40), S(a, 40), S(a, 12), a->th.accent_soft);
	icon_draw(a->frame, icon, x + S(a, 32), y + S(a, 30), S(a, 24), a->th.accent);
	struct text_style tt = ts(a, a->fonts.semibold, 18);
	text_draw(a->frame, &tt, x + S(a, 78), y + S(a, 49), title, a->th.text);
}

static void draw_info(struct app *a, struct screen *sc)
{
	draw_header(a, sc, "System information", NULL);
	float M = margin(a), top = content_top(a) - S(a, 10);
	float gap = S(a, 24);
	bool two = (float)a->W > S(a, 1150);
	float cw = two ? ((float)a->W - 2 * M - gap) / 2 : (float)a->W - 2 * M;
	float chh = S(a, 230);
	const struct json *rt = a->runtime;
	char b1[64], b2[64], line[160];
	/* Distro Buny */
	float x = M, y = top;
	info_card(a, x, y, cw, chh, ICON_HOME, "Distro Buny");
	float yy = y + S(a, 96);
	kv(a, x + S(a, 26), &yy, cw - S(a, 52), "Version", BUNY_VERSION);
	kv(a, x + S(a, 26), &yy, cw - S(a, 52), "Runtime kernel", json_get_str(rt, "kernel", NULL));
	snprintf(line, sizeof(line), "%s, %dx%d", display_backend(a->disp), a->W, a->H);
	kv(a, x + S(a, 26), &yy, cw - S(a, 52), "Display", line);
	snprintf(line, sizeof(line), "%.1f s after the kernel started", json_get_num(rt, "boot_ms", 0) / 1000.0);
	kv(a, x + S(a, 26), &yy, cw - S(a, 52), "Runtime ready", line);
	/* Firmware */
	x = two ? M + cw + gap : M;
	y = two ? top : top + chh + gap;
	info_card(a, x, y, cw, chh, ICON_LOCK, "Firmware & security");
	yy = y + S(a, 96);
	const char *bm = json_get_str(rt, "boot_mode", "?");
	kv(a, x + S(a, 26), &yy, cw - S(a, 52), "Boot mode", !strcmp(bm, "uefi") ? "UEFI" : "Legacy BIOS");
	const char *sb = json_get_str(rt, "secure_boot", "unknown");
	kv(a, x + S(a, 26), &yy, cw - S(a, 52), "Secure Boot", !strcmp(sb, "enabled") ? "Enabled" : !strcmp(sb, "disabled") ? "Disabled" : "Unknown");
	const char *ld = json_get_str(rt, "lockdown", NULL);
	kv(a, x + S(a, 26), &yy, cw - S(a, 52), "Kernel lockdown", ld && !strcmp(ld, "none") ? "Off" : ld && *ld ? ld : NULL);
	kv(a, x + S(a, 26), &yy, cw - S(a, 52), "Firmware vendor", json_get_str(rt, "firmware_vendor", NULL));
	/* Computer */
	x = M;
	y = top + chh + gap + (two ? 0 : chh + gap);
	info_card(a, x, y, cw, chh, ICON_CHIP, "Computer");
	yy = y + S(a, 96);
	kv(a, x + S(a, 26), &yy, cw - S(a, 52), "Processor", json_get_str(rt, "cpu", NULL));
	snprintf(line, sizeof(line), "%d", (int)json_get_num(rt, "cpus", 0));
	kv(a, x + S(a, 26), &yy, cw - S(a, 52), "Logical CPUs", line);
	format_size((uint64_t)json_get_num(rt, "memory", 0), b1, sizeof(b1));
	kv(a, x + S(a, 26), &yy, cw - S(a, 52), "Memory", b1);
	int kb = 0, pt = 0;
	input_device_count(a->in, &kb, &pt);
	snprintf(line, sizeof(line), "%d keyboard%s, %d pointing device%s", kb, kb == 1 ? "" : "s", pt, pt == 1 ? "" : "s");
	kv(a, x + S(a, 26), &yy, cw - S(a, 52), "Input", line);
	/* Drive */
	x = two ? M + cw + gap : M;
	y = two ? top + chh + gap : y + chh + gap;
	info_card(a, x, y, cw, chh, ICON_DRIVE, "Distro Buny drive");
	yy = y + S(a, 96);
	const struct json *d = json_get(rt, "data");
	kv(a, x + S(a, 26), &yy, cw - S(a, 52), "Device", d ? json_get_str(d, "device", NULL) : "not found");
	snprintf(line, sizeof(line), "%s \xC2\xB7 %s", d ? json_get_str(d, "fs", "?") : "?", d ? json_get_str(d, "label", "") : "");
	kv(a, x + S(a, 26), &yy, cw - S(a, 52), "File system", line);
	format_size((uint64_t)(d ? json_get_num(d, "size", 0) : 0), b1, sizeof(b1));
	format_size((uint64_t)(d ? fmax(json_get_num(d, "free", 0), 0) : 0), b2, sizeof(b2));
	snprintf(line, sizeof(line), "%s, %s free", b1, b2);
	if (expand_offer(a)) {
		char b3[32];
		format_size((uint64_t)json_get_num(json_get(rt, "expand"), "disk_bytes", 0), b3, sizeof(b3));
		snprintf(line, sizeof(line), "%.31s of a %s drive, %.31s free", b1, b3, b2);
	}
	kv(a, x + S(a, 26), &yy, cw - S(a, 52), "Capacity", line);
	size_t ready = 0, unsupported = 0;
	if (a->have_catalog) {
		struct ptrvec st = { 0 };
		ptrvec_push(&st, a->cat.root);
		while (st.n) {
			struct cat_node *n = st.v[--st.n];
			if (n->kind == CAT_ISO) {
				if (n->meta && n->meta->level >= COMPAT_PREPARED && !n->meta->error)
					ready++;
				else if (n->meta)
					unsupported++;
			}
			for (size_t i = 0; i < n->nchildren; i++)
				ptrvec_push(&st, n->children[i]);
		}
		free(st.v);
	}
	format_size(a->have_catalog ? a->cat.total_bytes : 0, b1, sizeof(b1));
	snprintf(line, sizeof(line), "%zu images (%s): %zu ready, %zu not supported", a->have_catalog ? a->cat.total_isos : 0, b1, ready, unsupported);
	kv(a, x + S(a, 26), &yy, cw - S(a, 52), "Collection", line);
	for (int i = 0; i < a->nitems; i++)
		if (a->items[i].action == ACT_LOG)
			draw_pill_button(a, &a->items[i], "View log", false);
	(void)sc;
}

/* ---------------------------------------------------------------- LOG */

static void layout_log(struct app *a, struct screen *sc)
{
	layout_header(a, sc, false);
	sc->view_h = (float)a->H - content_top(a) - S(a, 40);
}

static void draw_log(struct app *a, struct screen *sc)
{
	draw_header(a, sc, "Diagnostic log", "Messages from this session (newest last). Nothing is sent anywhere.");
	char **lines;
	size_t n = log_ring_snapshot(&lines);
	struct text_style st = ts(a, a->fonts.regular, 14);
	float lh = S(a, 22), top = content_top(a);
	sc->content_h = (float)n * lh;
	float M = margin(a);
	struct rect cr = { (int)M, (int)top - (int)S(a, 10), a->W - 2 * (int)M, a->H - (int)top - (int)S(a, 30) };
	card(a, cr, S(a, 20), 0, false);
	struct rect old = canvas_push_clip(a->frame, (struct rect){ cr.x + 8, cr.y + 8, cr.w - 16, cr.h - 16 });
	for (size_t i = 0; i < n; i++) {
		float y = top + S(a, 20) + (float)i * lh - sc->scroll;
		if (y < top - lh || y > (float)a->H)
			continue;
		color_t col = str_starts(lines[i], "[error]") ? a->th.danger : str_starts(lines[i], "[warn]") ? a->th.warn : a->th.text2;
		draw_text_fit(a, st, M + S(a, 22), y, (float)cr.w - S(a, 44), lines[i], col);
	}
	a->frame->clip = old;
	log_ring_free(lines, n);
	draw_scroll_indicator(a, sc, top);
}

/* ------------------------------------------------------------- SEARCH */

static bool matches(const char *hay, const char *needle)
{
	return hay && str_icontains(hay, needle);
}

static void collect_search(struct app *a, struct cat_node *n, struct cat_node **out, int *cnt, int max)
{
	if (*cnt >= max)
		return;
	if (n->kind == CAT_ISO) {
		struct cat_meta *m = n->meta;
		if (!a->query[0] || matches(n->name, a->query) || matches(n->display, a->query) ||
		    (m && (matches(m->name, a->query) || matches(m->variant, a->query) || matches(m->version, a->query) ||
			   matches(m->label, a->query))))
			if (visible_child(a, n))
				out[(*cnt)++] = n;
		return;
	}
	for (size_t i = 0; i < n->nchildren; i++)
		collect_search(a, n->children[i], out, cnt, max);
}

static void layout_search(struct app *a, struct screen *sc)
{
	layout_header(a, sc, false);
	float M = margin(a);
	add_item(a, (struct item){ .kind = IT_TEXTFIELD, .r = { (int)M, (int)S(a, 100), a->W - 2 * (int)M, (int)S(a, 60) } });
	if (!a->have_catalog)
		return;
	struct cat_node *res[200];
	int n = 0;
	collect_search(a, a->cat.root, res, &n, 200);
	float y = S(a, 190) - sc->scroll;
	for (int i = 0; i < n; i++) {
		add_item(a, (struct item){ .kind = IT_RESULT, .node = res[i], .r = { (int)M, (int)y, a->W - 2 * (int)M, (int)S(a, 78) }, .scrolls = true });
		y += S(a, 90);
	}
	sc->content_h = (float)n * S(a, 90);
	sc->view_h = (float)a->H - S(a, 190) - S(a, 30);
}

static void draw_search(struct app *a, struct screen *sc)
{
	struct theme *t = &a->th;
	for (int i = 0; i < a->nitems; i++)
		if (a->items[i].kind == IT_BACK)
			draw_round_button(a, &a->items[i], ICON_BACK);
	struct text_style ct = ts(a, a->fonts.medium, 15);
	text_draw(a->frame, &ct, margin(a) + S(a, 64), S(a, 58), "Search all images", t->text2);
	int results = 0;
	struct rect old = canvas_push_clip(a->frame, (struct rect){ 0, (int)S(a, 176), a->W, a->H });
	for (int i = 0; i < a->nitems; i++) {
		struct item *it = &a->items[i];
		if (it->kind == IT_RESULT) {
			results++;
			float h = anim_value(a, it->key);
			card(a, it->r, S(a, 18), h * 0.6f, false);
			float x = (float)it->r.x + S(a, 18), y = (float)it->r.y - h * S(a, 1.8f);
			icon_tile(a, it->node, x, y + S(a, 13), S(a, 52));
			struct cat_meta *m = it->node->meta;
			struct text_style t1 = ts(a, a->fonts.semibold, 18), t2 = ts(a, a->fonts.regular, 14);
			char title[300];
			snprintf(title, sizeof(title), "%s%s%s", iso_title(it->node), m && m->variant ? " \xC2\xB7 " : "", m && m->variant ? m->variant : "");
			draw_text_fit(a, t1, x + S(a, 70), y + S(a, 34), (float)it->r.w - S(a, 260), title, t->text);
			draw_text_fit(a, t2, x + S(a, 70), y + S(a, 58), (float)it->r.w - S(a, 260), it->node->rel_path, t->text3);
			color_t fg, bg;
			const char *lab = compat_label(a, it->node, &fg, &bg);
			pill(a, (float)(it->r.x + it->r.w) - pill_width(a, lab, 13) - S(a, 22), y + S(a, 24), lab, fg, bg, 13);
		}
	}
	a->frame->clip = old;
	for (int i = 0; i < a->nitems; i++) {
		struct item *it = &a->items[i];
		if (it->kind != IT_TEXTFIELD)
			continue;
		float x = (float)it->r.x, y = (float)it->r.y, w = (float)it->r.w, h = (float)it->r.h;
		gfx_shadow(a->frame, x, y, w, h, h / 2, S(a, 14), S(a, 4), t->shadow);
		gfx_fill_rrect(a->frame, x, y, w, h, h / 2, t->surface);
		gfx_stroke_rrect(a->frame, x, y, w, h, h / 2, S(a, 2), t->brand);
		icon_draw(a->frame, ICON_SEARCH, x + S(a, 20), y + (h - S(a, 24)) / 2, S(a, 24), t->accent);
		struct text_style qt = ts(a, a->fonts.medium, 20);
		float tx = x + S(a, 58);
		const char *q = a->query[0] ? a->query : "Type a name, version or edition";
		float tw = text_draw(a->frame, &qt, tx, y + h / 2 + S(a, 7), q, a->query[0] ? t->text : t->text3);
		if (((a->now / 530) & 1) == 0 || !a->query[0]) {
			float cx = a->query[0] ? tx + tw + S(a, 2) : tx - S(a, 4);
			gfx_fill_rect(a->frame, (struct rect){ (int)cx, (int)(y + S(a, 16)), (int)fmaxf(1, S(a, 2)), (int)(h - S(a, 32)) }, t->accent);
		}
		char count[64];
		snprintf(count, sizeof(count), "%d result%s", results, results == 1 ? "" : "s");
		struct text_style cs = ts(a, a->fonts.medium, 14);
		float cw = text_width(&cs, count);
		text_draw(a->frame, &cs, x + w - cw - S(a, 26), y + h / 2 + S(a, 5), count, t->text3);
	}
	draw_scroll_indicator(a, sc, S(a, 190));
}

/* ------------------------------------------------------------- NODATA */

static void layout_nodata(struct app *a, struct screen *sc)
{
	float W = (float)a->W, H = (float)a->H, bw = S(a, 240);
	add_item(a, (struct item){ .kind = IT_BUTTON, .action = ACT_RESCAN, .r = { (int)(W / 2 - bw - S(a, 8)), (int)(H / 2 + S(a, 90)), (int)bw, (int)S(a, 52) } });
	add_item(a, (struct item){ .kind = IT_BUTTON, .action = ACT_POWER, .r = { (int)(W / 2 + S(a, 8)), (int)(H / 2 + S(a, 90)), (int)bw, (int)S(a, 52) } });
	(void)sc;
}

static void draw_nodata(struct app *a, struct screen *sc)
{
	float W = (float)a->W, H = (float)a->H;
	if (a->logo)
		image_draw(a->frame, a->logo, (int)(W / 2 - S(a, 60)), (int)(H / 2 - S(a, 280)), (int)S(a, 120), (int)S(a, 120), 1.0f);
	struct text_style tt = ts(a, a->fonts.bold, 30), st = ts(a, a->fonts.regular, 17);
	draw_text_center(a, tt, W / 2, H / 2 - S(a, 100), "The Distro Buny drive was not found", a->th.text);
	text_draw_wrapped(a->frame, &st, W / 2 - S(a, 330), H / 2 - S(a, 56), S(a, 660),
			  "The runtime started, but the data partition with your ISO images is not visible. "
			  "If the drive is connected through a hub or an adapter, try another port, then choose Try again.",
			  a->th.text2, 4, 1.25f);
	for (int i = 0; i < a->nitems; i++) {
		if (a->items[i].action == ACT_RESCAN)
			draw_pill_button(a, &a->items[i], "Try again", true);
		else if (a->items[i].action == ACT_POWER)
			draw_pill_button(a, &a->items[i], "Power", false);
	}
	(void)sc;
}

/* ------------------------------------------------------------ dialogs */

void layout_dialog(struct app *a)
{
	a->nditems = 0;
	if (a->dlg.kind == DLG_NONE)
		return;
	float W = (float)a->W, H = (float)a->H, dw = fminf(S(a, 560), W - S(a, 40));
	float x = (W - dw) / 2;
	struct { enum action act; int index; } rows[16];
	int n = 0;
	if (a->dlg.kind == DLG_POWER) {
		rows[n++] = (typeof(rows[0])){ ACT_REBOOT, 0 };
		rows[n++] = (typeof(rows[0])){ ACT_POWEROFF, 0 };
		if (json_get_bool(a->runtime, "firmware_setup_supported", false))
			rows[n++] = (typeof(rows[0])){ ACT_FIRMWARE, 0 };
		rows[n++] = (typeof(rows[0])){ ACT_CANCEL, 0 };
	} else if (a->dlg.kind == DLG_OPTIONS) {
		struct cat_node *nd = a->stack[a->depth - 1].node;
		int nv = nd && nd->meta ? (int)nd->meta->variants.n : 0;
		for (int i = 0; i < nv && n < 10; i++)
			rows[n++] = (typeof(rows[0])){ ACT_VARIANT, i };
		rows[n++] = (typeof(rows[0])){ ACT_TOGGLE_SAFE, 0 };
		rows[n++] = (typeof(rows[0])){ ACT_STRATEGY, 0 };
		rows[n++] = (typeof(rows[0])){ ACT_BOOT, 0 };
		rows[n++] = (typeof(rows[0])){ ACT_CANCEL, 0 };
	} else if (a->dlg.kind == DLG_CONFIRM) {
		rows[n++] = (typeof(rows[0])){ ACT_CANCEL, 0 };  /* the safe choice has the initial focus */
		rows[n++] = (typeof(rows[0])){ ACT_CONFIRM_BOOT, 0 };
	} else if (a->dlg.kind == DLG_EXPAND) {
		rows[n++] = (typeof(rows[0])){ ACT_CANCEL, 0 };  /* writing to the drive needs a deliberate choice */
		rows[n++] = (typeof(rows[0])){ ACT_CONFIRM_EXPAND, 0 };
	} else if (a->dlg.kind == DLG_BUSY) {
		rows[n++] = (typeof(rows[0])){ ACT_NONE, 0 };    /* "Please wait", cannot be chosen */
	} else {
		if (a->dlg.retry)
			rows[n++] = (typeof(rows[0])){ ACT_RETRY, 0 };
		if (a->dlg.firmware)
			rows[n++] = (typeof(rows[0])){ ACT_FIRMWARE, 0 };
		rows[n++] = (typeof(rows[0])){ ACT_DISMISS, 0 };
	}
	float rh = S(a, 54), gap = S(a, 10);
	struct text_style mt = ts(a, a->fonts.regular, 16);
	float msg_h = 0;
	if (a->dlg.message[0]) {
		struct font_metrics m;
		font_metrics(&mt, &m);
		msg_h = (float)BUNY_MIN(text_count_lines(&mt, dw - S(a, 64), a->dlg.message), 8) * m.line_height * 1.25f + S(a, 16);
	}
	float dh = S(a, 96) + msg_h + (float)n * (rh + gap) + S(a, 20);
	float y = (H - dh) / 2 + S(a, 96) + msg_h;
	for (int i = 0; i < n; i++) {
		a->ditems[a->nditems++] = (struct item){ .kind = IT_DIALOG, .action = rows[i].act, .index = rows[i].index,
							 .r = { (int)(x + S(a, 28)), (int)y, (int)(dw - S(a, 56)), (int)rh },
							 .disabled = rows[i].act == ACT_NONE,
							 .key = (const void *)(uintptr_t)(0x900000 + rows[i].act * 64 + rows[i].index) };
		y += rh + gap;
	}
}

static const char *dialog_label(struct app *a, const struct item *it, char *buf, size_t len)
{
	switch (it->action) {
	case ACT_REBOOT: return "Restart";
	case ACT_POWEROFF: return "Shut down";
	case ACT_FIRMWARE: return "Restart into firmware settings";
	case ACT_CANCEL: return "Cancel";
	case ACT_RETRY: return "Try again";
	case ACT_DISMISS: return "Close";
	case ACT_CONFIRM_BOOT: return "Start anyway";
	case ACT_CONFIRM_EXPAND: return "Expand DISTROBUNY";
	case ACT_NONE: return a->dlg.kind == DLG_BUSY ? "Please wait\xE2\x80\xA6" : "";
	case ACT_BOOT: return "Start with these options";
	case ACT_TOGGLE_SAFE:
		snprintf(buf, len, "Safe graphics (nomodeset): %s", a->boot.safe_graphics ? "On" : "Off");
		return buf;
	case ACT_STRATEGY:
		snprintf(buf, len, "Media: %s", a->boot.strategy == MEDIA_VIRTUAL ? "Virtual medium" : a->boot.strategy == MEDIA_NATIVE ? "Native ISO file" : "Automatic");
		return buf;
	case ACT_VARIANT: {
		struct cat_node *nd = a->stack[a->depth - 1].node;
		const char *v = nd && nd->meta && (size_t)it->index < nd->meta->variants.n ? nd->meta->variants.v[it->index] : "?";
		snprintf(buf, len, "%s", v);
		return buf;
	}
	default: return "";
	}
}

static void draw_dialog(struct app *a)
{
	if (a->dlg.kind == DLG_NONE)
		return;
	struct theme *t = &a->th;
	float k = ease_out(a->dlg.appear);
	gfx_fill_rect(a->frame, (struct rect){ 0, 0, a->W, a->H }, color_alpha(t->scrim, k));
	if (!a->nditems)
		return;
	float W = (float)a->W, dw = fminf(S(a, 560), W - S(a, 40));
	float x = (W - dw) / 2;
	struct rect first = a->ditems[0].r, last = a->ditems[a->nditems - 1].r;
	struct text_style mt = ts(a, a->fonts.regular, 16);
	struct font_metrics m;
	font_metrics(&mt, &m);
	float msg_h = a->dlg.message[0] ? (float)BUNY_MIN(text_count_lines(&mt, dw - S(a, 64), a->dlg.message), 8) * m.line_height * 1.25f + S(a, 16) : 0;
	float y0 = (float)first.y - S(a, 96) - msg_h;
	float dh = (float)(last.y + last.h) - y0 + S(a, 28);
	float off = (1 - k) * S(a, 24);
	gfx_shadow(a->frame, x, y0 + off, dw, dh, S(a, 28), S(a, 40), S(a, 16), RGBA(0, 0, 0, (uint32_t)(70 * k)));
	gfx_fill_rrect(a->frame, x, y0 + off, dw, dh, S(a, 28), color_alpha(t->surface, k));
	enum ui_icon ic = a->dlg.kind == DLG_POWER ? ICON_POWER : a->dlg.kind == DLG_OPTIONS ? ICON_LIST :
			  a->dlg.kind == DLG_ERROR || a->dlg.kind == DLG_CONFIRM ? ICON_WARNING :
			  a->dlg.kind == DLG_EXPAND || a->dlg.kind == DLG_BUSY ? ICON_DRIVE : ICON_INFO;
	color_t icol = a->dlg.kind == DLG_ERROR ? t->danger : a->dlg.kind == DLG_CONFIRM ? t->warn : t->accent;
	icon_draw(a->frame, ic, x + S(a, 30), y0 + off + S(a, 30), S(a, 30), color_alpha(icol, k));
	struct text_style tt = ts(a, a->fonts.semibold, 22);
	draw_text_fit(a, tt, x + S(a, 74), y0 + off + S(a, 54), dw - S(a, 104), a->dlg.title, color_alpha(t->text, k));
	if (a->dlg.message[0])
		text_draw_wrapped(a->frame, &mt, x + S(a, 32), y0 + off + S(a, 100), dw - S(a, 64), a->dlg.message, color_alpha(t->text2, k), 8, 1.25f);
	struct cat_node *nd = a->depth ? a->stack[a->depth - 1].node : NULL;
	for (int i = 0; i < a->nditems; i++) {
		struct item *it = &a->ditems[i];
		float h = anim_value(a, it->key);
		float bx = (float)it->r.x, by = (float)it->r.y + off, bw = (float)it->r.w, bh = (float)it->r.h;
		bool primary = it->action == ACT_BOOT || it->action == ACT_RETRY || (it->action == ACT_CANCEL && a->dlg.kind == DLG_CONFIRM) ||
			       it->action == ACT_CONFIRM_EXPAND ||
			       (it->action == ACT_DISMISS && !a->dlg.retry && !a->dlg.firmware) || it->action == ACT_REBOOT ||
			       (it->action == ACT_FIRMWARE && a->dlg.kind == DLG_ERROR);
		bool selected = it->action == ACT_VARIANT && it->index == (a->boot.variant < 0 ? 0 : a->boot.variant);
		bool danger = it->action == ACT_CONFIRM_BOOT;
		color_t fill = primary ? color_mix(t->accent, t->brand, h * 0.4f) :
			       danger ? color_mix(t->danger_soft, t->danger, h * 0.25f) :
			       color_mix(t->surface_alt, t->accent_soft, fmaxf(h, selected ? 0.7f : 0));
		gfx_fill_rrect(a->frame, bx, by, bw, bh, S(a, 16), color_alpha(fill, k));
		char buf[300];
		const char *label = dialog_label(a, it, buf, sizeof(buf));
		struct text_style bt = ts(a, a->fonts.semibold, 16);
		color_t fg = primary ? t->accent_text : danger ? t->danger : it->disabled ? t->text3 : t->text;
		float lx = bx + S(a, 22);
		if (it->action == ACT_VARIANT) {
			if (selected)
				icon_draw(a->frame, ICON_CHECK, bx + S(a, 16), by + (bh - S(a, 20)) / 2, S(a, 20), color_alpha(t->accent, k));
			lx = bx + S(a, 48);
		}
		draw_text_fit(a, bt, lx, by + bh / 2 + S(a, 6), bw - (lx - bx) - S(a, 20), label, color_alpha(fg, k));
	}
	(void)nd;
}

/* ---------------------------------------------------------- top level */

void layout_screen(struct app *a, struct screen *sc)
{
	a->nitems = 0;
	switch (sc->kind) {
	case SCR_HOME: layout_home(a, sc); break;
	case SCR_FOLDER: layout_folder(a, sc); break;
	case SCR_DETAIL: layout_detail(a, sc); break;
	case SCR_BOOT: layout_boot(a, sc); break;
	case SCR_SETTINGS: layout_settings(a, sc); break;
	case SCR_INFO: layout_info(a, sc); break;
	case SCR_SEARCH: layout_search(a, sc); break;
	case SCR_LOG: layout_log(a, sc); break;
	case SCR_NODATA: layout_nodata(a, sc); break;
	}
}

static void draw_screen(struct app *a, struct screen *sc)
{
	draw_background(a);
	switch (sc->kind) {
	case SCR_HOME: draw_home(a, sc); break;
	case SCR_FOLDER: draw_folder(a, sc); break;
	case SCR_DETAIL: draw_detail(a, sc); break;
	case SCR_BOOT: draw_boot(a, sc); break;
	case SCR_SETTINGS: draw_settings(a, sc); break;
	case SCR_INFO: draw_info(a, sc); break;
	case SCR_SEARCH: draw_search(a, sc); break;
	case SCR_LOG: draw_log(a, sc); break;
	case SCR_NODATA: draw_nodata(a, sc); break;
	}
}

static void draw_focus(struct app *a)
{
	if (!a->focus_visible || !a->keyboard_mode)
		return;
	float *r = a->focus_rect;
	if (r[2] <= 0)
		return;
	float pad = S(a, 5);
	float radius = S(a, 26);
	struct item *f = NULL;
	struct screen *sc = &a->stack[a->depth - 1];
	if (a->dlg.kind != DLG_NONE) {
		if (a->dlg.focus >= 0 && a->dlg.focus < a->nditems)
			f = &a->ditems[a->dlg.focus];
	} else if (sc->focus >= 0 && sc->focus < a->nitems) {
		f = &a->items[sc->focus];
	}
	if (f) {
		if (f->kind == IT_BACK || (f->kind == IT_BUTTON && f->r.w == f->r.h))
			radius = (float)f->r.w / 2 + pad;
		else if (f->kind == IT_BUTTON || f->kind == IT_TEXTFIELD)
			radius = (float)f->r.h / 2 + pad;
		else if (f->kind == IT_DIALOG || f->kind == IT_CRUMB)
			radius = S(a, 18);
		else if (f->kind == IT_SETTING || f->kind == IT_RESULT)
			radius = S(a, 22);
	}
	gfx_stroke_rrect(a->frame, r[0] - pad, r[1] - pad - S(a, 3), r[2] + 2 * pad, r[3] + 2 * pad, radius, S(a, 3), a->th.focus);
}

static void draw_pointer(struct app *a)
{
	if (!a->pointer_visible)
		return;
	struct path p;
	path_init(&p);
	static const float pts[] = { 0, 0, 0, 17, 4.6f, 13, 7.6f, 19.6f, 10.2f, 18.5f, 7.2f, 12, 12.6f, 12 };
	path_move(&p, pts[0], pts[1]);
	for (int i = 1; i < 7; i++)
		path_line(&p, pts[2 * i], pts[2 * i + 1]);
	path_close(&p);
	float sc = fmaxf(1.0f, a->s * 1.1f);
	path_fill(a->frame, &p, sc, a->px + 1, a->py + 1, RGBA(0, 0, 0, 60));
	path_fill(a->frame, &p, sc, a->px, a->py, a->th.dark ? HEXC(0xF4F7F0) : HEXC(0x1D2817));
	path_stroke(a->frame, &p, sc, a->px, a->py, fmaxf(1.0f, sc * 1.1f), a->th.dark ? HEXC(0x1D2817) : HEXC(0xFFFFFF));
	path_free(&p);
}

static void draw_toast(struct app *a)
{
	if (a->now >= a->toast_until || !a->toast[0])
		return;
	struct text_style st = ts(a, a->fonts.medium, 15);
	float w = text_width(&st, a->toast) + S(a, 48), h = S(a, 46);
	float x = ((float)a->W - w) / 2, y = (float)a->H - S(a, 190);
	float k = BUNY_CLAMP((float)(a->toast_until - a->now) / 250.0f, 0.0f, 1.0f);
	gfx_shadow(a->frame, x, y, w, h, h / 2, S(a, 16), S(a, 6), RGBA(0, 0, 0, (uint32_t)(60 * k)));
	gfx_fill_rrect(a->frame, x, y, w, h, h / 2, color_alpha(a->th.dark ? HEXC(0xE9F0E2) : HEXC(0x26331E), k));
	text_draw(a->frame, &st, x + S(a, 24), y + h / 2 + S(a, 5), a->toast, color_alpha(a->th.dark ? HEXC(0x1D2817) : HEXC(0xFFFFFF), k));
}

void render_frame(struct app *a)
{
	struct screen *sc = &a->stack[a->depth - 1];
	if (a->transitioning && a->prev) {
		float t = a->set.animations ? (float)(a->now - a->trans_start) / 260.0f : 1.0f;
		if (t >= 1.0f) {
			a->transitioning = false;
		} else {
			/* render the new screen into the scene canvas, then cross-fade with a short slide */
			struct canvas *saved = a->frame;
			a->frame = a->scene;
			draw_screen(a, sc);
			a->frame = saved;
			float e = ease_out(t);
			gfx_copy(a->frame, 0, 0, a->prev, (struct rect){ 0, 0, a->W, a->H });
			float dx = (float)a->trans_dir * (1 - e) * S(a, 56);
			gfx_draw_canvas(a->frame, a->scene, (int)dx, 0, e);
			if (dx > 0)
				gfx_copy(a->frame, 0, 0, a->prev, (struct rect){ 0, 0, (int)dx, a->H });
			draw_pointer(a);
			return;
		}
	}
	draw_screen(a, sc);
	draw_focus(a);
	draw_dialog(a);
	draw_toast(a);
	draw_pointer(a);
}
