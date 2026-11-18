/*
 * mounter.h - mount via the system mount_nfs, query the mount table, unmount
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#ifndef OPENMACFUSE_MOUNTER_H
#define OPENMACFUSE_MOUNTER_H

#include <stddef.h>

#include <sys/types.h>

/*
 * Run /sbin/mount_nfs -o <opts> <spec> <mnt>, waiting at most timeout_s seconds.
 * Returns 0 on success; -1 on failure, with mount_nfs's error output in errbuf; -2 on timeout:
 * **mount_nfs is not killed** (a mount_nfs in the middle of the handshake must never be killed),
 * openmacfuse_mount_pid is still its pid, and the caller, after failing the handshake, waits for it to
 * exit with openmacfuse_mount_nfs_wait.
 * The server thread must already be running before this is called: the mount handshake takes
 * place during this call.
 */
int openmacfuse_run_mount_nfs(const char *spec, const char *opts, const char *mnt,
		       int timeout_s, char *errbuf, size_t errlen);

/*
 * pid of the running mount_nfs (0 if none). While the handshake is in progress (it is still
 * running), do not kill it and do not umount -f the mount point.
 */
extern volatile pid_t openmacfuse_mount_pid;

/*
 * Wait up to ms milliseconds for a mount_nfs left behind by a timeout to exit on its own: returns
 * 1 and clears openmacfuse_mount_pid if it has exited (or there was none), 0 if it is still running.
 */
int openmacfuse_mount_nfs_wait(int ms);

/*
 * Whether mnt is an NFS mount in the mount table (without contacting the server: getfsstat
 * MNT_NOWAIT).
 * If from is non-NULL the mount source (f_mntfromname) must also equal it:
 * a different mount stacked on the same path (e.g. a previous one that was not unmounted cleanly)
 * does not count as ours.
 */
int openmacfuse_is_nfs_mounted(const char *mnt, const char *from);

/*
 * Unmount: try a normal unmount first; if busy, wait a moment and then force. Returns 0 on success.
 */
int openmacfuse_unmount(const char *mnt, const char *from,
		 void (*log)(const char *fmt, ...));

/*
 * Force unmount right away (used when the device has been unplugged: skip the normal unmount so
 * the client's pending requests fail immediately).
 * Returns 0 on success.
 */
int openmacfuse_unmount_force(const char *mnt, const char *from,
		       void (*log)(const char *fmt, ...));

#endif /* OPENMACFUSE_MOUNTER_H */
