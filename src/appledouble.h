/*
 * appledouble.h - reads extended attributes and the resource fork out of AppleDouble (`._name`) files
 *
 * Without a named-attribute channel (nonamedattr) the system NFS client stores extended
 * attributes in `._X` files. Once named attributes are used the client no longer reads
 * them, so the server has to present the `._X` files of existing volumes as named attributes of X.
 *
 * Format (big-endian; per the AppleDouble header in RFC 1740, checked byte by byte against `._` files written by the client):
 *   0  magic 0x00051607 | 4 version 0x00020000 | 8 16 bytes of filler | 24 entry count u16
 *   from 26, 12 bytes per entry: id u32, offset u32, length u32 (id 9 = Finder info, id 2 = resource fork)
 *   Finder info entry: the first 32 bytes are the FinderInfo; after it (skipping 2 more bytes) comes the extended attribute block:
 *     'ATTR' u32 | debug_tag u32 | total_size u32 | data_start u32 | data_length u32 |
 *     3 reserved u32 | flags u16 | attribute count u16 | attribute table: offset u32, length u32, flags u16,
 *     name length u8 (including the trailing 0), name; each entry aligned to 4 bytes; all offsets are from the start of the file.
 *   An all-zero FinderInfo and the client's 286-byte placeholder resource fork ("intentionally left blank") do not count as attributes.
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#ifndef OPENMACFUSE_APPLEDOUBLE_H
#define OPENMACFUSE_APPLEDOUBLE_H

#include <stdint.h>

#define OPENMACFUSE_XA_FINDERINFO	"com.apple.FinderInfo"
#define OPENMACFUSE_XA_RESOURCEFORK	"com.apple.ResourceFork"

/* Read size bytes (at offset off) from `._X`; returns the number of bytes read, or a negative errno */
typedef int (*openmacfuse_ad_read_fn)(void *ctx, void *buf, uint32_t size, uint64_t off);

struct openmacfuse_ad_attr {
	char		*name;
	uint64_t	off;	/* offset of the value within `._X` */
	uint64_t	len;
};

struct openmacfuse_ad {
	uint32_t		n;
	struct openmacfuse_ad_attr	*a;
};

/* Parse; returns -EINVAL if it is not AppleDouble (the caller then treats it as an ordinary file) */
int  openmacfuse_ad_parse(openmacfuse_ad_read_fn rd, void *ctx, uint64_t fsize, struct openmacfuse_ad *out);
void openmacfuse_ad_free(struct openmacfuse_ad *ad);
const struct openmacfuse_ad_attr *openmacfuse_ad_find(const struct openmacfuse_ad *ad, const char *name);

/*
 * Build the front part of an AppleDouble file (header + FinderInfo entry + extended attribute block),
 * laid out the same way as the `._` files the system client writes itself: two entries
 * (9 = FinderInfo, 2 = resource fork), the FinderInfo entry starting at 0x32, the attribute block
 * 2 bytes after the FinderInfo, and the resource fork at 0xEE2 by default (pushed back when there
 * are many attributes).
 * The resource fork itself is not part of the output: *rsrc_off is where it starts and its length is
 * supplied separately by the caller (with no resource fork, use the 286-byte blank placeholder,
 * see openmacfuse_ad_blank_rsrc). Returns 0 / negative errno; the caller frees *out.
 */
struct openmacfuse_ad_val {
	const char	*name;
	const void	*val;
	uint32_t	len;
};
int  openmacfuse_ad_build_prefix(const uint8_t finfo[32], const struct openmacfuse_ad_val *v, uint32_t n,
			   uint64_t rsrc_len, uint8_t **out, uint32_t *outlen, uint32_t *rsrc_off);
#define OPENMACFUSE_AD_BLANK_RSRC_LEN	286u
/* The 286-byte placeholder used when there is no resource fork (same as what the client writes: an empty resource map + "intentionally left blank") */
const uint8_t *openmacfuse_ad_blank_rsrc(void);

#endif /* OPENMACFUSE_APPLEDOUBLE_H */
