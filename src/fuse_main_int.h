/*
 * fuse_main_int.h -- internal interface of fuse_main (for unit tests: the mount steps can be
 * replaced with fakes, so it can be tested on a development machine without mounting anything)
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#ifndef OPENMACFUSE_FUSE_MAIN_INT_H
#define OPENMACFUSE_FUSE_MAIN_INT_H

#include "fuse.h"
#include "openmacfuse.h"

/* The steps fuse_main calls in order; the defaults are the public functions of the same name */
struct openmacfuse_main_hooks {
	struct fuse_chan *(*mount)(const char *mountpoint, struct fuse_args *args);
	struct fuse *(*fnew)(struct fuse_chan *ch, struct fuse_args *args,
			     const struct fuse_operations *op, size_t op_size, void *user_data);
	int (*set_signals)(struct fuse_session *se);
	void (*remove_signals)(struct fuse_session *se);
	struct fuse_session *(*session)(struct fuse *f);
	int (*loop)(struct fuse *f);
	void (*unmount)(const char *mountpoint, struct fuse_chan *ch);
	void (*destroy)(struct fuse *f);
	/* Last step of going to the background (setsid, chdir, stdio redirected to /dev/null); tests replace it with a recorder */
	void (*detach)(void);
	/* For telemetry (openmacfuse_get_mount_info / openmacfuse_end_reason) */
	int (*mount_info)(struct fuse *f, struct openmacfuse_mount_info *mi, size_t size);
	const char *(*end_reason)(struct fuse *f);
};

/* Run fuse_main with the given steps (defaults are used when hooks, or any member of it, is NULL) */
int openmacfuse_main_hooked(int argc, char *argv[], const struct fuse_operations *op,
		      size_t op_size, void *user_data, const struct openmacfuse_main_hooks *hooks);

/*
 * The argument-list fix-ups fuse_main does before mounting (tested directly by the unit tests):
 * insert the default openmacfuse_case / openmacfuse_xattr at the front, add the last component of the mount
 * point when there is no volname, append the OPENMACFUSE_OPTS environment variable. Returns 0 on success.
 */
int openmacfuse_main_prepare_args(struct fuse_args *args, const char *mountpoint);

#endif /* OPENMACFUSE_FUSE_MAIN_INT_H */
