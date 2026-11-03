/*
 * Distro Buny — firmware and Secure Boot state
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BUNY_SECUREBOOT_H
#define BUNY_SECUREBOOT_H

#include <stdbool.h>

#include "common/util.h"

enum sb_state { SB_UNKNOWN = 0, SB_DISABLED, SB_ENABLED };

struct fw_state {
	bool uefi;                 /* booted through UEFI */
	enum sb_state secure_boot;
	bool setup_mode;           /* firmware in Secure Boot setup mode (no PK enrolled) */
	bool fw_ui_supported;      /* OsIndications BOOT_TO_FW_UI supported */
	char lockdown[24];         /* "none", "integrity", "confidentiality", "" if unknown */
	char fw_vendor[64];
};

void fw_state_read(const char *sysdir, struct fw_state *st);
const char *sb_state_name(enum sb_state s);
/* Subjects of the X.509 keys the running kernel holds (built-in, UEFI db/MOK
 * platform keys, machine keys), e.g. "Canonical Ltd. Master Certificate
 * Authority", read from /proc/keys (needs root).  kexec under Secure Boot only
 * accepts kernels signed by a chain ending in one of the trusted ones. */
int kernel_key_subjects(const char *proc_keys, struct ptrvec *out);
/* Request "boot into firmware setup" on the next reboot (writes OsIndications). */
int fw_request_setup(const char *efivars_dir);

#endif
