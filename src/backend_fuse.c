/*
 * backend_fuse.c - server backend interface (by inode) -> FUSE high-level callbacks (by path)
 *
 * NFS addresses objects by file handle (inode number) while the driver's fuse_operations
 * addresses them by path. Bridging the two takes the following:
 *
 * 1. Node table: inode number -> (parent directory inode number, name); the path is built by
 *    walking the parent chain.
 *    - It has a limit (max_nodes). NFS has no FORGET and a handle the client holds may be
 *      used again at any time, so an evicted node must be recoverable: the file handle
 *      carries the inode number of the directory the object was in when the handle was issued,
 *      and if the node is not in the table at PUTFH time, its name is recovered by inode
 *      number from the parent directory's listing snapshot
 *      (the parent is listed again if there is no snapshot), then getattr confirms the inode number.
 *    - Never evicted: the root directory, open files (nopen>0), directories that still have
 *      child nodes in the table
 *      (nchild>0, otherwise the children's paths could not be built), and nodes used by the current COMPOUND.
 *    - Files are evicted before directories, each least recently used first.
 *    - ESTALE is returned only when recovery fails (the parent was evicted too, or the file was removed).
 *
 * 2. Directory listing snapshots: the driver's readdir ignores offset and lists the whole
 *    directory from the start every time.
 *    The complete listing is cached here, keyed by (directory inode, verifier), and READDIR
 *    pages are served from the snapshot,
 *    so the whole directory is listed only once (linear). The verifier is derived from the
 *    directory's change / mtime / size:
 *    when the directory changes, the old cookieverf sent by the client no longer matches and NOT_SAME is returned.
 *    The total size of snapshots is limited (not counting the one in use); least recently used go first.
 *
 * 3. Attribute cache: a fixed 8192 slots hashed by inode (valid for 1 second by default).
 *    Attributes picked up during READDIR
 *    are used directly by the LOOKUP / GETATTR that follow; several GETATTRs in one COMPOUND call the driver only once.
 *    getxtimes is called in addition when creation time is wanted (only when the client asks for time_create / time_backup).
 *
 * 4. Writing: as soon as a modifying callback has run, the node table, directory snapshots and attribute cache are fixed up:
 *    - create: the node enters the table (pinned for this batch; nopen++ if created open); the parent's snapshot is invalidated and its generation bumped;
 *    - remove: when the canonical name is removed the node is unhooked (or re-attached to another known name if the hard link has one);
 *    - rename: the node is re-attached under the new parent / new name; before the rename the ancestor chain of handles already issued is frozen,
 *      so the handle bytes do not change; a "moved" hint (ino -> new parent + ancestor chain) is also recorded, so that if the node
 *      is evicted later and the client presents the old handle, it can still be found by inode number in the new directory;
 *    - write / setattr: the cached attributes of that inode are invalidated and the node generation bumped.
 *    The node generation (gen) is mixed into the change attribute and the directory verifier: any modification made through this layer is guaranteed to be visible to the client.
 *
 * 5. Equivalent names: a lookup first tries the name as given and, if that fails, looks it up by comparison key in the "directory name index"
 *    (key = fold(NFC(name)), NFC only when case-sensitive); on a hit it carries on with the real on-disk name.
 *    The index is built per directory (one listing) and then maintained incrementally as creates / removes / renames pass through this layer, so
 *    "check it does not exist, then create" paths such as file creation cost one in-memory lookup each time. Total size is limited; least recently used go first.
 *    Names being created / renamed to are converted to NFC if so configured; if a name differing only in case / form already exists it is treated as "already exists".
 *
 * 6. Named attributes and `._` files: named attributes map onto FUSE's xattr callbacks (if the file system registered the extension
 *    callbacks for reading and writing by offset those are used, so even a huge resource fork is never fetched whole). `._X` (AppleDouble) files on existing volumes:
 *    hidden from listings when X exists; when reading X's attributes, whatever the file system does not have is read from `._X`;
 *    before X's attributes are written, the whole `._X` is moved into the file system and then deleted (or kept, with a "migrated" marker recorded);
 *    when X is removed / renamed, `._X` is removed / renamed with it. An orphan `._` whose X does not exist is shown as usual.
 *
 * All functions are called serially under the server's global lock; there is no locking here.
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#define FUSE_USE_VERSION 26
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

#include "fuse.h"
#include "openmacfuse.h"
#include "appledouble.h"
#include "backend_fuse.h"
#include "names.h"
#include "nfs4_state.h"	/* OPENMACFUSE_FH_MAXANC */
#include "synth_ino.h"

/* Marker recorded on a file when ad_clean=0, meaning "`._X` already migrated" (hidden from attribute listings) */
#define BF_AD_MARKER	"org.openmacfuse.appledouble-migrated"

/*
 * Objects without FinderInfo: on the named-attribute channel it reads as 32 zero bytes (and is not listed).
 * Observed: the macOS client's
 * setattrlist(ATTR_CMN_FNDRINFO) first reads the existing FinderInfo, and if that fails it returns ENOATTR without ever sending the write,
 * which breaks NSWorkspace custom icons, SetFile and the like; on HFS+ FinderInfo always "exists" anyway.
 */
#define BF_FINDERINFO	"com.apple.FinderInfo"
/* Same for the resource fork: when one is created through ..namedfork/rsrc the client also requires it to "exist" first (on HFS+ every file has an empty resource fork) */
#define BF_RSRCFORK	"com.apple.ResourceFork"

#define BF_PATH_MAX	16384	/* limit on a built path (NFS looks names up level by level, so it can exceed PATH_MAX) */
#define BF_DEPTH_MAX	4096

/* ---------- Data structures ---------- */

struct bf_node {
	uint64_t	ino;
	uint64_t	parent;
	char		*name;		/* NULL for the root node */
	struct bf_node	*hnext;		/* hash chain */
	struct bf_node	*lprev, *lnext;	/* LRU list (only evictable nodes are on it) */
	uint32_t	nchild;		/* number of nodes in the table that have this one as parent */
	uint32_t	nopen;		/* open count (>0: not evicted) */
	uint32_t	epoch;		/* batch of the last COMPOUND that used it as the "current handle" */
	uint32_t	gen;		/* number of modifications through this layer: mixed into the change attribute and the directory verifier */
	uint64_t	*fchain;	/* frozen handle ancestor chain (keeps issued handles unchanged after a rename), normally NULL */
	uint8_t		nfchain;
	uint8_t		isdir;
	uint8_t		inlru;
};

/*
 * Attribute cache: separate from the node table, fixed-size and direct-mapped (the inode number
 * hashes to a slot; a collision overwrites).
 * Nodes are therefore only a few dozen bytes, so the node table limit matters little for memory;
 * a READDIR page holds a thousand-odd entries, and
 * 8192 slots are enough for the LOOKUP / GETATTR that follow to hit.
 */
#define BF_ACACHE_SLOTS	8192

struct bf_aslot {
	uint64_t	ino;		/* 0 = empty */
	uint64_t	at;		/* monotonic clock when the attributes were fetched (nanoseconds) */
	uint8_t		x;		/* includes the getxtimes result */
	struct openmacfuse_attr a;
};

struct bf_lru {
	struct bf_node	*head, *tail;
};

/* One directory entry in a snapshot */
struct bf_sent {
	uint64_t	ino;
	uint32_t	noff;
	uint16_t	nlen;
	uint16_t	type;		/* mode & S_IFMT */
};

struct bf_snap {
	struct bf_snap	*next;
	uint64_t	dir;
	uint64_t	verf;
	struct bf_sent	*ents;
	uint32_t	n, cap;
	char		*names;
	size_t		nlen, ncap;
	uint32_t	*idx;		/* open-addressing table, inode number -> index+1 (built only when recovering evicted nodes) */
	uint32_t	idxcap;
	uint32_t	nad;		/* number of entries starting with `._` */
	uint32_t	*nidx;		/* name -> index+1 (built only when there are `._` entries and hiding has to be decided) */
	uint32_t	nidxcap;
	uint64_t	last_use;
	int		oom;
};

/*
 * Directory name index (for equivalent-name lookup): one per directory, built from one listing
 * and then maintained incrementally as
 * creates / removes / renames pass through this layer. Bucketed by the hash of the comparison key (chained); removed entries are only marked.
 */
struct bf_dent {
	uint64_t	h;		/* hash of the comparison key */
	uint64_t	ino;
	uint32_t	noff;
	uint32_t	next;		/* next in the same bucket (index+1), 0 ends the chain */
	uint16_t	nlen;
	uint8_t		dead;
};

struct bf_didx {
	struct bf_didx	*next;
	uint64_t	dir;
	struct bf_dent	*e;
	uint32_t	n, cap, nlive;
	char		*names;
	size_t		nlen, ncap;
	uint32_t	*bk;		/* buckets: entry index+1 */
	uint32_t	nbk;
	uint32_t	nad;		/* number of live `._` entries */
	uint32_t	conflicts;	/* names differing only in case found while building the index */
	uint64_t	last_use;
	size_t		bytes;
	int		oom;
};

/* A parsed `._X` (freshness is checked against the `._X`'s inode + mtime + size) */
#define BF_ADC_SLOTS	16
struct bf_adc {
	uint64_t	adino;		/* 0 = empty */
	uint64_t	size;
	struct timespec	mtime;
	uint64_t	stamp;
	struct openmacfuse_ad	ad;
};

struct bf_open {
	struct fuse_file_info	fi;
	uint64_t		ino;
};

struct bf_stats {
	uint64_t	nodes_peak;
	uint64_t	evictions;
	uint64_t	recover_ok;
	uint64_t	recover_fail;
	uint64_t	snap_builds;
	uint64_t	snap_reuse;
	uint64_t	snap_entries;
	uint64_t	getattr_calls;
	uint64_t	getxtimes_calls;
	uint64_t	attr_hits;
	uint64_t	readdir_ents;
	uint64_t	read_calls;
	uint64_t	read_bytes;
	uint64_t	opens;
	uint64_t	writes;
	uint64_t	write_bytes;
	uint64_t	fsyncs;
	uint64_t	creates;
	uint64_t	removes;
	uint64_t	renames;
	uint64_t	links;
	uint64_t	setattrs;
	uint64_t	hint_hits;
	uint64_t	frozen;
	uint64_t	stale_ino;	/* times the object at the path turned out to be a different one (ino mismatch) */
	/* equivalent names */
	uint64_t	didx_builds;	/* directory name indexes built */
	uint64_t	didx_ents;	/* entries listed while building indexes */
	uint64_t	didx_drops;	/* times an index disagreed with the disk and was dropped for rebuilding */
	uint64_t	fold_hits;	/* found by case folding (name as given not found) */
	uint64_t	norm_hits;	/* found where only the NFC / NFD form differed */
	uint64_t	fold_ambig;	/* more than one equivalent name (inexact lookup in a conflicting directory) */
	uint64_t	exist_equiv;	/* creates treated as "already exists" because an equivalent name was there */
	uint64_t	conflict_dirs;	/* directories found to have conflicts at run time */
	uint64_t	conflict_pairs;	/* conflicting pairs found at run time */
	uint64_t	nfc_names;	/* names converted to NFC on create / rename */
	uint64_t	case_renames;	/* renames that only change case / form */
	/* `._` and named attributes */
	uint64_t	ad_hidden;	/* `._X` entries hidden by READDIR */
	uint64_t	ad_parsed;	/* `._X` parses */
	uint64_t	ad_reads;	/* attribute values read out of a `._X` (translation) */
	uint64_t	ad_lists;	/* attribute listings that merged in names from a `._X` */
	uint64_t	ad_migrated;	/* `._X` files migrated into the file system */
	uint64_t	ad_migrated_attrs;
	uint64_t	ad_deleted;	/* `._X` files deleted after migration */
	uint64_t	ad_follow_rm;	/* `._X` files removed along with X */
	uint64_t	ad_follow_mv;	/* `._X` files renamed along with X */
	uint64_t	ad_bad;		/* `._X` files that are not AppleDouble */
	uint64_t	xa_reads;
	uint64_t	xa_read_bytes;
	uint64_t	xa_writes;
	uint64_t	xa_write_bytes;
	uint64_t	xa_truncs;
	uint64_t	xa_removes;
	uint64_t	xa_lists;
	uint64_t	xa_rmw;		/* whole-value read-modify-writes (no extension callbacks) */
	uint64_t	fi_virtual;	/* times an empty value was returned for a missing FinderInfo / resource fork */
};

/*
 * "Moved" table (relocation hints): inode number -> another location last known for it
 * (parent directory + name)
 * and that parent's ancestor chain. Two sources:
 *   - RENAME: the object moved to a new directory. If the node is evicted afterwards, the old handle
 *     the client holds carries
 *     the old ancestor chain, which no longer leads to it; this table is consulted first and the object is looked up by inode number in the new parent.
 *   - hard links (LINK, or a LOOKUP of another name reaching the same inode): once the "canonical" name is removed,
 *     the node is re-attached under the other name recorded here.
 * Fixed size (BF_HINT_SLOTS slots, 4-way set associative: the inode hashes to a set of 4 slots, and a full set evicts
 * its oldest entry); these are only hints, the inode number is always verified on recovery.
 */
#define BF_HINT_SLOTS	8192
#define BF_HINT_WAYS	4

struct bf_hint {
	uint64_t	ino;		/* 0 = empty */
	uint64_t	stamp;		/* insertion order (the smallest is evicted when the set is full) */
	uint64_t	parent;
	char		*name;
	uint8_t		nchain;
	uint64_t	chain[OPENMACFUSE_FH_MAXANC];	/* ancestor chain of parent (nearest first) */
};

struct bf_ctx {
	const struct fuse_operations	*op;
	struct fuse_context		*fctx;
	uint64_t			root;
	uint64_t			salt;
	struct openmacfuse_synth		*synth;	/* assigns inode numbers by path when the file system provides none (synth_ino.c) */

	struct bf_node			**ht;
	size_t				htmask;
	size_t				nnodes;
	size_t				max_nodes;
	struct bf_lru			lru[2];		/* 0 files / 1 directories */
	struct bf_aslot			*acache;	/* BF_ACACHE_SLOTS of them */
	struct bf_hint			*hints;		/* BF_HINT_SLOTS of them */
	uint64_t			hint_clock;
	uint32_t			epoch;

	struct bf_snap			*snaps;
	size_t				snap_bytes;
	size_t				snap_max_bytes;
	uint64_t			snap_clock;

	struct openmacfuse_statfs		sf;
	uint64_t			sf_at;
	int				sf_ok;

	uint64_t			attr_ttl_ns;
	char				*path1;		/* path of the current object / directory */
	char				*path2;		/* path of a directory entry */
	char				*path3;		/* source path of a RENAME */
	char				*path4;		/* path of the `._X` */
	char				*path5;		/* path of the file X a named attribute belongs to */
	void				(*log)(const char *fmt, ...);
	int				fake_xattr;
	int				warned_flags;
	/* names and extended attributes */
	int				namedattr;
	int				adtrans;	/* hide / merge / migrate real on-disk `._X` files (on in both streams and adfile modes) */
	int				ad_clean;
	int				ci;
	int				nfc;
	int				raw_names;
	struct openmacfuse_ext_ops		ext;
	struct bf_didx			*didx;
	size_t				didx_bytes;
	size_t				didx_max;
	uint64_t			didx_clock;
	struct bf_adc			adc[BF_ADC_SLOTS];
	uint64_t			adc_clock;
	struct bf_stats			st;
};

static uint64_t now_ns(void)
{
	return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

static uint64_t mix64(uint64_t x)
{
	x ^= x >> 33;
	x *= 0xff51afd7ed558ccdull;
	x ^= x >> 33;
	x *= 0xc4ceb9fe1a85ec53ull;
	x ^= x >> 33;
	return x;
}

/* ---------- Node table ---------- */

static struct bf_node *node_find(struct bf_ctx *b, uint64_t ino)
{
	struct bf_node *n = b->ht[mix64(ino) & b->htmask];

	while (n && n->ino != ino)
		n = n->hnext;
	return n;
}

static void lru_unlink(struct bf_ctx *b, struct bf_node *n)
{
	struct bf_lru *l;

	if (!n->inlru)
		return;
	l = &b->lru[n->isdir ? 1 : 0];
	if (n->lprev)
		n->lprev->lnext = n->lnext;
	else
		l->head = n->lnext;
	if (n->lnext)
		n->lnext->lprev = n->lprev;
	else
		l->tail = n->lprev;
	n->lprev = n->lnext = NULL;
	n->inlru = 0;
}

static void lru_push_head(struct bf_ctx *b, struct bf_node *n)
{
	struct bf_lru *l = &b->lru[n->isdir ? 1 : 0];

	n->lprev = NULL;
	n->lnext = l->head;
	if (l->head)
		l->head->lprev = n;
	l->head = n;
	if (!l->tail)
		l->tail = n;
	n->inlru = 1;
}

/* Can the node be evicted: not the root, not if open, not if it still has children */
static int evictable(struct bf_ctx *b, const struct bf_node *n)
{
	return n->ino != b->root && n->nopen == 0 && n->nchild == 0;
}

/* Used once: move to the head of the LRU; a non-zero mark records it as in use by this batch (not evicted during this COMPOUND) */
static void node_touch(struct bf_ctx *b, struct bf_node *n, int mark)
{
	if (mark)
		n->epoch = b->epoch;
	if (n->inlru) {
		lru_unlink(b, n);
		lru_push_head(b, n);
	}
}

static void node_unhash(struct bf_ctx *b, struct bf_node *n)
{
	struct bf_node **pp = &b->ht[mix64(n->ino) & b->htmask];

	while (*pp && *pp != n)
		pp = &(*pp)->hnext;
	if (*pp)
		*pp = n->hnext;
}

static void node_drop(struct bf_ctx *b, struct bf_node *n)
{
	struct bf_node *p;

	lru_unlink(b, n);
	node_unhash(b, n);
	p = node_find(b, n->parent);
	if (p && p != n && p->nchild > 0) {
		p->nchild--;
		if (evictable(b, p) && !p->inlru)
			lru_push_head(b, p);
	}
	free(n->name);
	free(n->fchain);
	free(n);
	b->nnodes--;
	b->st.evictions++;
}

static void evict_if_needed(struct bf_ctx *b)
{
	int guard = 256, k;

	while (b->nnodes > b->max_nodes && guard-- > 0) {
		struct bf_node *v = NULL;

		/* Files first, then directories, taking the least recently used of each; nodes in use by this batch are moved to the head and skipped */
		for (k = 0; k < 2 && !v; k++) {
			struct bf_node *t = b->lru[k].tail;
			if (t && t->epoch != b->epoch)
				v = t;
			else if (t)
				node_touch(b, t, 0);
		}
		if (v)
			node_drop(b, v);
		else if (!b->lru[0].tail && !b->lru[1].tail)
			break;		/* everything is pinned: over the limit for now, retry on the next insert */
	}
}

/*
 * Insert (if already in the table, only the usage order is updated). The parent must already be in the table.
 * mark: in use by this COMPOUND; soft inserts (a by-product of READDIR) are not marked and may be evicted right away.
 */
static struct bf_node *node_insert(struct bf_ctx *b, uint64_t ino,
				   uint64_t parent, const char *name,
				   size_t namelen, int isdir, int mark)
{
	struct bf_node *n = node_find(b, ino), *p;
	size_t h;

	if (n) {
		node_touch(b, n, mark);
		return n;
	}
	p = node_find(b, parent);
	if (!p)
		return NULL;
	n = calloc(1, sizeof(*n));
	if (!n)
		return NULL;
	n->name = malloc(namelen + 1);
	if (!n->name) {
		free(n);
		return NULL;
	}
	memcpy(n->name, name, namelen);
	n->name[namelen] = 0;
	n->ino = ino;
	n->parent = parent;
	n->isdir = isdir ? 1 : 0;
	n->epoch = mark ? b->epoch : 0;
	h = mix64(ino) & b->htmask;
	n->hnext = b->ht[h];
	b->ht[h] = n;
	if (p->nchild++ == 0)
		lru_unlink(b, p);		/* it has a child now and can no longer be evicted */
	lru_push_head(b, n);
	b->nnodes++;
	if (b->nnodes > b->st.nodes_peak)
		b->st.nodes_peak = b->nnodes;
	/* The new node is at the head of the LRU, so this eviction pass will not pick it */
	evict_if_needed(b);
	return node_find(b, ino);
}

/* Build the path by walking the parent chain; returns its length, or a negative errno */
static int node_path(struct bf_ctx *b, const struct bf_node *n, char *buf,
		     size_t cap)
{
	const struct bf_node *chain[BF_DEPTH_MAX];
	int depth = 0, i;
	size_t len = 0;

	while (n && n->ino != b->root) {
		if (depth >= BF_DEPTH_MAX)
			return -ENAMETOOLONG;
		chain[depth++] = n;
		n = node_find(b, n->parent);
	}
	if (!n)
		return -ESTALE;		/* broken parent chain (should not happen: directories with children are not evicted) */
	if (depth == 0) {
		if (cap < 2)
			return -ENAMETOOLONG;
		buf[0] = '/';
		buf[1] = 0;
		return 1;
	}
	for (i = depth - 1; i >= 0; i--) {
		size_t l = strlen(chain[i]->name);
		if (len + 1 + l + 1 > cap)
			return -ENAMETOOLONG;
		buf[len++] = '/';
		memcpy(buf + len, chain[i]->name, l);
		len += l;
	}
	buf[len] = 0;
	return (int)len;
}

static int path_of(struct bf_ctx *b, uint64_t ino, char *buf)
{
	struct bf_node *n = node_find(b, ino);

	if (!n)
		return -ESTALE;
	return node_path(b, n, buf, BF_PATH_MAX);
}

/* Directory path + "/" + name */
static int join_path(char *out, const char *dir, int dirlen, const char *name,
		     size_t namelen)
{
	size_t off = (dirlen == 1 && dir[0] == '/') ? 0 : (size_t)dirlen;

	if (off + 1 + namelen + 1 > BF_PATH_MAX)
		return -ENAMETOOLONG;
	if (out != dir)
		memcpy(out, dir, off);
	out[off] = '/';
	memcpy(out + off + 1, name, namelen);
	out[off + 1 + namelen] = 0;
	return (int)(off + 1 + namelen);
}

/* ---------- Attributes ---------- */

static void stat_to_attr(const struct stat *sb, struct openmacfuse_attr *a)
{
	memset(a, 0, sizeof(*a));
	a->ino = sb->st_ino;
	a->mode = sb->st_mode;
	a->nlink = sb->st_nlink;
	a->uid = sb->st_uid;
	a->gid = sb->st_gid;
	a->size = (uint64_t)sb->st_size;
	a->used = (uint64_t)sb->st_blocks * 512;
	a->rdev = (uint64_t)sb->st_rdev;
	a->atime = sb->st_atimespec;
	a->mtime = sb->st_mtimespec;
	a->ctime = sb->st_ctimespec;
	a->crtime = sb->st_birthtimespec;
	a->flags = sb->st_flags;
	/* change: the metadata change time (nanoseconds); falls back to mtime if it is 0 */
	a->change = (uint64_t)sb->st_ctimespec.tv_sec * 1000000000ull +
		    (uint64_t)sb->st_ctimespec.tv_nsec;
	if (!a->change)
		a->change = (uint64_t)sb->st_mtimespec.tv_sec * 1000000000ull +
			    (uint64_t)sb->st_mtimespec.tv_nsec;
	if (!a->change)
		a->change = 1;
}

static int do_getattr(struct bf_ctx *b, const char *path, unsigned want,
		      struct openmacfuse_attr *a)
{
	struct stat sb;
	int r;

	memset(&sb, 0, sizeof(sb));
	b->st.getattr_calls++;
	r = b->op->getattr(path, &sb);
	if (r)
		return r;
	stat_to_attr(&sb, a);
	if (b->synth && a->ino == 0)
		a->ino = openmacfuse_synth_get(b->synth, path);
	if ((want & OPENMACFUSE_WANT_XTIMES) && b->op->getxtimes) {
		struct timespec bk = { 0, 0 }, cr = { 0, 0 };

		b->st.getxtimes_calls++;
		if (b->op->getxtimes(path, &bk, &cr) == 0) {
			a->crtime = cr;
			a->bkuptime = bk;
		}
	}
	return 0;
}

static void cache_attr(struct bf_ctx *b, struct bf_node *n,
		       const struct openmacfuse_attr *a, unsigned want)
{
	struct bf_aslot *sl = &b->acache[mix64(n->ino) & (BF_ACACHE_SLOTS - 1)];

	sl->a = *a;
	sl->a.ino = n->ino;
	sl->ino = n->ino;
	sl->x = (want & OPENMACFUSE_WANT_XTIMES) ? 1 : 0;
	sl->at = now_ns();
	n->isdir = S_ISDIR(a->mode) ? 1 : 0;	/* the caller must have taken the node off the LRU list first */
}

/* Attributes from the cache if still fresh; NULL if not */
static const struct openmacfuse_attr *attr_get(struct bf_ctx *b, uint64_t ino,
					unsigned want)
{
	struct bf_aslot *sl = &b->acache[mix64(ino) & (BF_ACACHE_SLOTS - 1)];

	if (sl->ino != ino)
		return NULL;
	if ((want & OPENMACFUSE_WANT_XTIMES) && !sl->x)
		return NULL;
	if (now_ns() - sl->at >= b->attr_ttl_ns)
		return NULL;
	return &sl->a;
}

/* A node's type may only become known when its attributes are first fetched: switch LRU lists */
static void node_set_attr(struct bf_ctx *b, struct bf_node *n,
			  const struct openmacfuse_attr *a, unsigned want)
{
	int was = n->isdir, inlru = n->inlru;

	(void)was;
	if (inlru)
		lru_unlink(b, n);	/* LRU lists are split by isdir: unlink before changing the type */
	cache_attr(b, n, a, want);
	if (inlru)
		lru_push_head(b, n);
}

/* ---------- Directory listing snapshots ---------- */

static size_t snap_size(const struct bf_snap *s)
{
	return sizeof(*s) + (size_t)s->cap * sizeof(struct bf_sent) + s->ncap +
	       (size_t)(s->idxcap + s->nidxcap) * sizeof(uint32_t);
}

static void snap_free(struct bf_snap *s)
{
	free(s->ents);
	free(s->names);
	free(s->idx);
	free(s->nidx);
	free(s);
}

static void snap_unlink(struct bf_ctx *b, struct bf_snap *s)
{
	struct bf_snap **pp = &b->snaps;

	while (*pp && *pp != s)
		pp = &(*pp)->next;
	if (*pp) {
		*pp = s->next;
		b->snap_bytes -= snap_size(s);
	}
}

static struct bf_snap *snap_find(struct bf_ctx *b, uint64_t dir)
{
	struct bf_snap *s;

	for (s = b->snaps; s; s = s->next)
		if (s->dir == dir) {
			s->last_use = ++b->snap_clock;
			return s;
		}
	return NULL;
}

/* Over the total limit: evict the least recently used (keep is left alone) */
static void snap_trim(struct bf_ctx *b, struct bf_snap *keep)
{
	while (b->snap_bytes > b->snap_max_bytes) {
		struct bf_snap *s, *old = NULL;

		for (s = b->snaps; s; s = s->next)
			if (s != keep && (!old || s->last_use < old->last_use))
				old = s;
		if (!old)
			break;
		snap_unlink(b, old);
		snap_free(old);
	}
}

struct snap_fill {
	struct bf_snap	*s;
};

static int snap_filler(void *buf, const char *name, const struct stat *st,
		       off_t off)
{
	struct snap_fill *sf = buf;
	struct bf_snap *s = sf->s;
	size_t l;
	struct bf_sent *e;

	(void)off;
	if (s->oom)
		return 1;
	if (!name || (name[0] == '.' && (name[1] == 0 ||
	    (name[1] == '.' && name[2] == 0))))
		return 0;
	l = strlen(name);
	if (l == 0 || l > 1023)
		return 0;
	if (s->n == s->cap) {
		uint32_t nc = s->cap ? s->cap * 2 : 256;
		struct bf_sent *ne = realloc(s->ents, (size_t)nc * sizeof(*ne));
		if (!ne) {
			s->oom = 1;
			return 1;
		}
		s->ents = ne;
		s->cap = nc;
	}
	if (s->nlen + l + 1 > s->ncap) {
		size_t nc = s->ncap ? s->ncap * 2 : 8192;
		char *nn;
		while (nc < s->nlen + l + 1)
			nc *= 2;
		nn = realloc(s->names, nc);
		if (!nn) {
			s->oom = 1;
			return 1;
		}
		s->names = nn;
		s->ncap = nc;
	}
	e = &s->ents[s->n++];
	e->ino = st ? (uint64_t)st->st_ino : 0;
	e->noff = (uint32_t)s->nlen;
	e->nlen = (uint16_t)l;
	e->type = st ? (uint16_t)(st->st_mode & S_IFMT) : 0;
	memcpy(s->names + s->nlen, name, l + 1);
	s->nlen += l + 1;
	if (l > 2 && name[0] == '.' && name[1] == '_')
		s->nad++;
	return 0;
}

/*
 * appledouble mode (the client writes real `._X` files itself): move every `._X` to just before X
 * (an orphan `._` whose X is absent stays where it is).
 * The system removes `._X` along with X; `rm -r` / fts remove entries one by one in listing order, so when
 * `._X` is listed after X the later attempt to remove
 * `._X` gets ENOENT and `rm -r` reports an error and exits 1 (although everything was in fact removed). Listed
 * first, `._X` is removed first, and when the system then tries to remove it along with X
 * it is simply gone, with no error. From macOS 26 on every new file carries com.apple.provenance and so has a
 * `._X`, which means `rm -r` on ext / exFAT
 * volumes is certain to hit this (observed on macOS 27: removing a non-empty directory failed on ext2, ext3 and ext4).
 */
static void snap_ad_first(struct bf_snap *s)
{
	uint32_t i, cap = 16, mask, h, k = 0;
	uint32_t *ht, *adof;
	uint8_t *moved;
	struct bf_sent *ne;

	if (!s->nad || s->n < 2)
		return;
	while (cap < s->n * 2)
		cap *= 2;
	mask = cap - 1;
	ht = calloc(cap, sizeof(*ht));
	adof = calloc(s->n, sizeof(*adof));	/* index of X -> index+1 of its `._X` */
	moved = calloc(s->n, 1);
	ne = malloc((size_t)s->n * sizeof(*ne));
	if (!ht || !adof || !moved || !ne)
		goto out;
	/* Hash the names (entries that are not `._`) */
	for (i = 0; i < s->n; i++) {
		const char *nm = s->names + s->ents[i].noff;

		if (s->ents[i].nlen > 2 && nm[0] == '.' && nm[1] == '_')
			continue;
		h = (uint32_t)openmacfuse_hash_bytes(nm, s->ents[i].nlen) & mask;
		while (ht[h])
			h = (h + 1) & mask;
		ht[h] = i + 1;
	}
	for (i = 0; i < s->n; i++) {
		const char *nm = s->names + s->ents[i].noff;
		uint32_t xl = s->ents[i].nlen - 2;

		if (!(s->ents[i].nlen > 2 && nm[0] == '.' && nm[1] == '_'))
			continue;
		h = (uint32_t)openmacfuse_hash_bytes(nm + 2, xl) & mask;
		while (ht[h]) {
			uint32_t x = ht[h] - 1;

			if (s->ents[x].nlen == xl && memcmp(s->names + s->ents[x].noff, nm + 2, xl) == 0) {
				if (!adof[x]) {
					adof[x] = i + 1;
					moved[i] = 1;
				}
				break;
			}
			h = (h + 1) & mask;
		}
	}
	for (i = 0; i < s->n; i++) {
		if (moved[i])
			continue;
		if (adof[i])
			ne[k++] = s->ents[adof[i] - 1];
		ne[k++] = s->ents[i];
	}
	if (k == s->n) {
		memcpy(s->ents, ne, (size_t)s->n * sizeof(*ne));
	}
out:
	free(ht);
	free(adof);
	free(moved);
	free(ne);
}

static struct bf_snap *snap_build(struct bf_ctx *b, uint64_t dir,
				  const char *dirpath, int dirlen,
				  uint64_t verf, int *err)
{
	struct fuse_file_info fi;
	struct snap_fill sf;
	struct bf_snap *s, *old;
	uint32_t i;
	int r = 0;

	*err = 0;
	s = calloc(1, sizeof(*s));
	if (!s) {
		*err = -ENOMEM;
		return NULL;
	}
	s->dir = dir;
	s->verf = verf;
	sf.s = s;
	memset(&fi, 0, sizeof(fi));
	fi.flags = O_RDONLY;
	if (b->op->opendir)
		r = b->op->opendir(dirpath, &fi);
	if (r == 0) {
		r = b->op->readdir(dirpath, &sf, snap_filler, 0, &fi);
		if (b->op->releasedir)
			b->op->releasedir(dirpath, &fi);
	}
	if (r == 0 && s->oom)
		r = -ENOMEM;
	if (r) {
		snap_free(s);
		*err = r;
		return NULL;
	}
	/* Entries for which filler gave no inode number: fill each in with getattr (our own driver always gives one) */
	for (i = 0; i < s->n; i++) {
		struct bf_sent *e = &s->ents[i];
		struct openmacfuse_attr a;

		if (e->ino && e->type)
			continue;
		if (join_path(b->path2, dirpath, dirlen, s->names + e->noff,
			      e->nlen) < 0 ||
		    do_getattr(b, b->path2, OPENMACFUSE_WANT_BASIC, &a))
			continue;
		e->ino = a.ino;
		e->type = (uint16_t)(a.mode & S_IFMT);
	}
	if (!b->adtrans)
		snap_ad_first(s);
	b->st.snap_builds++;
	b->st.snap_entries += s->n;
	/* Replace the old snapshot of the same directory */
	old = snap_find(b, dir);
	if (old) {
		snap_unlink(b, old);
		snap_free(old);
	}
	s->last_use = ++b->snap_clock;
	s->next = b->snaps;
	b->snaps = s;
	b->snap_bytes += snap_size(s);
	snap_trim(b, s);
	if (b->log && s->n >= 10000)
		b->log("directory snapshot: inode %llu, %u entries, %zu bytes",
		       (unsigned long long)dir, s->n, snap_size(s));
	return s;
}

/* Find the index of an entry in a snapshot by inode number; -1 if absent */
static long snap_find_ino(struct bf_ctx *b, struct bf_snap *s, uint64_t ino)
{
	uint32_t i, mask, h;

	if (!s->idx) {
		uint32_t cap = 16;
		size_t before = snap_size(s);

		while (cap < s->n * 2)
			cap *= 2;
		s->idx = calloc(cap, sizeof(uint32_t));
		if (!s->idx) {
			for (i = 0; i < s->n; i++)	/* out of memory: fall back to a linear search */
				if (s->ents[i].ino == ino)
					return i;
			return -1;
		}
		s->idxcap = cap;
		mask = cap - 1;
		for (i = 0; i < s->n; i++) {
			h = (uint32_t)mix64(s->ents[i].ino) & mask;
			while (s->idx[h])
				h = (h + 1) & mask;
			s->idx[h] = i + 1;
		}
		b->snap_bytes += snap_size(s) - before;
	}
	mask = s->idxcap - 1;
	h = (uint32_t)mix64(ino) & mask;
	while (s->idx[h]) {
		if (s->ents[s->idx[h] - 1].ino == ino)
			return s->idx[h] - 1;
		h = (h + 1) & mask;
	}
	return -1;
}

/* The directory's current verifier (cookieverf): it changes whenever the directory's change / mtime / size changes */
static uint64_t dir_verf(struct bf_ctx *b, uint64_t dir, const struct openmacfuse_attr *a)
{
	uint64_t v = mix64(b->salt ^ dir);

	v = mix64(v ^ a->change);
	v = mix64(v ^ ((uint64_t)a->mtime.tv_sec * 1000000000ull + (uint64_t)a->mtime.tv_nsec));
	v = mix64(v ^ a->size);
	return v ? v : 1;
}

/* Is this name in the snapshot (decides whether the X of a `._X` is there; the name hash is built on first use) */
static int snap_has_name(struct bf_ctx *b, struct bf_snap *s, const char *name,
			 size_t nl)
{
	uint32_t i, mask, h;

	if (!s->nidx) {
		uint32_t cap = 16;
		size_t before = snap_size(s);

		while (cap < s->n * 2)
			cap *= 2;
		s->nidx = calloc(cap, sizeof(uint32_t));
		if (!s->nidx) {
			for (i = 0; i < s->n; i++)
				if (s->ents[i].nlen == nl &&
				    memcmp(s->names + s->ents[i].noff, name, nl) == 0)
					return 1;
			return 0;
		}
		s->nidxcap = cap;
		mask = cap - 1;
		for (i = 0; i < s->n; i++) {
			h = (uint32_t)openmacfuse_hash_bytes(s->names + s->ents[i].noff,
						       s->ents[i].nlen) & mask;
			while (s->nidx[h])
				h = (h + 1) & mask;
			s->nidx[h] = i + 1;
		}
		b->snap_bytes += snap_size(s) - before;
	}
	mask = s->nidxcap - 1;
	h = (uint32_t)openmacfuse_hash_bytes(name, nl) & mask;
	while (s->nidx[h]) {
		const struct bf_sent *e = &s->ents[s->nidx[h] - 1];
		if (e->nlen == nl && memcmp(s->names + e->noff, name, nl) == 0)
			return 1;
		h = (h + 1) & mask;
	}
	return 0;
}

/* ---------- Directory name index (equivalent-name lookup) ---------- */

static int name_key(struct bf_ctx *b, const char *name, size_t nl, char *out)
{
	int n = openmacfuse_name_key(name, nl, b->ci, b->ext.casefold, out, OPENMACFUSE_KEY_MAX);

	if (n < 0) {
		if (nl >= OPENMACFUSE_KEY_MAX)
			nl = OPENMACFUSE_KEY_MAX - 1;
		memcpy(out, name, nl);
		out[nl] = 0;
		n = (int)nl;
	}
	return n;
}

static int nfc_equal(const char *a, size_t al, const char *b2, size_t bl)
{
	char x[OPENMACFUSE_KEY_MAX], y[OPENMACFUSE_KEY_MAX];
	int xl = openmacfuse_name_nfc(a, al, x, sizeof(x));
	int yl = openmacfuse_name_nfc(b2, bl, y, sizeof(y));

	if (xl < 0 || yl < 0)
		return al == bl && memcmp(a, b2, al) == 0;
	return xl == yl && memcmp(x, y, (size_t)xl) == 0;
}

static size_t didx_size(const struct bf_didx *x)
{
	return sizeof(*x) + (size_t)x->cap * sizeof(struct bf_dent) + x->ncap +
	       (size_t)x->nbk * sizeof(uint32_t);
}

static void didx_free(struct bf_didx *x)
{
	free(x->e);
	free(x->names);
	free(x->bk);
	free(x);
}

static void didx_account(struct bf_ctx *b, struct bf_didx *x)
{
	size_t now = didx_size(x);

	b->didx_bytes = b->didx_bytes - x->bytes + now;
	x->bytes = now;
}

static struct bf_didx *didx_find(struct bf_ctx *b, uint64_t dir)
{
	struct bf_didx *x;

	for (x = b->didx; x; x = x->next)
		if (x->dir == dir) {
			x->last_use = ++b->didx_clock;
			return x;
		}
	return NULL;
}

static void didx_unlink(struct bf_ctx *b, struct bf_didx *x)
{
	struct bf_didx **pp = &b->didx;

	while (*pp && *pp != x)
		pp = &(*pp)->next;
	if (*pp) {
		*pp = x->next;
		b->didx_bytes -= x->bytes;
	}
}

static void didx_drop(struct bf_ctx *b, uint64_t dir)
{
	struct bf_didx *x = didx_find(b, dir);

	if (x) {
		didx_unlink(b, x);
		didx_free(x);
	}
}

static void didx_drop_all(struct bf_ctx *b)
{
	while (b->didx) {
		struct bf_didx *x = b->didx;
		b->didx = x->next;
		didx_free(x);
	}
	b->didx_bytes = 0;
}

static void didx_trim(struct bf_ctx *b, struct bf_didx *keep)
{
	while (b->didx_bytes > b->didx_max) {
		struct bf_didx *x, *old = NULL;

		for (x = b->didx; x; x = x->next)
			if (x != keep && (!old || x->last_use < old->last_use))
				old = x;
		if (!old)
			break;
		didx_unlink(b, old);
		didx_free(old);
	}
}

static void didx_rehash(struct bf_didx *x, uint32_t nbk)
{
	uint32_t *nb = calloc(nbk, sizeof(uint32_t)), i;

	if (!nb)
		return;		/* not enough buckets: leave as is (the chains just get a little longer) */
	free(x->bk);
	x->bk = nb;
	x->nbk = nbk;
	for (i = 0; i < x->n; i++) {
		struct bf_dent *e = &x->e[i];
		uint32_t k;

		if (e->dead)
			continue;
		k = (uint32_t)(e->h % nbk);
		e->next = x->bk[k];
		x->bk[k] = i + 1;
	}
}

/* Add an entry; while building the index (counting) also count conflicts that differ only in case */
static int didx_add(struct bf_ctx *b, struct bf_didx *x, const char *name, size_t nl,
		    uint64_t ino, int counting)
{
	char key[OPENMACFUSE_KEY_MAX];
	int kl = name_key(b, name, nl, key);
	uint64_t h = openmacfuse_hash_bytes(key, (size_t)kl);
	struct bf_dent *e;
	uint32_t k;

	if (x->n == x->cap) {
		uint32_t nc = x->cap ? x->cap * 2 : 64;
		struct bf_dent *ne = realloc(x->e, (size_t)nc * sizeof(*ne));
		if (!ne)
			return -ENOMEM;
		x->e = ne;
		x->cap = nc;
	}
	if (x->nlen + nl + 1 > x->ncap) {
		size_t nc = x->ncap ? x->ncap * 2 : 4096;
		char *nn;

		while (nc < x->nlen + nl + 1)
			nc *= 2;
		nn = realloc(x->names, nc);
		if (!nn)
			return -ENOMEM;
		x->names = nn;
		x->ncap = nc;
	}
	if (counting && b->ci) {
		uint32_t i = x->bk[h % x->nbk];

		while (i) {
			const struct bf_dent *o = &x->e[i - 1];
			if (!o->dead && o->h == h &&
			    !nfc_equal(x->names + o->noff, o->nlen, name, nl)) {
				char k2[OPENMACFUSE_KEY_MAX];
				int k2l = name_key(b, x->names + o->noff, o->nlen, k2);
				if (k2l == kl && memcmp(k2, key, (size_t)kl) == 0) {
					x->conflicts++;
					break;
				}
			}
			i = o->next;
		}
	}
	e = &x->e[x->n];
	memset(e, 0, sizeof(*e));
	e->h = h;
	e->ino = ino;
	e->noff = (uint32_t)x->nlen;
	e->nlen = (uint16_t)nl;
	memcpy(x->names + x->nlen, name, nl);
	x->names[x->nlen + nl] = 0;
	x->nlen += nl + 1;
	x->n++;
	x->nlive++;
	if (nl > 2 && name[0] == '.' && name[1] == '_')
		x->nad++;
	if (x->nlive > x->nbk * 2) {
		didx_rehash(x, x->nbk * 4);
	} else {
		k = (uint32_t)(h % x->nbk);
		e->next = x->bk[k];
		x->bk[k] = x->n;
	}
	return 0;
}

/*
 * Find entries equivalent to name (same comparison key): returns the number of inexact matches,
 * storing their indexes in out (at most max);
 * *exact is the index of the exact match, or -1 if there is none.
 */
static int didx_match(struct bf_ctx *b, struct bf_didx *x, const char *name, size_t nl,
		      long *exact, uint32_t *out, int max)
{
	char key[OPENMACFUSE_KEY_MAX], k2[OPENMACFUSE_KEY_MAX];
	int kl = name_key(b, name, nl, key), cnt = 0;
	uint64_t h = openmacfuse_hash_bytes(key, (size_t)kl);
	uint32_t i = x->bk[h % x->nbk];

	*exact = -1;
	while (i) {
		const struct bf_dent *e = &x->e[i - 1];

		if (!e->dead && e->h == h) {
			const char *en = x->names + e->noff;

			if (e->nlen == nl && memcmp(en, name, nl) == 0) {
				*exact = (long)(i - 1);
			} else {
				int k2l = name_key(b, en, e->nlen, k2);
				if (k2l == kl && memcmp(k2, key, (size_t)kl) == 0) {
					if (cnt < max)
						out[cnt] = i - 1;
					cnt++;
				}
			}
		}
		i = e->next;
	}
	return cnt;
}

/*
 * Removed entries are only marked: compact once dead entries outnumber live ones (and exceed
 * 1024), otherwise heavy create / remove churn
 * (200,000 small-file creates and removes) makes the index only ever grow (measured: memory 9MB -> 19MB).
 */
static void didx_compact(struct bf_ctx *b, struct bf_didx *x)
{
	struct bf_dent *ne;
	char *nn;
	uint32_t i, k = 0;
	size_t nlen = 0;

	ne = malloc(((size_t)x->nlive + 1) * sizeof(*ne));
	nn = malloc(x->nlen ? x->nlen : 1);
	if (!ne || !nn) {
		free(ne);
		free(nn);
		return;
	}
	for (i = 0; i < x->n; i++) {
		struct bf_dent *e = &x->e[i];

		if (e->dead)
			continue;
		ne[k] = *e;
		ne[k].noff = (uint32_t)nlen;
		memcpy(nn + nlen, x->names + e->noff, e->nlen + 1u);
		nlen += e->nlen + 1u;
		k++;
	}
	free(x->e);
	free(x->names);
	x->e = ne;
	x->n = x->cap = k;
	x->cap = x->nlive + 1;
	x->names = nn;
	x->nlen = nlen;
	x->ncap = nlen ? nlen : 1;
	didx_rehash(x, x->nbk);
	didx_account(b, x);
}

static void didx_del(struct bf_ctx *b, struct bf_didx *x, const char *name, size_t nl)
{
	char key[OPENMACFUSE_KEY_MAX];
	int kl = name_key(b, name, nl, key);
	uint64_t h = openmacfuse_hash_bytes(key, (size_t)kl);
	uint32_t *pp = &x->bk[h % x->nbk];

	while (*pp) {
		struct bf_dent *e = &x->e[*pp - 1];

		if (!e->dead && e->nlen == nl && memcmp(x->names + e->noff, name, nl) == 0) {
			*pp = e->next;
			e->dead = 1;
			x->nlive--;
			if (nl > 2 && name[0] == '.' && name[1] == '_' && x->nad)
				x->nad--;
			if (x->n - x->nlive > x->nlive + 1024)
				didx_compact(b, x);
			return;
		}
		pp = &e->next;
	}
}

struct didx_fill {
	struct bf_ctx	*b;
	struct bf_didx	*x;
};

static int didx_filler(void *buf, const char *name, const struct stat *st, off_t off)
{
	struct didx_fill *f = buf;
	size_t l;

	(void)off;
	if (f->x->oom)
		return 1;
	if (!name || (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))))
		return 0;
	l = strlen(name);
	if (l == 0 || l > 1023)
		return 0;
	if (didx_add(f->b, f->x, name, l, st ? (uint64_t)st->st_ino : 0, 1)) {
		f->x->oom = 1;
		return 1;
	}
	return 0;
}

/* Name index of dir (built from one directory listing if absent; dirpath is dir's path) */
static struct bf_didx *didx_get(struct bf_ctx *b, uint64_t dir, const char *dirpath)
{
	struct bf_didx *x = didx_find(b, dir);
	struct fuse_file_info fi;
	struct didx_fill f;
	int r = 0;

	if (x)
		return x;
	x = calloc(1, sizeof(*x));
	if (!x)
		return NULL;
	x->dir = dir;
	x->nbk = 64;
	x->bk = calloc(x->nbk, sizeof(uint32_t));
	if (!x->bk) {
		free(x);
		return NULL;
	}
	f.b = b;
	f.x = x;
	memset(&fi, 0, sizeof(fi));
	fi.flags = O_RDONLY;
	if (b->op->opendir)
		r = b->op->opendir(dirpath, &fi);
	if (r == 0) {
		r = b->op->readdir(dirpath, &f, didx_filler, 0, &fi);
		if (b->op->releasedir)
			b->op->releasedir(dirpath, &fi);
	}
	if (r || x->oom) {
		didx_free(x);
		return NULL;
	}
	b->st.didx_builds++;
	b->st.didx_ents += x->n;
	if (x->conflicts) {
		b->st.conflict_dirs++;
		b->st.conflict_pairs += x->conflicts;
		if (b->log && b->st.conflict_dirs <= 20)
			b->log("case conflict: directory %s has %u pair(s) of names differing only in case (the exact name wins; each can still be opened)",
			       dirpath, x->conflicts);
	}
	x->last_use = ++b->didx_clock;
	x->next = b->didx;
	b->didx = x;
	x->bytes = 0;
	didx_account(b, x);
	didx_trim(b, x);
	return x;
}

/* A modification through this layer: update the index incrementally if there is one (do not build one otherwise) */
static void didx_note_add(struct bf_ctx *b, uint64_t dir, const char *name, uint64_t ino)
{
	struct bf_didx *x = didx_find(b, dir);

	if (x) {
		if (didx_add(b, x, name, strlen(name), ino, 0))
			didx_drop(b, dir);
		else
			didx_account(b, x);
	}
}

static void didx_note_del(struct bf_ctx *b, uint64_t dir, const char *name)
{
	struct bf_didx *x = didx_find(b, dir);

	if (x)
		didx_del(b, x, name, strlen(name));
}


/* ---------- Handle ancestor chains: canonical chain, freezing ---------- */

/* The ancestor chain computed from the node table's current parent chain (ignoring any frozen one), nearest first */
static int canon_chain(struct bf_ctx *b, const struct bf_node *n, uint64_t *anc,
		       int max)
{
	uint64_t p = n->parent;
	int k = 0;

	if (n->ino == b->root)
		return 0;
	while (p && k < max) {
		const struct bf_node *m;

		anc[k++] = p;
		if (p == b->root)
			break;
		m = node_find(b, p);
		if (!m)
			break;
		p = m->parent;
	}
	return k;
}

static void node_freeze(struct bf_ctx *b, struct bf_node *n, const uint64_t *anc,
			int nanc)
{
	if (n->fchain || n->ino == b->root)
		return;
	n->fchain = malloc((size_t)(nanc > 0 ? nanc : 1) * sizeof(uint64_t));
	if (!n->fchain)
		return;
	if (nanc > 0)
		memcpy(n->fchain, anc, (size_t)nanc * sizeof(uint64_t));
	n->nfchain = (uint8_t)nanc;
	b->st.frozen++;
}

/*
 * Called before a rename: freeze the ancestor chain of issued handles, as it is now, for the node
 * being moved (and, for a directory, every node below it in the table).
 * NFS requires a handle to stay the same for the object's lifetime (FH4_PERSISTENT), and
 * the client identifies vnodes by handle bytes: if the same object got different handle bytes after a rename, the client would take it for another file.
 */
static void freeze_subtree(struct bf_ctx *b, struct bf_node *x)
{
	uint64_t anc[OPENMACFUSE_FH_MAXANC];
	size_t i;
	int k;

	if (!x->fchain) {
		k = canon_chain(b, x, anc, OPENMACFUSE_FH_MAXANC);
		node_freeze(b, x, anc, k);
	}
	if (!x->isdir)
		return;
	for (i = 0; i <= b->htmask; i++) {
		struct bf_node *m;

		for (m = b->ht[i]; m; m = m->hnext) {
			const struct bf_node *p = m;
			int depth;

			if (m == x || m->fchain || m->ino == b->root)
				continue;
			for (depth = 0; depth < OPENMACFUSE_FH_MAXANC && p && p->ino != b->root;
			     depth++) {
				if (p->parent == x->ino)
					break;
				p = node_find(b, p->parent);
			}
			if (p && p->ino != b->root && depth < OPENMACFUSE_FH_MAXANC &&
			    p->parent == x->ino) {
				k = canon_chain(b, m, anc, OPENMACFUSE_FH_MAXANC);
				node_freeze(b, m, anc, k);
			}
		}
	}
}

/* ---------- "Moved" table (relocation hints) ---------- */

static struct bf_hint *hint_set0(struct bf_ctx *b, uint64_t ino)
{
	return &b->hints[(mix64(ino ^ 0x9e3779b97f4a7c15ull) &
			  (BF_HINT_SLOTS / BF_HINT_WAYS - 1)) * BF_HINT_WAYS];
}

static struct bf_hint *hint_find(struct bf_ctx *b, uint64_t ino)
{
	struct bf_hint *h = hint_set0(b, ino);
	int i;

	for (i = 0; i < BF_HINT_WAYS; i++)
		if (h[i].ino == ino)
			return &h[i];
	return NULL;
}

/* Find a slot for ino: its existing one, an empty one, or the oldest in the set */
static struct bf_hint *hint_slot(struct bf_ctx *b, uint64_t ino)
{
	struct bf_hint *h = hint_set0(b, ino), *old = h;
	int i;

	for (i = 0; i < BF_HINT_WAYS; i++)
		if (h[i].ino == ino)
			return &h[i];
	for (i = 0; i < BF_HINT_WAYS; i++) {
		if (!h[i].ino)
			return &h[i];
		if (h[i].stamp < old->stamp)
			old = &h[i];
	}
	return old;
}

static void hint_clear(struct bf_hint *h)
{
	free(h->name);
	memset(h, 0, sizeof(*h));
}

/* Record a hint: ino is now (also) called name under parent; parent must be in the node table */
static void hint_set(struct bf_ctx *b, uint64_t ino, uint64_t parent,
		     const char *name)
{
	struct bf_hint *h = hint_slot(b, ino);
	struct bf_node *p = node_find(b, parent);
	char *nm;

	if (!p)
		return;
	nm = strdup(name);
	if (!nm)
		return;
	hint_clear(h);
	h->ino = ino;
	h->stamp = ++b->hint_clock;
	h->parent = parent;
	h->name = nm;
	if (p->fchain) {
		h->nchain = p->nfchain;
		memcpy(h->chain, p->fchain, (size_t)p->nfchain * sizeof(uint64_t));
	} else {
		h->nchain = (uint8_t)canon_chain(b, p, h->chain, OPENMACFUSE_FH_MAXANC);
	}
}

/* ---------- Backend interface implementation ---------- */

static openmacfuse_ino_t bf_root(void *ctx)
{
	return ((struct bf_ctx *)ctx)->root;
}

static void bf_begin(void *ctx, const struct openmacfuse_cred *cred)
{
	struct bf_ctx *b = ctx;

	if (++b->epoch == 0)
		b->epoch = 1;
	b->fctx->uid = cred->uid;
	b->fctx->gid = cred->gid;
	b->fctx->pid = 0;
	b->fctx->umask = 0;	/* the mode sent by the client already has the process umask applied */
}

static int bf_getattr(void *ctx, openmacfuse_ino_t ino, struct openmacfuse_attr *st,
		      unsigned want)
{
	struct bf_ctx *b = ctx;
	struct bf_node *n = node_find(b, ino);
	int r;

	if (!n)
		return -ENOENT;
	{
		const struct openmacfuse_attr *ca = attr_get(b, ino, want);
		if (ca) {
			b->st.attr_hits++;
			*st = *ca;
			st->change += n->gen;
			return 0;
		}
	}
	r = node_path(b, n, b->path1, BF_PATH_MAX);
	if (r < 0)
		return r;
	r = do_getattr(b, b->path1, want, st);
	if (r)
		return r;
	if (st->ino && st->ino != ino && ino != b->root) {
		/* The path now holds a different object (the original was removed and a new one created under the same name): the handle is no longer valid */
		b->st.stale_ino++;
		return -ENOENT;
	}
	node_set_attr(b, n, st, want);
	st->ino = ino;
	st->change += n->gen;
	return 0;
}

static int real_name(struct bf_ctx *b, uint64_t dir, const char *name, char *real,
		     struct openmacfuse_attr *a);

static int bf_lookup(void *ctx, openmacfuse_ino_t dir, const char *oname,
		     openmacfuse_ino_t *out)
{
	struct bf_ctx *b = ctx;
	struct bf_node *d = node_find(b, dir), *n;
	struct openmacfuse_attr a;
	char name[OPENMACFUSE_KEY_MAX];
	size_t nl;
	int r;

	if (!d)
		return -ESTALE;
	/* If the name as given is not found, look for an equivalent name (NFC / NFD, and case when case-insensitive) and continue with the real on-disk name */
	r = real_name(b, dir, oname, name, &a);
	if (r)
		return r;
	nl = strlen(name);
	if (a.ino == 0 || a.ino == dir)
		return -EIO;	/* the driver gave no inode number as use_ino requires: no handle can be built */
	n = node_insert(b, a.ino, dir, name, nl, S_ISDIR(a.mode), 1);
	if (!n)
		return -ENOMEM;
	if (!S_ISDIR(a.mode) && a.nlink > 1 &&
	    (n->parent != dir || !n->name || strcmp(n->name, name) != 0))
		hint_set(b, a.ino, dir, name);	/* another name of a hard link */
	node_set_attr(b, n, &a, OPENMACFUSE_WANT_BASIC);
	*out = a.ino;
	return 0;
}

static int bf_lookupp(void *ctx, openmacfuse_ino_t dir, openmacfuse_ino_t *out)
{
	struct bf_ctx *b = ctx;
	struct bf_node *n = node_find(b, dir);

	if (!n)
		return -ESTALE;
	if (dir == b->root)
		return -ENOENT;
	*out = n->parent;
	if (node_find(b, n->parent))
		node_touch(b, node_find(b, n->parent), 1);
	return 0;
}

/*
 * Recover a node: find its name by inode number in the listing snapshot of the parent directory
 * (which must already be in the table);
 * if there is no snapshot (or it is not in it), list the parent again, then confirm the inode number with getattr.
 */
static int recover_one(struct bf_ctx *b, uint64_t ino, uint64_t parent)
{
	struct bf_node *p = node_find(b, parent), *n;
	struct bf_snap *s;
	struct openmacfuse_attr a, da;
	long i;
	int dlen, len, err, tries;

	if (!p)
		return -ESTALE;
	node_touch(b, p, 1);
	dlen = node_path(b, p, b->path1, BF_PATH_MAX);
	if (dlen < 0)
		return -ESTALE;
	for (tries = 0; tries < 2; tries++) {
		s = tries == 0 ? snap_find(b, parent) : NULL;
		if (!s) {
			if (do_getattr(b, b->path1, OPENMACFUSE_WANT_BASIC, &da))
				return -ESTALE;
			s = snap_build(b, parent, b->path1, dlen,
				       dir_verf(b, parent, &da), &err);
			if (!s)
				return -ESTALE;
		}
		i = snap_find_ino(b, s, ino);
		if (i < 0)
			continue;
		len = join_path(b->path2, b->path1, dlen,
				s->names + s->ents[i].noff, s->ents[i].nlen);
		if (len < 0 || do_getattr(b, b->path2, OPENMACFUSE_WANT_BASIC, &a) ||
		    a.ino != ino)
			continue;
		n = node_insert(b, ino, parent, s->names + s->ents[i].noff,
				s->ents[i].nlen, S_ISDIR(a.mode), 1);
		if (!n)
			return -ENOMEM;
		node_set_attr(b, n, &a, OPENMACFUSE_WANT_BASIC);
		return 0;
	}
	return -ESTALE;
}

/*
 * Recover an inode that is not in the table:
 *   1. the "moved" table has it -> recover that parent directory first (using the parent's ancestor chain recorded there), then look it up by inode number in that parent;
 *   2. otherwise follow the ancestor chain in the handle: recover anc[0] first (recursively), then look it up by inode number in anc[0].
 * The root is always in the table, so recovery always succeeds when the ancestor chain is complete
 * (<= 14 levels); the "moved" table is consulted first at every level,
 * so an ancestor directory that was renamed and then evicted can still be recovered.
 */
static int resolve_ino(struct bf_ctx *b, uint64_t ino, const uint64_t *anc,
		       int nanc, int depth)
{
	struct bf_node *n = node_find(b, ino);
	struct bf_hint *h;

	if (n) {
		node_touch(b, n, 1);
		return 0;
	}
	if (depth > 48)
		return -ESTALE;
	h = hint_find(b, ino);
	if (h) {
		uint64_t hp = h->parent, hc[OPENMACFUSE_FH_MAXANC];
		int hn = h->nchain;

		memcpy(hc, h->chain, sizeof(hc));
		if (resolve_ino(b, hp, hc, hn, depth + 1) == 0 &&
		    recover_one(b, ino, hp) == 0) {
			b->st.hint_hits++;
			return 0;
		}
	}
	if (nanc <= 0)
		return -ESTALE;
	if (resolve_ino(b, anc[0], anc + 1, nanc - 1, depth + 1))
		return -ESTALE;
	return recover_one(b, ino, anc[0]);
}

static int bf_resolve(void *ctx, openmacfuse_ino_t ino, const openmacfuse_ino_t *anc, int nanc)
{
	struct bf_ctx *b = ctx;
	struct bf_node *n = node_find(b, ino);
	uint64_t cur[OPENMACFUSE_FH_MAXANC];
	int k;

	if (n) {
		node_touch(b, n, 1);
		return 0;
	}
	if (resolve_ino(b, ino, anc, nanc, 0)) {
		b->st.recover_fail++;
		if (b->log)
			b->log("handle cannot be recovered: inode %llu (%d ancestor level(s), parent %llu)",
			       (unsigned long long)ino, nanc,
			       (unsigned long long)(nanc ? anc[0] : 0));
		return -ESTALE;
	}
	b->st.recover_ok++;
	/* After recovery the canonical chain differs from the one the client holds (a rename happened in between): freeze it as the client's, so the handle bytes do not change */
	n = node_find(b, ino);
	if (n && nanc > 0) {
		k = canon_chain(b, n, cur, OPENMACFUSE_FH_MAXANC);
		if (k != nanc || memcmp(cur, anc, (size_t)k * sizeof(uint64_t)) != 0)
			node_freeze(b, n, anc, nanc);
	}
	return 0;
}

/* Ancestor chain for a handle: the frozen one if there is one, otherwise the canonical parent chain from the node table (at most max levels) */
static int bf_ancestry(void *ctx, openmacfuse_ino_t ino, openmacfuse_ino_t parent_hint,
		       openmacfuse_ino_t *anc, int max)
{
	struct bf_ctx *b = ctx;
	struct bf_node *n = node_find(b, ino);
	uint64_t p;
	int k = 0;

	if (ino == b->root)
		return 0;
	if (n && n->fchain) {
		k = n->nfchain < max ? n->nfchain : max;
		memcpy(anc, n->fchain, (size_t)k * sizeof(uint64_t));
		return k;
	}
	if (n)
		return canon_chain(b, n, anc, max);
	p = parent_hint;
	while (p && k < max) {
		anc[k++] = p;
		if (p == b->root)
			break;
		n = node_find(b, p);
		if (!n)
			break;
		p = n->parent;
	}
	return k;
}

static int bf_readdir(void *ctx, openmacfuse_ino_t dir, uint64_t pos, uint64_t *verf,
		      unsigned want, openmacfuse_fill_fn fill, void *arg, int *eof)
{
	struct bf_ctx *b = ctx;
	struct bf_node *d = node_find(b, dir);
	struct bf_snap *s;
	struct openmacfuse_attr da;
	uint64_t cur;
	uint64_t i;
	int dlen, r, err;

	*eof = 0;
	if (!d)
		return -ESTALE;
	dlen = node_path(b, d, b->path1, BF_PATH_MAX);
	if (dlen < 0)
		return dlen;
	/* Directory attributes are fetched fresh every time (no cache): the verifier must reflect the directory's state right now */
	r = do_getattr(b, b->path1, OPENMACFUSE_WANT_BASIC, &da);
	if (r)
		return r;
	if (!S_ISDIR(da.mode))
		return -ENOTDIR;
	node_set_attr(b, d, &da, OPENMACFUSE_WANT_BASIC);
	da.change += d->gen;		/* a directory modified through this layer: the verifier must change */
	cur = dir_verf(b, dir, &da);
	/*
	 * The client resumes (non-zero cookie) with a zero verifier: macOS 12 was observed to send this
	 * when two processes list the same directory at once
	 * (it reuses a cookie from its own directory cache without the verifier). Our verifier is never 0,
	 * so 0 only means "the client has none" and is not taken as the directory having changed - otherwise we would return NOT_SAME and the client would report EIO to ls.
	 * If the directory really did change, the cookie (an index into the snapshot) may be off, at the cost of this listing possibly repeating / missing a few entries,
	 * which matches the behaviour the RFC allows of a server that does not check cookieverf.
	 */
	if (pos && *verf != 0 && *verf != cur) {
		if (b->log)
			b->log("READDIR verifier mismatch: directory %llu pos=%llu client %016llx current %016llx (change=%llu gen=%u mtime=%ld.%09ld size=%llu)",
			       (unsigned long long)dir, (unsigned long long)pos,
			       (unsigned long long)*verf, (unsigned long long)cur,
			       (unsigned long long)da.change, d->gen,
			       (long)da.mtime.tv_sec, da.mtime.tv_nsec,
			       (unsigned long long)da.size);
		return -OPENMACFUSE_E_NOT_SAME;
	}
	s = snap_find(b, dir);
	if (s && s->verf == cur) {
		if (pos == 0)
			b->st.snap_reuse++;
	} else {
		s = snap_build(b, dir, b->path1, dlen, cur, &err);
		if (!s)
			return err;
	}
	*verf = cur;
	if (pos > s->n)
		return -OPENMACFUSE_E_BAD_COOKIE;
	/* Inserting nodes may evict other snapshots: this snapshot is the keep one (just used) and will not be freed */
	for (i = pos; i < s->n; i++) {
		struct bf_sent *e = &s->ents[i];
		const char *nm = s->names + e->noff;
		struct openmacfuse_attr a;
		const struct openmacfuse_attr *pa = NULL;
		struct openmacfuse_dirent de;
		struct bf_node *n;

		/* Named-attribute channel: a `._X` whose X exists is not listed (its contents are presented as X's named attributes) */
		if (b->adtrans && s->nad && e->nlen > 2 && nm[0] == '.' && nm[1] == '_' &&
		    (e->type == S_IFREG || e->type == 0) &&
		    snap_has_name(b, s, nm + 2, e->nlen - 2u)) {
			b->st.ad_hidden++;
			continue;
		}
		n = node_insert(b, e->ino, dir, nm, e->nlen, e->type == S_IFDIR, 0);
		if (want) {
			const struct openmacfuse_attr *ca = n ? attr_get(b, e->ino, want) : NULL;

			if (ca) {
				b->st.attr_hits++;
				a = *ca;
				if (n)
					a.change += n->gen;
				pa = &a;
			} else if (join_path(b->path2, b->path1, dlen, nm, e->nlen) >= 0 &&
				   do_getattr(b, b->path2, want, &a) == 0) {
				a.ino = e->ino;
				if (n) {
					node_set_attr(b, n, &a, want);
					a.change += n->gen;
				}
				pa = &a;
			}
		} else {
			memset(&a, 0, sizeof(a));
			a.ino = e->ino;
			a.mode = e->type;
			pa = &a;
		}
		de.name = nm;
		de.ino = e->ino;
		de.pos = i + 1;
		de.attr = pa;
		b->st.readdir_ents++;
		if (fill(arg, &de))
			return 0;
	}
	*eof = 1;
	return 0;
}

static int bf_access(void *ctx, openmacfuse_ino_t ino, int mask)
{
	struct bf_ctx *b = ctx;
	int r;

	if (!b->op->access)
		return -ENOSYS;
	r = path_of(b, ino, b->path1);
	if (r < 0)
		return r;
	return b->op->access(b->path1, mask);
}

static int bf_open(void *ctx, openmacfuse_ino_t ino, int flags, uint64_t *fh)
{
	struct bf_ctx *b = ctx;
	struct bf_node *n = node_find(b, ino);
	struct bf_open *o;
	int r;

	if (!n)
		return -ESTALE;
	r = node_path(b, n, b->path1, BF_PATH_MAX);
	if (r < 0)
		return r;
	o = calloc(1, sizeof(*o));
	if (!o)
		return -ENOMEM;
	o->fi.flags = flags;
	o->ino = ino;
	r = b->op->open ? b->op->open(b->path1, &o->fi) : 0;
	if (r) {
		free(o);
		return r;
	}
	if (n->nopen++ == 0)
		lru_unlink(b, n);	/* open nodes are not evicted */
	b->st.opens++;
	*fh = (uint64_t)(uintptr_t)o;
	return 0;
}

static int bf_release(void *ctx, openmacfuse_ino_t ino, uint64_t fh)
{
	struct bf_ctx *b = ctx;
	struct bf_open *o = (struct bf_open *)(uintptr_t)fh;
	struct bf_node *n = node_find(b, ino);
	int r = 0;

	if (!o)
		return 0;
	if (n && node_path(b, n, b->path1, BF_PATH_MAX) >= 0) {
		if (b->op->flush)
			b->op->flush(b->path1, &o->fi);
		if (b->op->release)
			r = b->op->release(b->path1, &o->fi);
	}
	if (n && n->nopen > 0 && --n->nopen == 0 && evictable(b, n) && !n->inlru)
		lru_push_head(b, n);
	free(o);
	return r;
}

static int bf_read(void *ctx, openmacfuse_ino_t ino, uint64_t fh, void *buf,
		   uint32_t size, uint64_t off)
{
	struct bf_ctx *b = ctx;
	struct bf_open *o = (struct bf_open *)(uintptr_t)fh;
	uint32_t total = 0;
	int r;

	if (!b->op->read)
		return -ENOSYS;
	r = path_of(b, ino, b->path1);
	if (r < 0)
		return r;
	b->st.read_calls++;
	/* FUSE's read only returns short at end of file; to be safe, keep reading until full or until it returns 0 */
	while (total < size) {
		r = b->op->read(b->path1, (char *)buf + total, size - total,
				(off_t)(off + total), o ? &o->fi : NULL);
		if (r < 0)
			return total ? (int)total : r;
		if (r == 0)
			break;
		total += (uint32_t)r;
	}
	b->st.read_bytes += total;
	return (int)total;
}

static int bf_readlink(void *ctx, openmacfuse_ino_t ino, char *buf, size_t size)
{
	struct bf_ctx *b = ctx;
	int r;

	if (!b->op->readlink)
		return -EINVAL;
	r = path_of(b, ino, b->path1);
	if (r < 0)
		return r;
	buf[0] = 0;
	r = b->op->readlink(b->path1, buf, size);
	buf[size - 1] = 0;
	return r;
}

static int bf_statfs(void *ctx, struct openmacfuse_statfs *sf)
{
	struct bf_ctx *b = ctx;
	struct statvfs sv;
	uint64_t t = now_ns();
	int r;

	/* Finder / the system ask for volume space every 1-3 seconds: cache it for 1 second */
	if (b->sf_ok && t - b->sf_at < 1000000000ull) {
		*sf = b->sf;
		return 0;
	}
	if (!b->op->statfs)
		return -ENOSYS;
	memset(&sv, 0, sizeof(sv));
	r = b->op->statfs("/", &sv);
	if (r)
		return r;
	b->sf.bsize = sv.f_frsize ? sv.f_frsize : sv.f_bsize;
	b->sf.blocks = sv.f_blocks;
	b->sf.bfree = sv.f_bfree;
	b->sf.bavail = sv.f_bavail;
	b->sf.files = sv.f_files;
	b->sf.ffree = sv.f_ffree;
	b->sf.namemax = (uint32_t)sv.f_namemax;
	b->sf_at = t;
	b->sf_ok = 1;
	*sf = b->sf;
	return 0;
}

/*
 * Named-attribute experiment (OPENMACFUSE_NAMEDATTR=2): real xattrs cannot be written on a read-only volume,
 * so every object reports one extra attribute, org.openmacfuse.probe = "probe-value", to observe whether the client
 * reads attribute values through OPENATTR / READDIR / OPEN / READ. Off in normal operation.
 */
#define BF_PROBE_NAME	"org.openmacfuse.probe"
#define BF_PROBE_VALUE	"probe-value"


/* ---------- writing ---------- */

static void attr_drop(struct bf_ctx *b, uint64_t ino)
{
	struct bf_aslot *sl = &b->acache[mix64(ino) & (BF_ACACHE_SLOTS - 1)];

	if (sl->ino == ino)
		sl->ino = 0;
}

/* A directory's contents changed: invalidate its listing snapshot and cached attributes and bump its generation (change / verifier must change) */
static void dir_changed(struct bf_ctx *b, uint64_t dir)
{
	struct bf_node *d = node_find(b, dir);
	struct bf_snap *s = snap_find(b, dir);

	if (s) {
		snap_unlink(b, s);
		snap_free(s);
	}
	attr_drop(b, dir);
	if (d)
		d->gen++;
	b->sf_ok = 0;		/* volume space changed too */
}

/* Put the path of directory dir into path1 and dir/name into path2 */
static int child_path(struct bf_ctx *b, uint64_t dir, const char *name)
{
	struct bf_node *d = node_find(b, dir);
	int len;

	if (!d)
		return -ESTALE;
	len = node_path(b, d, b->path1, BF_PATH_MAX);
	if (len < 0)
		return len;
	len = join_path(b->path2, b->path1, len, name, strlen(name));
	return len < 0 ? len : 0;
}

/* Write the full path of name under directory dir into out (out must not be path1) */
static int child_path_into(struct bf_ctx *b, uint64_t dir, const char *name,
			   char *out)
{
	struct bf_node *d = node_find(b, dir);
	int len;

	if (!d)
		return -ESTALE;
	len = node_path(b, d, out, BF_PATH_MAX);
	if (len < 0)
		return len;
	len = join_path(out, out, len, name, strlen(name));
	return len < 0 ? len : 0;
}

/*
 * Write the real on-disk name under dir that is equivalent to name into real (OPENMACFUSE_KEY_MAX bytes):
 * first getattr the name as given,
 * and if that fails consult the directory name index (NFC / NFD forms are always equivalent; case too when case-insensitive).
 * Returns 0 on success with *a holding the attributes; on return path1 = directory path, path2 = directory/real name.
 * With more than one equivalent name (the volume already has a conflict): one differing from name only in form (same NFC) is preferred, otherwise the first.
 */
static int real_name(struct bf_ctx *b, uint64_t dir, const char *name, char *real,
		     struct openmacfuse_attr *a)
{
	size_t nl = strlen(name);
	struct bf_didx *x;
	uint32_t c[8];
	long ex;
	int r, n, i, pick = 0;
	const char *rn;

	if (nl >= OPENMACFUSE_KEY_MAX)
		return -ENAMETOOLONG;
	r = child_path(b, dir, name);
	if (r)
		return r;
	r = do_getattr(b, b->path2, OPENMACFUSE_WANT_BASIC, a);
	if (r != -ENOENT) {
		if (r == 0)
			memcpy(real, name, nl + 1);
		return r;
	}
	if ((!b->ci && openmacfuse_name_is_ascii(name, nl)) || b->raw_names)
		return -ENOENT;		/* case-sensitive + pure ASCII: there is no other spelling */
	x = didx_get(b, dir, b->path1);
	if (!x)
		return -ENOENT;
	n = didx_match(b, x, name, nl, &ex, c, 8);
	if (ex >= 0) {
		/* The index says the name exists but the disk does not have it: the index is stale, drop it */
		didx_drop(b, dir);
		b->st.didx_drops++;
		return -ENOENT;
	}
	if (n == 0)
		return -ENOENT;
	if (n > 1) {
		b->st.fold_ambig++;
		for (i = 0; i < n && i < 8; i++)
			if (nfc_equal(x->names + x->e[c[i]].noff, x->e[c[i]].nlen, name, nl)) {
				pick = i;
				break;
			}
	}
	rn = x->names + x->e[c[pick]].noff;
	memcpy(real, rn, x->e[c[pick]].nlen);
	real[x->e[c[pick]].nlen] = 0;
	r = child_path(b, dir, real);
	if (r == 0)
		r = do_getattr(b, b->path2, OPENMACFUSE_WANT_BASIC, a);
	if (r) {
		didx_drop(b, dir);
		b->st.didx_drops++;
		return -ENOENT;
	}
	if (nfc_equal(real, strlen(real), name, nl))
		b->st.norm_hits++;
	else
		b->st.fold_hits++;
	return 0;
}

/*
 * Target name of a create / rename: converted to NFC if so configured and written into fname; if the
 * directory already has a name equivalent to it (a different form, or
 * a different case when case-insensitive) returns -EEXIST. On success path1 = directory path, path2 = directory/fname.
 */
static int final_name(struct bf_ctx *b, const char *name, char *fname)
{
	size_t nl = strlen(name);
	int n;

	if (b->nfc && !b->raw_names && !openmacfuse_name_is_ascii(name, nl) &&
	    (n = openmacfuse_name_nfc(name, nl, fname, OPENMACFUSE_KEY_MAX)) > 0) {
		if ((size_t)n != nl || memcmp(fname, name, nl) != 0)
			b->st.nfc_names++;
		nl = (size_t)n;
	} else {
		if (nl >= OPENMACFUSE_KEY_MAX)
			return -ENAMETOOLONG;
		memcpy(fname, name, nl + 1);
	}
	return nl > 255 ? -ENAMETOOLONG : 0;
}

static int prep_new(struct bf_ctx *b, uint64_t dir, const char *name, char *fname)
{
	int r = final_name(b, name, fname);
	size_t fl;

	if (r)
		return r;
	r = child_path(b, dir, fname);
	if (r)
		return r;
	fl = strlen(fname);
	if (!b->raw_names && (b->ci || !openmacfuse_name_is_ascii(fname, fl))) {
		struct bf_didx *x = didx_get(b, dir, b->path1);
		uint32_t c[1];
		long ex;

		if (x && didx_match(b, x, fname, fl, &ex, c, 1) > 0) {
			b->st.exist_equiv++;
			return -EEXIST;
		}
		r = child_path(b, dir, fname);
	}
	return r;
}

/* Attach a node under a new parent directory / name (rename, or a hard link switching canonical name) */
static int node_move(struct bf_ctx *b, struct bf_node *n, uint64_t newparent,
		     const char *name)
{
	struct bf_node *op = node_find(b, n->parent), *np = node_find(b, newparent);
	char *nm;

	if (!np || n->ino == b->root)
		return -ESTALE;
	nm = strdup(name);
	if (!nm)
		return -ENOMEM;
	if (n->parent != newparent) {
		if (op && op != n && op->nchild > 0) {
			op->nchild--;
			if (evictable(b, op) && !op->inlru)
				lru_push_head(b, op);
		}
		if (np->nchild++ == 0)
			lru_unlink(b, np);	/* it has a child now and can no longer be evicted */
		n->parent = newparent;
	}
	free(n->name);
	n->name = nm;
	return 0;
}

/* Take a node that no longer exists out of the table (does not count as an eviction) */
static void node_forget(struct bf_ctx *b, struct bf_node *n)
{
	if (n->ino == b->root || n->nopen > 0 || n->nchild > 0)
		return;		/* open / still has children: keep it, a later getattr will find it gone */
	node_drop(b, n);
	b->st.evictions--;
}

/*
 * The name (dir, name) has been removed (REMOVE, or a RENAME overwrote the target); a holds the attributes from before the removal.
 * If it was the node's canonical name: re-attach the node to another hard-link name if one exists (and is known), otherwise drop the node.
 */
static void forget_name(struct bf_ctx *b, uint64_t dir, const char *name,
			const struct openmacfuse_attr *a)
{
	struct bf_node *n = node_find(b, a->ino);
	struct bf_hint *h = hint_find(b, a->ino);

	if (h && h->parent == dir && h->name && strcmp(h->name, name) == 0) {
		hint_clear(h);
		h = NULL;
	}
	attr_drop(b, a->ino);
	if (!n || n->parent != dir || !n->name || strcmp(n->name, name) != 0) {
		if (n)
			n->gen++;	/* link count changed */
		return;
	}
	if (!S_ISDIR(a->mode) && a->nlink > 1) {
		n->gen++;
		if (h && node_find(b, h->parent) && node_move(b, n, h->parent, h->name) == 0) {
			hint_clear(h);
			return;
		}
	}
	node_forget(b, n);
}

/* A newly created object: fetch its attributes and put it in the node table (pinned for this batch) */
static int adopt_new(struct bf_ctx *b, uint64_t dir, const char *name,
		     struct openmacfuse_attr *a)
{
	struct bf_node *n;
	int r = do_getattr(b, b->path2, OPENMACFUSE_WANT_BASIC, a);

	if (r)
		return r;
	if (a->ino == 0 || a->ino == dir)
		return -EIO;
	n = node_insert(b, a->ino, dir, name, strlen(name), S_ISDIR(a->mode), 1);
	if (!n)
		return -ENOMEM;
	if (n->parent != dir || !n->name || strcmp(n->name, name) != 0) {
		/* A stale node with the same number is still in the table (the inode number was reused): re-attach it at the new location */
		free(n->fchain);
		n->fchain = NULL;
		n->nfchain = 0;
		node_move(b, n, dir, name);
	}
	node_set_attr(b, n, a, OPENMACFUSE_WANT_BASIC);
	b->st.creates++;
	return 0;
}

static int bf_write(void *ctx, openmacfuse_ino_t ino, uint64_t fh, const void *buf,
		    uint32_t size, uint64_t off)
{
	struct bf_ctx *b = ctx;
	struct bf_open *o = (struct bf_open *)(uintptr_t)fh;
	uint32_t total = 0;
	int r;

	if (!b->op->write)
		return -EROFS;
	r = path_of(b, ino, b->path1);
	if (r < 0)
		return r;
	while (total < size) {
		r = b->op->write(b->path1, (const char *)buf + total, size - total,
				 (off_t)(off + total), o ? &o->fi : NULL);
		if (r < 0) {
			if (!total)
				return r;
			break;
		}
		if (r == 0) {
			if (!total)
				return -EIO;
			break;
		}
		total += (uint32_t)r;
	}
	attr_drop(b, ino);
	b->st.writes++;
	b->st.write_bytes += total;
	return (int)total;
}

static int bf_fsync(void *ctx, openmacfuse_ino_t ino, uint64_t fh, int datasync)
{
	struct bf_ctx *b = ctx;
	struct bf_open *o = (struct bf_open *)(uintptr_t)fh;
	struct fuse_file_info tmp;
	int r;

	if (!b->op->fsync)
		return 0;
	r = path_of(b, ino, b->path1);
	if (r < 0)
		return r;
	memset(&tmp, 0, sizeof(tmp));
	b->st.fsyncs++;
	return b->op->fsync(b->path1, datasync, o ? &o->fi : &tmp);
}

static int bf_create(void *ctx, openmacfuse_ino_t dir, const char *oname, uint32_t mode,
		     int flags, openmacfuse_ino_t *ino, uint64_t *fh)
{
	struct bf_ctx *b = ctx;
	struct bf_open *o;
	struct bf_node *n;
	struct openmacfuse_attr a;
	char name[OPENMACFUSE_KEY_MAX];
	int r = prep_new(b, dir, oname, name);

	if (r)
		return r;
	o = calloc(1, sizeof(*o));
	if (!o)
		return -ENOMEM;
	o->fi.flags = flags | O_CREAT | O_EXCL;
	if (b->op->create) {
		r = b->op->create(b->path2, S_IFREG | (mode & 07777), &o->fi);
	} else if (b->op->mknod) {
		r = b->op->mknod(b->path2, S_IFREG | (mode & 07777), 0);
		if (r == 0 && b->op->open) {
			o->fi.flags = flags;
			r = b->op->open(b->path2, &o->fi);
		}
	} else {
		r = -EROFS;
	}
	if (r) {
		free(o);
		return r;
	}
	r = adopt_new(b, dir, name, &a);
	if (r) {
		if (b->op->release)
			b->op->release(b->path2, &o->fi);
		free(o);
		didx_drop(b, dir);	/* created, but its attributes cannot be fetched: drop the index, rebuild next time */
		return r;
	}
	n = node_find(b, a.ino);
	if (n && n->nopen++ == 0)
		lru_unlink(b, n);	/* open nodes are not evicted */
	o->ino = a.ino;
	b->st.opens++;
	dir_changed(b, dir);
	didx_note_add(b, dir, name, a.ino);
	*ino = a.ino;
	*fh = (uint64_t)(uintptr_t)o;
	return 0;
}

static int bf_mkdir(void *ctx, openmacfuse_ino_t dir, const char *oname, uint32_t mode,
		    openmacfuse_ino_t *ino)
{
	struct bf_ctx *b = ctx;
	struct openmacfuse_attr a;
	char name[OPENMACFUSE_KEY_MAX];
	int r = prep_new(b, dir, oname, name);

	if (r)
		return r;
	if (!b->op->mkdir)
		return -EROFS;
	r = b->op->mkdir(b->path2, mode & 07777);
	if (r)
		return r;
	r = adopt_new(b, dir, name, &a);
	dir_changed(b, dir);
	if (r) {
		didx_drop(b, dir);	/* created, but its attributes cannot be fetched: drop the index, rebuild next time */
		return r;
	}
	didx_note_add(b, dir, name, a.ino);
	*ino = a.ino;
	return 0;
}

static int bf_symlink(void *ctx, openmacfuse_ino_t dir, const char *oname,
		      const char *target, openmacfuse_ino_t *ino)
{
	struct bf_ctx *b = ctx;
	struct openmacfuse_attr a;
	char name[OPENMACFUSE_KEY_MAX];
	int r = prep_new(b, dir, oname, name);

	if (r)
		return r;
	if (!b->op->symlink)
		return -EROFS;
	r = b->op->symlink(target, b->path2);
	if (r)
		return r;
	r = adopt_new(b, dir, name, &a);
	dir_changed(b, dir);
	if (r) {
		didx_drop(b, dir);	/* created, but its attributes cannot be fetched: drop the index, rebuild next time */
		return r;
	}
	didx_note_add(b, dir, name, a.ino);
	*ino = a.ino;
	return 0;
}

static int bf_mknod(void *ctx, openmacfuse_ino_t dir, const char *oname, uint32_t mode,
		    uint64_t rdev, openmacfuse_ino_t *ino)
{
	struct bf_ctx *b = ctx;
	struct openmacfuse_attr a;
	char name[OPENMACFUSE_KEY_MAX];
	int r = prep_new(b, dir, oname, name);

	if (r)
		return r;
	if (!b->op->mknod)
		return -ENOTSUP;
	r = b->op->mknod(b->path2, (mode_t)mode, (dev_t)rdev);
	if (r)
		return r;
	r = adopt_new(b, dir, name, &a);
	dir_changed(b, dir);
	if (r) {
		didx_drop(b, dir);	/* created, but its attributes cannot be fetched: drop the index, rebuild next time */
		return r;
	}
	didx_note_add(b, dir, name, a.ino);
	*ino = a.ino;
	return 0;
}

static int bf_link(void *ctx, openmacfuse_ino_t ino, openmacfuse_ino_t newdir,
		   const char *oname)
{
	struct bf_ctx *b = ctx;
	struct bf_node *n;
	char newname[OPENMACFUSE_KEY_MAX];
	int r;

	if (!b->op->link || b->synth)
		return b->op->link ? -ENOTSUP : -EROFS;	/* hard links cannot be represented when inode numbers are assigned by path */
	r = prep_new(b, newdir, oname, newname);	/* an equivalent name already exists: EEXIST */
	if (r)
		return r;
	r = path_of(b, ino, b->path1);
	if (r < 0)
		return r;
	r = child_path_into(b, newdir, newname, b->path2);
	if (r)
		return r;
	r = b->op->link(b->path1, b->path2);
	if (r)
		return r;
	attr_drop(b, ino);
	n = node_find(b, ino);
	if (n)
		n->gen++;
	hint_set(b, ino, newdir, newname);
	dir_changed(b, newdir);
	didx_note_add(b, newdir, newname, ino);
	b->st.links++;
	return 0;
}

/* Remove the real name rn under dir (a holds the attributes from before the removal) */
static int do_remove(struct bf_ctx *b, uint64_t dir, const char *rn,
		     const struct openmacfuse_attr *a)
{
	int r = child_path(b, dir, rn);

	if (r)
		return r;
	if (S_ISDIR(a->mode))
		r = b->op->rmdir ? b->op->rmdir(b->path2) : -EROFS;
	else
		r = b->op->unlink ? b->op->unlink(b->path2) : -EROFS;
	if (r)
		return r;
	if (b->synth)
		openmacfuse_synth_drop(b->synth, b->path2, S_ISDIR(a->mode));
	forget_name(b, dir, rn, a);
	dir_changed(b, dir);
	didx_note_del(b, dir, rn);
	if (S_ISDIR(a->mode))
		didx_drop(b, a->ino);
	b->st.removes++;
	return 0;
}

static int is_ad_name(const char *n)
{
	return n[0] == '.' && n[1] == '_' && n[2];
}

static int ad_path(struct bf_ctx *b, uint64_t dir, const char *name, struct openmacfuse_attr *a);

/* X (real name xn under dir) has been removed: on the named-attribute channel its `._X` is removed with it */
static void ad_follow_remove(struct bf_ctx *b, uint64_t dir, const char *xn)
{
	char adn[OPENMACFUSE_KEY_MAX + 2];
	struct openmacfuse_attr ada;

	if (!b->adtrans || is_ad_name(xn) || ad_path(b, dir, xn, &ada))
		return;
	snprintf(adn, sizeof(adn), "._%s", xn);
	if (do_remove(b, dir, adn, &ada) == 0) {
		b->st.ad_follow_rm++;
		b->st.removes--;
	}
}

static int bf_remove(void *ctx, openmacfuse_ino_t dir, const char *name)
{
	struct bf_ctx *b = ctx;
	struct openmacfuse_attr a;
	char rn[OPENMACFUSE_KEY_MAX];
	int r = real_name(b, dir, name, rn, &a);

	if (r)
		return r;
	r = do_remove(b, dir, rn, &a);
	if (r)
		return r;
	ad_follow_remove(b, dir, rn);
	return 0;
}

/* Rename (both names are real on-disk names / final names); the node table, "moved" table and name index are fixed up on the spot */
static int do_rename(struct bf_ctx *b, uint64_t olddir, const char *oldname,
		     uint64_t newdir, const char *newname)
{
	struct openmacfuse_attr sa, da;
	struct bf_node *x, *p;
	int r, dst_exists, canonical;

	if (!b->op->rename)
		return -EROFS;
	r = child_path_into(b, olddir, oldname, b->path3);
	if (r)
		return r;
	r = child_path_into(b, newdir, newname, b->path2);
	if (r)
		return r;
	r = do_getattr(b, b->path3, OPENMACFUSE_WANT_BASIC, &sa);
	if (r)
		return r;
	dst_exists = do_getattr(b, b->path2, OPENMACFUSE_WANT_BASIC, &da) == 0;
	if (dst_exists && da.ino == sa.ino && sa.ino) {
		/*
		 * Two names for the same file: POSIX says do nothing. Exception: a rename within one directory
		 * that only changes case /
		 * form (we get here when the file system's own lookup is case-insensitive) is still carried out.
		 */
		if (olddir != newdir || strcmp(oldname, newname) == 0)
			return 0;
		dst_exists = 0;
	}
	if (S_ISDIR(sa.mode)) {
		/* A directory cannot be moved into itself or one of its descendants */
		for (p = node_find(b, newdir); p; p = node_find(b, p->parent)) {
			if (p->ino == sa.ino)
				return -EINVAL;
			if (p->ino == b->root)
				break;
		}
		if (dst_exists && !S_ISDIR(da.mode))
			return -ENOTDIR;
	} else if (dst_exists && S_ISDIR(da.mode)) {
		return -EISDIR;
	}
	x = node_find(b, sa.ino);
	canonical = x && x->parent == olddir && x->name && strcmp(x->name, oldname) == 0;
	if (canonical)
		freeze_subtree(b, x);	/* handle bytes already issued stay the same */
	r = b->op->rename(b->path3, b->path2);
	if (r)
		return r;
	if (b->synth)
		openmacfuse_synth_rename(b->synth, b->path3, b->path2);	/* numbers follow the path, handles stay valid */
	if (dst_exists)
		forget_name(b, newdir, newname, &da);	/* the target was overwritten */
	{
		struct bf_hint *h = hint_find(b, sa.ino);
		if (h && h->parent == olddir && h->name && strcmp(h->name, oldname) == 0)
			hint_clear(h);
	}
	x = node_find(b, sa.ino);
	if (x && canonical)
		node_move(b, x, newdir, newname);
	if (x)
		x->gen++;
	hint_set(b, sa.ino, newdir, newname);	/* moved: the node can still be recovered if evicted later */
	attr_drop(b, sa.ino);
	dir_changed(b, olddir);
	if (newdir != olddir)
		dir_changed(b, newdir);
	didx_note_del(b, olddir, oldname);
	if (dst_exists) {
		didx_note_del(b, newdir, newname);
		if (S_ISDIR(da.mode))
			didx_drop(b, da.ino);
	}
	didx_note_add(b, newdir, newname, sa.ino);
	b->st.renames++;
	return 0;
}


/*
 * After X is renamed (olddir/on -> newdir/nn): `._X` is renamed along with it; if X has no `._` but the
 * new name used to have one
 * (left behind by the old file that was overwritten), that one is removed.
 */
static void ad_follow_rename(struct bf_ctx *b, uint64_t olddir, const char *on,
			     uint64_t newdir, const char *nn)
{
	char oad[OPENMACFUSE_KEY_MAX + 2], nad[OPENMACFUSE_KEY_MAX + 2];
	struct openmacfuse_attr ada;

	if (!b->adtrans || is_ad_name(on) || is_ad_name(nn))
		return;
	snprintf(oad, sizeof(oad), "._%s", on);
	snprintf(nad, sizeof(nad), "._%s", nn);
	if (ad_path(b, olddir, on, &ada) == 0) {
		if (do_rename(b, olddir, oad, newdir, nad) == 0) {
			b->st.ad_follow_mv++;
			b->st.renames--;
		}
	} else if (ad_path(b, newdir, nn, &ada) == 0) {
		if (do_remove(b, newdir, nad, &ada) == 0) {
			b->st.ad_follow_rm++;
			b->st.removes--;
		}
	}
}

/*
 * RENAME: the source name is looked up by equivalent name; the target name is converted to NFC if so configured. When the target location already has an equivalent name spelled differently:
 *   - it is the source itself (same directory, only case / form changes): just rename;
 *   - it is another object: overwrite it first (rename onto its real name, an atomic replace), then rename to the new spelling.
 */
static int bf_rename(void *ctx, openmacfuse_ino_t olddir, const char *oldname,
		     openmacfuse_ino_t newdir, const char *newname)
{
	struct bf_ctx *b = ctx;
	char oreal[OPENMACFUSE_KEY_MAX], nfinal[OPENMACFUSE_KEY_MAX], treal[OPENMACFUSE_KEY_MAX];
	struct openmacfuse_attr sa, ta;
	int r;

	if (!b->op->rename)
		return -EROFS;
	r = real_name(b, olddir, oldname, oreal, &sa);
	if (r)
		return r;
	r = final_name(b, newname, nfinal);
	if (r)
		return r;
	r = real_name(b, newdir, nfinal, treal, &ta);
	if (r && r != -ENOENT)
		return r;
	if (r == 0 && strcmp(treal, nfinal) != 0) {
		if (ta.ino == sa.ino) {
			if (olddir != newdir || strcmp(oreal, treal) != 0)
				return 0;	/* another name of a hard link: POSIX says do nothing */
			if (strcmp(oreal, nfinal) == 0)
				return 0;
			r = do_rename(b, olddir, oreal, newdir, nfinal);
			if (r == 0) {
				b->st.case_renames++;
				ad_follow_rename(b, olddir, oreal, newdir, nfinal);
			}
			return r;
		}
		r = do_rename(b, olddir, oreal, newdir, treal);
		if (r)
			return r;
		ad_follow_rename(b, olddir, oreal, newdir, treal);
		r = do_rename(b, newdir, treal, newdir, nfinal);
		if (r == 0) {
			b->st.case_renames++;
			ad_follow_rename(b, newdir, treal, newdir, nfinal);
		}
		return r;
	}
	r = do_rename(b, olddir, oreal, newdir, nfinal);
	if (r == 0)
		ad_follow_rename(b, olddir, oreal, newdir, nfinal);
	return r;
}

static int bf_setattr(void *ctx, openmacfuse_ino_t ino, uint64_t fh,
		      const struct openmacfuse_sattr *sa)
{
	struct bf_ctx *b = ctx;
	struct bf_open *o = (struct bf_open *)(uintptr_t)fh;
	struct bf_node *n = node_find(b, ino);
	struct openmacfuse_attr cur;
	int r = path_of(b, ino, b->path1), have_cur = 0;

	if (r < 0)
		return r;
	/* Only one of owner / group given, only one time given, or flags being changed: the current values are needed to fill in the rest */
	if (((sa->mask & (OPENMACFUSE_SA_UID | OPENMACFUSE_SA_GID)) &&
	     (sa->mask & (OPENMACFUSE_SA_UID | OPENMACFUSE_SA_GID)) != (OPENMACFUSE_SA_UID | OPENMACFUSE_SA_GID)) ||
	    ((sa->mask & (OPENMACFUSE_SA_ATIME | OPENMACFUSE_SA_ATIME_NOW | OPENMACFUSE_SA_MTIME | OPENMACFUSE_SA_MTIME_NOW)) &&
	     (!(sa->mask & (OPENMACFUSE_SA_ATIME | OPENMACFUSE_SA_ATIME_NOW)) ||
	      !(sa->mask & (OPENMACFUSE_SA_MTIME | OPENMACFUSE_SA_MTIME_NOW)))) ||
	    (sa->mask & OPENMACFUSE_SA_FLAGS)) {
		r = do_getattr(b, b->path1, OPENMACFUSE_WANT_BASIC, &cur);
		if (r)
			return r;
		have_cur = 1;
	}
	if (sa->mask & OPENMACFUSE_SA_MODE) {
		if (!b->op->chmod)
			return -ENOTSUP;
		r = b->op->chmod(b->path1, (mode_t)(sa->mode & 07777));
		if (r)
			return r;
	}
	if (sa->mask & (OPENMACFUSE_SA_UID | OPENMACFUSE_SA_GID)) {
		uid_t uid = (sa->mask & OPENMACFUSE_SA_UID) ? (uid_t)sa->uid : (uid_t)cur.uid;
		gid_t gid = (sa->mask & OPENMACFUSE_SA_GID) ? (gid_t)sa->gid : (gid_t)cur.gid;

		if (!b->op->chown)
			return -ENOTSUP;
		r = b->op->chown(b->path1, uid, gid);
		if (r)
			return r;
	}
	if (sa->mask & OPENMACFUSE_SA_SIZE) {
		if (o && b->op->ftruncate)
			r = b->op->ftruncate(b->path1, (off_t)sa->size, &o->fi);
		else if (b->op->truncate)
			r = b->op->truncate(b->path1, (off_t)sa->size);
		else
			r = -ENOTSUP;
		if (r)
			return r;
	}
	if (sa->mask & (OPENMACFUSE_SA_ATIME | OPENMACFUSE_SA_ATIME_NOW | OPENMACFUSE_SA_MTIME |
			OPENMACFUSE_SA_MTIME_NOW)) {
		struct timespec tv[2], now;

		clock_gettime(CLOCK_REALTIME, &now);
		tv[0] = (sa->mask & OPENMACFUSE_SA_ATIME) ? sa->atime :
			(sa->mask & OPENMACFUSE_SA_ATIME_NOW) ? now : cur.atime;
		tv[1] = (sa->mask & OPENMACFUSE_SA_MTIME) ? sa->mtime :
			(sa->mask & OPENMACFUSE_SA_MTIME_NOW) ? now : cur.mtime;
		(void)have_cur;
		if (b->op->utimens) {
			r = b->op->utimens(b->path1, tv);
		} else if (b->op->utime) {
			struct utimbuf u = { tv[0].tv_sec, tv[1].tv_sec };
			r = b->op->utime(b->path1, &u);
		} else {
			r = -ENOTSUP;
		}
		if (r)
			return r;
	}
	if (sa->mask & OPENMACFUSE_SA_CRTIME) {
		if (!b->op->setcrtime)
			return -ENOTSUP;
		r = b->op->setcrtime(b->path1, &sa->crtime);
		if (r)
			return r;
	}
	if (sa->mask & OPENMACFUSE_SA_BKUPTIME) {
		if (!b->op->setbkuptime)
			return -ENOTSUP;
		r = b->op->setbkuptime(b->path1, &sa->bkuptime);
		if (r)
			return r;
	}
	if (sa->mask & OPENMACFUSE_SA_FLAGS) {
		uint32_t nf = (cur.flags | sa->flags_set) & ~sa->flags_clear;

		/*
		 * hidden / archive: when creating files or doing cp -p the client often sends them along with
		 * other attributes
		 * (with the same value as now). If the value is unchanged nothing is called; if the file system has no chflags callback they are skipped
		 * (logged once only) so that the other attributes in the same SETATTR do not fail along with them.
		 */
		if (nf != cur.flags) {
			if (b->op->chflags) {
				r = b->op->chflags(b->path1, nf);
				if (r)
					return r;
			} else if (!b->warned_flags) {
				b->warned_flags = 1;
				if (b->log)
					b->log("the file system has no chflags callback: changes to hidden / archive are ignored");
			}
		}
	}
	attr_drop(b, ino);
	if (n)
		n->gen++;
	b->st.setattrs++;
	return 0;
}

static void bf_invalidate(void *ctx, openmacfuse_ino_t ino)
{
	attr_drop(ctx, ino);
}

/* ---------- `._X` (AppleDouble) translation ---------- */

/*
 * The `._X` of X (real name name under dir): its path is written into path4; returns 0 if it exists and is a regular file (*a holds its attributes).
 * If the directory name index is there it is consulted first (if the name is absent there is no need to ask the file system).
 */
static int ad_path(struct bf_ctx *b, uint64_t dir, const char *name, struct openmacfuse_attr *a)
{
	struct bf_node *d = node_find(b, dir);
	struct bf_didx *x;
	char adn[OPENMACFUSE_KEY_MAX + 2];
	size_t al;
	int len;

	if (!d || is_ad_name(name))
		return -ENOENT;
	al = (size_t)snprintf(adn, sizeof(adn), "._%s", name);
	if (al > 255)
		return -ENOENT;
	x = didx_find(b, dir);
	if (x) {
		uint32_t c[1];
		long ex;

		if (!x->nad)
			return -ENOENT;
		didx_match(b, x, adn, al, &ex, c, 0);
		if (ex < 0)
			return -ENOENT;
	}
	len = node_path(b, d, b->path4, BF_PATH_MAX);
	if (len < 0)
		return len;
	len = join_path(b->path4, b->path4, len, adn, al);
	if (len < 0)
		return len;
	if (do_getattr(b, b->path4, OPENMACFUSE_WANT_BASIC, a) || !S_ISREG(a->mode))
		return -ENOENT;
	return 0;
}

struct ad_rd {
	struct bf_ctx		*b;
	struct fuse_file_info	fi;
	int			open;
};

static int ad_read_cb(void *ctx, void *buf, uint32_t size, uint64_t off)
{
	struct ad_rd *r = ctx;

	if (!r->b->op->read)
		return -ENOSYS;
	return r->b->op->read(r->b->path4, buf, size, (off_t)off, &r->fi);
}

static int ad_open(struct bf_ctx *b, struct ad_rd *r)
{
	memset(r, 0, sizeof(*r));
	r->b = b;
	r->fi.flags = O_RDONLY;
	if (b->op->open && b->op->open(b->path4, &r->fi))
		return -EIO;
	r->open = 1;
	return 0;
}

static void ad_close(struct bf_ctx *b, struct ad_rd *r)
{
	if (r->open) {
		if (b->op->release)
			b->op->release(b->path4, &r->fi);
		r->open = 0;
	}
}

/* Parsed form of the `._X` at path4 (attributes ada); cached by inode + size + mtime. Returns NULL if it is not AppleDouble */
static const struct openmacfuse_ad *ad_get(struct bf_ctx *b, const struct openmacfuse_attr *ada)
{
	struct bf_adc *sl, *old = &b->adc[0];
	struct ad_rd rd;
	int i, r;

	for (i = 0; i < BF_ADC_SLOTS; i++) {
		struct bf_adc *c = &b->adc[i];
		if (c->adino && c->adino == ada->ino && c->size == ada->size &&
		    c->mtime.tv_sec == ada->mtime.tv_sec &&
		    c->mtime.tv_nsec == ada->mtime.tv_nsec) {
			c->stamp = ++b->adc_clock;
			return &c->ad;
		}
		if (c->stamp < old->stamp)
			old = c;
	}
	sl = old;
	openmacfuse_ad_free(&sl->ad);
	sl->adino = 0;
	if (ad_open(b, &rd))
		return NULL;
	r = openmacfuse_ad_parse(ad_read_cb, &rd, ada->size, &sl->ad);
	ad_close(b, &rd);
	b->st.ad_parsed++;
	if (r) {
		b->st.ad_bad++;
		return NULL;
	}
	sl->adino = ada->ino;
	sl->size = ada->size;
	sl->mtime = ada->mtime;
	sl->stamp = ++b->adc_clock;
	return &sl->ad;
}

static void ad_forget(struct bf_ctx *b, uint64_t adino)
{
	int i;

	for (i = 0; i < BF_ADC_SLOTS; i++)
		if (b->adc[i].adino == adino) {
			openmacfuse_ad_free(&b->adc[i].ad);
			b->adc[i].adino = 0;
		}
}

/* Read [off, off+size) of an attribute value from the `._X` at path4 */
static int ad_read_val(struct bf_ctx *b, const struct openmacfuse_ad_attr *at, void *buf,
		       uint32_t size, uint64_t off)
{
	struct ad_rd rd;
	uint32_t got = 0;
	int r = 0;

	if (off >= at->len)
		return 0;
	if (size > at->len - off)
		size = (uint32_t)(at->len - off);
	if (ad_open(b, &rd))
		return -EIO;
	while (got < size) {
		r = ad_read_cb(&rd, (char *)buf + got, size - got, at->off + off + got);
		if (r <= 0)
			break;
		got += (uint32_t)r;
	}
	ad_close(b, &rd);
	if (r < 0 && !got)
		return r;
	b->st.ad_reads++;
	return (int)got;
}

/* ---------- named attributes (extended attributes) ---------- */

static int xa_err(int r)
{
	/* A file system may return ENOATTR / ENODATA for "no such attribute" */
#ifdef ENODATA
	if (r == -ENODATA)
		return -ENOATTR;
#endif
	return r;
}

/* Attributes treated as "present with an empty value" when missing: returns the virtual length (32 zero bytes for FinderInfo, 0 bytes for a regular file's resource fork), otherwise -1 */
static int virt_len(struct bf_ctx *b, const struct bf_node *n, const char *name)
{
	if (!b->namedattr)
		return -1;
	if (strcmp(name, BF_FINDERINFO) == 0)
		return 32;
	if (strcmp(name, BF_RSRCFORK) == 0 && n && !n->isdir)
		return 0;
	return -1;
}

static int fs_getxattr(struct bf_ctx *b, const char *path, const char *name, void *buf,
		       size_t size)
{
	if (!b->op->getxattr)
		return -ENOTSUP;
#if defined(__APPLE__)
	return xa_err(b->op->getxattr(path, name, buf, size, 0));
#else
	return xa_err(b->op->getxattr(path, name, buf, size));
#endif
}

static int fs_setxattr(struct bf_ctx *b, const char *path, const char *name,
		       const void *val, size_t size, int flags)
{
	if (!b->op->setxattr)
		return -ENOTSUP;
#if defined(__APPLE__)
	return xa_err(b->op->setxattr(path, name, val, size, flags, 0));
#else
	return xa_err(b->op->setxattr(path, name, val, size, flags));
#endif
}

#define BF_XA_RMW_MAX	(64u << 20)	/* limit on whole-value read-modify-write when there are no extension callbacks */

/* Read the whole value into a malloc'd buffer (*out; returns the length); returns -ENOATTR if it does not exist */
static int fs_getall(struct bf_ctx *b, const char *path, const char *name, char **out)
{
	int n = fs_getxattr(b, path, name, NULL, 0), m;

	*out = NULL;
	if (n < 0)
		return n;
	if ((unsigned)n > BF_XA_RMW_MAX)
		return -E2BIG;
	*out = malloc(n ? (size_t)n : 1);
	if (!*out)
		return -ENOMEM;
	m = n ? fs_getxattr(b, path, name, *out, (size_t)n) : 0;
	if (m < 0) {
		free(*out);
		*out = NULL;
		return m;
	}
	return m;
}

static int fs_xread(struct bf_ctx *b, const char *path, const char *name, void *buf,
		    uint32_t size, uint64_t off)
{
	char *all;
	int n;

	if (b->ext.xattr_read)
		return xa_err(b->ext.xattr_read(path, name, buf, size, off));
	n = fs_getall(b, path, name, &all);
	if (n < 0)
		return n;
	b->st.xa_rmw++;
	if (off >= (uint64_t)n) {
		n = 0;
	} else {
		if ((uint64_t)n - off < size)
			size = (uint32_t)((uint64_t)n - off);
		memcpy(buf, all + off, size);
		n = (int)size;
	}
	free(all);
	return n;
}

static int fs_xwrite(struct bf_ctx *b, const char *path, const char *name,
		     const void *buf, uint32_t size, uint64_t off)
{
	char *all = NULL, *nb;
	int n, r;
	uint64_t nl;

	if (b->ext.xattr_write)
		return xa_err(b->ext.xattr_write(path, name, buf, size, off));
	n = fs_getall(b, path, name, &all);
	if (n == -ENOATTR)
		n = 0;
	else if (n < 0)
		return n;
	b->st.xa_rmw++;
	nl = off + size > (uint64_t)n ? off + size : (uint64_t)n;
	if (nl > BF_XA_RMW_MAX) {
		free(all);
		return -E2BIG;
	}
	nb = calloc(1, nl ? (size_t)nl : 1);
	if (!nb) {
		free(all);
		return -ENOMEM;
	}
	if (n)
		memcpy(nb, all, (size_t)n);
	if (size)
		memcpy(nb + off, buf, size);
	free(all);
	r = fs_setxattr(b, path, name, nb, (size_t)nl, 0);
	free(nb);
	return r < 0 ? r : (int)size;
}

static int fs_xtrunc(struct bf_ctx *b, const char *path, const char *name, uint64_t size)
{
	char *all = NULL, *nb;
	int n, r;

	if (b->ext.xattr_truncate)
		return xa_err(b->ext.xattr_truncate(path, name, size));
	n = fs_getall(b, path, name, &all);
	if (n < 0)
		return n;
	b->st.xa_rmw++;
	if (size > BF_XA_RMW_MAX) {
		free(all);
		return -E2BIG;
	}
	nb = calloc(1, size ? (size_t)size : 1);
	if (!nb) {
		free(all);
		return -ENOMEM;
	}
	memcpy(nb, all, (size_t)n < size ? (size_t)n : (size_t)size);
	free(all);
	r = fs_setxattr(b, path, name, nb, (size_t)size, 0);
	free(nb);
	return r;
}

/* Put the path of ino into path5 */
static int xpath(struct bf_ctx *b, uint64_t ino, struct bf_node **np)
{
	struct bf_node *n = node_find(b, ino);
	int r;

	if (!n)
		return -ESTALE;
	r = node_path(b, n, b->path5, BF_PATH_MAX);
	if (np)
		*np = n;
	return r < 0 ? r : 0;
}

/*
 * The attributes in X's (node n's) `._X` that still count: returns NULL if there is no `._X`, it is
 * not AppleDouble, or it has already been migrated
 * (the marker used when ad_clean=0). path4 = path of `._X`, path5 = path of X (the caller runs xpath first).
 * fs_has_marker < 0 means the marker has not been checked yet.
 */
static const struct openmacfuse_ad *ad_for(struct bf_ctx *b, struct bf_node *n, int fs_has_marker)
{
	struct openmacfuse_attr ada;

	if (!b->adtrans || !n || n->ino == b->root || !n->name)
		return NULL;
	if (ad_path(b, n->parent, n->name, &ada))
		return NULL;
	if (!b->ad_clean) {
		if (fs_has_marker < 0)
			fs_has_marker = fs_getxattr(b, b->path5, BF_AD_MARKER, NULL, 0) >= 0;
		if (fs_has_marker)
			return NULL;
	}
	return ad_get(b, &ada);
}

/*
 * Before X's attributes are modified: move every attribute in `._X` that the file system does not
 * have yet into it, then delete `._X`
 * (ad_clean=0: keep it and record the "migrated" marker). path5 = path of X (the caller runs xpath first).
 */
static int ad_migrate(struct bf_ctx *b, struct bf_node *n)
{
	const struct openmacfuse_ad *ad0 = ad_for(b, n, -1);
	struct openmacfuse_ad ad;
	struct openmacfuse_attr ada;
	uint64_t adino, parent;
	char adn[OPENMACFUSE_KEY_MAX + 2], xn[OPENMACFUSE_KEY_MAX], *buf;
	uint32_t i;
	int r = 0;

	if (!ad0)
		return 0;
	/* Take a copy (the cache slot may be evicted during the migration) */
	memset(&ad, 0, sizeof(ad));
	ad.a = calloc(ad0->n ? ad0->n : 1, sizeof(*ad.a));
	if (!ad.a)
		return -ENOMEM;
	for (i = 0; i < ad0->n; i++) {
		ad.a[i] = ad0->a[i];
		ad.a[i].name = strdup(ad0->a[i].name);
		if (!ad.a[i].name) {
			openmacfuse_ad_free(&ad);
			return -ENOMEM;
		}
		ad.n = i + 1;
	}
	buf = malloc(256 * 1024);
	if (!buf) {
		openmacfuse_ad_free(&ad);
		return -ENOMEM;
	}
	for (i = 0; i < ad.n && r >= 0; i++) {
		uint64_t off = 0;

		if (fs_getxattr(b, b->path5, ad.a[i].name, NULL, 0) >= 0)
			continue;	/* the file system already has a (newer) value */
		if (ad.a[i].len == 0)
			r = fs_xwrite(b, b->path5, ad.a[i].name, NULL, 0, 0);
		while (r >= 0 && off < ad.a[i].len) {
			int got = ad_read_val(b, &ad.a[i], buf, 256 * 1024, off);
			if (got <= 0) {
				r = got < 0 ? got : -EIO;
				break;
			}
			r = fs_xwrite(b, b->path5, ad.a[i].name, buf, (uint32_t)got, off);
			off += (uint64_t)got;
		}
		if (r >= 0)
			b->st.ad_migrated_attrs++;
	}
	free(buf);
	openmacfuse_ad_free(&ad);
	if (r < 0) {
		if (b->log)
			b->log("migrating the `._` attributes of %s failed: errno %d (`._` kept, will retry next time)", b->path5, -r);
		return r;
	}
	b->st.ad_migrated++;
	parent = n->parent;
	snprintf(xn, sizeof(xn), "%s", n->name);
	snprintf(adn, sizeof(adn), "._%s", xn);
	if (b->ad_clean) {
		if (ad_path(b, parent, xn, &ada) == 0) {
			adino = ada.ino;
			if (do_remove(b, parent, adn, &ada) == 0) {
				b->st.ad_deleted++;
				b->st.removes--;
				ad_forget(b, adino);
			}
		}
	} else {
		fs_xwrite(b, b->path5, BF_AD_MARKER, NULL, 0, 0);
	}
	return 0;
}

static int list_has(const char *fl, int fn, const char *name)
{
	const char *p;

	for (p = fl; fl && p < fl + fn; p += strlen(p) + 1)
		if (strcmp(p, name) == 0)
			return 1;
	return 0;
}

static int bf_listxattr(void *ctx, openmacfuse_ino_t ino, char *buf, size_t size)
{
	struct bf_ctx *b = ctx;
	struct bf_node *n;
	const struct openmacfuse_ad *ad;
	char *fl = NULL, *p;
	size_t total = 0, o = 0;
	int fn = 0, r, marker;
	uint32_t i;

	r = xpath(b, ino, &n);
	if (r)
		return r;
	b->st.xa_lists++;
	if (b->op->listxattr) {
		fn = b->op->listxattr(b->path5, NULL, 0);
		if (fn > 0) {
			fl = malloc((size_t)fn);
			if (!fl)
				return -ENOMEM;
			fn = b->op->listxattr(b->path5, fl, (size_t)fn);
		}
		if (fn < 0) {
			free(fl);
			fl = NULL;
			if (fn != -ENOTSUP && fn != -EOPNOTSUPP)
				return fn;
			fn = 0;
		}
	}
	marker = list_has(fl, fn, BF_AD_MARKER);
	for (p = fl; fl && p < fl + fn; p += strlen(p) + 1)
		if (*p && strcmp(p, BF_AD_MARKER) != 0)
			total += strlen(p) + 1;
	if (b->fake_xattr)
		total += sizeof(BF_PROBE_NAME);
	ad = ad_for(b, n, marker);
	if (ad) {
		for (i = 0; i < ad->n; i++)
			if (!list_has(fl, fn, ad->a[i].name))
				total += strlen(ad->a[i].name) + 1;
		b->st.ad_lists++;
	}
	if (!size || total > size) {
		free(fl);
		return size ? -ERANGE : (int)total;
	}
	for (p = fl; fl && p < fl + fn; p += strlen(p) + 1) {
		if (!*p || strcmp(p, BF_AD_MARKER) == 0)
			continue;
		memcpy(buf + o, p, strlen(p) + 1);
		o += strlen(p) + 1;
	}
	if (ad)
		for (i = 0; i < ad->n; i++)
			if (!list_has(fl, fn, ad->a[i].name)) {
				memcpy(buf + o, ad->a[i].name, strlen(ad->a[i].name) + 1);
				o += strlen(ad->a[i].name) + 1;
			}
	if (b->fake_xattr) {
		memcpy(buf + o, BF_PROBE_NAME, sizeof(BF_PROBE_NAME));
		o += sizeof(BF_PROBE_NAME);
	}
	free(fl);
	return (int)o;
}

static int bf_getxattr(void *ctx, openmacfuse_ino_t ino, const char *name, void *buf,
		       size_t size)
{
	struct bf_ctx *b = ctx;
	struct bf_node *n;
	const struct openmacfuse_ad *ad;
	const struct openmacfuse_ad_attr *at;
	int r;

	if (b->fake_xattr && strcmp(name, BF_PROBE_NAME) == 0) {
		size_t l = strlen(BF_PROBE_VALUE);
		if (size && size < l)
			return -ERANGE;
		if (size)
			memcpy(buf, BF_PROBE_VALUE, l);
		return (int)l;
	}
	if (strcmp(name, BF_AD_MARKER) == 0)
		return -ENOATTR;
	r = xpath(b, ino, &n);
	if (r)
		return r;
	r = fs_getxattr(b, b->path5, name, buf, size);
	if (r != -ENOATTR && r != -ENOTSUP && r != -EOPNOTSUPP)
		return r;
	ad = ad_for(b, n, -1);
	at = ad ? openmacfuse_ad_find(ad, name) : NULL;
	if (!at) {
		int vl = virt_len(b, n, name);

		if (vl < 0)
			return -ENOATTR;
		if (size && size < (size_t)vl)
			return -ERANGE;
		if (size && vl)
			memset(buf, 0, (size_t)vl);
		b->st.fi_virtual++;
		return vl;
	}
	if (!size)
		return (int)at->len;
	if (size < at->len)
		return -ERANGE;
	return ad_read_val(b, at, buf, (uint32_t)at->len, 0);
}

static int bf_xattr_read(void *ctx, openmacfuse_ino_t ino, const char *name, void *buf,
			 uint32_t size, uint64_t off)
{
	struct bf_ctx *b = ctx;
	struct bf_node *n;
	const struct openmacfuse_ad *ad;
	const struct openmacfuse_ad_attr *at;
	int r;

	if (b->fake_xattr && strcmp(name, BF_PROBE_NAME) == 0) {
		size_t l = strlen(BF_PROBE_VALUE);
		if (off >= l)
			return 0;
		if (size > l - off)
			size = (uint32_t)(l - off);
		memcpy(buf, &BF_PROBE_VALUE[off], size);
		return (int)size;
	}
	if (strcmp(name, BF_AD_MARKER) == 0)
		return -ENOATTR;
	r = xpath(b, ino, &n);
	if (r)
		return r;
	r = fs_xread(b, b->path5, name, buf, size, off);
	if (r >= 0) {
		b->st.xa_reads++;
		b->st.xa_read_bytes += (uint64_t)r;
		return r;
	}
	if (r != -ENOATTR && r != -ENOTSUP && r != -EOPNOTSUPP)
		return r;
	ad = ad_for(b, n, -1);
	at = ad ? openmacfuse_ad_find(ad, name) : NULL;
	if (!at) {
		int vl = virt_len(b, n, name);

		if (vl < 0)
			return -ENOATTR;
		if (off >= (uint64_t)vl)
			return 0;
		if (size > (uint64_t)vl - off)
			size = (uint32_t)((uint64_t)vl - off);
		memset(buf, 0, size);
		return (int)size;
	}
	return ad_read_val(b, at, buf, size, off);
}

/* Common preamble for modifying attributes: path into path5, migrate `._X` first */
static int xa_prep(struct bf_ctx *b, openmacfuse_ino_t ino, const char *name)
{
	struct bf_node *n;
	int r;

	if (strcmp(name, BF_AD_MARKER) == 0)
		return -EPERM;
	r = xpath(b, ino, &n);
	if (r)
		return r;
	r = ad_migrate(b, n);
	if (r)
		return r;
	return xpath(b, ino, NULL);
}

static void xa_changed(struct bf_ctx *b, openmacfuse_ino_t ino)
{
	struct bf_node *n = node_find(b, ino);

	attr_drop(b, ino);	/* modifying attributes touches ctime */
	if (n)
		n->gen++;
}

static int bf_xattr_write(void *ctx, openmacfuse_ino_t ino, const char *name, const void *buf,
			  uint32_t size, uint64_t off)
{
	struct bf_ctx *b = ctx;
	int r = xa_prep(b, ino, name);

	if (r)
		return r;
	r = fs_xwrite(b, b->path5, name, buf, size, off);
	if (r >= 0) {
		b->st.xa_writes++;
		b->st.xa_write_bytes += size;
		xa_changed(b, ino);
	}
	return r;
}

static int bf_xattr_truncate(void *ctx, openmacfuse_ino_t ino, const char *name, uint64_t size)
{
	struct bf_ctx *b = ctx;
	int r = xa_prep(b, ino, name);

	if (r)
		return r;
	r = fs_xtrunc(b, b->path5, name, size);
	if (r == -ENOATTR && size == 0 && virt_len(b, node_find(b, ino), name) >= 0)
		r = 0;		/* truncating an attribute that is "present with an empty value": nothing to do */
	if (r >= 0) {
		b->st.xa_truncs++;
		xa_changed(b, ino);
	}
	return r < 0 ? r : 0;
}

static int bf_removexattr(void *ctx, openmacfuse_ino_t ino, const char *name)
{
	struct bf_ctx *b = ctx;
	int r = xa_prep(b, ino, name);

	if (r)
		return r;
	if (!b->op->removexattr)
		return -ENOTSUP;
	r = xa_err(b->op->removexattr(b->path5, name));
	if (r == -ENOATTR && virt_len(b, node_find(b, ino), name) >= 0)
		r = 0;		/* a missing FinderInfo / resource fork reads as an "empty value", so removing it counts as success */
	if (r == 0) {
		b->st.xa_removes++;
		xa_changed(b, ino);
	}
	return r;
}

static const struct openmacfuse_backend_ops bf_ops = {
	.root		= bf_root,
	.begin		= bf_begin,
	.resolve	= bf_resolve,
	.ancestry	= bf_ancestry,
	.getattr	= bf_getattr,
	.lookup		= bf_lookup,
	.lookupp	= bf_lookupp,
	.readdir	= bf_readdir,
	.access		= bf_access,
	.open		= bf_open,
	.release	= bf_release,
	.read		= bf_read,
	.readlink	= bf_readlink,
	.statfs		= bf_statfs,
	.listxattr	= bf_listxattr,
	.getxattr	= bf_getxattr,
	.write		= bf_write,
	.fsync		= bf_fsync,
	.create		= bf_create,
	.mkdir		= bf_mkdir,
	.symlink	= bf_symlink,
	.mknod		= bf_mknod,
	.link		= bf_link,
	.remove		= bf_remove,
	.rename		= bf_rename,
	.setattr	= bf_setattr,
	.invalidate	= bf_invalidate,
	.xattr_read	= bf_xattr_read,
	.xattr_write	= bf_xattr_write,
	.xattr_truncate	= bf_xattr_truncate,
	.removexattr	= bf_removexattr,
};

int openmacfuse_backend_fuse_init(struct openmacfuse_backend *be,
			   const struct fuse_operations *op,
			   struct fuse_context *fctx,
			   const struct openmacfuse_bf_config *cfg)
{
	struct bf_ctx *b;
	struct bf_node *root;
	struct openmacfuse_attr a;
	size_t hs = 1024;
	int r;

	if (!op->getattr || !op->readdir)
		return -ENOSYS;
	b = calloc(1, sizeof(*b));
	if (!b)
		return -ENOMEM;
	b->op = op;
	b->fctx = fctx;
	b->max_nodes = cfg && cfg->max_nodes ? cfg->max_nodes : 32768;
	if (b->max_nodes < 4)
		b->max_nodes = 4;	/* tiny values for testing are allowed too (the root + the few in use) */
	b->attr_ttl_ns = (uint64_t)(cfg && cfg->attr_ttl_ms ? cfg->attr_ttl_ms : 1000) * 1000000ull;
	b->snap_max_bytes = cfg && cfg->snap_max_bytes ? cfg->snap_max_bytes : 16u << 20;
	b->log = cfg ? cfg->log : NULL;
	b->fake_xattr = cfg ? cfg->fake_xattr : 0;
	b->namedattr = cfg ? cfg->namedattr : 0;
	b->adtrans = cfg ? (cfg->adtrans || cfg->namedattr) : 0;
	b->ad_clean = cfg ? cfg->ad_clean : 1;
	b->ci = cfg ? cfg->case_insensitive : 0;
	b->nfc = cfg ? cfg->nfc : 0;
	b->raw_names = cfg ? cfg->raw_names : 0;
	if (cfg && cfg->ext)
		b->ext = *cfg->ext;
	b->didx_max = 16u << 20;
	b->salt = ((uint64_t)arc4random() << 32) | arc4random();
	while (hs < b->max_nodes && hs < (1u << 20))
		hs *= 2;
	b->ht = calloc(hs, sizeof(*b->ht));
	b->htmask = hs - 1;
	b->path1 = malloc(BF_PATH_MAX);
	b->path2 = malloc(BF_PATH_MAX);
	b->path3 = malloc(BF_PATH_MAX);
	b->path4 = malloc(BF_PATH_MAX);
	b->path5 = malloc(BF_PATH_MAX);
	b->acache = calloc(BF_ACACHE_SLOTS, sizeof(*b->acache));
	b->hints = calloc(BF_HINT_SLOTS, sizeof(*b->hints));
	if (!b->ht || !b->path1 || !b->path2 || !b->path3 || !b->path4 || !b->path5 ||
	    !b->acache || !b->hints) {
		r = -ENOMEM;
		goto err;
	}
	fctx->uid = getuid();
	fctx->gid = getgid();
	r = do_getattr(b, "/", OPENMACFUSE_WANT_BASIC, &a);
	if (r)
		goto err;
	if (!S_ISDIR(a.mode)) {
		r = -ENOTDIR;
		goto err;
	}
	root = calloc(1, sizeof(*root));
	if (!root) {
		r = -ENOMEM;
		goto err;
	}
	/*
	 * The file system provides no inode numbers (getattr's st_ino is 0, as with some exFAT and disk-image drivers): assign stable numbers by path,
	 * with 1 for the root directory (on such file systems every LOOKUP used to return an I/O error and directory listings were empty)
	 */
	if (a.ino == 0) {
		b->synth = openmacfuse_synth_new();
		if (!b->synth) {
			r = -ENOMEM;
			goto err;
		}
		a.ino = openmacfuse_synth_get(b->synth, "/");
		if (b->log)
			b->log("the file system provides no inode numbers (st_ino is 0): assigning inode numbers by path");
	}
	b->root = a.ino;
	root->ino = b->root;
	root->parent = b->root;
	root->isdir = 1;
	root->nopen = 1;	/* the root is always pinned */
	root->hnext = NULL;
	b->ht[mix64(root->ino) & b->htmask] = root;
	b->nnodes = 1;
	cache_attr(b, root, &a, OPENMACFUSE_WANT_BASIC);
	be->ops = &bf_ops;
	be->ctx = b;
	/* Read-only if the file system has no write callback; the ro mount option is applied separately by the caller */
	be->readonly = op->write == NULL;
	return 0;
err:
	openmacfuse_synth_free(b->synth);
	free(b->ht);
	free(b->path1);
	free(b->path2);
	free(b->path3);
	free(b->path4);
	free(b->path5);
	free(b->acache);
	free(b->hints);
	free(b);
	return r;
}

void openmacfuse_backend_fuse_free(struct openmacfuse_backend *be)
{
	struct bf_ctx *b = be->ctx;
	size_t i;

	if (!b)
		return;
	openmacfuse_synth_free(b->synth);
	b->synth = NULL;
	for (i = 0; i <= b->htmask; i++) {
		struct bf_node *n = b->ht[i], *nx;
		for (; n; n = nx) {
			nx = n->hnext;
			free(n->name);
			free(n->fchain);
			free(n);
		}
	}
	for (i = 0; i < BF_HINT_SLOTS; i++)
		free(b->hints[i].name);
	free(b->hints);
	free(b->path3);
	free(b->path4);
	free(b->path5);
	didx_drop_all(b);
	for (i = 0; i < BF_ADC_SLOTS; i++)
		openmacfuse_ad_free(&b->adc[i].ad);
	while (b->snaps) {
		struct bf_snap *s = b->snaps;
		b->snaps = s->next;
		snap_free(s);
	}
	free(b->ht);
	free(b->path1);
	free(b->path2);
	free(b->acache);
	free(b);
	be->ctx = NULL;
}

void openmacfuse_backend_fuse_stats(struct openmacfuse_backend *be, char *buf, size_t n)
{
	struct bf_ctx *b = be->ctx;

	if (!b) {
		snprintf(buf, n, "-");
		return;
	}
	snprintf(buf, n,
		 "nodes=%zu/%zu peak=%llu evict=%llu recover_ok=%llu recover_fail=%llu "
		 "snap_build=%llu snap_reuse=%llu snap_ents=%llu snap_bytes=%zu "
		 "getattr=%llu getxtimes=%llu attr_hit=%llu readdir_ents=%llu "
		 "opens=%llu reads=%llu read_bytes=%llu writes=%llu write_bytes=%llu "
		 "fsyncs=%llu creates=%llu removes=%llu renames=%llu links=%llu "
		 "setattrs=%llu hint_hits=%llu frozen=%llu stale_ino=%llu",
		 b->nnodes, b->max_nodes, (unsigned long long)b->st.nodes_peak,
		 (unsigned long long)b->st.evictions,
		 (unsigned long long)b->st.recover_ok,
		 (unsigned long long)b->st.recover_fail,
		 (unsigned long long)b->st.snap_builds,
		 (unsigned long long)b->st.snap_reuse,
		 (unsigned long long)b->st.snap_entries, b->snap_bytes,
		 (unsigned long long)b->st.getattr_calls,
		 (unsigned long long)b->st.getxtimes_calls,
		 (unsigned long long)b->st.attr_hits,
		 (unsigned long long)b->st.readdir_ents,
		 (unsigned long long)b->st.opens,
		 (unsigned long long)b->st.read_calls,
		 (unsigned long long)b->st.read_bytes,
		 (unsigned long long)b->st.writes,
		 (unsigned long long)b->st.write_bytes,
		 (unsigned long long)b->st.fsyncs,
		 (unsigned long long)b->st.creates,
		 (unsigned long long)b->st.removes,
		 (unsigned long long)b->st.renames,
		 (unsigned long long)b->st.links,
		 (unsigned long long)b->st.setattrs,
		 (unsigned long long)b->st.hint_hits,
		 (unsigned long long)b->st.frozen,
		 (unsigned long long)b->st.stale_ino);
}

void openmacfuse_backend_fuse_stats_m3(struct openmacfuse_backend *be, char *buf, size_t n)
{
	struct bf_ctx *b = be->ctx;

	if (!b) {
		snprintf(buf, n, "-");
		return;
	}
	snprintf(buf, n,
		 "case=%s nfc=%d xattr=%s adclean=%d | didx_build=%llu didx_ents=%llu "
		 "didx_bytes=%zu didx_drop=%llu fold_hit=%llu norm_hit=%llu ambig=%llu "
		 "exist_equiv=%llu conflict_dirs=%llu conflict_pairs=%llu nfc_names=%llu "
		 "case_renames=%llu | ad_hidden=%llu ad_parsed=%llu ad_reads=%llu "
		 "ad_lists=%llu ad_migrated=%llu ad_migrated_attrs=%llu ad_deleted=%llu "
		 "ad_follow_rm=%llu ad_follow_mv=%llu ad_bad=%llu | xa_lists=%llu "
		 "xa_reads=%llu xa_read_bytes=%llu xa_writes=%llu xa_write_bytes=%llu "
		 "xa_truncs=%llu xa_removes=%llu xa_rmw=%llu fi_virtual=%llu",
		 b->ci ? "insensitive" : "sensitive", b->nfc,
		 b->namedattr ? "streams" : b->adtrans ? "adfile" : "appledouble", b->ad_clean,
		 (unsigned long long)b->st.didx_builds,
		 (unsigned long long)b->st.didx_ents, b->didx_bytes,
		 (unsigned long long)b->st.didx_drops,
		 (unsigned long long)b->st.fold_hits,
		 (unsigned long long)b->st.norm_hits,
		 (unsigned long long)b->st.fold_ambig,
		 (unsigned long long)b->st.exist_equiv,
		 (unsigned long long)b->st.conflict_dirs,
		 (unsigned long long)b->st.conflict_pairs,
		 (unsigned long long)b->st.nfc_names,
		 (unsigned long long)b->st.case_renames,
		 (unsigned long long)b->st.ad_hidden,
		 (unsigned long long)b->st.ad_parsed,
		 (unsigned long long)b->st.ad_reads,
		 (unsigned long long)b->st.ad_lists,
		 (unsigned long long)b->st.ad_migrated,
		 (unsigned long long)b->st.ad_migrated_attrs,
		 (unsigned long long)b->st.ad_deleted,
		 (unsigned long long)b->st.ad_follow_rm,
		 (unsigned long long)b->st.ad_follow_mv,
		 (unsigned long long)b->st.ad_bad,
		 (unsigned long long)b->st.xa_lists,
		 (unsigned long long)b->st.xa_reads,
		 (unsigned long long)b->st.xa_read_bytes,
		 (unsigned long long)b->st.xa_writes,
		 (unsigned long long)b->st.xa_write_bytes,
		 (unsigned long long)b->st.xa_truncs,
		 (unsigned long long)b->st.xa_removes,
		 (unsigned long long)b->st.xa_rmw,
		 (unsigned long long)b->st.fi_virtual);
}

void openmacfuse_backend_fuse_set_case(struct openmacfuse_backend *be, int insensitive)
{
	struct bf_ctx *b = be->ctx;

	if (b && b->ci != (insensitive != 0)) {
		b->ci = insensitive != 0;
		didx_drop_all(b);
	}
}

/* ---------- Pre-mount case-conflict scan ---------- */

struct scan_ent {
	uint64_t	h;
	uint32_t	noff;
	uint16_t	nlen;
	uint16_t	isdir;
};

struct scan_dir {
	struct bf_ctx	*b;
	struct scan_ent	*e;
	uint32_t	n, cap;
	char		*names;
	size_t		nlen, ncap;
	int		oom;
};

static int scan_filler(void *buf, const char *name, const struct stat *st, off_t off)
{
	struct scan_dir *d = buf;
	char key[OPENMACFUSE_KEY_MAX];
	size_t l;
	int kl;

	(void)off;
	if (d->oom)
		return 1;
	if (!name || (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))))
		return 0;
	l = strlen(name);
	if (l == 0 || l > 1023)
		return 0;
	if (d->n == d->cap) {
		uint32_t nc = d->cap ? d->cap * 2 : 256;
		struct scan_ent *ne = realloc(d->e, (size_t)nc * sizeof(*ne));
		if (!ne) {
			d->oom = 1;
			return 1;
		}
		d->e = ne;
		d->cap = nc;
	}
	if (d->nlen + l + 1 > d->ncap) {
		size_t nc = d->ncap ? d->ncap * 2 : 8192;
		char *nn;
		while (nc < d->nlen + l + 1)
			nc *= 2;
		nn = realloc(d->names, nc);
		if (!nn) {
			d->oom = 1;
			return 1;
		}
		d->names = nn;
		d->ncap = nc;
	}
	kl = name_key(d->b, name, l, key);
	d->e[d->n].h = openmacfuse_hash_bytes(key, (size_t)kl);
	d->e[d->n].noff = (uint32_t)d->nlen;
	d->e[d->n].nlen = (uint16_t)l;
	d->e[d->n].isdir = st && S_ISDIR(st->st_mode);
	memcpy(d->names + d->nlen, name, l + 1);
	d->nlen += l + 1;
	d->n++;
	return 0;
}

static int scan_cmp(const void *a, const void *b2)
{
	const struct scan_ent *x = a, *y = b2;

	return x->h < y->h ? -1 : x->h > y->h;
}

int openmacfuse_backend_fuse_case_scan(struct openmacfuse_backend *be, uint32_t budget_ms,
				 struct openmacfuse_case_scan *out)
{
	struct bf_ctx *b = be->ctx;
	char **q = NULL;
	size_t qh = 0, qt = 0, qcap = 0;
	uint64_t t0 = now_ns(), limit = (uint64_t)budget_ms * 1000000ull;
	struct scan_dir d;
	int saved_ci, timed_out = 0;

	memset(out, 0, sizeof(*out));
	if (!b)
		return -EINVAL;
	saved_ci = b->ci;
	b->ci = 1;		/* look for conflicts using the case-insensitive key */
	memset(&d, 0, sizeof(d));
	d.b = b;
	qcap = 1024;
	q = malloc(qcap * sizeof(*q));
	if (!q)
		goto done;
	q[qt++] = strdup("/");
	while (qh < qt) {
		char *dir = q[qh++];
		struct fuse_file_info fi;
		uint32_t i, j;
		int r = 0;

		if (!dir)
			continue;
		if (now_ns() - t0 > limit) {
			timed_out = 1;
			free(dir);
			break;
		}
		d.n = 0;
		d.nlen = 0;
		d.oom = 0;
		memset(&fi, 0, sizeof(fi));
		fi.flags = O_RDONLY;
		if (b->op->opendir)
			r = b->op->opendir(dir, &fi);
		if (r == 0) {
			r = b->op->readdir(dir, &d, scan_filler, 0, &fi);
			if (b->op->releasedir)
				b->op->releasedir(dir, &fi);
		}
		if (r || d.oom) {
			free(dir);
			continue;
		}
		out->dirs++;
		out->entries += d.n;
		/* Sort entries with the same key (hash) together, then compare pairwise: same key but different NFC = case conflict; same NFC = duplicate form */
		qsort(d.e, d.n, sizeof(*d.e), scan_cmp);
		for (i = 0; i < d.n; i = j) {
			for (j = i + 1; j < d.n && d.e[j].h == d.e[i].h; j++) {
				const char *a = d.names + d.e[i].noff, *c = d.names + d.e[j].noff;
				char k1[OPENMACFUSE_KEY_MAX], k2[OPENMACFUSE_KEY_MAX];
				int l1 = name_key(b, a, d.e[i].nlen, k1);
				int l2 = name_key(b, c, d.e[j].nlen, k2);

				if (l1 != l2 || memcmp(k1, k2, (size_t)l1) != 0)
					continue;
				if (nfc_equal(a, d.e[i].nlen, c, d.e[j].nlen)) {
					out->normdups++;
				} else {
					if (!out->conflicts)
						snprintf(out->sample, sizeof(out->sample), "%s%s%s / %s",
							 dir, strcmp(dir, "/") ? "/" : "", a, c);
					out->conflicts++;
				}
			}
		}
		for (i = 0; i < d.n; i++) {
			size_t dl = strlen(dir);
			char *sub;

			if (!d.e[i].isdir)
				continue;
			if (qt == qcap) {
				char **nq;
				if (qcap >= (1u << 20))
					break;		/* queue limit: the rest counts as not scanned */
				nq = realloc(q, qcap * 2 * sizeof(*q));
				if (!nq)
					break;
				q = nq;
				qcap *= 2;
			}
			sub = malloc(dl + 1 + d.e[i].nlen + 1);
			if (!sub)
				break;
			sprintf(sub, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "",
				d.names + d.e[i].noff);
			q[qt++] = sub;
		}
		free(dir);
	}
done:
	out->complete = !timed_out && q && qh >= qt;
	while (q && qh < qt)
		free(q[qh++]);
	free(q);
	free(d.e);
	free(d.names);
	b->ci = saved_ci;
	out->ms = (double)(now_ns() - t0) / 1e6;
	return 0;
}
