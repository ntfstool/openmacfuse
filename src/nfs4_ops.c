/*
 * nfs4_ops.c - NFSv4.0 COMPOUND dispatch and operations (RFC 7530, sections 15 and 16)
 *
 * COMPOUND rules: each operation is executed in order and execution stops at the first failure;
 * the result array contains the operations that were executed (including the failed one), and the
 * overall status equals that of the last result. Operations pass objects to one another through
 * the "current file handle / saved file handle".
 *
 * What is implemented:
 *   - mount handshake, lookup, attributes, directories, open / read / close, symbolic links,
 *     byte-range locks;
 *   - writing (WRITE / COMMIT), creation (the three flavors of OPEN(CREATE) / CREATE), removal,
 *     rename, hard links, SETATTR, OPEN_CONFIRM / OPEN_DOWNGRADE, share reservations (share
 *     access / deny), seqid checking and replay for open-owners / lock-owners.
 *     When the backend is read-only, every modifying operation gets NFS4ERR_ROFS.
 *   - no delegations are handed out; there is no grace period: CLAIM_PREVIOUS / reclaim get
 *     NFS4ERR_NO_GRACE.
 *   - named attributes (OPENATTR, and LOOKUP / READDIR / OPEN / READ / WRITE / CREATE /
 *     REMOVE / GETATTR / SETATTR / COMMIT under the attribute directory), active when
 *     s->named_attr is set (nfuse_xattr=streams); attribute values are read and written by
 *     offset (backend xattr_read / xattr_write) rather than fetched whole.
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>

#include "adfile.h"
#include "nfs4_attr.h"
#include "nfs4_ops.h"
#include "watchdog.h"

#define DROP_OPEN_EXCL	(NFS4_OP_MAX + 1)	/* slot of OPEN(EXCLUSIVE4) in test_drop */

/* Execution context of one COMPOUND */
struct cctx {
	struct nfuse_server	*s;
	const rpc_cred_t	*cred;
	struct nfuse_fh		cur;
	struct nfuse_fh		saved;
	int			have_cur;
	int			have_saved;
	int			keep_err_body;	/* on error the op wrote its own error body (e.g. CLID_INUSE) */
	/*
	 * seqid bookkeeping: when this op finishes, its result is recorded in the replay caches
	 * of these owners
	 */
	int			nrec;
	struct nfuse_seq	*rec[2];
	uint32_t		rec_seqid[2];
	int			rec_has_other;
	uint8_t			rec_other[NFS4_OTHER_SIZE];
	int			replayed;	/* this op's result is a replay; do not record it again */
	int			open_excl;	/* this op is an OPEN(EXCLUSIVE4) (for the test hook) */
	unsigned		flags;		/* NFUSE_CF_* */
};

static const char *op_names[] = {
	[OP_ACCESS] = "ACCESS", [OP_CLOSE] = "CLOSE", [OP_COMMIT] = "COMMIT",
	[OP_CREATE] = "CREATE", [OP_DELEGPURGE] = "DELEGPURGE",
	[OP_DELEGRETURN] = "DELEGRETURN", [OP_GETATTR] = "GETATTR",
	[OP_GETFH] = "GETFH", [OP_LINK] = "LINK", [OP_LOCK] = "LOCK",
	[OP_LOCKT] = "LOCKT", [OP_LOCKU] = "LOCKU", [OP_LOOKUP] = "LOOKUP",
	[OP_LOOKUPP] = "LOOKUPP", [OP_NVERIFY] = "NVERIFY", [OP_OPEN] = "OPEN",
	[OP_OPENATTR] = "OPENATTR", [OP_OPEN_CONFIRM] = "OPEN_CONFIRM",
	[OP_OPEN_DOWNGRADE] = "OPEN_DOWNGRADE", [OP_PUTFH] = "PUTFH",
	[OP_PUTPUBFH] = "PUTPUBFH", [OP_PUTROOTFH] = "PUTROOTFH",
	[OP_READ] = "READ", [OP_READDIR] = "READDIR", [OP_READLINK] = "READLINK",
	[OP_REMOVE] = "REMOVE", [OP_RENAME] = "RENAME", [OP_RENEW] = "RENEW",
	[OP_RESTOREFH] = "RESTOREFH", [OP_SAVEFH] = "SAVEFH",
	[OP_SECINFO] = "SECINFO", [OP_SETATTR] = "SETATTR",
	[OP_SETCLIENTID] = "SETCLIENTID",
	[OP_SETCLIENTID_CONFIRM] = "SETCLIENTID_CONFIRM", [OP_VERIFY] = "VERIFY",
	[OP_WRITE] = "WRITE", [OP_RELEASE_LOCKOWNER] = "RELEASE_LOCKOWNER",
};

/*
 * Non-idempotent ops: a COMPOUND containing any of them goes into the DRC (WRITE / COMMIT are
 * idempotent and carry a lot of data, so they do not)
 */
static const uint8_t op_nonidem[NFS4_OP_MAX + 1] = {
	[OP_OPEN] = 1, [OP_OPEN_CONFIRM] = 1, [OP_OPEN_DOWNGRADE] = 1,
	[OP_CLOSE] = 1, [OP_CREATE] = 1, [OP_REMOVE] = 1, [OP_RENAME] = 1,
	[OP_LINK] = 1, [OP_SETATTR] = 1, [OP_LOCK] = 1, [OP_LOCKU] = 1,
	[OP_RELEASE_LOCKOWNER] = 1,
};

const char *nfs4_op_name(uint32_t op)
{
	if (op >= NFS4_OP_MIN && op <= NFS4_OP_MAX && op_names[op])
		return op_names[op];
	return "ILLEGAL";
}

int nfs4_op_code(const char *name)
{
	uint32_t i;

	for (i = NFS4_OP_MIN; i <= NFS4_OP_MAX; i++)
		if (op_names[i] && strcmp(op_names[i], name) == 0)
			return (int)i;
	return -1;
}

/* ---------- Common helpers ---------- */

static int is_pseudo(const struct nfuse_fh *fh)
{
	return fh->kind == NFUSE_FH_PSEUDOROOT;
}

static int be_writable(struct cctx *c)
{
	return !c->s->be.readonly;
}

/* Attribute bitmap requested by the client -> whether getxtimes needs to be called */
static unsigned want_of(const uint32_t req[NFS4_BITMAP_WORDS])
{
	unsigned w = NFUSE_WANT_BASIC;

	if (req[1] & ((1u << (FATTR4_TIME_BACKUP - 32)) |
		      (1u << (FATTR4_TIME_CREATE - 32))))
		w |= NFUSE_WANT_XTIMES;
	return w;
}

static void bm_set(uint32_t *w, int n)
{
	w[n / 32] |= 1u << (n % 32);
}

/* ---------- Named attributes ---------- */

#define ATTR_NAMES_MAX	65536

static uint32_t attr_hash(const char *name)
{
	uint32_t h = 2166136261u;

	while (*name)
		h = (h ^ (uint8_t)*name++) * 16777619u;
	return h;
}

static void attr_ht_put(struct nfuse_server *s, uint32_t id)
{
	uint32_t m = s->attr_htcap - 1, h = attr_hash(s->attr_names[id]) & m;

	while (s->attr_ht[h])
		h = (h + 1) & m;
	s->attr_ht[h] = id + 1;
}

/*
 * Attribute name -> id (handles carry the id; grow-only, at most 65536 distinct names, looked up
 * through a hash)
 */
static int attr_intern(struct nfuse_server *s, const char *name, nfuse_ino_t *id)
{
	uint32_t i, m, h;

	if (s->attr_htcap) {
		m = s->attr_htcap - 1;
		for (h = attr_hash(name) & m; s->attr_ht[h]; h = (h + 1) & m)
			if (strcmp(s->attr_names[s->attr_ht[h] - 1], name) == 0) {
				*id = s->attr_ht[h] - 1;
				return 0;
			}
	}
	if (s->nattr_names >= ATTR_NAMES_MAX)
		return -ENOMEM;
	if ((s->nattr_names + 1) * 2 > s->attr_htcap) {
		uint32_t nc = s->attr_htcap ? s->attr_htcap * 2 : 256;
		uint32_t *nt = calloc(nc, sizeof(*nt));

		if (!nt)
			return -ENOMEM;
		free(s->attr_ht);
		s->attr_ht = nt;
		s->attr_htcap = nc;
		for (i = 0; i < s->nattr_names; i++)
			attr_ht_put(s, i);
	}
	if (s->nattr_names == s->attr_names_cap) {
		uint32_t nc = s->attr_names_cap ? s->attr_names_cap * 2 : 64;
		char **n = realloc(s->attr_names, nc * sizeof(*n));
		if (!n)
			return -ENOMEM;
		s->attr_names = n;
		s->attr_names_cap = nc;
	}
	s->attr_names[s->nattr_names] = strdup(name);
	if (!s->attr_names[s->nattr_names])
		return -ENOMEM;
	*id = s->nattr_names++;
	attr_ht_put(s, (uint32_t)*id);
	return 0;
}

/*
 * Named attributes are writable: the backend has the write callbacks and the volume is not
 * read-only
 */
static int xattr_writable(struct nfuse_server *s)
{
	return !s->be.readonly && s->be.ops->xattr_write;
}

static const char *attr_name(struct nfuse_server *s, nfuse_ino_t id)
{
	return id < s->nattr_names ? s->attr_names[id] : NULL;
}

/* fileid of a named attribute object: kept clear of real inode numbers */
#define ATTRDIR_FILEID(ino)		(0x4000000000000000ull | (ino))
#define NAMEDATTR_FILEID(ino, id)	(0x6000000000000000ull | ((uint64_t)(id) << 40) | ((ino) & 0xffffffffffull))

/*
 * Get the attributes of the current object (synthesized separately for the pseudo root and for
 * named attribute directories / attributes)
 */
static uint32_t fh_attr(struct cctx *c, const struct nfuse_fh *fh,
			struct nfuse_attr *st, unsigned want)
{
	int rc;

	if (is_pseudo(fh)) {
		nfuse_attr_pseudoroot(c->s, st);
		return NFS4_OK;
	}
	rc = c->s->be.ops->getattr(c->s->be.ctx, fh->ino, st, want);
	if (rc == -ENOENT)
		return NFS4ERR_STALE;	/* the object the handle refers to no longer exists */
	if (rc)
		return nfuse_errno_to_nfs4(rc);
	if (fh->kind == NFUSE_FH_ATTRDIR) {
		st->ino = ATTRDIR_FILEID(fh->ino);
		st->mode = S_IFDIR | (xattr_writable(c->s) ? 0755 : 0555);
		st->nlink = 2;
		st->size = 0;
		st->used = 0;
		st->nf4type = NF4ATTRDIR;
	} else if (fh->kind == NFUSE_FH_ADFILE) {
		rc = nfuse_adf_attr(c->s, fh->ino, st);
		if (rc)
			return rc == -ENOENT ? NFS4ERR_STALE : nfuse_errno_to_nfs4(rc);
	} else if (fh->kind == NFUSE_FH_NAMEDATTR) {
		const char *nm = attr_name(c->s, fh->attr_id);
		if (!nm || !c->s->be.ops->getxattr)
			return NFS4ERR_STALE;
		rc = c->s->be.ops->getxattr(c->s->be.ctx, fh->ino, nm, NULL, 0);
		if (rc < 0)
			return rc == -ENOATTR ? NFS4ERR_STALE : nfuse_errno_to_nfs4(rc);
		st->ino = NAMEDATTR_FILEID(fh->ino, fh->attr_id);
		st->mode = S_IFREG | (xattr_writable(c->s) ? 0644 : 0444);
		st->nlink = 1;
		st->size = (uint64_t)rc;
		st->used = (uint64_t)rc;
		st->nf4type = NF4NAMEDATTR;
	}
	return NFS4_OK;
}

static uint32_t cur_attr(struct cctx *c, const struct nfuse_fh *fh,
			 struct nfuse_attr *st)
{
	return fh_attr(c, fh, st, NFUSE_WANT_BASIC);
}

/*
 * Handle of an ordinary node: the ancestor chain is obtained from the backend; parent is only a
 * fallback for when the node is not in the table
 */
static void set_node_fh(struct cctx *c, struct nfuse_fh *fh, nfuse_ino_t ino,
			nfuse_ino_t parent)
{
	int n = 0;

	memset(fh, 0, sizeof(*fh));
	fh->kind = NFUSE_FH_NODE;
	fh->boot = c->s->boot;
	fh->ino = ino;
	if (c->s->be.ops->ancestry)
		n = c->s->be.ops->ancestry(c->s->be.ctx, ino, parent, fh->anc,
					   NFUSE_FH_MAXANC);
	fh->nanc = (uint8_t)(n > 0 ? n : 0);
}

static void set_pseudo_fh(struct cctx *c, struct nfuse_fh *fh)
{
	memset(fh, 0, sizeof(*fh));
	fh->kind = NFUSE_FH_PSEUDOROOT;
	fh->boot = c->s->boot;
}

/*
 * Decode a component4 (file name), apply the checks the RFC requires, and copy the result into
 * out (NUL-terminated).
 * Empty name -> INVAL; "." / ".." -> BADNAME; contains '/' or NUL -> BADCHAR; longer than 255
 * bytes -> NAMETOOLONG.
 */
static uint32_t get_component(xdr_dec_t *d, char *out, size_t outsz)
{
	uint32_t len;
	const uint8_t *p = xdr_get_opaque_ref(d, 4096, &len);

	out[0] = 0;
	if (d->err)
		return NFS4ERR_BADXDR;
	if (len == 0)
		return NFS4ERR_INVAL;
	if (len > 255 || len >= outsz)
		return NFS4ERR_NAMETOOLONG;
	if (memchr(p, '/', len) || memchr(p, 0, len))
		return NFS4ERR_BADCHAR;
	memcpy(out, p, len);
	out[len] = 0;
	if (strcmp(out, ".") == 0 || strcmp(out, "..") == 0)
		return NFS4ERR_BADNAME;
	return NFS4_OK;
}

/*
 * The current handle must be a directory (named attribute directories included): a symbolic link
 * gets SYMLINK, anything else NOTDIR
 */
static uint32_t need_dir_fh(struct cctx *c, const struct nfuse_fh *fh,
			    struct nfuse_attr *st)
{
	uint32_t rc = cur_attr(c, fh, st);

	if (rc)
		return rc;
	if (S_ISDIR(st->mode))
		return NFS4_OK;
	return S_ISLNK(st->mode) ? NFS4ERR_SYMLINK : NFS4ERR_NOTDIR;
}

static uint32_t need_dir(struct cctx *c, struct nfuse_attr *st)
{
	if (!c->have_cur)
		return NFS4ERR_NOFILEHANDLE;
	return need_dir_fh(c, &c->cur, st);
}

/* Look up a name under directory dirfh (handles the pseudo root and named attribute directories) */
static uint32_t lookup_name(struct cctx *c, const struct nfuse_fh *dirfh,
			    const char *name, struct nfuse_fh *out)
{
	nfuse_ino_t ino;
	int rc;

	if (is_pseudo(dirfh)) {
		if (strcmp(name, c->s->export_name) != 0)
			return NFS4ERR_NOENT;
		set_node_fh(c, out, c->s->be.ops->root(c->s->be.ctx), 0);
		return NFS4_OK;
	}
	if (dirfh->kind == NFUSE_FH_ATTRDIR) {
		if (!c->s->be.ops->getxattr)
			return NFS4ERR_NOENT;
		rc = c->s->be.ops->getxattr(c->s->be.ctx, dirfh->ino, name, NULL, 0);
		if (rc < 0) {
			if (c->s->log && c->s->verbose)
				c->s->log("named attribute %s not found: errno %d", name, -rc);
			/*
			 * no such attribute: the file system may return ENOATTR / ENODATA /
			 * ENOTSUP; all are treated as nonexistent
			 */
			return rc == -EACCES || rc == -EPERM ? NFS4ERR_ACCESS :
			       NFS4ERR_NOENT;
		}
		if (attr_intern(c->s, name, &ino))
			return NFS4ERR_RESOURCE;
		memset(out, 0, sizeof(*out));
		out->kind = NFUSE_FH_NAMEDATTR;
		out->boot = c->s->boot;
		out->ino = dirfh->ino;
		out->attr_id = ino;
		/*
		 * carry along the owning file's ancestor chain (so PUTFH can still find it after
		 * the node has been evicted)
		 */
		out->nanc = dirfh->nanc > NFUSE_FH_MAXANC - 1 ? NFUSE_FH_MAXANC - 1 : dirfh->nanc;
		memcpy(out->anc, dirfh->anc, sizeof(out->anc[0]) * out->nanc);
		return NFS4_OK;
	}
	if (c->s->adfile && dirfh->kind == NFUSE_FH_NODE && nfuse_adf_target(name)) {
		/*
		 * `._X`: X exists and has extended attributes -> virtual file; X exists but has
		 * no attributes -> does not exist (unless there is a real file on disk that is
		 * not AppleDouble)
		 */
		nfuse_ino_t x;
		int ex;
		int lr = nfuse_adf_lookup(c->s, dirfh->ino, name, &x, &ex);

		if (c->s->verbose && c->s->log)
			c->s->log("LOOKUP virtual %s: %d exists=%d", name, lr, ex);
		if (lr == 0 && ex) {
			set_node_fh(c, out, x, dirfh->ino);
			out->kind = NFUSE_FH_ADFILE;
			return NFS4_OK;
		}
	}
	rc = c->s->be.ops->lookup(c->s->be.ctx, dirfh->ino, name, &ino);
	if (rc)
		return nfuse_errno_to_nfs4(rc);
	set_node_fh(c, out, ino, dirfh->ino);
	return NFS4_OK;
}

/*
 * Whether the virtual `._X` still has any open state. While writing an AppleDouble the client
 * issues COMMIT part-way through (observed: 4 WRITEs, COMMIT, 2 more WRITEs, COMMIT), at which
 * point the header and the resource fork may be only half written: parse and write back only when
 * the last open state is closed.
 */
static int adf_opens(struct nfuse_server *s, nfuse_ino_t x)
{
	struct nfuse_open *op;
	int n = 0;

	for (op = s->opens; op; op = op->next)
		if (op->kind == NFUSE_FH_ADFILE && op->ino == x)
			n++;
	return n;
}

/*
 * REMOVE / RENAME / CREATE on a `._X`: returns 1 if X exists (*x is X), to be handled as a
 * virtual file
 */
static int adf_name(struct cctx *c, const struct nfuse_fh *dirfh, const char *name,
		    nfuse_ino_t *x, int *exists)
{
	if (!c->s->adfile || dirfh->kind != NFUSE_FH_NODE || !nfuse_adf_target(name))
		return 0;
	return nfuse_adf_lookup(c->s, dirfh->ino, name, x, exists) == 0;
}

static void put_stateid(xdr_enc_t *e, const struct nfuse_stateid *sid)
{
	xdr_put_u32(e, sid->seqid);
	xdr_put_fixed(e, sid->other, NFS4_OTHER_SIZE);
}

static void get_stateid(xdr_dec_t *d, struct nfuse_stateid *sid)
{
	sid->seqid = xdr_get_u32(d);
	xdr_get_fixed(d, sid->other, NFS4_OTHER_SIZE);
}

/*
 * change_info4: all COMPOUNDs run serially under one lock, so nothing else modifies the directory
 * between the before and after values and the operation; atomic is true
 */
static void put_change_info(xdr_enc_t *e, uint64_t before, uint64_t after)
{
	xdr_put_bool(e, 1);
	xdr_put_u64(e, before);
	xdr_put_u64(e, after);
}

/*
 * The directory's current change value (it was just modified and the backend has invalidated its
 * cache, so this fetches the new value)
 */
static uint64_t dir_change(struct cctx *c, const struct nfuse_fh *dir)
{
	struct nfuse_attr st;

	if (cur_attr(c, dir, &st))
		return 0;
	return st.change;
}

/* ---------- seqid replay ---------- */

static uint32_t seq_replay(struct cctx *c, struct nfuse_seq *q, xdr_enc_t *e)
{
	if (q->len)
		xdr_put_fixed(e, q->body, q->len);
	c->keep_err_body = 1;
	if (q->have_fh && q->fh) {
		c->cur = *q->fh;
		c->have_cur = 1;
	}
	c->replayed = 1;
	c->s->seq_replays++;
	if (c->s->log && c->s->verbose)
		c->s->log("seqid replay: %s seqid=%u -> %u", nfs4_op_name(q->op),
			  q->seqid, q->status);
	return q->status;
}

/* When this op finishes, record its result in the owner's replay cache */
static void seq_will_record(struct cctx *c, struct nfuse_seq *q, uint32_t seqid)
{
	if (c->nrec < 2) {
		c->rec[c->nrec] = q;
		c->rec_seqid[c->nrec] = seqid;
		c->nrec++;
	}
}

/*
 * An unconfirmed open-owner receives a new OPEN: treat it as a new owner and discard its earlier
 * open state
 */
static void owner_reset(struct cctx *c, struct nfuse_owner *ow)
{
	struct nfuse_open *op, *nx;

	for (op = c->s->opens; op; op = nx) {
		nx = op->next;
		if (op->owner != ow)
			continue;
		if (op->kind == NFUSE_FH_NODE && c->s->be.ops->release)
			c->s->be.ops->release(c->s->be.ctx, op->ino, op->befh);
		nfuse_lock_detach_open(c->s, op);
		nfuse_open_free(c->s, op);
	}
	ow->seq.valid = 0;
}

/* ---------- Decoding settable attributes (fattr4): SETATTR / CREATE / OPEN(CREATE) ---------- */

static uint32_t get_nfstime(xdr_dec_t *d, struct timespec *t)
{
	int64_t sec = (int64_t)xdr_get_u64(d);
	uint32_t ns = xdr_get_u32(d);

	if (ns >= 1000000000u)
		return NFS4ERR_INVAL;
	t->tv_sec = (time_t)sec;
	t->tv_nsec = (long)ns;
	return NFS4_OK;
}

/*
 * Owner string: purely numeric (what the client sends under AUTH_SYS), or name[@domain] looked up
 * in the local account database
 */
static uint32_t get_owner_id(xdr_dec_t *d, int group, uint32_t *out)
{
	uint32_t len, i;
	const uint8_t *p = xdr_get_opaque_ref(d, 1024, &len);
	char buf[256], *at;
	int digits = 1;

	if (d->err)
		return NFS4ERR_BADXDR;
	if (len == 0 || len >= sizeof(buf))
		return NFS4ERR_BADOWNER;
	memcpy(buf, p, len);
	buf[len] = 0;
	for (i = 0; i < len; i++)
		if (buf[i] < '0' || buf[i] > '9')
			digits = 0;
	if (digits) {
		*out = (uint32_t)strtoul(buf, NULL, 10);
		return NFS4_OK;
	}
	at = strchr(buf, '@');
	if (at)
		*at = 0;
	if (group) {
		struct group *g = getgrnam(buf);
		if (!g)
			return NFS4ERR_BADOWNER;
		*out = (uint32_t)g->gr_gid;
	} else {
		struct passwd *pw = getpwnam(buf);
		if (!pw)
			return NFS4ERR_BADOWNER;
		*out = (uint32_t)pw->pw_uid;
	}
	return NFS4_OK;
}

/*
 * Decode the attributes to be set from a fattr4; set receives the accepted bits.
 * Read-only attribute -> INVAL; unsupported -> ATTRNOTSUPP; parse error -> BADXDR.
 */
static uint32_t get_sattr(xdr_dec_t *d, struct nfuse_sattr *sa,
			  uint32_t set[NFS4_BITMAP_WORDS])
{
	uint32_t bm[NFS4_BITMAP_WORDS], sup[NFS4_BITMAP_WORDS], alen, rc = NFS4_OK;
	const uint8_t *ap;
	xdr_dec_t a;
	int i;

	memset(sa, 0, sizeof(*sa));
	memset(set, 0, sizeof(uint32_t) * NFS4_BITMAP_WORDS);
	xdr_get_bitmap(d, bm, NFS4_BITMAP_WORDS);
	ap = xdr_get_opaque_ref(d, 65536, &alen);
	if (d->err)
		return NFS4ERR_BADXDR;
	nfuse_attr_supported(sup);
	xdr_dec_init(&a, ap, alen);
	for (i = 0; i < 32 * NFS4_BITMAP_WORDS && rc == NFS4_OK; i++) {
		uint32_t v, how;

		if (!(bm[i / 32] & (1u << (i % 32))))
			continue;
		switch (i) {
		case FATTR4_SIZE:
			sa->size = xdr_get_u64(&a);
			sa->mask |= NFUSE_SA_SIZE;
			break;
		case FATTR4_MODE:
			sa->mode = xdr_get_u32(&a) & 07777;
			sa->mask |= NFUSE_SA_MODE;
			break;
		case FATTR4_OWNER:
			rc = get_owner_id(&a, 0, &sa->uid);
			sa->mask |= NFUSE_SA_UID;
			break;
		case FATTR4_OWNER_GROUP:
			rc = get_owner_id(&a, 1, &sa->gid);
			sa->mask |= NFUSE_SA_GID;
			break;
		case FATTR4_TIME_ACCESS_SET:
		case FATTR4_TIME_MODIFY_SET: {
			int isa = i == FATTR4_TIME_ACCESS_SET;

			how = xdr_get_u32(&a);
			if (how == 1) {		/* SET_TO_CLIENT_TIME4 */
				rc = get_nfstime(&a, isa ? &sa->atime : &sa->mtime);
				sa->mask |= isa ? NFUSE_SA_ATIME : NFUSE_SA_MTIME;
			} else if (how == 0) {	/* SET_TO_SERVER_TIME4 */
				sa->mask |= isa ? NFUSE_SA_ATIME_NOW : NFUSE_SA_MTIME_NOW;
			} else {
				rc = NFS4ERR_BADXDR;
			}
			break;
		}
		case FATTR4_TIME_CREATE:
			rc = get_nfstime(&a, &sa->crtime);
			sa->mask |= NFUSE_SA_CRTIME;
			break;
		case FATTR4_TIME_BACKUP:
			rc = get_nfstime(&a, &sa->bkuptime);
			sa->mask |= NFUSE_SA_BKUPTIME;
			break;
		case FATTR4_HIDDEN:
		case FATTR4_ARCHIVE: {
			uint32_t bit = i == FATTR4_HIDDEN ? UF_HIDDEN : SF_ARCHIVED;

			v = xdr_get_u32(&a);
			if (v)
				sa->flags_set |= bit;
			else
				sa->flags_clear |= bit;
			sa->mask |= NFUSE_SA_FLAGS;
			break;
		}
		case FATTR4_SYSTEM:
			v = xdr_get_u32(&a);
			if (v)
				rc = NFS4ERR_ATTRNOTSUPP;	/* no corresponding BSD flag */
			break;
		case FATTR4_ACL:
		case FATTR4_MIMETYPE:
		case FATTR4_QUOTA_AVAIL_HARD:
		case FATTR4_QUOTA_AVAIL_SOFT:
		case FATTR4_QUOTA_USED:
		case FATTR4_FS_LOCATIONS:
			rc = NFS4ERR_ATTRNOTSUPP;
			break;
		default:
			/*
			 * the ones we support for reading are read-only attributes and cannot be
			 * set; unknown ones count as unsupported
			 */
			rc = (i <= FATTR4_MAX_ATTR && (sup[i / 32] & (1u << (i % 32)))) ?
			     NFS4ERR_INVAL : NFS4ERR_ATTRNOTSUPP;
			break;
		}
		if (a.err)
			rc = NFS4ERR_BADXDR;
		if (rc == NFS4_OK)
			bm_set(set, i);
	}
	return rc;
}

/* Attribute bits corresponding to a sattr (the attrsset returned to the client on success) */
static void sattr_bits(const struct nfuse_sattr *sa, uint32_t out[NFS4_BITMAP_WORDS])
{
	memset(out, 0, sizeof(uint32_t) * NFS4_BITMAP_WORDS);
	if (sa->mask & NFUSE_SA_SIZE)
		bm_set(out, FATTR4_SIZE);
	if (sa->mask & NFUSE_SA_MODE)
		bm_set(out, FATTR4_MODE);
	if (sa->mask & NFUSE_SA_UID)
		bm_set(out, FATTR4_OWNER);
	if (sa->mask & NFUSE_SA_GID)
		bm_set(out, FATTR4_OWNER_GROUP);
	if (sa->mask & (NFUSE_SA_ATIME | NFUSE_SA_ATIME_NOW))
		bm_set(out, FATTR4_TIME_ACCESS_SET);
	if (sa->mask & (NFUSE_SA_MTIME | NFUSE_SA_MTIME_NOW))
		bm_set(out, FATTR4_TIME_MODIFY_SET);
	if (sa->mask & NFUSE_SA_CRTIME)
		bm_set(out, FATTR4_TIME_CREATE);
	if (sa->mask & NFUSE_SA_BKUPTIME)
		bm_set(out, FATTR4_TIME_BACKUP);
	if (sa->mask & NFUSE_SA_FLAGS) {
		if ((sa->flags_set | sa->flags_clear) & UF_HIDDEN)
			bm_set(out, FATTR4_HIDDEN);
		if ((sa->flags_set | sa->flags_clear) & SF_ARCHIVED)
			bm_set(out, FATTR4_ARCHIVE);
	}
}

/*
 * Error from the backend setattr: when an attribute is not supported answer ATTRNOTSUPP (the
 * client reports ENOTSUP)
 */
static uint32_t setattr_err(int r)
{
	if (r == -ENOTSUP || r == -EOPNOTSUPP)
		return NFS4ERR_ATTRNOTSUPP;
	return nfuse_errno_to_nfs4(r);
}

/* ---------- File handle operations ---------- */

static uint32_t op_putrootfh(struct cctx *c, xdr_dec_t *d, xdr_enc_t *e)
{
	(void)d; (void)e;
	/*
	 * with an empty export name the backend root directory is the root itself (mount host:/);
	 * otherwise go to the pseudo root first
	 */
	if (c->s->export_name[0])
		set_pseudo_fh(c, &c->cur);
	else
		set_node_fh(c, &c->cur, c->s->be.ops->root(c->s->be.ctx), 0);
	c->have_cur = 1;
	return NFS4_OK;
}

static uint32_t op_putfh(struct cctx *c, xdr_dec_t *d, xdr_enc_t *e)
{
	uint32_t len, rc;
	const uint8_t *p = xdr_get_opaque_ref(d, NFS4_FHSIZE, &len);

	(void)e;
	if (d->err)
		return NFS4ERR_BADXDR;
	rc = nfuse_fh_decode(c->s, p, len, &c->cur);
	if (rc) {
		c->have_cur = 0;
		if (rc == NFS4ERR_STALE)
			c->s->stale_fh++;
		return rc;
	}
	if (c->cur.kind != NFUSE_FH_PSEUDOROOT && c->s->be.ops->resolve) {
		/*
		 * the node table may have evicted this inode: find it again along the ancestor
		 * chain in the handle; only if that fails is it STALE
		 */
		int r = c->s->be.ops->resolve(c->s->be.ctx, c->cur.ino,
				c->cur.anc, c->cur.nanc);
		if (r) {
			c->have_cur = 0;
			c->s->stale_fh++;
			return r == -ESTALE || r == -ENOENT ? NFS4ERR_STALE :
			       nfuse_errno_to_nfs4(r);
		}
	}
	c->have_cur = 1;
	return NFS4_OK;
}

static uint32_t op_getfh(struct cctx *c, xdr_dec_t *d, xdr_enc_t *e)
{
	uint8_t raw[NFUSE_FH_MAXLEN];
	uint32_t len;

	(void)d;
	if (!c->have_cur)
		return NFS4ERR_NOFILEHANDLE;
	len = nfuse_fh_encode(&c->cur, raw);
	xdr_put_opaque(e, raw, len);
	return NFS4_OK;
}

static uint32_t op_savefh(struct cctx *c, xdr_dec_t *d, xdr_enc_t *e)
{
	(void)d; (void)e;
	if (!c->have_cur)
		return NFS4ERR_NOFILEHANDLE;
	c->saved = c->cur;
	c->have_saved = 1;
	return NFS4_OK;
}

static uint32_t op_restorefh(struct cctx *c, xdr_dec_t *d, xdr_enc_t *e)
{
	(void)d; (void)e;
	if (!c->have_saved)
		return NFS4ERR_RESTOREFH;
	c->cur = c->saved;
	c->have_cur = 1;
	return NFS4_OK;
}

static uint32_t op_lookup(struct cctx *c, xdr_dec_t *d, xdr_enc_t *e)
{
	char name[256];
	struct nfuse_attr st;
	struct nfuse_fh nfh;
	uint32_t rc = get_component(d, name, sizeof(name));

	(void)e;
	if (rc == NFS4ERR_BADXDR)
		return rc;
	if (!c->have_cur)
		return NFS4ERR_NOFILEHANDLE;
	if (rc)
		return rc;
	rc = need_dir(c, &st);
	if (rc)
		return rc;
	rc = lookup_name(c, &c->cur, name, &nfh);
	if (c->s->verbose && c->s->log)
		c->s->log("LOOKUP %s -> %u", name, rc);
	if (rc)
		return rc;
	c->cur = nfh;
	return NFS4_OK;
}

static uint32_t op_lookupp(struct cctx *c, xdr_dec_t *d, xdr_enc_t *e)
{
	struct nfuse_attr st;
	nfuse_ino_t ino, gp = 0, root;
	uint32_t rc;
	int r;

	(void)d; (void)e;
	rc = need_dir(c, &st);
	if (rc)
		return rc;
	if (is_pseudo(&c->cur))
		return NFS4ERR_NOENT;
	if (c->cur.kind == NFUSE_FH_ATTRDIR) {
		/* the "parent" of a named attribute directory is the file it belongs to */
		ino = c->cur.ino;
		if (c->s->be.ops->lookupp(c->s->be.ctx, ino, &gp))
			gp = 0;
		set_node_fh(c, &c->cur, ino, gp);
		return NFS4_OK;
	}
	root = c->s->be.ops->root(c->s->be.ctx);
	if (c->cur.ino == root) {
		if (!c->s->export_name[0])
			return NFS4ERR_NOENT;
		set_pseudo_fh(c, &c->cur);
		return NFS4_OK;
	}
	r = c->s->be.ops->lookupp(c->s->be.ctx, c->cur.ino, &ino);
	if (r)
		return nfuse_errno_to_nfs4(r);
	if (ino != root && c->s->be.ops->lookupp(c->s->be.ctx, ino, &gp))
		gp = 0;
	set_node_fh(c, &c->cur, ino, gp);
	return NFS4_OK;
}

/*
 * OPENATTR: enter the named attribute directory of the current object. The attribute directory
 * always "exists" (an empty one simply means no attributes), so the result is the same with or
 * without createdir; the handle keeps the owning file's ancestor chain.
 */
static uint32_t op_openattr(struct cctx *c, xdr_dec_t *d, xdr_enc_t *e)
{
	int createdir = xdr_get_bool(d);

	(void)e; (void)createdir;
	if (d->err)
		return NFS4ERR_BADXDR;
	if (!c->have_cur)
		return NFS4ERR_NOFILEHANDLE;
	if (!c->s->named_attr || !c->s->be.ops->listxattr)
		return NFS4ERR_NOTSUPP;
	if (c->cur.kind != NFUSE_FH_NODE)
		return NFS4ERR_INVAL;
	c->cur.kind = NFUSE_FH_ATTRDIR;
	c->cur.attr_id = 0;
	c->s->namedattr_ops++;
	return NFS4_OK;
}

/* ---------- Attributes ---------- */

static uint32_t op_getattr(struct cctx *c, xdr_dec_t *d, xdr_enc_t *e)
{
	uint32_t req[NFS4_BITMAP_WORDS];
	struct nfuse_attr st;
	uint32_t rc;

	xdr_get_bitmap(d, req, NFS4_BITMAP_WORDS);
	if (d->err)
		return NFS4ERR_BADXDR;
	if (!c->have_cur)
		return NFS4ERR_NOFILEHANDLE;
	/* write-only attributes cannot be read */
	if ((req[1] & (1u << (FATTR4_TIME_ACCESS_SET - 32))) ||
	    (req[1] & (1u << (FATTR4_TIME_MODIFY_SET - 32))))
		return NFS4ERR_INVAL;
	rc = fh_attr(c, &c->cur, &st, want_of(req));
	if (rc)
		return rc;
	nfuse_attr_encode(c->s, e, req, &c->cur, &st, NFS4_OK);
	return NFS4_OK;
}

/*
 * Compute the access rights that can be granted from the caller's identity and the mode bits; if
 * the backend has an access callback, ask it once more per class (read / write / execute) and let
 * the backend's answer prevail. On a read-only volume write permission is not removed here: let
 * the actual write operation return NFS4ERR_ROFS (the client reports EROFS) rather than turning
 * it into EACCES at the ACCESS step.
 */
static uint32_t op_access(struct cctx *c, xdr_dec_t *d, xdr_enc_t *e)
{
	uint32_t want = xdr_get_u32(d), grant = 0, perm;
	struct nfuse_attr st;
	uint32_t rc, i;
	int isdir, ingroup = 0;

	if (d->err)
		return NFS4ERR_BADXDR;
	if (!c->have_cur)
		return NFS4ERR_NOFILEHANDLE;
	rc = cur_attr(c, &c->cur, &st);
	if (rc)
		return rc;
	isdir = S_ISDIR(st.mode);

	if (c->cred->uid == 0) {
		perm = 07;
	} else if (c->cred->uid == st.uid) {
		perm = (st.mode >> 6) & 07;
	} else {
		if (c->cred->gid == st.gid)
			ingroup = 1;
		for (i = 0; i < c->cred->ngids && !ingroup; i++)
			if (c->cred->gids[i] == st.gid)
				ingroup = 1;
		perm = ingroup ? (st.mode >> 3) & 07 : st.mode & 07;
	}
	if (perm & 04)
		grant |= ACCESS4_READ;
	if (perm & 02)
		grant |= ACCESS4_MODIFY | ACCESS4_EXTEND | (isdir ? ACCESS4_DELETE : 0);
	if (perm & 01)
		grant |= isdir ? ACCESS4_LOOKUP : ACCESS4_EXECUTE;
	/* for root, execute permission on a regular file still requires at least one x bit */
	if (c->cred->uid == 0 && !isdir && !(st.mode & 0111))
		grant &= ~ACCESS4_EXECUTE;

	if (c->cur.kind == NFUSE_FH_NODE && c->s->be.ops->access) {
		void *b = c->s->be.ctx;
		int r;

		if (want & ACCESS4_READ) {
			r = c->s->be.ops->access(b, c->cur.ino, R_OK);
			if (r == 0)
				grant |= ACCESS4_READ;
			else if (r != -ENOSYS)
				grant &= ~ACCESS4_READ;
		}
		if (want & (ACCESS4_LOOKUP | ACCESS4_EXECUTE)) {
			r = c->s->be.ops->access(b, c->cur.ino, X_OK);
			if (r == 0 && (isdir || (st.mode & 0111)))
				grant |= isdir ? ACCESS4_LOOKUP : ACCESS4_EXECUTE;
			else if (r != -ENOSYS && r != 0)
				grant &= ~(ACCESS4_LOOKUP | ACCESS4_EXECUTE);
		}
		if (want & (ACCESS4_MODIFY | ACCESS4_EXTEND | ACCESS4_DELETE)) {
			uint32_t wbits = ACCESS4_MODIFY | ACCESS4_EXTEND |
					 (isdir ? ACCESS4_DELETE : 0);
			r = c->s->be.ops->access(b, c->cur.ino, W_OK);
			/*
			 * EROFS is not a denial: leave it to the actual write operation to return
			 * ROFS
			 */
			if (r == 0 || r == -EROFS)
				grant |= wbits;
			else if (r != -ENOSYS)
				grant &= ~wbits;
		}
	}
	if (is_pseudo(&c->cur) ||
	    ((c->cur.kind == NFUSE_FH_ATTRDIR || c->cur.kind == NFUSE_FH_NAMEDATTR ||
	      c->cur.kind == NFUSE_FH_ADFILE) && !xattr_writable(c->s)))
		grant &= ~(ACCESS4_MODIFY | ACCESS4_EXTEND | ACCESS4_DELETE);

	want &= ACCESS4_READ | ACCESS4_LOOKUP | ACCESS4_MODIFY |
		ACCESS4_EXTEND | ACCESS4_DELETE | ACCESS4_EXECUTE;
	xdr_put_u32(e, want);		/* supported: every bit we are able to decide */
	xdr_put_u32(e, want & grant);
	return NFS4_OK;
}

/* ---------- Directories ---------- */

struct rd_state {
	struct cctx	*c;
	xdr_enc_t	*e;
	uint32_t	req[NFS4_BITMAP_WORDS];
	unsigned	want;
	struct nfuse_fh	dirfh;
	size_t		start;		/* start of READDIR4resok (before the cookieverf) */
	uint32_t	maxcount;
	uint32_t	dircount;
	uint32_t	dirbytes;
	uint32_t	nent;
	int		full;
};

/* Mapping between cookies and backend positions: 0/1/2 are reserved, the first entry starts at 3 */
#define RD_COOKIE_BASE	2

/*
 * When only these attributes are wanted the full attributes need not be fetched (type and fileid
 * come from the listing itself)
 */
static int rd_need_attrs(const uint32_t req[NFS4_BITMAP_WORDS])
{
	uint32_t w0 = req[0] & ~((1u << FATTR4_RDATTR_ERROR) |
				 (1u << FATTR4_FILEHANDLE) |
				 (1u << FATTR4_FILEID));
	uint32_t w1 = req[1] & ~(1u << (FATTR4_MOUNTED_ON_FILEID - 32));

	return w0 || w1;
}

static int rd_fill(void *arg, const struct nfuse_dirent *de)
{
	struct rd_state *r = arg;
	struct nfuse_attr st;
	const struct nfuse_attr *pst = de->attr;
	struct nfuse_fh fh;
	size_t mark = r->e->len;
	uint32_t err = NFS4_OK, namelen = (uint32_t)strlen(de->name);

	if (r->dircount && r->dirbytes + 8 + XDR_PAD(namelen) + 4 > r->dircount &&
	    r->nent > 0) {
		r->full = 1;
		return 1;
	}
	if (r->dirfh.kind == NFUSE_FH_ATTRDIR) {
		/* named attribute directory: de->ino is the attribute name id */
		memset(&fh, 0, sizeof(fh));
		fh.kind = NFUSE_FH_NAMEDATTR;
		fh.boot = r->c->s->boot;
		fh.ino = r->dirfh.ino;
		fh.attr_id = de->ino;
		if (fh_attr(r->c, &fh, &st, NFUSE_WANT_BASIC)) {
			memset(&st, 0, sizeof(st));
			err = NFS4ERR_STALE;
		}
		pst = &st;
	} else {
		if (!pst) {
			int rc = r->c->s->be.ops->getattr(r->c->s->be.ctx, de->ino,
							  &st, r->want);
			if (rc) {
				/*
				 * fetching the attributes of a single entry failed: tell the
				 * client through rdattr_error instead of failing the whole
				 * listing
				 */
				memset(&st, 0, sizeof(st));
				st.ino = de->ino;
				err = nfuse_errno_to_nfs4(rc);
			}
			pst = &st;
		}
		set_node_fh(r->c, &fh, de->ino,
			    is_pseudo(&r->dirfh) ? 0 : r->dirfh.ino);
	}

	xdr_put_bool(r->e, 1);				/* value_follows */
	xdr_put_u64(r->e, de->pos + RD_COOKIE_BASE);	/* cookie */
	xdr_put_opaque(r->e, de->name, namelen);
	if (err) {
		/*
		 * when the attributes cannot be fetched return only rdattr_error (if the client
		 * asked for it)
		 */
		uint32_t only[NFS4_BITMAP_WORDS] = { 0 };
		if (r->req[0] & (1u << FATTR4_RDATTR_ERROR))
			only[0] = 1u << FATTR4_RDATTR_ERROR;
		nfuse_attr_encode(r->c->s, r->e, only, &fh, pst, err);
	} else {
		nfuse_attr_encode(r->c->s, r->e, r->req, &fh, pst, NFS4_OK);
	}
	/* also leave 8 bytes for the trailing "no next entry + eof" */
	if (r->e->err || r->e->len - r->start + 8 > r->maxcount) {
		xdr_truncate(r->e, mark);
		r->e->err = 0;
		r->full = 1;
		return 1;
	}
	r->dirbytes += 8 + XDR_PAD(namelen) + 4;
	r->nent++;
	return 0;
}

/*
 * Listing of a named attribute directory: one listxattr fetches everything, and the position is
 * simply the index
 */
static int rd_attrdir(struct cctx *c, struct rd_state *r, uint64_t pos, int *eof)
{
	char *buf = NULL;
	int n, rc = 0;
	uint64_t idx = 0;
	char *p;

	*eof = 0;
	n = c->s->be.ops->listxattr(c->s->be.ctx, r->dirfh.ino, NULL, 0);
	if (n < 0)
		return n;
	if (n > 0) {
		buf = malloc((size_t)n);
		if (!buf)
			return -ENOMEM;
		n = c->s->be.ops->listxattr(c->s->be.ctx, r->dirfh.ino, buf, (size_t)n);
		if (n < 0) {
			free(buf);
			return n;
		}
	}
	for (p = buf; buf && p < buf + n; p += strlen(p) + 1, idx++) {
		nfuse_ino_t id;
		struct nfuse_dirent de;

		if (idx < pos || !*p)
			continue;
		if (attr_intern(c->s, p, &id)) {
			rc = -ENOMEM;
			break;
		}
		de.name = p;
		de.ino = id;
		de.pos = idx + 1;
		de.attr = NULL;
		if (rd_fill(r, &de))
			goto out;
	}
	*eof = 1;
out:
	free(buf);
	return rc;
}

static uint32_t op_readdir(struct cctx *c, xdr_dec_t *d, xdr_enc_t *e)
{
	struct rd_state r;
	struct nfuse_attr st;
	uint64_t cookie, pos, verf64;
	uint8_t verf[NFS4_VERIFIER_SIZE];
	uint32_t rc;
	int eof = 0, b;

	memset(&r, 0, sizeof(r));
	cookie = xdr_get_u64(d);
	xdr_get_fixed(d, verf, sizeof(verf));
	r.dircount = xdr_get_u32(d);
	r.maxcount = xdr_get_u32(d);
	xdr_get_bitmap(d, r.req, NFS4_BITMAP_WORDS);
	if (d->err)
		return NFS4ERR_BADXDR;
	rc = need_dir(c, &st);
	if (rc)
		return rc;
	if (cookie == 1 || cookie == 2)
		return NFS4ERR_BAD_COOKIE;
	pos = cookie ? cookie - RD_COOKIE_BASE : 0;
	memcpy(&verf64, verf, sizeof(verf64));

	r.c = c;
	r.e = e;
	r.dirfh = c->cur;
	r.want = rd_need_attrs(r.req) ? want_of(r.req) : 0;
	r.start = e->len;
	if (r.maxcount > c->s->rwsize + 4096)
		r.maxcount = c->s->rwsize + 4096;
	xdr_put_fixed(e, verf, sizeof(verf));		/* placeholder, filled in once the listing is done */

	if (is_pseudo(&c->cur)) {
		/* the pseudo root contains only one entry, the export directory */
		struct nfuse_dirent de = {
			.name = c->s->export_name,
			.ino = c->s->be.ops->root(c->s->be.ctx),
			.pos = 1,
		};
		verf64 = c->s->boot;
		if (pos == 0)
			rd_fill(&r, &de);
		eof = !r.full;
	} else if (c->cur.kind == NFUSE_FH_ATTRDIR) {
		verf64 = c->s->boot;
		b = rd_attrdir(c, &r, pos, &eof);
		if (b)
			return nfuse_errno_to_nfs4(b);
		if (r.full)
			eof = 0;
	} else {
		b = c->s->be.ops->readdir(c->s->be.ctx, c->cur.ino, pos, &verf64,
					  r.want, rd_fill, &r, &eof);
		if (b) {
			if (b == -NFUSE_E_NOT_SAME)
				c->s->readdir_notsame++;
			return nfuse_errno_to_nfs4(b);
		}
		if (r.full)
			eof = 0;
	}
	if (r.nent == 0 && !eof)
		return NFS4ERR_TOOSMALL;
	memcpy(e->buf + r.start, &verf64, sizeof(verf64));
	xdr_put_bool(e, 0);		/* end of the list */
	xdr_put_bool(e, eof);
	return NFS4_OK;
}

static uint32_t op_readlink(struct cctx *c, xdr_dec_t *d, xdr_enc_t *e)
{
	struct nfuse_attr st;
	char buf[4096];
	uint32_t rc;
	int r;

	(void)d;
	buf[0] = 0;
	if (!c->have_cur)
		return NFS4ERR_NOFILEHANDLE;
	rc = cur_attr(c, &c->cur, &st);
	if (rc)
		return rc;
	if (!S_ISLNK(st.mode))
		return NFS4ERR_INVAL;
	r = c->s->be.ops->readlink(c->s->be.ctx, c->cur.ino, buf, sizeof(buf));
	if (r)
		return nfuse_errno_to_nfs4(r);
	xdr_put_string(e, buf);
	return NFS4_OK;
}

/* ---------- Client ID ---------- */

static uint32_t op_setclientid(struct cctx *c, xdr_dec_t *d, xdr_enc_t *e)
{
	uint8_t verf[8], confirm[8];
	const uint8_t *id;
	uint32_t idlen, n, rc;
	uint64_t clientid;

	xdr_get_fixed(d, verf, 8);
	id = xdr_get_opaque_ref(d, NFS4_OPAQUE_LIMIT, &idlen);
	/*
	 * Callback information: cb_program, r_netid, r_addr, callback_ident. We hand out no
	 * delegations and never call back, so just read past it.
	 */
	(void)xdr_get_u32(d);
	(void)xdr_get_opaque_ref(d, 64, &n);
	(void)xdr_get_opaque_ref(d, 128, &n);
	(void)xdr_get_u32(d);
	if (d->err)
		return NFS4ERR_BADXDR;
	rc = nfuse_setclientid(c->s, verf, id, idlen, &clientid, confirm);
	if (rc)
		return rc;
	xdr_put_u64(e, clientid);
	xdr_put_fixed(e, confirm, 8);
	return NFS4_OK;
}

static uint32_t op_setclientid_confirm(struct cctx *c, xdr_dec_t *d, xdr_enc_t *e)
{
	uint64_t clientid = xdr_get_u64(d);
	uint8_t confirm[8];

	(void)e;
	xdr_get_fixed(d, confirm, 8);
	if (d->err)
		return NFS4ERR_BADXDR;
	return nfuse_setclientid_confirm(c->s, clientid, confirm);
}

static uint32_t op_renew(struct cctx *c, xdr_dec_t *d, xdr_enc_t *e)
{
	uint64_t clientid = xdr_get_u64(d);

	(void)e;
	if (d->err)
		return NFS4ERR_BADXDR;
	return nfuse_renew(c->s, clientid);
}

/* SECINFO: only AUTH_SYS is offered. Per the RFC, the current handle is consumed on success */
static uint32_t op_secinfo(struct cctx *c, xdr_dec_t *d, xdr_enc_t *e)
{
	char name[256];
	struct nfuse_attr st;
	struct nfuse_fh nfh;
	uint32_t rc = get_component(d, name, sizeof(name));

	if (rc == NFS4ERR_BADXDR)
		return rc;
	if (!c->have_cur)
		return NFS4ERR_NOFILEHANDLE;
	if (rc)
		return rc;
	rc = need_dir(c, &st);
	if (rc)
		return rc;
	rc = lookup_name(c, &c->cur, name, &nfh);
	if (rc)
		return rc;
	xdr_put_u32(e, 1);		/* one entry */
	xdr_put_u32(e, RPC_AUTH_SYS);
	c->have_cur = 0;
	return NFS4_OK;
}

/* ---------- Open / close ---------- */

static int open_matches(const struct nfuse_open *op, const struct nfuse_fh *fh)
{
	return op->ino == fh->ino && op->kind == fh->kind &&
	       (fh->kind != NFUSE_FH_NAMEDATTR || op->attr_id == fh->attr_id);
}

/*
 * Share reservation conflict: another owner's deny blocks the access I want, or my deny blocks
 * access another owner already has
 */
static int share_conflict(struct cctx *c, const struct nfuse_fh *fh,
			  const struct nfuse_owner *me, uint32_t access,
			  uint32_t deny)
{
	struct nfuse_open *op;

	for (op = c->s->opens; op; op = op->next) {
		if (op->owner == me || !open_matches(op, fh))
			continue;
		if ((access & op->share_deny) || (deny & op->share_access))
			return 1;
	}
	return 0;
}

/*
 * The EXCLUSIVE4 verifier is stored in the seconds of atime / mtime (the usual approach, RFC 7530
 * section 16.16.5)
 */
static void verf_to_sattr(const uint8_t v[8], struct nfuse_sattr *sa)
{
	memset(sa, 0, sizeof(*sa));
	sa->mask = NFUSE_SA_ATIME | NFUSE_SA_MTIME;
	sa->atime.tv_sec = (time_t)(((uint32_t)v[0] << 24) | ((uint32_t)v[1] << 16) |
				    ((uint32_t)v[2] << 8) | v[3]);
	sa->mtime.tv_sec = (time_t)(((uint32_t)v[4] << 24) | ((uint32_t)v[5] << 16) |
				    ((uint32_t)v[6] << 8) | v[7]);
}

static int verf_matches(const uint8_t v[8], const struct nfuse_attr *st)
{
	struct nfuse_sattr sa;

	verf_to_sattr(v, &sa);
	return st->atime.tv_sec == sa.atime.tv_sec && st->atime.tv_nsec == 0 &&
	       st->mtime.tv_sec == sa.mtime.tv_sec && st->mtime.tv_nsec == 0;
}

static uint32_t op_open(struct cctx *c, xdr_dec_t *d, xdr_enc_t *e)
{
	uint32_t seqid, access, deny, opentype, createmode = UNCHECKED4, claim;
	uint32_t ownerlen, rc, sattr_rc = NFS4_OK, namerc = NFS4_OK;
	uint32_t attrset[NFS4_BITMAP_WORDS] = { 0 }, dummy[NFS4_BITMAP_WORDS];
	uint32_t rflags = OPEN4_RESULT_LOCKTYPE_POSIX;
	uint64_t clientid, before, after;
	const uint8_t *owner;
	uint8_t verf[NFS4_VERIFIER_SIZE];
	char name[256];
	struct nfuse_sattr sa;
	struct nfuse_attr dst, st;
	struct nfuse_fh dirfh, nfh;
	struct nfuse_owner *ow;
	struct nfuse_open *op;
	uint64_t befh = 0;
	nfuse_ino_t adx = 0;
	int created = 0, r, need_write, adex = 0;

	memset(&sa, 0, sizeof(sa));
	seqid = xdr_get_u32(d);
	access = xdr_get_u32(d);
	deny = xdr_get_u32(d);
	clientid = xdr_get_u64(d);
	owner = xdr_get_opaque_ref(d, NFS4_OPAQUE_LIMIT, &ownerlen);
	opentype = xdr_get_u32(d);
	if (opentype == OPEN4_CREATE) {
		createmode = xdr_get_u32(d);
		if (createmode == UNCHECKED4 || createmode == GUARDED4) {
			sattr_rc = get_sattr(d, &sa, dummy);
			if (sattr_rc == NFS4ERR_BADXDR)
				return sattr_rc;
		} else if (createmode == EXCLUSIVE4) {
			xdr_get_fixed(d, verf, sizeof(verf));
		} else {
			return NFS4ERR_BADXDR;
		}
	} else if (opentype != OPEN4_NOCREATE) {
		return NFS4ERR_BADXDR;
	}
	claim = xdr_get_u32(d);
	switch (claim) {
	case CLAIM_NULL:
	case CLAIM_DELEGATE_PREV:
		namerc = get_component(d, name, sizeof(name));
		break;
	case CLAIM_PREVIOUS:
		(void)xdr_get_u32(d);		/* delegate_type */
		break;
	case CLAIM_DELEGATE_CUR: {
		struct nfuse_stateid ds;
		get_stateid(d, &ds);
		namerc = get_component(d, name, sizeof(name));
		break;
	}
	default:
		return NFS4ERR_BADXDR;
	}
	if (d->err || namerc == NFS4ERR_BADXDR)
		return NFS4ERR_BADXDR;
	if (!c->have_cur)
		return NFS4ERR_NOFILEHANDLE;
	/*
	 * There is no grace period: open state from before a restart can never be reclaimed
	 * (CLAIM_PREVIOUS gets NO_GRACE). NO_GRACE rather than STALE_CLIENTID is returned even
	 * when the clientid is no longer valid -- the macOS client has been observed, right after
	 * STALE_STATEID, to send CLAIM_PREVIOUS with the old clientid; on STALE_CLIENTID it
	 * retries every second and the writing process stays stuck, whereas on NO_GRACE it gives
	 * up reclaiming and reopens with CLAIM_NULL, after which RENEW fails -> SETCLIENTID and
	 * it recovers within a second. When the clientid is valid, go through the normal owner
	 * seqid processing before returning NO_GRACE (NO_GRACE has to advance the seqid).
	 */
	rc = nfuse_check_clientid(c->s, clientid);
	if (rc) {
		if (claim == CLAIM_PREVIOUS) {
			c->s->no_grace++;
			return NFS4ERR_NO_GRACE;
		}
		return rc;
	}
	c->open_excl = opentype == OPEN4_CREATE && createmode == EXCLUSIVE4;

	/* ---- open-owner and seqid ---- */
	{
		static uint32_t sweep_tick;
		if ((++sweep_tick & 255) == 0)
			nfuse_owners_sweep(c->s);
	}
	ow = nfuse_owner_get(c->s, clientid, owner, ownerlen, 1);
	if (!ow)
		return NFS4ERR_RESOURCE;
	r = nfuse_seq_check(&ow->seq, seqid, OP_OPEN);
	if (r == 1)
		return seq_replay(c, &ow->seq, e);
	if (r < 0) {
		if (ow->confirmed) {
			c->s->bad_seqid++;
			return NFS4ERR_BAD_SEQID;
		}
		owner_reset(c, ow);	/* unconfirmed owner: treat it as new */
	}
	seq_will_record(c, &ow->seq, seqid);

	/* ---- open type ---- */
	if (claim == CLAIM_PREVIOUS) {
		/*
		 * when the clientid is valid, record it by seqid as well (NO_GRACE advances the
		 * open-owner's seqid)
		 */
		c->s->no_grace++;
		return NFS4ERR_NO_GRACE;
	}
	if (claim != CLAIM_NULL)
		return NFS4ERR_NOTSUPP;		/* delegations are never handed out */
	if (namerc)
		return namerc;
	if ((access & OPEN4_SHARE_ACCESS_BOTH) == 0 ||
	    (access & ~OPEN4_SHARE_ACCESS_BOTH) || (deny & ~OPEN4_SHARE_ACCESS_BOTH))
		return NFS4ERR_INVAL;
	if (!be_writable(c) && ((access & OPEN4_SHARE_ACCESS_WRITE) ||
				opentype == OPEN4_CREATE))
		return NFS4ERR_ROFS;
	rc = need_dir(c, &dst);
	if (rc)
		return rc;
	dirfh = c->cur;
	before = dst.change;
	rc = lookup_name(c, &dirfh, name, &nfh);
	if (c->s->verbose && c->s->log)
		c->s->log("OPEN %s%s name=%s access=%u deny=%u type=%s mode=%u sattr=%#x size=%llu -> lookup %u",
			  dirfh.kind == NFUSE_FH_ATTRDIR ? "[attribute directory] " : "",
			  opentype == OPEN4_CREATE ? "CREATE" : "NOCREATE", name, access, deny,
			  opentype == OPEN4_CREATE ? (createmode == UNCHECKED4 ? "UNCHECKED" :
			  createmode == GUARDED4 ? "GUARDED" : "EXCLUSIVE") : "-", createmode,
			  sa.mask, (unsigned long long)sa.size, rc);

	if (opentype == OPEN4_CREATE && dirfh.kind == NFUSE_FH_ATTRDIR) {
		/*
		 * creating an attribute in a named attribute directory: create an empty value;
		 * the client then WRITEs the value
		 */
		if (!xattr_writable(c->s))
			return NFS4ERR_ROFS;
		if (rc == NFS4_OK) {
			if (createmode != UNCHECKED4)
				/*
				 * EXCLUSIVE4 has nowhere to store the verifier; retransmissions
				 * are covered by the DRC
				 */
				return NFS4ERR_EXIST;
			if (sattr_rc)
				return sattr_rc;
		} else if (rc == NFS4ERR_NOENT) {
			if (createmode != EXCLUSIVE4 && sattr_rc)
				return sattr_rc;
			r = c->s->be.ops->xattr_write(c->s->be.ctx, dirfh.ino, name, NULL, 0, 0);
			if (r < 0)
				return nfuse_errno_to_nfs4(r);
			rc = lookup_name(c, &dirfh, name, &nfh);
			if (rc)
				return rc;
			created = 1;
			if (createmode != EXCLUSIVE4 && (sa.mask & NFUSE_SA_SIZE))
				bm_set(attrset, FATTR4_SIZE);
		} else {
			return rc;
		}
	} else if (opentype == OPEN4_CREATE && rc == NFS4ERR_NOENT &&
		   adf_name(c, &dirfh, name, &adx, &adex)) {
		/*
		 * the client is creating `._X` (X exists and has no attributes yet): start a
		 * virtual-file session instead of creating a file on disk
		 */
		if (!xattr_writable(c->s))
			return NFS4ERR_ROFS;
		if (createmode != EXCLUSIVE4 && sattr_rc)
			return sattr_rc;
		r = nfuse_adf_create(c->s, adx, 1);
		if (r)
			return nfuse_errno_to_nfs4(r);
		set_node_fh(c, &nfh, adx, dirfh.ino);
		nfh.kind = NFUSE_FH_ADFILE;
		created = 1;
		if (createmode != EXCLUSIVE4)
			sattr_bits(&sa, attrset);
	} else if (opentype == OPEN4_CREATE && rc == NFS4_OK && nfh.kind == NFUSE_FH_ADFILE) {
		if (createmode != UNCHECKED4)
			return NFS4ERR_EXIST;
		if (sattr_rc)
			return sattr_rc;
	} else if (opentype == OPEN4_CREATE) {
		if (dirfh.kind != NFUSE_FH_NODE)
			return is_pseudo(&dirfh) ? NFS4ERR_ROFS : NFS4ERR_NOTSUPP;
		if (!c->s->be.ops->create)
			return NFS4ERR_ROFS;
		if (rc == NFS4_OK) {
			/* already exists */
			if (createmode == GUARDED4)
				return NFS4ERR_EXIST;
			rc = cur_attr(c, &nfh, &st);
			if (rc)
				return rc;
			if (createmode == EXCLUSIVE4) {
				/*
				 * a retransmitted exclusive create: if the verifier matches it
				 * counts as success (RFC 7530 section 16.16.5)
				 */
				if (!S_ISREG(st.mode) || !verf_matches(verf, &st))
					return NFS4ERR_EXIST;
				bm_set(attrset, FATTR4_TIME_ACCESS);
				bm_set(attrset, FATTR4_TIME_MODIFY);
			} else if (sattr_rc) {
				return sattr_rc;
			}
		} else if (rc == NFS4ERR_NOENT) {
			uint32_t mode;
			nfuse_ino_t ino;

			if (createmode != EXCLUSIVE4 && sattr_rc)
				return sattr_rc;
			mode = (sa.mask & NFUSE_SA_MODE) ? sa.mode : 0644;
			r = c->s->be.ops->create(c->s->be.ctx, dirfh.ino, name, mode,
						 O_RDWR, &ino, &befh);
			if (r)
				return nfuse_errno_to_nfs4(r);
			created = 1;
			set_node_fh(c, &nfh, ino, dirfh.ino);
			if (createmode == EXCLUSIVE4) {
				struct nfuse_sattr vs;

				verf_to_sattr(verf, &vs);
				r = c->s->be.ops->setattr(c->s->be.ctx, ino, befh, &vs);
				if (r && c->s->log)
					c->s->log("OPEN(EXCLUSIVE4) failed to store the verifier: errno %d", -r);
				bm_set(attrset, FATTR4_TIME_ACCESS);
				bm_set(attrset, FATTR4_TIME_MODIFY);
			} else {
				struct nfuse_sattr rest = sa;

				/*
				 * mode was already given at creation; a new file's size is 0
				 * anyway
				 */
				rest.mask &= ~(NFUSE_SA_MODE | NFUSE_SA_SIZE);
				sattr_bits(&sa, attrset);
				if (rest.mask) {
					r = c->s->be.ops->setattr(c->s->be.ctx, ino, befh, &rest);
					if (r) {
						/*
						 * the file has been created: failing to set
						 * attributes is only logged and does not fail the
						 * OPEN
						 */
						uint32_t rb[NFS4_BITMAP_WORDS];
						int k;

						if (c->s->log)
							c->s->log("OPEN(CREATE) failed to set attributes: errno %d (mask %#x)",
								  -r, rest.mask);
						sattr_bits(&rest, rb);
						for (k = 0; k < NFS4_BITMAP_WORDS; k++)
							attrset[k] &= ~rb[k];
					}
				}
			}
		} else {
			return rc;
		}
	} else if (rc) {
		return rc;
	}

	if (!created) {
		rc = cur_attr(c, &nfh, &st);
		if (rc)
			return rc;
		if (S_ISDIR(st.mode))
			return NFS4ERR_ISDIR;
		if (S_ISLNK(st.mode))
			return NFS4ERR_SYMLINK;
		if (!S_ISREG(st.mode))
			return NFS4ERR_INVAL;
	}
	if (share_conflict(c, &nfh, ow, access, deny)) {
		if (created && c->s->be.ops->release)
			c->s->be.ops->release(c->s->be.ctx, nfh.ino, befh);
		return NFS4ERR_SHARE_DENIED;
	}

	need_write = (access & OPEN4_SHARE_ACCESS_WRITE) != 0;
	for (op = c->s->opens; op; op = op->next)
		if (op->owner == ow && open_matches(op, &nfh))
			break;
	if (op) {
		/*
		 * the same owner opens the same file again: merge the access modes and bump the
		 * stateid seqid
		 */
		if (created && c->s->be.ops->release)
			c->s->be.ops->release(c->s->be.ctx, nfh.ino, befh);
		if (need_write && !op->bewrite && op->kind == NFUSE_FH_NODE &&
		    c->s->be.ops->open) {
			/* read upgraded to read-write: reopen the backend handle read-write */
			uint64_t nb = 0;

			r = c->s->be.ops->open(c->s->be.ctx, nfh.ino, O_RDWR, &nb);
			if (r)
				return nfuse_errno_to_nfs4(r);
			if (c->s->be.ops->release)
				c->s->be.ops->release(c->s->be.ctx, nfh.ino, op->befh);
			op->befh = nb;
			op->bewrite = 1;
		}
		op->share_access |= access;
		op->share_deny |= deny;
		op->sid.seqid++;
	} else {
		if (!created && nfh.kind == NFUSE_FH_NODE && c->s->be.ops->open) {
			r = c->s->be.ops->open(c->s->be.ctx, nfh.ino,
					       need_write ? O_RDWR : O_RDONLY, &befh);
			if (r)
				return nfuse_errno_to_nfs4(r);
		}
		op = nfuse_open_new(c->s, ow, nfh.ino, befh, access, deny);
		if (!op) {
			if (nfh.kind == NFUSE_FH_NODE && c->s->be.ops->release)
				c->s->be.ops->release(c->s->be.ctx, nfh.ino, befh);
			return NFS4ERR_RESOURCE;
		}
		op->bewrite = need_write || created;
		op->kind = nfh.kind;
		op->attr_id = nfh.kind == NFUSE_FH_NAMEDATTR ? nfh.attr_id : 0;
	}
	/*
	 * Opening a named attribute for write = whole-value replacement: when changing an
	 * existing attribute the macOS client sends only OPEN(NOCREATE) + WRITE and no truncation
	 * (observed: writing a long value and then a short one yields "short value + old tail";
	 * writing an empty value is just OPEN + CLOSE). So truncate to 0 right here in OPEN; the
	 * GETATTR that follows in the OPEN reply already shows 0 bytes -- exactly as when the
	 * client itself sends OPEN(CREATE, size=0). Truncating at the first WRITE instead is not
	 * an option: the attribute would then shrink behind the client's back while it has it
	 * cached, and the getxattr that follows makes the system NFS client dereference a NULL
	 * pointer in nfs4_named_attr_get and panic the kernel (reproduced step by step on macOS
	 * 12; macOS 27 behaves the same). The resource fork is the exception: it is written in
	 * chunks by position, and APFS does not truncate it either (observed).
	 */
	if (nfh.kind == NFUSE_FH_NAMEDATTR && (access & OPEN4_SHARE_ACCESS_WRITE) && !created) {
		const char *an = attr_name(c->s, nfh.attr_id);

		if (an && strcmp(an, "com.apple.ResourceFork") != 0 &&
		    c->s->be.ops->xattr_truncate) {
			r = c->s->be.ops->xattr_truncate(c->s->be.ctx, nfh.ino, an, 0);
			if (r && r != -ENOATTR)
				return nfuse_errno_to_nfs4(r);
			c->s->xattr_trunc_on_write++;
		}
	}
	/* UNCHECKED4 open of an existing file: only size (truncation) is honored */
	if (!created && opentype == OPEN4_CREATE && createmode == UNCHECKED4 &&
	    (sa.mask & NFUSE_SA_SIZE) && need_write) {
		struct nfuse_sattr ts;

		memset(&ts, 0, sizeof(ts));
		ts.mask = NFUSE_SA_SIZE;
		ts.size = sa.size;
		if (nfh.kind == NFUSE_FH_NAMEDATTR) {
			const char *an = attr_name(c->s, nfh.attr_id);
			r = an && c->s->be.ops->xattr_truncate ?
			    c->s->be.ops->xattr_truncate(c->s->be.ctx, nfh.ino, an, sa.size) :
			    -EROFS;
		} else if (nfh.kind == NFUSE_FH_ADFILE) {
			r = nfuse_adf_truncate(c->s, nfh.ino, sa.size);
		} else {
			r = c->s->be.ops->setattr(c->s->be.ctx, nfh.ino, op->befh, &ts);
		}
		if (r)
			return nfuse_errno_to_nfs4(r);
		bm_set(attrset, FATTR4_SIZE);
	}
	if (!ow->confirmed)
		rflags |= OPEN4_RESULT_CONFIRM;
	after = created ? dir_change(c, &dirfh) : before;

	put_stateid(e, &op->sid);
	put_change_info(e, before, after);
	xdr_put_u32(e, rflags);
	xdr_put_bitmap(e, attrset, NFS4_BITMAP_WORDS);
	xdr_put_u32(e, OPEN_DELEGATE_NONE);
	c->cur = nfh;
	return NFS4_OK;
}

static uint32_t op_open_confirm(struct cctx *c, xdr_dec_t *d, xdr_enc_t *e)
{
	struct nfuse_stateid sid;
	struct nfuse_open *op;
	uint32_t seqid, rc;
	int r;

	get_stateid(d, &sid);
	seqid = xdr_get_u32(d);
	if (d->err)
		return NFS4ERR_BADXDR;
	if (!c->have_cur)
		return NFS4ERR_NOFILEHANDLE;
	rc = nfuse_open_find(c->s, &sid, &op);
	if (rc)
		return rc;
	r = nfuse_seq_check(&op->owner->seq, seqid, OP_OPEN_CONFIRM);
	if (r == 1)
		return seq_replay(c, &op->owner->seq, e);
	if (r < 0) {
		c->s->bad_seqid++;
		return NFS4ERR_BAD_SEQID;
	}
	seq_will_record(c, &op->owner->seq, seqid);
	rc = nfuse_stateid_check_seq(&op->sid, &sid);
	if (rc)
		return rc;
	if (op->ino != c->cur.ino)
		return NFS4ERR_BAD_STATEID;
	op->owner->confirmed = 1;
	op->sid.seqid++;
	put_stateid(e, &op->sid);
	return NFS4_OK;
}

static uint32_t op_open_downgrade(struct cctx *c, xdr_dec_t *d, xdr_enc_t *e)
{
	struct nfuse_stateid sid;
	struct nfuse_open *op;
	uint32_t seqid, access, deny, rc;
	int r;

	get_stateid(d, &sid);
	seqid = xdr_get_u32(d);
	access = xdr_get_u32(d);
	deny = xdr_get_u32(d);
	if (d->err)
		return NFS4ERR_BADXDR;
	if (!c->have_cur)
		return NFS4ERR_NOFILEHANDLE;
	rc = nfuse_open_find(c->s, &sid, &op);
	if (rc)
		return rc;
	r = nfuse_seq_check(&op->owner->seq, seqid, OP_OPEN_DOWNGRADE);
	if (r == 1)
		return seq_replay(c, &op->owner->seq, e);
	if (r < 0) {
		c->s->bad_seqid++;
		return NFS4ERR_BAD_SEQID;
	}
	seq_will_record(c, &op->owner->seq, seqid);
	rc = nfuse_stateid_check_seq(&op->sid, &sid);
	if (rc)
		return rc;
	if (op->ino != c->cur.ino)
		return NFS4ERR_BAD_STATEID;
	/* can only narrow: the new access / deny must be a subset of the current ones */
	if ((access & OPEN4_SHARE_ACCESS_BOTH) == 0 ||
	    (access & ~op->share_access) || (deny & ~op->share_deny))
		return NFS4ERR_INVAL;
	op->share_access = access;
	op->share_deny = deny;
	op->sid.seqid++;
	put_stateid(e, &op->sid);
	return NFS4_OK;
}

static uint32_t op_close(struct cctx *c, xdr_dec_t *d, xdr_enc_t *e)
{
	struct nfuse_stateid sid, out;
	struct nfuse_open *op;
	uint32_t seqid, rc;
	int r;

	seqid = xdr_get_u32(d);
	get_stateid(d, &sid);
	if (d->err)
		return NFS4ERR_BADXDR;
	if (!c->have_cur)
		return NFS4ERR_NOFILEHANDLE;
	rc = nfuse_open_find(c->s, &sid, &op);
	if (rc == NFS4ERR_BAD_STATEID) {
		/*
		 * the open state was already released by the "first pass" of this CLOSE:
		 * recognize the retransmission from the owner's replay cache
		 */
		struct nfuse_owner *ow;

		for (ow = c->s->owners; ow; ow = ow->next)
			if (ow->seq.valid && ow->seq.op == OP_CLOSE &&
			    ow->seq.seqid == seqid &&
			    memcmp(ow->seq.other, sid.other, NFS4_OTHER_SIZE) == 0)
				return seq_replay(c, &ow->seq, e);
	}
	if (rc)
		return rc;
	r = nfuse_seq_check(&op->owner->seq, seqid, OP_CLOSE);
	if (r == 1)
		return seq_replay(c, &op->owner->seq, e);
	if (r < 0) {
		c->s->bad_seqid++;
		return NFS4ERR_BAD_SEQID;
	}
	seq_will_record(c, &op->owner->seq, seqid);
	c->rec_has_other = 1;
	memcpy(c->rec_other, sid.other, NFS4_OTHER_SIZE);
	rc = nfuse_stateid_check_seq(&op->sid, &sid);
	if (rc)
		return rc;
	if (op->kind == NFUSE_FH_NODE && c->s->be.ops->release)
		c->s->be.ops->release(c->s->be.ctx, op->ino, op->befh);
	out = op->sid;
	out.seqid++;
	nfuse_lock_detach_open(c->s, op);
	{
		int adf = op->kind == NFUSE_FH_ADFILE;
		nfuse_ino_t ax = op->ino;

		nfuse_open_free(c->s, op);
		if (adf && !adf_opens(c->s, ax))
			nfuse_adf_apply(c->s, ax);	/* the last open is closed: write X's attributes back */
	}
	put_stateid(e, &out);
	return NFS4_OK;
}

/* ---------- Read / write / commit ---------- */

/*
 * The stateid used for reads and writes: that of an open state, or of a lock derived from one;
 * special stateids (all zeros / all ones) go through a temporary open. When NFS4_OK is returned
 * *opp is the open state (NULL for a special stateid).
 */
static uint32_t io_stateid(struct cctx *c, const struct nfuse_stateid *sid,
			   struct nfuse_open **opp)
{
	uint32_t rc;

	*opp = NULL;
	if (nfuse_stateid_is_special(sid))
		return NFS4_OK;
	rc = nfuse_open_lookup(c->s, sid, opp);
	if (rc == NFS4ERR_BAD_STATEID) {
		/* the client may also read and write with a lock stateid */
		struct nfuse_lock *lk;
		if (nfuse_lock_lookup(c->s, sid, &lk) == NFS4_OK &&
		    nfuse_lock_attach(c->s, lk)) {
			*opp = lk->open;
			rc = NFS4_OK;
		}
	}
	if (rc)
		return rc;
	if (!open_matches(*opp, &c->cur))
		return NFS4ERR_BAD_STATEID;
	return NFS4_OK;
}

/*
 * Read a named attribute: the backend reads by offset (however large the resource fork is, the
 * whole value is never fetched), straight into the reply buffer
 */
static uint32_t read_namedattr(struct cctx *c, uint64_t off, uint32_t count,
			       uint64_t size, xdr_enc_t *e)
{
	const char *nm = attr_name(c->s, c->cur.attr_id);
	size_t eofpos, datapos;
	uint8_t *buf;
	int got;

	if (!nm)
		return NFS4ERR_STALE;
	if (!c->s->be.ops->xattr_read)
		return NFS4ERR_NOTSUPP;
	if (count > c->s->rwsize)
		count = c->s->rwsize;
	if (off >= size)
		count = 0;
	else if (size - off < count)
		count = (uint32_t)(size - off);
	eofpos = xdr_reserve_u32(e);
	datapos = e->len;
	buf = xdr_put_opaque_space(e, count);
	if (!buf)
		return NFS4ERR_RESOURCE;
	got = count ? c->s->be.ops->xattr_read(c->s->be.ctx, c->cur.ino, nm, buf, count, off) : 0;
	if (got < 0)
		return nfuse_errno_to_nfs4(got);
	xdr_truncate(e, datapos + 4 + XDR_PAD((uint32_t)got));
	xdr_patch_u32(e, datapos, (uint32_t)got);
	if (XDR_PAD((uint32_t)got) > (uint32_t)got)
		memset(e->buf + datapos + 4 + got, 0, XDR_PAD((uint32_t)got) - (uint32_t)got);
	xdr_patch_u32(e, eofpos, (off + (uint64_t)got >= size) ? 1 : 0);
	c->s->namedattr_ops++;
	return NFS4_OK;
}

static uint32_t op_read(struct cctx *c, xdr_dec_t *d, xdr_enc_t *e)
{
	struct nfuse_stateid sid;
	struct nfuse_open *op = NULL;
	struct nfuse_attr st;
	uint64_t off, befh = 0;
	uint32_t count, rc;
	size_t eofpos, datapos;
	uint8_t *buf;
	int got, temp_open = 0;

	get_stateid(d, &sid);
	off = xdr_get_u64(d);
	count = xdr_get_u32(d);
	if (d->err)
		return NFS4ERR_BADXDR;
	if (!c->have_cur)
		return NFS4ERR_NOFILEHANDLE;
	rc = cur_attr(c, &c->cur, &st);
	if (rc)
		return rc;
	if (S_ISDIR(st.mode))
		return NFS4ERR_ISDIR;
	if (!S_ISREG(st.mode))
		return NFS4ERR_INVAL;
	if (c->cur.kind == NFUSE_FH_NAMEDATTR) {
		rc = io_stateid(c, &sid, &op);
		if (rc)
			return rc;
		return read_namedattr(c, off, count, st.size, e);
	}
	if (c->cur.kind == NFUSE_FH_ADFILE) {
		rc = io_stateid(c, &sid, &op);
		if (rc)
			return rc;
		if (count > c->s->rwsize)
			count = c->s->rwsize;
		eofpos = xdr_reserve_u32(e);
		datapos = e->len;
		buf = xdr_put_opaque_space(e, count);
		if (!buf)
			return NFS4ERR_RESOURCE;
		got = nfuse_adf_read(c->s, c->cur.ino, buf, count, off);
		if (got < 0)
			return nfuse_errno_to_nfs4(got);
		xdr_truncate(e, datapos + 4 + XDR_PAD((uint32_t)got));
		xdr_patch_u32(e, datapos, (uint32_t)got);
		if (XDR_PAD((uint32_t)got) > (uint32_t)got)
			memset(e->buf + datapos + 4 + got, 0, XDR_PAD((uint32_t)got) - (uint32_t)got);
		xdr_patch_u32(e, eofpos, (off + (uint64_t)got >= st.size) ? 1 : 0);
		return NFS4_OK;
	}
	rc = io_stateid(c, &sid, &op);
	if (rc)
		return rc;
	if (op) {
		befh = op->befh;
	} else {
		/* anonymous read: temporary open */
		if (c->s->be.ops->open &&
		    c->s->be.ops->open(c->s->be.ctx, c->cur.ino, O_RDONLY, &befh))
			return NFS4ERR_ACCESS;
		temp_open = 1;
	}
	if (count > c->s->rwsize)
		count = c->s->rwsize;

	eofpos = xdr_reserve_u32(e);
	datapos = e->len;
	buf = xdr_put_opaque_space(e, count);
	if (!buf) {
		rc = NFS4ERR_RESOURCE;
		goto out;
	}
	got = c->s->be.ops->read(c->s->be.ctx, c->cur.ino, befh, buf, count, off);
	if (got < 0) {
		rc = nfuse_errno_to_nfs4(got);
		goto out;
	}
	/* finish up with the length actually read: fix the length and pad to alignment */
	xdr_truncate(e, datapos + 4 + XDR_PAD((uint32_t)got));
	xdr_patch_u32(e, datapos, (uint32_t)got);
	if (XDR_PAD((uint32_t)got) > (uint32_t)got)
		memset(e->buf + datapos + 4 + got, 0, XDR_PAD((uint32_t)got) - (uint32_t)got);
	xdr_patch_u32(e, eofpos, (off + (uint64_t)got >= st.size) ? 1 : 0);
	rc = NFS4_OK;
out:
	if (temp_open && c->s->be.ops->release)
		c->s->be.ops->release(c->s->be.ctx, c->cur.ino, befh);
	return rc;
}

/*
 * WRITE: data is handed from the receive buffer straight to the backend write (nothing is
 * accumulated in the server). The client normally sends UNSTABLE4 + COMMIT; for FILE_SYNC4 /
 * DATA_SYNC4 the data is fsynced before replying. The write verifier is a random number chosen at
 * each start.
 */
/*
 * Can the application tell when data failed to reach the disk? (Measured on macOS 12 by cutting
 * power to a USB SSD, with per-request server logs:) after the client receives NFS4ERR_IO /
 * NFS4ERR_NXIO for a COMMIT, the application's fsync() still returns success. Two workarounds
 * were tried and neither helps: not answering that COMMIT and waiting for the forced unmount
 * (fsync still returns success after 2 seconds); answering success but with a different write
 * verifier to force the client to retransmit the WRITEs (the client does not retransmit, it just
 * CLOSEs). So IO is returned here as the protocol prescribes, and in addition, once the device is
 * gone, CLOSE returns IO as well (see op_ok_when_lost) so that close() reports the error.
 */
static uint32_t op_write(struct cctx *c, xdr_dec_t *d, xdr_enc_t *e)
{
	struct nfuse_stateid sid;
	struct nfuse_open *op = NULL;
	struct nfuse_attr st;
	uint64_t off, befh = 0;
	uint32_t stable, len, rc, committed = UNSTABLE4;
	const uint8_t *data;
	int n, temp_open = 0;

	get_stateid(d, &sid);
	off = xdr_get_u64(d);
	stable = xdr_get_u32(d);
	data = xdr_get_opaque_ref(d, c->s->rwsize + 65536, &len);
	if (d->err)
		return NFS4ERR_BADXDR;
	if (!c->have_cur)
		return NFS4ERR_NOFILEHANDLE;
	if (!be_writable(c) || !c->s->be.ops->write)
		return NFS4ERR_ROFS;
	if (c->cur.kind == NFUSE_FH_NAMEDATTR) {
		/*
		 * write [off, off+len) of a named attribute: no truncation (truncation goes
		 * through SETATTR size / the size given at OPEN)
		 */
		const char *an = attr_name(c->s, c->cur.attr_id);

		if (!an)
			return NFS4ERR_STALE;
		if (!xattr_writable(c->s))
			return NFS4ERR_ROFS;
		rc = io_stateid(c, &sid, &op);
		if (rc)
			return rc;
		if (op && !(op->share_access & OPEN4_SHARE_ACCESS_WRITE))
			return NFS4ERR_OPENMODE;
		if (off + len < off || off + len > 0xffffffffull)
			return NFS4ERR_FBIG;
		n = c->s->be.ops->xattr_write(c->s->be.ctx, c->cur.ino, an, data, len, off);
		if (n < 0)
			return nfuse_errno_to_nfs4(n);
		c->s->namedattr_ops++;
		xdr_put_u32(e, (uint32_t)n);
		xdr_put_u32(e, stable == UNSTABLE4 ? UNSTABLE4 : FILE_SYNC4);
		xdr_put_fixed(e, c->s->writeverf, NFS4_VERIFIER_SIZE);
		return NFS4_OK;
	}
	if (c->cur.kind == NFUSE_FH_ADFILE) {
		/*
		 * virtual `._X`: write into the session; a stable write is written back to X's
		 * attributes on the spot
		 */
		if (!xattr_writable(c->s))
			return NFS4ERR_ROFS;
		rc = io_stateid(c, &sid, &op);
		if (rc)
			return rc;
		if (op && !(op->share_access & OPEN4_SHARE_ACCESS_WRITE))
			return NFS4ERR_OPENMODE;
		n = nfuse_adf_write(c->s, c->cur.ino, data, len, off);
		if (n < 0)
			return nfuse_errno_to_nfs4(n);
		if (stable != UNSTABLE4 && !adf_opens(c->s, c->cur.ino)) {
			int r = nfuse_adf_apply(c->s, c->cur.ino);
			if (r)
				return nfuse_errno_to_nfs4(r);
		}
		xdr_put_u32(e, (uint32_t)n);
		xdr_put_u32(e, stable == UNSTABLE4 ? UNSTABLE4 : FILE_SYNC4);
		xdr_put_fixed(e, c->s->writeverf, NFS4_VERIFIER_SIZE);
		return NFS4_OK;
	}
	if (c->cur.kind != NFUSE_FH_NODE)
		return is_pseudo(&c->cur) ? NFS4ERR_ISDIR : NFS4ERR_INVAL;
	rc = cur_attr(c, &c->cur, &st);
	if (rc)
		return rc;
	if (S_ISDIR(st.mode))
		return NFS4ERR_ISDIR;
	if (!S_ISREG(st.mode))
		return NFS4ERR_INVAL;
	if (off + len < off || off + len > 0x7fffffffffffffffull)
		return NFS4ERR_FBIG;
	rc = io_stateid(c, &sid, &op);
	if (rc)
		return rc;
	if (op) {
		if (!(op->share_access & OPEN4_SHARE_ACCESS_WRITE) || !op->bewrite)
			return NFS4ERR_OPENMODE;
		befh = op->befh;
	} else {
		/* anonymous write: temporary read-write open */
		int r = c->s->be.ops->open ?
			c->s->be.ops->open(c->s->be.ctx, c->cur.ino, O_RDWR, &befh) : 0;
		if (r)
			return nfuse_errno_to_nfs4(r);
		temp_open = 1;
	}
	n = len ? c->s->be.ops->write(c->s->be.ctx, c->cur.ino, befh, data, len, off) : 0;
	if (n < 0) {
		rc = nfuse_errno_to_nfs4(n);
		goto out;
	}
	if (stable != UNSTABLE4 && n > 0) {
		int r = c->s->be.ops->fsync ?
			c->s->be.ops->fsync(c->s->be.ctx, c->cur.ino, befh,
					    stable == DATA_SYNC4) : 0;
		if (r) {
			rc = nfuse_errno_to_nfs4(r);
			goto out;
		}
		committed = stable == DATA_SYNC4 ? DATA_SYNC4 : FILE_SYNC4;
		c->s->stable_writes++;
	} else if (stable != UNSTABLE4) {
		committed = stable;
	}
	c->s->write_bytes += (uint64_t)n;
	if (c->s->verbose && c->s->log)
		c->s->log("WRITE ino=%llu off=%llu len=%u stable=%u stateid=%s -> %d",
			  (unsigned long long)c->cur.ino, (unsigned long long)off, len,
			  stable, op ? "open" : "special", n);
	xdr_put_u32(e, (uint32_t)n);
	xdr_put_u32(e, committed);
	xdr_put_fixed(e, c->s->writeverf, NFS4_VERIFIER_SIZE);
	rc = NFS4_OK;
out:
	if (temp_open && c->s->be.ops->release)
		c->s->be.ops->release(c->s->be.ctx, c->cur.ino, befh);
	return rc;
}

/*
 * COMMIT: call the backend fsync (using an open handle if there is one) and return the write
 * verifier
 */
static uint32_t op_commit(struct cctx *c, xdr_dec_t *d, xdr_enc_t *e)
{
	struct nfuse_attr st;
	struct nfuse_open *op, *best = NULL;
	uint32_t rc;
	int r;

	(void)xdr_get_u64(d);		/* offset */
	(void)xdr_get_u32(d);		/* count: the whole file is flushed together */
	if (d->err)
		return NFS4ERR_BADXDR;
	if (!c->have_cur)
		return NFS4ERR_NOFILEHANDLE;
	if (!be_writable(c))
		return NFS4ERR_ROFS;
	if (c->cur.kind == NFUSE_FH_ADFILE) {
		r = adf_opens(c->s, c->cur.ino) ? 0 : nfuse_adf_apply(c->s, c->cur.ino);
		if (r)
			return nfuse_errno_to_nfs4(r);
		xdr_put_fixed(e, c->s->writeverf, NFS4_VERIFIER_SIZE);
		return NFS4_OK;
	}
	if (c->cur.kind == NFUSE_FH_NAMEDATTR) {
		/* named attribute: flushing follows the owning file */
		r = c->s->be.ops->fsync ?
		    c->s->be.ops->fsync(c->s->be.ctx, c->cur.ino, 0, 0) : 0;
		if (r)
			return nfuse_errno_to_nfs4(r);
		xdr_put_fixed(e, c->s->writeverf, NFS4_VERIFIER_SIZE);
		return NFS4_OK;
	}
	if (c->cur.kind != NFUSE_FH_NODE)
		return is_pseudo(&c->cur) ? NFS4ERR_ISDIR : NFS4ERR_INVAL;
	rc = cur_attr(c, &c->cur, &st);
	if (rc)
		return rc;
	if (S_ISDIR(st.mode))
		return NFS4ERR_ISDIR;
	if (!S_ISREG(st.mode))
		return NFS4ERR_INVAL;
	for (op = c->s->opens; op; op = op->next)
		if (op->kind == NFUSE_FH_NODE && op->ino == c->cur.ino &&
		    (!best || (op->bewrite && !best->bewrite)))
			best = op;
	r = c->s->be.ops->fsync ?
	    c->s->be.ops->fsync(c->s->be.ctx, c->cur.ino, best ? best->befh : 0, 0) : 0;
	if (r)
		return r == -ENXIO ? NFS4ERR_IO : nfuse_errno_to_nfs4(r);
	c->s->commits++;
	xdr_put_fixed(e, c->s->writeverf, NFS4_VERIFIER_SIZE);
	return NFS4_OK;
}

/* ---------- Byte-range locks ---------- */

/* offset/length -> [off,end); a length of all ones means to end of file */
static uint32_t lock_range(uint64_t off, uint64_t len, uint64_t *end)
{
	if (len == 0)
		return NFS4ERR_INVAL;
	if (len == UINT64_MAX) {
		*end = UINT64_MAX;
		return NFS4_OK;
	}
	if (off + len < off)
		return NFS4ERR_INVAL;
	*end = off + len;
	return NFS4_OK;
}

static uint32_t lt_type(uint32_t locktype)
{
	/* READ_LT=1 / READW_LT=3 are read locks, WRITE_LT=2 / WRITEW_LT=4 are write locks */
	return (locktype == 2 || locktype == 4) ? NFUSE_LT_WRITE : NFUSE_LT_READ;
}

/* LOCK4denied: the conflicting range and its holder */
static void put_denied(struct cctx *c, xdr_enc_t *e, const struct nfuse_lockrange *r,
		       const struct nfuse_lockowner *lo)
{
	xdr_put_u64(e, r->off);
	xdr_put_u64(e, r->end == UINT64_MAX ? UINT64_MAX : r->end - r->off);
	xdr_put_u32(e, r->type == NFUSE_LT_WRITE ? 2 : 1);
	xdr_put_u64(e, lo->clientid);
	xdr_put_opaque(e, lo->owner, lo->ownerlen);
	c->keep_err_body = 1;
}

static uint32_t op_lock(struct cctx *c, xdr_dec_t *d, xdr_enc_t *e)
{
	uint32_t locktype, rc, ownerlen = 0, open_seqid = 0, lock_seqid;
	int reclaim, newowner, r;
	uint64_t off, len, end, clientid = 0;
	struct nfuse_stateid osid, lsid;
	const uint8_t *owner = NULL;
	struct nfuse_lock *lk = NULL;
	struct nfuse_lockowner *lo, *holder;
	struct nfuse_lockrange *cf;
	struct nfuse_open *op;
	nfuse_ino_t lkey;

	locktype = xdr_get_u32(d);
	reclaim = xdr_get_bool(d);
	off = xdr_get_u64(d);
	len = xdr_get_u64(d);
	newowner = xdr_get_bool(d);
	if (newowner) {
		open_seqid = xdr_get_u32(d);
		get_stateid(d, &osid);
		lock_seqid = xdr_get_u32(d);
		clientid = xdr_get_u64(d);
		owner = xdr_get_opaque_ref(d, NFS4_OPAQUE_LIMIT, &ownerlen);
	} else {
		get_stateid(d, &lsid);
		lock_seqid = xdr_get_u32(d);
	}
	if (d->err)
		return NFS4ERR_BADXDR;
	if (!c->have_cur)
		return NFS4ERR_NOFILEHANDLE;
	if (reclaim) {
		/*
		 * no grace period: like OPEN(CLAIM_PREVIOUS), answer NO_GRACE first (see op_open)
		 */
		c->s->no_grace++;
		return NFS4ERR_NO_GRACE;
	}
	if (newowner) {
		/*
		 * new lock-owner: sequencing follows the open-owner (open_seqid), with lock_seqid
		 * as the starting point
		 */
		rc = nfuse_check_clientid(c->s, clientid);
		if (rc)
			return rc;
		rc = nfuse_open_find(c->s, &osid, &op);
		if (rc)
			return rc;
		r = nfuse_seq_check(&op->owner->seq, open_seqid, OP_LOCK);
		if (r == 1)
			return seq_replay(c, &op->owner->seq, e);
		if (r < 0) {
			c->s->bad_seqid++;
			return NFS4ERR_BAD_SEQID;
		}
		seq_will_record(c, &op->owner->seq, open_seqid);
		lo = nfuse_lockowner_get(c->s, clientid, owner, ownerlen, 1);
		if (!lo)
			return NFS4ERR_RESOURCE;
		seq_will_record(c, &lo->seq, lock_seqid);
		rc = nfuse_stateid_check_seq(&op->sid, &osid);
		if (rc)
			return rc;
	} else {
		rc = nfuse_lock_find(c->s, &lsid, &lk);
		if (rc)
			return rc;
		lo = lk->lo;
		lo->last_used = time(NULL);
		r = nfuse_seq_check(&lo->seq, lock_seqid, OP_LOCK);
		if (r == 1)
			return seq_replay(c, &lo->seq, e);
		if (r < 0) {
			c->s->bad_seqid++;
			return NFS4ERR_BAD_SEQID;
		}
		seq_will_record(c, &lo->seq, lock_seqid);
		rc = nfuse_stateid_check_seq(&lk->sid, &lsid);
		if (rc)
			return rc;
		/* the original open state may have been closed: attach to the current one */
		op = nfuse_lock_attach(c->s, lk);
		if (!op)
			return NFS4ERR_BAD_STATEID;
	}
	if (c->cur.kind != NFUSE_FH_NODE && c->cur.kind != NFUSE_FH_NAMEDATTR &&
	    c->cur.kind != NFUSE_FH_ADFILE)
		return NFS4ERR_INVAL;
	rc = lock_range(off, len, &end);
	if (rc)
		return rc;
	lkey = nfuse_lock_key(c->cur.kind, c->cur.ino, c->cur.attr_id);
	if (newowner) {
		if (!open_matches(op, &c->cur))
			return NFS4ERR_BAD_STATEID;
		lk = nfuse_lock_get(c->s, lo, op, lkey);
		if (!lk)
			return NFS4ERR_RESOURCE;
	} else if (lk->ino != lkey) {
		return NFS4ERR_BAD_STATEID;
	}
	cf = nfuse_lock_conflict(c->s, lkey, lo, off, end, lt_type(locktype),
				&holder);
	if (cf) {
		put_denied(c, e, cf, holder);
		return NFS4ERR_DENIED;
	}
	if (nfuse_lock_add(lk, off, end, lt_type(locktype)))
		return NFS4ERR_RESOURCE;
	lk->sid.seqid++;
	put_stateid(e, &lk->sid);
	return NFS4_OK;
}

static uint32_t op_lockt(struct cctx *c, xdr_dec_t *d, xdr_enc_t *e)
{
	uint32_t locktype, ownerlen, rc;
	uint64_t off, len, end, clientid;
	const uint8_t *owner;
	struct nfuse_lockowner *lo, *holder;
	struct nfuse_lockrange *cf;

	locktype = xdr_get_u32(d);
	off = xdr_get_u64(d);
	len = xdr_get_u64(d);
	clientid = xdr_get_u64(d);
	owner = xdr_get_opaque_ref(d, NFS4_OPAQUE_LIMIT, &ownerlen);
	if (d->err)
		return NFS4ERR_BADXDR;
	if (!c->have_cur)
		return NFS4ERR_NOFILEHANDLE;
	if (c->cur.kind != NFUSE_FH_NODE && c->cur.kind != NFUSE_FH_NAMEDATTR &&
	    c->cur.kind != NFUSE_FH_ADFILE)
		return NFS4ERR_INVAL;
	rc = lock_range(off, len, &end);
	if (rc)
		return rc;
	lo = nfuse_lockowner_get(c->s, clientid, owner, ownerlen, 0);
	cf = nfuse_lock_conflict(c->s, nfuse_lock_key(c->cur.kind, c->cur.ino, c->cur.attr_id),
				lo, off, end, lt_type(locktype), &holder);
	if (cf) {
		put_denied(c, e, cf, holder);
		return NFS4ERR_DENIED;
	}
	return NFS4_OK;
}

static uint32_t op_locku(struct cctx *c, xdr_dec_t *d, xdr_enc_t *e)
{
	struct nfuse_stateid sid;
	struct nfuse_lock *lk;
	uint64_t off, len, end;
	uint32_t rc, seqid;
	int r;

	(void)xdr_get_u32(d);		/* locktype */
	seqid = xdr_get_u32(d);
	get_stateid(d, &sid);
	off = xdr_get_u64(d);
	len = xdr_get_u64(d);
	if (d->err)
		return NFS4ERR_BADXDR;
	if (!c->have_cur)
		return NFS4ERR_NOFILEHANDLE;
	rc = nfuse_lock_find(c->s, &sid, &lk);
	if (rc)
		return rc;
	r = nfuse_seq_check(&lk->lo->seq, seqid, OP_LOCKU);
	if (r == 1)
		return seq_replay(c, &lk->lo->seq, e);
	if (r < 0) {
		c->s->bad_seqid++;
		return NFS4ERR_BAD_SEQID;
	}
	seq_will_record(c, &lk->lo->seq, seqid);
	rc = nfuse_stateid_check_seq(&lk->sid, &sid);
	if (rc)
		return rc;
	rc = lock_range(off, len, &end);
	if (rc)
		return rc;
	if (lk->ino != nfuse_lock_key(c->cur.kind, c->cur.ino, c->cur.attr_id))
		return NFS4ERR_BAD_STATEID;
	if (nfuse_lock_remove(lk, off, end))
		return NFS4ERR_RESOURCE;
	lk->sid.seqid++;
	put_stateid(e, &lk->sid);
	return NFS4_OK;
}

static uint32_t op_release_lockowner(struct cctx *c, xdr_dec_t *d, xdr_enc_t *e)
{
	uint64_t clientid;
	const uint8_t *owner;
	uint32_t n;

	(void)e;
	clientid = xdr_get_u64(d);
	owner = xdr_get_opaque_ref(d, NFS4_OPAQUE_LIMIT, &n);
	if (d->err)
		return NFS4ERR_BADXDR;
	return nfuse_lockowner_release(c->s, clientid, owner, n);
}

/* ---------- Attribute changes / create, remove, rename ---------- */

/*
 * SETATTR: the result carries the attrsset bitmap whether it succeeds or fails (RFC 7530 section
 * 16.32)
 */
static uint32_t op_setattr(struct cctx *c, xdr_dec_t *d, xdr_enc_t *e)
{
	struct nfuse_stateid sid;
	struct nfuse_sattr sa;
	struct nfuse_attr st;
	struct nfuse_open *op = NULL;
	uint32_t set[NFS4_BITMAP_WORDS], empty[NFS4_BITMAP_WORDS] = { 0 };
	uint32_t rc;
	int r;

	get_stateid(d, &sid);
	rc = get_sattr(d, &sa, set);
	c->keep_err_body = 1;
	if (rc == NFS4ERR_BADXDR || d->err)
		goto fail;
	if (!c->have_cur) {
		rc = NFS4ERR_NOFILEHANDLE;
		goto fail;
	}
	if (!be_writable(c) || !c->s->be.ops->setattr) {
		rc = NFS4ERR_ROFS;
		goto fail;
	}
	if (rc)
		goto fail;
	if (c->cur.kind == NFUSE_FH_NAMEDATTR) {
		/*
		 * Named attribute: only the size is meaningful (truncation); times / permissions
		 * and the like have nowhere to be stored and are reported as set (the client sets
		 * mtime along with writing an attribute, and refusing would fail the whole
		 * setxattr).
		 */
		const char *an = attr_name(c->s, c->cur.attr_id);

		if (!an) {
			rc = NFS4ERR_STALE;
			goto fail;
		}
		if (sa.mask & NFUSE_SA_SIZE) {
			rc = io_stateid(c, &sid, &op);
			if (rc)
				goto fail;
			r = c->s->be.ops->xattr_truncate ?
			    c->s->be.ops->xattr_truncate(c->s->be.ctx, c->cur.ino, an, sa.size) :
			    -EROFS;
			if (r) {
				rc = nfuse_errno_to_nfs4(r);
				goto fail;
			}
		}
		c->s->namedattr_ops++;
		xdr_put_bitmap(e, set, NFS4_BITMAP_WORDS);
		return NFS4_OK;
	}
	if (c->cur.kind == NFUSE_FH_ATTRDIR) {
		xdr_put_bitmap(e, set, NFS4_BITMAP_WORDS);
		return NFS4_OK;		/* the attribute directory itself has nothing that can be changed */
	}
	if (c->cur.kind == NFUSE_FH_ADFILE) {
		/*
		 * virtual `._X`: only the size is meaningful; times / permissions have nowhere to
		 * be stored and are reported as set
		 */
		if (sa.mask & NFUSE_SA_SIZE) {
			r = nfuse_adf_truncate(c->s, c->cur.ino, sa.size);
			if (r) {
				rc = nfuse_errno_to_nfs4(r);
				goto fail;
			}
		}
		xdr_put_bitmap(e, set, NFS4_BITMAP_WORDS);
		return NFS4_OK;
	}
	if (c->cur.kind != NFUSE_FH_NODE) {
		rc = is_pseudo(&c->cur) ? NFS4ERR_ROFS : NFS4ERR_INVAL;
		goto fail;
	}
	rc = cur_attr(c, &c->cur, &st);
	if (rc)
		goto fail;
	if (sa.mask & NFUSE_SA_SIZE) {
		if (S_ISDIR(st.mode)) {
			rc = NFS4ERR_ISDIR;
			goto fail;
		}
		if (!S_ISREG(st.mode)) {
			rc = NFS4ERR_INVAL;
			goto fail;
		}
		rc = io_stateid(c, &sid, &op);
		if (rc)
			goto fail;
		if (op && !(op->share_access & OPEN4_SHARE_ACCESS_WRITE)) {
			rc = NFS4ERR_OPENMODE;
			goto fail;
		}
	}
	if (!sa.mask) {
		xdr_put_bitmap(e, set, NFS4_BITMAP_WORDS);
		return NFS4_OK;		/* only something like system=false was set: nothing to do */
	}
	r = c->s->be.ops->setattr(c->s->be.ctx, c->cur.ino,
				  op && op->bewrite ? op->befh : 0, &sa);
	if (c->s->verbose && c->s->log)
		c->s->log("SETATTR ino=%llu mask=%#x size=%llu mode=%o uid=%u gid=%u "
			  "atime=%ld.%09ld mtime=%ld.%09ld crtime=%ld -> %d",
			  (unsigned long long)c->cur.ino, sa.mask,
			  (unsigned long long)sa.size, sa.mode, sa.uid, sa.gid,
			  (long)sa.atime.tv_sec, sa.atime.tv_nsec,
			  (long)sa.mtime.tv_sec, sa.mtime.tv_nsec,
			  (long)sa.crtime.tv_sec, r);
	if (r) {
		rc = setattr_err(r);
		goto fail;
	}
	xdr_put_bitmap(e, set, NFS4_BITMAP_WORDS);
	return NFS4_OK;
fail:
	xdr_put_bitmap(e, empty, NFS4_BITMAP_WORDS);
	return rc;
}

/*
 * Set the remaining attributes after creating an object (mode was already given at creation; a
 * failure is only logged and removed from attrset)
 */
static void apply_rest(struct cctx *c, nfuse_ino_t ino, const struct nfuse_sattr *sa,
		       uint32_t attrset[NFS4_BITMAP_WORDS], int islnk)
{
	struct nfuse_sattr rest = *sa;
	int r, k;

	sattr_bits(sa, attrset);
	rest.mask &= ~(NFUSE_SA_MODE | NFUSE_SA_SIZE);
	if (islnk)
		rest.mask &= NFUSE_SA_UID | NFUSE_SA_GID;
	if (!rest.mask)
		return;
	r = c->s->be.ops->setattr(c->s->be.ctx, ino, 0, &rest);
	if (r) {
		uint32_t rb[NFS4_BITMAP_WORDS];

		if (c->s->log)
			c->s->log("failed to set attributes after CREATE: errno %d (mask %#x)", -r, rest.mask);
		sattr_bits(&rest, rb);
		for (k = 0; k < NFS4_BITMAP_WORDS; k++)
			attrset[k] &= ~rb[k];
	}
}

/* CREATE: directories, symbolic links, devices / FIFOs / sockets (regular files go through OPEN) */
static uint32_t op_create(struct cctx *c, xdr_dec_t *d, xdr_enc_t *e)
{
	uint32_t type, linklen = 0, spec1 = 0, spec2 = 0, namerc, sattr_rc;
	uint32_t attrset[NFS4_BITMAP_WORDS] = { 0 }, dummy[NFS4_BITMAP_WORDS], rc;
	const uint8_t *link = NULL;
	char name[256], target[4097];
	struct nfuse_sattr sa;
	struct nfuse_attr dst;
	struct nfuse_fh dirfh;
	uint64_t before, after;
	nfuse_ino_t ino = 0;
	uint32_t mode;
	int r;

	type = xdr_get_u32(d);
	switch (type) {
	case NF4LNK:
		link = xdr_get_opaque_ref(d, 4096, &linklen);
		break;
	case NF4BLK:
	case NF4CHR:
		spec1 = xdr_get_u32(d);
		spec2 = xdr_get_u32(d);
		break;
	case NF4SOCK:
	case NF4FIFO:
	case NF4DIR:
		break;
	default:
		if (!d->err)
			return NFS4ERR_BADTYPE;
	}
	namerc = get_component(d, name, sizeof(name));
	if (namerc == NFS4ERR_BADXDR || d->err)
		return NFS4ERR_BADXDR;
	sattr_rc = get_sattr(d, &sa, dummy);
	if (sattr_rc == NFS4ERR_BADXDR)
		return sattr_rc;
	if (!c->have_cur)
		return NFS4ERR_NOFILEHANDLE;
	if (namerc)
		return namerc;
	if (!be_writable(c))
		return NFS4ERR_ROFS;
	rc = need_dir(c, &dst);
	if (rc)
		return rc;
	if (c->cur.kind != NFUSE_FH_NODE)
		return is_pseudo(&c->cur) ? NFS4ERR_ROFS : NFS4ERR_NOTSUPP;
	if (sattr_rc)
		return sattr_rc;
	dirfh = c->cur;
	before = dst.change;
	mode = (sa.mask & NFUSE_SA_MODE) ? sa.mode : (type == NF4DIR ? 0755 : 0644);
	switch (type) {
	case NF4DIR:
		r = c->s->be.ops->mkdir ?
		    c->s->be.ops->mkdir(c->s->be.ctx, dirfh.ino, name, mode, &ino) : -EROFS;
		break;
	case NF4LNK:
		if (linklen == 0)
			return NFS4ERR_INVAL;
		memcpy(target, link, linklen);
		target[linklen] = 0;
		if (memchr(target, 0, linklen))
			return NFS4ERR_INVAL;
		r = c->s->be.ops->symlink ?
		    c->s->be.ops->symlink(c->s->be.ctx, dirfh.ino, name, target, &ino) :
		    -EROFS;
		break;
	default: {
		uint32_t t = type == NF4BLK ? S_IFBLK : type == NF4CHR ? S_IFCHR :
			     type == NF4SOCK ? S_IFSOCK : S_IFIFO;
		r = c->s->be.ops->mknod ?
		    c->s->be.ops->mknod(c->s->be.ctx, dirfh.ino, name, t | mode,
					makedev(spec1, spec2), &ino) : -ENOTSUP;
		break;
	}
	}
	if (r)
		return r == -ENOTSUP ? NFS4ERR_BADTYPE : nfuse_errno_to_nfs4(r);
	apply_rest(c, ino, &sa, attrset, type == NF4LNK);
	if (type == NF4LNK) {
		int k;
		uint32_t mb[NFS4_BITMAP_WORDS] = { 0 };

		bm_set(mb, FATTR4_MODE);	/* a symbolic link's mode is not determined by the creation arguments */
		for (k = 0; k < NFS4_BITMAP_WORDS; k++)
			attrset[k] &= ~mb[k];
	}
	after = dir_change(c, &dirfh);
	put_change_info(e, before, after);
	xdr_put_bitmap(e, attrset, NFS4_BITMAP_WORDS);
	set_node_fh(c, &c->cur, ino, dirfh.ino);
	return NFS4_OK;
}

static uint32_t op_remove(struct cctx *c, xdr_dec_t *d, xdr_enc_t *e)
{
	char name[256];
	struct nfuse_attr dst;
	uint64_t before, after;
	uint32_t rc = get_component(d, name, sizeof(name));
	int r;

	if (rc == NFS4ERR_BADXDR)
		return rc;
	if (!c->have_cur)
		return NFS4ERR_NOFILEHANDLE;
	if (rc)
		return rc;
	if (!be_writable(c) || !c->s->be.ops->remove)
		return NFS4ERR_ROFS;
	rc = need_dir(c, &dst);
	if (rc)
		return rc;
	if (c->cur.kind == NFUSE_FH_ATTRDIR) {
		/* remove a named attribute */
		if (!xattr_writable(c->s) || !c->s->be.ops->removexattr)
			return NFS4ERR_ROFS;
		before = dst.change;
		r = c->s->be.ops->removexattr(c->s->be.ctx, c->cur.ino, name);
		if (r)
			return nfuse_errno_to_nfs4(r);
		c->s->namedattr_ops++;
		after = dir_change(c, &c->cur);
		put_change_info(e, before, after);
		return NFS4_OK;
	}
	if (c->cur.kind != NFUSE_FH_NODE)
		return is_pseudo(&c->cur) ? NFS4ERR_ROFS : NFS4ERR_NOTSUPP;
	before = dst.change;
	{
		nfuse_ino_t x;
		int ex;

		if (c->s->adfile && nfuse_adf_alias(c->s, c->cur.ino, name, &x)) {
			/*
			 * removing the rename alias `._A`: those are B's attributes and must not
			 * be removed; just drop the alias
			 */
			if (c->s->verbose && c->s->log)
				c->s->log("REMOVE alias %s (attributes of inode %llu left untouched)", name, (unsigned long long)x);
			nfuse_adf_alias_drop(c->s, c->cur.ino, name);
			c->s->adf_alias_removes++;
			put_change_info(e, before, before + 1);
			return NFS4_OK;
		}
		if (adf_name(c, &c->cur, name, &x, &ex) && ex) {
			/*
			 * removing the virtual `._X` = removing all of X's extended attributes
			 * (an old on-disk `._X` is migrated and removed along with them by the
			 * backend)
			 */
			r = nfuse_adf_remove(c->s, x);
			if (r)
				return nfuse_errno_to_nfs4(r);
			after = dir_change(c, &c->cur);
			put_change_info(e, before, after + 1);
			return NFS4_OK;
		}
	}
	if (c->s->adimgs) {
		/*
		 * an ordinary file X is being removed while its virtual `._X` is being written:
		 * discard the session (the attributes go away together with X)
		 */
		nfuse_ino_t gone;

		if (c->s->be.ops->lookup(c->s->be.ctx, c->cur.ino, name, &gone) == 0)
			nfuse_adf_forget(c->s, gone);
	}
	r = c->s->be.ops->remove(c->s->be.ctx, c->cur.ino, name);
	if (r)
		return nfuse_errno_to_nfs4(r);
	after = dir_change(c, &c->cur);
	put_change_info(e, before, after);
	return NFS4_OK;
}

/* RENAME: the saved handle is the source directory, the current handle is the target directory */
static uint32_t op_rename(struct cctx *c, xdr_dec_t *d, xdr_enc_t *e)
{
	char oldname[256], newname[256];
	struct nfuse_attr sst, tst;
	uint64_t sb, sa, tb, ta;
	uint32_t rc1 = get_component(d, oldname, sizeof(oldname));
	uint32_t rc2 = get_component(d, newname, sizeof(newname));
	uint32_t rc;
	int r;

	if (rc1 == NFS4ERR_BADXDR || rc2 == NFS4ERR_BADXDR)
		return NFS4ERR_BADXDR;
	if (!c->have_cur || !c->have_saved)
		return NFS4ERR_NOFILEHANDLE;
	if (rc1)
		return rc1;
	if (rc2)
		return rc2;
	if (!be_writable(c) || !c->s->be.ops->rename)
		return NFS4ERR_ROFS;
	rc = need_dir_fh(c, &c->saved, &sst);
	if (rc)
		return rc;
	rc = need_dir_fh(c, &c->cur, &tst);
	if (rc)
		return rc;
	if (c->saved.kind != NFUSE_FH_NODE || c->cur.kind != NFUSE_FH_NODE)
		return NFS4ERR_XDEV;
	sb = sst.change;
	tb = tst.change;
	{
		nfuse_ino_t xa, xb;
		int exa, exb;

		if (adf_name(c, &c->saved, oldname, &xa, &exa) && exa) {
			/*
			 * renaming the virtual `._A`: only `._B` (with B existing) is accepted ->
			 * the attributes move from A to B; any other name gets XDEV (mv falls
			 * back to copy + delete)
			 */
			nfuse_ino_t ya;
			int alias = nfuse_adf_alias(c->s, c->saved.ino, oldname, &ya);

			if (!adf_name(c, &c->cur, newname, &xb, &exb))
				return NFS4ERR_XDEV;
			if (alias) {
				/*
				 * `._A` is a rename alias (A was just renamed to B, so `._A` is
				 * really B's): renaming it to `._B` is the same file and does
				 * nothing; renaming it to some other `._C` copies B's attributes
				 * over (B's own attributes are left untouched)
				 */
				if (c->s->verbose && c->s->log)
					c->s->log("RENAME alias %s -> %s (inode %llu -> %llu)", oldname, newname,
						  (unsigned long long)xa, (unsigned long long)xb);
				nfuse_adf_alias_drop(c->s, c->saved.ino, oldname);
				if (xa != xb) {
					r = nfuse_adf_copy(c->s, xa, xb);
					if (r)
						return nfuse_errno_to_nfs4(r);
				}
			} else if (xa != xb) {
				r = nfuse_adf_move(c->s, xa, xb);
				if (r)
					return nfuse_errno_to_nfs4(r);
			}
			put_change_info(e, sb, sb + 1);
			put_change_info(e, tb, tb + 1);
			return NFS4_OK;
		}
	}
	r = c->s->be.ops->rename(c->s->be.ctx, c->saved.ino, oldname, c->cur.ino,
				 newname);
	if (r)
		return nfuse_errno_to_nfs4(r);
	if (c->s->adfile && !nfuse_adf_target(oldname)) {
		/*
		 * record the rename alias: the macOS 27 client will next write `._<old name>`
		 * under the old name (see adfile.h)
		 */
		nfuse_ino_t y;

		if (c->s->be.ops->lookup(c->s->be.ctx, c->cur.ino, newname, &y) == 0)
			nfuse_adf_renamed(c->s, c->saved.ino, oldname, c->cur.ino, newname, y);
	}
	sa = dir_change(c, &c->saved);
	ta = c->saved.ino == c->cur.ino ? sa : dir_change(c, &c->cur);
	put_change_info(e, sb, sa);
	put_change_info(e, tb, ta);
	return NFS4_OK;
}

/*
 * LINK: the saved handle is the existing object, the current handle is the target directory (and
 * remains the target directory afterwards)
 */
static uint32_t op_link(struct cctx *c, xdr_dec_t *d, xdr_enc_t *e)
{
	char name[256];
	struct nfuse_attr dst, st;
	uint64_t before, after;
	uint32_t rc = get_component(d, name, sizeof(name));
	int r;

	if (rc == NFS4ERR_BADXDR)
		return rc;
	if (!c->have_cur || !c->have_saved)
		return NFS4ERR_NOFILEHANDLE;
	if (rc)
		return rc;
	if (!be_writable(c) || !c->s->be.ops->link)
		return NFS4ERR_ROFS;
	rc = need_dir(c, &dst);
	if (rc)
		return rc;
	if (c->cur.kind != NFUSE_FH_NODE || c->saved.kind != NFUSE_FH_NODE)
		return NFS4ERR_XDEV;
	rc = cur_attr(c, &c->saved, &st);
	if (rc)
		return rc;
	if (S_ISDIR(st.mode))
		return NFS4ERR_ISDIR;
	before = dst.change;
	r = c->s->be.ops->link(c->s->be.ctx, c->saved.ino, c->cur.ino, name);
	if (r)
		return nfuse_errno_to_nfs4(r);
	after = dir_change(c, &c->cur);
	put_change_info(e, before, after);
	return NFS4_OK;
}

/*
 * VERIFY / NVERIFY (RFC 7530 sections 16.35 / 16.15): compare, byte by byte, the fattr4 supplied
 * by the client with the values encoded for the current object under the same bitmap. The client
 * uses NVERIFY to decide whether cached named attributes and the like are still fresh. An
 * unsupported attribute in the bitmap gets ATTRNOTSUPP, a write-only one gets INVAL.
 */
static uint32_t verify_common(struct cctx *c, xdr_dec_t *d, int want_same)
{
	uint32_t bm[NFS4_BITMAP_WORDS], sup[NFS4_BITMAP_WORDS], alen, rc, i;
	const uint8_t *ap;
	struct nfuse_attr st;
	xdr_enc_t te;
	int same;

	xdr_get_bitmap(d, bm, NFS4_BITMAP_WORDS);
	ap = xdr_get_opaque_ref(d, 65536, &alen);
	if (d->err)
		return NFS4ERR_BADXDR;
	if (!c->have_cur)
		return NFS4ERR_NOFILEHANDLE;
	nfuse_attr_supported(sup);
	for (i = 0; i < NFS4_BITMAP_WORDS; i++)
		if (bm[i] & ~sup[i])
			return NFS4ERR_ATTRNOTSUPP;
	if (bm[1] & ((1u << (FATTR4_TIME_ACCESS_SET - 32)) |
		     (1u << (FATTR4_TIME_MODIFY_SET - 32))))
		return NFS4ERR_INVAL;
	rc = fh_attr(c, &c->cur, &st, want_of(bm));
	if (rc)
		return rc;
	if (xdr_enc_init(&te, 1024, 65536))
		return NFS4ERR_RESOURCE;
	nfuse_attr_encode(c->s, &te, bm, &c->cur, &st, NFS4_OK);
	if (te.err) {
		xdr_enc_free(&te);
		return NFS4ERR_RESOURCE;
	}
	/*
	 * te holds bitmap + length + values: skip the bitmap and the length and compare the
	 * values only
	 */
	{
		xdr_dec_t td;
		uint32_t tb[NFS4_BITMAP_WORDS], tl;
		const uint8_t *tv;

		xdr_dec_init(&td, te.buf, te.len);
		xdr_get_bitmap(&td, tb, NFS4_BITMAP_WORDS);
		tv = xdr_get_opaque_ref(&td, 65536, &tl);
		same = !td.err && tl == alen && memcmp(tv, ap, alen) == 0;
		xdr_enc_free(&te);
	}
	if (want_same)
		return same ? NFS4_OK : NFS4ERR_NOT_SAME;
	return same ? NFS4ERR_SAME : NFS4_OK;
}

static uint32_t op_verify(struct cctx *c, xdr_dec_t *d, xdr_enc_t *e)
{
	(void)e;
	return verify_common(c, d, 1);
}

static uint32_t op_nverify(struct cctx *c, xdr_dec_t *d, xdr_enc_t *e)
{
	(void)e;
	return verify_common(c, d, 0);
}

static uint32_t op_notsupp(struct cctx *c, xdr_dec_t *d, xdr_enc_t *e)
{
	(void)c; (void)d; (void)e;
	return NFS4ERR_NOTSUPP;
}

typedef uint32_t (*op_fn)(struct cctx *, xdr_dec_t *, xdr_enc_t *);

static const op_fn op_table[NFS4_OP_MAX + 1] = {
	[OP_ACCESS]		= op_access,
	[OP_CLOSE]		= op_close,
	[OP_COMMIT]		= op_commit,
	[OP_CREATE]		= op_create,
	[OP_DELEGPURGE]		= op_notsupp,
	[OP_DELEGRETURN]	= op_notsupp,
	[OP_GETATTR]		= op_getattr,
	[OP_GETFH]		= op_getfh,
	[OP_LINK]		= op_link,
	[OP_LOCK]		= op_lock,
	[OP_LOCKT]		= op_lockt,
	[OP_LOCKU]		= op_locku,
	[OP_LOOKUP]		= op_lookup,
	[OP_LOOKUPP]		= op_lookupp,
	[OP_NVERIFY]		= op_nverify,
	[OP_OPEN]		= op_open,
	[OP_OPENATTR]		= op_openattr,
	[OP_OPEN_CONFIRM]	= op_open_confirm,
	[OP_OPEN_DOWNGRADE]	= op_open_downgrade,
	[OP_PUTFH]		= op_putfh,
	[OP_PUTPUBFH]		= op_putrootfh,
	[OP_PUTROOTFH]		= op_putrootfh,
	[OP_READ]		= op_read,
	[OP_READDIR]		= op_readdir,
	[OP_READLINK]		= op_readlink,
	[OP_REMOVE]		= op_remove,
	[OP_RENAME]		= op_rename,
	[OP_RENEW]		= op_renew,
	[OP_RESTOREFH]		= op_restorefh,
	[OP_SAVEFH]		= op_savefh,
	[OP_SECINFO]		= op_secinfo,
	[OP_SETATTR]		= op_setattr,
	[OP_SETCLIENTID]	= op_setclientid,
	[OP_SETCLIENTID_CONFIRM] = op_setclientid_confirm,
	[OP_VERIFY]		= op_verify,
	[OP_WRITE]		= op_write,
	[OP_RELEASE_LOCKOWNER]	= op_release_lockowner,
};

/* ---------- COMPOUND ---------- */

/*
 * Operations that still execute normally after the device is gone: those that only set the
 * current handle or only touch server state and never file system data (CLOSE / LOCKU and the
 * like release state so that the client can clean up its open files; any file system callbacks
 * they reach fail fast on their own). Everything else gets NFS4ERR_IO.
 */
static int op_ok_when_lost(uint32_t op)
{
	switch (op) {
	case OP_PUTFH: case OP_PUTROOTFH: case OP_PUTPUBFH: case OP_SAVEFH:
	case OP_RESTOREFH: case OP_GETFH: case OP_RENEW: case OP_SETCLIENTID:
	case OP_SETCLIENTID_CONFIRM: case OP_RELEASE_LOCKOWNER:
	case OP_LOCKU: case OP_OPEN_DOWNGRADE: case OP_DELEGRETURN:
		return 1;
	default:
		return 0;
	}
}

/*
 * Test hook (NFUSE_TEST_HANG): this op "hangs" -- it sleeps holding the global lock, and never
 * returns when test_hang_ms is 0 (simulates an unresponsive backend / device, to verify the
 * watchdog). Fires only once.
 */
static void test_hang(struct nfuse_server *s, uint32_t opcode)
{
	uint64_t t0 = nfuse_uptime_ns();

	s->test_hang_op = 0;
	if (s->log)
		s->log("test hook: %s hangs (%s%s)", nfs4_op_name(opcode),
		       s->test_hang_ms ? "for a limited time" : "never returns",
		       s->test_hang_progress ? ", pretending the backend makes progress meanwhile" : "");
	for (;;) {
		usleep(100 * 1000);
		if (s->test_hang_progress)
			s->test_progress++;
		if (s->test_hang_ms && nfuse_uptime_ns() - t0 >= (uint64_t)s->test_hang_ms * 1000000ULL)
			break;
	}
	if (s->log)
		s->log("test hook: %s returned after hanging for %.1f s", nfs4_op_name(opcode),
		       (nfuse_uptime_ns() - t0) / 1e9);
}

uint32_t nfs4_compound_gate(struct nfuse_server *s, xdr_dec_t *d, int *proved, uint32_t *first_op)
{
	uint32_t taglen, nops, op, len;
	const uint8_t *p;
	struct nfuse_fh fh;

	*proved = 0;
	*first_op = 0;
	(void)xdr_get_opaque_ref(d, 1024, &taglen);
	(void)xdr_get_u32(d);
	nops = xdr_get_u32(d);
	if (d->err)
		return NFS4ERR_BADXDR;
	if (!nops)
		return NFS4_OK;
	op = xdr_get_u32(d);
	if (d->err)
		return NFS4ERR_BADXDR;
	*first_op = op;
	switch (op) {
	case OP_RENEW:
	case OP_RELEASE_LOCKOWNER:
	case OP_SETCLIENTID_CONFIRM:
		return nops == 1 ? NFS4_OK : NFS4ERR_ACCESS;
	case OP_SETCLIENTID: {
		/*
		 * The first thing the client does after reconnecting is SETCLIENTID (observed
		 * with macOS 12). Only one whose client id string equals that of the client seen
		 * at mount time is allowed: other local processes do not know this string (it
		 * lives in the kernel and cannot be seen without capturing loopback packets).
		 */
		const uint8_t *id;
		uint32_t idlen, k;

		if (nops != 1)
			return NFS4ERR_ACCESS;
		xdr_skip_opaque(d, NFS4_VERIFIER_SIZE);
		id = xdr_get_opaque_ref(d, NFS4_OPAQUE_LIMIT, &idlen);
		if (!id || d->err)
			return NFS4ERR_BADXDR;
		for (k = 0; k < NFUSE_MAX_CLIENTS; k++)
			if (s->clients[k].used && s->clients[k].confirmed &&
			    s->clients[k].idlen == idlen && memcmp(s->clients[k].id, id, idlen) == 0)
				return NFS4_OK;
		return NFS4ERR_WRONGSEC;
	}
	case OP_PUTFH:
		p = xdr_get_opaque_ref(d, NFS4_FHSIZE, &len);
		if (!p || d->err)
			return NFS4ERR_BADHANDLE;
		if (nfuse_fh_decode(s, p, len, &fh) != NFS4_OK)
			return NFS4ERR_STALE;
		*proved = 1;
		return NFS4_OK;
	default:
		return NFS4ERR_WRONGSEC;
	}
}

int nfs4_compound_fail(xdr_dec_t *d, xdr_enc_t *e, uint32_t status)
{
	const uint8_t *tag;
	uint32_t taglen, nops, op;

	tag = xdr_get_opaque_ref(d, 1024, &taglen);
	(void)xdr_get_u32(d);			/* minorversion */
	nops = xdr_get_u32(d);
	op = nops ? xdr_get_u32(d) : 0;
	if (d->err)
		return -1;
	xdr_put_u32(e, status);
	xdr_put_opaque(e, tag, taglen);
	if (!nops) {
		xdr_put_u32(e, 0);
		return 0;
	}
	xdr_put_u32(e, 1);
	xdr_put_u32(e, (op >= NFS4_OP_MIN && op <= NFS4_OP_MAX) ? op : OP_ILLEGAL);
	xdr_put_u32(e, status);
	return 0;
}

int nfs4_compound(struct nfuse_server *s, const rpc_call_t *call,
		  xdr_dec_t *d, xdr_enc_t *e, unsigned *flags)
{
	struct cctx c;
	const uint8_t *tag;
	uint32_t taglen, minor, nops, i, status = NFS4_OK;
	size_t statpos, cntpos;
	char trace[512];
	size_t tl = 0;

	memset(&c, 0, sizeof(c));
	c.s = s;
	c.cred = &call->cred;
	*flags = 0;

	tag = xdr_get_opaque_ref(d, 1024, &taglen);
	minor = xdr_get_u32(d);
	nops = xdr_get_u32(d);
	if (d->err)
		return -1;

	statpos = xdr_reserve_u32(e);
	xdr_put_opaque(e, tag, taglen);		/* echo the tag back unchanged */
	cntpos = xdr_reserve_u32(e);

	if (minor != 0) {
		/* 4.0 only; a 4.1 client falls back on seeing this */
		xdr_patch_u32(e, statpos, NFS4ERR_MINOR_VERS_MISMATCH);
		return 0;
	}
	if (s->test_reboot) {
		/*
		 * test hook: simulate a server restart (all state dropped, state_boot replaced,
		 * handles stay valid)
		 */
		s->test_reboot = 0;
		nfuse_state_reboot(s);
		if (s->log)
			s->log("test hook: simulating a server restart (#%llu), all old stateids / clientids are now invalid",
			       (unsigned long long)s->reboots);
	}
	trace[0] = 0;
	s->compounds++;
	if (s->be.ops->begin) {
		struct nfuse_cred cr = { call->cred.uid, call->cred.gid };
		s->be.ops->begin(s->be.ctx, &cr);
	}

	for (i = 0; i < nops; i++) {
		uint32_t opcode = xdr_get_u32(d);
		size_t oppos, bodypos;
		op_fn fn;
		int k;

		if (d->err) {
			status = NFS4ERR_BADXDR;
			/* the opcode cannot be read: return an ILLEGAL result as required */
			xdr_put_u32(e, OP_ILLEGAL);
			xdr_put_u32(e, status);
			i++;
			break;
		}
		oppos = e->len;
		if (opcode < NFS4_OP_MIN || opcode > NFS4_OP_MAX || !op_table[opcode]) {
			xdr_put_u32(e, OP_ILLEGAL);
			xdr_put_u32(e, NFS4ERR_OP_ILLEGAL);
			status = NFS4ERR_OP_ILLEGAL;
			i++;
			break;
		}
		fn = op_table[opcode];
		xdr_put_u32(e, opcode);
		xdr_reserve_u32(e);			/* op status */
		bodypos = e->len;
		c.keep_err_body = 0;
		c.nrec = 0;
		c.replayed = 0;
		c.rec_has_other = 0;
		c.open_excl = 0;
		s->cur_op_t0 = nfuse_uptime_ns();
		s->cur_op = opcode;
		if (s->test_hang_op == opcode && !s->dev_lost) {
			if (s->test_hang_skip)
				s->test_hang_skip--;
			else
				test_hang(s, opcode);
		}
		if (s->dev_lost && !op_ok_when_lost(opcode)) {
			/*
			 * the device is gone: do not enter the file system, answer IO directly
			 * (the client hands EIO to the application without retrying)
			 */
			status = NFS4ERR_IO;
			s->lost_replies++;
		} else
			status = fn(&c, d, e);
		s->cur_op = 0;
		if (status == NFS4_OK && d->err)
			status = NFS4ERR_BADXDR;
		if (status != NFS4_OK && !c.keep_err_body)
			xdr_truncate(e, bodypos);	/* on failure keep only opcode + status */
		xdr_patch_u32(e, oppos + 4, status);

		/* seqid: record this result in the owner's replay cache */
		if (!c.replayed && nfuse_seq_bumps(status))
			for (k = 0; k < c.nrec; k++) {
				nfuse_seq_record(c.rec[k], c.rec_seqid[k], opcode, status,
						 e->buf + bodypos,
						 (uint32_t)(e->len - bodypos),
						 opcode == OP_OPEN && status == NFS4_OK ?
						 &c.cur : NULL);
				if (c.rec_has_other)
					memcpy(c.rec[k]->other, c.rec_other,
					       NFS4_OTHER_SIZE);
			}
		if (op_nonidem[opcode])
			*flags |= NFUSE_CF_CACHE;
		/* test hook: after this op succeeds, drop the whole reply */
		if (status == NFS4_OK) {
			int slot = (opcode == OP_OPEN && c.open_excl) ? DROP_OPEN_EXCL :
				   (int)opcode;
			if (s->test_drop[slot] > 0) {
				s->test_drop[slot]--;
				s->test_dropped++;
				*flags |= NFUSE_CF_DROP;
				if (s->log)
					s->log("test hook: dropping the reply to %s (xid=%08x)",
					       slot == DROP_OPEN_EXCL ? "OPEN(EXCLUSIVE4)" :
					       nfs4_op_name(opcode), call->xid);
			}
		}

		s->op_count[opcode]++;
		if (status)
			s->op_err[opcode]++;
		if (s->verbose && tl < sizeof(trace) - 40) {
			if (status)
				tl += (size_t)snprintf(trace + tl, sizeof(trace) - tl,
						       "%s%s=%u", i ? "," : "",
						       nfs4_op_name(opcode), status);
			else
				tl += (size_t)snprintf(trace + tl, sizeof(trace) - tl,
						       "%s%s", i ? "," : "",
						       nfs4_op_name(opcode));
		}
		if (status != NFS4_OK) {
			i++;
			break;
		}
	}
	if (s->be.ops->end)
		s->be.ops->end(s->be.ctx);
	if (i > nops)
		i = nops;
	xdr_patch_u32(e, statpos, status);
	xdr_patch_u32(e, cntpos, i);
	if (s->verbose) {
		if (s->log)
			s->log("xid=%08x uid=%u tag=%.*s ops=%s -> %u",
			       call->xid, call->cred.uid,
			       (int)(taglen > 16 ? 16 : taglen),
			       tag ? (const char *)tag : "", trace, status);
		else
			fprintf(stderr, "[nfuse] xid=%08x uid=%u tag=%.*s ops=%s -> %u\n",
				call->xid, call->cred.uid,
				(int)(taglen > 16 ? 16 : taglen),
				tag ? (const char *)tag : "", trace, status);
	}
	return 0;
}
