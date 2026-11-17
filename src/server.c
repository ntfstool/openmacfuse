/*
 * server.c - transport layer implementation
 *
 * One thread per connection: read one complete RPC record -> decode the call header -> execute
 * under the global lock -> write the reply. The system NFS client opens only one connection per
 * mount (confirmed by packet capture) and may pipeline requests; they are processed here one at a
 * time in arrival order.
 *
 * Memory bound: the largest single record = read/write block size + 64KB of header headroom, and
 * anything larger drops the connection; send and receive buffers come from a pool of fixed count
 * and fixed size (see server.h) and do not grow with file size.
 *
 * Every thread waits for data with poll and a 100ms timeout, so that a pause can stop it at a
 * request boundary.
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "nfs4_ops.h"
#include "nfs4_prot.h"
#include "rpc.h"
#include "server.h"
#include "watchdog.h"

#define REC_HEADROOM	(64 * 1024)

int nfuse_listen_tcp(const char *addr, uint16_t port, uint16_t *bound)
{
	struct sockaddr_in sin;
	socklen_t sl = sizeof(sin);
	int fd, one = 1;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	memset(&sin, 0, sizeof(sin));
	sin.sin_len = sizeof(sin);
	sin.sin_family = AF_INET;
	sin.sin_port = htons(port);
	if (inet_pton(AF_INET, addr, &sin.sin_addr) != 1) {
		close(fd);
		errno = EINVAL;
		return -1;
	}
	if (bind(fd, (struct sockaddr *)&sin, sizeof(sin)) < 0 ||
	    listen(fd, 8) < 0) {
		int er = errno;
		close(fd);
		errno = er;
		return -1;
	}
	if (getsockname(fd, (struct sockaddr *)&sin, &sl) == 0 && bound)
		*bound = ntohs(sin.sin_port);
	return fd;
}

int nfuse_listen_unix(const char *path)
{
	struct sockaddr_un sun;
	int fd;

	if (strlen(path) >= sizeof(sun.sun_path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;
	snprintf(sun.sun_path, sizeof(sun.sun_path), "%s", path);
	unlink(path);
	if (bind(fd, (struct sockaddr *)&sun, SUN_LEN(&sun)) < 0 ||
	    listen(fd, 8) < 0) {
		int er = errno;
		close(fd);
		errno = er;
		return -1;
	}
	return fd;
}

struct nfuse_conn {
	struct nfuse_conn	*next;
	struct nfuse_tp		*tp;
	int			fd;
	int			dead;		/* peer disconnected / error, fd already closed */
	int			has_thread;
	pthread_t		th;
	uint8_t			*rbuf;		/* receive buffer (from the pool) */
	uint8_t			*tbuf;		/* send buffer (from the pool) */
	/*
	 * Watchdog: heartbeat (updated on every loop iteration), when the request being processed
	 * started (0 = idle), and whether the thread is blocked reading a request
	 */
	volatile uint64_t	beat;
	volatile uint64_t	req_t0;
	volatile int		in_read;
	/* length of the current request (copied when the watchdog takes over) */
	size_t			rlen;
	/*
	 * taken over by the watchdog: when the original thread comes back it no longer touches
	 * this socket (no late reply is written)
	 */
	volatile int		taken;
	/*
	 * Local-host security: trusted = a connection from the system NFS client (see accept_hook
	 * in server.h); denied = number of rejected requests
	 */
	int			trusted;
	int			denied;
};

#define NFUSE_GATE_MAX_DENIED	8	/* an untrusted connection is dropped after this many rejections */

/*
 * Handle one RPC call and write the reply into out; returns 0 to send the reply, 1 to drop it
 * without replying, 2 to drop it and close the connection (test hook), 3 to close after replying
 */
static int handle_call(struct nfuse_server *s, struct nfuse_conn *cn,
		       const uint8_t *msg, size_t len, xdr_enc_t *out)
{
	xdr_dec_t d;
	rpc_call_t call;
	int rc;

	xdr_dec_init(&d, msg, len);
	rpc_reply_begin(out);
	rc = rpc_decode_call(&d, &call);
	switch (rc) {
	case RPC_DEC_OK:
		break;
	case RPC_DEC_RPCMISMATCH:
		rpc_reply_rpc_mismatch(out, call.xid);
		return 0;
	case RPC_DEC_BADCRED:
		rpc_reply_auth_error(out, call.xid, RPC_AUTH_BADCRED);
		return 0;
	case RPC_DEC_TOOWEAK:
		rpc_reply_auth_error(out, call.xid, RPC_AUTH_TOOWEAK);
		return 0;
	default:
		return 1;
	}
	if (call.prog != NFS4_PROGRAM) {
		rpc_reply_accepted(out, call.xid, RPC_PROG_UNAVAIL);
		return 0;
	}
	if (call.vers != NFS4_VERSION) {
		rpc_reply_prog_mismatch(out, call.xid, NFS4_VERSION, NFS4_VERSION);
		return 0;
	}
	switch (call.proc) {
	case NFSPROC4_NULL:
		rpc_reply_accepted(out, call.xid, RPC_SUCCESS);
		if (s->verbose) {
			if (s->log)
				s->log("xid=%08x NULL (connection fd=%d)", call.xid, cn ? cn->fd : -1);
			else
				fprintf(stderr, "[nfuse] xid=%08x NULL\n", call.xid);
		}
		return 0;
	case NFSPROC4_COMPOUND: {
		size_t hdr;
		unsigned flags = 0;
		uint64_t sum;
		const uint8_t *cached;
		uint32_t clen;

		if (s->hung) {
			/*
			 * The watchdog has declared the server stuck: the request holding the
			 * global lock may never come back, so do not wait for the lock here and
			 * answer NFS4ERR_IO directly (the client hands EIO to the application
			 * without retrying); the mount is force-unmounted shortly afterwards.
			 */
			rpc_reply_accepted(out, call.xid, RPC_SUCCESS);
			if (nfs4_compound_fail(&d, out, NFS4ERR_IO) < 0) {
				xdr_truncate(out, out->len - 4);
				out->err = 0;
				xdr_put_u32(out, RPC_GARBAGE_ARGS);
			}
			return 0;
		}
		if (cn && !cn->trusted) {
			/*
			 * Untrusted connection: it has not yet proved that it is the system NFS
			 * client. Only let through ops that do not touch files, or a request
			 * whose first op is PUTFH with a valid handle (handles carry this run's
			 * random boot id, which only the client on the connection used for the
			 * mount can know) -- that is exactly what a client reconnecting after a
			 * disconnect sends, and once it has presented one the connection becomes
			 * trusted.
			 */
			xdr_dec_t g = d;
			int proved = 0;
			uint32_t fop = 0, st = nfs4_compound_gate(s, &g, &proved, &fop);

			if (proved) {
				cn->trusted = 1;
				s->gate_proved++;
				if (s->log)
					s->log("local security: connection fd=%d presented a valid handle, treating it as the client reconnecting", cn->fd);
			} else if (st != NFS4_OK) {
				cn->denied++;
				s->gate_denied++;
				if (s->log && (cn->denied <= 3 || cn->denied == NFUSE_GATE_MAX_DENIED))
					s->log("local security: rejected a request on untrusted connection fd=%d (xid=%08x, first op %s, status %u, rejection %d%s)",
					       cn->fd, call.xid, fop ? nfs4_op_name(fop) : "-", st, cn->denied,
					       cn->denied >= NFUSE_GATE_MAX_DENIED ? ", disconnecting" : "");
				rpc_reply_accepted(out, call.xid, RPC_SUCCESS);
				if (nfs4_compound_fail(&d, out, st) < 0) {
					xdr_truncate(out, out->len - 4);
					out->err = 0;
					xdr_put_u32(out, RPC_GARBAGE_ARGS);
				}
				return cn->denied >= NFUSE_GATE_MAX_DENIED ? 3 : 0;
			}
		}
		sum = nfuse_drc_sum(msg, len);
		pthread_mutex_lock(&s->lock);
		/*
		 * Duplicate request cache: a non-idempotent request with the same xid and the
		 * same contents has already been executed, so return the previous reply verbatim
		 */
		cached = s->no_drc ? NULL : nfuse_drc_lookup(&s->drc, call.xid, sum, &clen);
		if (cached) {
			xdr_put_fixed(out, cached, clen);
			pthread_mutex_unlock(&s->lock);
			if (s->log)
				s->log("DRC hit: xid=%08x, replaying the previous reply (%u bytes)",
				       call.xid, clen);
			return 0;
		}
		rpc_reply_accepted(out, call.xid, RPC_SUCCESS);
		hdr = out->len;
		{
			uint64_t t0 = nfuse_uptime_ns(), dt;

			rc = nfs4_compound(s, &call, &d, out, &flags);
			dt = nfuse_uptime_ns() - t0;
			/*
			 * total time the server spends processing COMPOUNDs (lock held), to see
			 * the server's share with large directories / concurrency
			 */
			s->busy_ns += dt;
			if (dt > s->busy_max_ns)
				s->busy_max_ns = dt;
			if (dt >= 1000000000ULL)
				s->busy_slow++;
		}
		if (rc < 0) {
			/*
			 * not even the COMPOUND header can be parsed: answer GARBAGE_ARGS instead
			 */
			xdr_truncate(out, hdr - 4);
			out->err = 0;
			xdr_put_u32(out, RPC_GARBAGE_ARGS);
			flags = 0;
		}
		if ((flags & NFUSE_CF_CACHE) && !s->no_drc && !out->err && out->len > 4)
			nfuse_drc_insert(&s->drc, call.xid, sum, out->buf + 4,
					 (uint32_t)(out->len - 4));
		pthread_mutex_unlock(&s->lock);
		if (flags & NFUSE_CF_DROP)
			return s->test_drop_close ? 2 : 1;
		return 0;
	}
	default:
		rpc_reply_accepted(out, call.xid, RPC_PROC_UNAVAIL);
		return 0;
	}
}

/* ---------- Buffer pool (the caller holds tp->mu) ---------- */

static void *pool_get(struct nfuse_tp *tp)
{
	void *b;

	if (tp->nfree > 0)
		return tp->freebufs[--tp->nfree];
	if (tp->nalloc >= 2 * NFUSE_TP_MAX_CONNS)
		return NULL;
	b = malloc(tp->bufsize);
	if (b)
		tp->nalloc++;
	return b;
}

static void pool_put(struct nfuse_tp *tp, void *b)
{
	if (b)
		tp->freebufs[tp->nfree++] = b;
}

/* Wait for fd to become readable: 1 readable, 0 timeout, -1 error */
static int wait_readable(int fd, int ms)
{
	struct pollfd p = { .fd = fd, .events = POLLIN };
	int r = poll(&p, 1, ms);

	if (r < 0)
		return errno == EINTR ? 0 : -1;
	return r > 0 ? 1 : 0;
}

static void conn_mark_dead(struct nfuse_conn *cn)
{
	struct nfuse_tp *tp = cn->tp;

	pthread_mutex_lock(&tp->mu);
	if (!cn->dead) {
		cn->dead = 1;
		close(cn->fd);
		cn->fd = -1;
		pool_put(tp, cn->rbuf);
		pool_put(tp, cn->tbuf);
		cn->rbuf = cn->tbuf = NULL;
		tp->nconns--;
	}
	pthread_mutex_unlock(&tp->mu);
	if (tp->wake_w >= 0) {
		char ch = 'c';
		(void)!write(tp->wake_w, &ch, 1);
	}
}

static void *conn_thread(void *arg)
{
	struct nfuse_conn *cn = arg;
	struct nfuse_tp *tp = cn->tp;
	struct nfuse_server *s = tp->s;
	size_t rcap = tp->bufsize, rlen = 0;
	xdr_enc_t out;

	/*
	 * Both directions use fixed-size buffers from the pool: the limit given to
	 * rpc_read_record equals the capacity, so it never grows
	 */
	out.buf = cn->tbuf;
	out.cap = tp->bufsize;
	out.limit = tp->bufsize;
	out.len = 0;
	out.err = 0;
	while (!tp->pausing) {
		int rc;
		size_t n;

		cn->beat = nfuse_uptime_ns();
		if (s->test_stall_path && access(s->test_stall_path, F_OK) == 0) {
			/*
			 * Test hook: simulate stalled service threads -- no requests are read and
			 * no heartbeats updated until the file is removed
			 */
			if (s->log)
				s->log("test hook: service threads stalled (while %s exists)", s->test_stall_path);
			while (access(s->test_stall_path, F_OK) == 0)
				usleep(100 * 1000);
			continue;
		}
		rc = wait_readable(cn->fd, 100);
		if (rc == 0)
			continue;
		if (rc < 0)
			goto dead;
		cn->in_read = 1;
		rc = rpc_read_record(cn->fd, &cn->rbuf, &rcap, &rlen, tp->bufsize);
		cn->in_read = 0;
		if (rc <= 0)
			goto dead;
		cn->rlen = rlen;
		cn->req_t0 = nfuse_uptime_ns();
		rc = handle_call(s, cn, cn->rbuf, rlen, &out);
		if (cn->taken) {
			/*
			 * While this connection was stuck it was taken over (the takeover thread
			 * has already answered this request with an error). The late reply must
			 * not be sent any more: the client gave up on this request long ago, and
			 * "a late reply + a client reconnect" is suspected of being involved in a
			 * kernel panic seen during testing, so it is always discarded and the
			 * thread simply exits (the socket now belongs to the takeover thread).
			 */
			cn->req_t0 = 0;
			if (s->log)
				s->log("connection fd=%d has been taken over, discarding the late reply of the stuck request", cn->fd);
			return NULL;
		}
		if (rc == 2) {
			if (s->log)
				s->log("test hook: closing connection fd=%d after dropping the reply", cn->fd);
			goto dead;
		}
		if (rc == 1) {
			cn->req_t0 = 0;
			continue;
		}
		n = rpc_reply_finish(&out);
		if (n == 0) {
			if (s->log)
				s->log("failed to encode the reply (buffer limit exceeded?), closing the connection");
			goto dead;
		}
		if (rpc_write_all(cn->fd, out.buf, n) < 0)
			goto dead;
		cn->req_t0 = 0;
		if (rc == 3)
			goto dead;
	}
	return NULL;
dead:
	cn->req_t0 = 0;
	if (s->log)
		s->log("connection closed fd=%d", cn->fd);
	conn_mark_dead(cn);
	return NULL;
}

/*
 * Reap the records of closed connections (join the thread, unlink from the list); the caller does
 * not hold tp->mu
 */
static void reap_dead(struct nfuse_tp *tp, int join_live)
{
	struct nfuse_conn **pp, *cn;

	pthread_mutex_lock(&tp->mu);
	pp = &tp->conns;
	while ((cn = *pp)) {
		if (cn->dead || join_live) {
			int dead = cn->dead;

			if (cn->has_thread) {
				pthread_t th = cn->th;
				cn->has_thread = 0;
				pthread_mutex_unlock(&tp->mu);
				pthread_join(th, NULL);
				pthread_mutex_lock(&tp->mu);
				/*
				 * the list may have changed during the join: start over from the
				 * head
				 */
				pp = &tp->conns;
				continue;
			}
			if (dead) {
				*pp = cn->next;
				free(cn);
				continue;
			}
		}
		pp = &cn->next;
	}
	pthread_mutex_unlock(&tp->mu);
}

static void *accept_thread(void *arg)
{
	struct nfuse_tp *tp = arg;
	struct nfuse_server *s = tp->s;

	while (!tp->pausing) {
		struct nfuse_conn *cn;
		int one = 1, fd, rc;

		tp->acc_beat = nfuse_uptime_ns();
		rc = wait_readable(tp->lfd, 100);

		if (rc <= 0) {
			reap_dead(tp, 0);
			continue;
		}
		fd = accept(tp->lfd, NULL, NULL);
		if (fd < 0)
			continue;
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
		{
			/*
			 * Local unix sockets default to send and receive buffers of only 8KB
			 * (net.local.stream.*space). The macOS 12 client has been observed, when
			 * a single write over a unix socket exceeds 512KB, not to send the second
			 * half and to return EIO to the application directly (TCP does not have
			 * this problem, nor does macOS 27); with the buffers enlarged to hold
			 * several complete read/write requests it works normally.
			 */
			int sz = (int)(4 * tp->bufsize);
			setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));
			setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
		}
		{
			int trusted = 1;

			if (tp->accept_hook && tp->accept_hook(tp->accept_arg, fd, &trusted) < 0) {
				close(fd);
				tp->rejected++;
				continue;
			}
			cn = calloc(1, sizeof(*cn));
			if (cn)
				cn->trusted = trusted;
		}
		pthread_mutex_lock(&tp->mu);
		if (cn && tp->nconns < NFUSE_TP_MAX_CONNS) {
			cn->rbuf = pool_get(tp);
			cn->tbuf = pool_get(tp);
		}
		if (!cn || !cn->rbuf || !cn->tbuf) {
			if (cn) {
				pool_put(tp, cn->rbuf);
				pool_put(tp, cn->tbuf);
			}
			tp->rejected++;
			pthread_mutex_unlock(&tp->mu);
			free(cn);
			close(fd);
			if (s->log)
				s->log("connection count or buffer pool exhausted, refusing new connection");
			continue;
		}
		cn->tp = tp;
		cn->fd = fd;
		cn->next = tp->conns;
		tp->conns = cn;
		tp->nconns++;
		tp->total_conns++;
		if (pthread_create(&cn->th, NULL, conn_thread, cn) == 0)
			cn->has_thread = 1;
		pthread_mutex_unlock(&tp->mu);
		if (!cn->has_thread)
			conn_mark_dead(cn);
		if (s->log)
			s->log("accepted connection fd=%d (total %llu)", fd,
			       (unsigned long long)tp->total_conns);
	}
	return NULL;
}

int nfuse_tp_init(struct nfuse_tp *tp, struct nfuse_server *s, int lfd, int wake_w)
{
	memset(tp, 0, sizeof(*tp));
	tp->s = s;
	tp->lfd = lfd;
	tp->wake_w = wake_w;
	tp->bufsize = s->rwsize + REC_HEADROOM;
	pthread_mutex_init(&tp->mu, NULL);
	signal(SIGPIPE, SIG_IGN);
	return 0;
}

int nfuse_tp_resume(struct nfuse_tp *tp)
{
	struct nfuse_conn *cn;

	if (tp->running)
		return 0;
	tp->pausing = 0;
	if (pthread_create(&tp->acc_th, NULL, accept_thread, tp) != 0)
		return -1;
	pthread_mutex_lock(&tp->mu);
	for (cn = tp->conns; cn; cn = cn->next) {
		if (cn->dead || cn->has_thread)
			continue;
		if (pthread_create(&cn->th, NULL, conn_thread, cn) == 0)
			cn->has_thread = 1;
	}
	pthread_mutex_unlock(&tp->mu);
	tp->running = 1;
	return 0;
}

void nfuse_tp_pause(struct nfuse_tp *tp)
{
	if (!tp->running)
		return;
	tp->pausing = 1;
	pthread_join(tp->acc_th, NULL);
	reap_dead(tp, 1);	/* join all connection threads (live ones stay on the list) */
	tp->running = 0;
	tp->pausing = 0;
}

void nfuse_tp_shutdown(struct nfuse_tp *tp)
{
	struct nfuse_conn *cn;
	int i;

	nfuse_tp_pause(tp);
	pthread_mutex_lock(&tp->mu);
	while ((cn = tp->conns)) {
		tp->conns = cn->next;
		if (!cn->dead) {
			close(cn->fd);
			pool_put(tp, cn->rbuf);
			pool_put(tp, cn->tbuf);
			tp->nconns--;
		}
		free(cn);
	}
	for (i = 0; i < tp->nfree; i++)
		free(tp->freebufs[i]);
	tp->nfree = 0;
	tp->nalloc = 0;
	pthread_mutex_unlock(&tp->mu);
	if (tp->lfd >= 0)
		close(tp->lfd);
	tp->lfd = -1;
}

void nfuse_tp_health(struct nfuse_tp *tp, uint64_t *oldest_req, uint64_t *oldest_idle_beat,
		     uint64_t *acc_beat)
{
	struct nfuse_conn *cn;

	*oldest_req = *oldest_idle_beat = 0;
	*acc_beat = tp->running && !tp->pausing ? tp->acc_beat : 0;
	if (!tp->running)
		return;
	/*
	 * While stopping (pause / shutdown waiting to join the threads) only in-flight requests
	 * are considered: a thread stuck in the backend can never be joined (observed in testing:
	 * after the mount handshake hung and mount_nfs timed out, the failure path stopped here;
	 * previously nothing was reported at this point and the watchdog did not fire).
	 */
	pthread_mutex_lock(&tp->mu);
	for (cn = tp->conns; cn; cn = cn->next) {
		uint64_t r = cn->req_t0, b = cn->beat;

		if (cn->dead)
			/*
			 * has_thread is not checked: reap_dead clears it before the join, and the
			 * stuck thread is precisely the one that never joins
			 */
			continue;
		if (r) {
			if (!*oldest_req || r < *oldest_req)
				*oldest_req = r;
		} else if (!tp->pausing && !cn->in_read && b && (!*oldest_idle_beat || b < *oldest_idle_beat)) {
			*oldest_idle_beat = b;
		}
	}
	pthread_mutex_unlock(&tp->mu);
}

/*
 * Watchdog takeover: a stuck connection thread will no longer read or write its socket. Another
 * thread is started in its place: it first answers IO to the request that thread has in hand,
 * then reads the later requests on this connection one by one and answers IO to each (s->hung is
 * already set, so handle_call takes no lock and does not enter the file system). Every request
 * the client has in flight now has a reply, so the forced unmount does not have to wait (without
 * this, unmount(MNT_FORCE) was measured to wait about 2 seconds).
 */
struct nfuse_takeover {
	struct nfuse_tp	*tp;
	int		fd;
	uint8_t		*req;
	size_t		reqlen;
};

static void *takeover_thread(void *arg)
{
	struct nfuse_takeover *t = arg;
	struct nfuse_server *s = t->tp->s;
	size_t cap = t->tp->bufsize, rcap = 0, rlen = 0, n;
	uint8_t *rb = NULL;
	xdr_enc_t out;
	int rc;

	memset(&out, 0, sizeof(out));
	out.buf = malloc(cap);
	out.cap = out.limit = cap;
	if (!out.buf)
		return NULL;
	if (t->req) {
		rc = handle_call(s, NULL, t->req, t->reqlen, &out);
		if ((rc == 0 || rc == 3) && (n = rpc_reply_finish(&out)))
			(void)rpc_write_all(t->fd, out.buf, n);
	}
	for (;;) {
		rc = rpc_read_record(t->fd, &rb, &rcap, &rlen, t->tp->bufsize);
		if (rc <= 0)
			break;
		rc = handle_call(s, NULL, rb, rlen, &out);
		if ((rc == 0 || rc == 3) && (n = rpc_reply_finish(&out)) &&
		    rpc_write_all(t->fd, out.buf, n) < 0)
			break;
	}
	return NULL;
}

int nfuse_tp_takeover(struct nfuse_tp *tp)
{
	struct nfuse_conn *cn;
	int k = 0;

	pthread_mutex_lock(&tp->mu);
	for (cn = tp->conns; cn; cn = cn->next) {
		struct nfuse_takeover *t;
		pthread_t th;

		if (cn->dead || cn->fd < 0 || !cn->req_t0 || cn->taken)
			continue;	/* an idle connection thread takes the hung branch by itself */
		t = calloc(1, sizeof(*t));
		if (!t)
			continue;
		t->tp = tp;
		t->fd = cn->fd;
		if (cn->rbuf && cn->rlen && (t->req = malloc(cn->rlen))) {
			memcpy(t->req, cn->rbuf, cn->rlen);
			t->reqlen = cn->rlen;
		}
		cn->taken = 1;
		if (pthread_create(&th, NULL, takeover_thread, t) == 0) {
			pthread_detach(th);
			k++;
		} else
			cn->taken = 0;
	}
	pthread_mutex_unlock(&tp->mu);
	return k;
}

int nfuse_tp_nconns(struct nfuse_tp *tp)
{
	int n;

	pthread_mutex_lock(&tp->mu);
	n = tp->nconns;
	pthread_mutex_unlock(&tp->mu);
	return n;
}

size_t nfuse_tp_pool_bytes(struct nfuse_tp *tp)
{
	return (size_t)tp->nalloc * tp->bufsize;
}

int nfuse_serve(struct nfuse_server *s, int lfd, volatile int *stop_flag)
{
	struct nfuse_tp tp;

	nfuse_tp_init(&tp, s, lfd, -1);
	if (nfuse_tp_resume(&tp))
		return -1;
	while (!*stop_flag)
		usleep(100 * 1000);
	nfuse_tp_shutdown(&tp);
	return 0;
}
