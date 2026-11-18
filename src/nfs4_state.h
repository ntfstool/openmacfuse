/*
 * nfs4_state.h - global server state: file handles, client IDs, open state, locks,
 *                seqid replay cache, duplicate request cache (DRC)
 *
 * There is only ever one client, on the local machine (the system NFS client), so everything uses
 * small tables with linear search, but the interfaces are designed as for a full implementation:
 * switching to hash tables later will not affect callers.
 *
 * Two "boot ids":
 *   boot        goes into file handles. After the process restarts, every old handle gets
 *               NFS4ERR_STALE.
 *   state_boot  goes into stateids / clientids. After the process restarts, old stateids get
 *               NFS4ERR_STALE_STATEID and old clientids get NFS4ERR_STALE_CLIENTID, which sends the
 *               client through recovery (a new SETCLIENTID; CLAIM_PREVIOUS gets NO_GRACE).
 *   Normally both are generated together; the "simulate server restart" test hook replaces only
 *   state_boot (handles stay valid), to verify that a client presenting old state gets the proper
 *   error codes and does not hang.
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#ifndef OPENMACFUSE_NFS4_STATE_H
#define OPENMACFUSE_NFS4_STATE_H

#include <pthread.h>
#include <stdint.h>
#include <time.h>

#include "backend.h"
#include "nfs4_prot.h"

/* ---------- File handles ----------
 * Variable length, at most 128 bytes (NFS4_FHSIZE), big-endian:
 *   kind(1) nanc(1) reserved(2) boot(4) ino(8)
 *   [ ancestor inode numbers x nanc (8 bytes each, nearest first) ]
 *   For NAMEDATTR, ino is followed by the attribute name id (8 bytes) and then the ancestor chain
 *   (at most 13 entries).
 *   For ATTRDIR / NAMEDATTR, ino is the owning file and the ancestor chain is the owning file's
 *   (so it can still be found after its node has been evicted).
 * boot is a random number chosen at each start: after the process restarts every old handle gets
 * NFS4ERR_STALE, and other processes cannot guess a valid handle.
 * ino is the stable identity (under FUSE use_ino, the file system's own inode number). The
 * ancestor chain is only a hint for "finding it again" (after the node table evicts an entry,
 * PUTFH walks the chain level by level to find it; see backend_fuse.c).
 * The handle bytes handed out for a given object stay the same for the lifetime of the process
 * (also across renames).
 */
#define OPENMACFUSE_FH_MAXANC		14
#define OPENMACFUSE_FH_MAXLEN		(16 + 8 * OPENMACFUSE_FH_MAXANC)	/* = 128 */
#define OPENMACFUSE_FH_NODE		1	/* ordinary node in the backend */
#define OPENMACFUSE_FH_PSEUDOROOT	2	/* pseudo root: contains a single subdirectory (the export name) */
#define OPENMACFUSE_FH_ATTRDIR	3	/* named attribute directory: ino is the owning file */
#define OPENMACFUSE_FH_NAMEDATTR	4	/* one named attribute: ino is the owning file */
/*
 * virtual `._X` (AppleDouble synthesis / parsing, see adfile.h): ino is X, the ancestor chain is
 * X's
 */
#define OPENMACFUSE_FH_ADFILE		5

struct openmacfuse_fh {
	uint8_t		kind;
	uint8_t		nanc;
	uint32_t	boot;
	openmacfuse_ino_t	ino;
	openmacfuse_ino_t	attr_id;			/* NAMEDATTR: attribute name id */
	/* ancestor chain (anc[0] is the parent directory); at most 13 for NAMEDATTR */
	openmacfuse_ino_t	anc[OPENMACFUSE_FH_MAXANC];
};

/* ---------- Client ID (SETCLIENTID) ---------- */
#define OPENMACFUSE_MAX_CLIENTS	8

struct openmacfuse_client {
	int		used;
	int		confirmed;
	uint64_t	clientid;
	uint8_t		verifier[NFS4_VERIFIER_SIZE];	/* client boot verifier */
	uint8_t		confirm[NFS4_VERIFIER_SIZE];	/* confirm verifier we issued */
	uint8_t		id[NFS4_OPAQUE_LIMIT];
	uint32_t	idlen;
	time_t		last_renew;
};

/* ---------- seqid and replay cache (RFC 7530, section 9.1.7) ----------
 * Each open-owner / lock-owner remembers its most recent seqid-carrying operation: sequence
 * number, opcode, result status and result body (without opcode and status). When the client
 * retransmits the same operation with the same seqid it is replayed verbatim (OPEN also restores
 * the current file handle); a seqid that is neither "the last one" nor "the last one + 1" gets
 * NFS4ERR_BAD_SEQID. The first request of a new owner is accepted with any seqid.
 * The following errors do not advance the seqid: STALE_CLIENTID, STALE_STATEID, BAD_STATEID,
 * BAD_SEQID, BADXDR, RESOURCE, NOFILEHANDLE, MOVED.
 */
/* result bodies larger than this are not cached (an OPEN result is about 100 bytes) */
#define OPENMACFUSE_REPLAY_MAX	512

struct openmacfuse_fh;

struct openmacfuse_seq {
	int		valid;
	uint32_t	seqid;
	uint32_t	op;
	uint32_t	status;
	uint32_t	len;
	uint8_t		*body;		/* malloc'd, at most OPENMACFUSE_REPLAY_MAX */
	int		have_fh;	/* OPEN: restore the current handle on replay */
	struct openmacfuse_fh	*fh;
	uint8_t		other[NFS4_OTHER_SIZE];	/* CLOSE: stateid.other from the request */
};

/* Returns: 0 process normally, 1 replay, -1 BAD_SEQID */
int  openmacfuse_seq_check(const struct openmacfuse_seq *q, uint32_t seqid, uint32_t op);
/* Whether this error code advances the seqid */
int  openmacfuse_seq_bumps(uint32_t status);
void openmacfuse_seq_record(struct openmacfuse_seq *q, uint32_t seqid, uint32_t op,
		      uint32_t status, const uint8_t *body, uint32_t len,
		      const struct openmacfuse_fh *fh);
void openmacfuse_seq_free(struct openmacfuse_seq *q);

/* ---------- open-owner ----------
 * The first OPEN of an open-owner requires an OPEN_CONFIRM from the client; once confirmed it is
 * not required again within the lease period (which is what the system client expects, and saves
 * a round trip).
 */
struct openmacfuse_owner {
	struct openmacfuse_owner *next;
	uint64_t	clientid;
	uint8_t		owner[NFS4_OPAQUE_LIMIT];
	uint32_t	ownerlen;
	int		confirmed;
	struct openmacfuse_seq seq;
	time_t		last_used;
};

/* ---------- Open state (the stateid of an OPEN) ---------- */
struct openmacfuse_stateid {
	uint32_t	seqid;
	uint8_t		other[NFS4_OTHER_SIZE];
};

struct openmacfuse_open {
	struct openmacfuse_open *next;
	struct openmacfuse_stateid sid;
	struct openmacfuse_owner *owner;
	openmacfuse_ino_t	ino;
	uint64_t	befh;		/* handle returned by the backend open */
	int		bewrite;	/* the backend handle was opened read-write */
	uint32_t	share_access;
	uint32_t	share_deny;
	uint8_t		kind;		/* OPENMACFUSE_FH_NODE or OPENMACFUSE_FH_NAMEDATTR */
	openmacfuse_ino_t	attr_id;	/* attribute name id for NAMEDATTR */
};

/* ---------- Byte-range locks (RFC 7530, sections 9.4 and 16.10-16.12) ----------
 * There is only one client (the local kernel), but it acts for many processes, one lock-owner per
 * process:
 * conflicts between different lock-owners are decided by the usual read / write lock rules. One
 * (lock-owner, file) pair has one lock state, i.e. one lock stateid. There is no grace period
 * (reclaim after a restart gets NO_GRACE).
 */
#define OPENMACFUSE_LT_READ	1
#define OPENMACFUSE_LT_WRITE	2

struct openmacfuse_lockowner {
	struct openmacfuse_lockowner *next;
	uint64_t	clientid;
	uint8_t		owner[NFS4_OPAQUE_LIMIT];
	uint32_t	ownerlen;
	struct openmacfuse_seq seq;
	time_t		last_used;
};

struct openmacfuse_lockrange {
	struct openmacfuse_lockrange *next;
	uint64_t	off;
	uint64_t	end;		/* exclusive; UINT64_MAX means to end of file */
	uint32_t	type;		/* OPENMACFUSE_LT_READ / OPENMACFUSE_LT_WRITE */
};

/*
 * Lifetime of a lock state: it follows the lock-owner, not the open state.
 * When the open state is CLOSEd the lock state is merely "detached" (open = NULL, the remaining
 * lock ranges are dropped as POSIX close semantics require) and its stateid stays valid -- the
 * macOS 12 client has been observed not to send RELEASE_LOCKOWNER, and the next time it opens the
 * same file it presents the old lock stateid via exist_lock_owner; answering BAD_STATEID would
 * send it into recovery (CLAIM_PREVIOUS / reopen) and the application would see EIO.
 * A detached lock state that is used again is attached to the file's current open state; ones
 * left unused for a long time are reclaimed.
 */
struct openmacfuse_lock {
	struct openmacfuse_lock	*next;
	struct openmacfuse_stateid	sid;
	struct openmacfuse_lockowner	*lo;
	struct openmacfuse_open	*open;	/* open state it derives from; NULL = detached (the open state was closed) */
	time_t			detached_at;
	openmacfuse_ino_t		ino;
	struct openmacfuse_lockrange	*ranges;
};

/* ---------- Duplicate request cache (DRC) ----------
 * After a disconnect and reconnect the client retransmits, with the same xid, any request it has
 * not received a reply for. For a COMPOUND containing non-idempotent operations (OPEN / CLOSE /
 * CREATE / REMOVE / RENAME / LINK / SETATTR / LOCK ...)
 * the whole RPC reply is cached under (xid, digest of the request body) and returned verbatim on
 * retransmission, instead of executing it a second time. The cache is bounded in entry count and
 * total bytes, with least-recently-used eviction.
 * There is only one local client, so entries are not keyed by client address (address / port
 * change after a reconnect anyway); the content digest guards against xid collisions after a
 * client restart.
 */
#define OPENMACFUSE_DRC_MAX_ENTS	1024
#define OPENMACFUSE_DRC_MAX_BYTES	(1u << 20)
#define OPENMACFUSE_DRC_MAX_REPLY	4096	/* replies larger than this are not cached */

struct openmacfuse_drc_ent;

struct openmacfuse_drc {
	struct openmacfuse_drc_ent	*ht[512];
	struct openmacfuse_drc_ent	*head, *tail;	/* LRU: newest at head, oldest at tail */
	uint32_t		n;
	size_t			bytes;
	uint64_t		hits;
	uint64_t		inserts;
};

const uint8_t *openmacfuse_drc_lookup(struct openmacfuse_drc *c, uint32_t xid, uint64_t sum,
				uint32_t *len);
void openmacfuse_drc_insert(struct openmacfuse_drc *c, uint32_t xid, uint64_t sum,
		      const uint8_t *reply, uint32_t len);
void openmacfuse_drc_free(struct openmacfuse_drc *c);
uint64_t openmacfuse_drc_sum(const uint8_t *msg, size_t len);

/* ---------- Server ---------- */
struct openmacfuse_server {
	struct openmacfuse_backend	be;
	/* mount path is /<name>; an empty string means mount / directly */
	char			export_name[256];
	uint32_t		boot;			/* boot id in file handles */
	uint32_t		state_boot;		/* boot id in stateids / clientids */
	/*
	 * state_boot values used earlier by this process (to recognize old clientids after a
	 * simulated restart)
	 */
	uint32_t		prev_boots[4];
	uint8_t			writeverf[NFS4_VERIFIER_SIZE];	/* write verifier for WRITE / COMMIT */
	uint64_t		fsid_major;
	uint64_t		fsid_minor;
	uint32_t		lease_time;
	uint32_t		rwsize;			/* maxread / maxwrite */
	int			verbose;
	/* named attribute channel: advertise and implement OPENATTR (on by default) */
	int			named_attr;
	int			case_insensitive;	/* advertise case_insensitive to the client */
	int			adfile;			/* virtual `._X` (openmacfuse_xattr=adfile, the default) */
	struct openmacfuse_adimg	*adimgs;		/* write sessions of virtual `._X` files */
	void			*adclean;		/* clean image cache for virtual `._X` files */
	size_t			adf_pre_bytes;
	size_t			adf_full_bytes;
	uint64_t		adf_lookups, adf_virtual, adf_builds, adf_sessions, adf_creates;
	uint64_t		adf_reads, adf_read_bytes, adf_writes, adf_write_bytes, adf_applies;
	uint64_t		adf_attr_writes, adf_attr_same, adf_attr_removes, adf_removes;
	uint64_t		adf_renames, adf_invalid;
	/* names just renamed away -> file (see openmacfuse_adf_renamed in adfile.h) */
	void			*adalias;
	uint64_t		adf_alias_adds, adf_alias_hits, adf_alias_removes;
	int			no_drc;			/* for tests: disable the DRC */
	void			(*log)(const char *fmt, ...);	/* optional: log output */

	pthread_mutex_t		lock;		/* all COMPOUNDs execute serially */
	struct openmacfuse_client	clients[OPENMACFUSE_MAX_CLIENTS];
	uint32_t		next_clientid;
	struct openmacfuse_owner	*owners;
	struct openmacfuse_open	*opens;
	struct openmacfuse_lockowner	*lockowners;
	struct openmacfuse_lock	*locks;
	uint64_t		next_other;
	uint32_t		nowners;
	uint32_t		nlockowners;
	struct openmacfuse_drc	drc;

	/* Test hooks (only active when OPENMACFUSE_TEST_* environment variables are set) */
	/* replies still to drop per op; [NFS4_OP_MAX+1] = OPEN(EXCLUSIVE4) */
	uint32_t		test_drop[NFS4_OP_MAX + 2];
	/*
	 * also close the connection after dropping a reply (forces the client to reconnect and
	 * retransmit)
	 */
	int			test_drop_close;
	/* simulate a server restart before the next COMPOUND (replace state_boot) */
	volatile int		test_reboot;

	/*
	 * The underlying device is gone (openmacfuse_device_lost): requests get NFS4ERR_IO immediately
	 * without entering the file system
	 */
	volatile int		dev_lost;
	uint64_t		lost_replies;		/* number of ops answered with IO because of that */

	/*
	 * Watchdog: the op currently executing (0 = none) and its start time (openmacfuse_uptime_ns);
	 * hung = declared stuck
	 */
	volatile uint32_t	cur_op;
	volatile uint64_t	cur_op_t0;
	volatile int		hung;
	/*
	 * Watchdog test hooks: OPENMACFUSE_TEST_HANG=<op>[:nth], OPENMACFUSE_TEST_HANG_MS,
	 * OPENMACFUSE_TEST_HANG_PROGRESS, OPENMACFUSE_TEST_STALL
	 */
	uint32_t		test_hang_op;
	uint32_t		test_hang_skip;		/* execute normally this many times first */
	uint32_t		test_hang_ms;		/* 0 = never return */
	/*
	 * pretend the backend keeps making progress while hung (verifies that a slow but moving
	 * op is not killed by mistake)
	 */
	int			test_hang_progress;
	volatile uint64_t	test_progress;
	const char		*test_stall_path;	/* service threads stall while this file exists */
	/*
	 * Local-host security (TCP): requests rejected on untrusted connections, and times a
	 * connection became trusted by presenting a valid handle
	 */
	uint64_t		gate_denied;
	uint64_t		gate_proved;
	/*
	 * Total time spent processing COMPOUNDs / longest single one / count of those >= 1 second
	 * (lock hold time)
	 */
	uint64_t		busy_ns;
	uint64_t		busy_max_ns;
	uint64_t		busy_slow;

	/*
	 * Statistics (to be hooked up later to the file system's own statistics channel /
	 * logging)
	 */
	uint64_t		op_count[NFS4_OP_MAX + 1];
	uint64_t		op_err[NFS4_OP_MAX + 1];
	uint64_t		compounds;
	uint64_t		stale_fh;		/* times PUTFH returned STALE */
	uint64_t		readdir_notsame;	/* times a cookieverf was no longer valid */
	uint64_t		seq_replays;		/* seqid replays */
	uint64_t		bad_seqid;
	uint64_t		test_dropped;
	uint64_t		reboots;
	uint64_t		resurrected;		/* times a pre-restart clientid was accepted */
	/* times NO_GRACE was returned (client tried to reclaim old state after a restart) */
	uint64_t		no_grace;
	uint64_t		write_bytes;
	uint64_t		stable_writes;
	uint64_t		commits;

	/*
	 * Named attributes: attribute name table (id -> name; handles carry the id), grow-only
	 * and bounded; name -> id goes through a hash
	 */
	char			**attr_names;
	uint32_t		nattr_names;
	uint32_t		attr_names_cap;
	uint32_t		*attr_ht;		/* id+1, open addressing */
	uint32_t		attr_htcap;
	uint64_t		namedattr_ops;		/* operations on named attribute objects (statistics) */
	/*
	 * times the server truncated on the client's behalf when an existing attribute was opened
	 * for write (see op_open)
	 */
	uint64_t		xattr_trunc_on_write;
};

int  openmacfuse_server_init(struct openmacfuse_server *s, const char *export_name);
/*
 * Simulate / handle a server restart: drop all state (releasing backend open handles) and replace
 * state_boot
 */
void openmacfuse_state_reboot(struct openmacfuse_server *s);
/* Free all state (at exit) */
void openmacfuse_state_free(struct openmacfuse_server *s);

/* File handle encoding / decoding; encode returns the length */
uint32_t openmacfuse_fh_encode(const struct openmacfuse_fh *fh, uint8_t out[OPENMACFUSE_FH_MAXLEN]);
/* Returns NFS4_OK / NFS4ERR_BADHANDLE / NFS4ERR_STALE */
uint32_t openmacfuse_fh_decode(const struct openmacfuse_server *s, const uint8_t *p,
			uint32_t len, struct openmacfuse_fh *fh);

/* Client ID */
uint32_t openmacfuse_setclientid(struct openmacfuse_server *s, const uint8_t verf[8],
			  const uint8_t *id, uint32_t idlen,
			  uint64_t *clientid, uint8_t confirm[8]);
uint32_t openmacfuse_setclientid_confirm(struct openmacfuse_server *s, uint64_t clientid,
				  const uint8_t confirm[8]);
uint32_t openmacfuse_renew(struct openmacfuse_server *s, uint64_t clientid);
/* Check that the clientid carried by OPEN and similar ops is valid */
uint32_t openmacfuse_check_clientid(struct openmacfuse_server *s, uint64_t clientid);

/* open-owner: if not found and create is non-zero, create one */
struct openmacfuse_owner *openmacfuse_owner_get(struct openmacfuse_server *s, uint64_t clientid,
				  const uint8_t *owner, uint32_t ownerlen,
				  int create);
/*
 * Reclaim owners / lock-owners that have had no open state and gone unused for a long time
 * (bounded, so the tables do not only grow)
 */
void openmacfuse_owners_sweep(struct openmacfuse_server *s);

/* Open state */
struct openmacfuse_open *openmacfuse_open_new(struct openmacfuse_server *s, struct openmacfuse_owner *ow,
				openmacfuse_ino_t ino, uint64_t befh, uint32_t access,
				uint32_t deny);
/* Find an open state by stateid.other (ignoring seqid): NFS4_OK / STALE_STATEID / BAD_STATEID */
uint32_t openmacfuse_open_find(struct openmacfuse_server *s, const struct openmacfuse_stateid *sid,
			 struct openmacfuse_open **out);
/* Then check the stateid's seqid: NFS4_OK / OLD_STATEID / BAD_STATEID */
uint32_t openmacfuse_stateid_check_seq(const struct openmacfuse_stateid *have,
				 const struct openmacfuse_stateid *want);
/* Look up by stateid (find + seqid check) */
uint32_t openmacfuse_open_lookup(struct openmacfuse_server *s, const struct openmacfuse_stateid *sid,
			  struct openmacfuse_open **out);
void openmacfuse_open_free(struct openmacfuse_server *s, struct openmacfuse_open *op);
/* Special stateids: all zeros (anonymous) or all ones (bypass) */
int  openmacfuse_stateid_is_special(const struct openmacfuse_stateid *sid);

/* Locks */
struct openmacfuse_lockowner *openmacfuse_lockowner_get(struct openmacfuse_server *s,
					  uint64_t clientid,
					  const uint8_t *owner,
					  uint32_t ownerlen, int create);
/*
 * Lock object key: for a regular file it is the inode number; a named attribute (e.g. the
 * resource fork, which the Carbon Resource Manager locks) uses the same synthetic number as the
 * named attribute's fileid, so its locks do not interfere with those of the owning file.
 */
static inline openmacfuse_ino_t openmacfuse_lock_key(uint8_t kind, openmacfuse_ino_t ino, openmacfuse_ino_t attr_id)
{
	if (kind == OPENMACFUSE_FH_ADFILE)
		return 0x5000000000000000ull | (ino & 0x0fffffffffffffffull);
	return kind == OPENMACFUSE_FH_NAMEDATTR ?
	       (0x6000000000000000ull | ((uint64_t)attr_id << 40) | (ino & 0xffffffffffull)) : ino;
}

struct openmacfuse_lock *openmacfuse_lock_get(struct openmacfuse_server *s, struct openmacfuse_lockowner *lo,
				struct openmacfuse_open *op, openmacfuse_ino_t ino);
/* Find by lock stateid.other (ignoring seqid) */
uint32_t openmacfuse_lock_find(struct openmacfuse_server *s, const struct openmacfuse_stateid *sid,
			 struct openmacfuse_lock **out);
/* Look up by lock stateid; return values as for openmacfuse_open_lookup */
uint32_t openmacfuse_lock_lookup(struct openmacfuse_server *s, const struct openmacfuse_stateid *sid,
			  struct openmacfuse_lock **out);
/*
 * Find another owner's lock range conflicting with [off,end); NULL if none, *holder is the holder
 */
struct openmacfuse_lockrange *openmacfuse_lock_conflict(struct openmacfuse_server *s, openmacfuse_ino_t ino,
					  struct openmacfuse_lockowner *me, uint64_t off,
					  uint64_t end, uint32_t type,
					  struct openmacfuse_lockowner **holder);
/*
 * Lock (overlapping parts already held by the same owner take the new type) / unlock a range; 0
 * on success, -1 out of memory
 */
int  openmacfuse_lock_add(struct openmacfuse_lock *lk, uint64_t off, uint64_t end, uint32_t type);
int  openmacfuse_lock_remove(struct openmacfuse_lock *lk, uint64_t off, uint64_t end);
/*
 * An open state is about to be closed: detach the lock states derived from it (drop the lock
 * ranges, keep the stateid)
 */
void openmacfuse_lock_detach_open(struct openmacfuse_server *s, struct openmacfuse_open *op);
/*
 * A detached lock state is used again: attach it to one of the file's current open states; NULL
 * if there is none
 */
struct openmacfuse_open *openmacfuse_lock_attach(struct openmacfuse_server *s, struct openmacfuse_lock *lk);
/* RELEASE_LOCKOWNER: returns NFS4ERR_LOCKS_HELD while lock ranges are still held */
uint32_t openmacfuse_lockowner_release(struct openmacfuse_server *s, uint64_t clientid,
				const uint8_t *owner, uint32_t ownerlen);

#endif /* OPENMACFUSE_NFS4_STATE_H */
