/*
 * nfs4_attr.h - fattr4 attribute encoding (RFC 7530, section 5)
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#ifndef OPENMACFUSE_NFS4_ATTR_H
#define OPENMACFUSE_NFS4_ATTR_H

#include "nfs4_state.h"
#include "xdr.h"

/* Bitmap of the attributes this server supports */
void openmacfuse_attr_supported(uint32_t w[NFS4_BITMAP_WORDS]);

/*
 * Encode an object's fattr4 (bitmap + attribute values) according to the bitmap the client
 * requested.
 * fh is used for the filehandle attribute; st holds the object's attributes; rdattr_err is only
 * meaningful in READDIR.
 * Unsupported attributes are simply dropped from the result bitmap (which is what GETATTR is
 * required to do).
 */
void openmacfuse_attr_encode(struct openmacfuse_server *s, xdr_enc_t *e,
		      const uint32_t req[NFS4_BITMAP_WORDS],
		      const struct openmacfuse_fh *fh, const struct openmacfuse_attr *st,
		      uint32_t rdattr_err);

/* Attributes of the pseudo root directory (read-only, 0555, owned by root) */
void openmacfuse_attr_pseudoroot(struct openmacfuse_server *s, struct openmacfuse_attr *st);

/* Backend errno -> nfsstat4 */
uint32_t openmacfuse_errno_to_nfs4(int neg_errno);

#endif /* OPENMACFUSE_NFS4_ATTR_H */
