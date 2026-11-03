/*
 * Distro Buny — ISO probing and identification
 *
 * Opening a probe reads only metadata: volume descriptors, a handful of small
 * text files (.disk/info, .treeinfo, boot configurations) and the kernel's
 * setup header.  A probe of a typical ISO touches well under 1 MiB.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BUNY_IDENTIFY_H
#define BUNY_IDENTIFY_H

#include <stdbool.h>
#include <stdint.h>

#include "boot-adapters/bootcfg.h"
#include "filesystem/iso9660.h"
#include "iso-manager/rules.h"

struct iso_identity {
	char *rule_id;      /* rules section id, NULL if unrecognised */
	char *name;         /* "Kubuntu" (falls back to a name derived from the file name) */
	char *variant;      /* "KDE Plasma", "Live Server", ... */
	char *version;      /* "26.04.1 LTS" */
	char *icon;         /* icon id for the interface */
	char *vendor;       /* "ubuntu", "fedora", ... */
	char *arch;         /* from the kernel header: "x86_64", "i386", or NULL */
	char *kernel_version;  /* from the kernel header, e.g. "7.0.0-30-generic" */
	bool kernel_signed;    /* the kernel carries an Authenticode signature (PE certificate table) */
	char *kernel_signer;   /* signer name found in the signature, if any */
	char *info_line;    /* first line of /.disk/info, if any */
	bool recognized;
};

/* Environment shared by all probes in a scan. */
struct probe_env {
	const struct ruleset *rules;
	const char *data_fs_uuid;   /* data partition filesystem UUID (for parameter generation) */
	const char *data_fs_label;
	const char *data_fs_type;   /* "exfat", "ext4", ... */
};

struct iso_probe {
	char *host_path;      /* path used to open the file */
	char *iso_path;       /* path relative to the data partition root, with leading '/' */
	char *file_name;
	uint64_t file_size;
	struct iso_image *iso;
	struct iso_info info;
	char *disk_info;      /* /.disk/info */
	char *tree_name, *tree_version, *tree_variant, *tree_arch;
	struct boot_entries loopback;   /* evaluated with iso_path set (ISO-file aware) */
	struct boot_entries grub;       /* main GRUB config, evaluated as on a real medium */
	struct boot_entries sdboot;
	struct boot_entries syslinux;
	char *loopback_cfg;   /* config that produced loopback entries */
	char *grub_cfg;       /* config that produced grub entries */
	bool loopback_native; /* loopback entries reference the ISO path: native ISO-file support */
	struct iso_identity id;
	char *error;          /* set when the image could not be read */
};

/* Probe an ISO.  Always fills p (check p->error).  Returns 0 on success. */
int iso_probe_open(struct iso_probe *p, const char *host_path, const char *iso_path, const struct probe_env *env);
void iso_probe_close(struct iso_probe *p);

/* Identify a folder (category) by name; returns icon id (static or rule-owned) and display name. */
void identify_folder(const struct ruleset *rules, const char *folder_name, const char **icon, char **display_name,
		     const char **description);

/* Pretty name derived from a file name when no rule matches ("my-distro_1.2.iso" -> "My Distro 1.2"). */
char *name_from_filename(const char *file_name);

/* Read the x86 boot protocol header of a kernel inside the ISO. */
int kernel_header_info(struct iso_image *iso, const char *kernel_path, char **arch, char **version);
/* Authenticode signature of a kernel: returns true if signed; *signer receives a name if found. */
bool kernel_signature_info(struct iso_image *iso, const char *kernel_path, char **signer);

#endif
