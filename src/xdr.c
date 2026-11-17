/*
 * xdr.c - XDR encoding and decoding implementation (RFC 4506)
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>

#include "xdr.h"

/* ---------- Decoding ---------- */

void xdr_dec_init(xdr_dec_t *d, const void *buf, size_t len)
{
	d->buf = buf;
	d->len = len;
	d->pos = 0;
	d->err = 0;
}

/* Whether at least n bytes remain; sets the error if not */
static int dec_need(xdr_dec_t *d, size_t n)
{
	if (d->err)
		return 0;
	if (d->len - d->pos < n) {
		d->err = 1;
		return 0;
	}
	return 1;
}

uint32_t xdr_get_u32(xdr_dec_t *d)
{
	const uint8_t *p;

	if (!dec_need(d, 4))
		return 0;
	p = d->buf + d->pos;
	d->pos += 4;
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

uint64_t xdr_get_u64(xdr_dec_t *d)
{
	uint64_t hi = xdr_get_u32(d);
	uint64_t lo = xdr_get_u32(d);

	return (hi << 32) | lo;
}

int xdr_get_bool(xdr_dec_t *d)
{
	uint32_t v = xdr_get_u32(d);

	/* a boolean may only be 0 or 1, anything else is a format error */
	if (v > 1)
		d->err = 1;
	return v == 1;
}

void xdr_get_fixed(xdr_dec_t *d, void *out, size_t n)
{
	size_t padded = XDR_PAD(n);

	if (!dec_need(d, padded)) {
		memset(out, 0, n);
		return;
	}
	memcpy(out, d->buf + d->pos, n);
	d->pos += padded;
}

const uint8_t *xdr_get_opaque_ref(xdr_dec_t *d, uint32_t max, uint32_t *len)
{
	uint32_t n = xdr_get_u32(d);
	const uint8_t *p;

	*len = 0;
	if (d->err)
		return NULL;
	if (n > max) {
		d->err = 1;
		return NULL;
	}
	if (!dec_need(d, XDR_PAD((size_t)n)))
		return NULL;
	p = d->buf + d->pos;
	d->pos += XDR_PAD((size_t)n);
	*len = n;
	return p;
}

int xdr_get_bitmap(xdr_dec_t *d, uint32_t *w, int maxw)
{
	uint32_t n = xdr_get_u32(d);
	uint32_t i;

	for (i = 0; i < (uint32_t)maxw; i++)
		w[i] = 0;
	/*
	 * give the bitmap length a generous upper bound so a malicious length cannot stall the
	 * loop
	 */
	if (n > 64) {
		d->err = 1;
		return 0;
	}
	for (i = 0; i < n && !d->err; i++) {
		uint32_t v = xdr_get_u32(d);
		if (i < (uint32_t)maxw)
			w[i] = v;
	}
	return (int)(n < (uint32_t)maxw ? n : (uint32_t)maxw);
}

void xdr_skip_opaque(xdr_dec_t *d, uint32_t n)
{
	if (dec_need(d, XDR_PAD((size_t)n)))
		d->pos += XDR_PAD((size_t)n);
}

/* ---------- Encoding ---------- */

int xdr_enc_init(xdr_enc_t *e, size_t initcap, size_t limit)
{
	e->buf = malloc(initcap);
	e->cap = e->buf ? initcap : 0;
	e->len = 0;
	e->limit = limit;
	e->err = e->buf ? 0 : 1;
	return e->err ? -1 : 0;
}

void xdr_enc_free(xdr_enc_t *e)
{
	free(e->buf);
	e->buf = NULL;
	e->cap = e->len = 0;
}

void xdr_enc_reset(xdr_enc_t *e)
{
	e->len = 0;
	e->err = 0;
}

/*
 * Make sure n more bytes can be written, growing the buffer if needed; sets the error past the
 * limit
 */
static uint8_t *enc_room(xdr_enc_t *e, size_t n)
{
	uint8_t *p;

	if (e->err)
		return NULL;
	if (e->limit && e->len + n > e->limit) {
		e->err = 1;
		return NULL;
	}
	if (e->len + n > e->cap) {
		size_t nc = e->cap ? e->cap : 1024;
		while (nc < e->len + n)
			nc *= 2;
		if (e->limit && nc > e->limit)
			nc = e->limit;
		p = realloc(e->buf, nc);
		if (!p) {
			e->err = 1;
			return NULL;
		}
		e->buf = p;
		e->cap = nc;
	}
	p = e->buf + e->len;
	e->len += n;
	return p;
}

void xdr_put_u32(xdr_enc_t *e, uint32_t v)
{
	uint8_t *p = enc_room(e, 4);

	if (!p)
		return;
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

void xdr_put_u64(xdr_enc_t *e, uint64_t v)
{
	xdr_put_u32(e, (uint32_t)(v >> 32));
	xdr_put_u32(e, (uint32_t)v);
}

void xdr_put_bool(xdr_enc_t *e, int v)
{
	xdr_put_u32(e, v ? 1 : 0);
}

void xdr_put_fixed(xdr_enc_t *e, const void *src, size_t n)
{
	size_t padded = XDR_PAD(n);
	uint8_t *p = enc_room(e, padded);

	if (!p)
		return;
	if (n)
		memcpy(p, src, n);
	if (padded > n)
		memset(p + n, 0, padded - n);
}

void xdr_put_opaque(xdr_enc_t *e, const void *src, uint32_t n)
{
	xdr_put_u32(e, n);
	xdr_put_fixed(e, src, n);
}

void xdr_put_string(xdr_enc_t *e, const char *s)
{
	xdr_put_opaque(e, s, (uint32_t)strlen(s));
}

void xdr_put_bitmap(xdr_enc_t *e, const uint32_t *w, int nw)
{
	int i;

	/* drop trailing all-zero words: the shortest encoding, as the spec requires */
	while (nw > 0 && w[nw - 1] == 0)
		nw--;
	xdr_put_u32(e, (uint32_t)nw);
	for (i = 0; i < nw; i++)
		xdr_put_u32(e, w[i]);
}

size_t xdr_reserve_u32(xdr_enc_t *e)
{
	size_t at = e->len;

	xdr_put_u32(e, 0);
	return at;
}

void xdr_patch_u32(xdr_enc_t *e, size_t at, uint32_t v)
{
	if (e->err || at + 4 > e->len)
		return;
	e->buf[at] = (uint8_t)(v >> 24);
	e->buf[at + 1] = (uint8_t)(v >> 16);
	e->buf[at + 2] = (uint8_t)(v >> 8);
	e->buf[at + 3] = (uint8_t)v;
}

uint8_t *xdr_put_opaque_space(xdr_enc_t *e, uint32_t n)
{
	size_t padded = XDR_PAD((size_t)n);
	uint8_t *p;

	xdr_put_u32(e, n);
	p = enc_room(e, padded);
	if (!p)
		return NULL;
	if (padded > n)
		memset(p + n, 0, padded - n);
	return p;
}

void xdr_truncate(xdr_enc_t *e, size_t len)
{
	if (len <= e->len)
		e->len = len;
}
