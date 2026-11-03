/*
 * Distro Buny — interface application logic
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "interface/app.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "boot/handoff.h"
#include "boot/ipc.h"
#include "common/log.h"
#include "common/strbuf.h"

#ifndef BUNY_VERSION
#define BUNY_VERSION "dev"
#endif

struct probe_result {
	struct cat_node *node;
	struct cat_meta *meta;
	struct probe_result *next;
};

static atomic_bool g_boot_cancel;

/* ------------------------------------------------------------- utilities */

void app_emit(struct app *a, const char *fmt, ...)
{
	if (!a->lab)
		return;
	va_list ap;
	va_start(ap, fmt);
	char *m = xvasprintf(fmt, ap);
	va_end(ap);
	fprintf(stderr, "BUNY-EVENT %s\n", m);
	fflush(stderr);
	free(m);
}

static void toast(struct app *a, const char *msg)
{
	snprintf(a->toast, sizeof(a->toast), "%s", msg);
	a->toast_until = a->now + 2600;
	a->dirty = true;
}

static void wake(struct app *a)
{
	uint64_t one = 1;
	if (a->wake_fd >= 0 && write(a->wake_fd, &one, sizeof(one)) < 0) {
		/* the main loop polls with a timeout anyway */
	}
}

static void compute_scale(struct app *a)
{
	float s = (float)a->H / 1000.0f;
	s = fminf(s, (float)a->W / 1500.0f);
	s = BUNY_CLAMP(s, 0.62f, 3.0f);
	if (a->set.scale_pct)
		s *= (float)a->set.scale_pct / 100.0f;
	a->s = s;
	input_set_screen(a->in, a->W, a->H, fmaxf(1.0f, s));
}

bool secure_boot_on(struct app *a)
{
	return !strcmp(json_get_str(a->runtime, "secure_boot", ""), "enabled");
}

/* Will this runtime's kernel accept the system's kernel for kexec under Secure
 * Boot?  Unsigned: no.  Signed: when the certificate authority that issued the
 * signing certificate (or the signer itself) is one of the keys the running
 * kernel trusts (runtime.json "trusted_keys", read from /proc/keys by init).
 * Without that list the answer is "yes": a refusal is still explained when the
 * system is started. */
bool sb_kernel_trusted(struct app *a, const struct cat_meta *m)
{
	if (!m->kernel_signed)
		return false;
	const struct json *keys = json_get(a->runtime, "trusted_keys");
	if (!keys || keys->type != JSON_ARRAY || keys->n == 0 || !m->kernel_signer)
		return true;
	char signer[256], issuer[256] = "";
	const char *p = strstr(m->kernel_signer, " (issued by ");
	if (p) {
		snprintf(signer, sizeof(signer), "%.*s", (int)(p - m->kernel_signer), m->kernel_signer);
		snprintf(issuer, sizeof(issuer), "%s", p + strlen(" (issued by "));
		size_t l = strlen(issuer);
		if (l && issuer[l - 1] == ')')
			issuer[l - 1] = '\0';
	} else {
		snprintf(signer, sizeof(signer), "%s", m->kernel_signer);
	}
	for (size_t i = 0; i < keys->n; i++) {
		const char *k = keys->items[i]->type == JSON_STRING ? keys->items[i]->str : "";
		if ((issuer[0] && strcasestr(k, issuer)) || (signer[0] && strcasestr(k, signer)))
			return true;
	}
	return false;
}

static void apply_theme(struct app *a)
{
	if (a->set.theme)
		theme_dark(&a->th);
	else
		theme_light(&a->th);
	canvas_free(a->bg);
	a->bg = NULL;
}

/* -------------------------------------------------------------- settings */

static char *esp_state_path(struct app *a, const char *name)
{
	return xasprintf("%s/distrobuny/state/%s", a->esp_dir, name);
}

static void settings_defaults(struct settings *s)
{
	*s = (struct settings){ .theme = 0, .animations = true, .scale_pct = 0, .show_unsupported = true,
				.remember_last = true, .strategy = MEDIA_AUTO, .show_details = false };
}

static void settings_load(struct app *a)
{
	settings_defaults(&a->set);
	if (!a->esp_dir[0])
		return;
	char *p = esp_state_path(a, "settings.json");
	size_t len;
	char *text = read_file(p, 65536, &len);
	free(p);
	if (!text)
		return;
	char err[128];
	struct json *j = json_parse(text, len, err, sizeof(err));
	free(text);
	if (!j) {
		LOGW("settings.json is invalid (%s); using defaults", err);
		return;
	}
	a->set.theme = !strcmp(json_get_str(j, "theme", "light"), "dark");
	a->set.animations = json_get_bool(j, "animations", true);
	int sc = (int)json_get_num(j, "scale", 0);
	a->set.scale_pct = (sc == 100 || sc == 125 || sc == 150 || sc == 200) ? sc : 0;
	a->set.show_unsupported = json_get_bool(j, "show_unsupported", true);
	a->set.remember_last = json_get_bool(j, "remember_last", true);
	const char *st = json_get_str(j, "strategy", "auto");
	a->set.strategy = !strcmp(st, "virtual") ? MEDIA_VIRTUAL : !strcmp(st, "native") ? MEDIA_NATIVE : MEDIA_AUTO;
	a->set.show_details = json_get_bool(j, "show_details", false);
	json_free(j);
}

static int ipc_request(struct app *a, struct ipc_msg *m, const int *fds, int nfds, struct ipc_msg *reply)
{
	if (a->ipc_fd < 0)
		return -1;
	if (ipc_send(a->ipc_fd, m, fds, nfds) < 0)
		return -1;
	int rf[BUNY_IPC_MAX_FDS], nrf = 0;
	if (ipc_recv(a->ipc_fd, reply, rf, &nrf) < 0)
		return -1;
	for (int i = 0; i < nrf; i++)
		close(rf[i]);
	return reply->type == IPC_REPLY ? 0 : -1;
}

static void save_state_file(struct app *a, const char *name, const char *content)
{
	if (a->ipc_fd < 0)
		return;
	struct ipc_msg m, r;
	memset(&m, 0, sizeof(m));
	m.type = IPC_SAVE;
	snprintf(m.path, sizeof(m.path), "%s", name);
	snprintf(m.text, sizeof(m.text), "%s", content);
	if (ipc_request(a, &m, NULL, 0, &r) < 0 || r.status != 0)
		LOGW("saving %s failed: %s", name, r.text);
}

static void settings_save(struct app *a)
{
	struct strbuf sb = STRBUF_INIT;
	struct json_writer w;
	jw_init(&w, &sb, true);
	jw_obj_begin(&w);
	jw_key(&w, "theme");
	jw_str(&w, a->set.theme ? "dark" : "light");
	jw_key(&w, "animations");
	jw_bool(&w, a->set.animations);
	jw_key(&w, "scale");
	jw_int(&w, a->set.scale_pct);
	jw_key(&w, "show_unsupported");
	jw_bool(&w, a->set.show_unsupported);
	jw_key(&w, "remember_last");
	jw_bool(&w, a->set.remember_last);
	jw_key(&w, "strategy");
	jw_str(&w, a->set.strategy == MEDIA_VIRTUAL ? "virtual" : a->set.strategy == MEDIA_NATIVE ? "native" : "auto");
	jw_key(&w, "show_details");
	jw_bool(&w, a->set.show_details);
	jw_obj_end(&w);
	sb_addc(&sb, '\n');
	save_state_file(a, "settings.json", sb_str(&sb));
	sb_free(&sb);
}

static void last_boot_load(struct app *a)
{
	if (!a->esp_dir[0])
		return;
	char *p = esp_state_path(a, "last-boot.json");
	size_t len;
	char *text = read_file(p, 65536, &len);
	free(p);
	if (!text)
		return;
	struct json *j = json_parse(text, len, NULL, 0);
	free(text);
	if (!j)
		return;
	const char *path = json_get_str(j, "path", NULL);
	const char *title = json_get_str(j, "title", NULL);
	if (path && title && a->have_catalog && catalog_find(&a->cat, path)) {
		snprintf(a->last_boot_path, sizeof(a->last_boot_path), "%s", path);
		snprintf(a->last_boot_title, sizeof(a->last_boot_title), "%s", title);
	}
	json_free(j);
}

/* ------------------------------------------------------------ navigation */

static void snapshot(struct app *a, int dir)
{
	if (!a->set.animations)
		return;
	if (!a->prev)
		a->prev = canvas_new(a->W, a->H);
	gfx_copy(a->prev, 0, 0, a->frame, (struct rect){ 0, 0, a->W, a->H });
	a->transitioning = true;
	a->trans_start = a->now;
	a->trans_dir = dir;
}

static void emit_hint(struct app *a)
{
	const char *hint = empty_hint(a, &a->stack[a->depth - 1]);
	if (hint)
		app_emit(a, "ui hint=\"%s\"", hint);
}

static void emit_screen(struct app *a)
{
	static const char *names[] = { "home", "folder", "detail", "boot", "settings", "info", "search", "log", "nodata" };
	struct screen *sc = &a->stack[a->depth - 1];
	app_emit(a, "ui screen=%s path=\"%s\"", names[sc->kind], sc->node ? sc->node->rel_path : "");
	emit_hint(a);
}

static void push(struct app *a, enum screen_kind k, struct cat_node *n)
{
	if (a->depth >= MAX_DEPTH)
		return;
	snapshot(a, 1);
	struct screen *sc = &a->stack[a->depth++];
	memset(sc, 0, sizeof(*sc));
	sc->kind = k;
	sc->node = n;
	sc->focus = -1;
	a->dirty = true;
	emit_screen(a);
}

static void pop(struct app *a)
{
	if (a->depth <= 1)
		return;
	snapshot(a, -1);
	a->depth--;
	a->dirty = true;
	emit_screen(a);
}

static void open_node(struct app *a, struct cat_node *n)
{
	if (!n)
		return;
	if (n->kind == CAT_FOLDER)
		push(a, SCR_FOLDER, n);
	else
		push(a, SCR_DETAIL, n);
}

/* Navigate to an arbitrary node from Home (used by search, last boot and autoboot). */
static void go_to(struct app *a, struct cat_node *n)
{
	struct cat_node *chain[MAX_DEPTH];
	int k = 0;
	for (struct cat_node *x = n; x && x->parent && k < MAX_DEPTH - 2; x = x->parent)
		chain[k++] = x;
	snapshot(a, 1);
	a->depth = 1;
	for (int i = k - 1; i >= 0; i--) {
		struct screen *sc = &a->stack[a->depth++];
		memset(sc, 0, sizeof(*sc));
		sc->kind = chain[i]->kind == CAT_FOLDER ? SCR_FOLDER : SCR_DETAIL;
		sc->node = chain[i];
		sc->focus = -1;
		if (i > 0)
			sc->focus_key = chain[i - 1];
	}
	a->dirty = true;
	emit_screen(a);
}

static void open_dialog(struct app *a, enum dialog_kind k, const char *title, const char *msg, bool retry)
{
	memset(&a->dlg, 0, sizeof(a->dlg));
	a->dlg.kind = k;
	snprintf(a->dlg.title, sizeof(a->dlg.title), "%s", title);
	snprintf(a->dlg.message, sizeof(a->dlg.message), "%s", msg ? msg : "");
	a->dlg.retry = retry;
	a->dlg.opened_ms = a->now;
	a->dlg.focus = 0;
	a->dirty = true;
	app_emit(a, "ui dialog=%d title=\"%s\"", k, title);
}

/* --------------------------------------------------------------- probing */

struct probe_task {
	struct app *a;
	struct cat_node **nodes;
	char **paths;
	size_t n;
	struct probe_env env;
};

static void *probe_thread(void *arg)
{
	struct probe_task *t = arg;
	for (size_t i = 0; i < t->n && !atomic_load(&t->a->quit) && !atomic_load(&t->a->probe_stop); i++) {
		struct cat_meta *m = catalog_probe_meta(t->a->data_dir, t->paths[i], &t->env, NULL);
		struct probe_result *r = xcalloc(1, sizeof(*r));
		r->node = t->nodes[i];
		r->meta = m;
		pthread_mutex_lock(&t->a->lock);
		r->next = t->a->results;
		t->a->results = r;
		pthread_mutex_unlock(&t->a->lock);
		wake(t->a);
	}
	for (size_t i = 0; i < t->n; i++)
		free(t->paths[i]);
	free(t->paths);
	free(t->nodes);
	free(t);
	return NULL;
}

static void collect_unprobed(struct cat_node *n, struct ptrvec *out)
{
	if (n->kind == CAT_ISO) {
		if (!n->meta && !n->error)
			ptrvec_push(out, n);
		return;
	}
	for (size_t i = 0; i < n->nchildren; i++)
		collect_unprobed(n->children[i], out);
}

static void start_probing(struct app *a)
{
	struct ptrvec v = { 0 };
	collect_unprobed(a->cat.root, &v);
	if (!v.n) {
		free(v.v);
		return;
	}
	struct probe_task *t = xcalloc(1, sizeof(*t));
	t->a = a;
	t->n = v.n;
	t->nodes = (struct cat_node **)v.v;
	t->paths = xcalloc(v.n, sizeof(char *));
	for (size_t i = 0; i < v.n; i++)
		t->paths[i] = xstrdup(t->nodes[i]->rel_path);
	t->env = (struct probe_env){ &a->rules, a->data_uuid[0] ? a->data_uuid : NULL, a->data_label, a->data_fs[0] ? a->data_fs : NULL };
	LOGI("probing %zu image(s) not covered by the index", v.n);
	if (pthread_create(&a->probe_thread, NULL, probe_thread, t) == 0)
		a->probe_running = true;
}

static void apply_results(struct app *a)
{
	pthread_mutex_lock(&a->lock);
	struct probe_result *r = a->results;
	a->results = NULL;
	pthread_mutex_unlock(&a->lock);
	while (r) {
		struct probe_result *next = r->next;
		catalog_set_meta(r->node, r->meta);
		a->probed++;
		free(r);
		r = next;
		a->dirty = true;
	}
}

/* ------------------------------------------------------------------ boot */

static void boot_progress(void *ctx, enum handoff_stage stage, uint64_t done, uint64_t total)
{
	struct app *a = ctx;
	atomic_store(&a->boot.stage, stage);
	atomic_store(&a->boot.done, done);
	atomic_store(&a->boot.total, total);
	static uint64_t last;
	uint64_t n = now_ms();
	if (n - last > 33) {
		last = n;
		wake(a);
	}
}

static void boot_fail(struct app *a, const char *fmt, ...) BUNY_PRINTF(2, 3);
static void boot_fail(struct app *a, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(a->boot.error, sizeof(a->boot.error), fmt, ap);
	va_end(ap);
	atomic_store(&a->boot.phase, BP_FAILED);
	wake(a);
}

static void *boot_thread(void *arg)
{
	struct app *a = arg;
	struct boot_job *b = &a->boot;
	char *host = path_join(a->data_dir, b->rel_path);
	struct probe_env env = { &a->rules, a->data_uuid[0] ? a->data_uuid : NULL, a->data_label, a->data_fs[0] ? a->data_fs : NULL };
	struct iso_probe p;
	iso_probe_open(&p, host, b->rel_path, &env);
	free(host);
	struct boot_ctx ctx = { .data_fs_uuid = env.data_fs_uuid, .data_fs_label = env.data_fs_label, .data_fs_type = env.data_fs_type,
				.strategy = b->strategy, .variant = b->variant, .safe_graphics = b->safe_graphics, .serial_console = a->lab };
	struct boot_plan plan;
	if (plan_build(&p, &ctx, &plan) < 0) {
		boot_fail(a, "%s", plan.reason ? plan.reason : "This image cannot be started.");
		goto out;
	}
	snprintf(b->adapter, sizeof(b->adapter), "%s", plan.adapter->id);
	atomic_store(&b->phase, BP_LOADING);
	wake(a);
	char err[1024];
	struct handoff_payload pay;
	if (handoff_prepare(p.iso, &plan, &pay, boot_progress, a, err, sizeof(err)) < 0) {
		boot_fail(a, "%s", err);
		goto out_plan;
	}
	snprintf(b->cmdline, sizeof(b->cmdline), "%s", pay.cmdline);
	if (atomic_load(&g_boot_cancel)) {
		handoff_payload_close(&pay);
		atomic_store(&b->phase, BP_IDLE);
		goto out_plan;
	}
	if (a->ipc_fd >= 0) {
		struct ipc_msg m, r;
		memset(&m, 0, sizeof(m));
		memset(&r, 0, sizeof(r));
		m.type = IPC_BOOT;
		m.arg = pay.needs_preinit ? IPC_BOOT_NEEDS_MEDIUM : 0;
		snprintf(m.path, sizeof(m.path), "%s", b->rel_path);
		snprintf(m.label, sizeof(m.label), "%s", p.info.volume_id);
		snprintf(m.text, sizeof(m.text), "%s", pay.cmdline);
		atomic_store(&b->stage, HS_MEDIUM);
		int fds[2] = { pay.kernel_fd, pay.initrd_fd };
		int rc = ipc_request(a, &m, fds, 2, &r);
		handoff_payload_close(&pay);
		if (rc < 0) {
			boot_fail(a, "The Distro Buny runtime did not answer the start request.");
			goto out_plan;
		}
		if (r.status != 0) {
			b->error_code = r.status;
			boot_fail(a, "%s", r.text);
			goto out_plan;
		}
		b->dry_run_only = strstr(r.text, "dry run") != NULL;
	} else {
		handoff_payload_close(&pay);
		b->dry_run_only = true;
	}
	atomic_store(&b->phase, BP_READY);
	wake(a);
out_plan:
	plan_free(&plan);
out:
	iso_probe_close(&p);
	return NULL;
}

static void start_boot(struct app *a, struct cat_node *n);

/* Start an image from the menu: images that do something destructive (see
 * boot_plan.warning) need an explicit confirmation first. */
bool is_best_effort(const struct cat_meta *m)
{
	return m && m->adapter && !strcmp(m->adapter, ADAPTER_BEST_EFFORT);
}

static void request_boot(struct app *a, struct cat_node *n)
{
	if (n && n->meta && n->meta->warning) {
		open_dialog(a, DLG_CONFIRM, "This image erases a disk", n->meta->warning, false);
		return;
	}
	if (n && is_best_effort(n->meta)) {
		open_dialog(a, DLG_CONFIRM, "Distro Buny does not know this system",
			    "Distro Buny has not seen this kind of system before. It starts the image's own boot entry with a "
			    "virtual disc that carries the image's name and ID, which is how most systems find their files. If "
			    "the system stops with an error or a text prompt, restart the computer: nothing has been changed. "
			    "If it is an installer, check the target disk carefully before you confirm an installation.", false);
		return;
	}
	start_boot(a, n);
}

static void start_boot(struct app *a, struct cat_node *n)
{
	if (a->boot.running)
		pthread_join(a->boot.thread, NULL);
	struct boot_job *b = &a->boot;
	b->running = false;
	atomic_store(&g_boot_cancel, false);
	atomic_store(&b->phase, BP_PREPARING);
	atomic_store(&b->done, 0);
	atomic_store(&b->total, 0);
	b->error[0] = '\0';
	b->error_code = 0;
	snprintf(b->rel_path, sizeof(b->rel_path), "%s", n->rel_path);
	struct cat_meta *m = n->meta;
	if (m && m->recognized)
		snprintf(b->title, sizeof(b->title), "%s%s%s", m->name, m->version ? " " : "", m->version ? m->version : "");
	else
		snprintf(b->title, sizeof(b->title), "%s", iso_title(n));
	if (b->strategy == MEDIA_AUTO)
		b->strategy = (enum media_strategy)a->set.strategy;
	push(a, SCR_BOOT, n);
	app_emit(a, "boot-start iso=\"%s\" variant=%d strategy=%d safe=%d", n->rel_path, b->variant, b->strategy, b->safe_graphics);
	if (pthread_create(&b->thread, NULL, boot_thread, a) == 0)
		b->running = true;
	else
		boot_fail(a, "Could not start the preparation thread.");
}

static void reset_boot_options(struct app *a)
{
	a->boot.variant = -1;
	a->boot.safe_graphics = false;
	a->boot.strategy = MEDIA_AUTO;
}

static void boot_poll(struct app *a)
{
	struct boot_job *b = &a->boot;
	int phase = atomic_load(&b->phase);
	struct screen *sc = &a->stack[a->depth - 1];
	if (phase == BP_FAILED && sc->kind == SCR_BOOT) {
		app_emit(a, "boot-error code=%d reason=\"%s\"", b->error_code, b->error);
		atomic_store(&b->phase, BP_DONE);
		pop(a);
		int e = b->error_code;
		bool sb = e == ENODATA || e == EKEYREJECTED || e == EBADMSG || e == ENOKEY || e == EKEYREVOKED || e == EKEYEXPIRED ||
			  (e == EPERM && secure_boot_on(a));
		if (sb) {
			char msg[1400];
			snprintf(msg, sizeof(msg),
				 "%s.\n\nWith Secure Boot enabled, this computer only starts kernels signed by a key it trusts. "
				 "Systems signed by Canonical (Ubuntu and its flavours, Linux Mint, Zorin OS) start normally. "
				 "To start this system, turn Secure Boot off in the firmware settings, or enrol its vendor's key with the MOK manager "
				 "(see the user guide).", b->error);
			open_dialog(a, DLG_ERROR, "Secure Boot blocked this system", msg, false);
			a->dlg.firmware = json_get_bool(a->runtime, "firmware_setup_supported", false);
		} else {
			open_dialog(a, DLG_ERROR, "This system could not be started", b->error, true);
		}
	} else if (phase == BP_IDLE && sc->kind == SCR_BOOT) {
		pop(a);
	} else if (phase == BP_READY && sc->kind == SCR_BOOT) {
		if (!b->ready_ms) {
			b->ready_ms = a->now;
			app_emit(a, "boot-ready adapter=%s dry_run=%d cmdline=\"%s\"", b->adapter, b->dry_run_only, b->cmdline);
			if (a->set.remember_last && !b->dry_run_only) {
				char *content = xasprintf("{\"path\": \"%s\", \"title\": \"%s\"}\n", b->rel_path, b->title);
				save_state_file(a, "last-boot.json", content);
				free(content);
			}
		} else if (a->now - b->ready_ms > 700) {
			b->ready_ms = 0;
			if (b->dry_run_only) {
				atomic_store(&b->phase, BP_DONE);
				pop(a);
				toast(a, "Validated: everything is ready, but this environment does not start systems.");
				return;
			}
			atomic_store(&b->phase, BP_HANDOFF);
			app_emit(a, "handoff");
			struct ipc_msg m, r;
			memset(&m, 0, sizeof(m));
			m.type = IPC_EXECUTE;
			/* On success this never returns: the machine switches kernels. */
			if (ipc_request(a, &m, NULL, 0, &r) < 0 || r.status != 0) {
				snprintf(b->error, sizeof(b->error), "%.1000s", r.text[0] ? r.text : "The hand-off failed.");
				atomic_store(&b->phase, BP_FAILED);
			}
		}
	}
}

/* ------------------------------------------------------- drive expansion */

static void load_json_file(const char *path, struct json **out);

/* "possible" or "refused" when Home should mention the drive's unused space
 * (runtime.json "expand", written by init), otherwise NULL. */
const char *expand_offer(struct app *a)
{
	const struct json *x = json_get(a->runtime, "expand");
	const char *st = x ? json_get_str(x, "state", NULL) : NULL;
	if (!st || !a->have_catalog)
		return NULL;
	if (!strcmp(st, "possible") || !strcmp(st, "resume"))
		return "possible";
	if (!strcmp(st, "refused"))
		return "refused";
	return NULL;
}

static void expand_sizes(struct app *a, char *data, char *grown, char *disk, size_t len)
{
	const struct json *x = json_get(a->runtime, "expand");
	format_size((uint64_t)json_get_num(x, "data_bytes", 0), data, len);
	format_size((uint64_t)json_get_num(x, "new_data_bytes", 0), grown, len);
	format_size((uint64_t)json_get_num(x, "disk_bytes", 0), disk, len);
}

/* Home's banner: explain, and ask before anything is written. */
static void expand_ask(struct app *a)
{
	const char *offer = expand_offer(a);
	if (!offer)
		return;
	const struct json *x = json_get(a->runtime, "expand");
	if (!strcmp(offer, "refused")) {
		open_dialog(a, DLG_INFO, "DISTROBUNY cannot grow on this drive", json_get_str(x, "reason", ""), false);
		return;
	}
	char data[32], grown[32], disk[32], msg[1024];
	expand_sizes(a, data, grown, disk, sizeof(data));
	if (json_get_bool(x, "limited", false))
		snprintf(msg, sizeof(msg), "DISTROBUNY grows from %s to %s, the most its file system allows on this %s drive. ", data, grown, disk);
	else
		snprintf(msg, sizeof(msg), "DISTROBUNY grows from %s to %s, so the whole %s drive can hold ISO images. ", data, grown, disk);
	strncat(msg, "The files on it stay where they are; nothing is erased.\n\nThis takes a few seconds. Do not unplug the drive "
		     "or switch off the computer until it is done.", sizeof(msg) - strlen(msg) - 1);
	open_dialog(a, DLG_EXPAND, "Use the whole drive?", msg, false);
}

static void *expand_thread(void *arg)
{
	struct app *a = arg;
	struct ipc_msg m, r;
	memset(&m, 0, sizeof(m));
	memset(&r, 0, sizeof(r));
	m.type = IPC_EXPAND;
	if (ipc_request(a, &m, NULL, 0, &r) < 0) {
		a->expand.status = EIO;
		snprintf(a->expand.text, sizeof(a->expand.text), "%s", "The Distro Buny runtime did not answer.");
	} else {
		a->expand.status = r.status;
		snprintf(a->expand.text, sizeof(a->expand.text), "%.1000s", r.text);
	}
	atomic_store(&a->expand.phase, EX_FINISHED);
	wake(a);
	return NULL;
}

static void start_expand(struct app *a)
{
	app_emit(a, "expand-request");
	if (a->ipc_fd < 0) {
		toast(a, "Expanding the drive is not available in this environment.");
		return;
	}
	/* the data partition is unmounted while it grows: stop reading images from it */
	atomic_store(&a->probe_stop, true);
	if (a->probe_running) {
		pthread_join(a->probe_thread, NULL);
		a->probe_running = false;
	}
	atomic_store(&a->probe_stop, false);
	apply_results(a);
	open_dialog(a, DLG_BUSY, "Expanding DISTROBUNY\xE2\x80\xA6",
		    "Do not unplug the drive or switch off the computer. This takes a few seconds.", false);
	atomic_store(&a->expand.phase, EX_RUNNING);
	if (pthread_create(&a->expand.thread, NULL, expand_thread, a) == 0) {
		a->expand.running = true;
	} else {
		atomic_store(&a->expand.phase, EX_IDLE);
		open_dialog(a, DLG_ERROR, "The drive could not be expanded", "The operation could not be started.", false);
	}
}

static void expand_poll(struct app *a)
{
	if (atomic_load(&a->expand.phase) != EX_FINISHED)
		return;
	pthread_join(a->expand.thread, NULL);
	a->expand.running = false;
	atomic_store(&a->expand.phase, EX_IDLE);
	app_emit(a, "expand-result status=%d text=\"%s\"", a->expand.status, a->expand.text);
	/* init rewrote runtime.json with the new size */
	const char *rt = getenv("BUNY_RUNTIME");
	if (rt) {
		json_free(a->runtime);
		a->runtime = NULL;
		load_json_file(rt, &a->runtime);
	}
	if (a->expand.status == 0) {
		char size[32], msg[512];
		const struct json *d = json_get(a->runtime, "data");
		format_size((uint64_t)json_get_num(d, "size", 0), size, sizeof(size));
		snprintf(msg, sizeof(msg), "DISTROBUNY now holds %s. Add ISO images to it from any computer, as before.", size);
		open_dialog(a, DLG_INFO, "The drive now uses all of its space", msg, false);
	} else {
		open_dialog(a, DLG_ERROR, "The drive could not be expanded", a->expand.text, false);
	}
	start_probing(a);
}

/* ------------------------------------------------------------ actions */

static void cycle_setting(struct app *a, int idx, int dir)
{
	struct settings *s = &a->set;
	switch (idx) {
	case 0:
		s->theme = !s->theme;
		apply_theme(a);
		break;
	case 1: s->animations = !s->animations; break;
	case 2: {
		static const int steps[] = { 0, 100, 125, 150, 200 };
		int i = 0;
		for (int k = 0; k < 5; k++)
			if (steps[k] == s->scale_pct)
				i = k;
		i = (i + dir + 5) % 5;
		s->scale_pct = steps[i];
		compute_scale(a);
		break;
	}
	case 3: s->show_unsupported = !s->show_unsupported; break;
	case 4: s->remember_last = !s->remember_last; break;
	case 5: s->strategy = (s->strategy + (dir > 0 ? 1 : 2)) % 3; break;
	case 6: s->show_details = !s->show_details; break;
	}
	settings_save(a);
	a->dirty = true;
	app_emit(a, "setting index=%d theme=%s", idx, a->set.theme ? "dark" : "light");
}

static void power_action(struct app *a, enum power_action act)
{
	if (a->ipc_fd < 0) {
		toast(a, "Power actions are not available in this environment.");
		return;
	}
	struct ipc_msg m, r;
	memset(&m, 0, sizeof(m));
	m.type = IPC_POWER;
	m.arg = act;
	app_emit(a, "power action=%d", act);
	if (ipc_request(a, &m, NULL, 0, &r) < 0 || r.status != 0)
		open_dialog(a, DLG_ERROR, "The power action failed", r.text, false);
}

static void activate_dialog(struct app *a, struct item *it)
{
	struct screen *sc = &a->stack[a->depth - 1];
	switch (it->action) {
	case ACT_REBOOT: a->dlg.kind = DLG_NONE; power_action(a, POWER_REBOOT); break;
	case ACT_POWEROFF: a->dlg.kind = DLG_NONE; power_action(a, POWER_OFF); break;
	case ACT_FIRMWARE: a->dlg.kind = DLG_NONE; power_action(a, POWER_FIRMWARE); break;
	case ACT_CANCEL:
	case ACT_DISMISS: a->dlg.kind = DLG_NONE; break;
	case ACT_VARIANT: a->boot.variant = it->index; break;
	case ACT_TOGGLE_SAFE: a->boot.safe_graphics = !a->boot.safe_graphics; break;
	case ACT_STRATEGY: a->boot.strategy = (a->boot.strategy + 1) % 3; break;
	case ACT_BOOT:
		a->dlg.kind = DLG_NONE;
		if (sc->node)
			request_boot(a, sc->node);
		break;
	case ACT_CONFIRM_BOOT:
		a->dlg.kind = DLG_NONE;
		if (sc->node)
			start_boot(a, sc->node);
		break;
	case ACT_CONFIRM_EXPAND:
		a->dlg.kind = DLG_NONE;
		start_expand(a);
		break;
	case ACT_RETRY:
		a->dlg.kind = DLG_NONE;
		if (sc->node && sc->kind == SCR_DETAIL)
			start_boot(a, sc->node);
		break;
	default: break;
	}
	a->dirty = true;
}

static void activate(struct app *a, struct item *it)
{
	if (it->disabled) {
		if (it->action == ACT_BOOT || it->action == ACT_OPTIONS)
			toast(a, "Distro Buny cannot start this image. See the explanation on this page.");
		return;
	}
	struct screen *sc = &a->stack[a->depth - 1];
	switch (it->kind) {
	case IT_CATEGORY:
		if (it->node == a->cat.root) {
			push(a, SCR_FOLDER, a->cat.root);
		} else {
			open_node(a, it->node);
		}
		break;
	case IT_FOLDER:
	case IT_ISO:
	case IT_RESULT:
		reset_boot_options(a);
		if (it->kind == IT_RESULT)
			go_to(a, it->node);
		else
			open_node(a, it->node);
		break;
	case IT_BACK: pop(a); break;
	case IT_CRUMB: {
		/* pop until the crumb's node is on top */
		for (int d = a->depth - 1; d > 0; d--)
			if (a->stack[d - 1].node == it->node || (d - 1 == 0 && it->node == a->cat.root)) {
				snapshot(a, -1);
				a->depth = d;
				emit_screen(a);
				break;
			}
		break;
	}
	case IT_SETTING: cycle_setting(a, it->index, 1); break;
	case IT_TEXTFIELD: break;
	case IT_DIALOG: activate_dialog(a, it); break;
	case IT_BUTTON:
		switch (it->action) {
		case ACT_SETTINGS: push(a, SCR_SETTINGS, NULL); break;
		case ACT_INFO: push(a, SCR_INFO, NULL); break;
		case ACT_LOG: push(a, SCR_LOG, NULL); break;
		case ACT_SEARCH: push(a, SCR_SEARCH, NULL); break;
		case ACT_POWER: open_dialog(a, DLG_POWER, "Power", NULL, false); break;
		case ACT_EXPAND: expand_ask(a); break;
		case ACT_BOOT: request_boot(a, sc->node); break;
		case ACT_OPTIONS: open_dialog(a, DLG_OPTIONS, "Boot options", "Choose how to start this image.", false); break;
		case ACT_CANCEL:
			atomic_store(&g_boot_cancel, true);
			atomic_store(&a->boot.phase, BP_IDLE);
			break;
		case ACT_LAST_BOOT: {
			struct cat_node *n = catalog_find(&a->cat, a->last_boot_path);
			if (n) {
				reset_boot_options(a);
				go_to(a, n);
			}
			break;
		}
		case ACT_RESCAN: {
			struct ipc_msg m, r;
			memset(&m, 0, sizeof(m));
			m.type = IPC_RESCAN;
			if (ipc_request(a, &m, NULL, 0, &r) == 0 && r.status == 0)
				toast(a, "Drive found. Restarting the interface\xE2\x80\xA6"), atomic_store(&a->quit, true);
			else
				toast(a, "The drive is still not visible.");
			break;
		}
		default: break;
		}
		break;
	default: break;
	}
	a->dirty = true;
}

/* -------------------------------------------------------------- focus */

static bool focusable(const struct item *it)
{
	return it->kind != IT_TEXTFIELD;
}

static int default_focus(struct app *a, struct screen *sc)
{
	enum item_kind pref[] = { IT_CATEGORY, IT_FOLDER, IT_ISO, IT_RESULT, IT_SETTING };
	for (size_t k = 0; k < ARRAY_SIZE(pref); k++)
		for (int i = 0; i < a->nitems; i++)
			if (a->items[i].kind == pref[k])
				return i;
	for (int i = 0; i < a->nitems; i++)
		if (a->items[i].kind == IT_BUTTON && !a->items[i].disabled && (sc->kind != SCR_DETAIL || a->items[i].action == ACT_BOOT))
			return i;
	for (int i = 0; i < a->nitems; i++)
		if (focusable(&a->items[i]))
			return i;
	return -1;
}

static void resolve_focus(struct app *a, struct screen *sc)
{
	sc->focus = -1;
	if (sc->focus_key)
		for (int i = 0; i < a->nitems; i++)
			if (a->items[i].key == sc->focus_key)
				sc->focus = i;
	if (sc->focus < 0) {
		sc->focus = default_focus(a, sc);
		sc->focus_key = sc->focus >= 0 ? a->items[sc->focus].key : NULL;
	}
}

static int spatial(struct item *items, int n, int from, int dx, int dy)
{
	if (from < 0)
		return n ? 0 : -1;
	float cx = (float)items[from].r.x + (float)items[from].r.w / 2, cy = (float)items[from].r.y + (float)items[from].r.h / 2;
	int best = -1;
	float best_score = 1e30f;
	for (int i = 0; i < n; i++) {
		if (i == from || !focusable(&items[i]))
			continue;
		float ix = (float)items[i].r.x + (float)items[i].r.w / 2, iy = (float)items[i].r.y + (float)items[i].r.h / 2;
		float ddx = ix - cx, ddy = iy - cy;
		float along = ddx * (float)dx + ddy * (float)dy;
		if (along <= 1)
			continue;
		float across = fabsf(ddx * (float)dy) + fabsf(ddy * (float)dx);
		/* overlapping rows/columns are strongly preferred */
		bool overlap = dx ? !(items[i].r.y > items[from].r.y + items[from].r.h || items[i].r.y + items[i].r.h < items[from].r.y)
				  : !(items[i].r.x > items[from].r.x + items[from].r.w || items[i].r.x + items[i].r.w < items[from].r.x);
		float score = along + across * (overlap ? 0.5f : 3.0f);
		if (score < best_score) {
			best_score = score;
			best = i;
		}
	}
	return best < 0 ? from : best;
}

static void ensure_visible(struct app *a, struct screen *sc)
{
	if (sc->focus < 0 || !a->items[sc->focus].scrolls)
		return;
	struct rect r = a->items[sc->focus].r;
	float top = sc->kind == SCR_SEARCH ? S(a, 190) : S(a, 196);
	float bottom = (float)a->H - S(a, 40);
	if ((float)r.y < top)
		sc->scroll_target -= top - (float)r.y + S(a, 10);
	else if ((float)(r.y + r.h) > bottom)
		sc->scroll_target += (float)(r.y + r.h) - bottom + S(a, 10);
	float maxs = fmaxf(0, sc->content_h - sc->view_h);
	sc->scroll_target = BUNY_CLAMP(sc->scroll_target, 0.0f, maxs);
}

static void move_focus(struct app *a, int dx, int dy)
{
	if (a->dlg.kind != DLG_NONE) {
		if (dy && a->nditems)
			a->dlg.focus = BUNY_CLAMP(a->dlg.focus + dy, 0, a->nditems - 1);
		return;
	}
	struct screen *sc = &a->stack[a->depth - 1];
	int nf = spatial(a->items, a->nitems, sc->focus, dx, dy);
	if (nf >= 0) {
		sc->focus = nf;
		sc->focus_key = a->items[nf].key;
		ensure_visible(a, sc);
	}
}

/* -------------------------------------------------------------- input */

static struct item *hit(struct app *a, float x, float y)
{
	if (a->dlg.kind != DLG_NONE) {
		for (int i = 0; i < a->nditems; i++)
			if (rect_contains(a->ditems[i].r, (int)x, (int)y))
				return &a->ditems[i];
		return NULL;
	}
	struct screen *sc = &a->stack[a->depth - 1];
	float top = sc->kind == SCR_SEARCH ? S(a, 176) : S(a, 182);
	for (int i = a->nitems - 1; i >= 0; i--) {
		struct item *it = &a->items[i];
		if (it->scrolls && y < top)
			continue;
		if (rect_contains(it->r, (int)x, (int)y))
			return it;
	}
	return NULL;
}

static void handle_key(struct app *a, const struct in_event *e)
{
	struct screen *sc = &a->stack[a->depth - 1];
	a->keyboard_mode = true;
	a->pointer_visible = false;
	a->focus_visible = true;
	if (a->dlg.kind != DLG_NONE) {
		switch (e->code) {
		case KEY_ESC: case KEY_BACKSPACE:
			if (a->dlg.kind != DLG_BUSY)
				a->dlg.kind = DLG_NONE;
			break;
		case KEY_UP: move_focus(a, 0, -1); break;
		case KEY_DOWN: case KEY_TAB: move_focus(a, 0, 1); break;
		case KEY_ENTER: case KEY_KPENTER: case KEY_SPACE:
			if (a->dlg.focus >= 0 && a->dlg.focus < a->nditems)
				activate_dialog(a, &a->ditems[a->dlg.focus]);
			break;
		}
		a->dirty = true;
		return;
	}
	if (sc->kind == SCR_BOOT) {
		if (e->code == KEY_ESC && atomic_load(&a->boot.phase) <= BP_LOADING) {
			atomic_store(&g_boot_cancel, true);
			atomic_store(&a->boot.phase, BP_IDLE);
		}
		return;
	}
	if (sc->kind == SCR_SEARCH) {
		size_t ql = strlen(a->query);
		if (e->code == KEY_BACKSPACE) {
			if (ql) {
				/* remove the last UTF-8 character */
				while (ql > 0 && ((unsigned char)a->query[ql - 1] & 0xC0) == 0x80)
					ql--;
				a->query[ql ? ql - 1 : 0] = '\0';
				sc->scroll = sc->scroll_target = 0;
				sc->focus_key = NULL;
			} else {
				pop(a);
			}
			a->dirty = true;
			return;
		}
		if (e->ch && e->ch >= 0x20 && ql + 2 < sizeof(a->query)) {
			a->query[ql] = (char)e->ch;
			a->query[ql + 1] = '\0';
			sc->scroll = sc->scroll_target = 0;
			sc->focus_key = NULL;
			a->dirty = true;
			app_emit(a, "ui search query=\"%s\"", a->query);
			return;
		}
	}
	switch (e->code) {
	case KEY_LEFT:
		if (sc->kind == SCR_SETTINGS && sc->focus >= 0 && a->items[sc->focus].kind == IT_SETTING)
			cycle_setting(a, a->items[sc->focus].index, -1);
		else
			move_focus(a, -1, 0);
		break;
	case KEY_RIGHT:
		if (sc->kind == SCR_SETTINGS && sc->focus >= 0 && a->items[sc->focus].kind == IT_SETTING)
			cycle_setting(a, a->items[sc->focus].index, 1);
		else
			move_focus(a, 1, 0);
		break;
	case KEY_UP: move_focus(a, 0, -1); break;
	case KEY_DOWN: move_focus(a, 0, 1); break;
	case KEY_TAB: {
		int n = a->nitems;
		if (n) {
			int d = (e->mods & MOD_SHIFT) ? -1 : 1;
			int f = sc->focus;
			for (int k = 0; k < n; k++) {
				f = (f + d + n) % n;
				if (focusable(&a->items[f]))
					break;
			}
			sc->focus = f;
			sc->focus_key = a->items[f].key;
			ensure_visible(a, sc);
		}
		break;
	}
	case KEY_ENTER:
	case KEY_KPENTER:
	case KEY_SPACE:
		if (sc->focus >= 0 && sc->focus < a->nitems)
			activate(a, &a->items[sc->focus]);
		break;
	case KEY_ESC:
	case KEY_BACKSPACE:
		pop(a);
		break;
	case KEY_HOME:
		if (a->depth > 1) {
			snapshot(a, -1);
			a->depth = 1;
			emit_screen(a);
		}
		break;
	case KEY_PAGEDOWN:
		sc->scroll_target = fminf(sc->scroll_target + sc->view_h * 0.8f, fmaxf(0, sc->content_h - sc->view_h));
		break;
	case KEY_PAGEUP:
		sc->scroll_target = fmaxf(0, sc->scroll_target - sc->view_h * 0.8f);
		break;
	case KEY_F10:
		open_dialog(a, DLG_POWER, "Power", NULL, false);
		break;
	default:
		/* typing anywhere starts a search */
		if (e->ch && e->ch > 0x20 && a->have_catalog && sc->kind != SCR_SETTINGS) {
			a->query[0] = e->ch == '/' ? '\0' : (char)e->ch;
			a->query[1] = '\0';
			push(a, SCR_SEARCH, NULL);
		}
		break;
	}
	a->dirty = true;
}

static void handle_pointer(struct app *a, const struct in_event *e)
{
	a->px = e->x;
	a->py = e->y;
	a->pointer_visible = true;
	a->pointer_ms = a->now;
	a->dirty = true;
	if (e->type == IN_MOTION) {
		a->keyboard_mode = false;
		struct item *it = hit(a, e->x, e->y);
		a->hover_key = it ? it->key : NULL;
		return;
	}
	if (e->type == IN_SCROLL) {
		struct screen *sc = &a->stack[a->depth - 1];
		sc->scroll_target = BUNY_CLAMP(sc->scroll_target + e->dy * S(a, 90), 0.0f, fmaxf(0, sc->content_h - sc->view_h));
		return;
	}
	if (e->type == IN_BUTTON) {
		if (e->code == BTN_RIGHT && e->pressed) {
			if (a->dlg.kind == DLG_BUSY)
				return;
			if (a->dlg.kind != DLG_NONE)
				a->dlg.kind = DLG_NONE;
			else if (a->stack[a->depth - 1].kind != SCR_BOOT)
				pop(a);
			return;
		}
		if (e->code != BTN_LEFT)
			return;
		struct item *it = hit(a, e->x, e->y);
		if (e->pressed) {
			a->pressing = true;
			a->hover_key = it ? it->key : NULL;
			if (!it && a->dlg.kind != DLG_NONE && a->dlg.kind != DLG_OPTIONS && a->dlg.kind != DLG_BUSY)
				a->dlg.kind = DLG_NONE; /* click outside closes */
			return;
		}
		if (a->pressing && it && it->key == a->hover_key) {
			if (a->dlg.kind == DLG_NONE) {
				struct screen *sc = &a->stack[a->depth - 1];
				sc->focus_key = it->key;
			}
			activate(a, it);
		}
		a->pressing = false;
	}
}

/* ---------------------------------------------------------- animation */

static bool update_anims(struct app *a, float dt)
{
	bool busy = false;
	struct screen *sc = &a->stack[a->depth - 1];
	const void *fk = a->dlg.kind != DLG_NONE ? (a->dlg.focus >= 0 && a->dlg.focus < a->nditems ? a->ditems[a->dlg.focus].key : NULL)
						  : (sc->focus >= 0 ? a->items[sc->focus].key : NULL);
	const void *targets[2] = { a->keyboard_mode ? fk : NULL, a->keyboard_mode ? NULL : a->hover_key };
	/* make sure targets have slots */
	for (int t = 0; t < 2; t++) {
		if (!targets[t])
			continue;
		bool found = false;
		for (int i = 0; i < 64; i++)
			if (a->anims[i].key == targets[t])
				found = true;
		if (!found) {
			int victim = 0;
			for (int i = 0; i < 64; i++)
				if (a->anims[i].value < a->anims[victim].value || !a->anims[i].key) {
					victim = i;
					if (!a->anims[i].key)
						break;
				}
			a->anims[victim] = (struct anim_slot){ targets[t], 0, a->now };
		}
	}
	float k = a->set.animations ? 1.0f - expf(-dt / 70.0f) : 1.0f;
	for (int i = 0; i < 64; i++) {
		if (!a->anims[i].key)
			continue;
		float target = (a->anims[i].key == targets[0] || a->anims[i].key == targets[1]) ? 1.0f : 0.0f;
		float v = a->anims[i].value + (target - a->anims[i].value) * k;
		if (fabsf(v - target) < 0.004f)
			v = target;
		else
			busy = true;
		a->anims[i].value = v;
		if (v == 0 && target == 0)
			a->anims[i].key = NULL;
	}
	/* smooth scrolling */
	float diff = sc->scroll_target - sc->scroll;
	if (fabsf(diff) > 0.5f) {
		sc->scroll += diff * (a->set.animations ? 1.0f - expf(-dt / 60.0f) : 1.0f);
		busy = true;
	} else {
		sc->scroll = sc->scroll_target;
	}
	/* focus ring */
	struct rect fr = { 0, 0, 0, 0 };
	if (a->dlg.kind != DLG_NONE) {
		if (a->dlg.focus >= 0 && a->dlg.focus < a->nditems)
			fr = a->ditems[a->dlg.focus].r;
	} else if (sc->focus >= 0) {
		fr = a->items[sc->focus].r;
	}
	float to[4] = { (float)fr.x, (float)fr.y, (float)fr.w, (float)fr.h };
	if (memcmp(to, a->focus_to, sizeof(to)) != 0) {
		bool jump = a->focus_to[2] == 0 || fabsf(to[1] - a->focus_to[1]) > (float)a->H * 0.5f;
		memcpy(a->focus_from, jump ? to : a->focus_rect, sizeof(to));
		memcpy(a->focus_to, to, sizeof(to));
		a->focus_anim_start = a->now;
	}
	float t = a->set.animations ? BUNY_CLAMP((float)(a->now - a->focus_anim_start) / 150.0f, 0.0f, 1.0f) : 1.0f;
	float e = 1 - (1 - t) * (1 - t) * (1 - t);
	for (int i = 0; i < 4; i++)
		a->focus_rect[i] = a->focus_from[i] + (a->focus_to[i] - a->focus_from[i]) * e;
	if (t < 1)
		busy = true;
	if (a->dlg.kind != DLG_NONE && a->dlg.appear < 1) {
		a->dlg.appear = a->set.animations ? fminf(1.0f, (float)(a->now - a->dlg.opened_ms) / 180.0f) : 1.0f;
		busy = true;
	}
	if (a->transitioning)
		busy = true;
	if (sc->kind == SCR_BOOT || sc->kind == SCR_SEARCH)
		busy = true;
	if (a->now < a->toast_until)
		busy = true;
	return busy;
}

/* Lay out the visible screen so input is always handled against what is shown. */
static void refresh_layout(struct app *a)
{
	struct screen *sc = &a->stack[a->depth - 1];
	layout_screen(a, sc);
	layout_dialog(a);
	resolve_focus(a, sc);
}

/* ---------------------------------------------------------- scripting */

/* Development/test scripting (offscreen backend): BUNY_SCRIPT="key:Down;wait:300;shot:/tmp/x.png;quit" */
struct script {
	char **cmds;
	size_t n, pos;
	uint64_t wait_until;
};

static int key_code(const char *name)
{
	static const struct { const char *n; int c; } k[] = {
		{ "Up", KEY_UP }, { "Down", KEY_DOWN }, { "Left", KEY_LEFT }, { "Right", KEY_RIGHT }, { "Enter", KEY_ENTER },
		{ "Esc", KEY_ESC }, { "Backspace", KEY_BACKSPACE }, { "Tab", KEY_TAB }, { "Home", KEY_HOME },
		{ "PageDown", KEY_PAGEDOWN }, { "PageUp", KEY_PAGEUP }, { "Space", KEY_SPACE }, { "F10", KEY_F10 },
	};
	for (size_t i = 0; i < ARRAY_SIZE(k); i++)
		if (!strcmp(name, k[i].n))
			return k[i].c;
	return -1;
}

static const struct item *script_find_item(struct app *a, const char *t)
{
	const struct item *v = a->dlg.kind != DLG_NONE ? a->ditems : a->items;
	int n = a->dlg.kind != DLG_NONE ? a->nditems : a->nitems;
	for (int i = 0; i < n; i++) {
		const struct item *it = &v[i];
		if (!strcmp(t, "@back") ? it->kind == IT_BACK || it->action == ACT_BACK :
		    !strcmp(t, "@boot") ? it->action == ACT_BOOT :
		    !strcmp(t, "@search") ? it->action == ACT_SEARCH :
		    !strcmp(t, "@settings") ? it->action == ACT_SETTINGS :
		    !strcmp(t, "@expand") ? it->action == ACT_EXPAND :
		    !strcmp(t, "@confirm-expand") ? it->action == ACT_CONFIRM_EXPAND :
		    !strcmp(t, "@cancel") ? it->action == ACT_CANCEL :
		    str_starts(t, "@crumb:") ? it->kind == IT_CRUMB && it->index == atoi(t + 7) :
		    it->node && !strcmp(it->node->rel_path, t) && it->kind != IT_CRUMB)
			return it;
	}
	return NULL;
}

static void script_step(struct app *a, struct script *s)
{
	while (s->pos < s->n && a->now >= s->wait_until) {
		char *c = s->cmds[s->pos++];
		refresh_layout(a);
		if (str_starts(c, "key:")) {
			struct in_event e = { .type = IN_KEY, .code = key_code(c + 4), .pressed = true };
			if (e.code >= 0)
				handle_key(a, &e);
		} else if (str_starts(c, "type:")) {
			for (const char *p = c + 5; *p; p++) {
				struct in_event e = { .type = IN_KEY, .code = 0, .pressed = true, .ch = (unsigned char)*p };
				handle_key(a, &e);
			}
		} else if (str_starts(c, "move:") || str_starts(c, "click:")) {
			float x = 0, y = 0;
			sscanf(strchr(c, ':') + 1, "%f,%f", &x, &y);
			struct in_event e = { .type = IN_MOTION, .x = x, .y = y };
			handle_pointer(a, &e);
			if (c[0] == 'c') {
				e.type = IN_BUTTON;
				e.code = BTN_LEFT;
				e.pressed = true;
				handle_pointer(a, &e);
				e.pressed = false;
				handle_pointer(a, &e);
			}
		} else if (str_starts(c, "clickon:")) {
			/* click the centre of an item, found by what it shows: a catalog path,
			 * @back, @boot, @search, @settings or @crumb:N.  Exercises real hit-testing and activation;
			 * only the coordinates are looked up instead of hard-coded. */
			const char *t = c + 8;
			const struct item *it = script_find_item(a, t);
			if (!it) {
				app_emit(a, "script-missing target=\"%s\"", t);
				continue;
			}
			float x = it->r.x + it->r.w / 2.0f, y = it->r.y + it->r.h / 2.0f;
			struct in_event e = { .type = IN_MOTION, .x = x, .y = y };
			handle_pointer(a, &e);
			e.type = IN_BUTTON;
			e.code = BTN_LEFT;
			e.pressed = true;
			handle_pointer(a, &e);
			e.pressed = false;
			handle_pointer(a, &e);
		} else if (str_starts(c, "scroll:")) {
			struct in_event e = { .type = IN_SCROLL, .dy = strtof(c + 7, NULL), .x = a->px, .y = a->py };
			handle_pointer(a, &e);
		} else if (str_starts(c, "wait:")) {
			s->wait_until = a->now + strtoull(c + 5, NULL, 10);
			return;
		} else if (str_starts(c, "shot:")) {
			/* render the current (settled) state before capturing */
			refresh_layout(a);
			update_anims(a, 1000);
			render_frame(a);
			if (display_screenshot(a->frame, c + 5) == 0)
				LOGI("screenshot written to %s", c + 5);
		} else if (!strcmp(c, "quit")) {
			atomic_store(&a->quit, true);
			return;
		}
	}
}

/* ------------------------------------------------------------ lifecycle */

static void load_json_file(const char *path, struct json **out)
{
	size_t len;
	char *t = read_file(path, 16u << 20, &len);
	if (!t)
		return;
	char err[128];
	*out = json_parse(t, len, err, sizeof(err));
	if (!*out)
		LOGW("%s: %s", path, err);
	free(t);
}

int app_init(struct app *a)
{
	memset(a, 0, sizeof(*a));
	a->ipc_fd = -1;
	a->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	pthread_mutex_init(&a->lock, NULL);
	a->start_ms = now_ms();
	a->now = a->start_ms;
	const char *share = getenv("BUNY_SHARE");
	snprintf(a->share_dir, sizeof(a->share_dir), "%s", share ? share : "/usr/share/distrobuny");
	const char *ipc = getenv(BUNY_IPC_FD_ENV);
	if (ipc)
		a->ipc_fd = atoi(ipc);
	const char *rt = getenv("BUNY_RUNTIME");
	if (rt)
		load_json_file(rt, &a->runtime);
	a->lab = json_get_bool(a->runtime, "lab", false) || getenv("BUNY_LAB");
	const char *data = getenv("BUNY_DATA");
	const struct json *dj = json_get(a->runtime, "data");
	snprintf(a->data_dir, sizeof(a->data_dir), "%s", data ? data : "/run/distrobuny/data");
	if (dj) {
		snprintf(a->data_uuid, sizeof(a->data_uuid), "%s", json_get_str(dj, "uuid", ""));
		snprintf(a->data_fs, sizeof(a->data_fs), "%s", json_get_str(dj, "fs", ""));
		snprintf(a->data_label, sizeof(a->data_label), "%s", json_get_str(dj, "label", "DISTROBUNY"));
	} else {
		snprintf(a->data_label, sizeof(a->data_label), "DISTROBUNY");
	}
	const char *esp = json_get_str(a->runtime, "esp", getenv("BUNY_ESP"));
	if (esp)
		snprintf(a->esp_dir, sizeof(a->esp_dir), "%s", esp);

	char err[256];
	a->disp = display_open(err, sizeof(err));
	if (!a->disp) {
		LOGE("%s", err);
		return -1;
	}
	a->W = display_width(a->disp);
	a->H = display_height(a->disp);
	a->frame = canvas_new(a->W, a->H);
	a->scene = canvas_new(a->W, a->H);
	a->in = input_open(a->W, a->H);
	settings_load(a);
	apply_theme(a);
	compute_scale(a);
	a->px = (float)a->W / 2;
	a->py = (float)a->H / 2;
	reset_boot_options(a);

	char *p = path_join(a->share_dir, "fonts");
	if (fonts_load(&a->fonts, p) < 0) {
		free(p);
		LOGE("no fonts available");
		return -1;
	}
	free(p);
	p = path_join(a->share_dir, "logo.bimg");
	a->logo = image_load(p);
	free(p);
	p = path_join(a->share_dir, "icons.txt");
	a->icons = distro_icons_load(p);
	free(p);
	p = path_join(a->share_dir, "distributions.conf");
	if (rules_load(&a->rules, p, err, sizeof(err)) < 0)
		LOGW("identification rules: %s", err);
	free(p);
	p = path_join(a->share_dir, "compatibility.json");
	load_json_file(p, &a->compat);
	free(p);

	/* catalog: fast directory scan + metadata index from the build */
	struct stat st;
	if (stat(a->data_dir, &st) == 0 && S_ISDIR(st.st_mode) && catalog_scan(&a->cat, a->data_dir, &a->rules, err, sizeof(err)) == 0) {
		a->have_catalog = true;
		char *idx = xasprintf("%s/.distrobuny/index.json", a->data_dir);
		int restored = catalog_load_index(&a->cat, idx);
		free(idx);
		LOGI("catalog: %zu images, %d restored from the index", a->cat.total_isos, restored > 0 ? restored : 0);
		app_emit(a, "catalog images=%zu restored=%d", a->cat.total_isos, restored > 0 ? restored : 0);
		start_probing(a);
	} else {
		LOGW("no data partition at %s", a->data_dir);
	}
	last_boot_load(a);

	a->depth = 1;
	a->stack[0] = (struct screen){ .kind = a->have_catalog ? SCR_HOME : SCR_NODATA, .focus = -1 };
	/* a boot menu is usually driven by the keyboard: show focus from the start */
	a->keyboard_mode = true;
	a->focus_visible = true;
	a->dirty = true;
	int kb, pt;
	input_device_count(a->in, &kb, &pt);
	app_emit(a, "ui-ready isos=%zu display=%s %dx%d keyboards=%d pointers=%d ms=%llu", a->have_catalog ? a->cat.total_isos : 0,
		 display_backend(a->disp), a->W, a->H, kb, pt, (unsigned long long)(now_ms() - a->start_ms));
	emit_hint(a);
	if (expand_offer(a))
		app_emit(a, "ui expand offer=%s", expand_offer(a));
	return 0;
}

static void autoboot(struct app *a)
{
	const char *ab = json_get_str(a->runtime, "autoboot", getenv("BUNY_AUTOBOOT"));
	if (!ab || a->autoboot_done || !a->have_catalog || a->now - a->start_ms < 1500)
		return;
	a->autoboot_done = true;
	struct cat_node *n = catalog_find(&a->cat, ab);
	if (!n || n->kind != CAT_ISO) {
		app_emit(a, "autoboot-missing path=\"%s\"", ab);
		return;
	}
	reset_boot_options(a);
	go_to(a, n);
	start_boot(a, n);
}

void app_run(struct app *a)
{
	struct script scr = { 0 };
	const char *sc_env = getenv("BUNY_SCRIPT");
	if (sc_env) {
		char *dup = xstrdup(sc_env);
		for (char *tok = strtok(dup, ";"); tok; tok = strtok(NULL, ";")) {
			scr.cmds = xrealloc(scr.cmds, (scr.n + 1) * sizeof(char *));
			scr.cmds[scr.n++] = xstrdup(tok);
		}
		free(dup);
	}
	uint64_t last = now_ms();
	struct screen *prev_top = NULL;
	enum screen_kind prev_kind = SCR_HOME;
	const void *prev_focus = NULL;
	const struct cat_node *prev_node = (const struct cat_node *)-1;
	bool busy = true;
	while (!atomic_load(&a->quit)) {
		struct pollfd pfd[40];
		int nfd = 0;
		int fds[36];
		int ni = input_fds(a->in, fds, 36);
		for (int i = 0; i < ni; i++)
			pfd[nfd++] = (struct pollfd){ fds[i], POLLIN, 0 };
		if (a->wake_fd >= 0)
			pfd[nfd++] = (struct pollfd){ a->wake_fd, POLLIN, 0 };
		int timeout = busy || a->dirty ? 16 : (scr.n ? 30 : 500);
		poll(pfd, (nfds_t)nfd, timeout);
		a->now = now_ms();
		float dt = (float)(a->now - last);
		last = a->now;

		struct in_event ev[128];
		int n = input_read(a->in, ev, 128);
		for (int i = 0; i < n; i++) {
			refresh_layout(a);
			if (ev[i].type == IN_KEY)
				handle_key(a, &ev[i]);
			else
				handle_pointer(a, &ev[i]);
		}
		if (a->wake_fd >= 0) {
			uint64_t v;
			while (read(a->wake_fd, &v, sizeof(v)) == sizeof(v)) {
			}
		}
		apply_results(a);
		boot_poll(a);
		expand_poll(a);
		autoboot(a);
		if (scr.n)
			script_step(a, &scr);
		if (scr.n && scr.pos >= scr.n && a->now >= scr.wait_until)
			atomic_store(&a->quit, true);
		/* hide the pointer after keyboard use or inactivity */
		if (a->pointer_visible && a->now - a->pointer_ms > 8000) {
			a->pointer_visible = false;
			a->dirty = true;
		}

		struct screen *sc = &a->stack[a->depth - 1];
		refresh_layout(a);
		busy = update_anims(a, dt);
		if (busy || a->dirty) {
			render_frame(a);
			display_present(a->disp, a->frame, &(struct rect){ 0, 0, a->W, a->H }, 1);
			a->dirty = false;
		}
		/* lab: report focus changes for navigation tests */
		const void *fk = sc->focus >= 0 ? a->items[sc->focus].key : NULL;
		if (a->lab && (sc != prev_top || sc->kind != prev_kind || sc->node != prev_node)) {
			/* lab: where things are, so VM tests can click real items through
			 * the virtual tablet instead of guessing pixels */
			for (int i = 0; i < a->nitems; i++) {
				const struct item *it = &a->items[i];
				if (it->r.y + it->r.h <= 0 || it->r.y >= a->H || (!it->node && it->action == ACT_NONE))
					continue;
				color_t fg, bg;
				const char *badge = it->kind == IT_ISO && it->node ? compat_label(a, it->node, &fg, &bg) : "";
				app_emit(a, "ui item x=%d y=%d w=%d h=%d kind=%d action=%d path=\"%s\" badge=\"%s\"", it->r.x, it->r.y,
					 it->r.w, it->r.h, it->kind, it->action, it->node ? it->node->rel_path : "", badge);
			}
			prev_node = sc->node;
		}
		if (a->lab && (sc != prev_top || sc->kind != prev_kind || fk != prev_focus)) {
			prev_top = sc;
			prev_kind = sc->kind;
			prev_focus = fk;
			if (sc->focus >= 0) {
				struct item *it = &a->items[sc->focus];
				app_emit(a, "ui focus kind=%d node=\"%s\" action=%d index=%d", it->kind, it->node ? it->node->rel_path : "",
					 it->action, it->index);
			}
		}
	}
	for (size_t i = 0; i < scr.n; i++)
		free(scr.cmds[i]);
	free(scr.cmds);
}

void app_free(struct app *a)
{
	atomic_store(&a->quit, true);
	if (a->probe_running)
		pthread_join(a->probe_thread, NULL);
	if (a->boot.running)
		pthread_join(a->boot.thread, NULL);
	if (a->expand.running)
		pthread_join(a->expand.thread, NULL);
	apply_results(a);
	if (a->have_catalog)
		catalog_free(&a->cat);
	rules_free(&a->rules);
	json_free(a->runtime);
	json_free(a->compat);
	image_free(a->logo);
	distro_icons_free(a->icons);
	canvas_free(a->frame);
	canvas_free(a->scene);
	canvas_free(a->prev);
	canvas_free(a->bg);
	input_close(a->in);
	display_close(a->disp);
	if (a->wake_fd >= 0)
		close(a->wake_fd);
}
