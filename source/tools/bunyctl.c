/*
 * bunyctl — Distro Buny host command-line tool
 *
 * Uses exactly the same core library as the boot-time runtime, so what the
 * build pipeline and the test suite report is what the boot interface does.
 *
 *   bunyctl probe [options] ISO...        identify images and show boot plans
 *   bunyctl scan  [options] DIR           catalog a directory tree (JSON index)
 *   bunyctl extract ISO PATH OUT          copy one file out of an ISO
 *   bunyctl adapters                      list boot adapters
 *   bunyctl map --image IMG --iso-path PATH
 *                                         print the on-disk extents of a file on the
 *                                         image's exFAT data partition (diagnostics)
 *   bunyctl stage --image IMG --iso HOSTISO --iso-path PATH --preinit BIN --out DIR
 *                                         prepare kernel, initrd (+ virtual medium
 *                                         overlay) and cmdline for an ISO stored in a
 *                                         Distro Buny disk image (lab testing)
 *   bunyctl expand --image IMG [--check] [--stop-after N]
 *                                         grow the image's DISTROBUNY partition to the
 *                                         end of the image file, as the runtime does on
 *                                         a bigger stick (disk image files only)
 *
 * Options:
 *   --rules FILE        identification rules (default: configuration/distributions/distributions.conf)
 *   --json              machine-readable output
 *   --root DIR          data partition root; ISO paths are computed relative to it
 *   --uuid UUID         data partition filesystem UUID (for native parameters)
 *   --fs TYPE           data partition filesystem type
 *   --strategy S        auto | virtual | native
 *   --entries           also print every boot entry found in the image
 *   --index FILE        (scan) write the catalog index to FILE
 *   --serial-console    (stage) mirror the distribution's console to ttyS0
 *   --check             (expand) only report what would be done
 *   --stop-after N      (expand) stop after N writes, like a power cut (tests)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "boot-adapters/adapters.h"
#include "boot/handoff.h"
#include "filesystem/blockdev.h"
#include "filesystem/exfat.h"
#include "filesystem/expand.h"
#include "common/json.h"
#include "common/log.h"
#include "iso-manager/catalog.h"

#ifndef BUNY_VERSION
#define BUNY_VERSION "dev"
#endif

struct opts {
	const char *rules_path;
	const char *root;
	const char *uuid;
	const char *fs;
	const char *index;
	enum media_strategy strategy;
	const char *image, *iso, *iso_path, *preinit, *out;
	bool serial_console;
	bool check;
	unsigned stop_after;
	bool json;
	bool entries;
	int variant;
};

static void usage(void)
{
	fprintf(stderr, "usage: bunyctl {probe|scan|extract|adapters|map|stage|expand|version} [options] ...\n"
			"see the header of source/tools/bunyctl.c or documentation/DEVELOPER_GUIDE.md\n");
}

static int load_rules(struct ruleset *rs, const struct opts *o)
{
	char err[256];
	const char *path = o->rules_path ? o->rules_path : "configuration/distributions/distributions.conf";
	if (rules_load(rs, path, err, sizeof(err)) < 0) {
		fprintf(stderr, "bunyctl: %s\n", err);
		return -1;
	}
	return 0;
}

static char *rel_to_root(const char *iso, const char *root)
{
	char a[PATH_MAX], b[PATH_MAX];
	if (!root || !realpath(iso, a) || !realpath(root, b))
		return xasprintf("/%s", path_basename(iso));
	size_t bl = strlen(b);
	if (strncmp(a, b, bl) == 0 && a[bl] == '/')
		return xstrdup(a + bl);
	return xasprintf("/%s", path_basename(iso));
}

static void print_entry(const char *tag, const struct boot_entry *e)
{
	char *cl = entry_cmdline(e);
	printf("    [%s]%s %s\n      kernel  %s\n", tag, e->is_default ? "*" : " ", e->title ? e->title : "", e->kernel ? e->kernel : "-");
	for (size_t i = 0; i < e->initrds.n; i++)
		printf("      initrd  %s\n", (char *)e->initrds.v[i]);
	printf("      args    %s\n", cl);
	free(cl);
}

static void entry_json(struct json_writer *w, const struct boot_entry *e)
{
	jw_obj_begin(w);
	jw_key(w, "title");
	jw_str(w, e->title);
	jw_key(w, "kernel");
	jw_str(w, e->kernel);
	jw_key(w, "initrds");
	jw_arr_begin(w);
	for (size_t i = 0; i < e->initrds.n; i++)
		jw_str(w, e->initrds.v[i]);
	jw_arr_end(w);
	char *cl = entry_cmdline(e);
	jw_key(w, "cmdline");
	jw_str(w, cl);
	free(cl);
	jw_obj_end(w);
}

static int cmd_probe(int argc, char **argv, const struct opts *o)
{
	struct ruleset rs = { 0 };
	if (load_rules(&rs, o) < 0)
		return 2;
	struct strbuf out = STRBUF_INIT;
	struct json_writer w;
	jw_init(&w, &out, true);
	if (o->json)
		jw_arr_begin(&w);
	int failures = 0;
	for (int i = 0; i < argc; i++) {
		char *rel = rel_to_root(argv[i], o->root);
		struct probe_env env = { &rs, o->uuid, "DISTROBUNY", o->fs };
		struct iso_probe p;
		iso_probe_open(&p, argv[i], rel, &env);
		struct boot_ctx ctx = { .data_fs_uuid = o->uuid ? o->uuid : "@DATA_UUID@", .data_fs_label = "DISTROBUNY",
					.data_fs_type = o->fs, .strategy = o->strategy, .variant = o->variant };
		struct boot_plan plan;
		plan_build(&p, &ctx, &plan);
		if (plan.level < COMPAT_PREPARED)
			failures++;
		if (o->json) {
			jw_obj_begin(&w);
			jw_key(&w, "file");
			jw_str(&w, argv[i]);
			jw_key(&w, "iso_path");
			jw_str(&w, rel);
			jw_key(&w, "size");
			jw_int(&w, (long long)p.file_size);
			jw_key(&w, "label");
			jw_str(&w, p.info.volume_id);
			jw_key(&w, "uuid");
			jw_str(&w, p.info.uuid);
			jw_key(&w, "name");
			jw_str(&w, p.id.name);
			jw_key(&w, "variant");
			jw_str(&w, p.id.variant);
			jw_key(&w, "version");
			jw_str(&w, p.id.version);
			jw_key(&w, "arch");
			jw_str(&w, p.id.arch);
			jw_key(&w, "kernel_version");
			jw_str(&w, p.id.kernel_version);
			jw_key(&w, "icon");
			jw_str(&w, p.id.icon);
			jw_key(&w, "recognized");
			jw_bool(&w, p.id.recognized);
			jw_key(&w, "rule");
			jw_str(&w, p.id.rule_id);
			jw_key(&w, "adapter");
			jw_str(&w, plan.adapter ? plan.adapter->id : NULL);
			jw_key(&w, "level");
			jw_str(&w, compat_level_name(plan.level));
			jw_key(&w, "strategy");
			jw_str(&w, media_strategy_name(plan.strategy));
			jw_key(&w, "reason");
			jw_str(&w, plan.reason);
			jw_key(&w, "virtual_possible");
			jw_bool(&w, plan.virtual_possible);
			jw_key(&w, "virtual_blocker");
			jw_str(&w, plan.virtual_blocker);
			jw_key(&w, "native_possible");
			jw_bool(&w, plan.native_possible);
			jw_key(&w, "native_blocker");
			jw_str(&w, plan.native_blocker);
			jw_key(&w, "error");
			jw_str(&w, p.error);
			jw_key(&w, "entry");
			if (plan.entry)
				entry_json(&w, plan.entry);
			else
				jw_null(&w);
			jw_key(&w, "variants");
			jw_arr_begin(&w);
			for (size_t k = 0; k < plan.variant_titles.n; k++)
				jw_str(&w, plan.variant_titles.v[k]);
			jw_arr_end(&w);
			jw_obj_end(&w);
		} else {
			printf("%s\n", argv[i]);
			printf("  identity   %s%s%s%s%s  [%s]\n", p.id.name, p.id.variant ? " — " : "", p.id.variant ? p.id.variant : "",
			       p.id.version ? " " : "", p.id.version ? p.id.version : "", p.id.recognized ? p.id.rule_id : "unrecognised");
			printf("  image      label=\"%s\" uuid=%s arch=%s kernel=%s\n", p.info.volume_id, p.info.uuid,
			       p.id.arch ? p.id.arch : "?", p.id.kernel_version ? p.id.kernel_version : "?");
			if (p.error)
				printf("  error      %s\n", p.error);
			printf("  adapter    %s\n", plan.adapter ? plan.adapter->name : "(none)");
			printf("  status     %s via %s\n", compat_level_name(plan.level), media_strategy_name(plan.strategy));
			printf("  reason     %s\n", plan.reason ? plan.reason : "");
			if (plan.warning)
				printf("  WARNING    %s\n", plan.warning);
			if (!plan.virtual_possible && plan.virtual_blocker)
				printf("  virtual ✗  %s\n", plan.virtual_blocker);
			if (!plan.native_possible && plan.native_blocker)
				printf("  native  ✗  %s\n", plan.native_blocker);
			if (plan.entry)
				print_entry("plan", plan.entry);
			if (o->entries) {
				for (size_t k = 0; k < p.loopback.n; k++)
					print_entry("loopback", p.loopback.v[k]);
				for (size_t k = 0; k < p.grub.n; k++)
					print_entry("grub", p.grub.v[k]);
				for (size_t k = 0; k < p.sdboot.n; k++)
					print_entry("sd-boot", p.sdboot.v[k]);
				for (size_t k = 0; k < p.syslinux.n; k++)
					print_entry("syslinux", p.syslinux.v[k]);
			}
			printf("\n");
		}
		plan_free(&plan);
		iso_probe_close(&p);
		free(rel);
	}
	if (o->json) {
		jw_arr_end(&w);
		sb_addc(&out, '\n');
		fputs(sb_str(&out), stdout);
	}
	sb_free(&out);
	rules_free(&rs);
	return failures ? 1 : 0;
}

struct probe_all_ctx {
	struct catalog *cat;
	struct probe_env *env;
	size_t done;
	bool progress;
};

static void probe_visit(struct cat_node *n, void *vctx)
{
	struct probe_all_ctx *c = vctx;
	if (n->kind != CAT_ISO || n->error || n->meta)
		return;
	catalog_probe(c->cat, n, c->env, NULL);
	c->done++;
	if (c->progress)
		fprintf(stderr, "\r  probed %zu/%zu", c->done, c->cat->total_isos);
}

static int cmd_scan(int argc, char **argv, const struct opts *o)
{
	if (argc != 1) {
		usage();
		return 2;
	}
	struct ruleset rs = { 0 };
	if (load_rules(&rs, o) < 0)
		return 2;
	struct catalog cat;
	char err[256];
	uint64_t t0 = now_us();
	if (catalog_scan(&cat, argv[0], &rs, err, sizeof(err)) < 0) {
		fprintf(stderr, "bunyctl: %s\n", err);
		rules_free(&rs);
		return 2;
	}
	uint64_t t1 = now_us();
	int restored = o->index ? catalog_load_index(&cat, o->index) : 0;
	struct probe_env env = { &rs, o->uuid, "DISTROBUNY", o->fs };
	struct probe_all_ctx pc = { &cat, &env, 0, isatty(2) && !o->json };
	catalog_walk(cat.root, probe_visit, &pc);
	if (pc.progress)
		fprintf(stderr, "\n");
	uint64_t t2 = now_us();
	fprintf(stderr, "bunyctl: %zu images, scan %.1f ms, probe %.1f ms (%zu probed, %d from index)\n", cat.total_isos,
		(t1 - t0) / 1000.0, (t2 - t1) / 1000.0, pc.done, restored > 0 ? restored : 0);
	if (o->index && catalog_save_index(&cat, o->index) < 0)
		fprintf(stderr, "bunyctl: cannot write %s: %s\n", o->index, strerror(errno));
	if (o->json) {
		struct strbuf sb = STRBUF_INIT;
		catalog_to_json(&cat, &sb, true);
		fputs(sb_str(&sb), stdout);
		sb_free(&sb);
	}
	catalog_free(&cat);
	rules_free(&rs);
	return 0;
}

static int cmd_extract(int argc, char **argv)
{
	if (argc != 3) {
		usage();
		return 2;
	}
	char err[256];
	struct iso_image *img = iso_open_path(argv[0], err, sizeof(err));
	if (!img) {
		fprintf(stderr, "bunyctl: %s: %s\n", argv[0], err);
		return 1;
	}
	struct iso_file f;
	if (iso_lookup(img, argv[1], &f) < 0 || f.is_dir) {
		fprintf(stderr, "bunyctl: %s: not found in image\n", argv[1]);
		iso_close(img);
		return 1;
	}
	int fd = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	int rc = fd >= 0 ? iso_copy_to_fd(img, &f, fd, NULL, NULL) : -1;
	if (fd >= 0)
		close(fd);
	iso_file_free(&f);
	iso_close(img);
	if (rc < 0) {
		fprintf(stderr, "bunyctl: extraction failed\n");
		return 1;
	}
	return 0;
}

static int cmd_stage(const struct opts *o)
{
	if (!o->image || !o->iso || !o->iso_path || !o->preinit || !o->out) {
		fprintf(stderr, "bunyctl stage: --image, --iso, --iso-path, --preinit and --out are required\n");
		return 2;
	}
	int fd = open(o->image, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "bunyctl: %s: %s\n", o->image, strerror(errno));
		return 1;
	}
	struct gpt_entry *parts;
	size_t np;
	if (gpt_read(fd, &parts, &np) < 0 || np == 0) {
		fprintf(stderr, "bunyctl: %s has no GPT\n", o->image);
		close(fd);
		return 1;
	}
	const struct gpt_entry *data = &parts[0];
	for (size_t i = 0; i < np; i++)
		if (!strcmp(parts[i].name, "DISTROBUNY"))
			data = &parts[i];
	uint64_t base = data->first_lba * 512;
	struct exfat_vol v;
	struct file_extent *ext = NULL;
	size_t next = 0;
	uint64_t size = 0;
	if (exfat_open(&v, fd, base) < 0 || exfat_file_extents(&v, o->iso_path, &ext, &next, &size) < 0) {
		fprintf(stderr, "bunyctl: cannot map %s in the data partition: %s\n", o->iso_path, strerror(errno));
		close(fd);
		free(parts);
		return 1;
	}
	char uuid[16];
	snprintf(uuid, sizeof(uuid), "%04X-%04X", v.serial >> 16, v.serial & 0xFFFF);
	printf("data partition: start %llu, %llu sectors, exFAT serial %s, cluster %llu bytes\n",
	       (unsigned long long)data->first_lba, (unsigned long long)(data->last_lba - data->first_lba + 1), uuid,
	       (unsigned long long)v.cluster_size);
	for (size_t i = 0; i < next; i++)
		printf("extent %zu: offset %llu length %llu\n", i, (unsigned long long)ext[i].offset, (unsigned long long)ext[i].length);

	struct ruleset rs = { 0 };
	if (load_rules(&rs, o) < 0)
		return 2;
	struct probe_env env = { &rs, uuid, "DISTROBUNY", "exfat" };
	struct iso_probe p;
	iso_probe_open(&p, o->iso, o->iso_path, &env);
	struct boot_ctx ctx = { .data_fs_uuid = uuid, .data_fs_label = "DISTROBUNY", .data_fs_type = "exfat",
				.strategy = o->strategy, .variant = o->variant, .serial_console = o->serial_console,
				.iso_fragmented = next > 1 };
	struct boot_plan plan;
	int rc = 1;
	if (plan_build(&p, &ctx, &plan) < 0) {
		fprintf(stderr, "bunyctl: no boot plan: %s\n", plan.reason ? plan.reason : "unknown");
		goto out;
	}
	char err[512];
	struct handoff_payload pay;
	if (handoff_prepare(p.iso, &plan, &pay, NULL, NULL, err, sizeof(err)) < 0) {
		fprintf(stderr, "bunyctl: %s\n", err);
		goto out_plan;
	}
	char *cmdline = xstrdup(pay.cmdline);
	if (pay.needs_preinit) {
		struct handoff_medium m = {
			.part_dev = o->image, .part_base = base, .part_start = data->first_lba,
			.part_size = data->last_lba - data->first_lba + 1, .fstype = "exfat", .vol_serial = v.serial,
			.have_serial = true, .ext = ext, .next = next, .iso_size = size, .iso_label = p.info.volume_id,
			.iso_file = o->iso, .verbose = o->serial_console,
		};
		free(cmdline);
		if (handoff_attach_medium(&pay, &m, o->preinit, &cmdline, err, sizeof(err)) < 0) {
			fprintf(stderr, "bunyctl: %s\n", err);
			handoff_payload_close(&pay);
			goto out_plan;
		}
		printf("virtual medium: verified (label \"%s\", %zu extent(s))\n", p.info.volume_id, next);
	}
	if (handoff_dump(&pay, cmdline, o->out) < 0) {
		fprintf(stderr, "bunyctl: cannot write to %s: %s\n", o->out, strerror(errno));
	} else {
		printf("adapter: %s, strategy: %s\ncmdline: %s\n", plan.adapter->id, media_strategy_name(plan.strategy), cmdline);
		rc = 0;
	}
	free(cmdline);
	handoff_payload_close(&pay);
out_plan:
	plan_free(&plan);
out:
	iso_probe_close(&p);
	rules_free(&rs);
	free(ext);
	free(parts);
	close(fd);
	return rc;
}

static int cmd_map(const struct opts *o)
{
	if (!o->image || !o->iso_path) {
		fprintf(stderr, "bunyctl map: --image and --iso-path are required\n");
		return 2;
	}
	int fd = open(o->image, O_RDONLY | O_CLOEXEC);
	struct gpt_entry *parts = NULL;
	size_t np = 0;
	if (fd < 0 || gpt_read(fd, &parts, &np) < 0 || !np) {
		fprintf(stderr, "bunyctl: %s: no GPT\n", o->image);
		return 1;
	}
	const struct gpt_entry *data = &parts[0];
	for (size_t i = 0; i < np; i++)
		if (!strcmp(parts[i].name, "DISTROBUNY"))
			data = &parts[i];
	struct exfat_vol v;
	struct file_extent *ext = NULL;
	size_t next = 0;
	uint64_t size = 0;
	int rc = 0;
	if (exfat_open(&v, fd, data->first_lba * 512) < 0 || exfat_file_extents(&v, o->iso_path, &ext, &next, &size) < 0) {
		fprintf(stderr, "bunyctl: %s: %s\n", o->iso_path, strerror(errno));
		rc = 1;
	} else {
		printf("size %llu\n", (unsigned long long)size);
		for (size_t i = 0; i < next; i++)
			printf("extent %llu %llu\n", (unsigned long long)ext[i].offset, (unsigned long long)ext[i].length);
	}
	free(ext);
	free(parts);
	close(fd);
	return rc;
}

/* Grow DISTROBUNY in a disk image file (enlarge the file first, as a bigger
 * stick would be).  Block devices are refused: on a real drive this is the
 * runtime's job, with the user's confirmation. */
static int cmd_expand(const struct opts *o)
{
	if (!o->image) {
		fprintf(stderr, "bunyctl expand: --image is required\n");
		return 2;
	}
	int fd = open(o->image, (o->check ? O_RDONLY : O_RDWR) | O_CLOEXEC);
	struct stat st;
	if (fd < 0 || fstat(fd, &st) < 0) {
		fprintf(stderr, "bunyctl: %s: %s\n", o->image, strerror(errno));
		return 1;
	}
	if (!S_ISREG(st.st_mode)) {
		fprintf(stderr, "bunyctl: %s is not a disk image file; drives are expanded from the Distro Buny menu\n", o->image);
		close(fd);
		return 2;
	}
	struct expand_plan p;
	if (expand_plan(fd, (uint64_t)st.st_size, 512, &p) < 0) {
		fprintf(stderr, "bunyctl: %s: %s\n", o->image, strerror(errno));
		close(fd);
		return 1;
	}
	printf("state %s code=%s data=%llu new=%llu clusters=%u new_clusters=%u bitmap=%u+%u limited=%d\n",
	       expand_state_name(p.state), p.code, (unsigned long long)p.data_bytes, (unsigned long long)p.new_data_bytes,
	       p.clusters, p.new_clusters, p.bitmap_first, p.new_bitmap_clusters - p.bitmap_clusters, p.fs_limited);
	if (p.state == EXPAND_REFUSED)
		printf("reason %s\n", p.reason);
	int rc = 0;
	if (!o->check && (p.state == EXPAND_POSSIBLE || p.state == EXPAND_RESUME)) {
		p.stop_after_writes = o->stop_after;
		char err[512];
		if (expand_run(fd, &p, err, sizeof(err)) < 0) {
			fprintf(stderr, "bunyctl: expanding %s: %s\n", o->image, err);
			rc = errno == EINTR ? 3 : 1;
		} else {
			printf("expanded data=%llu writes=%u\n", (unsigned long long)p.data_bytes, p.writes);
		}
	}
	expand_plan_free(&p);
	close(fd);
	return rc;
}

static int cmd_adapters(void)
{
	size_t n;
	const struct adapter *const *all = adapters_all(&n);
	for (size_t i = 0; i < n; i++)
		printf("%-18s %s\n%18s %s\n", all[i]->id, all[i]->name, "", all[i]->mechanism);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		usage();
		return 2;
	}
	const char *cmd = argv[1];
	struct opts o = { .variant = -1 };
	static const struct option lopts[] = {
		{ "rules", required_argument, 0, 'r' }, { "json", no_argument, 0, 'j' },
		{ "root", required_argument, 0, 'R' },  { "uuid", required_argument, 0, 'u' },
		{ "fs", required_argument, 0, 'f' },    { "strategy", required_argument, 0, 's' },
		{ "entries", no_argument, 0, 'e' },     { "index", required_argument, 0, 'i' },
		{ "variant", required_argument, 0, 'v' }, { "verbose", no_argument, 0, 'V' },
		{ "image", required_argument, 0, 'I' }, { "iso", required_argument, 0, 'S' },
		{ "iso-path", required_argument, 0, 'P' }, { "preinit", required_argument, 0, 'N' },
		{ "out", required_argument, 0, 'o' }, { "serial-console", no_argument, 0, 'C' },
		{ "check", no_argument, 0, 'k' }, { "stop-after", required_argument, 0, 'x' },
		{ 0, 0, 0, 0 },
	};
	optind = 2;
	int c;
	log_set_tag("bunyctl");
	log_set_level(LOG_LVL_WARN);
	while ((c = getopt_long(argc, argv, "", lopts, NULL)) != -1) {
		switch (c) {
		case 'r': o.rules_path = optarg; break;
		case 'j': o.json = true; break;
		case 'R': o.root = optarg; break;
		case 'u': o.uuid = optarg; break;
		case 'f': o.fs = optarg; break;
		case 'e': o.entries = true; break;
		case 'i': o.index = optarg; break;
		case 'v': o.variant = atoi(optarg); break;
		case 'V': log_set_level(LOG_LVL_DEBUG); break;
		case 'I': o.image = optarg; break;
		case 'S': o.iso = optarg; break;
		case 'P': o.iso_path = optarg; break;
		case 'N': o.preinit = optarg; break;
		case 'o': o.out = optarg; break;
		case 'C': o.serial_console = true; break;
		case 'k': o.check = true; break;
		case 'x': o.stop_after = (unsigned)strtoul(optarg, NULL, 10); break;
		case 's':
			o.strategy = !strcmp(optarg, "virtual") ? MEDIA_VIRTUAL : !strcmp(optarg, "native") ? MEDIA_NATIVE : MEDIA_AUTO;
			break;
		default: usage(); return 2;
		}
	}
	int rest = argc - optind;
	char **rargv = argv + optind;
	if (!strcmp(cmd, "probe"))
		return cmd_probe(rest, rargv, &o);
	if (!strcmp(cmd, "scan"))
		return cmd_scan(rest, rargv, &o);
	if (!strcmp(cmd, "extract"))
		return cmd_extract(rest, rargv);
	if (!strcmp(cmd, "adapters"))
		return cmd_adapters();
	if (!strcmp(cmd, "stage"))
		return cmd_stage(&o);
	if (!strcmp(cmd, "map"))
		return cmd_map(&o);
	if (!strcmp(cmd, "expand"))
		return cmd_expand(&o);
	if (!strcmp(cmd, "version")) {
		printf("bunyctl %s\n", BUNY_VERSION);
		return 0;
	}
	usage();
	return 2;
}
