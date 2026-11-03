/*
 * Distro Buny — SHA-256 (FIPS 180-4)
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BUNY_SHA256_H
#define BUNY_SHA256_H

#include <stddef.h>
#include <stdint.h>

struct sha256_ctx {
	uint32_t h[8];
	uint64_t total;
	uint8_t block[64];
	size_t used;
};

void sha256_init(struct sha256_ctx *c);
void sha256_update(struct sha256_ctx *c, const void *data, size_t len);
void sha256_final(struct sha256_ctx *c, uint8_t out[32]);
void sha256_hex(const uint8_t digest[32], char out[65]);
/* One-shot helper. */
void sha256_buf(const void *data, size_t len, uint8_t out[32]);

#endif
