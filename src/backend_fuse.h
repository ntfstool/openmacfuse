/*
 * backend_fuse.h - translates the server's inode-based backend interface into FUSE's path-based callbacks
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#ifndef OPENMACFUSE_BACKEND_FUSE_H
#define OPENMACFUSE_BACKEND_FUSE_H

#include <stddef.h>
#include <stdint.h>

#include "backend.h"

struct fuse_operations;
struct fuse_context;
struct openmacfuse_ext_ops;

struct openmacfuse_bf_config {
	size_t		max_nodes;	/* node table limit (default 32768) */
	uint32_t	attr_ttl_ms;	/* attribute cache lifetime (default 1000) */
	size_t		snap_max_bytes;	/* limit on cached directory listing snapshots (default 16MB; a snapshot in use is exempt) */
	void		(*log)(const char *fmt, ...);
	int		fake_xattr;	/* named-attribute experiment: every object gets one extra read-only attribute, org.openmacfuse.probe */
	/* names and extended attributes */
	int		namedattr;	/* named-attribute channel (experimental): the client uses OPENATTR */
	int		adtrans;	/* real on-disk `._X` files: hidden from listings, merged into X's attribute view, migrated on write (adfile / streams modes) */
	int		ad_clean;	/* after migrating a `._X` on attribute write, delete it (0: keep it and record a "migrated" marker) */
	int		case_insensitive;	/* case-insensitive lookup */
	int		nfc;		/* convert names to NFC on create / rename */
	const struct openmacfuse_ext_ops *ext;	/* extension callbacks registered by the file system (may be NULL) */
	int		raw_names;	/* test hook: names pass through untouched, no equivalence lookup / NFC (to build volumes where both spellings coexist) */
};

/* Result of the pre-mount case-conflict scan */
struct openmacfuse_case_scan {
	uint32_t	dirs;		/* directories scanned */
	uint64_t	entries;	/* directory entries scanned */
	uint32_t	conflicts;	/* names that differ only in case (number of pairs) */
	uint32_t	normdups;	/* names that differ only in NFC / NFD form (number of pairs) */
	int		complete;	/* the whole volume was scanned within the time budget */
	double		ms;
	char		sample[512];	/* first conflict found (path) */
};

/*
 * Initialise: fetch the root directory's attributes (getattr("/")) for the root node.
 * Returns 0 on success, negative errno on failure.
 * fctx is owned by the compatibility library: at the start of every COMPOUND the
 * caller's uid/gid are written into it, and that is what fuse_get_context() returns
 * inside the driver's callbacks.
 */
int  openmacfuse_backend_fuse_init(struct openmacfuse_backend *be,
			    const struct fuse_operations *op,
			    struct fuse_context *fctx,
			    const struct openmacfuse_bf_config *cfg);
void openmacfuse_backend_fuse_free(struct openmacfuse_backend *be);
/* Write the statistics (node table, snapshots, callback counts) as one line of text */
void openmacfuse_backend_fuse_stats(struct openmacfuse_backend *be, char *buf, size_t n);
/* Write the name / xattr statistics (case / normalisation / `._` / named attributes) as one line of text */
void openmacfuse_backend_fuse_stats_m3(struct openmacfuse_backend *be, char *buf, size_t n);
/*
 * Scan directories level by level from the root, looking for names that differ only in
 * case (under the current fold function), for at most budget_ms milliseconds.
 * Returns 0; the result is in out.
 */
int  openmacfuse_backend_fuse_case_scan(struct openmacfuse_backend *be, uint32_t budget_ms,
				 struct openmacfuse_case_scan *out);
/* Change the case mode (decided before mounting; drops any name index already built) */
void openmacfuse_backend_fuse_set_case(struct openmacfuse_backend *be, int insensitive);

#endif /* OPENMACFUSE_BACKEND_FUSE_H */
