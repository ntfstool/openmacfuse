/*
 * watchdog.h -- automatic recovery: guard process (force-unmounts on behalf of a driver process
 * that died or froze as a whole) and the monotonic clock
 *
 * The watchdog thread itself lives in fuse_lib.c (it needs the internals of struct fuse); only
 * the parts that do not depend on that state are here:
 *   nfuse_uptime_ns   monotonic clock that excludes system sleep (CLOCK_UPTIME_RAW). Both the
 *                     watchdog and the guard process time with it: it does not advance during
 *                     sleep, so after wake-up "slept for 8 hours" is not mistaken for "a request
 *                     has been stuck for 8 hours".
 *   nfuse_guard_start forks a minimal guard process: the parent (the driver) writes one byte per
 *                     second to a pipe as a heartbeat;
 *                     EOF on the pipe (driver exited / got kill -9 / crashed) -> if the mount is
 *                     still there, unmount(MNT_FORCE);
 *                     no heartbeat for stall_ms (whole process frozen / the watchdog thread itself
 *                     stuck) -> force-unmount first, then SIGKILL the driver if it has not exited
 *                     after 3 seconds. The child only makes system calls (no malloc / stdio /
 *                     locks after fork) and closes every other inherited descriptor (no device,
 *                     listening socket or client connection is kept).
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#ifndef NFUSE_WATCHDOG_H
#define NFUSE_WATCHDOG_H

#include <limits.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/mount.h>

uint64_t nfuse_uptime_ns(void);

struct nfuse_guard_cfg {
	char	mnt[PATH_MAX];		/* mount point */
	char	from[PATH_MAX + 300];	/* mount source (only our own entry gets unmounted) */
	int	log_fd;			/* log (NFUSE_LOG), -1 = none */
	int	report_fd;		/* telemetry JSON lines (supplied by the file system, see report_fd in nfuse.h), -1 = none */
	uint32_t stall_ms;		/* heartbeat silence that counts as frozen; 0 = only handle "driver exited" */
	int	lock_fd;		/* mount-point lock (see nfuse_guard_lock): held by the guard process until it exits, -1 = none */
	fsid_t	fsid;			/* f_fsid of this mount (the client assigns one per mount): over TCP the source string is
					 * just 127.0.0.1:/volname and is identical after a same-named volume is remounted, so this
					 * is the only way to recognise "our" entry; all zero = do not compare */
};

/*
 * Start the guard process. Returns the write end of the heartbeat pipe (non-blocking,
 * FD_CLOEXEC), -1 on failure. *gpid receives the guard process pid. The caller writes 1 byte
 * per second to the returned descriptor.
 */
int nfuse_guard_start(const struct nfuse_guard_cfg *cfg, pid_t *gpid);

/*
 * Mount-point lock: /tmp/nfuse-mnt-<hash of mount point>.lock. The guard process holds a shared
 * lock until it exits; a new driver takes the exclusive lock once before mounting (waiting at
 * most wait_ms) to make sure the guard process of the previous driver on the same mount point
 * has finished cleaning up. Reason: measured: after the driver gets kill -9
 * the guard's unmount(MNT_FORCE) takes about 2 seconds to return; during that time the old mount
 * is already gone from the mount table, a new driver mounts on the same path, and when the old
 * forced unmount completes it detaches the new mount from the directory tree (the files in the
 * new mount "all disappear", and unmounting it later returns EPERM).
 *   nfuse_guard_lock_open  open (create if missing) the lock file; returns a descriptor (FD_CLOEXEC), -1 on failure
 *   nfuse_guard_wait       wait for the exclusive lock (released as soon as it is obtained); returns the
 *                          milliseconds waited, -1 on timeout
 */
int nfuse_guard_lock_open(const char *mnt);
int nfuse_guard_wait(const char *mnt, int wait_ms);

#endif /* NFUSE_WATCHDOG_H */
