/*
 * nfs4_ops.h - NFSv4.0 COMPOUND dispatch
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#ifndef OPENMACFUSE_NFS4_OPS_H
#define OPENMACFUSE_NFS4_OPS_H

#include "nfs4_state.h"
#include "rpc.h"
#include "xdr.h"

/* Output bits for *flags of nfs4_compound */
#define OPENMACFUSE_CF_CACHE	0x1	/* contains a non-idempotent op: reply goes into the DRC */
#define OPENMACFUSE_CF_DROP	0x2	/* test hook: do not send this reply */

/*
 * Process one COMPOUND call: d is positioned at the start of COMPOUND4args, the result is written
 * to e (which already holds the RPC reply header).
 * Returns 0 on success; -1 if the arguments cannot be parsed (the caller replies GARBAGE_ARGS).
 * The caller holds s->lock.
 */
int nfs4_compound(struct openmacfuse_server *s, const rpc_call_t *call,
		  xdr_dec_t *d, xdr_enc_t *e, unsigned *flags);

/*
 * Fail the whole COMPOUND with status without executing it (used once the watchdog has declared
 * the backend stuck: reply IO without waiting for the global lock).
 * The reply carries only the result of the first op. Returns 0; -1 if even the argument header
 * cannot be parsed. No lock required.
 */
int nfs4_compound_fail(xdr_dec_t *d, xdr_enc_t *e, uint32_t status);

/*
 * Decide whether a request on an untrusted connection (local-host security) may pass; only the
 * first op is examined. Allowed: RENEW / RELEASE_LOCKOWNER / SETCLIENTID_CONFIRM (when it is the
 * only op), a SETCLIENTID whose client id string matches the one seen at mount time (the first
 * thing a reconnecting client sends), and an empty COMPOUND; if the first op is PUTFH with a
 * valid handle (boot id matches) it is allowed and *proved is set; everything else (PUTROOTFH /
 * PUTPUBFH / SETCLIENTID from an unknown client / invalid handle ...) gets a rejecting status
 * code. NFS4_OK means allowed.
 * d is positioned at the start of COMPOUND4args (the caller passes a copy).
 * No lock required (only the boot id is read).
 */
uint32_t nfs4_compound_gate(struct openmacfuse_server *s, xdr_dec_t *d, int *proved, uint32_t *first_op);

const char *nfs4_op_name(uint32_t op);
/*
 * Op name -> opcode (used by the test hooks to parse environment variables); -1 if unknown.
 */
int nfs4_op_code(const char *name);

#endif /* OPENMACFUSE_NFS4_OPS_H */
