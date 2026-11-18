/*
 * server.h - transport layer: listen, accept connections, send and receive RPC records, dispatch
 * to the NFS handlers
 *
 * Threading model: one accept thread plus one thread per connection; all COMPOUNDs execute
 * serially under s->lock (the backend / file system callbacks need not consider concurrency).
 *
 * The threads can be "parked at a request boundary" and started again (openmacfuse_tp_pause /
 * openmacfuse_tp_resume):
 * the compatibility library mounts inside fuse_new, after which the file system may daemon()
 * (fork), and threads do not survive a fork; so they are stopped before fuse_new returns and
 * restarted in the child inside fuse_loop.
 * While stopped, the connections and the listening socket stay open and the client's requests
 * queue up in the sockets.
 *
 * Memory bound: send and receive buffers come from a pool of fixed size and fixed count (one
 * receive and one send block per connection, each rwsize + 64KB); it does not grow with file size
 * or request volume.
 * When the pool is full, new connections are refused.
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#ifndef OPENMACFUSE_SERVER_H
#define OPENMACFUSE_SERVER_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "nfs4_state.h"

/*
 * Listen on 127.0.0.1:port (port=0 lets the system pick); returns the listening socket, *bound is
 * the actual port
 */
int openmacfuse_listen_tcp(const char *addr, uint16_t port, uint16_t *bound);
/* Listen on a local unix socket path */
int openmacfuse_listen_unix(const char *path);

/* the system client opens one connection per mount; leave headroom for reconnects */
#define OPENMACFUSE_TP_MAX_CONNS	4

struct openmacfuse_conn;

struct openmacfuse_tp {
	struct openmacfuse_server	*s;
	int			lfd;		/* listening socket */
	int			wake_w;		/* optional: a byte is written here when a connection closes */

	pthread_mutex_t		mu;
	volatile int		pausing;	/* non-zero: each thread exits after finishing the request in hand */
	int			running;	/* whether the threads are running */
	pthread_t		acc_th;
	struct openmacfuse_conn	*conns;
	int			nconns;		/* connections currently open */
	uint64_t		total_conns;	/* connections accepted in total */
	uint64_t		rejected;	/* connections refused because the buffer pool was full */
	volatile uint64_t	acc_beat;	/* heartbeat of the accept thread (for the watchdog) */
	/*
	 * Optional (local-host security): called once per accepted connection.
	 * Returning -1 refuses it (it is closed); otherwise *trusted set to 0 marks it untrusted
	 * -- on such a connection only ops that do not touch files and requests whose "first op
	 * is a PUTFH with a valid handle" are let through (see nfs4_compound_gate), and it
	 * becomes trusted once a valid handle has been presented. NULL: every connection is
	 * trusted (unix sockets rely on directory permissions).
	 * openmacfuse_tp_init zeroes this; the caller sets it after init and before resume.
	 */
	int			(*accept_hook)(void *arg, int fd, int *trusted);
	void			*accept_arg;

	/*
	 * Buffer pool: at most 2 * OPENMACFUSE_TP_MAX_CONNS blocks of bufsize bytes each, allocated on
	 * demand and returned after use
	 */
	size_t			bufsize;
	void			*freebufs[2 * OPENMACFUSE_TP_MAX_CONNS];
	int			nfree;
	int			nalloc;		/* blocks allocated (<= the limit) */
};

int  openmacfuse_tp_init(struct openmacfuse_tp *tp, struct openmacfuse_server *s, int lfd, int wake_w);
/* Start the accept thread and a thread for every existing connection */
int  openmacfuse_tp_resume(struct openmacfuse_tp *tp);
/*
 * Have all threads exit after finishing the request in hand, and join them; connections stay open
 */
void openmacfuse_tp_pause(struct openmacfuse_tp *tp);
/* Stop the threads, close all connections and the listening socket, free the buffer pool */
void openmacfuse_tp_shutdown(struct openmacfuse_tp *tp);
/*
 * For the watchdog: the earliest start time among requests being processed
 * (0 = none), the stalest heartbeat among idle connection threads
 * (0 = none; threads blocked reading a request do not count), and the accept thread's heartbeat
 * (0 when the threads are not running). All times are openmacfuse_uptime_ns
 */
void openmacfuse_tp_health(struct openmacfuse_tp *tp, uint64_t *oldest_req, uint64_t *oldest_idle_beat,
		     uint64_t *acc_beat);
/*
 * For the watchdog (called after s->hung has been set): for every connection stuck with a request
 * in hand, start another thread to take it over -- it answers IO to the stuck request and to all
 * later ones, so the client has no requests in flight and a forced unmount is quick. Returns the
 * number of connections taken over.
 */
int  openmacfuse_tp_takeover(struct openmacfuse_tp *tp);
/* Connections currently open */
int  openmacfuse_tp_nconns(struct openmacfuse_tp *tp);
/* Bytes allocated by the buffer pool (for statistics) */
size_t openmacfuse_tp_pool_bytes(struct openmacfuse_tp *tp);

/*
 * Blocking entry point used by the early prototype: starts the threads and returns after stopping
 * them once stop_flag becomes non-zero.
 */
int openmacfuse_serve(struct openmacfuse_server *s, int lfd, volatile int *stop_flag);

#endif /* OPENMACFUSE_SERVER_H */
