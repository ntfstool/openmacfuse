/*
 * fuse_lib.c -- independent implementation of the FUSE 2.x high-level interface (entry point of libnfuse.a)
 *
 * Typical call sequence of a FUSE 2.x file system:
 *   fuse_mount -> fuse_new -> fuse_set_signal_handlers -> [daemon()] ->
 *   fuse_loop -> fuse_unmount -> fuse_destroy
 * On errors it may be fuse_destroy -> fuse_unmount; both orders have to be handled.
 *
 * What each step does here:
 *   fuse_mount  parses the -o options; creates a 0700 temporary directory and listens on a
 *               local unix socket inside it (or on a random 127.0.0.1 port when the
 *               environment asks for TCP).
 *   fuse_new    copies the callback table, calls init, builds the node-table backend; starts
 *               the server thread and runs mount_nfs to mount (the handshake needs the
 *               callbacks, which is why it happens here); if the unix socket cannot be mounted
 *               it retries over TCP automatically.
 *               Before returning, the server thread is parked at a request boundary -- the
 *               file system may daemon() next and threads do not survive a fork; the
 *               connection and the listening socket stay open, requests queue in the socket.
 *   fuse_loop   restarts the server thread and waits for "exit signal received" or
 *               "connection closed and no longer in the mount table".
 *   fuse_unmount unmounts if still mounted (the server thread is still running, so requests
 *               issued during the unmount get answered), then stops the threads, closes the
 *               sockets and removes the temporary directory.
 *   fuse_destroy calls the file system's destroy, frees the backend, writes the statistics log.
 *
 * Tunable environment variables (all for testing / troubleshooting only; the defaults are the
 * production behaviour):
 *   NFUSE_TRANSPORT=unix|tcp|auto  transport (default auto: unix socket from macOS 26 on,
 *                                  127.0.0.1 TCP on older systems; falls back to TCP
 *                                  automatically when unix cannot be mounted. See transport_auto)
 *   NFUSE_LOG=<file>           append the log to a file (default: none; stderr with -o debug)
 *   NFUSE_VERBOSE=1            log every COMPOUND
 *   NFUSE_MAX_NODES=<n>        node table limit (default 32768)
 *   NFUSE_ATTR_TTL_MS=<n>      attribute cache lifetime (default 1000)
 *   NFUSE_NO_RDONLY=1          do not add rdonly when mounting (to verify that the server itself returns ROFS)
 *   NFUSE_NAMEDATTR=2          named-attribute experiment: additionally report one read-only
 *                              attribute org.nfuse.probe on every object
 *                              (named attributes are on by default and controlled by the mount option nfuse_xattr)
 *   NFUSE_MOUNT_EXTRA=<opts>   appended to the mount_nfs -o options
 *   NFUSE_NO_DRC=1                 turn off the duplicate request cache (for tests: proves the test itself is effective)
 *   NFUSE_TEST_DROP=<op,...>       test hook: each listed occurrence drops one reply of the COMPOUND containing that operation
 *                                  (e.g. RENAME,REMOVE,OPEN_EXCL; OPEN_EXCL = OPEN(EXCLUSIVE4)),
 *                                  forcing the client to retransmit, to verify the DRC
 *   NFUSE_TEST_DROP_MODE=close|silent  close the connection after dropping the reply (default; the client reconnects and retransmits) / just do not reply
 *   NFUSE_TEST_RAWNAMES=1          names pass through unchanged (no NFC / NFD or case-equivalent lookup); only for building test volumes
 *   NFUSE_TEST_HOOKS=1             simulate a server restart on SIGUSR2 (all state cleared, state_boot replaced,
 *                                  file handles remain valid), to verify that a client presenting an old stateid gets the error code the RFC specifies
 *
 * Mount options (-o, see include/nfuse.h): nfuse_xattr=streams|appledouble, nfuse_adclean=1|0,
 * nfuse_case=insensitive|sensitive, nfuse_case_scan_ms=<ms>, nfuse_nfc=1|0.
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#define FUSE_USE_VERSION 26
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/sysctl.h>
#include <sys/un.h>
#include <sys/time.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/task_info.h>
#include <libproc.h>
#include <sys/proc_info.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "fuse.h"
#include "nfuse.h"
#include "backend_fuse.h"
#include "mounter.h"
#include "names.h"
#include "adfile.h"
#include "nfs4_ops.h"
#include "nfs4_state.h"
#include "server.h"
#include "watchdog.h"

#define NFUSE_DEFAULT_RWSIZE	262144
#define NFUSE_SOCK_MAX		103	/* sun_path is 104 bytes including the terminating NUL */

struct fuse_session {
	struct fuse	*f;
};

struct fuse_chan {
	char		mnt[PATH_MAX];		/* mount point (symlinks resolved, same as in the mount table) */
	char		volname[256];		/* export name = volume name shown in Finder */
	uint32_t	rwsize;
	int		debug;
	int		ro;			/* read-only mount (-o ro) */
	int		want_tcp;
	int		lfd;			/* listening socket */
	int		is_unix;
	uint16_t	port;
	char		sockdir[PATH_MAX];
	char		sockpath[PATH_MAX];
	int		mounted;		/* we mounted it and have not yet confirmed it is unmounted */
	char		from[PATH_MAX + 300];	/* mount source (the mount_nfs spec); identifies which mount table entry is ours */
	struct fuse	*f;
	/* Mac semantics options */
	int		xattr_mode;		/* nfuse_xattr: 0 = appledouble, 1 = streams (named attributes, experimental), 2 = adfile (default) */
	int		ad_clean;		/* nfuse_adclean */
	int		case_ci;		/* nfuse_case: 1 = insensitive */
	uint32_t	case_scan_ms;		/* nfuse_case_scan_ms */
	int		nfc;			/* nfuse_nfc */
	int		nobrowse;		/* nobrowse is passed to mount_nfs only when given */
	int		nosuid, nodev, noexec;	/* passed to mount_nfs only when given (suid / dev / exec cancel them) */
	int		case_downgraded;
};

struct fuse {
	struct fuse_operations	op;
	struct fuse_chan	*ch;
	struct fuse_session	se;
	struct fuse_context	ctx;
	struct nfuse_server	srv;
	struct nfuse_tp		tp;
	int			tp_ok;
	int			wake[2];
	volatile sig_atomic_t	exited;
	volatile sig_atomic_t	lost;		/* nfuse_device_lost was called: the device is gone, force-unmount */
	struct timeval		t_lost;
	int			destroyed;
	struct timeval		t_mount;
	size_t			pool_bytes;	/* bytes used by the buffer pool (recorded before shutdown) */
	struct nfuse_case_scan	case_scan;	/* result of the pre-mount case scan */
	int			case_scanned;
	/* Watchdog / guard process */
	pthread_t		wd_th;
	int			wd_running;
	volatile int		wd_stop;
	uint64_t		wd_ns;		/* how long without progress counts as hung (0 = off) */
	uint64_t		wd_max_ns;	/* absolute limit for a single request (0 = none) */
	volatile int		loop_running;	/* fuse_loop is running: check its heartbeat */
	volatile uint64_t	loop_beat;
	volatile uint64_t	extra_t0;	/* something the library itself is doing that may hang in the backend (destroy during unmount etc.), 0 = nothing */
	const char		*extra_name;
	int			guard_fd;	/* write end of the guard process heartbeat pipe, -1 = none */
	pid_t			guard_pid;
	uint32_t		guard_ms;
	int			guard_on;
	uint64_t		wd_fired_ns;
	uint64_t		mount_stuck_ns;	/* how long a request may be stuck during the mount handshake before the handshake is failed (default 5 seconds) */
	volatile int		mount_failed_hung; /* handshake stuck; connections already taken over so that mount_nfs fails on its own */
	int			inited;		/* init was called (destroy must then be called for clean-up even when the mount fails) */
	const char		*end_reason;	/* why fuse_loop returned (nfuse_end_reason) */
	double			mount_s;	/* duration of the mount_nfs handshake */
	uint64_t		wd_age_max_ns;	/* age of the oldest in-flight request observed (basis for the watchdog headroom) */
	uint64_t		wd_quiet_max_ns;/* longest stretch without device progress while a request was in flight */
	/* Local security (with 127.0.0.1 TCP) */
	int			tcp_guard;	/* on: connections go through the accept check, untrusted connections may only issue a limited set of requests */
	volatile int		mounting;	/* mount_nfs is running (mount handshake in progress) */
	int			trusted_given;	/* one mount-handshake connection has already been recognised */
	uint64_t		guard_rejected;	/* rejected connections */
	uint64_t		guard_untrusted;/* connections accepted but untrusted */
	double			guard_scan_ms_max;
};

static struct fuse *g_fuse;
static struct fuse_context g_dummy_ctx;

/* Watchdog (defined further down) */
static void wd_config(struct fuse *f);
static void wd_start(struct fuse *f);
static void wd_stop(struct fuse *f);
static void tp_setup_guard(struct fuse *f);
static void mount_stuck(struct fuse *f, uint64_t since, const char *why);
static struct nfuse_ext_ops g_ext;
static int g_ext_set;
static FILE *g_logf;
static volatile int g_log_sync;		/* non-zero: fsync the log after every line (during the handshake, after the watchdog fired, NFUSE_LOG_SYNC=1) */
static int g_log_sync_env;
static int g_log_stderr;
static pthread_mutex_t g_log_mu = PTHREAD_MUTEX_INITIALIZER;

/* ---------- Logging ---------- */

static void nfuse_log(const char *fmt, ...)
{
	va_list ap;
	char line[1024];
	struct timeval tv;
	struct tm tm;
	time_t t;

	if (!g_logf && !g_log_stderr)
		return;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	gettimeofday(&tv, NULL);
	t = tv.tv_sec;
	localtime_r(&t, &tm);
	pthread_mutex_lock(&g_log_mu);
	if (g_logf) {
		fprintf(g_logf, "%02d:%02d:%02d.%03d [nfuse %d] %s\n", tm.tm_hour,
			tm.tm_min, tm.tm_sec, (int)(tv.tv_usec / 1000), (int)getpid(),
			line);
		fflush(g_logf);
		/*
		 * Flush to disk at once: log lines from critical moments (mount handshake, watchdog, unmount)
		 * must reach the disk before a possible kernel panic, otherwise only the first part that had
		 * already left the cache survives the crash. With NFUSE_LOG_SYNC=1 every line is synced
		 * (for troubleshooting).
		 */
		if (g_log_sync)
			fsync(fileno(g_logf));
	}
	if (g_log_stderr)
		fprintf(stderr, "[nfuse] %s\n", line);
	pthread_mutex_unlock(&g_log_mu);
}

static long env_long(const char *name, long def)
{
	const char *v = getenv(name);

	return (v && *v) ? strtol(v, NULL, 10) : def;
}

static void log_open(int debug)
{
	const char *p = getenv("NFUSE_LOG");

	if (p && *p && !g_logf)
		g_logf = fopen(p, "a");
	g_log_stderr = debug;
	g_log_sync_env = env_long("NFUSE_LOG_SYNC", 0) != 0;
	g_log_sync = 1;		/* sync every line until the mount handshake completes; back to NFUSE_LOG_SYNC once fuse_loop starts serving */
}


/* ---------- Option parsing ---------- */

/* Handle a single -o item (key or key=val) */
static void chan_opt(struct fuse_chan *ch, const char *kv)
{
	const char *eq = strchr(kv, '=');
	size_t kl = eq ? (size_t)(eq - kv) : strlen(kv);
	const char *val = eq ? eq + 1 : "";

#define KEY(s)	(kl == strlen(s) && strncmp(kv, s, kl) == 0)
	if (KEY("volname")) {
		snprintf(ch->volname, sizeof(ch->volname), "%s", val);
	} else if (KEY("rwsize")) {
		long v = strtol(val, NULL, 10);
		if (v >= 4096 && v <= 1048576)
			ch->rwsize = (uint32_t)v;
	} else if (KEY("debug")) {
		ch->debug = 1;
	} else if (KEY("ro") || KEY("rdonly")) {
		ch->ro = 1;
	} else if (KEY("rw")) {
		ch->ro = 0;
	} else if (KEY("nfuse_xattr")) {
		if (strcmp(val, "adfile") == 0)
			ch->xattr_mode = 2;
		else if (strcmp(val, "streams") == 0)
			ch->xattr_mode = 1;
		else if (strcmp(val, "appledouble") == 0)
			ch->xattr_mode = 0;
		else
			nfuse_log("nfuse_xattr=%s is not recognised (adfile | appledouble | streams), using the default adfile", val);
	} else if (KEY("nfuse_adclean")) {
		ch->ad_clean = strtol(val, NULL, 10) != 0;
	} else if (KEY("nfuse_case")) {
		if (strcmp(val, "insensitive") == 0)
			ch->case_ci = 1;
		else if (strcmp(val, "sensitive") == 0)
			ch->case_ci = 0;
		else
			nfuse_log("nfuse_case=%s is not recognised (insensitive | sensitive), using the default insensitive", val);
	} else if (KEY("nfuse_case_scan_ms")) {
		long v = strtol(val, NULL, 10);
		ch->case_scan_ms = v < 0 ? 0 : v > 60000 ? 60000 : (uint32_t)v;
	} else if (KEY("nfuse_nfc")) {
		ch->nfc = strtol(val, NULL, 10) != 0;
	} else if (KEY("nobrowse")) {
		ch->nobrowse = 1;
	} else if (KEY("browse")) {
		ch->nobrowse = 0;
	} else if (KEY("nosuid") || KEY("suid")) {
		ch->nosuid = kv[0] == 'n';
	} else if (KEY("nodev") || KEY("dev")) {
		ch->nodev = kv[0] == 'n';
	} else if (KEY("noexec") || KEY("exec")) {
		ch->noexec = kv[0] == 'n';
	} else if (KEY("local") || KEY("allow_other") || KEY("allow_root") ||
		   KEY("fsname") || KEY("nonempty") || KEY("async") ||
		   KEY("kernel_cache") || KEY("noatime") || KEY("atime") ||
		   KEY("relatime") || KEY("default_permissions") ||
		   KEY("blkdev") || KEY("blksize") ||
		   KEY("use_ino") || KEY("attr_timeout") ||
		   KEY("entry_timeout") || KEY("noappledouble") ||
		   KEY("noapplexattr") || KEY("acl") || KEY("big_writes")) {
		/* Known; accepted and ignored (local: an NFS mount cannot be made a "local volume") */
	} else if (kl) {
		nfuse_log("ignoring unknown mount option: %.*s", (int)kl, kv);
	}
#undef KEY
}

/* Split a comma-separated option string (\, escapes supported) */
static void chan_opts(struct fuse_chan *ch, const char *s)
{
	char item[1024];
	size_t n = 0;

	for (;; s++) {
		if (*s == '\\' && s[1]) {
			if (n < sizeof(item) - 1)
				item[n++] = *++s;
			continue;
		}
		if (*s == ',' || *s == 0) {
			item[n] = 0;
			if (n)
				chan_opt(ch, item);
			n = 0;
			if (*s == 0)
				break;
			continue;
		}
		if (n < sizeof(item) - 1)
			item[n++] = *s;
	}
}

static void parse_args(struct fuse_args *args,
		       void (*fn)(void *, const char *), void *arg)
{
	int i;

	if (!args)
		return;
	for (i = 0; i < args->argc; i++) {
		const char *a = args->argv[i];

		if (!a)
			continue;
		if (strcmp(a, "-o") == 0 && i + 1 < args->argc)
			fn(arg, args->argv[++i]);
		else if (strncmp(a, "-o", 2) == 0)
			fn(arg, a + 2);
		else if (strcmp(a, "-d") == 0)
			fn(arg, "debug");
	}
}

static void chan_opts_cb(void *arg, const char *s)
{
	chan_opts(arg, s);
}

/* Of the fuse_new arguments only debug matters */
static void new_opts_cb(void *arg, const char *s)
{
	int *debug = arg;
	const char *p = strstr(s, "debug");

	if (p && (p == s || p[-1] == ',') && (p[5] == 0 || p[5] == ','))
		*debug = 1;
}

/* ---------- Listening ---------- */

/*
 * Remove socket directories left behind by an earlier abnormal exit (kill -9, crash): this
 * user's nfuse.* under base whose socket nobody listens on any more (connect refused) and
 * whose directory is more than a minute old.
 */
static void sweep_stale_sockdirs(const char *base, size_t bl)
{
	DIR *dp;
	struct dirent *de;
	char dir[PATH_MAX], sock[PATH_MAX];
	time_t now = time(NULL);

	snprintf(dir, sizeof(dir), "%.*s", (int)bl, base);
	dp = opendir(dir);
	if (!dp)
		return;
	while ((de = readdir(dp))) {
		struct stat st;
		struct sockaddr_un sun;
		int fd, r;

		if (strncmp(de->d_name, "nfuse.", 6) != 0)
			continue;
		snprintf(dir, sizeof(dir), "%.*s/%s", (int)bl, base, de->d_name);
		if (lstat(dir, &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid() ||
		    now - st.st_mtime < 60)
			continue;
		snprintf(sock, sizeof(sock), "%s/s", dir);
		if (strlen(sock) >= sizeof(sun.sun_path))
			continue;
		fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (fd < 0)
			continue;
		memset(&sun, 0, sizeof(sun));
		sun.sun_family = AF_UNIX;
		snprintf(sun.sun_path, sizeof(sun.sun_path), "%s", sock);
		r = connect(fd, (struct sockaddr *)&sun, SUN_LEN(&sun));
		close(fd);
		if (r == 0 || (errno != ECONNREFUSED && errno != ENOENT))
			continue;	/* still in use */
		unlink(sock);
		if (rmdir(dir) == 0)
			nfuse_log("removed socket directory %s left behind by an earlier abnormal exit", dir);
	}
	closedir(dp);
}

static int chan_listen_unix(struct fuse_chan *ch)
{
	const char *bases[2] = { getenv("TMPDIR"), "/tmp" };
	int i;

	for (i = 0; i < 2; i++) {
		char tmpl[PATH_MAX];
		size_t bl;

		if (!bases[i] || !*bases[i])
			continue;
		bl = strlen(bases[i]);
		while (bl > 1 && bases[i][bl - 1] == '/')
			bl--;
		/* <base>/nfuse.XXXXXX/s must be shorter than sun_path */
		if (bl + strlen("/nfuse.XXXXXX/s") > NFUSE_SOCK_MAX)
			continue;
		sweep_stale_sockdirs(bases[i], bl);
		snprintf(tmpl, sizeof(tmpl), "%.*s/nfuse.XXXXXX", (int)bl,
			 bases[i]);
		if (!mkdtemp(tmpl))	/* a directory created by mkdtemp is 0700 already */
			continue;
		chmod(tmpl, 0700);
		snprintf(ch->sockdir, sizeof(ch->sockdir), "%s", tmpl);
		snprintf(ch->sockpath, sizeof(ch->sockpath), "%s/s", tmpl);
		ch->lfd = nfuse_listen_unix(ch->sockpath);
		if (ch->lfd >= 0) {
			ch->is_unix = 1;
			return 0;
		}
		nfuse_log("listening on unix socket %s failed: %s", ch->sockpath,
			 strerror(errno));
		rmdir(ch->sockdir);
		ch->sockdir[0] = ch->sockpath[0] = 0;
	}
	return -1;
}

static int chan_listen_tcp(struct fuse_chan *ch)
{
	ch->lfd = nfuse_listen_tcp("127.0.0.1", 0, &ch->port);
	if (ch->lfd < 0) {
		nfuse_log("listening on 127.0.0.1 failed: %s", strerror(errno));
		return -1;
	}
	ch->is_unix = 0;
	return 0;
}

static void chan_cleanup_sock(struct fuse_chan *ch)
{
	if (ch->sockpath[0])
		unlink(ch->sockpath);
	if (ch->sockdir[0])
		rmdir(ch->sockdir);
	ch->sockpath[0] = ch->sockdir[0] = 0;
}

/*
 * Default transport. A unix socket (in a 0700 directory, only this user can connect) is safer
 * than 127.0.0.1 TCP, but measured on macOS 12.7.6: when writing over a unix socket, the client
 * gets EMSGSIZE when sending a large WRITE request (kernel log "nfs send error 40"), does not
 * retry and returns EIO to the application straight away (of a sequential write only the first
 * two or three blocks arrive); the same write works over TCP, and unix sockets work on macOS 27.
 * The cause is in the client: the buffer space available to the sender of a local stream socket
 * shrinks with the receiver's unread data, and macOS 12 treats a record that does not fit as a
 * fatal error. Neither larger buffers nor a smaller rwsize on the server side help.
 * Hence TCP is the default before macOS 26; 13 / 14 / 15 / 26 have not been measured yet (to be
 * enabled one by one once confirmed).
 */
static int transport_auto_tcp(void)
{
	char v[32];
	size_t n = sizeof(v);

	if (sysctlbyname("kern.osproductversion", v, &n, NULL, 0) != 0)
		return 1;
	v[sizeof(v) - 1] = 0;
	return atoi(v) < 26;
}

/* ---------- Public interface ---------- */

int fuse_version(void)
{
	return FUSE_VERSION;
}

int nfuse_set_ext_ops(const struct nfuse_ext_ops *ops, size_t ops_size)
{
	memset(&g_ext, 0, sizeof(g_ext));
	if (ops)
		memcpy(&g_ext, ops, ops_size < sizeof(g_ext) ? ops_size : sizeof(g_ext));
	g_ext_set = ops != NULL;
	return 0;
}

struct fuse_chan *fuse_mount(const char *mountpoint, struct fuse_args *args)
{
	struct fuse_chan *ch;
	const char *tr;
	char *p;

	if (!mountpoint)
		return NULL;
	ch = calloc(1, sizeof(*ch));
	if (!ch)
		return NULL;
	ch->lfd = -1;
	ch->rwsize = NFUSE_DEFAULT_RWSIZE;
	/*
	 * Default adfile: the client keeps the system default nonamedattr (reads and writes `._X` as
	 * before) and the server presents `._X` virtually, backed by the extended attributes of X
	 * (adfile.c), so no `._` files appear on the volume any more. streams (named attributes /
	 * OPENATTR) is experimental: on macOS 27 the kernel of the system NFS client panicked when the
	 * xattr command went down the named-attribute path (see docs/DESIGN.md); only use it on a test
	 * machine that can be isolated.
	 */
	ch->xattr_mode = 2;
	ch->ad_clean = 1;
	ch->case_ci = 1;
	ch->case_scan_ms = 300;
	ch->nfc = 1;
	parse_args(args, chan_opts_cb, ch);
	log_open(ch->debug);
	if (!realpath(mountpoint, ch->mnt)) {
		nfuse_log("mount point %s is not usable: %s", mountpoint, strerror(errno));
		free(ch);
		return NULL;
	}
	/* Export name: Finder uses the last component of the mount path as the volume name; it must not contain '/', and an empty one becomes nfuse */
	if (!ch->volname[0])
		snprintf(ch->volname, sizeof(ch->volname), "nfuse");
	for (p = ch->volname; *p; p++)
		if (*p == '/')
			*p = '_';
	tr = getenv("NFUSE_TRANSPORT");
	ch->want_tcp = tr && *tr && strcmp(tr, "auto") != 0 ? strcmp(tr, "tcp") == 0 :
		       transport_auto_tcp();
	if (ch->want_tcp ? chan_listen_tcp(ch) :
	    (chan_listen_unix(ch) && chan_listen_tcp(ch))) {
		free(ch);
		return NULL;
	}
	{
		/* The guard process of the previous driver on this mount point is still cleaning up (a forced unmount takes about 2 seconds): wait for it before mounting, see watchdog.h */
		long wms = env_long("NFUSE_GUARD_WAIT_MS", 10000);
		int w = nfuse_guard_wait(ch->mnt, (int)wms);

		if (w < 0)
			nfuse_log("the previous guard process on mount point %s has not finished cleaning up after %ld ms, continuing anyway", ch->mnt, wms);
		else if (w > 0)
			nfuse_log("waited for the previous guard process on the same mount point to finish cleaning up: %d ms", w);
	}
	nfuse_log("fuse_mount: mount point %s, volume name %s, rwsize %u, listening on %s%s, mount flags [%s%s%s%s]",
		 ch->mnt, ch->volname, ch->rwsize, ch->is_unix ? "unix " : "tcp ",
		 ch->is_unix ? ch->sockpath : "127.0.0.1", ch->ro ? " ro" : "", ch->nosuid ? " nosuid" : "",
		 ch->nodev ? " nodev" : "", ch->noexec ? " noexec" : "");
	return ch;
}

static void fuse_signal_handler(int sig)
{
	struct fuse *f = g_fuse;

	(void)sig;
	if (f) {
		char c = 's';
		f->exited = 1;
		(void)!write(f->wake[1], &c, 1);
	}
}

/* Test hook: SIGUSR2 -> simulate a server restart before the next COMPOUND (only installed with NFUSE_TEST_HOOKS=1) */
static void test_reboot_handler(int sig)
{
	(void)sig;
	if (g_fuse)
		g_fuse->srv.test_reboot = 1;
}

static void test_hooks_init(struct fuse *f)
{
	const char *drop = getenv("NFUSE_TEST_DROP");
	const char *mode = getenv("NFUSE_TEST_DROP_MODE");

	f->srv.no_drc = env_long("NFUSE_NO_DRC", 0) != 0;
	if (drop && *drop) {
		char buf[256], *tok, *save = NULL;

		snprintf(buf, sizeof(buf), "%s", drop);
		for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
			int op = strcmp(tok, "OPEN_EXCL") == 0 ? NFS4_OP_MAX + 1 :
				 nfs4_op_code(tok);
			if (op > 0)
				f->srv.test_drop[op]++;
			else
				nfuse_log("NFUSE_TEST_DROP: unknown operation %s", tok);
		}
		f->srv.test_drop_close = !(mode && strcmp(mode, "silent") == 0);
		nfuse_log("test hook: dropping replies of %s (%s)", drop,
			  f->srv.test_drop_close ? "connection closed after the drop" : "just not replying");
	}
	if (env_long("NFUSE_TEST_HOOKS", 0)) {
		struct sigaction sa;

		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = test_reboot_handler;
		sigemptyset(&sa.sa_mask);
		sigaction(SIGUSR2, &sa, NULL);
		nfuse_log("test hook: SIGUSR2 simulates a server restart");
	}
	if (f->srv.no_drc)
		nfuse_log("test: DRC disabled");
	{
		const char *h = getenv("NFUSE_TEST_HANG");
		const char *st = getenv("NFUSE_TEST_STALL");

		if (h && *h) {
			char buf[64], *colon;
			int op;

			snprintf(buf, sizeof(buf), "%s", h);
			colon = strchr(buf, ':');
			if (colon)
				*colon++ = 0;
			op = nfs4_op_code(buf);
			if (op > 0) {
				f->srv.test_hang_op = (uint32_t)op;
				f->srv.test_hang_skip = colon ? (uint32_t)strtoul(colon, NULL, 10) : 0;
				if (f->srv.test_hang_skip)
					f->srv.test_hang_skip--;
				f->srv.test_hang_ms = (uint32_t)env_long("NFUSE_TEST_HANG_MS", 0);
				f->srv.test_hang_progress = env_long("NFUSE_TEST_HANG_PROGRESS", 0) != 0;
				nfuse_log("test hook: occurrence %u of %s hangs (%s%s)", f->srv.test_hang_skip + 1, buf,
					  f->srv.test_hang_ms ? "time-limited" : "never returns",
					  f->srv.test_hang_progress ? ", pretending the backend makes progress meanwhile" : "");
			} else
				nfuse_log("NFUSE_TEST_HANG: unknown operation %s", buf);
		}
		if (st && *st) {
			f->srv.test_stall_path = st;
			nfuse_log("test hook: server threads stall while %s exists", st);
		}
	}
}

/* Build the mount_nfs spec and -o options */
static void mount_args(struct fuse *f, char *spec, size_t sl, char *opts,
		       size_t ol)
{
	struct fuse_chan *ch = f->ch;
	const char *extra = getenv("NFUSE_MOUNT_EXTRA");
	int rdonly = env_long("NFUSE_NO_RDONLY", 0) == 0;

	if (ch->is_unix) {
		/* Local socket: the host part is the absolute path in angle brackets; nocallback is mandatory */
		snprintf(spec, sl, "<%s>:/%s", ch->sockpath, ch->volname);
		snprintf(opts, ol, "vers=4.0,proto=ticotsord,nocallback");
	} else {
		snprintf(spec, sl, "127.0.0.1:/%s", ch->volname);
		snprintf(opts, ol, "vers=4.0,tcp,port=%u,nocallback", ch->port);
	}
	/*
	 * Read-only mounts (our own choice of values, not the hard,nointr other NFS-backed FUSE layers use):
	 *   soft: when the server does not answer, calls eventually fail (after retrans timeouts) instead of waiting forever;
	 *   intr: a stuck call can be interrupted by a signal;
	 *   deadtimeout=60: declared dead after 60 seconds without a response, the system can then force-unmount;
	 *   timeo=10 (starting at 1 second, adjusted dynamically by the client), retrans=10;
	 *   rdonly: lets the client return EROFS by itself;
	 *   locks: has to be given explicitly. For a "soft and read-only" mount the system defaults
	 *     to locallocks, NFSv4 does not accept locallocks and mount fails with EINVAL straight
	 *     away (measured on macOS 27). nolocks gives EINVAL as well.
	 *
	 * Read-write mounts: hard,intr,timeo=10,deadtimeout=60,locks
	 *   hard: a soft timeout would make WRITE / COMMIT fail silently and lose data half-way (the
	 *     application believes the write succeeded); read-write mounts must be hard -- with a
	 *     slow server the client keeps waiting and no data is lost;
	 *   intr: a stuck process can be interrupted by a signal (Ctrl-C / kill) and does not turn
	 *     into an unkillable process;
	 *   deadtimeout=60: when the server (the driver process) is gone or hung, 60 seconds after
	 *     the client reports "not responding" it marks the mount dead and force-unmounts it, so
	 *     the mount point does not stay stuck forever; an application can also umount -f at any time;
	 *   timeo=10: retransmissions start at 1 second (local connection, normal requests are
	 *     answered within milliseconds); with hard, retrans only affects how soon "not
	 *     responding" is reported, the default is used;
	 *   locks: same as read-only (file locks go through NFSv4 LOCK, the server decides conflicts).
	 *   The exact number of seconds will be settled after measurements (unplugging, sleep / wake, forced kill).
	 */
	/*
	 * adfile (virtual `._X`) needs nordirplus: with READDIRPLUS the client concludes from its own
	 * cached directory listing that `._X` does not exist and never asks the server (measured on
	 * macOS 12: after ls, xattr -l could not read the attributes stored in the `._X` of an older
	 * volume and the server did not receive a single LOOKUP `._X`; with nordirplus it works).
	 */
	const char *xo0 = f->srv.named_attr ? ",namedattr" : f->srv.adfile ? ",nordirplus" : "";
	char xo[96];

	/* nosuid / nodev / noexec are passed to mount_nfs when given (setuid programs on an ext volume must not take effect when it is mounted as root) */
	snprintf(xo, sizeof(xo), "%s%s%s%s", ch->nosuid ? ",nosuid" : "", ch->nodev ? ",nodev" : "",
		 ch->noexec ? ",noexec" : "", xo0);
	if (ch->ro)
		snprintf(opts + strlen(opts), ol - strlen(opts),
			 ",rwsize=%u%s%s,soft,intr,timeo=10,retrans=10,deadtimeout=60%s%s%s",
			 ch->rwsize, ch->nobrowse ? ",nobrowse" : "", rdonly ? ",rdonly,locks" : "", xo,
			 extra && *extra ? "," : "", extra && *extra ? extra : "");
	else
		snprintf(opts + strlen(opts), ol - strlen(opts),
			 ",rwsize=%u%s,hard,intr,timeo=10,deadtimeout=60,locks%s%s%s",
			 ch->rwsize, ch->nobrowse ? ",nobrowse" : "", xo,
			 extra && *extra ? "," : "", extra && *extra ? extra : "");
}

static int do_mount(struct fuse *f, char *err, size_t errlen)
{
	char spec[PATH_MAX + 300], opts[1024];
	struct timeval t0, t1;
	int rc;

	long tmo = env_long("NFUSE_MOUNT_TIMEOUT_S", 30);

	mount_args(f, spec, sizeof(spec), opts, sizeof(opts));
	snprintf(f->ch->from, sizeof(f->ch->from), "%s", spec);
	nfuse_log("mount_nfs -o %s '%s' '%s'", opts, spec, f->ch->mnt);
	gettimeofday(&t0, NULL);
	f->mounting = 1;
	f->trusted_given = 0;
	rc = nfuse_run_mount_nfs(spec, opts, f->ch->mnt, tmo > 0 ? (int)tmo : 30, err, errlen);
	f->mounting = 0;
	gettimeofday(&t1, NULL);
	if (rc == -2) {
		/*
		 * mount_nfs timed out: the handshake has not finished -- do not kill mount_nfs, do not
		 * umount -f (a mount in the middle of its handshake must not be touched, see docs/DESIGN.md).
		 * Handle it like a stuck handshake: take over the connections, answer in-flight and later
		 * requests with IO and let mount_nfs fail and return on its own; the failure path of
		 * fuse_new then exits (24).
		 */
		nfuse_log("%s: handled like a stuck handshake (taking over the connections and answering IO; mount_nfs is not killed, nothing is force-unmounted)", err);
		if (!f->mount_failed_hung)
			mount_stuck(f, 0, "mount_timeout");
		rc = -1;
	}
	if (rc == 0 && !nfuse_is_nfs_mounted(f->ch->mnt, f->ch->from)) {
		snprintf(err, errlen, "mount_nfs reported success but the mount is not in the mount table");
		rc = -1;
	}
	f->mount_s = (double)(t1.tv_sec - t0.tv_sec) + (t1.tv_usec - t0.tv_usec) / 1e6;
	nfuse_log("mount_nfs %s, took %.3f s%s%s", rc == 0 ? "succeeded" : "failed",
		 (double)(t1.tv_sec - t0.tv_sec) + (t1.tv_usec - t0.tv_usec) / 1e6,
		 rc ? ": " : "", rc ? err : "");
	f->t_mount = t1;
	return rc;
}

struct fuse *fuse_new(struct fuse_chan *ch, struct fuse_args *args,
		      const struct fuse_operations *op, size_t op_size,
		      void *user_data)
{
	struct fuse *f;
	struct nfuse_bf_config cfg;
	struct fuse_conn_info conn;
	char err[1024];
	int debug = 0, r;

	if (!ch || !op)
		return NULL;
	parse_args(args, new_opts_cb, &debug);
	if (debug && !ch->debug) {
		ch->debug = 1;
		log_open(1);
	}
	f = calloc(1, sizeof(*f));
	if (!f)
		return NULL;
	memcpy(&f->op, op, op_size < sizeof(f->op) ? op_size : sizeof(f->op));
	f->ch = ch;
	f->se.f = f;
	f->wake[0] = f->wake[1] = -1;
	f->guard_fd = -1;
	f->ctx.fuse = f;
	f->ctx.uid = getuid();
	f->ctx.gid = getgid();
	f->ctx.umask = 022;
	f->ctx.private_data = user_data;
	g_fuse = f;

	/* init: called once before serving starts, as in traditional FUSE */
	if (f->op.init) {
		memset(&conn, 0, sizeof(conn));
		conn.proto_major = 7;
		conn.proto_minor = 12;
		conn.max_write = ch->rwsize;
		conn.max_readahead = ch->rwsize;
		conn.capable = FUSE_CAP_BIG_WRITES | FUSE_CAP_DONT_MASK |
			       FUSE_CAP_IOCTL_DIR;
		f->ctx.private_data = f->op.init(&conn);
		f->inited = 1;
	}

	nfuse_fork_warmup();		/* the driver may daemon() next: CF types the child needs (names, IOKit results) are initialised before the fork */
	nfuse_server_init(&f->srv, ch->volname);
	f->srv.rwsize = ch->rwsize;
	f->srv.verbose = env_long("NFUSE_VERBOSE", 0) != 0;
	f->srv.log = (g_logf || g_log_stderr) ? nfuse_log : NULL;
	f->srv.named_attr = ch->xattr_mode == 1 || env_long("NFUSE_NAMEDATTR", 0) != 0;
	f->srv.adfile = ch->xattr_mode == 2 && !f->srv.named_attr;

	memset(&cfg, 0, sizeof(cfg));
	cfg.max_nodes = (size_t)env_long("NFUSE_MAX_NODES", 0);
	cfg.attr_ttl_ms = (uint32_t)env_long("NFUSE_ATTR_TTL_MS", 0);
	cfg.log = f->srv.log;
	cfg.fake_xattr = env_long("NFUSE_NAMEDATTR", 0) >= 2;
	cfg.namedattr = f->srv.named_attr;
	cfg.adtrans = f->srv.adfile;
	cfg.ad_clean = ch->ad_clean;
	cfg.case_insensitive = ch->case_ci;
	cfg.nfc = ch->nfc;
	cfg.ext = g_ext_set ? &g_ext : NULL;
	cfg.raw_names = env_long("NFUSE_TEST_RAWNAMES", 0) != 0;
	if (cfg.raw_names)
		nfuse_log("test hook: names pass through unchanged (no equivalence lookup / NFC)");
	r = nfuse_backend_fuse_init(&f->srv.be, &f->op, &f->ctx, &cfg);
	if (r) {
		nfuse_log("backend initialisation failed (getattr(\"/\")): %s", strerror(-r));
		goto fail;
	}
	/*
	 * Case-insensitive: before mounting, scan the directories within the time limit; if the volume
	 * already has names that differ only in case (written by Linux / WSL), the whole volume is
	 * mounted case-sensitive; if the scan did not finish it is mounted insensitive, and when a
	 * conflicting directory is met at run time the exact name wins and each entry can still be
	 * opened (see real_name in backend_fuse.c).
	 */
	if (ch->case_ci && ch->case_scan_ms) {
		struct nfuse_case_scan cs;

		nfuse_backend_fuse_case_scan(&f->srv.be, ch->case_scan_ms, &cs);
		f->case_scan = cs;
		f->case_scanned = 1;
		if (cs.conflicts) {
			ch->case_ci = 0;
			ch->case_downgraded = 1;
			nfuse_backend_fuse_set_case(&f->srv.be, 0);
		}
		nfuse_log("case scan: %s, %u directories %llu entries, took %.1f ms, %u pairs differing only in case, %u pairs differing only in normalisation%s%s -> mounting %s",
			  cs.complete ? "whole volume scanned" : "time limit reached before finishing", cs.dirs,
			  (unsigned long long)cs.entries, cs.ms, cs.conflicts, cs.normdups,
			  cs.conflicts ? ", e.g. " : "", cs.conflicts ? cs.sample : "",
			  ch->case_ci ? "case-insensitive" : "case-sensitive (automatic downgrade)");
	}
	f->srv.case_insensitive = ch->case_ci;
	nfuse_log("options: nfuse_xattr=%s nfuse_adclean=%d nfuse_case=%s nfuse_nfc=%d ext_ops=%s%s%s",
		  f->srv.named_attr ? "streams (experimental)" : f->srv.adfile ? "adfile" : "appledouble", ch->ad_clean,
		  ch->case_ci ? "insensitive" : "sensitive", ch->nfc,
		  g_ext_set ? "yes" : "no",
		  g_ext_set && g_ext.casefold ? " (own case folding)" : "",
		  g_ext_set && g_ext.xattr_read ? " (offset-based attribute I/O)" : "");
	if (ch->ro)
		f->srv.be.readonly = 1;
	if (f->srv.be.readonly && !ch->ro)
		ch->ro = 1;	/* the file system has no write callback: mount read-only */
	test_hooks_init(f);
	if (pipe(f->wake) < 0)
		goto fail;
	fcntl(f->wake[0], F_SETFL, O_NONBLOCK);
	fcntl(f->wake[1], F_SETFL, O_NONBLOCK);
	fcntl(f->wake[0], F_SETFD, FD_CLOEXEC);
	fcntl(f->wake[1], F_SETFD, FD_CLOEXEC);
	fcntl(ch->lfd, F_SETFD, FD_CLOEXEC);

	nfuse_tp_init(&f->tp, &f->srv, ch->lfd, f->wake[1]);
	f->tp_ok = 1;
	tp_setup_guard(f);
	wd_config(f);
	wd_start(f);	/* watch during the mount handshake already (when the server thread hangs, mount_nfs hangs too) */
	if (nfuse_tp_resume(&f->tp))
		goto fail;
	if (do_mount(f, err, sizeof(err))) {
		if (!ch->is_unix || ch->want_tcp || f->mount_failed_hung)
			goto fail_mount;
		/* The local socket could not be mounted (unsupported on older systems etc.): retry once over 127.0.0.1 TCP */
		nfuse_log("mounting over the unix socket failed, retrying over TCP");
		nfuse_tp_shutdown(&f->tp);	/* this closes the unix listening socket */
		chan_cleanup_sock(ch);
		if (chan_listen_tcp(ch))
			goto fail_mount;
		fcntl(ch->lfd, F_SETFD, FD_CLOEXEC);
		nfuse_tp_init(&f->tp, &f->srv, ch->lfd, f->wake[1]);
		tp_setup_guard(f);
		if (nfuse_tp_resume(&f->tp) || do_mount(f, err, sizeof(err)))
			goto fail_mount;
	}
	ch->mounted = 1;
	ch->f = f;
	/* Park at a request boundary: the driver may daemon() next, fuse_loop starts the threads again */
	nfuse_tp_pause(&f->tp);
	wd_stop(f);
	return f;

fail_mount:
	syslog(LOG_ERR, "nfuse: mount_nfs failed: %s", err);
	fprintf(stderr, "nfuse: mount failed: %s\n", err);
	if (f->mount_failed_hung) {
		/*
		 * The handshake got stuck and mount_nfs has been made to fail: the stuck thread holds the
		 * global lock and may be stuck in the device, so it cannot be joined and the file system must
		 * not be touched again.
		 * If mount_nfs has already exited: make sure our entry is not in the mount table (if it is,
		 * it is ours and the handshake has finished: force-unmount it).
		 * If mount_nfs is still running (the timeout path; it has not exited within 3 seconds of the
		 * takeover): the handshake has not finished, leave the mount point alone and just exit --
		 * measurements showed that the driver exiting mid-handshake is safe:
		 * mount_nfs fails and exits on its own within about 20 seconds and leaves no mount behind.
		 * Exit code 24, the same outcome as the watchdog.
		 */
		int done = nfuse_mount_nfs_wait(3000);
		int still = done && f->ch->from[0] && nfuse_is_nfs_mounted(f->ch->mnt, f->ch->from);

		if (still)
			nfuse_unmount_force(f->ch->mnt, f->ch->from, nfuse_log);
		nfuse_log("mount handshake stuck, mount_nfs %s (%s); mount table: %s; exit code %d",
			  done ? "has failed and returned" : "is still running (not killed, left to fail on its own)", err,
			  !done ? "not checked, not touched" : still ? "leftover entry, force-unmounted" : "no such mount", NFUSE_EXIT_WATCHDOG);
		_exit(NFUSE_EXIT_WATCHDOG);
	}
fail:
	if (f->tp_ok) {
		nfuse_tp_shutdown(&f->tp);	/* this blocks when the server thread is stuck in the backend: the watchdog is still on and will force an exit in time */
		ch->lfd = -1;
	}
	if (f->inited && f->op.destroy) {
		/*
		 * Once init has been called, destroy has to follow for clean-up (the file system may have
		 * modified the volume in init: an ext driver marks it "not cleanly unmounted", an exFAT
		 * driver marks it dirty), just as when unmounting after a successful mount. The server
		 * thread is stopped; the watchdog is still watching (destroy may touch the device).
		 */
		f->extra_name = "destroy";
		f->extra_t0 = nfuse_uptime_ns();
		f->op.destroy(f->ctx.private_data);
		f->extra_t0 = 0;
		f->inited = 0;
	}
	wd_stop(f);
	nfuse_backend_fuse_free(&f->srv.be);
	if (f->wake[0] >= 0)
		close(f->wake[0]);
	if (f->wake[1] >= 0)
		close(f->wake[1]);
	g_fuse = NULL;
	free(f);
	return NULL;
}

/* ---------- Local security (127.0.0.1 TCP) ---------- */

/*
 * Find the process that owns the client end of the TCP connection "local port lport -> our
 * port fport". The connections of the system NFS client are created by the kernel and are not
 * in any process's descriptor table: not found = kernel.
 * Returns the owner pid; 0 if not found; *seen receives the number of processes whose
 * descriptor table was visible (non-root only sees this user's processes).
 */
static pid_t tcp_peer_owner(uint16_t lport, uint16_t fport, int *seen)
{
	int n, i, cap;
	pid_t *pids, owner = 0, me = getpid();
	struct proc_fdinfo *fds = NULL;
	int fdcap = 0;

	*seen = 0;
	n = proc_listallpids(NULL, 0);
	if (n <= 0)
		return 0;
	cap = n + 64;
	pids = calloc((size_t)cap, sizeof(*pids));
	if (!pids)
		return 0;
	n = proc_listallpids(pids, cap * (int)sizeof(*pids));
	for (i = 0; i < n && !owner; i++) {
		int sz, nf, j;

		if (pids[i] <= 0 || pids[i] == me)
			continue;
		sz = proc_pidinfo(pids[i], PROC_PIDLISTFDS, 0, NULL, 0);
		if (sz <= 0)
			continue;
		if (sz > fdcap) {
			struct proc_fdinfo *nfd = realloc(fds, (size_t)sz * 2);
			if (!nfd)
				continue;
			fds = nfd;
			fdcap = sz * 2;
		}
		sz = proc_pidinfo(pids[i], PROC_PIDLISTFDS, 0, fds, fdcap);
		if (sz <= 0)
			continue;
		(*seen)++;
		nf = sz / (int)sizeof(struct proc_fdinfo);
		for (j = 0; j < nf; j++) {
			struct socket_fdinfo si;
			struct in_sockinfo *in;

			if (fds[j].proc_fdtype != PROX_FDTYPE_SOCKET)
				continue;
			if (proc_pidfdinfo(pids[i], fds[j].proc_fd, PROC_PIDFDSOCKETINFO, &si,
					   sizeof(si)) != (int)sizeof(si))
				continue;
			if (si.psi.soi_kind != SOCKINFO_TCP)
				continue;
			in = &si.psi.soi_proto.pri_tcp.tcpsi_ini;
			if (ntohs((uint16_t)in->insi_lport) == lport &&
			    ntohs((uint16_t)in->insi_fport) == fport) {
				owner = pids[i];
				break;
			}
		}
	}
	free(fds);
	free(pids);
	return owner;
}

/*
 * Connection check for 127.0.0.1 TCP (before macOS 26, see docs/DESIGN.md):
 * 1. The peer TCP port belongs to a user process (not the kernel NFS client) -> reject; except
 *    mount_nfs itself (merely untrusted).
 * 2. The first kernel connection arriving during the mount handshake -> trusted; all other
 *    connections are untrusted and become trusted only by presenting a valid file handle (server.c).
 * Running as root, rule 1 sees every process; non-root only sees this user's processes
 * (connections from other users are stopped by rule 2).
 */
static int owner_is_child(pid_t pid)
{
	struct proc_bsdinfo bi;

	return proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bi, sizeof(bi)) == (int)sizeof(bi) &&
	       (pid_t)bi.pbi_ppid == getpid();
}

static int tcp_accept_check(void *arg, int fd, int *trusted)
{
	struct fuse *f = arg;
	struct sockaddr_in peer, me;
	socklen_t pl = sizeof(peer), ml = sizeof(me);
	uint64_t t0 = nfuse_uptime_ns();
	pid_t owner;
	int seen = 0;
	double ms;

	*trusted = 0;
	if (getpeername(fd, (struct sockaddr *)&peer, &pl) || getsockname(fd, (struct sockaddr *)&me, &ml) ||
	    peer.sin_family != AF_INET || peer.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) {
		nfuse_log("local security: connection rejected (peer address unavailable or not 127.0.0.1)");
		f->guard_rejected++;
		return -1;
	}
	/* Test hook: NFUSE_TEST_GUARD_NOSCAN=1 skips the process scan (simulates "a process we cannot see", to verify the second gate on its own) */
	owner = env_long("NFUSE_TEST_GUARD_NOSCAN", 0) ? 0 :
		tcp_peer_owner(ntohs(peer.sin_port), ntohs(me.sin_port), &seen);
	ms = (nfuse_uptime_ns() - t0) / 1e6;
	if (ms > f->guard_scan_ms_max)
		f->guard_scan_ms_max = ms;
	/* mount_nfs (a child we started ourselves; it may connect right after posix_spawn returns, before its pid has been recorded) is merely untrusted, not rejected */
	if (owner > 0 && owner != nfuse_mount_pid && !owner_is_child(owner)) {
		char name[64] = "?";

		proc_name(owner, name, sizeof(name));
		nfuse_log("local security: rejected local process %d (%s) connecting from port %u -- only the system NFS client is accepted (scanned %d processes, %.1f ms)",
			  (int)owner, name, ntohs(peer.sin_port), seen, ms);
		syslog(LOG_WARNING, "nfuse: rejected NFS connection from local process %d (%s)", (int)owner, name);
		f->guard_rejected++;
		return -1;
	}
	if (!owner && f->mounting && !f->trusted_given) {
		f->trusted_given = 1;
		*trusted = 1;
	}
	if (!*trusted)
		f->guard_untrusted++;
	nfuse_log("local security: connection accepted (peer port %u, %s, scanned %d processes in %.1f ms%s)", ntohs(peer.sin_port),
		  owner ? "connection of the mount_nfs started by this process, untrusted" : *trusted ? "kernel connection of the mount handshake, trusted" :
		  "kernel connection (or a process we cannot see), trusted only after presenting a valid file handle",
		  seen, ms, getuid() ? "; non-root only sees this user's processes" : "");
	return 0;
}

static void tp_setup_guard(struct fuse *f)
{
	f->tcp_guard = !f->ch->is_unix && env_long("NFUSE_TCP_GUARD", 1) != 0;
	if (f->tcp_guard) {
		f->tp.accept_hook = tcp_accept_check;
		f->tp.accept_arg = f;
	}
}

/* ---------- Watchdog ---------- */

/*
 * Declare a hang: later requests do not wait for the lock and return IO directly (handle_call
 * in server.c), the file system is notified so it can record telemetry, a mount_nfs that is
 * still running is left to fail on its own (it is never killed), an established mount is
 * force-unmounted, _exit. No clean-up at all: the
 * stuck thread holds the global lock and may be stuck in the device, and touching the file
 * system again would only hang along with it (the disk is not responding anyway at this point,
 * and data in memory that has not been written cannot be saved).
 */
static void wd_fire(struct fuse *f, const char *why, uint64_t since, uint64_t quiet)
{
	uint64_t now = nfuse_uptime_ns(), t0;
	uint32_t op = f->srv.cur_op;
	const char *opn = op ? nfs4_op_name(op) : (f->extra_t0 && f->extra_name ? f->extra_name : "-");
	double secs = since ? (now - since) / 1e9 : 0, qs = quiet / 1e9;
	int code = 0, rc = 0, mounted;
	struct fuse_chan *ch = f->ch;

	f->srv.hung = 1;
	f->srv.dev_lost = 1;
	f->wd_fired_ns = now;
	g_log_sync = 1;
	nfuse_log("watchdog: %s -- operation %s has been running for %.1f s with no backend progress at all for %.1f s (threshold %.0f s%s). All further requests return IO; force-unmounting, then exiting",
		  why, opn, secs, qs, f->wd_ns / 1e9,
		  strcmp(why, "too_long") == 0 ? ", absolute limit" : "");
	syslog(LOG_ERR, "nfuse: watchdog: %s, op %s stuck %.1fs (no backend progress %.1fs); force unmounting %s",
	       why, opn, secs, qs, ch ? ch->mnt : "?");
	if (g_ext_set && g_ext.watchdog)
		code = g_ext.watchdog(why, opn, secs);
	if (code <= 0)
		code = NFUSE_EXIT_WATCHDOG;
	if (f->mounting || nfuse_mount_pid > 0) {
		/* Handshake in progress: do not kill mount_nfs, do not touch the mount point; take over the connections, answer IO, then exit; mount_nfs fails and returns on its own */
		nfuse_log("watchdog: mount handshake in progress (mount_nfs pid %d still running) -- not killing it, not force-unmounting; taking over the connections, then exiting",
			  (int)nfuse_mount_pid);
	}
	/* Stuck connections are handed to the takeover thread, which answers IO (the client then has no in-flight requests and the forced unmount does not have to wait, see server.c) */
	{
		int k = nfuse_tp_takeover(&f->tp);

		if (k)
			nfuse_log("watchdog: took over %d stuck connections; in-flight and later requests return IO", k);
	}
	t0 = nfuse_uptime_ns();
	mounted = !f->mounting && nfuse_mount_pid <= 0 && ch && ch->from[0] &&
		  nfuse_is_nfs_mounted(ch->mnt, ch->from);
	if (mounted)
		rc = nfuse_unmount_force(ch->mnt, ch->from, nfuse_log);
	nfuse_log("watchdog: %s%s, took %.3f s; %.3f s since the hang was declared; exit code %d",
		  mounted ? "forced unmount" : "mount point was already gone", mounted ? (rc == 0 ? " succeeded" : " failed") : "",
		  (nfuse_uptime_ns() - t0) / 1e9, (nfuse_uptime_ns() - now) / 1e9, code);
	_exit(code);
}

/* Mount handshake stuck: take over the connections and answer IO so that mount_nfs fails on its own (see the comment in wd_thread) */
static void mount_stuck(struct fuse *f, uint64_t since, const char *why)
{
	uint32_t op = f->srv.cur_op;
	double secs = since ? (nfuse_uptime_ns() - since) / 1e9 : 0;
	int k;

	if (f->mount_failed_hung)
		return;
	f->srv.hung = 1;
	f->srv.dev_lost = 1;
	f->mount_failed_hung = 1;
	g_log_sync = 1;
	k = nfuse_tp_takeover(&f->tp);
	nfuse_log("mount handshake stuck (%s): %s has had no reply for %.1f s -- taking over %d connections and answering IO so that mount_nfs reports the error and returns on its own (not killing it, not force-unmounting), then exiting",
		  why, op ? nfs4_op_name(op) : "-", secs, k);
	syslog(LOG_ERR, "nfuse: mount handshake stuck (%s, %s %.1fs), failing the mount", why,
	       op ? nfs4_op_name(op) : "-", secs);
	if (g_ext_set && g_ext.watchdog)
		(void)g_ext.watchdog(why, op ? nfs4_op_name(op) : "-", secs);
}

static uint64_t wd_progress(struct fuse *f)
{
	uint64_t v = f->srv.test_progress;

	if (g_ext_set && g_ext.progress)
		v += g_ext.progress();
	return v;
}

static void *wd_thread(void *arg)
{
	struct fuse *f = arg;
	uint64_t pv = wd_progress(f), pt = nfuse_uptime_ns(), hb = 0;

	while (!f->wd_stop) {
		uint64_t now, v, req, idle, acc, x;

		usleep(200 * 1000);
		now = nfuse_uptime_ns();
		v = wd_progress(f);
		if (v != pv) {
			pv = v;
			pt = now;
		}
		if (f->guard_fd >= 0 && now - hb >= 1000000000ULL) {
			(void)!write(f->guard_fd, "h", 1);	/* heartbeat for the guard process (non-blocking; if the pipe is full, never mind) */
			hb = now;
		}
		if ((!f->wd_ns && !f->mount_stuck_ns) || f->wd_fired_ns)
			continue;
		nfuse_tp_health(&f->tp, &req, &idle, &acc);
		/*
		 * During the mount handshake (two kernel panics seen on macOS 12): when handshake
		 * requests get no reply, the macOS 12 NFS client closes the connection after about 13 seconds
		 * and reconnects, and on the second reconnect (at about 28 seconds) the kernel panics with a
		 * double free in nfs_connect -> nfs_socket_destroy.
		 * So once a handshake request has been stuck for mount_stuck seconds (and the device made no
		 * progress during that time, or it has been stuck for twice as long) we stop waiting: take
		 * over the connection and answer it with IO so that mount_nfs reports the error and returns
		 * on its own (it is not killed, nothing is force-unmounted); the driver then exits (24).
		 */
		if (f->mounting && !f->srv.hung && req && f->mount_stuck_ns) {
			uint64_t n2 = nfuse_uptime_ns(), from2 = req > pt ? req : pt;

			if (req <= n2 && (n2 - req >= 2 * f->mount_stuck_ns ||
					  (n2 - req >= f->mount_stuck_ns && n2 - (from2 < n2 ? from2 : n2) >= f->mount_stuck_ns)))
				mount_stuck(f, req, "mount_stuck");
		}
		/*
		 * A heartbeat / request start time written by another thread after we sampled now is newer
		 * than now: treat it as "just now".
		 * (Without this the unsigned subtraction underflowed to 1.8e10 seconds and the accept thread
		 * was declared stalled the moment it started.)
		 */
		now = nfuse_uptime_ns();
		if (req > now)
			req = now;
		if (idle > now)
			idle = now;
		if (acc > now)
			acc = now;
		x = f->extra_t0;
		if (x > now)
			x = now;
		if (x && (!req || x < req))
			req = x;
		if (req) {
			uint64_t from = req > pt ? req : pt;

			if (from > now)
				from = now;
			if (now - req > f->wd_age_max_ns)
				f->wd_age_max_ns = now - req;
			if (now - from > f->wd_quiet_max_ns)
				f->wd_quiet_max_ns = now - from;

			if (!f->wd_ns)
				continue;	/* only the mount-handshake check is enabled */
			if (now - req >= f->wd_ns && now - from >= f->wd_ns)
				wd_fire(f, "stuck", req, now - from);
			if (f->wd_max_ns && now - req >= f->wd_max_ns)
				wd_fire(f, "too_long", req, now - from);
		}
		if (!f->wd_ns)
			continue;
		if (idle && now - idle >= f->wd_ns)
			wd_fire(f, "conn_stall", idle, now - idle);
		if (acc && now - acc >= f->wd_ns)
			wd_fire(f, "accept_stall", acc, now - acc);
		if (f->loop_running && f->loop_beat && f->loop_beat < now && now - f->loop_beat >= f->wd_ns)
			wd_fire(f, "loop_stall", f->loop_beat, now - f->loop_beat);
	}
	return NULL;
}

static void wd_start(struct fuse *f)
{
	if (f->wd_running || (!f->wd_ns && !f->mount_stuck_ns && f->guard_fd < 0))
		return;
	f->wd_stop = 0;
	if (pthread_create(&f->wd_th, NULL, wd_thread, f) == 0)
		f->wd_running = 1;
	else
		nfuse_log("failed to start the watchdog thread");
}

static void wd_stop(struct fuse *f)
{
	if (!f->wd_running)
		return;
	f->wd_stop = 1;
	pthread_join(f->wd_th, NULL);
	f->wd_running = 0;
}

static void wd_config(struct fuse *f)
{
	const char *e = getenv("NFUSE_WATCHDOG_S");
	const char *m = getenv("NFUSE_WATCHDOG_MAX_S");
	const char *g = getenv("NFUSE_GUARD_S");
	double ws = e && *e ? strtod(e, NULL) : 30;
	double ms = m && *m ? strtod(m, NULL) : 600;
	double gs = g && *g ? strtod(g, NULL) : 20;

	f->wd_ns = ws > 0 ? (uint64_t)(ws * 1e9) : 0;
	{
		const char *ms2 = getenv("NFUSE_MOUNT_STUCK_S");
		double v = ms2 && *ms2 ? strtod(ms2, NULL) : 5;

		f->mount_stuck_ns = v > 0 ? (uint64_t)(v * 1e9) : 0;
	}
	f->wd_max_ns = ms > 0 ? (uint64_t)(ms * 1e9) : 0;
	f->guard_on = env_long("NFUSE_GUARD", 1) != 0;
	f->guard_ms = gs > 0 ? (uint32_t)(gs * 1000) : 0;
	f->guard_fd = -1;
	nfuse_log("automatic recovery: watchdog %s (no progress for %.0f s, absolute limit %.0f s, progress counter %s), guard process %s (frozen after %.0f s without a heartbeat)",
		  f->wd_ns ? "on" : "off", ws, ms,
		  g_ext_set && g_ext.progress ? "provided by the file system" : "none (time only)",
		  f->guard_on ? "on" : "off", gs);
}

/* Start the guard process at the beginning of fuse_loop (after daemon, in this process) */
static void guard_start(struct fuse *f)
{
	struct nfuse_guard_cfg c;

	if (!f->guard_on || f->guard_fd >= 0 || !f->ch)
		return;
	memset(&c, 0, sizeof(c));
	snprintf(c.mnt, sizeof(c.mnt), "%s", f->ch->mnt);
	snprintf(c.from, sizeof(c.from), "%s", f->ch->from);
	c.log_fd = g_logf ? fileno(g_logf) : -1;
	c.report_fd = g_ext_set && g_ext.report_fd ? g_ext.report_fd() : -1;
	c.stall_ms = f->guard_ms;
	c.lock_fd = nfuse_guard_lock_open(f->ch->mnt);
	if (c.lock_fd >= 0 && flock(c.lock_fd, LOCK_SH | LOCK_NB) != 0) {
		close(c.lock_fd);	/* could not get it (should not happen): go without the lock */
		c.lock_fd = -1;
	}
	{
		/* Used to identify the mount (over TCP the source string is identical after a same-named volume is remounted). No statfs on the mount point: the server threads are not running yet and it would hang */
		int n = getfsstat(NULL, 0, MNT_NOWAIT), i;
		struct statfs *sf = n > 0 ? calloc((size_t)n + 8, sizeof(*sf)) : NULL;

		if (sf) {
			n = getfsstat(sf, (int)(((size_t)n + 8) * sizeof(*sf)), MNT_NOWAIT);
			for (i = 0; i < n; i++)
				if (strcmp(sf[i].f_mntonname, f->ch->mnt) == 0 &&
				    strcmp(sf[i].f_mntfromname, f->ch->from) == 0)
					c.fsid = sf[i].f_fsid;	/* with several entries for one path take the last (topmost) one */
			free(sf);
		}
	}
	if (g_logf)
		fflush(g_logf);
	f->guard_fd = nfuse_guard_start(&c, &f->guard_pid);
	/* The open file description of the lock is inherited and held by the guard process; the driver keeping its own copy does no harm (closed on exit), and it is not closed here so the shared lock is not affected */
	if (f->guard_fd < 0)
		nfuse_log("failed to start the guard process: %s", strerror(errno));
	else
		nfuse_log("guard process pid %d", (int)f->guard_pid);
}

const char *nfuse_end_reason(struct fuse *f)
{
	return f && f->end_reason ? f->end_reason : "";
}

int nfuse_get_mount_info(struct fuse *f, struct nfuse_mount_info *mi, size_t size)
{
	struct nfuse_mount_info m;
	struct fuse_chan *ch;

	if (!f || !(ch = f->ch) || !mi)
		return -1;
	memset(&m, 0, sizeof(m));
	snprintf(m.case_mode, sizeof(m.case_mode), "%s", ch->case_ci ? "insensitive" : "sensitive");
	m.case_downgraded = ch->case_downgraded;
	m.case_conflicts = f->case_scan.conflicts;
	m.case_scan_complete = f->case_scanned ? (f->case_scan.complete ? 1 : 0) : -1;
	snprintf(m.xattr_mode, sizeof(m.xattr_mode), "%s",
		 f->srv.named_attr ? "streams" : f->srv.adfile ? "adfile" : "appledouble");
	snprintf(m.transport, sizeof(m.transport), "%s", ch->is_unix ? "unix" : "tcp");
	m.mount_s = f->mount_s;
	m.browse = !ch->nobrowse;
	m.readonly = ch->ro;
	m.rwsize = ch->rwsize;
	m.watchdog_s = f->wd_ns / 1e9;
	m.watchdog_max_s = f->wd_max_ns / 1e9;
	m.guard_on = f->guard_on;
	m.guard_s = f->guard_ms / 1000.0;
	m.tcp_guard = f->tcp_guard;
	memcpy(mi, &m, size < sizeof(m) ? size : sizeof(m));
	return 0;
}

struct fuse_session *fuse_get_session(struct fuse *f)
{
	return f ? &f->se : NULL;
}

int fuse_set_signal_handlers(struct fuse_session *se)
{
	struct sigaction sa;

	(void)se;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = fuse_signal_handler;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGHUP, &sa, NULL) || sigaction(SIGINT, &sa, NULL) ||
	    sigaction(SIGTERM, &sa, NULL))
		return -1;
	sa.sa_handler = SIG_IGN;
	return sigaction(SIGPIPE, &sa, NULL);
}

void fuse_remove_signal_handlers(struct fuse_session *se)
{
	struct sigaction sa;

	(void)se;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_DFL;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGHUP, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
}

void fuse_exit(struct fuse *f)
{
	if (f) {
		char c = 'x';
		f->exited = 1;
		(void)!write(f->wake[1], &c, 1);
	}
}

/*
 * The device is gone: requests return IO at once and no longer reach the file system; fuse_loop
 * is woken up so that it returns, after which fuse_unmount goes straight to a forced unmount.
 * Only sets flags / writes to the wake-up pipe, so it may be called from any thread (and from a
 * signal handler).
 */
void nfuse_device_lost(struct fuse *f, const char *reason)
{
	char c = 'l';

	if (!f)
		f = g_fuse;
	if (!f || f->lost)
		return;
	f->srv.dev_lost = 1;
	gettimeofday(&f->t_lost, NULL);
	f->lost = 1;
	f->exited = 1;
	nfuse_log("device lost (%s): all further requests return IO; force-unmounting %s, then exiting",
		  reason ? reason : "no reason given", f->ch ? f->ch->mnt : "?");
	(void)!write(f->wake[1], &c, 1);
}

int fuse_loop(struct fuse *f)
{
	int absent = 0;

	if (!f || !f->ch)
		return -1;
	g_fuse = f;
	guard_start(f);		/* fork the guard process first, then start the server threads (no request is being handled at fork time) */
	if (nfuse_tp_resume(&f->tp)) {
		nfuse_log("failed to start the server threads");
		return -1;
	}
	nfuse_log("serving (pid %d)", (int)getpid());
	g_log_sync = g_log_sync_env;
	f->loop_beat = nfuse_uptime_ns();
	f->loop_running = 1;
	wd_start(f);
	while (!f->exited) {
		struct pollfd p = { .fd = f->wake[0], .events = POLLIN };
		char buf[64];
		int nconns;

		f->loop_beat = nfuse_uptime_ns();
		if (poll(&p, 1, 1000) > 0)
			while (read(f->wake[0], buf, sizeof(buf)) > 0)
				;
		if (f->exited)
			break;
		/*
		 * Detecting an unmount: the client sends no request at all when unmounting, it only closes
		 * the connection. All connections closed and not in the mount table -> unmounted; absent
		 * from the mount table for 3 consecutive seconds also counts (in case a disconnect event was
		 * missed). Still mounted but the connection is closed -> wait for the client to reconnect.
		 */
		nconns = nfuse_tp_nconns(&f->tp);
		if (!nfuse_is_nfs_mounted(f->ch->mnt, f->ch->from)) {
			if (nconns == 0 || ++absent >= 3) {
				nfuse_log("unmount detected (%d connections), stopping service", nconns);
				f->ch->mounted = 0;
				break;
			}
		} else {
			absent = 0;
		}
	}
	f->loop_running = 0;
	f->end_reason = f->lost ? "device_lost" : f->exited ? "exit_requested" : "unmounted";
	if (f->lost)
		nfuse_log("device lost, fuse_loop returns");
	else if (f->exited)
		nfuse_log("exit requested");
	return 0;
}

/* Real memory of this process (same as footprint / "Memory" in Activity Monitor): current and peak, in MB */
static void phys_footprint(double *cur, double *peak)
{
	task_vm_info_data_t vi;
	mach_msg_type_number_t n = TASK_VM_INFO_COUNT;

	*cur = *peak = 0;
	if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &n) !=
	    KERN_SUCCESS)
		return;
	*cur = vi.phys_footprint / 1048576.0;
	if (n >= TASK_VM_INFO_REV2_COUNT)
		*peak = vi.ledger_phys_footprint_peak / 1048576.0;
}

/* Unmount + stop the server threads (they keep running during the unmount) */
static void shutdown_mount(struct fuse *f)
{
	struct fuse_chan *ch = f->ch;

	if (ch && ch->mounted) {
		if (f->tp_ok)
			nfuse_tp_resume(&f->tp);
		if (f->lost) {
			/* Device gone: skip the normal unmount (it waits for the client to write back, which can only fail or hang) and force it right away */
			struct timeval tv;
			int rc = nfuse_unmount_force(ch->mnt, ch->from, nfuse_log);

			gettimeofday(&tv, NULL);
			nfuse_log("device lost: forced unmount of %s %s, %.3f s after detection", ch->mnt,
				  rc == 0 ? "succeeded" : "failed",
				  (tv.tv_sec - f->t_lost.tv_sec) +
				  (tv.tv_usec - f->t_lost.tv_usec) / 1e6);
		} else if (nfuse_unmount(ch->mnt, ch->from, nfuse_log) == 0)
			nfuse_log("unmounted %s", ch->mnt);
		ch->mounted = 0;
	}
	if (f->tp_ok) {
		f->pool_bytes = nfuse_tp_pool_bytes(&f->tp);
		nfuse_tp_shutdown(&f->tp);
		f->tp_ok = 0;
		if (ch)
			ch->lfd = -1;
	}
}

void fuse_unmount(const char *mountpoint, struct fuse_chan *ch)
{
	(void)mountpoint;
	if (!ch)
		return;
	if (ch->f) {
		shutdown_mount(ch->f);
		ch->f->ch = NULL;
	} else if (ch->lfd >= 0) {
		close(ch->lfd);
	}
	chan_cleanup_sock(ch);
	free(ch);
}

static void log_stats(struct fuse *f)
{
	char bs[1024];
	char ops[2048];
	size_t ol = 0;
	int i;

	if (!g_logf && !g_log_stderr)
		return;
	nfuse_backend_fuse_stats(&f->srv.be, bs, sizeof(bs));
	ops[0] = 0;
	for (i = NFS4_OP_MIN; i <= NFS4_OP_MAX && ol < sizeof(ops) - 64; i++)
		if (f->srv.op_count[i])
			ol += (size_t)snprintf(ops + ol, sizeof(ops) - ol, " %s=%llu/%llu",
					       nfs4_op_name((uint32_t)i),
					       (unsigned long long)f->srv.op_count[i],
					       (unsigned long long)f->srv.op_err[i]);
	{
		double cur, peak;
		phys_footprint(&cur, &peak);
		nfuse_log("stats: COMPOUND %llu, STALE %llu, NOT_SAME %llu, connections total %llu, rejected %llu, buffer pool %zu bytes, process memory footprint current %.1f MB / peak %.1f MB",
			 (unsigned long long)f->srv.compounds,
			 (unsigned long long)f->srv.stale_fh,
			 (unsigned long long)f->srv.readdir_notsame,
			 (unsigned long long)f->tp.total_conns,
			 (unsigned long long)f->tp.rejected, f->pool_bytes, cur, peak);
	}
	nfuse_log("writes: %llu bytes, %llu stable writes, %llu COMMITs; DRC %llu hits / %llu entries stored (currently %u entries, %zu bytes); %llu seqid replays, %llu BAD_SEQID; %llu NO_GRACE, %llu old clientids accepted; %llu test reply drops, %llu simulated restarts",
		  (unsigned long long)f->srv.write_bytes,
		  (unsigned long long)f->srv.stable_writes,
		  (unsigned long long)f->srv.commits,
		  (unsigned long long)f->srv.drc.hits,
		  (unsigned long long)f->srv.drc.inserts, f->srv.drc.n, f->srv.drc.bytes,
		  (unsigned long long)f->srv.seq_replays,
		  (unsigned long long)f->srv.bad_seqid,
		  (unsigned long long)f->srv.no_grace,
		  (unsigned long long)f->srv.resurrected,
		  (unsigned long long)f->srv.test_dropped,
		  (unsigned long long)f->srv.reboots);
	nfuse_log("operations (count/failed): %s", ops);
	nfuse_log("watchdog headroom: oldest in-flight request %.2f s, longest stretch without device progress while a request was in flight %.2f s (threshold %.0f s, sampled every 0.2 s)",
		  f->wd_age_max_ns / 1e9, f->wd_quiet_max_ns / 1e9, f->wd_ns / 1e9);
	nfuse_log("server processing time: total %.3f s, longest single %.3f s (%s), %llu of >= 1 s",
		  f->srv.busy_ns / 1e9, f->srv.busy_max_ns / 1e9, "holding the global lock",
		  (unsigned long long)f->srv.busy_slow);
	if (f->tcp_guard)
		nfuse_log("local security: %llu connections rejected, %llu untrusted connections, %llu requests denied on untrusted connections, %llu promoted to trusted by presenting a valid file handle, longest process scan %.1f ms",
			  (unsigned long long)f->guard_rejected, (unsigned long long)f->guard_untrusted,
			  (unsigned long long)f->srv.gate_denied, (unsigned long long)f->srv.gate_proved,
			  f->guard_scan_ms_max);
	if (f->lost)
		nfuse_log("%llu operations answered with IO after the device was lost",
			  (unsigned long long)f->srv.lost_replies);
	nfuse_log("backend: %s", bs);
	nfuse_backend_fuse_stats_m3(&f->srv.be, bs, sizeof(bs));
	nfuse_log("names/xattr:%s | namedattr_ops=%llu xattr_trunc_on_write=%llu attr_names=%u | case_scan=%s dirs=%u entries=%llu conflicts=%u normdups=%u ms=%.1f",
		  bs, (unsigned long long)f->srv.namedattr_ops,
		  (unsigned long long)f->srv.xattr_trunc_on_write, f->srv.nattr_names,
		  !f->case_scanned ? "none" : f->case_scan.complete ? "complete" : "partial",
		  f->case_scan.dirs, (unsigned long long)f->case_scan.entries,
		  f->case_scan.conflicts, f->case_scan.normdups, f->case_scan.ms);
	nfuse_log("virtual ._ files:adf_lookups=%llu adf_virtual=%llu adf_builds=%llu adf_sessions=%llu adf_creates=%llu adf_reads=%llu adf_read_bytes=%llu adf_writes=%llu adf_write_bytes=%llu adf_applies=%llu adf_attr_writes=%llu adf_attr_same=%llu adf_attr_removes=%llu adf_removes=%llu adf_renames=%llu adf_invalid=%llu adf_alias_adds=%llu adf_alias_hits=%llu adf_alias_removes=%llu adf_pre_bytes=%zu",
		  (unsigned long long)f->srv.adf_lookups, (unsigned long long)f->srv.adf_virtual,
		  (unsigned long long)f->srv.adf_builds, (unsigned long long)f->srv.adf_sessions,
		  (unsigned long long)f->srv.adf_creates, (unsigned long long)f->srv.adf_reads,
		  (unsigned long long)f->srv.adf_read_bytes, (unsigned long long)f->srv.adf_writes,
		  (unsigned long long)f->srv.adf_write_bytes, (unsigned long long)f->srv.adf_applies,
		  (unsigned long long)f->srv.adf_attr_writes, (unsigned long long)f->srv.adf_attr_same,
		  (unsigned long long)f->srv.adf_attr_removes, (unsigned long long)f->srv.adf_removes,
		  (unsigned long long)f->srv.adf_renames, (unsigned long long)f->srv.adf_invalid,
		  (unsigned long long)f->srv.adf_alias_adds, (unsigned long long)f->srv.adf_alias_hits,
		  (unsigned long long)f->srv.adf_alias_removes,
		  f->srv.adf_pre_bytes);
}

void fuse_destroy(struct fuse *f)
{
	uint32_t i;

	if (!f || f->destroyed)
		return;
	f->destroyed = 1;
	/* An error path may call destroy before unmount: unmount first if still mounted (needs the server threads) */
	shutdown_mount(f);
	/* The next steps all enter the file system (the final flush may hang on an unresponsive device): the watchdog keeps watching */
	f->extra_name = "destroy";
	f->extra_t0 = nfuse_uptime_ns();
	nfuse_adf_flush_all(&f->srv);	/* virtual `._X` sessions not yet written back (the client unmounted without closing them) */
	nfuse_state_free(&f->srv);	/* files the client left open (forced unmount etc.) are closed on its behalf before the file system cleans up */
	if (f->op.destroy)
		f->op.destroy(f->ctx.private_data);
	f->extra_t0 = 0;
	wd_stop(f);
	if (f->guard_fd >= 0) {
		close(f->guard_fd);	/* the guard process sees EOF, finds the mount gone and exits by itself */
		f->guard_fd = -1;
	}
	log_stats(f);
	nfuse_backend_fuse_free(&f->srv.be);
	for (i = 0; i < f->srv.nattr_names; i++)
		free(f->srv.attr_names[i]);
	free(f->srv.attr_names);
	free(f->srv.attr_ht);
	nfuse_adf_free_all(&f->srv);
	if (f->ch)
		f->ch->f = NULL;
	close(f->wake[0]);
	close(f->wake[1]);
	if (g_fuse == f)
		g_fuse = NULL;
	free(f);
}

struct fuse_context *fuse_get_context(void)
{
	if (g_fuse)
		return &g_fuse->ctx;
	g_dummy_ctx.uid = getuid();
	g_dummy_ctx.gid = getgid();
	return &g_dummy_ctx;
}
