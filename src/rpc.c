/*
 * rpc.c - ONC RPC v2 implementation (RFC 5531)
 *
 * On TCP every RPC message is split into fragments by "record marking": each fragment is preceded
 * by 4 bytes whose top bit is 1 for the last fragment and whose remaining 31 bits are the
 * fragment length.
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nfs4_prot.h"
#include "rpc.h"

/* AUTH_SYS credential body (RFC 5531, appendix A): stamp, machinename<255>, uid, gid, gids<16> */
static int decode_auth_sys(const uint8_t *body, uint32_t len, rpc_cred_t *cr)
{
	xdr_dec_t d;
	uint32_t n, i;

	xdr_dec_init(&d, body, len);
	(void)xdr_get_u32(&d);				/* stamp */
	(void)xdr_get_opaque_ref(&d, 255, &n);		/* machinename */
	cr->uid = xdr_get_u32(&d);
	cr->gid = xdr_get_u32(&d);
	n = xdr_get_u32(&d);
	if (n > RPC_MAX_GIDS)
		return -1;
	cr->ngids = n;
	for (i = 0; i < n; i++)
		cr->gids[i] = xdr_get_u32(&d);
	return d.err ? -1 : 0;
}

int rpc_decode_call(xdr_dec_t *d, rpc_call_t *c)
{
	uint32_t mtype, flavor, blen;
	const uint8_t *body;

	memset(c, 0, sizeof(*c));
	c->xid = xdr_get_u32(d);
	mtype = xdr_get_u32(d);
	if (d->err || mtype != RPC_MSG_CALL)
		return RPC_DEC_DROP;
	c->rpcvers = xdr_get_u32(d);
	c->prog = xdr_get_u32(d);
	c->vers = xdr_get_u32(d);
	c->proc = xdr_get_u32(d);
	if (d->err)
		return RPC_DEC_DROP;
	if (c->rpcvers != RPC_VERSION)
		return RPC_DEC_RPCMISMATCH;

	/* credential */
	flavor = xdr_get_u32(d);
	body = xdr_get_opaque_ref(d, 400, &blen);
	if (d->err)
		return RPC_DEC_BADCRED;
	c->cred.flavor = flavor;
	if (flavor == RPC_AUTH_SYS) {
		if (decode_auth_sys(body, blen, &c->cred))
			return RPC_DEC_BADCRED;
	} else if (flavor == RPC_AUTH_NONE) {
		c->cred.uid = c->cred.gid = (uint32_t)-2;	/* nobody */
	} else {
		return RPC_DEC_TOOWEAK;
	}

	/* verifier: only the AUTH_NONE form is accepted, its contents are ignored */
	(void)xdr_get_u32(d);
	(void)xdr_get_opaque_ref(d, 400, &blen);
	if (d->err)
		return RPC_DEC_BADCRED;
	return RPC_DEC_OK;
}

void rpc_reply_begin(xdr_enc_t *e)
{
	xdr_enc_reset(e);
	xdr_put_u32(e, 0);	/* placeholder for the record mark */
}

/* Common part: xid, REPLY, MSG_ACCEPTED, null verifier */
static void reply_accepted_head(xdr_enc_t *e, uint32_t xid, uint32_t stat)
{
	xdr_put_u32(e, xid);
	xdr_put_u32(e, RPC_MSG_REPLY);
	xdr_put_u32(e, RPC_MSG_ACCEPTED);
	xdr_put_u32(e, RPC_AUTH_NONE);
	xdr_put_u32(e, 0);
	xdr_put_u32(e, stat);
}

void rpc_reply_accepted(xdr_enc_t *e, uint32_t xid, uint32_t accept_stat)
{
	reply_accepted_head(e, xid, accept_stat);
}

void rpc_reply_prog_mismatch(xdr_enc_t *e, uint32_t xid, uint32_t lo, uint32_t hi)
{
	reply_accepted_head(e, xid, RPC_PROG_MISMATCH);
	xdr_put_u32(e, lo);
	xdr_put_u32(e, hi);
}

void rpc_reply_rpc_mismatch(xdr_enc_t *e, uint32_t xid)
{
	xdr_put_u32(e, xid);
	xdr_put_u32(e, RPC_MSG_REPLY);
	xdr_put_u32(e, RPC_MSG_DENIED);
	xdr_put_u32(e, RPC_MISMATCH);
	xdr_put_u32(e, RPC_VERSION);
	xdr_put_u32(e, RPC_VERSION);
}

void rpc_reply_auth_error(xdr_enc_t *e, uint32_t xid, uint32_t auth_stat)
{
	xdr_put_u32(e, xid);
	xdr_put_u32(e, RPC_MSG_REPLY);
	xdr_put_u32(e, RPC_MSG_DENIED);
	xdr_put_u32(e, RPC_AUTH_ERROR);
	xdr_put_u32(e, auth_stat);
}

size_t rpc_reply_finish(xdr_enc_t *e)
{
	if (e->err || e->len < 4)
		return 0;
	xdr_patch_u32(e, 0, RPC_LAST_FRAG | (uint32_t)(e->len - 4));
	return e->len;
}

/*
 * Read exactly n bytes; returns 1 on success, 0 if the peer closed on a message boundary, -1 on
 * error or a disconnect mid-message
 */
static int read_full(int fd, void *buf, size_t n, int at_boundary)
{
	uint8_t *p = buf;
	size_t got = 0;

	while (got < n) {
		ssize_t r = read(fd, p + got, n - got);
		if (r > 0) {
			got += (size_t)r;
			continue;
		}
		if (r == 0)
			return (at_boundary && got == 0) ? 0 : -1;
		if (errno == EINTR)
			continue;
		return -1;
	}
	return 1;
}

int rpc_read_record(int fd, uint8_t **buf, size_t *cap, size_t *len, size_t maxlen)
{
	int first = 1;

	*len = 0;
	for (;;) {
		uint8_t hdr[4];
		uint32_t mark, flen;
		int rc = read_full(fd, hdr, 4, first);

		if (rc <= 0)
			return rc;
		first = 0;
		mark = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) |
		       ((uint32_t)hdr[2] << 8) | (uint32_t)hdr[3];
		flen = mark & ~RPC_LAST_FRAG;
		if (*len + flen > maxlen)
			return -1;
		if (*len + flen > *cap) {
			size_t nc = *cap ? *cap : 65536;
			uint8_t *nb;
			while (nc < *len + flen)
				nc *= 2;
			nb = realloc(*buf, nc);
			if (!nb)
				return -1;
			*buf = nb;
			*cap = nc;
		}
		if (flen && read_full(fd, *buf + *len, flen, 0) != 1)
			return -1;
		*len += flen;
		if (mark & RPC_LAST_FRAG)
			return 1;
	}
}

int rpc_write_all(int fd, const uint8_t *buf, size_t len)
{
	while (len) {
		ssize_t w = write(fd, buf, len);
		if (w > 0) {
			buf += w;
			len -= (size_t)w;
			continue;
		}
		if (w < 0 && errno == EINTR)
			continue;
		return -1;
	}
	return 0;
}
