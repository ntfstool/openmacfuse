/*
 * mounter.c - mount via the system mount_nfs, query the mount table, unmount
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/ucred.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "mounter.h"
#include "watchdog.h"

extern char **environ;

volatile pid_t openmacfuse_mount_pid;

int openmacfuse_run_mount_nfs(const char *spec, const char *opts, const char *mnt,
		       int timeout_s, char *errbuf, size_t errlen)
{
	posix_spawn_file_actions_t fa;
	char *argv[] = { "mount_nfs", "-o", (char *)opts, (char *)spec,
			 (char *)mnt, NULL };
	int pfd[2], st = 0, rc;
	pid_t pid;
	size_t got = 0;
	uint64_t t0 = openmacfuse_uptime_ns();

	if (errlen)
		errbuf[0] = 0;
	if (pipe(pfd) < 0) {
		snprintf(errbuf, errlen, "pipe: %s", strerror(errno));
		return -1;
	}
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_adddup2(&fa, pfd[1], 1);
	posix_spawn_file_actions_adddup2(&fa, pfd[1], 2);
	posix_spawn_file_actions_addclose(&fa, pfd[0]);
	posix_spawn_file_actions_addclose(&fa, pfd[1]);
	rc = posix_spawn(&pid, "/sbin/mount_nfs", &fa, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	close(pfd[1]);
	if (rc) {
		close(pfd[0]);
		snprintf(errbuf, errlen, "posix_spawn mount_nfs: %s", strerror(rc));
		return -1;
	}
	openmacfuse_mount_pid = pid;
	/* collect the output while waiting for the process to exit */
	for (;;) {
		struct pollfd p = { .fd = pfd[0], .events = POLLIN };
		pid_t w = waitpid(pid, &st, WNOHANG);

		if (w == pid)
			break;
		if (openmacfuse_uptime_ns() - t0 > (uint64_t)timeout_s * 1000000000ULL) {
			/*
			 * Timeout: do not kill mount_nfs (the lesson from a kernel panic hit
			 * during testing: a mount_nfs in the middle of the handshake must not be
			 * killed and must not be umount -f'ed; let it fail and return on its
			 * own). openmacfuse_mount_pid keeps its pid; the caller takes over the
			 * connection and answers IO so that mount_nfs reports the error itself,
			 * then uses openmacfuse_mount_nfs_wait to wait a while and see whether it has
			 * exited.
			 */
			close(pfd[0]);
			snprintf(errbuf, errlen, "mount_nfs timed out (%d s)", timeout_s);
			return -2;
		}
		if (poll(&p, 1, 50) > 0 && errlen && got < errlen - 1) {
			ssize_t n = read(pfd[0], errbuf + got, errlen - 1 - got);
			if (n > 0)
				got += (size_t)n;
		}
	}
	openmacfuse_mount_pid = 0;
	for (;;) {
		ssize_t n;
		if (!errlen || got >= errlen - 1)
			break;
		n = read(pfd[0], errbuf + got, errlen - 1 - got);
		if (n <= 0)
			break;
		got += (size_t)n;
	}
	close(pfd[0]);
	if (errlen) {
		errbuf[got] = 0;
		while (got && (errbuf[got - 1] == '\n' || errbuf[got - 1] == '\r'))
			errbuf[--got] = 0;
	}
	if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
		return 0;
	if (errlen && !errbuf[0])
		snprintf(errbuf, errlen, "mount_nfs exit code %d",
			 WIFEXITED(st) ? WEXITSTATUS(st) : -1);
	return -1;
}

int openmacfuse_mount_nfs_wait(int ms)
{
	pid_t pid = openmacfuse_mount_pid;
	int st, waited = 0;

	if (pid <= 0)
		return 1;
	for (;;) {
		pid_t w = waitpid(pid, &st, WNOHANG);

		if (w == pid || (w < 0 && errno == ECHILD)) {
			openmacfuse_mount_pid = 0;
			return 1;
		}
		if (waited >= ms)
			return 0;
		usleep(50 * 1000);
		waited += 50;
	}
}

int openmacfuse_is_nfs_mounted(const char *mnt, const char *from)
{
	struct statfs *buf;
	int n, i, found = 0;

	n = getfsstat(NULL, 0, MNT_NOWAIT);
	if (n <= 0)
		return 0;
	buf = calloc((size_t)n + 8, sizeof(*buf));
	if (!buf)
		return 1;	/* out of memory: treat it as "still mounted", better to wait longer */
	n = getfsstat(buf, (int)(((size_t)n + 8) * sizeof(*buf)), MNT_NOWAIT);
	for (i = 0; i < n; i++)
		if (strcmp(buf[i].f_mntonname, mnt) == 0 &&
		    strcmp(buf[i].f_fstypename, "nfs") == 0 &&
		    (!from || !*from || strcmp(buf[i].f_mntfromname, from) == 0)) {
			found = 1;
			break;
		}
	free(buf);
	return found;
}

int openmacfuse_unmount(const char *mnt, const char *from,
		 void (*log)(const char *fmt, ...))
{
	int i;

	if (!openmacfuse_is_nfs_mounted(mnt, from))
		return 0;
	for (i = 0; i < 5; i++) {
		if (unmount(mnt, 0) == 0)
			return 0;
		if (errno != EBUSY)
			break;
		usleep(200 * 1000);
	}
	if (log)
		log("normal unmount failed (%s), forcing unmount", strerror(errno));
	return openmacfuse_unmount_force(mnt, from, log);
}

int openmacfuse_unmount_force(const char *mnt, const char *from,
		       void (*log)(const char *fmt, ...))
{
	int i;

	for (i = 0; i < 3; i++) {
		if (!openmacfuse_is_nfs_mounted(mnt, from))
			return 0;
		if (unmount(mnt, MNT_FORCE) == 0)
			return 0;
		if (log)
			log("forced unmount failed (attempt %d): %s", i + 1, strerror(errno));
		usleep(300 * 1000);
	}
	return openmacfuse_is_nfs_mounted(mnt, from) ? -1 : 0;
}
