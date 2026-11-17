/*
 * xdr.h - XDR encoding and decoding (RFC 4506)
 *
 * Rules: every value is big-endian and aligned to 4 bytes; variable-length data is preceded by a
 * 32-bit length and zero-padded to alignment.
 * The decoder only reads from a block of memory that has been received in full. On error it sets
 * err to 1 and every later read returns 0, so callers check err once at the key points instead of
 * after every field.
 * The encoder writes into a growable buffer and also sets err when the limit is exceeded.
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#ifndef NFUSE_XDR_H
#define NFUSE_XDR_H

#include <stddef.h>
#include <stdint.h>

/* ---------- Decoding ---------- */
typedef struct xdr_dec {
	const uint8_t	*buf;
	size_t		len;
	size_t		pos;
	int		err;
} xdr_dec_t;

void	 xdr_dec_init(xdr_dec_t *d, const void *buf, size_t len);
uint32_t xdr_get_u32(xdr_dec_t *d);
uint64_t xdr_get_u64(xdr_dec_t *d);
int	 xdr_get_bool(xdr_dec_t *d);
/* Fixed-length opaque data, copied to out */
void	 xdr_get_fixed(xdr_dec_t *d, void *out, size_t n);
/*
 * Variable-length opaque data: returns a pointer into the buffer (no copy); a length above max
 * sets the error.
 */
const uint8_t *xdr_get_opaque_ref(xdr_dec_t *d, uint32_t max, uint32_t *len);
/*
 * Bitmap (uint32<>): keeps at most maxw words, extra words are read and discarded; returns the
 * actual word count.
 */
int	 xdr_get_bitmap(xdr_dec_t *d, uint32_t *w, int maxw);
/* Skip n bytes (including padding) */
void	 xdr_skip_opaque(xdr_dec_t *d, uint32_t n);

/* ---------- Encoding ---------- */
typedef struct xdr_enc {
	uint8_t		*buf;
	size_t		cap;
	size_t		len;
	size_t		limit;	/* maximum allowed length, 0 means unlimited */
	int		err;
} xdr_enc_t;

int	 xdr_enc_init(xdr_enc_t *e, size_t initcap, size_t limit);
void	 xdr_enc_free(xdr_enc_t *e);
void	 xdr_enc_reset(xdr_enc_t *e);
void	 xdr_put_u32(xdr_enc_t *e, uint32_t v);
void	 xdr_put_u64(xdr_enc_t *e, uint64_t v);
void	 xdr_put_bool(xdr_enc_t *e, int v);
void	 xdr_put_fixed(xdr_enc_t *e, const void *p, size_t n);
void	 xdr_put_opaque(xdr_enc_t *e, const void *p, uint32_t n);
void	 xdr_put_string(xdr_enc_t *e, const char *s);
void	 xdr_put_bitmap(xdr_enc_t *e, const uint32_t *w, int nw);
/*
 * Reserve room for one u32, to be filled in later with xdr_patch_u32 (e.g. the total length of
 * the attribute values).
 */
size_t	 xdr_reserve_u32(xdr_enc_t *e);
void	 xdr_patch_u32(xdr_enc_t *e, size_t at, uint32_t v);
/* Reserve space for variable-length data (including zero padding) and
 * return the write position, or NULL on failure.
 * Used by READ: the backend reads straight into the reply buffer, saving a
 * copy. */
uint8_t	*xdr_put_opaque_space(xdr_enc_t *e, uint32_t n);
/* Roll back to a given length (used by READDIR to undo the last entry when it does not fit) */
void	 xdr_truncate(xdr_enc_t *e, size_t len);

#define XDR_PAD(n)	(((n) + 3u) & ~3u)

#endif /* NFUSE_XDR_H */
