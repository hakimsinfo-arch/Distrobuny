/*
 * Unit tests: common library (SHA-256, JSON, strings, command lines).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "test.h"

#include <unistd.h>

#include "boot-adapters/bootcfg.h"
#include "common/json.h"
#include "common/sha256.h"
#include "common/strbuf.h"
#include "common/util.h"
#include "security/secureboot.h"

static void sha256_vectors(void)
{
	/* FIPS 180-4 example vectors */
	struct { const char *in; const char *hex; } v[] = {
		{ "", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
		{ "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
		{ "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
		  "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" },
	};
	for (size_t i = 0; i < ARRAY_SIZE(v); i++) {
		uint8_t d[32];
		char hex[65];
		sha256_buf(v[i].in, strlen(v[i].in), d);
		sha256_hex(d, hex);
		CHECK_STR(hex, v[i].hex);
	}
	/* incremental updates crossing block boundaries */
	char big[1000];
	memset(big, 'a', sizeof(big));
	struct sha256_ctx c;
	sha256_init(&c);
	for (int i = 0; i < 1000; i++)
		sha256_update(&c, big, 1000);
	uint8_t d[32];
	char hex[65];
	sha256_final(&c, d);
	sha256_hex(d, hex);
	CHECK_STR(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"); /* one million 'a' */
}

static void json_roundtrip(void)
{
	const char *text = "{\"a\": 1, \"b\": [true, false, null, \"x\\u00e9\\n\"], \"c\": {\"d\": -2.5e3}}";
	char err[128];
	struct json *j = json_parse(text, strlen(text), err, sizeof(err));
	CHECK(j != NULL);
	CHECK(json_get_num(j, "a", 0) == 1);
	const struct json *b = json_get(j, "b");
	CHECK(b && b->type == JSON_ARRAY && b->n == 4);
	CHECK_STR(b->items[3]->str, "x\xC3\xA9\n");
	CHECK(json_get_num(json_get(j, "c"), "d", 0) == -2500);
	json_free(j);

	const char *bad[] = { "{", "[1,]", "{\"a\" 1}", "\"unterminated", "[1] x", "{\"a\":\x01}" };
	for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
		struct json *x = json_parse(bad[i], strlen(bad[i]), err, sizeof(err));
		CHECK(x == NULL);
		json_free(x);
	}
	/* nesting bomb is rejected, not recursed into */
	struct strbuf deep = STRBUF_INIT;
	for (int i = 0; i < 5000; i++)
		sb_addc(&deep, '[');
	CHECK(json_parse(deep.buf, deep.len, err, sizeof(err)) == NULL);
	sb_free(&deep);

	struct strbuf sb = STRBUF_INIT;
	struct json_writer w;
	jw_init(&w, &sb, false);
	jw_obj_begin(&w);
	jw_key(&w, "s");
	jw_str(&w, "q\"\\");
	jw_key(&w, "n");
	jw_arr_begin(&w);
	jw_int(&w, 1);
	jw_bool(&w, true);
	jw_null(&w);
	jw_arr_end(&w);
	jw_obj_end(&w);
	CHECK_STR(sb_str(&sb), "{\"s\":\"q\\\"\\\\\",\"n\":[1,true,null]}");
	sb_free(&sb);
}

static void strings(void)
{
	CHECK(natural_cmp("file2", "file10") < 0);
	CHECK(natural_cmp("Ubuntu", "ubuntu") == 0);
	CHECK(natural_cmp("a", "ab") < 0);
	char buf[32];
	format_size(4700000000ull, buf, sizeof(buf));
	CHECK_STR(buf, "4.7 GB");
	char *j = path_join("/a/", "/b");
	CHECK_STR(j, "/a/b");
	free(j);
	char *d = path_dirname("/Desktop/x.iso");
	CHECK_STR(d, "/Desktop");
	free(d);
}

static void cmdline_quoting(void)
{
	struct boot_entry e = { 0 };
	cmdline_split("quiet img_loop=\"/Not friendly/arch.iso\" splash", &e.args);
	CHECK(e.args.n == 3);
	CHECK_STR(e.args.v[1], "img_loop=/Not friendly/arch.iso");
	entry_set_arg(&e, "rdinit", "/distrobuny/preinit");
	entry_remove_arg(&e, "splash");
	char *cl = entry_cmdline(&e);
	CHECK_STR(cl, "quiet \"img_loop=/Not friendly/arch.iso\" rdinit=/distrobuny/preinit");
	free(cl);
	/* arguments are inserted before an installer's "---" separator */
	struct boot_entry f = { 0 };
	cmdline_split("quiet --- splash", &f.args);
	entry_set_arg(&f, "live-media", "/dev/loop200");
	cl = entry_cmdline(&f);
	CHECK_STR(cl, "quiet live-media=/dev/loop200 --- splash");
	free(cl);
	/* "--" hands the rest to init (Linux Mint ends its command line with it):
	 * console parameters must stay in front, both of them */
	struct boot_entry g = { 0 };
	cmdline_split("boot=casper quiet --", &g.args);
	entry_add_arg(&g, "console=tty0");
	entry_add_arg(&g, "console=ttyS0,115200");
	cl = entry_cmdline(&g);
	CHECK_STR(cl, "boot=casper quiet console=tty0 console=ttyS0,115200 --");
	free(cl);
	ptrvec_free(&g.args, free);
	ptrvec_free(&e.args, free);
	ptrvec_free(&f.args, free);
}

static void kernel_keys(void)
{
	/* /proc/keys as root on an Ubuntu kernel with Secure Boot (abridged) */
	const char *text =
		"0b2f1a2c I--Q---     2 perm 1f0b0000     0     0 keyring   .builtin_trusted_keys: 1\n"
		"1e8f4f2d I------     1 perm 1f010000     0     0 asymmetri Canonical Ltd. Master Certificate Authority: "
		"ad91990bc22ab1f517048c23b6655a268e345a63: X509.rsa 2acc2fd0 []\n"
		"2c4e1a11 I------     1 perm 1f010000     0     0 asymmetri Microsoft Corporation UEFI CA 2011: "
		"13adbf4309bd82709c8cd54f316ed522988a1bd4: X509.rsa 2acc2fd0 []\n"
		"3a6e5b01 I------     1 perm 1f010000     0     0 asymmetri Fedora Project: fedoraca: "
		"fd77f6d1e2d4d8c6ab1b42ef1a1f0ed27cae4e5a: X509.rsa 41c0fe6f []\n"
		"3a6e5b02 I------     1 perm 1f010000     0     0 asymmetri Microsoft Corporation UEFI CA 2011: "
		"13adbf4309bd82709c8cd54f316ed522988a1bd4: X509.rsa 2acc2fd0 []\n"
		"4f00aa00 I------     1 perm 3f010000     0     0 blacklist bin:0123: 0\n";
	char path[] = "/tmp/buny-keys-XXXXXX";
	int fd = mkstemp(path);
	CHECK(fd >= 0 && write(fd, text, strlen(text)) == (ssize_t)strlen(text));
	close(fd);
	struct ptrvec v = { 0 };
	CHECK(kernel_key_subjects(path, &v) == 0);
	unlink(path);
	CHECK(v.n == 3); /* duplicates and non-X.509 entries are dropped */
	if (v.n == 3) {
		CHECK_STR(v.v[0], "Canonical Ltd. Master Certificate Authority");
		CHECK_STR(v.v[1], "Microsoft Corporation UEFI CA 2011");
		CHECK_STR(v.v[2], "Fedora Project: fedoraca");
	}
	ptrvec_free(&v, free);
	CHECK(kernel_key_subjects("/nonexistent/keys", &v) < 0);
}

int main(void)
{
	TEST(sha256_vectors);
	TEST(json_roundtrip);
	TEST(strings);
	TEST(cmdline_quoting);
	TEST(kernel_keys);
	return test_summary("core");
}
