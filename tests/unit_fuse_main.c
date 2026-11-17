/*
 * unit_fuse_main.c -- unit tests for fuse_parse_cmdline / the fuse_main flow (run on the development machine, nothing is mounted)
 *
 *   make -C nfuse test
 *
 * The mount steps (fuse_mount / fuse_new / fuse_loop / fuse_unmount / fuse_destroy) are replaced
 * with fakes (nfuse_main_hooked); no server thread is started and mount_nfs is never called.
 * The backgrounding tests really fork: they verify that the parent exits with 0 once mounted,
 * that the caller's output pipe reaches end-of-file after the mount (while the child keeps
 * running), that init is called in the child, that the exit code is passed through when the
 * mount fails, that destroy is called to clean up after init when the mount fails, and that the
 * child unmounts and exits when the caller gives up half-way.
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#define FUSE_USE_VERSION 29
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fuse.h"
#include "../src/fuse_main_int.h"
#include "../src/report.h"

static int g_fail, g_pass;

#define CHECK(cond, ...) do { \
	if (cond) { g_pass++; } else { g_fail++; fprintf(stderr, "  [FAIL] %s:%d: ", __FILE__, __LINE__); \
		fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static const char *joined(const struct fuse_args *a)
{
	static char buf[4096];
	size_t n = 0;
	int i;

	buf[0] = 0;
	for (i = 0; i < a->argc; i++)
		n += (size_t)snprintf(buf + n, sizeof(buf) - n, "%s%s", i ? "|" : "", a->argv[i]);
	return buf;
}

/* ---------- fuse_parse_cmdline ---------- */

static void test_parse_cmdline(void)
{
	{
		char *argv[] = { "fs", "/mnt/x", "-f", "-s", "-o", "ro,volname=V", "-d", NULL };
		struct fuse_args a = FUSE_ARGS_INIT(7, argv);
		char *mp = NULL;
		int mt = -1, fg = -1;

		CHECK(fuse_parse_cmdline(&a, &mp, &mt, &fg) == 0, "parse succeeds");
		CHECK(mp && strcmp(mp, "/mnt/x") == 0, "mount point: %s", mp ? mp : "(null)");
		CHECK(mt == 0 && fg == 1, "-s -> single-threaded, -f -> foreground (mt=%d fg=%d)", mt, fg);
		CHECK(strcmp(joined(&a), "fs|-o|ro,volname=V|-d") == 0, "-f / -s removed, -o / -d kept: %s", joined(&a));
		free(mp);
		fuse_opt_free_args(&a);
	}
	{
		char *argv[] = { "fs", "-odebug", "/m", NULL };
		struct fuse_args a = FUSE_ARGS_INIT(3, argv);
		char *mp = NULL;
		int mt = -1, fg = -1;

		CHECK(fuse_parse_cmdline(&a, &mp, &mt, &fg) == 0 && fg == 1 && mt == 1,
		      "-o debug also means foreground; multi-threaded by default (fg=%d mt=%d)", fg, mt);
		CHECK(strcmp(joined(&a), "fs|-o|debug") == 0, "debug is left for the library: %s", joined(&a));
		free(mp);
		fuse_opt_free_args(&a);
	}
	{
		char *argv[] = { "fs", "-h", "/m", NULL };
		struct fuse_args a = FUSE_ARGS_INIT(3, argv);
		char *mp = (char *)1;

		CHECK(fuse_parse_cmdline(&a, &mp, NULL, NULL) == 0 && mp == NULL, "-h: mount point is NULL");
		fuse_opt_free_args(&a);
	}
	{
		char *argv[] = { "fs", "/a", "/b", NULL };
		struct fuse_args a = FUSE_ARGS_INIT(3, argv);
		char *mp = NULL;

		CHECK(fuse_parse_cmdline(&a, &mp, NULL, NULL) == -1 && mp == NULL, "two mount points are an error");
	}
}

/* ---------- Argument fix-ups ---------- */

static void test_prepare(void)
{
	unsetenv("NFUSE_OPTS");
	{
		char *argv[] = { "fs", "-o", "ro,nosuid,nodev", NULL };
		struct fuse_args a = FUSE_ARGS_INIT(3, argv);

		CHECK(nfuse_main_prepare_args(&a, "/Volumes/My Disk/") == 0, "fix-up succeeds");
		CHECK(strcmp(joined(&a), "fs|-onfuse_case=sensitive,nfuse_xattr=appledouble|-o|ro,nosuid,nodev|-ovolname=My Disk") == 0,
		      "defaults first, volume name added, nosuid etc. left as is for the library: %s", joined(&a));
		fuse_opt_free_args(&a);
	}
	{
		char *argv[] = { "fs", "-ovolname=X,noexec,nfuse_case=insensitive", NULL };
		struct fuse_args a = FUSE_ARGS_INIT(2, argv);

		setenv("NFUSE_OPTS", "nfuse_xattr=adfile", 1);
		CHECK(nfuse_main_prepare_args(&a, "/Volumes/a,b") == 0, "fix-up succeeds");
		CHECK(strcmp(joined(&a), "fs|-onfuse_case=sensitive,nfuse_xattr=appledouble|-ovolname=X,noexec,nfuse_case=insensitive|-onfuse_xattr=adfile") == 0,
		      "an existing volname is not added again; NFUSE_OPTS comes last: %s", joined(&a));
		unsetenv("NFUSE_OPTS");
		fuse_opt_free_args(&a);
	}
	{
		char *argv[] = { "fs", NULL };
		struct fuse_args a = FUSE_ARGS_INIT(1, argv);

		CHECK(nfuse_main_prepare_args(&a, "/Volumes/a,b") == 0 &&
		      strcmp(joined(&a), "fs|-onfuse_case=sensitive,nfuse_xattr=appledouble|-ovolname=a\\,b") == 0,
		      "comma in the volume name is escaped: %s", joined(&a));
		fuse_opt_free_args(&a);
	}
}

/* ---------- Fake mount steps ---------- */

static char g_log[2048];
static int g_fail_mount, g_fail_new, g_call_init_in_new, g_loop_sleep_ms, g_kill_parent_in_new;
static const char *g_marker;	/* file written in loop (proves the background process is running) */
static const char *g_initpid;	/* init writes its own pid here */
static const char *g_result;	/* the child finally writes rc / how the clean-up went */
static struct fuse *g_fake = (struct fuse *)0x1234;
static struct fuse_chan *g_fake_ch = (struct fuse_chan *)0x5678;
static char g_seen_args[1024];
static const struct fuse_operations *g_seen_op;

static void L(const char *s)
{
	size_t n = strlen(g_log);

	snprintf(g_log + n, sizeof(g_log) - n, "%s%s", n ? "," : "", s);
}

static void write_file(const char *p, const char *s)
{
	FILE *f;

	if (!p)
		return;
	f = fopen(p, "w");
	if (f) {
		fputs(s, f);
		fclose(f);
	}
}

static struct fuse_chan *f_mount(const char *mp, struct fuse_args *a)
{
	L("mount");
	snprintf(g_seen_args, sizeof(g_seen_args), "%s", joined(a));
	return g_fail_mount ? NULL : g_fake_ch;
}

static struct fuse *f_new(struct fuse_chan *ch, struct fuse_args *a, const struct fuse_operations *op,
			  size_t sz, void *ud)
{
	L("new");
	g_seen_op = op;
	if (g_call_init_in_new && op->init) {
		struct fuse_conn_info c;

		memset(&c, 0, sizeof(c));
		op->init(&c);	/* the fuse_new of nfuse also calls init before mounting */
	}
	if (g_kill_parent_in_new) {
		pid_t pp = getppid();

		kill(pp, SIGKILL);
		while (getppid() == pp)
			usleep(10000);
	}
	return g_fail_new ? NULL : g_fake;
}

static int f_sig(struct fuse_session *se)
{
	L("sig");
	signal(SIGPIPE, SIG_IGN);	/* the real fuse_set_signal_handlers ignores SIGPIPE too */
	return 0;
}
static void f_rsig(struct fuse_session *se) { L("rsig"); }
static struct fuse_session *f_sess(struct fuse *f) { return (struct fuse_session *)f; }
static int f_loop(struct fuse *f)
{
	L("loop");
	if (g_loop_sleep_ms)
		usleep((useconds_t)g_loop_sleep_ms * 1000);
	write_file(g_marker, "looped");
	return 0;
}
static void f_unmount(const char *mp, struct fuse_chan *ch) { L(ch == g_fake_ch ? "unmount" : "unmount?"); }
static void f_destroy(struct fuse *f) { L("destroy"); }
static void f_detach_record(void) { L("detach"); }
static const char *g_end = "unmounted";
static int f_minfo(struct fuse *f, struct nfuse_mount_info *mi, size_t sz)
{
	memset(mi, 0, sz);
	snprintf(mi->case_mode, sizeof(mi->case_mode), "sensitive");
	snprintf(mi->xattr_mode, sizeof(mi->xattr_mode), "appledouble");
	snprintf(mi->transport, sizeof(mi->transport), "tcp");
	mi->case_scan_complete = -1;
	mi->rwsize = 262144;
	mi->watchdog_s = 30;
	mi->watchdog_max_s = 600;
	mi->guard_on = 1;
	mi->guard_s = 20;
	return 0;
}
static const char *f_end(struct fuse *f) { return g_end; }

static const struct nfuse_main_hooks fake_fg = {
	.mount = f_mount, .fnew = f_new, .set_signals = f_sig, .remove_signals = f_rsig,
	.session = f_sess, .loop = f_loop, .unmount = f_unmount, .destroy = f_destroy,
	.detach = f_detach_record, .mount_info = f_minfo, .end_reason = f_end,
};

/* File system callbacks */
static int g_inits, g_destroys;
static void *g_destroy_arg;
static void *u_init(struct fuse_conn_info *c)
{
	char b[32];

	g_inits++;
	snprintf(b, sizeof(b), "%d", (int)getpid());
	write_file(g_initpid, b);
	return (void *)0xabc;
}
static void u_destroy(void *p)
{
	g_destroys++;
	g_destroy_arg = p;
	/* Some file systems' destroy takes private_data from fuse_get_context */
	if (fuse_get_context()->private_data != (void *)0xabc)
		g_destroy_arg = (void *)0xbad;
}
static int u_getattr(const char *p, struct stat *st) { return -ENOENT; }

static struct fuse_operations uops = {
	.getattr = u_getattr, .init = u_init, .destroy = u_destroy,
	.flag_nopath = 1, .flag_nullpath_ok = 1,
};

static void reset(void)
{
	g_log[0] = 0;
	g_fail_mount = g_fail_new = g_call_init_in_new = g_loop_sleep_ms = g_kill_parent_in_new = 0;
	g_marker = g_initpid = g_result = NULL;
	g_inits = g_destroys = 0;
	g_destroy_arg = NULL;
	g_seen_op = NULL;
	g_seen_args[0] = 0;
	g_end = "unmounted";
}

static void test_foreground_flow(void)
{
	char *argv[] = { "fuse2fs", "/Volumes/E", "-f", "-o", "volname=E,local,allow_other,fakeroot,nobrowse", NULL };
	int rc;

	reset();
	unsetenv("NFUSE_WATCH_DEV");
	unsetenv("NFUSE_TEST_DEVLOST_FILE");
	unsetenv("NFUSE_MOUNT_EXTRA");
	rc = nfuse_main_hooked(5, argv, &uops, sizeof(uops), NULL, &fake_fg);
	CHECK(rc == 0, "foreground returns 0 normally: %d", rc);
	CHECK(strcmp(g_log, "mount,new,sig,loop,rsig,unmount,destroy") == 0, "call order: %s", g_log);
	CHECK(strcmp(g_seen_args, "fuse2fs|-onfuse_case=sensitive,nfuse_xattr=appledouble|-o|volname=E,local,allow_other,fakeroot,nobrowse") == 0,
	      "arguments handed to fuse_mount: %s", g_seen_args);
	CHECK(g_seen_op == &uops, "the callback table is handed to fuse_new unchanged (clean-up after init / destroy is the library's job)");
}

static void test_foreground_errors(void)
{
	int rc;

	{
		char *argv[] = { "fs", "-f", NULL };

		reset();
		rc = nfuse_main_hooked(2, argv, &uops, sizeof(uops), NULL, &fake_fg);
		CHECK(rc == 2 && g_log[0] == 0, "no mount point given -> 2, nothing mounted (rc=%d log=%s)", rc, g_log);
	}
	{
		char *argv[] = { "fs", "-f", "/m", "/n", NULL };

		reset();
		rc = nfuse_main_hooked(4, argv, &uops, sizeof(uops), NULL, &fake_fg);
		CHECK(rc == 1, "surplus argument -> 1: %d", rc);
	}
	{
		char *argv[] = { "fs", "-h", NULL };

		reset();
		rc = nfuse_main_hooked(2, argv, &uops, sizeof(uops), NULL, &fake_fg);
		CHECK(rc == 0 && g_log[0] == 0, "-h -> 0, nothing mounted");
	}
	{
		char *argv[] = { "fs", "--version", "/m", NULL };

		reset();
		rc = nfuse_main_hooked(3, argv, &uops, sizeof(uops), NULL, &fake_fg);
		CHECK(rc == 0 && g_log[0] == 0, "--version -> 0, nothing mounted");
	}
	{
		char *argv[] = { "fs", "-f", "/m", NULL };

		reset();
		g_fail_mount = 1;
		rc = nfuse_main_hooked(3, argv, &uops, sizeof(uops), NULL, &fake_fg);
		CHECK(rc == 3 && strcmp(g_log, "mount") == 0, "fuse_mount fails -> 3 (%d, %s)", rc, g_log);
	}
	{
		char *argv[] = { "fs", "-f", "/m", NULL };

		reset();
		g_fail_new = 1;
		g_call_init_in_new = 1;
		rc = nfuse_main_hooked(3, argv, &uops, sizeof(uops), NULL, &fake_fg);
		CHECK(rc == 4, "mount fails -> 4: %d", rc);
		CHECK(strcmp(g_log, "mount,new,unmount") == 0, "channel cleaned up, loop not entered: %s", g_log);
		CHECK(g_inits == 1 && g_destroys == 0,
		      "fuse_main does not call destroy itself (the failure path of the library's fuse_new does, see unit_fuse_lib; not repeated)");
	}
}

/* Background: run fuse_main in a "caller" child process; returns the caller's exit code, *eof_ms = how long until end-of-file on the output pipe */
static int run_bg(char **argv, int argc, const struct nfuse_main_hooks *hk, double *eof_ms)
{
	int out[2], st = 0;
	pid_t caller;
	struct timeval t0, t1;
	char buf[256];

	if (pipe(out) < 0)
		return -1;
	gettimeofday(&t0, NULL);
	caller = fork();
	if (caller == 0) {
		int rc;

		close(out[0]);
		dup2(out[1], 1);
		dup2(out[1], 2);
		close(out[1]);
		rc = nfuse_main_hooked(argc, argv, &uops, sizeof(uops), NULL, hk);
		/* Only the background child (or an error) gets here: record the result */
		{
			char r[256];

			snprintf(r, sizeof(r), "rc=%d log=%s destroys=%d", rc, g_log, g_destroys);
			write_file(g_result, r);
		}
		_exit(rc ? 40 + rc : 0);
	}
	close(out[1]);
	while (read(out[0], buf, sizeof(buf)) > 0)
		;
	gettimeofday(&t1, NULL);
	close(out[0]);
	*eof_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_usec - t0.tv_usec) / 1000.0;
	waitpid(caller, &st, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

static int wait_file(const char *p, int ms)
{
	struct stat st;

	while (ms > 0) {
		if (stat(p, &st) == 0 && st.st_size > 0)
			return 1;
		usleep(20000);
		ms -= 20;
	}
	return 0;
}

static char *slurp(const char *p)
{
	static char b[512];
	FILE *f = fopen(p, "r");
	size_t n = 0;

	b[0] = 0;
	if (f) {
		n = fread(b, 1, sizeof(b) - 1, f);
		b[n] = 0;
		fclose(f);
	}
	return b;
}

static void test_background(const char *tmp)
{
	char marker[512], initpid[512], result[512];
	char *argv[] = { "exfat", "-s", "-o", "allow_other,default_permissions,fsname=/dev/disk9s1,ro", "/Volumes/X", NULL };
	struct nfuse_main_hooks hk = fake_fg;
	double eof_ms = 0;
	int rc;

	snprintf(marker, sizeof(marker), "%s/marker", tmp);
	snprintf(initpid, sizeof(initpid), "%s/initpid", tmp);
	snprintf(result, sizeof(result), "%s/result", tmp);

	/* 1. Mounted: the caller exits with 0; the output pipe ends right after the mount (the background keeps running for 1.5 seconds); init runs in the background process */
	reset();
	unlink(marker);
	unlink(initpid);
	unlink(result);
	g_marker = marker;
	g_initpid = initpid;
	g_result = result;
	g_call_init_in_new = 1;
	g_loop_sleep_ms = 1500;
	hk.detach = NULL;	/* use the real one: setsid, chdir, stdio -> /dev/null */
	rc = run_bg(argv, 5, &hk, &eof_ms);
	CHECK(rc == 0, "background mount: caller exit code 0: %d", rc);
	CHECK(eof_ms < 1200, "the caller's output pipe reaches end-of-file right after the mount (%.0f ms, background process still running)", eof_ms);
	CHECK(wait_file(marker, 5000), "the background process ran fuse_loop");
	CHECK(wait_file(result, 5000) && strstr(slurp(result), "rc=0 log=mount,new,sig,loop,rsig,unmount,destroy") != NULL,
	      "the background process ran the whole flow: %s", slurp(result));
	CHECK(wait_file(initpid, 100) && atoi(slurp(initpid)) > 0, "init was called");
	/* init is not in the caller process: the caller has exited, the pid written is that of the background process (the same process as result) */

	/* 2. Mount failed: the caller exits with the exit code of the background process (the error is handled only once, in the background process) */
	reset();
	unlink(result);
	g_result = result;
	g_fail_new = 1;
	g_call_init_in_new = 1;
	hk.detach = f_detach_record;
	rc = run_bg(argv, 5, &hk, &eof_ms);
	CHECK(rc == 44, "mount failed: caller exit code passed through (expected 44 = 40 + 4): %d", rc);
	CHECK(wait_file(result, 2000) && strstr(slurp(result), "rc=4 log=mount,new,unmount destroys=0") != NULL,
	      "the failure is cleaned up in the background process: %s", slurp(result));

	/* 3. The caller is killed during the mount: the background process notices nobody is waiting, unmounts, destroys and returns 5 */
	reset();
	unlink(result);
	g_result = result;
	g_kill_parent_in_new = 1;
	hk.detach = f_detach_record;
	rc = run_bg(argv, 5, &hk, &eof_ms);
	CHECK(rc == 128 + SIGKILL, "caller killed: %d", rc);
	CHECK(wait_file(result, 3000) && strstr(slurp(result), "rc=5 log=mount,new,sig,detach,rsig,unmount,destroy") != NULL,
	      "the background process unmounts and exits without entering the loop: %s", slurp(result));
}


/* ---------- NFUSE_REPORT telemetry ---------- */

static int count_lines(const char *p, const char *needle)
{
	FILE *f = fopen(p, "r");
	char line[4096];
	int n = 0;

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f))
		if (!needle || strstr(line, needle))
			n++;
	fclose(f);
	return n;
}

static int all_lines_shaped(const char *p)
{
	FILE *f = fopen(p, "r");
	char line[4096];
	int ok = 1, n = 0;

	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f)) {
		size_t l = strlen(line);

		n++;
		if (strncmp(line, "{\"ts\":", 6) != 0 || !strstr(line, ",\"role\":\"nfuse\",\"seq\":") ||
		    !strstr(line, ",\"tool\":\"fuse2fs\"") || l < 3 || line[l - 1] != '\n' || line[l - 2] != '}')
			ok = 0;
	}
	fclose(f);
	return ok && n > 0;
}

static void test_report(const char *tmp)
{
	char rep[512], lost[512];
	char *argv[] = { "/Applications/X.app/Contents/Resources/nfuse/fuse2fs", "/Volumes/E", "-f", NULL };
	int rc;

	snprintf(rep, sizeof(rep), "%s/rep.jsonl", tmp);
	snprintf(lost, sizeof(lost), "%s/LOST", tmp);
	unsetenv("NFUSE_WATCH_DEV");
	unsetenv("NFUSE_TEST_DEVLOST_FILE");

	/* The telemetry tool field is the real program name of this process (getprogname), not the argv[0] passed to fuse_main */
	setprogname("fuse2fs");

	/* 1. Normal unmount */
	reset();
	unlink(rep);
	setenv("NFUSE_REPORT", rep, 1);
	rc = nfuse_main_hooked(3, argv, &uops, sizeof(uops), NULL, &fake_fg);
	nfuse_report_close();
	CHECK(rc == 0, "foreground normal: %d", rc);
	CHECK(all_lines_shaped(rep), "every line is {\"ts\":...,\"role\":\"nfuse\",\"seq\":...,\"tool\":\"<program>\"...} (the common telemetry line format)");
	CHECK(count_lines(rep, "\"type\":\"start\"") == 1 && count_lines(rep, "\"foreground\":1") == 1, "one start line");
	CHECK(count_lines(rep, "\"type\":\"nfuse_mount\",\"tool\":\"fuse2fs\",\"case\":\"sensitive\"") == 1 &&
	      count_lines(rep, "\"xattr\":\"appledouble\",\"transport\":\"tcp\"") == 1 &&
	      count_lines(rep, "\"watchdog_s\":30,\"watchdog_max_s\":600,\"guard\":1,\"guard_s\":20") == 1,
	      "one nfuse_mount line with the expected fields");
	CHECK(count_lines(rep, "\"type\":\"exit\",\"tool\":\"fuse2fs\",\"code\":0,\"ret\":0,\"reason\":\"unmounted\"") == 1,
	      "exit: code 0 / unmounted");
	CHECK(count_lines(rep, NULL) == 3, "3 lines in total: %d", count_lines(rep, NULL));

	/* 2. Device lost: outcome code 23, fuse_main returns 0 */
	reset();
	unlink(rep);
	g_end = "device_lost";
	rc = nfuse_main_hooked(3, argv, &uops, sizeof(uops), NULL, &fake_fg);
	nfuse_report_close();
	CHECK(rc == 0 && count_lines(rep, "\"type\":\"exit\",\"tool\":\"fuse2fs\",\"code\":23,\"ret\":0,\"reason\":\"device_lost\"") == 1,
	      "device lost: exit code 23");

	/* 3. Mount failed */
	reset();
	unlink(rep);
	g_fail_new = 1;
	rc = nfuse_main_hooked(3, argv, &uops, sizeof(uops), NULL, &fake_fg);
	nfuse_report_close();
	CHECK(rc == 4 && count_lines(rep, "\"type\":\"mount_fail\",\"tool\":\"fuse2fs\",\"stage\":\"fuse_new\",\"ret\":4") == 1 &&
	      count_lines(rep, "\"code\":4,\"ret\":4,\"reason\":\"mount_failed\"") == 1 && count_lines(rep, "nfuse_mount") == 0,
	      "mount failed: mount_fail + exit code 4, no nfuse_mount");

	/* 4. The device watcher starts before mounting: the test hook file already exists -> one device_lost line (how=test_hook) */
	reset();
	unlink(rep);
	write_file(lost, "x");
	setenv("NFUSE_TEST_DEVLOST_FILE", lost, 1);
	g_loop_sleep_ms = 1300;
	rc = nfuse_main_hooked(3, argv, &uops, sizeof(uops), NULL, &fake_fg);
	nfuse_report_close();
	unsetenv("NFUSE_TEST_DEVLOST_FILE");
	CHECK(count_lines(rep, "\"type\":\"device_lost\",\"tool\":\"fuse2fs\",\"err\":6,\"how\":\"test_hook\"") == 1,
	      "device watcher: one device_lost line (reported only once): %d", count_lines(rep, "device_lost"));
	CHECK(count_lines(rep, "\"watch_dev\":1") == 2, "start / nfuse_mount both carry watch_dev=1");

	/* 5. NFUSE_REPORT not set: nothing is written */
	reset();
	unlink(rep);
	unsetenv("NFUSE_REPORT");
	rc = nfuse_main_hooked(3, argv, &uops, sizeof(uops), NULL, &fake_fg);
	CHECK(rc == 0 && count_lines(rep, NULL) == -1, "no file is created when NFUSE_REPORT is not set");
	unlink(lost);
}

int main(void)
{
	char tmp[] = "/tmp/nfuse-unit.XXXXXX";

	if (!mkdtemp(tmp)) {
		perror("mkdtemp");
		return 1;
	}
	test_parse_cmdline();
	test_prepare();
	test_foreground_flow();
	test_foreground_errors();
	test_background(tmp);
	test_report(tmp);
	{
		char cmd[600];

		snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmp);
		(void)!system(cmd);
	}
	printf("unit_fuse_main: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
