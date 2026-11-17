/*
 * appledouble.c - AppleDouble (`._X`) parsing; the format is described in appledouble.h
 *
 * Only the header and the attribute table are read (a few hundred bytes to a few KB); attribute values
 * are read by offset on demand, and the resource fork is never loaded whole however large it is.
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "appledouble.h"

#define AD_MAGIC	0x00051607u
#define AD_VERSION	0x00020000u
#define AD_ID_RSRC	2u
#define AD_ID_FINFO	9u
#define ATTR_MAGIC	0x41545452u	/* 'ATTR' */
#define AD_HEAD_MAX	(1u << 20)	/* read at most 1MB of attribute table */
#define RSRC_BLANK_LEN	286u

static uint32_t be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t be16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static int ad_add(struct nfuse_ad *ad, const char *name, size_t nl, uint64_t off,
		  uint64_t len)
{
	struct nfuse_ad_attr *na = realloc(ad->a, (ad->n + 1) * sizeof(*na));

	if (!na)
		return -ENOMEM;
	ad->a = na;
	na[ad->n].name = malloc(nl + 1);
	if (!na[ad->n].name)
		return -ENOMEM;
	memcpy(na[ad->n].name, name, nl);
	na[ad->n].name[nl] = 0;
	na[ad->n].off = off;
	na[ad->n].len = len;
	ad->n++;
	return 0;
}

/* Read all of [off, off+size); returns -EIO on a short read */
static int rd_full(nfuse_ad_read_fn rd, void *ctx, uint8_t *buf, uint32_t size,
		   uint64_t off)
{
	uint32_t got = 0;

	while (got < size) {
		int r = rd(ctx, buf + got, size - got, off + got);
		if (r < 0)
			return r;
		if (r == 0)
			return -EIO;
		got += (uint32_t)r;
	}
	return 0;
}

static int all_zero(const uint8_t *p, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (p[i])
			return 0;
	return 1;
}

int nfuse_ad_parse(nfuse_ad_read_fn rd, void *ctx, uint64_t fsize, struct nfuse_ad *out)
{
	uint8_t head[26 + 12 * 16], *buf = NULL;
	uint32_t nent, i, finfo_off = 0, finfo_len = 0, rsrc_off = 0, rsrc_len = 0;
	int r, have_finfo = 0, have_rsrc = 0;

	memset(out, 0, sizeof(*out));
	if (fsize < 26)
		return -EINVAL;
	r = rd_full(rd, ctx, head, 26, 0);
	if (r)
		return r == -EIO ? -EINVAL : r;
	if (be32(head) != AD_MAGIC || be32(head + 4) != AD_VERSION)
		return -EINVAL;
	nent = be16(head + 24);
	if (nent > 16 || 26 + 12ull * nent > fsize)
		return -EINVAL;
	if (nent && rd_full(rd, ctx, head + 26, 12 * nent, 26))
		return -EINVAL;
	for (i = 0; i < nent; i++) {
		const uint8_t *e = head + 26 + 12 * i;
		uint32_t id = be32(e), off = be32(e + 4), len = be32(e + 8);

		if ((uint64_t)off + len > fsize)
			continue;		/* bad entry: skip it */
		if (id == AD_ID_FINFO) {
			finfo_off = off;
			finfo_len = len;
			have_finfo = 1;
		} else if (id == AD_ID_RSRC) {
			rsrc_off = off;
			rsrc_len = len;
			have_rsrc = 1;
		}
	}
	if (have_finfo && finfo_len >= 32) {
		uint8_t fi[32];

		if (rd_full(rd, ctx, fi, 32, finfo_off) == 0 && !all_zero(fi, 32) &&
		    (r = ad_add(out, NFUSE_XA_FINDERINFO, strlen(NFUSE_XA_FINDERINFO),
				finfo_off, 32)))
			goto fail;
	}
	/* Extended attribute block: right after the FinderInfo, with 2 bytes of padding in between (the unpadded form is accepted too) */
	if (have_finfo && finfo_len > 32 + 36) {
		uint32_t base = 0, k, total, data_start, nattr, p, maxread;
		uint8_t h[36];

		for (k = 0; k < 2 && !base; k++) {
			uint32_t at = finfo_off + 32 + (k == 0 ? 2 : 0);
			if ((uint64_t)at + 36 <= fsize && rd_full(rd, ctx, h, 36, at) == 0 &&
			    be32(h) == ATTR_MAGIC)
				base = at;
		}
		if (base) {
			total = be32(h + 8);
			data_start = be32(h + 12);
			nattr = be16(h + 34);
			maxread = data_start > base + 36 ? data_start - (base + 36) : 0;
			if (maxread > AD_HEAD_MAX)
				maxread = AD_HEAD_MAX;
			if ((uint64_t)base + 36 + maxread > fsize)
				maxread = (uint32_t)(fsize - base - 36);
			buf = maxread ? malloc(maxread) : NULL;
			if (maxread && (!buf || rd_full(rd, ctx, buf, maxread, base + 36)))
				goto done_attrs;
			for (k = 0, p = 0; k < nattr && p + 11 <= maxread; k++) {
				uint32_t off = be32(buf + p), len = be32(buf + p + 4);
				uint32_t nl = buf[p + 10];

				if (nl == 0 || p + 11 + nl > maxread)
					break;
				(void)total;
				if ((uint64_t)off + len <= fsize) {
					size_t l = strnlen((const char *)buf + p + 11, nl);
					if (l && (r = ad_add(out, (const char *)buf + p + 11, l, off,
							     len)))
						goto fail;
				}
				p = (p + 11 + nl + 3) & ~3u;
			}
		}
	}
done_attrs:
	free(buf);
	buf = NULL;
	if (have_rsrc && rsrc_len > 0) {
		int blank = 0;

		if (rsrc_len == RSRC_BLANK_LEN) {
			uint8_t rb[64];
			if (rd_full(rd, ctx, rb, sizeof(rb), rsrc_off) == 0 &&
			    memmem(rb, sizeof(rb), "intentionally left blank", 24))
				blank = 1;
		}
		if (!blank && (r = ad_add(out, NFUSE_XA_RESOURCEFORK,
					  strlen(NFUSE_XA_RESOURCEFORK), rsrc_off, rsrc_len)))
			goto fail;
	}
	return 0;
fail:
	free(buf);
	nfuse_ad_free(out);
	return r;
}

void nfuse_ad_free(struct nfuse_ad *ad)
{
	uint32_t i;

	for (i = 0; i < ad->n; i++)
		free(ad->a[i].name);
	free(ad->a);
	ad->a = NULL;
	ad->n = 0;
}

const struct nfuse_ad_attr *nfuse_ad_find(const struct nfuse_ad *ad, const char *name)
{
	uint32_t i;

	for (i = 0; i < ad->n; i++)
		if (strcmp(ad->a[i].name, name) == 0)
			return &ad->a[i];
	return NULL;
}

/* ---------- Building ---------- */

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

#define AD_FINFO_OFF	0x32u
#define AD_ATTR_HDR	(AD_FINFO_OFF + 32 + 2)	/* 0x54 */
#define AD_RSRC_DEFAULT	0xEE2u

const uint8_t *nfuse_ad_blank_rsrc(void)
{
	static uint8_t blank[NFUSE_AD_BLANK_RSRC_LEN];
	static int init;

	if (!init) {
		static const char msg[] = "This resource fork intentionally left blank   ";

		memset(blank, 0, sizeof(blank));
		/* Empty resource map: data offset 0x100, map offset 0x100, data length 0, map length 0x1e */
		put32(blank + 0, 0x100);
		put32(blank + 4, 0x100);
		put32(blank + 8, 0);
		put32(blank + 12, 0x1e);
		memcpy(blank + 16, msg, sizeof(msg) - 1);
		put32(blank + 0x100, 0x100);
		put32(blank + 0x104, 0x100);
		put32(blank + 0x108, 0);
		put32(blank + 0x10c, 0x1e);
		put16(blank + 0x118, 0x1e);	/* type list offset */
		put16(blank + 0x11a, 0x1e);	/* name list offset */
		put16(blank + 0x11c, 0xffff);	/* number of types - 1 = -1 */
		init = 1;
	}
	return blank;
}

int nfuse_ad_build_prefix(const uint8_t finfo[32], const struct nfuse_ad_val *v, uint32_t n,
			  uint64_t rsrc_len, uint8_t **out, uint32_t *outlen, uint32_t *rsrc_off)
{
	uint64_t tab = 0, data = 0, data_start, end, roff;
	uint32_t i, p, d;
	uint8_t *b;

	*out = NULL;
	for (i = 0; i < n; i++) {
		size_t nl = strlen(v[i].name) + 1;
		if (nl > 255)
			return -ENAMETOOLONG;
		tab = (tab + 11 + nl + 3) & ~3ull;
		data += v[i].len;
	}
	data_start = (AD_ATTR_HDR + 36 + tab + 3) & ~3ull;
	end = data_start + data;
	roff = end > AD_RSRC_DEFAULT ? (end + 4095) & ~4095ull : AD_RSRC_DEFAULT;
	if (roff > (1u << 30) || rsrc_len > 0xffffffffull)
		return -EFBIG;
	b = calloc(1, (size_t)roff);
	if (!b)
		return -ENOMEM;
	put32(b, AD_MAGIC);
	put32(b + 4, AD_VERSION);
	memcpy(b + 8, "Mac OS X        ", 16);
	put16(b + 24, 2);
	put32(b + 26, AD_ID_FINFO);
	put32(b + 30, AD_FINFO_OFF);
	put32(b + 34, (uint32_t)(roff - AD_FINFO_OFF));
	put32(b + 38, AD_ID_RSRC);
	put32(b + 42, (uint32_t)roff);
	put32(b + 46, (uint32_t)rsrc_len);
	if (finfo)
		memcpy(b + AD_FINFO_OFF, finfo, 32);
	put32(b + AD_ATTR_HDR, ATTR_MAGIC);
	put32(b + AD_ATTR_HDR + 8, (uint32_t)roff);		/* total_size */
	put32(b + AD_ATTR_HDR + 12, (uint32_t)data_start);
	put32(b + AD_ATTR_HDR + 16, (uint32_t)data);
	put16(b + AD_ATTR_HDR + 34, (uint16_t)n);
	p = AD_ATTR_HDR + 36;
	d = (uint32_t)data_start;
	for (i = 0; i < n; i++) {
		size_t nl = strlen(v[i].name) + 1;

		put32(b + p, d);
		put32(b + p + 4, v[i].len);
		b[p + 10] = (uint8_t)nl;
		memcpy(b + p + 11, v[i].name, nl);
		p = (uint32_t)((p + 11 + nl + 3) & ~3u);
		if (v[i].len)
			memcpy(b + d, v[i].val, v[i].len);
		d += v[i].len;
	}
	*out = b;
	*outlen = (uint32_t)roff;
	*rsrc_off = (uint32_t)roff;
	return 0;
}
