/*
 * main.c - entry point of the prototype server: openmacfuse-m0
 *
 * Exports an in-memory fake directory tree that mount_nfs can mount directly, for verification.
 * Usage:
 *   openmacfuse-m0 [-a 127.0.0.1] [-p port] [-n export-name] [-U unix-socket-path] [-v]
 *     -p 0 (the default) lets the system pick a port; after start-up one line is printed on stdout,
 *        "LISTEN tcp 127.0.0.1 <port>" or "LISTEN unix <path>", which scripts use to mount.
 *     -n export name, default OPENMACFUSEM0: the mount spec is 127.0.0.1:/OPENMACFUSEM0 (Finder uses the last
 *        path component as the volume name);
 *        with -n "" the spec is 127.0.0.1:/.
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "backend.h"
#include "nfs4_ops.h"
#include "nfs4_state.h"
#include "server.h"

static volatile int g_stop;

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;	/* accept is interrupted by the signal (SA_RESTART is not set), so the loop exits right away */
}

static void usage(const char *p)
{
	fprintf(stderr,
		"usage: %s [-a address] [-p port] [-n export-name] [-U unix-socket] [-v]\n", p);
}

static void print_stats(struct openmacfuse_server *s)
{
	int i;

	fprintf(stderr, "[openmacfuse] exit statistics: %llu COMPOUNDs\n",
		(unsigned long long)s->compounds);
	for (i = NFS4_OP_MIN; i <= NFS4_OP_MAX; i++)
		if (s->op_count[i])
			fprintf(stderr, "[openmacfuse]   %-20s %8llu  failed %llu\n",
				nfs4_op_name((uint32_t)i),
				(unsigned long long)s->op_count[i],
				(unsigned long long)s->op_err[i]);
}

int main(int argc, char **argv)
{
	static struct openmacfuse_server srv;
	const char *addr = "127.0.0.1", *unix_path = NULL, *name = "OPENMACFUSEM0";
	uint16_t port = 0, bound = 0;
	struct sigaction sa;
	int ch, lfd, verbose = 0;

	while ((ch = getopt(argc, argv, "a:p:n:U:vh")) != -1) {
		switch (ch) {
		case 'a': addr = optarg; break;
		case 'p': port = (uint16_t)atoi(optarg); break;
		case 'n': name = optarg; break;
		case 'U': unix_path = optarg; break;
		case 'v': verbose = 1; break;
		default:  usage(argv[0]); return 2;
		}
	}

	openmacfuse_server_init(&srv, name);
	srv.verbose = verbose;
	if (openmacfuse_backend_mem_init(&srv.be) != 0) {
		fprintf(stderr, "backend initialization failed\n");
		return 1;
	}

	if (unix_path) {
		lfd = openmacfuse_listen_unix(unix_path);
		if (lfd < 0) {
			fprintf(stderr, "listen on %s failed: %s\n", unix_path, strerror(errno));
			return 1;
		}
		printf("LISTEN unix %s\n", unix_path);
	} else {
		lfd = openmacfuse_listen_tcp(addr, port, &bound);
		if (lfd < 0) {
			fprintf(stderr, "listen on %s:%u failed: %s\n", addr, port, strerror(errno));
			return 1;
		}
		printf("LISTEN tcp %s %u\n", addr, bound);
	}
	fflush(stdout);

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	openmacfuse_serve(&srv, lfd, &g_stop);	/* lfd is already closed on return */
	if (unix_path)
		unlink(unix_path);
	print_stats(&srv);
	return 0;
}
