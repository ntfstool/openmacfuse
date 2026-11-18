/*
 * unit_fork_cf.c -- using CoreFoundation / IOKit in a forked (not exec'ed) child must not be aborted by the objc runtime (macOS 27)
 *
 * How to reproduce: the process has started a thread at some point (a joined one counts), then
 * forks; the first time the child initialises an objc class that has +initialize, the objc
 * runtime of macOS 27 aborts. A file system that reads bitmaps on several threads before calling
 * fuse_main crashes exactly this way.
 * This test follows the order of fuse_main: start a thread -> openmacfuse_fork_warmup -> fork -> the
 * child does NFC, case folding and the IOKit query of the device watcher (I/O statistics of
 * disk0; only reads the registry, does not touch the device). A second run without the warm-up
 * serves as a control (expected to crash on macOS 27; only recorded, not counted as a failure).
 * Nothing is mounted; runs on the development machine.
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "names.h"
#include "devwatch.h"

static void *noop(void *a) { return a; }

static int child_work(void)
{
	char out[64];
	int ok = 0;

	if (openmacfuse_name_nfc("e\xcc\x81", 3, out, sizeof(out)) != 2 || strcmp(out, "\xc3\xa9") != 0)
		return 2;
	if (openmacfuse_name_fold_default("\xc3\xa9", out, sizeof(out)) < 0)
		return 3;
	(void)openmacfuse_devwatch_iostat("disk0", &ok);
	return ok ? 0 : 4;
}

/* Returns the child's result: 0 fine; >0 exit code of the child; <0 killed by a signal (-signal) */
static int run(int warm)
{
	pid_t p;
	int st;

	fflush(NULL);
	if (warm)
		openmacfuse_fork_warmup();
	p = fork();
	if (p == 0)
		_exit(child_work());
	if (p < 0 || waitpid(p, &st, 0) != p)
		return 99;
	return WIFSIGNALED(st) ? -WTERMSIG(st) : WEXITSTATUS(st);
}

int main(void)
{
	pthread_t t;
	int r0, r1, fails = 0;

	pthread_create(&t, NULL, noop, NULL);
	pthread_join(t, NULL);		/* from here on the process counts as "has started threads" */
	/* The control has to run first: once these classes have been initialised in the parent, the effect can no longer be observed */
	r0 = run(0);
	printf("  [NOTE] without warm-up: child %s\n", r0 == -SIGABRT ? "got SIGABRT (objc fork abort, the macOS 27 behaviour)" :
	       r0 == 0 ? "fine (this system does not abort)" : "abnormal");
	r1 = run(1);
	if (r1 == 0)
		printf("  [PASS] fork after openmacfuse_fork_warmup: NFC / folding / IOKit query work in the child\n");
	else if (r1 == 4)
		printf("  [PASS] fork after openmacfuse_fork_warmup: child did not crash (the I/O statistics of disk0 cannot be read on this machine)\n");
	else {
		printf("  [FAIL] fork after openmacfuse_fork_warmup: child result %d\n", r1);
		fails++;
	}
	return fails ? 1 : 0;
}
