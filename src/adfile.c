/*
 * adfile.c - virtual `._X` (see adfile.h)
 *
 * Two layers:
 *   - clean images (direct-mapped, 4096 slots, a collision overwrites): only the front part is cached
 *     (header + FinderInfo + extended attribute block, usually under 4KB) and the resource fork is
 *     read from the backend by offset; an X with no attributes is just recorded as "empty". X's
 *     change value tells whether the image is out of date.
 *   - sessions (a linked list; only the few being written at any moment): when the client writes /
 *     truncates / creates, the whole contents go into memory (resource fork included); on COMMIT /
 *     CLOSE / a stable write they are parsed and the differences written back to the backend, then
 *     the session is discarded and the image is synthesised from the backend again next time.
 * Memory limits: 64MB per image, at most 4096 x front part for clean images, 256MB for all sessions together.
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <time.h>

#include "adfile.h"
#include "appledouble.h"
#include "nfs4_state.h"

#define ADF_IMG_MAX	(64u << 20)
#define ADF_FULL_TOTAL	(256u << 20)
#define ADF_CHUNK	(1u << 20)
#define ADF_ALIASES	64		/* rename aliases: at most this many, recycled by age */
#define ADF_ALIAS_MS	5000		/* alias lifetime (the client comes to create `._A` within milliseconds of the rename) */
#define ADF_SLOTS	4096		/* clean image cache: direct-mapped, a collision overwrites; no attributes is just an "empty" marker */

/* Clean image (synthesised from the backend's current state) */
struct nfuse_adclean {
	nfuse_ino_t	x;		/* 0 = empty slot */
	uint64_t	change;		/* X's change when synthesised */
	uint8_t		*pre;		/* header + FinderInfo + extended attribute block (NULL when empty) */
	uint32_t	prelen;		/* = start of the resource fork */
	uint64_t	rsrc_len;	/* resource fork length in the image (the 286-byte placeholder if there is none) */
	uint8_t		rsrc_real;
	uint8_t		empty;		/* X has no attributes at all */
};

/* Session: the client created / wrote / truncated it, the whole contents are in memory */
struct nfuse_adimg {
	struct nfuse_adimg	*next;
	nfuse_ino_t		x;
	uint8_t			*full;
	uint64_t		flen, fcap;
	int			dirty;
	uint32_t		gen;
};

static const char *FI = "com.apple.FinderInfo";
static const char *RF = "com.apple.ResourceFork";

const char *nfuse_adf_target(const char *name)
{
	if (name[0] == '.' && name[1] == '_' && name[2] && strcmp(name, "._.") != 0)
		return name + 2;
	return NULL;
}

static struct nfuse_adclean *slot_of(struct nfuse_server *s, nfuse_ino_t x)
{
	uint64_t h = x * 0x9e3779b97f4a7c15ull;

	if (!s->adclean) {
		s->adclean = calloc(ADF_SLOTS, sizeof(struct nfuse_adclean));
		if (!s->adclean)
			return NULL;
	}
	return &((struct nfuse_adclean *)s->adclean)[(h >> 32) & (ADF_SLOTS - 1)];
}

static void clean_clear(struct nfuse_server *s, struct nfuse_adclean *c)
{
	if (c->pre)
		s->adf_pre_bytes -= c->prelen;
	free(c->pre);
	memset(c, 0, sizeof(*c));
}

static void clean_forget(struct nfuse_server *s, nfuse_ino_t x)
{
	struct nfuse_adclean *c = slot_of(s, x);

	if (c && c->x == x)
		clean_clear(s, c);
}

static struct nfuse_adimg *sess_find(struct nfuse_server *s, nfuse_ino_t x, int create)
{
	struct nfuse_adimg *m;

	for (m = s->adimgs; m; m = m->next)
		if (m->x == x)
			return m;
	if (!create)
		return NULL;
	m = calloc(1, sizeof(*m));
	if (!m)
		return NULL;
	m->x = x;
	m->next = s->adimgs;
	s->adimgs = m;
	s->adf_sessions++;
	return m;
}

static void sess_free(struct nfuse_server *s, struct nfuse_adimg *m)
{
	struct nfuse_adimg **pp = &s->adimgs;

	while (*pp && *pp != m)
		pp = &(*pp)->next;
	if (*pp)
		*pp = m->next;
	s->adf_full_bytes -= m->fcap;
	free(m->full);
	free(m);
}

static int x_change(struct nfuse_server *s, nfuse_ino_t x, uint64_t *chg)
{
	struct nfuse_attr st;
	int r = s->be.ops->getattr(s->be.ctx, x, &st, NFUSE_WANT_BASIC);

	if (r)
		return r;
	*chg = st.change;
	return 0;
}

/* X's attribute name list (malloc'd; *len is the size in bytes) */
static int x_list(struct nfuse_server *s, nfuse_ino_t x, char **out, int *len)
{
	int n;

	*out = NULL;
	*len = 0;
	if (!s->be.ops->listxattr)
		return 0;
	n = s->be.ops->listxattr(s->be.ctx, x, NULL, 0);
	if (n == -ENOTSUP || n == -EOPNOTSUPP || n == -ENOATTR || n == 0)
		return 0;
	if (n < 0)
		return n;
	*out = malloc((size_t)n);
	if (!*out)
		return -ENOMEM;
	n = s->be.ops->listxattr(s->be.ctx, x, *out, (size_t)n);
	if (n < 0) {
		free(*out);
		*out = NULL;
		return n;
	}
	*len = n;
	return 0;
}

static int x_size(struct nfuse_server *s, nfuse_ino_t x, const char *name)
{
	return s->be.ops->getxattr ? s->be.ops->getxattr(s->be.ctx, x, name, NULL, 0) : -ENOATTR;
}

/* Read the whole value of one of X's attributes (for small values; the resource fork does not come through here) */
static int x_read_all(struct nfuse_server *s, nfuse_ino_t x, const char *name, uint8_t **out,
		      uint32_t *len)
{
	int n = x_size(s, x, name), got = 0, r;

	*out = NULL;
	*len = 0;
	if (n < 0)
		return n;
	if ((unsigned)n > ADF_IMG_MAX)
		return -EFBIG;
	*out = malloc(n ? (size_t)n : 1);
	if (!*out)
		return -ENOMEM;
	while (got < n) {
		r = s->be.ops->xattr_read(s->be.ctx, x, name, *out + got, (uint32_t)(n - got),
					  (uint64_t)got);
		if (r <= 0)
			break;
		got += r;
	}
	*len = (uint32_t)got;
	return 0;
}

/* Synthesise the clean image from the backend's current state (reused as is if up to date); returns the slot, NULL on failure (*err) */
static struct nfuse_adclean *clean_get(struct nfuse_server *s, nfuse_ino_t x, int *err)
{
	struct nfuse_adclean *c = slot_of(s, x);
	struct nfuse_ad_val *vals = NULL;
	uint8_t finfo[32], **bufs = NULL;
	char *names = NULL, *p;
	int nl = 0, r, haveFi = 0, nv = 0, cnt = 0, i;
	uint64_t chg, rlen = 0;
	uint32_t roff;

	*err = 0;
	if (!c) {
		*err = -ENOMEM;
		return NULL;
	}
	if (!s->be.ops->xattr_read) {
		*err = -ENOTSUP;
		return NULL;
	}
	r = x_change(s, x, &chg);
	if (r) {
		*err = r;
		return NULL;
	}
	if (c->x == x && c->change == chg)
		return c;
	clean_clear(s, c);
	r = x_list(s, x, &names, &nl);
	if (r)
		goto out;
	for (p = names; names && p < names + nl; p += strlen(p) + 1)
		cnt++;
	vals = calloc((size_t)cnt + 1, sizeof(*vals));
	bufs = calloc((size_t)cnt + 1, sizeof(*bufs));
	if (!vals || !bufs) {
		r = -ENOMEM;
		goto out;
	}
	memset(finfo, 0, sizeof(finfo));
	for (p = names; names && p < names + nl; p += strlen(p) + 1) {
		if (!*p)
			continue;
		if (strcmp(p, FI) == 0) {
			if (s->be.ops->xattr_read(s->be.ctx, x, FI, finfo, 32, 0) > 0)
				haveFi = 1;
		} else if (strcmp(p, RF) == 0) {
			int n = x_size(s, x, RF);
			if (n > 0)
				rlen = (uint64_t)n;
		} else {
			uint32_t l;

			r = x_read_all(s, x, p, &bufs[nv], &l);
			if (r == -ENOATTR) {
				r = 0;
				continue;
			}
			if (r)
				goto out;
			vals[nv].name = p;
			vals[nv].val = bufs[nv];
			vals[nv].len = l;
			nv++;
		}
	}
	c->x = x;
	c->change = chg;
	c->rsrc_real = rlen > 0;
	c->rsrc_len = rlen ? rlen : NFUSE_AD_BLANK_RSRC_LEN;
	c->empty = !haveFi && !rlen && nv == 0;
	if (!c->empty) {
		r = nfuse_ad_build_prefix(haveFi ? finfo : NULL, vals, (uint32_t)nv, c->rsrc_len,
					  &c->pre, &c->prelen, &roff);
		if (!r && (uint64_t)c->prelen + c->rsrc_len > ADF_IMG_MAX)
			r = -EFBIG;
		if (r) {
			clean_clear(s, c);
			goto out;
		}
		s->adf_pre_bytes += c->prelen;
	}
	s->adf_builds++;
out:
	for (i = 0; bufs && i < nv; i++)
		free(bufs[i]);
	free(bufs);
	free(vals);
	free(names);
	if (r) {
		*err = r;
		return NULL;
	}
	return c;
}

/* Bytes [off, off+count) of a clean image (front part + resource fork read by offset) */
static int clean_read(struct nfuse_server *s, nfuse_ino_t x, const struct nfuse_adclean *c,
		      uint8_t *buf, uint32_t count, uint64_t off)
{
	uint64_t total = c->empty ? 0 : (uint64_t)c->prelen + c->rsrc_len;
	uint32_t got = 0;

	if (off >= total)
		return 0;
	if (count > total - off)
		count = (uint32_t)(total - off);
	if (off < c->prelen) {
		uint32_t k = count < c->prelen - off ? count : (uint32_t)(c->prelen - off);
		memcpy(buf, c->pre + off, k);
		got = k;
	}
	while (got < count) {
		uint64_t ro = off + got - c->prelen;
		int r;

		if (!c->rsrc_real) {
			memcpy(buf + got, nfuse_ad_blank_rsrc() + ro, count - got);
			got = count;
			break;
		}
		r = s->be.ops->xattr_read(s->be.ctx, x, RF, buf + got, count - got, ro);
		if (r < 0)
			return got ? (int)got : r;
		if (r == 0) {
			memset(buf + got, 0, count - got);
			got = count;
			break;
		}
		got += (uint32_t)r;
	}
	return (int)got;
}

static int full_reserve(struct nfuse_server *s, struct nfuse_adimg *m, uint64_t len)
{
	uint64_t nc;
	uint8_t *nb;

	if (len > ADF_IMG_MAX)
		return -EFBIG;
	if (len <= m->fcap)
		return 0;
	nc = m->fcap ? m->fcap : 8192;
	while (nc < len)
		nc *= 2;
	if (nc > ADF_IMG_MAX)
		nc = ADF_IMG_MAX;
	if (s->adf_full_bytes + (nc - m->fcap) > ADF_FULL_TOTAL)
		return -ENOMEM;
	nb = realloc(m->full, (size_t)nc);
	if (!nb)
		return -ENOMEM;
	memset(nb + m->fcap, 0, (size_t)(nc - m->fcap));
	s->adf_full_bytes += nc - m->fcap;
	m->full = nb;
	m->fcap = nc;
	return 0;
}

/* Session (if there is none, load the whole clean image into memory) */
static struct nfuse_adimg *sess_get(struct nfuse_server *s, nfuse_ino_t x, int *err)
{
	struct nfuse_adimg *m = sess_find(s, x, 0);
	struct nfuse_adclean *c;
	uint64_t total, off = 0;
	int r;

	*err = 0;
	if (m)
		return m;
	c = clean_get(s, x, &r);
	if (!c) {
		*err = r;
		return NULL;
	}
	m = sess_find(s, x, 1);
	if (!m) {
		*err = -ENOMEM;
		return NULL;
	}
	total = c->empty ? 0 : (uint64_t)c->prelen + c->rsrc_len;
	r = full_reserve(s, m, total ? total : 1);
	while (!r && off < total) {
		uint32_t k = total - off > ADF_CHUNK ? ADF_CHUNK : (uint32_t)(total - off);
		int n = clean_read(s, x, c, m->full + off, k, off);
		if (n <= 0)
			r = n < 0 ? n : -EIO;
		else
			off += (uint64_t)n;
	}
	if (r) {
		sess_free(s, m);
		*err = r;
		return NULL;
	}
	m->flen = total;
	return m;
}

/* ---------- Rename aliases (see adfile.h) ---------- */

struct nfuse_adalias {
	nfuse_ino_t	dir, ndir, y;		/* dir = 0 is an empty slot */
	uint64_t	expire_ms;
	char		name[256];
	char		nname[256];
};

static uint64_t mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static struct nfuse_adalias *alias_find(struct nfuse_server *s, nfuse_ino_t dir, const char *name)
{
	struct nfuse_adalias *a = s->adalias;
	int i;

	for (i = 0; a && i < ADF_ALIASES; i++)
		if (a[i].dir == dir && strcmp(a[i].name, name) == 0)
			return &a[i];
	return NULL;
}

void nfuse_adf_renamed(struct nfuse_server *s, nfuse_ino_t odir, const char *oname,
		       nfuse_ino_t ndir, const char *nname, nfuse_ino_t y)
{
	struct nfuse_adalias *a, *slot;
	uint64_t now = mono_ms();
	int i;

	if (!s->adfile || nfuse_adf_target(oname) || strlen(oname) >= sizeof(slot->name) ||
	    strlen(nname) >= sizeof(slot->nname))
		return;
	if (!s->adalias) {
		s->adalias = calloc(ADF_ALIASES, sizeof(struct nfuse_adalias));
		if (!s->adalias)
			return;
	}
	a = s->adalias;
	slot = alias_find(s, odir, oname);
	for (i = 0; !slot && i < ADF_ALIASES; i++)	/* empty or expired */
		if (!a[i].dir || a[i].expire_ms <= now)
			slot = &a[i];
	if (!slot) {					/* all in use: evict the one that expires first */
		slot = &a[0];
		for (i = 1; i < ADF_ALIASES; i++)
			if (a[i].expire_ms < slot->expire_ms)
				slot = &a[i];
	}
	slot->dir = odir;
	slot->ndir = ndir;
	slot->y = y;
	slot->expire_ms = now + ADF_ALIAS_MS;
	strcpy(slot->name, oname);
	strcpy(slot->nname, nname);
	s->adf_alias_adds++;
}

int nfuse_adf_alias(struct nfuse_server *s, nfuse_ino_t dir, const char *name, nfuse_ino_t *y)
{
	const char *t = nfuse_adf_target(name);
	struct nfuse_adalias *a;
	nfuse_ino_t cur, now_y;

	if (!t || !s->adalias)
		return 0;
	a = alias_find(s, dir, t);
	if (!a)
		return 0;
	if (a->expire_ms <= mono_ms() ||
	    s->be.ops->lookup(s->be.ctx, dir, t, &cur) == 0 ||		/* A exists again: no longer an alias */
	    s->be.ops->lookup(s->be.ctx, a->ndir, a->nname, &now_y) || now_y != a->y) {	/* B is no longer where it was */
		memset(a, 0, sizeof(*a));
		return 0;
	}
	*y = a->y;
	return 1;
}

void nfuse_adf_alias_drop(struct nfuse_server *s, nfuse_ino_t dir, const char *name)
{
	const char *t = nfuse_adf_target(name);
	struct nfuse_adalias *a = t ? alias_find(s, dir, t) : NULL;

	if (a)
		memset(a, 0, sizeof(*a));
}

int nfuse_adf_lookup(struct nfuse_server *s, nfuse_ino_t dir, const char *name,
		     nfuse_ino_t *x, int *exists)
{
	const char *t = nfuse_adf_target(name);
	struct nfuse_adclean *c;
	int r;

	*exists = 0;
	if (!t || !s->be.ops->lookup)
		return -ENOENT;
	if (s->be.ops->lookup(s->be.ctx, dir, t, x)) {
		/* A is not there: was it just renamed away (the client is writing `._A` under the old name)? If so treat it as the virtual `._` of the new name */
		if (!nfuse_adf_alias(s, dir, name, x))
			return -ENOENT;
		s->adf_alias_hits++;
	}
	s->adf_lookups++;
	if (sess_find(s, *x, 0)) {
		*exists = 1;
		return 0;
	}
	c = clean_get(s, *x, &r);
	if (c && !c->empty) {
		*exists = 1;
		s->adf_virtual++;
	}
	return 0;
}

int nfuse_adf_attr(struct nfuse_server *s, nfuse_ino_t x, struct nfuse_attr *st)
{
	struct nfuse_adimg *m = sess_find(s, x, 0);
	struct nfuse_adclean *c = NULL;
	int r;

	if (!m) {
		c = clean_get(s, x, &r);
		if (!c)
			return r;
		if (c->empty)
			return -ENOENT;		/* X has no attributes any more: `._X` does not exist */
	}
	st->ino = NFUSE_ADF_FILEID(x);
	st->mode = S_IFREG | (st->mode & 0666);
	st->nlink = 1;
	st->size = m ? m->flen : (uint64_t)c->prelen + c->rsrc_len;
	st->used = st->size;
	st->change += (uint64_t)(m ? m->gen : 0) << 32;
	st->nf4type = 0;
	st->flags = 0;
	return 0;
}

int nfuse_adf_read(struct nfuse_server *s, nfuse_ino_t x, void *buf, uint32_t count,
		   uint64_t off)
{
	struct nfuse_adimg *m = sess_find(s, x, 0);
	struct nfuse_adclean *c;
	int r;

	s->adf_reads++;
	if (m) {
		if (off >= m->flen)
			return 0;
		if (count > m->flen - off)
			count = (uint32_t)(m->flen - off);
		memcpy(buf, m->full + off, count);
		s->adf_read_bytes += count;
		return (int)count;
	}
	c = clean_get(s, x, &r);
	if (!c)
		return r;
	r = clean_read(s, x, c, buf, count, off);
	if (r > 0)
		s->adf_read_bytes += (uint64_t)r;
	return r;
}

int nfuse_adf_write(struct nfuse_server *s, nfuse_ino_t x, const void *buf, uint32_t len,
		    uint64_t off)
{
	struct nfuse_adimg *m;
	int r;

	if (off + len > ADF_IMG_MAX)
		return -EFBIG;
	m = sess_get(s, x, &r);
	if (!m)
		return r;
	r = full_reserve(s, m, off + len);
	if (r)
		return r;
	if (off > m->flen)
		memset(m->full + m->flen, 0, (size_t)(off - m->flen));
	memcpy(m->full + off, buf, len);
	if (off + len > m->flen)
		m->flen = off + len;
	m->dirty = 1;
	m->gen++;
	s->adf_writes++;
	s->adf_write_bytes += len;
	return (int)len;
}

int nfuse_adf_truncate(struct nfuse_server *s, nfuse_ino_t x, uint64_t size)
{
	struct nfuse_adimg *m = sess_get(s, x, &(int){0});
	int r;

	if (!m)
		return -ENOMEM;
	r = full_reserve(s, m, size ? size : 1);
	if (r)
		return r;
	if (size > m->flen)
		memset(m->full + m->flen, 0, (size_t)(size - m->flen));
	m->flen = size;
	m->dirty = 1;
	m->gen++;
	return 0;
}

int nfuse_adf_create(struct nfuse_server *s, nfuse_ino_t x, int trunc)
{
	int r;
	struct nfuse_adimg *m = sess_get(s, x, &r);

	if (!m)
		return r;
	s->adf_creates++;
	if (trunc && m->flen) {
		m->flen = 0;
		m->dirty = 1;
		m->gen++;
	}
	return 0;
}

struct buf_rd {
	const uint8_t	*b;
	uint64_t	len;
};

static int buf_read_cb(void *ctx, void *out, uint32_t size, uint64_t off)
{
	struct buf_rd *r = ctx;

	if (off >= r->len)
		return 0;
	if (size > r->len - off)
		size = (uint32_t)(r->len - off);
	memcpy(out, r->b + off, size);
	return (int)size;
}

/* Does the value of name in the backend already equal v (compared chunk by chunk) */
static int x_equal(struct nfuse_server *s, nfuse_ino_t x, const char *name, const uint8_t *v,
		   uint64_t len)
{
	uint8_t *tmp;
	uint64_t off = 0;
	int n = x_size(s, x, name), eq = 1;

	if (n < 0 || (uint64_t)n != len)
		return 0;
	tmp = malloc(ADF_CHUNK);
	if (!tmp)
		return 0;
	while (off < len && eq) {
		uint32_t k = len - off > ADF_CHUNK ? ADF_CHUNK : (uint32_t)(len - off);
		int r = s->be.ops->xattr_read(s->be.ctx, x, name, tmp, k, off);
		if (r != (int)k || memcmp(tmp, v + off, k) != 0)
			eq = 0;
		off += k;
	}
	free(tmp);
	return eq;
}

/* Sync an AppleDouble image (b, len) into x's backend attributes */
static int sync_to(struct nfuse_server *s, nfuse_ino_t x, const uint8_t *b, uint64_t len)
{
	struct nfuse_ad ad;
	struct buf_rd rd = { b, len };
	char *names = NULL, *p;
	int nl, r, err = 0;
	uint32_t i;

	memset(&ad, 0, sizeof(ad));
	if (len) {
		r = nfuse_ad_parse(buf_read_cb, &rd, len, &ad);
		if (r) {
			s->adf_invalid++;
			if (s->log)
				s->log("virtual ._ file (inode %llu): data written is not AppleDouble (%llu bytes), attributes left unchanged",
				       (unsigned long long)x, (unsigned long long)len);
			return 0;
		}
	}
	for (i = 0; i < ad.n; i++) {
		const struct nfuse_ad_attr *a = &ad.a[i];

		if (a->off + a->len > len)
			continue;
		if (x_equal(s, x, a->name, b + a->off, a->len)) {
			s->adf_attr_same++;
			continue;
		}
		if (x_size(s, x, a->name) >= 0 && s->be.ops->xattr_truncate)
			s->be.ops->xattr_truncate(s->be.ctx, x, a->name, 0);
		r = 0;
		if (!a->len)
			r = s->be.ops->xattr_write(s->be.ctx, x, a->name, NULL, 0, 0);
		else {
			uint64_t off = 0;
			while (off < a->len && r >= 0) {
				uint32_t k = a->len - off > ADF_CHUNK ? ADF_CHUNK : (uint32_t)(a->len - off);
				r = s->be.ops->xattr_write(s->be.ctx, x, a->name, b + a->off + off, k, off);
				off += k;
			}
		}
		if (r < 0) {
			err = r;
			if (s->log)
				s->log("virtual ._: writing attribute %s (inode %llu) failed, errno %d", a->name,
				       (unsigned long long)x, -r);
		} else {
			s->adf_attr_writes++;
		}
	}
	/* Attributes no longer present in the image: remove them */
	if (x_list(s, x, &names, &nl) == 0) {
		for (p = names; names && p < names + nl; p += strlen(p) + 1) {
			if (!*p || nfuse_ad_find(&ad, p))
				continue;
			r = s->be.ops->removexattr ? s->be.ops->removexattr(s->be.ctx, x, p) : -ENOTSUP;
			if (r && r != -ENOATTR)
				err = r;
			else
				s->adf_attr_removes++;
		}
	}
	free(names);
	nfuse_ad_free(&ad);
	return err;
}

int nfuse_adf_apply(struct nfuse_server *s, nfuse_ino_t x)
{
	struct nfuse_adimg *m = sess_find(s, x, 0);
	int r = 0;

	if (!m)
		return 0;
	if (m->dirty) {
		r = sync_to(s, x, m->full, m->flen);
		s->adf_applies++;
		if (s->be.ops->invalidate)
			s->be.ops->invalidate(s->be.ctx, x);
	}
	/* Discard the session after writing back; next time the image is synthesised from the backend again */
	sess_free(s, m);
	clean_forget(s, x);
	return r;
}

int nfuse_adf_remove(struct nfuse_server *s, nfuse_ino_t x)
{
	struct nfuse_adimg *m = sess_find(s, x, 0);
	int r;

	if (m)
		sess_free(s, m);
	clean_forget(s, x);
	r = sync_to(s, x, NULL, 0);
	clean_forget(s, x);
	s->adf_removes++;
	return r;
}

static int adf_copy_to(struct nfuse_server *s, nfuse_ino_t a, nfuse_ino_t b)
{
	struct nfuse_adimg *m;
	int r;

	m = sess_get(s, a, &r);
	if (!m)
		return r;
	nfuse_adf_forget(s, b);
	r = sync_to(s, b, m->full, m->flen);
	clean_forget(s, b);
	if (s->be.ops->invalidate)
		s->be.ops->invalidate(s->be.ctx, b);
	return r;
}

int nfuse_adf_move(struct nfuse_server *s, nfuse_ino_t a, nfuse_ino_t b)
{
	int r = adf_copy_to(s, a, b);

	if (r)
		return r;
	s->adf_renames++;
	return nfuse_adf_remove(s, a);
}

int nfuse_adf_copy(struct nfuse_server *s, nfuse_ino_t a, nfuse_ino_t b)
{
	int r = adf_copy_to(s, a, b);

	if (!r)
		s->adf_renames++;
	return r;
}

void nfuse_adf_forget(struct nfuse_server *s, nfuse_ino_t x)
{
	struct nfuse_adimg *m = sess_find(s, x, 0);

	if (m)
		sess_free(s, m);
	clean_forget(s, x);
}

void nfuse_adf_flush_all(struct nfuse_server *s)
{
	while (s->adimgs)
		nfuse_adf_apply(s, s->adimgs->x);
}

void nfuse_adf_free_all(struct nfuse_server *s)
{
	int i;

	while (s->adimgs)
		sess_free(s, s->adimgs);
	free(s->adalias);
	s->adalias = NULL;
	if (s->adclean) {
		for (i = 0; i < ADF_SLOTS; i++)
			clean_clear(s, &((struct nfuse_adclean *)s->adclean)[i]);
		free(s->adclean);
		s->adclean = NULL;
	}
}
