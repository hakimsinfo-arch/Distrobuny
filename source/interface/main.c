/*
 * buny-ui — the Distro Buny graphical interface
 *
 * Runs unprivileged inside the Distro Buny runtime (started by buny-init),
 * or on a development machine with the offscreen backend:
 *
 *   BUNY_DISPLAY=offscreen:1920x1080 BUNY_DATA="Iso layout" \
 *   BUNY_SHARE=build/runtime-root/usr/share/distrobuny \
 *   BUNY_SCRIPT="wait:500;shot:/tmp/home.png;key:Enter;wait:400;shot:/tmp/desktop.png" buny-ui
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/log.h"
#include "interface/app.h"

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	signal(SIGPIPE, SIG_IGN);
	log_set_tag("buny-ui");
	log_set_level(getenv("BUNY_DEBUG") ? LOG_LVL_DEBUG : LOG_LVL_INFO);
	static struct app app;
	if (app_init(&app) < 0) {
		fprintf(stderr, "buny-ui: initialisation failed\n");
		return 1;
	}
	app_run(&app);
	app_free(&app);
	return 0;
}
