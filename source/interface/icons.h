/*
 * Distro Buny — icons
 *
 * Interface icons are drawn as vector shapes at any size.  Distribution
 * marks come from the generated icon set (Simple Icons path data, or a
 * monogram badge for distributions without an official mark).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BUNY_ICONS_H
#define BUNY_ICONS_H

#include "interface/font.h"
#include "interface/gfx.h"

enum ui_icon {
	ICON_BACK, ICON_CHEVRON_RIGHT, ICON_CHEVRON_LEFT, ICON_CHEVRON_DOWN, ICON_GEAR, ICON_INFO, ICON_POWER, ICON_SEARCH, ICON_HOME,
	ICON_FOLDER, ICON_MONITOR, ICON_SERVER, ICON_GAMEPAD, ICON_TERMINAL, ICON_TOOLS, ICON_SHIELD, ICON_CHIP,
	ICON_DRIVE, ICON_CHECK, ICON_WARNING, ICON_CLOSE, ICON_RESTART, ICON_DISC, ICON_PLAY, ICON_LOCK, ICON_LIST,
	ICON_MEMORY,
};

void icon_draw(struct canvas *c, enum ui_icon id, float x, float y, float size, color_t col);
/* Map an icon name from the catalog ("cat-desktop", "folder", ...) to a UI icon; -1 if none. */
int icon_from_name(const char *name);

struct distro_icons;
struct distro_icons *distro_icons_load(const char *path);
void distro_icons_free(struct distro_icons *d);
/* Draw a distribution mark centred in a size x size box.  Returns false if the id is unknown. */
bool distro_icon_draw(struct distro_icons *d, struct canvas *c, const char *id, float x, float y, float size,
		      struct font *mono_font, float opacity);
/* Brand colour of a mark (for tints); default if unknown. */
color_t distro_icon_color(struct distro_icons *d, const char *id, color_t def);
bool distro_icon_known(struct distro_icons *d, const char *id);

#endif
