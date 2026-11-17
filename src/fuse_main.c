/*
 * fuse_main.c -- FUSE 2.x fuse_main / fuse_parse_cmdline / fuse_loop_mt
 *
 * For file systems whose main() is "one call to fuse_main and done" (typical third-party ext,
 * exFAT and BitLocker drivers). The behaviour is an independent implementation of the semantics
 * described in the public FUSE 2.x documentation; the order of going to the background was
 * redesigned around nfuse's constraints (see the fuse_main comment in include/fuse.h):
 *
 *   parent process                         child process (the final background process)
 *   parse command line, fix up arguments
 *   fork ---------------------------------> fuse_mount, fuse_new (init callback, mount_nfs)
 *   wait on the result pipe                 signal handlers; setsid, chdir("/"), stdio -> /dev/null
 *                                           write 'O' to the result pipe --> parent _exit(0)
 *                                           device watcher thread; fuse_loop (the guard process is forked here)
 *                                           unmount, destroy, return to the file system's main
 *   Mount failed: the child itself returns the error code to the file system, which prints the
 *   error and exits; the parent sees end-of-file on the pipe, waits for the child and calls
 *   _exit with its exit code.
 *
 * Why not "mount first, then daemon()" as a file system calling fuse_new directly would do: by
 * convention FUSE 2.x file systems start threads in the init callback (a statistics thread, for
 * example), so init has to be called in the final background process; nfuse calls init inside
 * fuse_new, before mounting, hence fork first and fuse_new second. The CoreFoundation classes
 * the child needs are initialised before the fork (nfuse_fork_warmup): when the caller has
 * already started threads (some ext drivers do), the objc runtime of macOS 27 does not allow
 * classes to be initialised in a forked child.
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#define FUSE_USE_VERSION 26
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "fuse.h"
#include "nfuse.h"
#include "devwatch.h"
#include "names.h"
#include "fuse_main_int.h"
#include "report.h"

/* ---------- fuse_parse_cmdline ---------- */

enum {
	K_HELP,
	K_HELP_NOHEADER,
	K_VERSION,
	K_FOREGROUND,
	K_SINGLE,
	K_DEBUG,
};

struct cmdline {
	char	*mountpoint;
	int	foreground;
	int	single;
	int	help;		/* 1 = -h / --help, 2 = -ho */
	int	version;
	int	extra_nonopt;	/* surplus non-option arguments */
};

static const struct fuse_opt cmd_opts[] = {
	FUSE_OPT_KEY("-h", K_HELP),
	FUSE_OPT_KEY("--help", K_HELP),
	FUSE_OPT_KEY("-ho", K_HELP_NOHEADER),
	FUSE_OPT_KEY("-V", K_VERSION),
	FUSE_OPT_KEY("--version", K_VERSION),
	FUSE_OPT_KEY("-f", K_FOREGROUND),
	FUSE_OPT_KEY("-s", K_SINGLE),
	FUSE_OPT_KEY("-d", K_DEBUG),
	FUSE_OPT_KEY("debug", K_DEBUG),
	FUSE_OPT_END
};

static int cmd_proc(void *data, const char *arg, int key, struct fuse_args *outargs)
{
	struct cmdline *c = data;

	(void)outargs;
	switch (key) {
	case K_HELP:
		c->help = 1;
		return 0;
	case K_HELP_NOHEADER:
		c->help = 2;
		return 0;
	case K_VERSION:
		c->version = 1;
		return 0;
	case K_FOREGROUND:
		c->foreground = 1;
		return 0;
	case K_SINGLE:
		c->single = 1;
		return 0;
	case K_DEBUG:
		c->foreground = 1;
		return 1;	/* left for fuse_mount / fuse_new (they understand -d and -o debug) */
	case FUSE_OPT_KEY_NONOPT:
		if (!c->mountpoint) {
			c->mountpoint = strdup(arg);
			return c->mountpoint ? 0 : -1;
		}
		fprintf(stderr, "fuse: unexpected argument: %s\n", arg);
		c->extra_nonopt = 1;
		return -1;
	}
	return 1;
}

static int parse_cmdline(struct fuse_args *args, struct cmdline *c)
{
	memset(c, 0, sizeof(*c));
	if (fuse_opt_parse(args, c, cmd_opts, cmd_proc) == -1) {
		free(c->mountpoint);
		c->mountpoint = NULL;
		return -1;
	}
	return 0;
}

int fuse_parse_cmdline(struct fuse_args *args, char **mountpoint, int *multithreaded,
		       int *foreground)
{
	struct cmdline c;

	if (parse_cmdline(args, &c))
		return -1;
	if (c.help || c.version) {
		free(c.mountpoint);
		c.mountpoint = NULL;
	}
	if (mountpoint)
		*mountpoint = c.mountpoint;
	else
		free(c.mountpoint);
	if (multithreaded)
		*multithreaded = !c.single;
	if (foreground)
		*foreground = c.foreground;
	return 0;
}

int fuse_loop_mt(struct fuse *f)
{
	return fuse_loop(f);
}

static void print_help(const char *prog, int header)
{
	if (header)
		fprintf(stderr, "usage: %s mountpoint [options]\n\n", prog ? prog : "fuse");
	fprintf(stderr,
		"general options:\n"
		"    -o opt,[opt...]        mount options\n"
		"    -h   --help            print help\n"
		"    -V   --version         print version\n"
		"\n"
		"FUSE options (nfuse: in-process NFSv4.0 server + the system mount_nfs):\n"
		"    -d   -o debug          debug output (implies foreground)\n"
		"    -f                     run in the foreground\n"
		"    -s                     single-threaded (nfuse callbacks are serial anyway; accepted and ignored)\n"
		"    -o ro / rw             read-only / read-write\n"
		"    -o volname=NAME        volume name (default: last component of the mount point)\n"
		"    -o nobrowse            hide from Finder\n"
		"    -o nosuid,nodev,noexec passed on to mount_nfs\n"
		"    -o nfuse_case=sensitive|insensitive   default sensitive\n"
		"    -o nfuse_xattr=appledouble|adfile     default appledouble\n"
		"    allow_other, default_permissions, local, fsname=, subtype= and similar options of other macOS FUSE implementations are accepted and ignored\n");
}

/* ---------- Argument fix-ups ---------- */

/* Walk every -o item in args (split and unescaped); stops when fn returns non-zero */
static int each_opt(const struct fuse_args *args, int (*fn)(void *, const char *), void *arg)
{
	int i;

	for (i = 1; i < args->argc; i++) {
		const char *a = args->argv[i], *s;
		char *item;
		size_t k = 0;

		if (!a || strncmp(a, "-o", 2) != 0)
			continue;
		s = a[2] ? a + 2 : (i + 1 < args->argc ? args->argv[++i] : NULL);
		if (!s)
			break;
		item = malloc(strlen(s) + 1);
		if (!item)
			return -1;
		for (;; s++) {
			if (*s == '\\' && s[1]) {
				item[k++] = *++s;
				continue;
			}
			if (*s == ',' || *s == 0) {
				item[k] = 0;
				if (k && fn(arg, item)) {
					free(item);
					return 1;
				}
				k = 0;
				if (*s == 0)
					break;
				continue;
			}
			item[k++] = *s;
		}
		free(item);
	}
	return 0;
}

static int find_volname(void *arg, const char *item)
{
	int *found = arg;

	if (strncmp(item, "volname=", 8) == 0 && item[8]) {
		*found = 1;
		return 1;
	}
	return 0;
}

int nfuse_main_prepare_args(struct fuse_args *args, const char *mountpoint)
{
	const char *env = getenv("NFUSE_OPTS");
	int has_vol = 0;

	/* Defaults go first: the same option on the command line comes later, and the last one wins */
	if (fuse_opt_insert_arg(args, 1, "-onfuse_case=sensitive,nfuse_xattr=appledouble"))
		return -1;
	if (env && *env) {
		char *o = malloc(strlen(env) + 3);

		if (!o)
			return -1;
		sprintf(o, "-o%s", env);
		if (fuse_opt_add_arg(args, o)) {
			free(o);
			return -1;
		}
		free(o);
	}
	if (each_opt(args, find_volname, &has_vol) < 0)
		return -1;
	if (!has_vol && mountpoint) {
		/* Volume name = last component of the mount point (trailing '/' removed); commas and backslashes must be escaped */
		char base[PATH_MAX], *opt = NULL;
		size_t n = strlen(mountpoint);
		const char *b;

		while (n > 1 && mountpoint[n - 1] == '/')
			n--;
		snprintf(base, sizeof(base), "%.*s", (int)n, mountpoint);
		b = strrchr(base, '/');
		b = b ? b + 1 : base;
		if (*b) {
			char *kv = malloc(strlen(b) + 9);

			if (!kv)
				return -1;
			sprintf(kv, "volname=%s", b);
			if (fuse_opt_add_opt_escaped(&opt, kv) == 0) {
				char *a = malloc(strlen(opt) + 3);

				if (a) {
					sprintf(a, "-o%s", opt);
					if (fuse_opt_add_arg(args, a)) {
						free(a);
						free(opt);
						free(kv);
						return -1;
					}
					free(a);
				}
			}
			free(opt);
			free(kv);
		}
	}
	/* nosuid / nodev / noexec are passed on to mount_nfs by the library itself (fuse_lib.c chan_opt / mount_args; also applies without fuse_main) */
	return 0;
}

/* ---------- Going to the background ---------- */

static void default_detach(void)
{
	int fd;

	setsid();
	if (chdir("/") != 0) {
		/* does not affect serving */
	}
	fd = open("/dev/null", O_RDWR);
	if (fd >= 0) {
		dup2(fd, 0);
		dup2(fd, 1);
		dup2(fd, 2);
		if (fd > 2)
			close(fd);
	}
}

static const struct nfuse_main_hooks default_hooks = {
	.mount = fuse_mount,
	.fnew = fuse_new,
	.set_signals = fuse_set_signal_handlers,
	.remove_signals = fuse_remove_signal_handlers,
	.session = fuse_get_session,
	.loop = fuse_loop,
	.unmount = fuse_unmount,
	.destroy = fuse_destroy,
	.detach = default_detach,
	.mount_info = nfuse_get_mount_info,
	.end_reason = nfuse_end_reason,
};

/* Parent: wait for the child to report the result; does not return */
static void parent_wait(pid_t child, int rfd)
{
	char c = 0;
	ssize_t n;
	int st = 0;

	do
		n = read(rfd, &c, 1);
	while (n < 0 && errno == EINTR);
	if (n == 1 && c == 'O')
		_exit(0);
	/* The child did not mount (returned to the file system, which printed the error and exited; _exit(24) when the handshake got stuck) or was killed: exit with its outcome */
	while (waitpid(child, &st, 0) < 0 && errno == EINTR)
		;
	if (WIFEXITED(st))
		_exit(WEXITSTATUS(st) ? WEXITSTATUS(st) : 4);
	if (WIFSIGNALED(st))
		_exit(128 + WTERMSIG(st));
	_exit(4);
}

/* ---------- Telemetry (NFUSE_REPORT, see report.h) ---------- */

static double mono_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void report_start(int argc, char *argv[], int foreground, int watch)
{
	char cmd[700], esc[1500], osv[32] = "", arch[32] = "";
	size_t n = 0, l = sizeof(osv);
	int i;

	cmd[0] = 0;
	for (i = 0; argv && i < argc && argv[i] && n < sizeof(cmd) - 1; i++)
		n += (size_t)snprintf(cmd + n, sizeof(cmd) - n, "%s%s", i ? " " : "", argv[i]);
	if (n >= sizeof(cmd))
		n = sizeof(cmd) - 1;
	sysctlbyname("kern.osproductversion", osv, &l, NULL, 0);
	osv[sizeof(osv) - 1] = 0;
	l = sizeof(arch);
	if (sysctlbyname("hw.machine", arch, &l, NULL, 0) != 0)
		snprintf(arch, sizeof(arch), "?");
	arch[sizeof(arch) - 1] = 0;
	nfuse_report("start", "\"version\":\"nfuse-fuse-%d.%d\",\"os\":\"%s\",\"arch\":\"%s\",\"uid\":%d,\"euid\":%d,"
		     "\"foreground\":%d,\"watch_dev\":%d,\"cmd\":\"%s\"",
		     FUSE_MAJOR_VERSION, FUSE_MINOR_VERSION, osv, arch, (int)getuid(), (int)geteuid(),
		     foreground, watch, nfuse_report_esc(cmd, esc, sizeof(esc)));
}

static void report_mount(const struct nfuse_main_hooks *h, struct fuse *f, int watch)
{
	struct nfuse_mount_info mi;
	int iostat = 0;

	if (nfuse_report_fd() < 0 || !h->mount_info || h->mount_info(f, &mi, sizeof(mi)) != 0)
		return;
	if (watch) {
		const char *d = getenv("NFUSE_WATCH_DEV");
		const char *b = d ? strrchr(d, '/') : NULL;

		b = b ? b + 1 : d;
		if (b && b[0] == 'r' && strncmp(b + 1, "disk", 4) == 0)
			b++;
		(void)nfuse_devwatch_iostat(b, &iostat);
	}
	/* fsync is up to the file system itself (fs); flush_ms does not apply (-1) */
	nfuse_report("nfuse_mount", "\"case\":\"%s\",\"case_downgraded\":%d,\"case_conflicts\":%u,\"case_scan_complete\":%d,"
		     "\"xattr\":\"%s\",\"transport\":\"%s\",\"mount_s\":%.3f,\"browse\":%d,\"ro\":%d,\"rwsize\":%u,"
		     "\"fsync\":\"fs\",\"flush_ms\":-1,\"watchdog_s\":%.0f,\"watchdog_max_s\":%.0f,\"guard\":%d,\"guard_s\":%.0f,"
		     "\"tcp_guard\":%d,\"watch_dev\":%d,\"iostat\":%d",
		     mi.case_mode, mi.case_downgraded, mi.case_conflicts, mi.case_scan_complete, mi.xattr_mode,
		     mi.transport, mi.mount_s, mi.browse, mi.readonly, mi.rwsize, mi.watchdog_s, mi.watchdog_max_s,
		     mi.guard_on, mi.guard_s, mi.tcp_guard, watch, iostat);
}

static double g_t0;

static void report_exit(int code, int ret, const char *reason)
{
	nfuse_report("exit", "\"code\":%d,\"ret\":%d,\"reason\":\"%s\",\"lasted_s\":%.1f", code, ret, reason,
		     g_t0 ? mono_s() - g_t0 : 0);
}

/* Watchdog callback (on the watchdog thread; only writes telemetry, returning 0 = use the library's default exit code 24) */
static int ext_watchdog(const char *why, const char *op, double secs)
{
	nfuse_report("watchdog", "\"why\":\"%s\",\"op\":\"%s\",\"secs\":%.1f,\"io_done\":%llu", why, op, secs,
		     (unsigned long long)nfuse_devwatch_progress());
	return 0;
}

static int ext_report_fd(void)
{
	return nfuse_report_fd();
}

int nfuse_main_hooked(int argc, char *argv[], const struct fuse_operations *op,
		      size_t op_size, void *user_data, const struct nfuse_main_hooks *hooks)
{
	struct nfuse_main_hooks hk = default_hooks;
	const struct nfuse_main_hooks *h = &hk;
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	struct fuse_chan *ch = NULL;
	struct fuse *f = NULL;
	struct cmdline c;
	const char *reason = "";
	int pfd[2] = { -1, -1 };
	int rc = 0, watch = 0, report = 0;

	if (!op)
		return 1;
	if (hooks) {
		/* Tests replace only some of the steps: missing ones use the default */
#define HK(x)	if (hooks->x) hk.x = hooks->x
		HK(mount); HK(fnew); HK(set_signals); HK(remove_signals); HK(session);
		HK(loop); HK(unmount); HK(destroy); HK(detach); HK(mount_info); HK(end_reason);
#undef HK
	}
	if (parse_cmdline(&args, &c)) {
		fuse_opt_free_args(&args);
		return 1;
	}
	if (c.help) {
		print_help(argv && argc > 0 ? argv[0] : NULL, c.help == 1);
		goto out;
	}
	if (c.version) {
		fprintf(stderr, "FUSE library version: %d.%d (nfuse)\n", FUSE_MAJOR_VERSION,
			FUSE_MINOR_VERSION);
		goto out;
	}
	if (!c.mountpoint) {
		fprintf(stderr, "fuse: no mount point given\n");
		rc = 2;
		goto out;
	}
	if (nfuse_main_prepare_args(&args, c.mountpoint)) {
		rc = 1;
		goto out;
	}
	if (c.single) {
		/* Accepted and ignored: nfuse callbacks are serial anyway */
	}

	if (!c.foreground) {
		pid_t pid;

		fflush(NULL);	/* output still sitting in buffers must not be written once by each of parent and child */
		/*
		 * The child (name normalisation in fuse_new, the IOKit queries of the device watcher) needs
		 * CoreFoundation: the objc classes behind it have to be initialised here, before the fork.
		 * When the caller started threads before fuse_main (e.g. an ext driver reading bitmaps on
		 * several threads), the objc runtime of macOS 27 aborts outright the first time these classes
		 * are initialised in a forked child (see nfuse_fork_warmup in names.c).
		 * Pure CF interfaces only; no IOKit / mach ports, no threads.
		 */
		nfuse_fork_warmup();
		if (pipe(pfd) < 0) {
			perror("fuse: pipe");
			rc = 5;
			goto out;
		}
		pid = fork();
		if (pid < 0) {
			perror("fuse: fork");
			close(pfd[0]);
			close(pfd[1]);
			rc = 5;
			goto out;
		}
		if (pid > 0) {
			close(pfd[1]);
			parent_wait(pid, pfd[0]);	/* does not return */
		}
		close(pfd[0]);
		pfd[0] = -1;
		fcntl(pfd[1], F_SETFD, FD_CLOEXEC);	/* mount_nfs must not inherit it */
	}

	/* Everything below runs in the final process (the background child, or this process when in the foreground) */
	g_t0 = mono_s();
	report = nfuse_report_open(argv && argc > 0 ? argv[0] : NULL) == 0;
	watch = nfuse_devwatch_wanted();
	if (watch && nfuse_devwatch_prepare() != 0) {
		fprintf(stderr, "nfuse: device watcher: %s is not usable, not watching\n",
			getenv("NFUSE_WATCH_DEV") ? getenv("NFUSE_WATCH_DEV") : "?");
		watch = 0;
	}
	if (report)
		report_start(argc, argv, c.foreground, watch);
	{
		/*
		 * Extension callbacks: watchdog (writes telemetry), the telemetry descriptor for the guard
		 * process, device I/O progress. The rest stay NULL = the library's generic approach (neither
		 * case folding nor offset-based attribute I/O is registered). Nothing is registered when none
		 * of them applies.
		 */
		struct nfuse_ext_ops ext;

		memset(&ext, 0, sizeof(ext));
		if (watch)
			ext.progress = nfuse_devwatch_progress;
		if (report) {
			ext.watchdog = ext_watchdog;
			ext.report_fd = ext_report_fd;
		}
		if (watch || report)
			nfuse_set_ext_ops(&ext, sizeof(ext));
	}
	/* The device watcher starts before the mount handshake: progress is reported during the handshake too (used to detect a stuck handshake), and unplugging is detected */
	if (watch && nfuse_devwatch_start(NULL))
		watch = 0;

	ch = h->mount(c.mountpoint, &args);
	if (!ch) {
		fprintf(stderr, "fuse: mount point %s is not usable\n", c.mountpoint);
		nfuse_report("mount_fail", "\"stage\":\"fuse_mount\",\"ret\":3,\"ms\":%.0f", (mono_s() - g_t0) * 1000);
		rc = 3;
		reason = "mount_failed";
		goto stop_watch;
	}
	/*
	 * fuse_new: init -> mount_nfs. When the mount fails the library calls destroy itself to clean
	 * up (if init was called). When the handshake gets stuck / mount_nfs times out the library does
	 * not return: it takes over the connection and answers IO so that mount_nfs fails on its own
	 * (it is not killed and nothing is force-unmounted), writes the watchdog telemetry and calls
	 * _exit(24); the parent passes the 24 through to the caller.
	 */
	f = h->fnew(ch, &args, op, op_size, user_data);
	if (!f) {
		/* fuse_new already printed / logged; the listening socket at the mount point etc. are cleaned up by fuse_unmount */
		h->unmount(c.mountpoint, ch);
		ch = NULL;
		nfuse_report("mount_fail", "\"stage\":\"fuse_new\",\"ret\":4,\"ms\":%.0f", (mono_s() - g_t0) * 1000);
		rc = 4;
		reason = "mount_failed";
		goto stop_watch;
	}
	report_mount(h, f, watch);
	if (h->set_signals(h->session(f))) {
		fprintf(stderr, "fuse: failed to install signal handlers\n");
		rc = 6;
		reason = "signal_setup_failed";
		goto teardown;
	}
	if (!c.foreground) {
		ssize_t n;

		h->detach();
		do
			n = write(pfd[1], "O", 1);
		while (n < 0 && errno == EINTR);
		close(pfd[1]);
		pfd[1] = -1;
		if (n != 1) {
			/* The parent is already gone: the caller gave up waiting and nobody would look after the mount -- unmount and exit */
			syslog(LOG_WARNING, "nfuse: caller gave up before mount completed; unmounting %s",
			       c.mountpoint);
			rc = 5;
			reason = "caller_gone";
			goto teardown_sig;
		}
	}
	/* The guard process is forked at the start of fuse_loop (in the library); the watchdog starts with fuse_loop */
	if (h->loop(f))
		rc = 7;
	reason = h->end_reason ? h->end_reason(f) : "";
	if (!*reason)
		reason = rc ? "loop_error" : "unmounted";
teardown_sig:
	h->remove_signals(h->session(f));
teardown:
	if (watch)
		nfuse_devwatch_stop();
	watch = 0;
	h->unmount(c.mountpoint, ch);
	ch = NULL;
	h->destroy(f);
	f = NULL;
stop_watch:
	if (watch)
		nfuse_devwatch_stop();
	if (report)
		report_exit(strcmp(reason, "device_lost") == 0 ? NFUSE_EXIT_DEVICE_LOST : rc, rc, reason);
out:
	if (pfd[1] >= 0)
		close(pfd[1]);
	free(c.mountpoint);
	fuse_opt_free_args(&args);
	return rc;
}

int fuse_main_real(int argc, char *argv[], const struct fuse_operations *op,
		   size_t op_size, void *user_data)
{
	return nfuse_main_hooked(argc, argv, op, op_size, user_data, NULL);
}

/*
 * A real function of the same name: in the header fuse_main is a macro (it adds the size of the
 * callback table) and the sources never need this function; but configure scripts decide whether
 * a FUSE library is present by checking whether the library has a fuse_main symbol (e.g.
 * AC_CHECK_LIB(fuse, fuse_main)), and without it such file systems cannot be configured.
 */
#undef fuse_main
int fuse_main(int argc, char *argv[], const struct fuse_operations *op, void *user_data);
int fuse_main(int argc, char *argv[], const struct fuse_operations *op, void *user_data)
{
	return nfuse_main_hooked(argc, argv, op, sizeof(*op), user_data, NULL);
}
