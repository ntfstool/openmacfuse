/*
 * backend.h - the "backend" interface below the NFS server
 *
 * The server (rpc / xdr / state / operation dispatch) knows only this set of callbacks,
 * which work on inode numbers, and does not care where the data comes from:
 *   - backend_mem.c, a fake in-memory directory tree;
 *   - backend_fuse.c: the FUSE compatibility layer (fuse_main / fuse_operations is a path-based
 *     interface), which maintains a node table mapping "inode number <-> parent directory +
 *     name" and translates the callbacks here into the path callbacks of
 *     fuse_operations.
 *
 * Conventions:
 *   - every callback returns 0 on success and a negative errno on failure (as FUSE does, so errors pass straight through);
 *   - inode numbers are assigned by the backend and stay stable for the life of the process (they go directly into file handles);
 *   - the server's calls into the backend are serialised (one global lock), so the backend needs no locking of its own.
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#ifndef NFUSE_BACKEND_H
#define NFUSE_BACKEND_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

typedef uint64_t nfuse_ino_t;

/* File attributes: fields mean the same as in struct stat, plus creation / backup times and BSD flags */
struct nfuse_attr {
	nfuse_ino_t	ino;
	uint32_t	mode;		/* includes the S_IFMT type bits */
	uint32_t	nlink;
	uint32_t	uid;
	uint32_t	gid;
	uint64_t	size;
	uint64_t	used;		/* bytes actually allocated */
	uint64_t	rdev;
	struct timespec	atime;
	struct timespec	mtime;
	struct timespec	ctime;
	struct timespec	crtime;		/* creation time (getxtimes on the Mac) */
	struct timespec	bkuptime;	/* backup time */
	uint32_t	flags;		/* UF_HIDDEN etc., see sys/stat.h */
	uint64_t	change;		/* NFSv4 change attribute; if 0 the server derives it from ctime */
	uint32_t	nf4type;	/* if non-zero, overrides the NFS type derived from mode (named-attribute experiment) */
};

struct nfuse_statfs {
	uint64_t	bsize;
	uint64_t	blocks;
	uint64_t	bfree;
	uint64_t	bavail;
	uint64_t	files;
	uint64_t	ffree;
	uint32_t	namemax;
};

/* Directory entry: pos is the position of the "next" entry, where the next read resumes */
struct nfuse_dirent {
	const char		*name;
	nfuse_ino_t		ino;
	uint64_t		pos;
	const struct nfuse_attr	*attr;	/* filled in if the backend has the attributes at hand, may be NULL */
};

/* Fill callback: returns 0 to continue, non-zero when there is no more room and listing must stop (that entry is not consumed) */
typedef int (*nfuse_fill_fn)(void *arg, const struct nfuse_dirent *de);

/* want for getattr / readdir: which attributes are needed (saves callbacks nobody uses) */
#define NFUSE_WANT_BASIC		0x1	/* the struct stat set */
#define NFUSE_WANT_XTIMES	0x2	/* also creation / backup times (getxtimes) */

/* Non-errno errors a backend may return (negated); the server maps them to the matching NFS error */
#define NFUSE_E_NOT_SAME		10027	/* the directory changed, the old cookieverf is no longer valid */
#define NFUSE_E_BAD_COOKIE	10003

/* Attributes supplied with SETATTR / create: mask says which fields are valid */
#define NFUSE_SA_SIZE		0x001
#define NFUSE_SA_MODE		0x002
#define NFUSE_SA_UID		0x004
#define NFUSE_SA_GID		0x008
#define NFUSE_SA_ATIME		0x010	/* atime, explicit value */
#define NFUSE_SA_MTIME		0x020
#define NFUSE_SA_ATIME_NOW	0x040	/* set to the server's current time */
#define NFUSE_SA_MTIME_NOW	0x080
#define NFUSE_SA_CRTIME		0x100	/* creation time (Mac extension setcrtime) */
#define NFUSE_SA_BKUPTIME	0x200	/* backup time (setbkuptime) */
#define NFUSE_SA_FLAGS		0x400	/* BSD flags (hidden / archive, chflags) */

struct nfuse_sattr {
	uint32_t	mask;
	uint64_t	size;
	uint32_t	mode;		/* permission bits 07777 only */
	uint32_t	uid;
	uint32_t	gid;
	struct timespec	atime;
	struct timespec	mtime;
	struct timespec	crtime;
	struct timespec	bkuptime;
	uint32_t	flags_set;	/* NFUSE_SA_FLAGS: bits to set */
	uint32_t	flags_clear;	/* bits to clear */
};

/* Caller identity (handed to the backend at the start of every COMPOUND; corresponds to fuse_get_context) */
struct nfuse_cred {
	uint32_t	uid;
	uint32_t	gid;
};

struct nfuse_backend_ops {
	/* Root directory inode number */
	nfuse_ino_t (*root)(void *bctx);
	/* Start / end of every COMPOUND (may be NULL): sets the caller identity and the node table batch */
	void (*begin)(void *bctx, const struct nfuse_cred *cred);
	void (*end)(void *bctx);
	/*
	 * Make sure the ino in a handle is usable now (called on PUTFH). anc is the ancestor
	 * chain carried in the handle (anc[0] is the parent directory, at most nanc entries).
	 * A backend with a node table limit uses it to bring evicted nodes back level by level
	 * along the chain; if it cannot, it returns -ESTALE (the server replies NFS4ERR_STALE). May be NULL.
	 */
	int (*resolve)(void *bctx, nfuse_ino_t ino, const nfuse_ino_t *anc, int nanc);
	/*
	 * The ancestor chain to store in a handle issued for ino (canonical parent chain,
	 * nearest first); returns the number of entries.
	 * If ino is not in the table, parent_hint is taken as its parent. May be NULL (handles carry no ancestors).
	 */
	int (*ancestry)(void *bctx, nfuse_ino_t ino, nfuse_ino_t parent_hint,
			nfuse_ino_t *anc, int max);
	int (*getattr)(void *bctx, nfuse_ino_t ino, struct nfuse_attr *st,
		       unsigned want);
	/* Look up a name in directory dir; the name is guaranteed not to be "." / ".." */
	int (*lookup)(void *bctx, nfuse_ino_t dir, const char *name,
		      nfuse_ino_t *out);
	/* Parent directory (returns -ENOENT for the root directory's parent) */
	int (*lookupp)(void *bctx, nfuse_ino_t dir, nfuse_ino_t *out);
	/*
	 * List the directory starting at position pos (pos=0 is the beginning), without "." / "..".
	 * *verf in: the cookieverf sent by the client (ignored when pos=0); out: the verifier of this listing.
	 * If the directory has changed and the old verf no longer matches, returns -NFUSE_E_NOT_SAME.
	 * When want is non-zero, supply attributes in de->attr where possible (saves the server a getattr per entry).
	 */
	int (*readdir)(void *bctx, nfuse_ino_t dir, uint64_t pos, uint64_t *verf,
		       unsigned want, nfuse_fill_fn fill, void *arg, int *eof);
	/* Permission check (mask is a combination of R_OK/W_OK/X_OK); if NULL the server decides from the mode bits */
	int (*access)(void *bctx, nfuse_ino_t ino, int mask);
	/* Open / close: correspond to FUSE's open/release; fh is filled in by the backend */
	int (*open)(void *bctx, nfuse_ino_t ino, int flags, uint64_t *fh);
	int (*release)(void *bctx, nfuse_ino_t ino, uint64_t fh);
	/* Read: returns the number of bytes read (fewer than size only at end of file), or a negative errno */
	int (*read)(void *bctx, nfuse_ino_t ino, uint64_t fh, void *buf,
		    uint32_t size, uint64_t off);
	int (*readlink)(void *bctx, nfuse_ino_t ino, char *buf, size_t size);
	int (*statfs)(void *bctx, struct nfuse_statfs *sf);
	/* Extended attributes (named attributes, may be NULL): same semantics as listxattr / getxattr (returns the size when buf is NULL) */
	int (*listxattr)(void *bctx, nfuse_ino_t ino, char *buf, size_t size);
	int (*getxattr)(void *bctx, nfuse_ino_t ino, const char *name,
			void *buf, size_t size);

	/* ---------- writing ----------
	 * All may be NULL (read-only backend). The modifying callbacks keep the node table /
	 * directory snapshots / attribute cache consistent themselves (the server only handles
	 * the protocol). Names are guaranteed not to be "." / ".." and to contain no '/'.
	 */
	/* Write: returns the number of bytes written (should equal size), or a negative errno */
	int (*write)(void *bctx, nfuse_ino_t ino, uint64_t fh, const void *buf,
		     uint32_t size, uint64_t off);
	/* Flush to disk: fh 0 means there is no open handle (the file may already be closed at COMMIT time) */
	int (*fsync)(void *bctx, nfuse_ino_t ino, uint64_t fh, int datasync);
	/* Create a regular file in dir and open it (returns -EEXIST if the name exists); fh is filled in by the backend */
	int (*create)(void *bctx, nfuse_ino_t dir, const char *name, uint32_t mode,
		      int flags, nfuse_ino_t *ino, uint64_t *fh);
	int (*mkdir)(void *bctx, nfuse_ino_t dir, const char *name, uint32_t mode,
		     nfuse_ino_t *ino);
	int (*symlink)(void *bctx, nfuse_ino_t dir, const char *name,
		       const char *target, nfuse_ino_t *ino);
	/* Device / FIFO / socket (mode includes the type bits) */
	int (*mknod)(void *bctx, nfuse_ino_t dir, const char *name, uint32_t mode,
		     uint64_t rdev, nfuse_ino_t *ino);
	/* Give ino another name in newdir (hard link) */
	int (*link)(void *bctx, nfuse_ino_t ino, nfuse_ino_t newdir,
		    const char *newname);
	/* Remove a name: rmdir if the target is a directory, otherwise unlink (the backend works out which) */
	int (*remove)(void *bctx, nfuse_ino_t dir, const char *name);
	int (*rename)(void *bctx, nfuse_ino_t olddir, const char *oldname,
		      nfuse_ino_t newdir, const char *newname);
	/* Change attributes: a non-zero fh is an open handle (truncation uses ftruncate) */
	int (*setattr)(void *bctx, nfuse_ino_t ino, uint64_t fh,
		       const struct nfuse_sattr *sa);
	/* Tell the backend to drop its cached attributes for ino (size / mtime changed after a WRITE) */
	void (*invalidate)(void *bctx, nfuse_ino_t ino);

	/* ---------- named-attribute writes (all may be NULL: named attributes read-only) ----------
	 * Same semantics as extended attributes; "does not exist" is always -ENOATTR. Large values (the resource fork) are read and written by offset, never fetched whole.
	 */
	int (*xattr_read)(void *bctx, nfuse_ino_t ino, const char *name, void *buf,
			  uint32_t size, uint64_t off);
	/* Write at off (creating it if absent; size 0 only creates), without truncating; returns the number of bytes written */
	int (*xattr_write)(void *bctx, nfuse_ino_t ino, const char *name,
			   const void *buf, uint32_t size, uint64_t off);
	int (*xattr_truncate)(void *bctx, nfuse_ino_t ino, const char *name,
			      uint64_t size);
	int (*removexattr)(void *bctx, nfuse_ino_t ino, const char *name);
};

struct nfuse_backend {
	const struct nfuse_backend_ops	*ops;
	void				*ctx;
	int				readonly;
};

/* Fake in-memory directory tree (used by the prototype server) */
int  nfuse_backend_mem_init(struct nfuse_backend *be);

#endif /* NFUSE_BACKEND_H */
