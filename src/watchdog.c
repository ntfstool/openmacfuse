/*
 * watchdog.c -- monotonic clock + guard process (see watchdog.h)
 *
 * Why a separate process: when the driver process gets kill -9 / crashes, or is frozen as a
 * whole (every thread stopped, including the in-process watchdog thread), the mount is still
 * there but the server is gone, and the system NFS client (hard mount) keeps processes that
 * touch the mount point waiting until deadtimeout (measured: 66-70 seconds) before giving up.
 * The guard process does not depend on any thread in the driver; as soon as the driver is gone
 * (EOF on the pipe) it calls unmount(MNT_FORCE).
 *
 * After fork the child only makes system calls and calls pure functions that touch no global
 * state (strcmp and the like): no malloc, no stdio, no locks of any kind -- another thread may
 * have been holding those locks at the time of the fork. Log lines and telemetry lines are
 * assembled by hand in small stack buffers.
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/file.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <crt_externs.h>

#include "watchdog.h"

uint64_t nfuse_uptime_ns(void)
{
	return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

/* ---------- Guard process (the child only uses the small non-allocating helpers below) ---------- */

struct gbuf {
	char	b[1024];
	int	n;
};

static void gb_s(struct gbuf *g, const char *s)
{
	while (s && *s && g->n < (int)sizeof(g->b) - 2)
		g->b[g->n++] = *s++;
}

static void gb_u(struct gbuf *g, uint64_t v)
{
	char t[24];
	int i = 0;

	do {
		t[i++] = (char)('0' + v % 10);
		v /= 10;
	} while (v && i < (int)sizeof(t));
	while (i-- > 0 && g->n < (int)sizeof(g->b) - 2)
		g->b[g->n++] = t[i];
}

static void gb_u2(struct gbuf *g, unsigned v)
{
	if (v < 10)
		gb_s(g, "0");
	gb_u(g, v);
}

/* milliseconds -> "seconds.milliseconds" */
static void gb_secs(struct gbuf *g, uint64_t ms)
{
	gb_u(g, ms / 1000);
	gb_s(g, ".");
	if (ms % 1000 < 100)
		gb_s(g, "0");
	if (ms % 1000 < 10)
		gb_s(g, "0");
	gb_u(g, ms % 1000);
}

static long g_gmtoff;		/* computed in the parent before fork (localtime_r cannot be used in the child) */
static int g_have_fsid;
static pid_t g_driver_pid;

static uint64_t wall_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}

/* Same format as the log lines of fuse_lib.c: HH:MM:SS.mmm [nfuse-guard PID] text */
static void glog(const struct nfuse_guard_cfg *c, struct gbuf *msg)
{
	struct gbuf g;
	uint64_t ms = wall_ms();
	uint64_t s = ms / 1000 + (uint64_t)g_gmtoff;

	if (c->log_fd < 0)
		return;
	g.n = 0;
	gb_u2(&g, (unsigned)(s / 3600 % 24));
	gb_s(&g, ":");
	gb_u2(&g, (unsigned)(s / 60 % 60));
	gb_s(&g, ":");
	gb_u2(&g, (unsigned)(s % 60));
	gb_s(&g, ".");
	if (ms % 1000 < 100)
		gb_s(&g, "0");
	if (ms % 1000 < 10)
		gb_s(&g, "0");
	gb_u(&g, ms % 1000);
	gb_s(&g, " [nfuse-guard ");
	gb_u(&g, (uint64_t)getpid());
	gb_s(&g, "] ");
	msg->b[msg->n] = 0;
	gb_s(&g, msg->b);
	g.b[g.n++] = '\n';
	(void)!write(c->log_fd, g.b, (size_t)g.n);
}

/* One JSON telemetry line (same field names as the driver telemetry: ts / pid / role / seq / type) */
static void greport(const struct nfuse_guard_cfg *c, const char *what, int mounted,
		    int rc, uint64_t ms, uint64_t silent_ms, int killed)
{
	struct gbuf g;

	if (c->report_fd < 0)
		return;
	g.n = 0;
	gb_s(&g, "{\"ts\":");
	gb_u(&g, wall_ms());
	gb_s(&g, ",\"pid\":");
	gb_u(&g, (uint64_t)g_driver_pid);
	gb_s(&g, ",\"role\":\"nfuse\",\"seq\":0,\"type\":\"nfuse_guard\",\"what\":\"");
	gb_s(&g, what);
	gb_s(&g, "\",\"mounted\":");
	gb_u(&g, (uint64_t)(mounted ? 1 : 0));
	gb_s(&g, ",\"unmount_rc\":");
	gb_s(&g, rc ? "-1" : "0");
	gb_s(&g, ",\"ms\":");
	gb_u(&g, ms);
	gb_s(&g, ",\"silent_ms\":");
	gb_u(&g, silent_ms);
	gb_s(&g, ",\"killed\":");
	gb_u(&g, (uint64_t)(killed ? 1 : 0));
	gb_s(&g, ",\"guard_pid\":");
	gb_u(&g, (uint64_t)getpid());
	gb_s(&g, "}");
	g.b[g.n++] = '\n';
	(void)!write(c->report_fd, g.b, (size_t)g.n);
}

/* Is our entry (mount point and source both match) still in the mount table? The buffer comes from mmap, not malloc */
static int g_mounted(const struct nfuse_guard_cfg *c)
{
	int n, i, found = 0;
	size_t sz;
	struct statfs *b;

	n = getfsstat(NULL, 0, MNT_NOWAIT);
	if (n <= 0)
		return 0;
	sz = ((size_t)n + 16) * sizeof(*b);
	b = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
	if (b == MAP_FAILED)
		return 1;	/* cannot tell: treat it as still mounted */
	n = getfsstat(b, (int)sz, MNT_NOWAIT);
	for (i = 0; i < n; i++)
		if (strcmp(b[i].f_mntonname, c->mnt) == 0 &&
		    strcmp(b[i].f_fstypename, "nfs") == 0 &&
		    (!c->from[0] || strcmp(b[i].f_mntfromname, c->from) == 0) &&
		    (!g_have_fsid || memcmp(&b[i].f_fsid, &c->fsid, sizeof(c->fsid)) == 0)) {
			found = 1;
			break;
		}
	munmap(b, sz);
	return found;
}

/*
 * unmount(path) removes the topmost mount on that path. When another mount is stacked on the
 * same path (for example an application or a test has already remounted a volume of the same
 * name there) we must not unmount by path -- that would remove somebody else's mount. Seen in a
 * kill -9 test run: the guard process of the old driver force-unmounted the same-named volume
 * the new driver had just mounted, and the files "disappeared" in the middle of the test's reads.
 */
static int g_top_is_ours(const struct nfuse_guard_cfg *c)
{
	/* statfs(mount point) is not an option: with the driver dead, statfs on a hard NFS mount hangs. Use getfsstat(MNT_NOWAIT); in mount-table order the last entry for a path is the topmost */
	int n, i, top = -1, ours = 1;
	size_t sz;
	struct statfs *b;

	if (!g_have_fsid)
		return 1;
	n = getfsstat(NULL, 0, MNT_NOWAIT);
	if (n <= 0)
		return 0;
	sz = ((size_t)n + 16) * sizeof(*b);
	b = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
	if (b == MAP_FAILED)
		return 0;
	n = getfsstat(b, (int)sz, MNT_NOWAIT);
	for (i = 0; i < n; i++)
		if (strcmp(b[i].f_mntonname, c->mnt) == 0)
			top = i;
	if (top >= 0)
		ours = memcmp(&b[top].f_fsid, &c->fsid, sizeof(c->fsid)) == 0;
	munmap(b, sz);
	return ours;
}

static int g_force_unmount(const struct nfuse_guard_cfg *c)
{
	int i;

	for (i = 0; i < 5; i++) {
		if (!g_mounted(c))
			return 0;
		if (!g_top_is_ours(c))
			return -1;	/* somebody else's mount is stacked on top: leave it alone */
		if (unmount(c->mnt, MNT_FORCE) == 0)
			return 0;
		usleep(300 * 1000);
	}
	return g_mounted(c) ? -1 : 0;
}

/* Wait up to ms milliseconds for EOF on the pipe (driver exit): 1 if it came */
static int g_wait_eof(int rfd, int ms)
{
	uint64_t end = nfuse_uptime_ns() + (uint64_t)ms * 1000000ULL;

	while (nfuse_uptime_ns() < end) {
		struct pollfd p = { .fd = rfd, .events = POLLIN };
		char b[256];
		ssize_t n;

		if (poll(&p, 1, 100) <= 0)
			continue;
		n = read(rfd, b, sizeof(b));
		if (n == 0)
			return 1;
		if (n < 0 && errno != EINTR && errno != EAGAIN)
			return 1;
	}
	return 0;
}

static void guard_main(const struct nfuse_guard_cfg *c, int rfd)
{
	uint64_t last = nfuse_uptime_ns(), t0, silent, gapmax = 0;
	struct gbuf m;
	int mounted, rc, killed = 0, stalled = 0;

	m.n = 0;
	gb_s(&m, "guard process started: driver pid ");
	gb_u(&m, (uint64_t)g_driver_pid);
	gb_s(&m, ", mount point ");
	gb_s(&m, c->mnt);
	gb_s(&m, g_have_fsid ? " (mount identified by fsid)" : " (no fsid available, identified by mount point + source only)");
	if (c->stall_ms) {
		gb_s(&m, ", frozen after ");
		gb_secs(&m, c->stall_ms);
		gb_s(&m, " s without a heartbeat");
	}
	glog(c, &m);
	for (;;) {
		struct pollfd p = { .fd = rfd, .events = POLLIN };
		int r = poll(&p, 1, 250);

		if (r > 0) {
			char b[256];
			ssize_t n = read(rfd, b, sizeof(b));

			if (n > 0) {
				uint64_t t = nfuse_uptime_ns();

				if (t - last > gapmax)
					gapmax = t - last;	/* longest heartbeat gap: the basis for choosing the threshold */
				last = t;
				continue;
			}
			if (n == 0 || (errno != EINTR && errno != EAGAIN))
				break;		/* driver exited (every write end closed) */
		}
		if (getppid() != g_driver_pid)
			break;			/* reparented: the driver is gone */
		if (c->stall_ms && nfuse_uptime_ns() - last >= (uint64_t)c->stall_ms * 1000000ULL) {
			stalled = 1;
			break;
		}
	}
	silent = (nfuse_uptime_ns() - last) / 1000000ULL;
	t0 = nfuse_uptime_ns();
	mounted = g_mounted(c);
	if (!stalled) {
		if (!mounted) {
			/* Normal exit: the driver already unmounted by itself */
			m.n = 0;
			gb_s(&m, "driver exited and the mount is gone, guard process exiting; longest heartbeat gap was ");
			gb_secs(&m, gapmax / 1000000ULL);
			gb_s(&m, " s");
			glog(c, &m);
			_exit(0);
		}
		rc = g_force_unmount(c);
		m.n = 0;
		gb_s(&m, "driver process ");
		gb_u(&m, (uint64_t)g_driver_pid);
		gb_s(&m, " exited (killed / crashed) with the mount point still mounted -> forced unmount ");
		gb_s(&m, rc ? "failed" : "succeeded");
		gb_s(&m, ", took ");
		gb_secs(&m, (nfuse_uptime_ns() - t0) / 1000000ULL);
		gb_s(&m, " s");
		glog(c, &m);
		greport(c, "driver_gone", 1, rc, (nfuse_uptime_ns() - t0) / 1000000ULL, silent, 0);
		_exit(0);
	}
	m.n = 0;
	gb_s(&m, "driver process ");
	gb_u(&m, (uint64_t)g_driver_pid);
	gb_s(&m, " has had no heartbeat for ");
	gb_secs(&m, silent);
	gb_s(&m, " s (whole process frozen or watchdog stuck) -> force-unmounting the mount point, SIGKILL if it does not exit within 3 s");
	glog(c, &m);
	rc = mounted ? g_force_unmount(c) : 0;
	if (!g_wait_eof(rfd, 3000)) {
		kill(g_driver_pid, SIGKILL);
		killed = 1;
		g_wait_eof(rfd, 3000);
	}
	if (g_mounted(c))
		rc = g_force_unmount(c);
	m.n = 0;
	gb_s(&m, "freeze handled: forced unmount");
	gb_s(&m, !mounted ? " (mount was already gone)" : rc ? " failed" : " succeeded");
	gb_s(&m, killed ? ", driver SIGKILLed" : ", driver exited by itself");
	gb_s(&m, ", took ");
	gb_secs(&m, (nfuse_uptime_ns() - t0) / 1000000ULL);
	gb_s(&m, " s");
	glog(c, &m);
	greport(c, "driver_stalled", mounted, rc, (nfuse_uptime_ns() - t0) / 1000000ULL, silent, killed);
	_exit(0);
}

static struct nfuse_guard_cfg g_cfg;	/* after fork the child reads its own copy */

int nfuse_guard_lock_open(const char *mnt)
{
	char path[80];
	uint64_t h = 1469598103934665603ULL;
	const unsigned char *p;
	int fd;

	for (p = (const unsigned char *)mnt; *p; p++) {
		h ^= *p;
		h *= 1099511628211ULL;
	}
	snprintf(path, sizeof(path), "/tmp/nfuse-mnt-%016llx.lock", (unsigned long long)h);
	fd = open(path, O_RDONLY | O_CREAT | O_CLOEXEC, 0644);
	if (fd < 0)
		fd = open(path, O_RDONLY | O_CLOEXEC);	/* created by another user: a read-only descriptor can still be locked */
	return fd;
}

int nfuse_guard_wait(const char *mnt, int wait_ms)
{
	uint64_t t0 = nfuse_uptime_ns();
	int fd = nfuse_guard_lock_open(mnt), ms;

	if (fd < 0)
		return 0;
	for (;;) {
		if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
			flock(fd, LOCK_UN);
			close(fd);
			return (int)((nfuse_uptime_ns() - t0) / 1000000ULL);
		}
		ms = (int)((nfuse_uptime_ns() - t0) / 1000000ULL);
		if (ms >= wait_ms) {
			close(fd);
			return -1;
		}
		usleep(50 * 1000);
	}
}

/*
 * Retitle the child: the forked process has exactly the same command line as the driver, so an
 * application or script looking for the driver with `pgrep -f` would count it too. The command
 * line shown by ps / pgrep -f is read from the argument area in the process's own memory
 * (KERN_PROCARGS2); rewriting that area in place in the child (not growing it, only using the
 * original size) is enough to tell them apart: it shows as "nfuse-guard <driver pid> <mount point>".
 * The process name (p_comm, what pgrep without -f compares) cannot be changed and remains the
 * executable name.
 */
static void guard_retitle(void)
{
	char **argv = *_NSGetArgv();
	int argc = *_NSGetArgc(), i;
	char *start, *end, *p;
	char pid[24];
	struct gbuf g;
	const char *tok[3];

	if (!argv || argc < 1 || !argv[0])
		return;
	start = argv[0];
	end = argv[0] + strlen(argv[0]);
	for (i = 1; i < argc && argv[i]; i++) {
		if (argv[i] != end + 1)
			return;		/* argument area not contiguous: leave it */
		end = argv[i] + strlen(argv[i]);
	}
	g.n = 0;
	gb_u(&g, (uint64_t)g_driver_pid);
	g.b[g.n] = 0;
	memcpy(pid, g.b, (size_t)g.n + 1);
	tok[0] = "nfuse-guard";
	tok[1] = pid;
	tok[2] = g_cfg.mnt;
	/*
	 * ps reads argc NUL-terminated strings as the arguments: fill all argc strings (extras get "-"),
	 * otherwise it would show the environment variables that follow as arguments.
	 * With fewer than 3 arguments the remaining words are joined into the last string with spaces.
	 */
	memset(start, 0, (size_t)(end - start));
	p = start;
	for (i = 0; i < argc; i++) {
		size_t room = (size_t)(end - p), n;

		g.n = 0;
		if (i < 3)
			gb_s(&g, tok[i]);
		else
			gb_s(&g, "-");
		if (i == argc - 1)
			for (int k = i + 1; k < 3; k++) {
				gb_s(&g, " ");
				gb_s(&g, tok[k]);
			}
		n = (size_t)g.n;
		if (room == 0)
			break;
		if (n >= room)
			n = room - 1;
		memcpy(p, g.b, n);
		p[n] = 0;
		argv[i] = p;
		p += n + 1;
		if (p > end)
			p = end;
	}
}

int nfuse_guard_start(const struct nfuse_guard_cfg *cfg, pid_t *gpid)
{
	int pfd[2];
	pid_t pid;
	time_t t = time(NULL);
	struct tm tm;

	if (pipe(pfd) < 0)
		return -1;
	g_cfg = *cfg;
	{
		static const fsid_t zero;

		g_have_fsid = memcmp(&g_cfg.fsid, &zero, sizeof(zero)) != 0;
	}
	localtime_r(&t, &tm);
	g_gmtoff = tm.tm_gmtoff;
	g_driver_pid = getpid();
	pid = fork();
	if (pid < 0) {
		close(pfd[0]);
		close(pfd[1]);
		return -1;
	}
	if (pid == 0) {
		int fd, maxfd = getdtablesize(), sig;
		struct sigaction sa;
		sigset_t none;

		for (fd = 0; fd < maxfd; fd++)
			if (fd != pfd[0] && fd != g_cfg.log_fd && fd != g_cfg.report_fd && fd != g_cfg.lock_fd)
				close(fd);
		setsid();	/* not in the driver's process group / terminal: Ctrl-C in the terminal does not hit the guard process */
		memset(&sa, 0, sizeof(sa));
		sigemptyset(&sa.sa_mask);
		for (sig = 1; sig < NSIG; sig++) {
			if (sig == SIGKILL || sig == SIGSTOP)
				continue;
			sa.sa_handler = (sig == SIGINT || sig == SIGHUP || sig == SIGPIPE ||
					 sig == SIGUSR1 || sig == SIGUSR2 || sig == SIGTSTP ||
					 sig == SIGTTIN || sig == SIGTTOU || sig == SIGCHLD) ?
					SIG_IGN : SIG_DFL;
			sigaction(sig, &sa, NULL);	/* none of the handlers the driver installed (crash reporters included) are carried over */
		}
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		guard_retitle();
		guard_main(&g_cfg, pfd[0]);
		_exit(0);
	}
	close(pfd[0]);
	fcntl(pfd[1], F_SETFD, FD_CLOEXEC);
	fcntl(pfd[1], F_SETFL, O_NONBLOCK);
	if (gpid)
		*gpid = pid;
	return pfd[1];
}
