/*
 * nfs4_state.c - file handles, client IDs, open state, locks, seqid replay cache, DRC (RFC 7530,
 * sections 9 and 16)
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nfs4_state.h"

static void lock_free(struct openmacfuse_server *s, struct openmacfuse_lock *lk);

static uint32_t rand32(void)
{
	return arc4random();
}

int openmacfuse_server_init(struct openmacfuse_server *s, const char *export_name)
{
	uint32_t a, b;

	memset(s, 0, sizeof(*s));
	snprintf(s->export_name, sizeof(s->export_name), "%s",
		 export_name ? export_name : "");
	s->boot = rand32() | 1;		/* never 0, so it can be told apart from the all-zero special value */
	s->state_boot = s->boot;
	a = rand32();
	b = rand32();
	/* a random number per start: after a restart the client retransmits uncommitted writes */
	memcpy(s->writeverf, &a, 4);
	memcpy(s->writeverf + 4, &b, 4);
	s->fsid_major = 0x4e465553;	/* 'NFUS' */
	s->fsid_minor = s->boot;
	s->lease_time = 60;
	s->rwsize = 1024 * 1024;
	s->next_clientid = 1;
	s->next_other = 1;
	pthread_mutex_init(&s->lock, NULL);
	return 0;
}

/* ---------- Big-endian helpers ---------- */

static void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static uint32_t get_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void put_be64(uint8_t *p, uint64_t v)
{
	put_be32(p, (uint32_t)(v >> 32));
	put_be32(p + 4, (uint32_t)v);
}

static uint64_t get_be64(const uint8_t *p)
{
	return ((uint64_t)get_be32(p) << 32) | get_be32(p + 4);
}

/* ---------- File handles ---------- */

uint32_t openmacfuse_fh_encode(const struct openmacfuse_fh *fh, uint8_t out[OPENMACFUSE_FH_MAXLEN])
{
	uint32_t len = 16, i, n = 0;

	memset(out, 0, 16);
	out[0] = fh->kind;
	put_be32(out + 4, fh->boot);
	put_be64(out + 8, fh->ino);
	if (fh->kind == OPENMACFUSE_FH_NODE || fh->kind == OPENMACFUSE_FH_ATTRDIR ||
	    fh->kind == OPENMACFUSE_FH_ADFILE) {
		n = fh->nanc > OPENMACFUSE_FH_MAXANC ? OPENMACFUSE_FH_MAXANC : fh->nanc;
		for (i = 0; i < n; i++)
			put_be64(out + 16 + 8 * i, fh->anc[i]);
		len += 8 * n;
	} else if (fh->kind == OPENMACFUSE_FH_NAMEDATTR) {
		put_be64(out + 16, fh->attr_id);
		len += 8;
		n = fh->nanc > OPENMACFUSE_FH_MAXANC - 1 ? OPENMACFUSE_FH_MAXANC - 1 : fh->nanc;
		for (i = 0; i < n; i++)
			put_be64(out + 24 + 8 * i, fh->anc[i]);
		len += 8 * n;
	}
	out[1] = (uint8_t)n;
	return len;
}

uint32_t openmacfuse_fh_decode(const struct openmacfuse_server *s, const uint8_t *p,
			uint32_t len, struct openmacfuse_fh *fh)
{
	uint32_t i, want;

	memset(fh, 0, sizeof(*fh));
	if (len < 16)
		return NFS4ERR_BADHANDLE;
	fh->kind = p[0];
	fh->nanc = p[1];
	if (fh->kind < OPENMACFUSE_FH_NODE || fh->kind > OPENMACFUSE_FH_ADFILE ||
	    fh->nanc > OPENMACFUSE_FH_MAXANC ||
	    (fh->kind == OPENMACFUSE_FH_NAMEDATTR && fh->nanc > OPENMACFUSE_FH_MAXANC - 1) ||
	    (fh->kind == OPENMACFUSE_FH_PSEUDOROOT && fh->nanc))
		return NFS4ERR_BADHANDLE;
	want = 16 + 8u * fh->nanc + (fh->kind == OPENMACFUSE_FH_NAMEDATTR ? 8 : 0);
	if (len != want)
		return NFS4ERR_BADHANDLE;
	fh->boot = get_be32(p + 4);
	fh->ino = get_be64(p + 8);
	if (fh->kind == OPENMACFUSE_FH_NAMEDATTR) {
		fh->attr_id = get_be64(p + 16);
		for (i = 0; i < fh->nanc; i++)
			fh->anc[i] = get_be64(p + 24 + 8 * i);
	} else {
		for (i = 0; i < fh->nanc; i++)
			fh->anc[i] = get_be64(p + 16 + 8 * i);
	}
	if (fh->boot != s->boot)
		return NFS4ERR_STALE;	/* a handle issued by a previous run */
	return NFS4_OK;
}

/* ---------- seqid replay cache ---------- */

int openmacfuse_seq_check(const struct openmacfuse_seq *q, uint32_t seqid, uint32_t op)
{
	if (!q->valid)
		return 0;			/* new owner: any seqid is accepted */
	if (seqid == (uint32_t)(q->seqid + 1))
		return 0;
	if (seqid == q->seqid && q->op == op)
		return 1;
	return -1;
}

int openmacfuse_seq_bumps(uint32_t status)
{
	switch (status) {
	case NFS4ERR_STALE_CLIENTID:
	case NFS4ERR_STALE_STATEID:
	case NFS4ERR_BAD_STATEID:
	case NFS4ERR_BAD_SEQID:
	case NFS4ERR_BADXDR:
	case NFS4ERR_RESOURCE:
	case NFS4ERR_NOFILEHANDLE:
	case NFS4ERR_MOVED:
		return 0;
	default:
		return 1;
	}
}

void openmacfuse_seq_record(struct openmacfuse_seq *q, uint32_t seqid, uint32_t op,
		      uint32_t status, const uint8_t *body, uint32_t len,
		      const struct openmacfuse_fh *fh)
{
	q->valid = 1;
	q->seqid = seqid;
	q->op = op;
	q->status = status;
	q->len = 0;
	q->have_fh = 0;
	if (len > OPENMACFUSE_REPLAY_MAX)
		/*
		 * does not fit: a replay returns the status only (cannot happen: results are all
		 * small)
		 */
		len = 0;
	if (len) {
		if (!q->body)
			q->body = malloc(OPENMACFUSE_REPLAY_MAX);
		if (q->body) {
			memcpy(q->body, body, len);
			q->len = len;
		}
	}
	if (fh) {
		if (!q->fh)
			q->fh = malloc(sizeof(*q->fh));
		if (q->fh) {
			*q->fh = *fh;
			q->have_fh = 1;
		}
	}
}

void openmacfuse_seq_free(struct openmacfuse_seq *q)
{
	free(q->body);
	free(q->fh);
	memset(q, 0, sizeof(*q));
}

/* ---------- Client ID ----------
 * A simplified version of RFC 7530 section 16.33.5:
 *   same id + same verifier: keep the clientid and issue a new confirm verifier (the client is
 *   updating its callback address);
 *   same id + new verifier: the client restarted; issue a new clientid and drop the old record on
 *   confirmation;
 *   new id: new record.
 */

static struct openmacfuse_client *client_by_id(struct openmacfuse_server *s,
					const uint8_t *id, uint32_t idlen,
					int confirmed)
{
	int i;

	for (i = 0; i < OPENMACFUSE_MAX_CLIENTS; i++) {
		struct openmacfuse_client *c = &s->clients[i];
		if (c->used && c->confirmed == confirmed && c->idlen == idlen &&
		    memcmp(c->id, id, idlen) == 0)
			return c;
	}
	return NULL;
}

static struct openmacfuse_client *client_by_clientid(struct openmacfuse_server *s,
					      uint64_t clientid)
{
	int i;

	for (i = 0; i < OPENMACFUSE_MAX_CLIENTS; i++)
		if (s->clients[i].used && s->clients[i].clientid == clientid)
			return &s->clients[i];
	return NULL;
}

static struct openmacfuse_client *client_alloc(struct openmacfuse_server *s)
{
	int i, victim = -1;

	for (i = 0; i < OPENMACFUSE_MAX_CLIENTS; i++)
		if (!s->clients[i].used)
			return &s->clients[i];
	/*
	 * table full: evict the unconfirmed record whose lease was renewed longest ago
	 * (practically never happens with a local client)
	 */
	for (i = 0; i < OPENMACFUSE_MAX_CLIENTS; i++)
		if (!s->clients[i].confirmed &&
		    (victim < 0 || s->clients[i].last_renew < s->clients[victim].last_renew))
			victim = i;
	if (victim < 0)
		return NULL;
	memset(&s->clients[victim], 0, sizeof(s->clients[victim]));
	return &s->clients[victim];
}

static void make_confirm(uint8_t out[8])
{
	uint32_t a = rand32(), b = rand32();

	memcpy(out, &a, 4);
	memcpy(out + 4, &b, 4);
}

uint32_t openmacfuse_setclientid(struct openmacfuse_server *s, const uint8_t verf[8],
			  const uint8_t *id, uint32_t idlen,
			  uint64_t *clientid, uint8_t confirm[8])
{
	struct openmacfuse_client *conf = client_by_id(s, id, idlen, 1);
	struct openmacfuse_client *unc = client_by_id(s, id, idlen, 0);
	struct openmacfuse_client *c;

	if (idlen > NFS4_OPAQUE_LIMIT)
		return NFS4ERR_INVAL;
	/* earlier unconfirmed records with the same id are discarded */
	if (unc)
		memset(unc, 0, sizeof(*unc));
	c = client_alloc(s);
	if (!c)
		return NFS4ERR_RESOURCE;
	c->used = 1;
	c->confirmed = 0;
	memcpy(c->verifier, verf, 8);
	memcpy(c->id, id, idlen);
	c->idlen = idlen;
	if (conf && memcmp(conf->verifier, verf, 8) == 0)
		c->clientid = conf->clientid;		/* just a callback update */
	else
		c->clientid = ((uint64_t)s->state_boot << 32) | s->next_clientid++;
	make_confirm(c->confirm);
	c->last_renew = time(NULL);
	*clientid = c->clientid;
	memcpy(confirm, c->confirm, 8);
	return NFS4_OK;
}

uint32_t openmacfuse_setclientid_confirm(struct openmacfuse_server *s, uint64_t clientid,
				  const uint8_t confirm[8])
{
	int i;
	struct openmacfuse_client *c = NULL;

	for (i = 0; i < OPENMACFUSE_MAX_CLIENTS; i++) {
		struct openmacfuse_client *x = &s->clients[i];
		if (x->used && x->clientid == clientid &&
		    memcmp(x->confirm, confirm, 8) == 0) {
			c = x;
			break;
		}
	}
	if (!c)
		return NFS4ERR_STALE_CLIENTID;
	if (c->confirmed)
		return NFS4_OK;		/* a retransmitted confirm */
	/* the old confirmed record with the same id gives way */
	for (i = 0; i < OPENMACFUSE_MAX_CLIENTS; i++) {
		struct openmacfuse_client *x = &s->clients[i];
		if (x != c && x->used && x->confirmed && x->idlen == c->idlen &&
		    memcmp(x->id, c->id, c->idlen) == 0)
			memset(x, 0, sizeof(*x));
	}
	c->confirmed = 1;
	c->last_renew = time(NULL);
	return NFS4_OK;
}

/*
 * The process has "restarted" internally (currently only the test hook does this): when the
 * client presents a clientid from before the restart, accept it as a confirmed client (there is
 * only one local client, so it cannot be someone else). Old stateids still all get STALE_STATEID,
 * so the client still learns that its opens / locks are gone; it just does not have to go through
 * SETCLIENTID again. Reason: after CLAIM_PREVIOUS gets NO_GRACE, the macOS 12 client reopens with
 * CLAIM_NULL, and on receiving STALE_CLIENTID it sends neither RENEW nor SETCLIENTID but retries
 * once a second, leaving the writing process stuck forever (macOS 27 does issue SETCLIENTID and
 * does not have this problem).
 */
static struct openmacfuse_client *client_resurrect(struct openmacfuse_server *s, uint64_t clientid)
{
	uint32_t b = (uint32_t)(clientid >> 32);
	struct openmacfuse_client *c;
	int i;

	for (i = 0; i < 4; i++)
		if (s->prev_boots[i] && s->prev_boots[i] == b)
			break;
	if (i == 4)
		return NULL;
	c = client_alloc(s);
	if (!c)
		return NULL;
	c->used = 1;
	c->confirmed = 1;
	c->clientid = clientid;
	c->last_renew = time(NULL);
	s->resurrected++;
	return c;
}

uint32_t openmacfuse_renew(struct openmacfuse_server *s, uint64_t clientid)
{
	struct openmacfuse_client *c = client_by_clientid(s, clientid);

	if (!c)
		c = client_resurrect(s, clientid);

	if (!c || !c->confirmed)
		return NFS4ERR_STALE_CLIENTID;
	/*
	 * with a single client the lease is never allowed to expire (it just carries on after
	 * sleep / wake); only the time is recorded here
	 */
	c->last_renew = time(NULL);
	return NFS4_OK;
}

uint32_t openmacfuse_check_clientid(struct openmacfuse_server *s, uint64_t clientid)
{
	struct openmacfuse_client *c = client_by_clientid(s, clientid);

	if (!c)
		c = client_resurrect(s, clientid);
	if (!c || !c->confirmed)
		return NFS4ERR_STALE_CLIENTID;
	c->last_renew = time(NULL);	/* RFC: an operation carrying a clientid renews the lease implicitly */
	return NFS4_OK;
}

/* ---------- open-owner ---------- */

int openmacfuse_stateid_is_special(const struct openmacfuse_stateid *sid)
{
	static const uint8_t zero[NFS4_OTHER_SIZE];
	static const uint8_t ones[NFS4_OTHER_SIZE] = {
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

	if (sid->seqid == 0 && memcmp(sid->other, zero, sizeof(zero)) == 0)
		return 1;
	if (sid->seqid == 0xffffffffu && memcmp(sid->other, ones, sizeof(ones)) == 0)
		return 1;
	return 0;
}

struct openmacfuse_owner *openmacfuse_owner_get(struct openmacfuse_server *s, uint64_t clientid,
				  const uint8_t *owner, uint32_t ownerlen,
				  int create)
{
	struct openmacfuse_owner *ow;

	if (ownerlen > NFS4_OPAQUE_LIMIT)
		return NULL;
	for (ow = s->owners; ow; ow = ow->next)
		if (ow->clientid == clientid && ow->ownerlen == ownerlen &&
		    memcmp(ow->owner, owner, ownerlen) == 0) {
			ow->last_used = time(NULL);
			return ow;
		}
	if (!create)
		return NULL;
	ow = calloc(1, sizeof(*ow));
	if (!ow)
		return NULL;
	ow->clientid = clientid;
	memcpy(ow->owner, owner, ownerlen);
	ow->ownerlen = ownerlen;
	ow->last_used = time(NULL);
	ow->next = s->owners;
	s->owners = ow;
	s->nowners++;
	return ow;
}

/*
 * Reclaim: open-owners with no open state that have gone unused for two lease periods;
 * lock-owners with no lock state that have gone unused for two lease periods. An owner has to
 * stay around for a while after its last CLOSE so that a retransmitted CLOSE can still be
 * replayed by seqid. Nothing is scanned while the counts are small (the local client has one
 * open-owner per user and one lock-owner per process).
 */
void openmacfuse_owners_sweep(struct openmacfuse_server *s)
{
	time_t now = time(NULL), idle = 2 * (time_t)s->lease_time;
	struct openmacfuse_owner **pp, *ow;
	struct openmacfuse_lockowner **lpp, *lo;

	if (s->nowners > 64) {
		for (pp = &s->owners; (ow = *pp);) {
			struct openmacfuse_open *op;
			int busy = 0;

			for (op = s->opens; op && !busy; op = op->next)
				busy = op->owner == ow;
			if (!busy && now - ow->last_used > idle) {
				*pp = ow->next;
				openmacfuse_seq_free(&ow->seq);
				free(ow);
				s->nowners--;
				continue;
			}
			pp = &ow->next;
		}
	}
	/* lock states detached from their open state and unused for two lease periods */
	{
		struct openmacfuse_lock *lk, *nx;

		for (lk = s->locks; lk; lk = nx) {
			nx = lk->next;
			if (!lk->open && !lk->ranges && now - lk->detached_at > idle)
				lock_free(s, lk);
		}
	}
	if (s->nlockowners > 64) {
		for (lpp = &s->lockowners; (lo = *lpp);) {
			struct openmacfuse_lock *lk;
			int busy = 0;

			for (lk = s->locks; lk && !busy; lk = lk->next)
				busy = lk->lo == lo;
			if (!busy && now - lo->last_used > idle) {
				*lpp = lo->next;
				openmacfuse_seq_free(&lo->seq);
				free(lo);
				s->nlockowners--;
				continue;
			}
			lpp = &lo->next;
		}
	}
}

/* ---------- Open state ---------- */

static void new_other(struct openmacfuse_server *s, uint8_t other[NFS4_OTHER_SIZE],
		      uint32_t tagbit)
{
	uint64_t n = s->next_other++;

	put_be32(other, s->state_boot);
	put_be32(other + 4, (uint32_t)(n >> 32) | tagbit);
	put_be32(other + 8, (uint32_t)n);
}

struct openmacfuse_open *openmacfuse_open_new(struct openmacfuse_server *s, struct openmacfuse_owner *ow,
				openmacfuse_ino_t ino, uint64_t befh, uint32_t access,
				uint32_t deny)
{
	struct openmacfuse_open *op = calloc(1, sizeof(*op));

	if (!op)
		return NULL;
	op->sid.seqid = 1;
	new_other(s, op->sid.other, 0);
	op->owner = ow;
	op->ino = ino;
	op->befh = befh;
	op->share_access = access;
	op->share_deny = deny;
	op->next = s->opens;
	s->opens = op;
	return op;
}

uint32_t openmacfuse_open_find(struct openmacfuse_server *s, const struct openmacfuse_stateid *sid,
			 struct openmacfuse_open **out)
{
	struct openmacfuse_open *op;

	*out = NULL;
	if (get_be32(sid->other) != s->state_boot)
		return NFS4ERR_STALE_STATEID;
	for (op = s->opens; op; op = op->next)
		if (memcmp(op->sid.other, sid->other, NFS4_OTHER_SIZE) == 0) {
			*out = op;
			return NFS4_OK;
		}
	return NFS4ERR_BAD_STATEID;
}

uint32_t openmacfuse_stateid_check_seq(const struct openmacfuse_stateid *have,
				 const struct openmacfuse_stateid *want)
{
	/*
	 * a seqid of 0 means "the current one": no old / new check (4.0 clients do not normally
	 * send this)
	 */
	if (want->seqid != 0 && want->seqid < have->seqid)
		return NFS4ERR_OLD_STATEID;
	if (want->seqid > have->seqid)
		return NFS4ERR_BAD_STATEID;
	return NFS4_OK;
}

uint32_t openmacfuse_open_lookup(struct openmacfuse_server *s, const struct openmacfuse_stateid *sid,
			  struct openmacfuse_open **out)
{
	uint32_t rc = openmacfuse_open_find(s, sid, out);

	if (rc)
		return rc;
	rc = openmacfuse_stateid_check_seq(&(*out)->sid, sid);
	if (rc)
		*out = NULL;
	return rc;
}

void openmacfuse_open_free(struct openmacfuse_server *s, struct openmacfuse_open *op)
{
	struct openmacfuse_open **pp;

	for (pp = &s->opens; *pp; pp = &(*pp)->next) {
		if (*pp == op) {
			*pp = op->next;
			free(op);
			return;
		}
	}
}

/* ---------- Byte-range locks ---------- */

struct openmacfuse_lockowner *openmacfuse_lockowner_get(struct openmacfuse_server *s,
					  uint64_t clientid,
					  const uint8_t *owner,
					  uint32_t ownerlen, int create)
{
	struct openmacfuse_lockowner *lo;

	if (ownerlen > NFS4_OPAQUE_LIMIT)
		return NULL;
	for (lo = s->lockowners; lo; lo = lo->next)
		if (lo->clientid == clientid && lo->ownerlen == ownerlen &&
		    memcmp(lo->owner, owner, ownerlen) == 0) {
			lo->last_used = time(NULL);
			return lo;
		}
	if (!create)
		return NULL;
	lo = calloc(1, sizeof(*lo));
	if (!lo)
		return NULL;
	lo->clientid = clientid;
	memcpy(lo->owner, owner, ownerlen);
	lo->ownerlen = ownerlen;
	lo->last_used = time(NULL);
	lo->next = s->lockowners;
	s->lockowners = lo;
	s->nlockowners++;
	return lo;
}

struct openmacfuse_lock *openmacfuse_lock_get(struct openmacfuse_server *s, struct openmacfuse_lockowner *lo,
				struct openmacfuse_open *op, openmacfuse_ino_t ino)
{
	struct openmacfuse_lock *lk;

	for (lk = s->locks; lk; lk = lk->next)
		if (lk->lo == lo && lk->ino == ino)
			return lk;
	lk = calloc(1, sizeof(*lk));
	if (!lk)
		return NULL;
	lk->sid.seqid = 0;	/* becomes 1 after the first successful LOCK */
	new_other(s, lk->sid.other, 0x80000000u);	/* the top bit tells lock from open */
	lk->lo = lo;
	lk->open = op;
	lk->ino = ino;
	lk->next = s->locks;
	s->locks = lk;
	return lk;
}

uint32_t openmacfuse_lock_find(struct openmacfuse_server *s, const struct openmacfuse_stateid *sid,
			 struct openmacfuse_lock **out)
{
	struct openmacfuse_lock *lk;

	*out = NULL;
	if (get_be32(sid->other) != s->state_boot)
		return NFS4ERR_STALE_STATEID;
	for (lk = s->locks; lk; lk = lk->next)
		if (memcmp(lk->sid.other, sid->other, NFS4_OTHER_SIZE) == 0) {
			*out = lk;
			return NFS4_OK;
		}
	return NFS4ERR_BAD_STATEID;
}

uint32_t openmacfuse_lock_lookup(struct openmacfuse_server *s, const struct openmacfuse_stateid *sid,
			  struct openmacfuse_lock **out)
{
	uint32_t rc = openmacfuse_lock_find(s, sid, out);

	if (rc)
		return rc;
	rc = openmacfuse_stateid_check_seq(&(*out)->sid, sid);
	if (rc)
		*out = NULL;
	return rc;
}

struct openmacfuse_lockrange *openmacfuse_lock_conflict(struct openmacfuse_server *s, openmacfuse_ino_t ino,
					  struct openmacfuse_lockowner *me, uint64_t off,
					  uint64_t end, uint32_t type,
					  struct openmacfuse_lockowner **holder)
{
	struct openmacfuse_lock *lk;
	struct openmacfuse_lockrange *r;

	for (lk = s->locks; lk; lk = lk->next) {
		if (lk->ino != ino || lk->lo == me)
			continue;
		for (r = lk->ranges; r; r = r->next) {
			if (r->off >= end || off >= r->end)
				continue;	/* no overlap */
			if (r->type == OPENMACFUSE_LT_WRITE || type == OPENMACFUSE_LT_WRITE) {
				*holder = lk->lo;
				return r;
			}
		}
	}
	return NULL;
}

int openmacfuse_lock_remove(struct openmacfuse_lock *lk, uint64_t off, uint64_t end)
{
	struct openmacfuse_lockrange **pp = &lk->ranges, *r;

	while ((r = *pp)) {
		if (r->off >= end || off >= r->end) {
			pp = &r->next;
			continue;
		}
		if (r->off < off && r->end > end) {
			/* cut out of the middle: split into two ranges */
			struct openmacfuse_lockrange *tail = calloc(1, sizeof(*tail));
			if (!tail)
				return -1;
			tail->off = end;
			tail->end = r->end;
			tail->type = r->type;
			tail->next = r->next;
			r->end = off;
			r->next = tail;
			return 0;
		}
		if (r->off < off) {
			r->end = off;		/* keep the left part */
			pp = &r->next;
		} else if (r->end > end) {
			r->off = end;		/* keep the right part */
			pp = &r->next;
		} else {
			*pp = r->next;		/* the whole range lies inside */
			free(r);
		}
	}
	return 0;
}

int openmacfuse_lock_add(struct openmacfuse_lock *lk, uint64_t off, uint64_t end, uint32_t type)
{
	struct openmacfuse_lockrange *r;

	if (openmacfuse_lock_remove(lk, off, end))
		return -1;
	r = calloc(1, sizeof(*r));
	if (!r)
		return -1;
	r->off = off;
	r->end = end;
	r->type = type;
	r->next = lk->ranges;
	lk->ranges = r;
	return 0;
}

static void lock_free(struct openmacfuse_server *s, struct openmacfuse_lock *lk)
{
	struct openmacfuse_lock **pp;

	for (pp = &s->locks; *pp; pp = &(*pp)->next)
		if (*pp == lk) {
			*pp = lk->next;
			break;
		}
	while (lk->ranges) {
		struct openmacfuse_lockrange *r = lk->ranges;
		lk->ranges = r->next;
		free(r);
	}
	free(lk);
}

void openmacfuse_lock_detach_open(struct openmacfuse_server *s, struct openmacfuse_open *op)
{
	struct openmacfuse_lock *lk;

	for (lk = s->locks; lk; lk = lk->next) {
		if (lk->open != op)
			continue;
		while (lk->ranges) {	/* POSIX: closing the file drops this process's locks on it */
			struct openmacfuse_lockrange *r = lk->ranges;
			lk->ranges = r->next;
			free(r);
		}
		lk->open = NULL;
		lk->detached_at = time(NULL);
	}
}

struct openmacfuse_open *openmacfuse_lock_attach(struct openmacfuse_server *s, struct openmacfuse_lock *lk)
{
	struct openmacfuse_open *op, *best = NULL;

	if (lk->open)
		return lk->open;
	for (op = s->opens; op; op = op->next)
		if (openmacfuse_lock_key(op->kind, op->ino, op->attr_id) == lk->ino &&
		    (op->kind == OPENMACFUSE_FH_NODE || op->kind == OPENMACFUSE_FH_NAMEDATTR ||
		     op->kind == OPENMACFUSE_FH_ADFILE) &&
		    op->owner->clientid == lk->lo->clientid &&
		    (!best || ((op->share_access & OPEN4_SHARE_ACCESS_WRITE) &&
			       !(best->share_access & OPEN4_SHARE_ACCESS_WRITE))))
			best = op;
	if (best) {
		lk->open = best;
		lk->detached_at = 0;
	}
	return best;
}

uint32_t openmacfuse_lockowner_release(struct openmacfuse_server *s, uint64_t clientid,
				const uint8_t *owner, uint32_t ownerlen)
{
	struct openmacfuse_lockowner *lo = openmacfuse_lockowner_get(s, clientid, owner,
						       ownerlen, 0), **pp;
	struct openmacfuse_lock *lk, *nx;

	if (!lo)
		return NFS4_OK;
	for (lk = s->locks; lk; lk = lk->next)
		if (lk->lo == lo && lk->ranges)
			return NFS4ERR_LOCKS_HELD;
	for (lk = s->locks; lk; lk = nx) {
		nx = lk->next;
		if (lk->lo == lo)
			lock_free(s, lk);
	}
	for (pp = &s->lockowners; *pp; pp = &(*pp)->next)
		if (*pp == lo) {
			*pp = lo->next;
			break;
		}
	openmacfuse_seq_free(&lo->seq);
	free(lo);
	s->nlockowners--;
	return NFS4_OK;
}

/* ---------- Restart / teardown ---------- */

static void free_all_state(struct openmacfuse_server *s, int release_backend)
{
	while (s->locks)
		lock_free(s, s->locks);
	while (s->opens) {
		struct openmacfuse_open *op = s->opens;
		s->opens = op->next;
		if (release_backend && op->kind == OPENMACFUSE_FH_NODE &&
		    s->be.ops && s->be.ops->release)
			s->be.ops->release(s->be.ctx, op->ino, op->befh);
		free(op);
	}
	while (s->owners) {
		struct openmacfuse_owner *ow = s->owners;
		s->owners = ow->next;
		openmacfuse_seq_free(&ow->seq);
		free(ow);
	}
	while (s->lockowners) {
		struct openmacfuse_lockowner *lo = s->lockowners;
		s->lockowners = lo->next;
		openmacfuse_seq_free(&lo->seq);
		free(lo);
	}
	s->nowners = s->nlockowners = 0;
	memset(s->clients, 0, sizeof(s->clients));
}

void openmacfuse_state_reboot(struct openmacfuse_server *s)
{
	uint32_t nb;

	free_all_state(s, 1);
	memmove(s->prev_boots + 1, s->prev_boots, sizeof(s->prev_boots) - sizeof(s->prev_boots[0]));
	s->prev_boots[0] = s->state_boot;
	do
		nb = rand32() | 1;
	while (nb == s->state_boot || nb == s->boot);
	s->state_boot = nb;
	nb = rand32();
	/* replace the write verifier too: the client must retransmit uncommitted data */
	memcpy(s->writeverf, &nb, 4);
	openmacfuse_drc_free(&s->drc);
	s->reboots++;
}

void openmacfuse_state_free(struct openmacfuse_server *s)
{
	free_all_state(s, 1);	/* files the client never closed (forced unmount etc.) are closed on its behalf here */
	openmacfuse_drc_free(&s->drc);
}

/* ---------- DRC ---------- */

struct openmacfuse_drc_ent {
	struct openmacfuse_drc_ent	*hnext;
	struct openmacfuse_drc_ent	*lprev, *lnext;
	uint32_t		xid;
	uint32_t		len;
	uint64_t		sum;
	uint8_t			reply[];
};

static uint64_t mix64(uint64_t x)
{
	x ^= x >> 33;
	x *= 0xff51afd7ed558ccdull;
	x ^= x >> 33;
	x *= 0xc4ceb9fe1a85ec53ull;
	x ^= x >> 33;
	return x;
}

uint64_t openmacfuse_drc_sum(const uint8_t *msg, size_t len)
{
	/*
	 * skip the xid (first 4 bytes); digest the first 512 bytes of the request + the total
	 * length
	 */
	uint64_t h = 0xcbf29ce484222325ull ^ (uint64_t)len;
	size_t i, n = len < 516 ? len : 516;

	for (i = 4; i < n; i++) {
		h ^= msg[i];
		h *= 0x100000001b3ull;
	}
	return mix64(h);
}

static uint32_t drc_slot(uint32_t xid)
{
	return (uint32_t)mix64(xid) & 511;
}

static void drc_unlink(struct openmacfuse_drc *c, struct openmacfuse_drc_ent *e)
{
	struct openmacfuse_drc_ent **pp = &c->ht[drc_slot(e->xid)];

	while (*pp && *pp != e)
		pp = &(*pp)->hnext;
	if (*pp)
		*pp = e->hnext;
	if (e->lprev)
		e->lprev->lnext = e->lnext;
	else
		c->head = e->lnext;
	if (e->lnext)
		e->lnext->lprev = e->lprev;
	else
		c->tail = e->lprev;
	c->n--;
	c->bytes -= sizeof(*e) + e->len;
}

const uint8_t *openmacfuse_drc_lookup(struct openmacfuse_drc *c, uint32_t xid, uint64_t sum,
				uint32_t *len)
{
	struct openmacfuse_drc_ent *e;

	for (e = c->ht[drc_slot(xid)]; e; e = e->hnext)
		if (e->xid == xid && e->sum == sum) {
			c->hits++;
			*len = e->len;
			return e->reply;
		}
	return NULL;
}

void openmacfuse_drc_insert(struct openmacfuse_drc *c, uint32_t xid, uint64_t sum,
		      const uint8_t *reply, uint32_t len)
{
	struct openmacfuse_drc_ent *e;
	uint32_t slot = drc_slot(xid);

	if (len > OPENMACFUSE_DRC_MAX_REPLY)
		return;
	for (e = c->ht[slot]; e; e = e->hnext)
		if (e->xid == xid && e->sum == sum) {
			drc_unlink(c, e);
			free(e);
			break;
		}
	while (c->tail && (c->n >= OPENMACFUSE_DRC_MAX_ENTS ||
			   c->bytes + sizeof(*e) + len > OPENMACFUSE_DRC_MAX_BYTES)) {
		e = c->tail;
		drc_unlink(c, e);
		free(e);
	}
	e = malloc(sizeof(*e) + len);
	if (!e)
		return;
	e->xid = xid;
	e->sum = sum;
	e->len = len;
	memcpy(e->reply, reply, len);
	e->hnext = c->ht[slot];
	c->ht[slot] = e;
	e->lprev = NULL;
	e->lnext = c->head;
	if (c->head)
		c->head->lprev = e;
	c->head = e;
	if (!c->tail)
		c->tail = e;
	c->n++;
	c->bytes += sizeof(*e) + len;
	c->inserts++;
}

void openmacfuse_drc_free(struct openmacfuse_drc *c)
{
	while (c->head) {
		struct openmacfuse_drc_ent *e = c->head;
		drc_unlink(c, e);
		free(e);
	}
	memset(c->ht, 0, sizeof(c->ht));
}
