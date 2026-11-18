/*
 * unit_fuse_lib.c -- unit tests for a few library behaviours on the fuse_main path (run on the development machine, nothing is mounted)
 *
 *   make test
 *
 * Uses the real fuse_mount / fuse_new but makes the file system's getattr("/") fail: fuse_new
 * returns NULL before it gets to mount_nfs, so only the local listening socket (a 0700
 * directory under $TMPDIR) and the mount-point lock file under /tmp are created; nothing is mounted.
 *   - fuse_new fails (init was called) -> the library calls destroy once; its argument and fuse_get_context are both the return value of init
 *   - no init -> destroy is not called
 *   - nosuid / nodev / noexec / suid are parsed by the library (mount flags in the fuse_mount log line)
 *   - openmacfuse_mount_nfs_wait returns 1 at once when there is no mount_nfs
 *   - openmacfuse_end_reason is NULL-safe
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#define FUSE_USE_VERSION 26
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fuse.h"
#include "openmacfuse.h"
#include "../src/mounter.h"
#include "../src/synth_ino.h"

static int g_fail, g_pass;

#define CHECK(cond, ...) do { \
	if (cond) { g_pass++; } else { g_fail++; fprintf(stderr, "  [FAIL] %s:%d: ", __FILE__, __LINE__); \
		fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static int g_inits, g_destroys;
static void *g_darg, *g_dctx;

static void *t_init(struct fuse_conn_info *c)
{
	g_inits++;
	return (void *)0x5a5a;
}
static void t_destroy(void *p)
{
	g_destroys++;
	g_darg = p;
	g_dctx = fuse_get_context()->private_data;
}
static int t_getattr_fail(const char *p, struct stat *st) { return -EIO; }

static int log_has(const char *log, const char *needle)
{
	FILE *f = fopen(log, "r");
	char line[2048];
	int hit = 0;

	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f))
		if (strstr(line, needle))
			hit = 1;
	fclose(f);
	return hit;
}

static void test_destroy_on_failed_new(const char *mnt)
{
	struct fuse_operations ops;
	char *argv[] = { "t", "-o", "volname=T", NULL };
	struct fuse_args a = FUSE_ARGS_INIT(3, argv);
	struct fuse_chan *ch;
	struct fuse *f;

	memset(&ops, 0, sizeof(ops));
	ops.getattr = t_getattr_fail;
	ops.init = t_init;
	ops.destroy = t_destroy;
	ch = fuse_mount(mnt, &a);
	CHECK(ch != NULL, "fuse_mount (only creates the listening socket)");
	if (!ch)
		return;
	f = fuse_new(ch, &a, &ops, sizeof(ops), (void *)0x1111);
	CHECK(f == NULL, "getattr(\"/\") fails -> fuse_new returns NULL before mounting");
	CHECK(g_inits == 1 && g_destroys == 1, "init was called -> destroy once (inits=%d destroys=%d)", g_inits, g_destroys);
	CHECK(g_darg == (void *)0x5a5a && g_dctx == (void *)0x5a5a,
	      "the argument of destroy and fuse_get_context()->private_data are both the return value of init (%p / %p)", g_darg, g_dctx);
	fuse_unmount(mnt, ch);

	/* No init: destroy is not called */
	g_inits = g_destroys = 0;
	ops.init = NULL;
	ch = fuse_mount(mnt, &a);
	f = ch ? fuse_new(ch, &a, &ops, sizeof(ops), NULL) : NULL;
	CHECK(ch && !f && g_destroys == 0, "no init callback -> destroy is not called");
	if (ch)
		fuse_unmount(mnt, ch);
}

static void test_mount_flags(const char *mnt, const char *log)
{
	char *argv[] = { "t", "-o", "volname=T,ro,nosuid,nodev,noexec", NULL };
	char *argv2[] = { "t", "-o", "nosuid,nodev,suid,dev", NULL };
	struct fuse_args a = FUSE_ARGS_INIT(3, argv), b = FUSE_ARGS_INIT(3, argv2);
	struct fuse_chan *ch;

	ch = fuse_mount(mnt, &a);
	CHECK(ch && log_has(log, "mount flags [ ro nosuid nodev noexec]"), "the library understands nosuid / nodev / noexec (and will pass them to mount_nfs)");
	if (ch)
		fuse_unmount(mnt, ch);
	ch = fuse_mount(mnt, &b);
	CHECK(ch && log_has(log, "mount flags []"), "a later suid / dev cancels an earlier nosuid / nodev");
	if (ch)
		fuse_unmount(mnt, ch);
	CHECK(!log_has(log, "ignoring unknown mount option: nosuid") && !log_has(log, "ignoring unknown mount option: suid"),
	      "no longer treated as unknown options");
}

static void test_synth(void)
{
	struct openmacfuse_synth *s = openmacfuse_synth_new();
	uint64_t a, b, c, d, root;
	char p[64];
	int i, ok = 1;

	root = openmacfuse_synth_get(s, "/");
	a = openmacfuse_synth_get(s, "/dir");
	b = openmacfuse_synth_get(s, "/dir/f");
	c = openmacfuse_synth_get(s, "/dir/sub/g");
	CHECK(root == 1 && a > 1 && b > 1 && c > 1 && a != b && b != c, "allocated by path: root is 1, all others differ");
	CHECK(openmacfuse_synth_get(s, "/dir/f") == b, "the number of a path does not change");
	openmacfuse_synth_rename(s, "/dir", "/moved");
	CHECK(openmacfuse_synth_get(s, "/moved") == a && openmacfuse_synth_get(s, "/moved/f") == b &&
	      openmacfuse_synth_get(s, "/moved/sub/g") == c, "directory rename: its own number and those below it move along");
	d = openmacfuse_synth_get(s, "/dir");
	CHECK(d != a, "the old path showing up again gets a new number");
	openmacfuse_synth_rename(s, "/moved/f", "/dirx");
	CHECK(openmacfuse_synth_get(s, "/dirx") == b, "rename over an existing entry: the number follows the source");
	openmacfuse_synth_drop(s, "/moved", 1);
	CHECK(openmacfuse_synth_get(s, "/moved/sub/g") != c, "removing a directory removes the paths below it");
	openmacfuse_synth_drop(s, "/", 1);
	CHECK(openmacfuse_synth_get(s, "/") == 1, "the root is always kept");
	for (i = 0; i < 5000; i++) {
		snprintf(p, sizeof(p), "/many/%d", i);
		if (openmacfuse_synth_get(s, p) == 0)
			ok = 0;
	}
	for (i = 0; i < 5000 && ok; i++) {
		snprintf(p, sizeof(p), "/many/%d", i);
		if (openmacfuse_synth_get(s, p) != openmacfuse_synth_get(s, p))
			ok = 0;
	}
	CHECK(ok && openmacfuse_synth_count(s) >= 5000, "all 5000 entries present after growing (%zu)", openmacfuse_synth_count(s));
	openmacfuse_synth_free(s);
}

int main(void)
{
	char tmp[] = "/tmp/openmacfuse-unit-lib.XXXXXX", mnt[600], log[600], cmd[700];

	if (!mkdtemp(tmp)) {
		perror("mkdtemp");
		return 1;
	}
	snprintf(mnt, sizeof(mnt), "%s/mnt", tmp);
	snprintf(log, sizeof(log), "%s/openmacfuse.log", tmp);
	mkdir(mnt, 0755);
	setenv("OPENMACFUSE_LOG", log, 1);
	setenv("OPENMACFUSE_GUARD_WAIT_MS", "0", 1);
	setenv("OPENMACFUSE_TRANSPORT", "tcp", 1);	/* only listens on a random 127.0.0.1 port, no socket directory is created */

	test_destroy_on_failed_new(mnt);
	test_mount_flags(mnt, log);
	test_synth();
	openmacfuse_mount_pid = 0;
	CHECK(openmacfuse_mount_nfs_wait(100) == 1, "returns 1 at once when there is no mount_nfs");
	CHECK(openmacfuse_end_reason(NULL)[0] == 0, "openmacfuse_end_reason(NULL) is the empty string");

	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmp);
	(void)!system(cmd);
	printf("unit_fuse_lib: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
