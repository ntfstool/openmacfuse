/*
 * rpc.h - ONC RPC v2 (RFC 5531): TCP record marking, call header decoding, reply header encoding
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#ifndef OPENMACFUSE_RPC_H
#define OPENMACFUSE_RPC_H

#include <stddef.h>
#include <stdint.h>

#include "xdr.h"

#define RPC_MAX_GIDS	16

/* Caller identity (decoded from AUTH_SYS; with AUTH_NONE uid/gid are nobody) */
typedef struct rpc_cred {
	uint32_t	flavor;
	uint32_t	uid;
	uint32_t	gid;
	uint32_t	ngids;
	uint32_t	gids[RPC_MAX_GIDS];
} rpc_cred_t;

typedef struct rpc_call {
	uint32_t	xid;
	uint32_t	rpcvers;
	uint32_t	prog;
	uint32_t	vers;
	uint32_t	proc;
	rpc_cred_t	cred;
} rpc_call_t;

/* Return values of rpc_decode_call */
enum rpc_decode_rc {
	RPC_DEC_OK = 0,
	RPC_DEC_DROP,		/* cannot even read the xid, or not a CALL: drop silently */
	RPC_DEC_RPCMISMATCH,	/* rpcvers != 2 */
	RPC_DEC_BADCRED,	/* malformed credential body */
	RPC_DEC_TOOWEAK,	/* unsupported authentication flavor */
};

/* Decode the call header; on success d is positioned at the start of the procedure arguments */
int rpc_decode_call(xdr_dec_t *d, rpc_call_t *c);

/*
 * Reply header encoding. The first 4 bytes of the reply buffer are reserved for the record mark,
 * so call rpc_reply_begin() first and rpc_reply_finish() once the body is written.
 */
void rpc_reply_begin(xdr_enc_t *e);
void rpc_reply_accepted(xdr_enc_t *e, uint32_t xid, uint32_t accept_stat);
void rpc_reply_prog_mismatch(xdr_enc_t *e, uint32_t xid, uint32_t lo, uint32_t hi);
void rpc_reply_rpc_mismatch(xdr_enc_t *e, uint32_t xid);
void rpc_reply_auth_error(xdr_enc_t *e, uint32_t xid, uint32_t auth_stat);
/* Fill in the record mark; returns the total number of bytes to send */
size_t rpc_reply_finish(xdr_enc_t *e);

/*
 * Read one complete RPC record (all fragments concatenated) from the socket into *buf.
 * maxlen is the per-record limit (anything larger is treated as an error and the connection is
 * dropped).
 * Returns 1 if a record was read, 0 if the peer closed cleanly, -1 on error.
 */
int rpc_read_record(int fd, uint8_t **buf, size_t *cap, size_t *len, size_t maxlen);
/* Write the whole buffer (handles short writes and EINTR); returns 0 on success, -1 on failure */
int rpc_write_all(int fd, const uint8_t *buf, size_t len);

#endif /* OPENMACFUSE_RPC_H */
