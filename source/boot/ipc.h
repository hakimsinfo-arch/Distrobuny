/*
 * Distro Buny — runtime IPC between buny-init (privileged) and buny-ui
 *
 * The interface parses untrusted ISO content and therefore runs without
 * privileges.  Every operation that needs them is requested from init over a
 * SOCK_SEQPACKET socket pair; init validates each request.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BUNY_IPC_H
#define BUNY_IPC_H

#include <stdint.h>

#define BUNY_IPC_FD_ENV "BUNY_IPC_FD"
#define BUNY_IPC_MAX_FDS 2

enum ipc_type {
	IPC_BOOT = 1,     /* fds: kernel, initrd.  path: ISO path on the data partition.  text: cmdline */
	IPC_EXECUTE,      /* jump into the kernel loaded by the last successful IPC_BOOT */
	IPC_POWER,        /* arg: enum power_action */
	IPC_SAVE,         /* path: state file name (whitelisted).  text: contents */
	IPC_RESCAN,       /* re-probe block devices (drive hot-plug) */
	IPC_REPLY,        /* status: 0 or errno-style code.  text: message */
	IPC_EXPAND,       /* grow the data partition to the end of the drive (confirmed by the user) */
};

enum power_action { POWER_REBOOT = 0, POWER_OFF = 1, POWER_FIRMWARE = 2 };

#define IPC_BOOT_NEEDS_MEDIUM 0x1u   /* attach the virtual medium (pre-init) */
#define IPC_BOOT_DRY_RUN 0x2u        /* validate only, do not load (tests) */

struct ipc_msg {
	uint32_t type;
	uint32_t arg;
	int32_t status;
	uint32_t reserved;
	char path[1024];
	char label[64];
	char text[6144];
};

int ipc_send(int sock, const struct ipc_msg *m, const int *fds, int nfds);
/* Receives one message; returns 0, or -1 on error/EOF.  Unused fds are closed. */
int ipc_recv(int sock, struct ipc_msg *m, int *fds, int *nfds);

#endif
