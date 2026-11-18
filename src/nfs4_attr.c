/*
 * nfs4_attr.c - fattr4 attribute encoding
 *
 * Attribute values must be encoded in ascending attribute number order: first the result bitmap,
 * then one variable-length opaque (all the attribute values concatenated).
 *
 * Observed on the wire (what the macOS 12 NFS client asks for; see docs/DESIGN.md):
 *   at mount time it requests 0-6,8-10,13,14,16-18,20-23,25-31,33-37,41-45,47,49,50,52,53,55;
 *   an ordinary GETATTR always requests 1,3,4,8,20,33,35,36,37,45,47,50,52,53;
 *   READDIR requests 1,3,4,8,11,19,20,33,35,36,37,45,47,50,52,53 for each entry;
 *   statfs additionally wants 21-23,42-45.
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>

#include "nfs4_attr.h"

#define BIT(n)	(1u << ((n) & 31))

void openmacfuse_attr_supported(uint32_t w[NFS4_BITMAP_WORDS])
{
	w[0] = BIT(FATTR4_SUPPORTED_ATTRS) | BIT(FATTR4_TYPE) |
	       BIT(FATTR4_FH_EXPIRE_TYPE) | BIT(FATTR4_CHANGE) |
	       BIT(FATTR4_SIZE) | BIT(FATTR4_LINK_SUPPORT) |
	       BIT(FATTR4_SYMLINK_SUPPORT) | BIT(FATTR4_NAMED_ATTR) |
	       BIT(FATTR4_FSID) | BIT(FATTR4_UNIQUE_HANDLES) |
	       BIT(FATTR4_LEASE_TIME) | BIT(FATTR4_RDATTR_ERROR) |
	       BIT(FATTR4_ARCHIVE) | BIT(FATTR4_CANSETTIME) |
	       BIT(FATTR4_CASE_INSENSITIVE) | BIT(FATTR4_CASE_PRESERVING) |
	       BIT(FATTR4_CHOWN_RESTRICTED) | BIT(FATTR4_FILEHANDLE) |
	       BIT(FATTR4_FILEID) | BIT(FATTR4_FILES_AVAIL) |
	       BIT(FATTR4_FILES_FREE) | BIT(FATTR4_FILES_TOTAL) |
	       BIT(FATTR4_HIDDEN) | BIT(FATTR4_HOMOGENEOUS) |
	       BIT(FATTR4_MAXFILESIZE) | BIT(FATTR4_MAXLINK) |
	       BIT(FATTR4_MAXNAME) | BIT(FATTR4_MAXREAD) | BIT(FATTR4_MAXWRITE);
	w[1] = BIT(FATTR4_MODE) | BIT(FATTR4_NO_TRUNC) |
	       BIT(FATTR4_NUMLINKS) | BIT(FATTR4_OWNER) |
	       BIT(FATTR4_OWNER_GROUP) | BIT(FATTR4_RAWDEV) |
	       BIT(FATTR4_SPACE_AVAIL) | BIT(FATTR4_SPACE_FREE) |
	       BIT(FATTR4_SPACE_TOTAL) | BIT(FATTR4_SPACE_USED) |
	       BIT(FATTR4_SYSTEM) | BIT(FATTR4_TIME_ACCESS) |
	       BIT(FATTR4_TIME_BACKUP) | BIT(FATTR4_TIME_CREATE) |
	       BIT(FATTR4_TIME_DELTA) | BIT(FATTR4_TIME_METADATA) |
	       BIT(FATTR4_TIME_MODIFY) | BIT(FATTR4_MOUNTED_ON_FILEID) |
	       /*
	        * write-only attributes: unless these are advertised the client never sends
	        * SETATTR to change times (touch / utimes are silently ignored)
	        */
	       BIT(FATTR4_TIME_ACCESS_SET) | BIT(FATTR4_TIME_MODIFY_SET);
	w[2] = 0;
}

static int has(const uint32_t *w, int n)
{
	return (w[n / 32] >> (n % 32)) & 1;
}

static uint32_t ftype(uint32_t mode)
{
	switch (mode & S_IFMT) {
	case S_IFREG:	return NF4REG;
	case S_IFDIR:	return NF4DIR;
	case S_IFLNK:	return NF4LNK;
	case S_IFBLK:	return NF4BLK;
	case S_IFCHR:	return NF4CHR;
	case S_IFSOCK:	return NF4SOCK;
	case S_IFIFO:	return NF4FIFO;
	default:	return NF4REG;
	}
}

static void put_time(xdr_enc_t *e, const struct timespec *t)
{
	xdr_put_u64(e, (uint64_t)(int64_t)t->tv_sec);
	xdr_put_u32(e, (uint32_t)t->tv_nsec);
}

static void put_numeric_id(xdr_enc_t *e, uint32_t id)
{
	char buf[16];

	/*
	 * under AUTH_SYS the macOS client accepts purely numeric owner strings
	 */
	snprintf(buf, sizeof(buf), "%u", id);
	xdr_put_string(e, buf);
}

void openmacfuse_attr_pseudoroot(struct openmacfuse_server *s, struct openmacfuse_attr *st)
{
	memset(st, 0, sizeof(*st));
	st->ino = 0xfffffffffffffffeull;
	st->mode = S_IFDIR | 0555;
	st->nlink = 3;
	st->size = 4096;
	st->change = 1;		/* the pseudo root never changes; all times are 0 */
	(void)s;
}

void openmacfuse_attr_encode(struct openmacfuse_server *s, xdr_enc_t *e,
		      const uint32_t req[NFS4_BITMAP_WORDS],
		      const struct openmacfuse_fh *fh, const struct openmacfuse_attr *st,
		      uint32_t rdattr_err)
{
	uint32_t sup[NFS4_BITMAP_WORDS], res[NFS4_BITMAP_WORDS];
	struct openmacfuse_statfs sf;
	int have_sf = 0, i;
	size_t lenpos, start;

	memset(&sf, 0, sizeof(sf));
	openmacfuse_attr_supported(sup);
	for (i = 0; i < NFS4_BITMAP_WORDS; i++)
		res[i] = req[i] & sup[i];
	/* write-only attributes have no value to return */
	res[1] &= ~(BIT(FATTR4_TIME_ACCESS_SET) | BIT(FATTR4_TIME_MODIFY_SET));

	/*
	 * the pseudo root belongs to no volume: volume attributes are fetched as usual anyway
	 * (the client does issue statfs on the pseudo root)
	 */
	/* attributes that need volume information fetch it only once */
	if (has(res, FATTR4_FILES_AVAIL) || has(res, FATTR4_FILES_FREE) ||
	    has(res, FATTR4_FILES_TOTAL) || has(res, FATTR4_SPACE_AVAIL) ||
	    has(res, FATTR4_SPACE_FREE) || has(res, FATTR4_SPACE_TOTAL)) {
		if (s->be.ops->statfs && s->be.ops->statfs(s->be.ctx, &sf) == 0)
			have_sf = 1;
	}
	(void)have_sf;	/* if it cannot be fetched, answer 0 instead of failing the whole GETATTR */

	xdr_put_bitmap(e, res, NFS4_BITMAP_WORDS);
	lenpos = xdr_reserve_u32(e);
	start = e->len;

	for (i = 0; i <= FATTR4_MAX_ATTR; i++) {
		if (!has(res, i))
			continue;
		switch (i) {
		case FATTR4_SUPPORTED_ATTRS:
			xdr_put_bitmap(e, sup, NFS4_BITMAP_WORDS);
			break;
		case FATTR4_TYPE:
			xdr_put_u32(e, st->nf4type ? st->nf4type : ftype(st->mode));
			break;
		case FATTR4_FH_EXPIRE_TYPE:
			xdr_put_u32(e, FH4_PERSISTENT);
			break;
		case FATTR4_CHANGE:
			xdr_put_u64(e, st->change ? st->change :
				    (uint64_t)st->ctime.tv_sec * 1000000000ull +
				    (uint64_t)st->ctime.tv_nsec);
			break;
		case FATTR4_SIZE:
			xdr_put_u64(e, st->size);
			break;
		case FATTR4_LINK_SUPPORT:
		case FATTR4_SYMLINK_SUPPORT:
		case FATTR4_UNIQUE_HANDLES:
		case FATTR4_CANSETTIME:
		case FATTR4_CASE_PRESERVING:
		case FATTR4_CHOWN_RESTRICTED:
		case FATTR4_HOMOGENEOUS:
		case FATTR4_NO_TRUNC:
			xdr_put_bool(e, 1);
			break;
		case FATTR4_NAMED_ATTR:
			/*
			 * advertised when the named attribute channel is on (openmacfuse_xattr=streams,
			 * the default)
			 */
			xdr_put_bool(e, s->named_attr != 0);
			break;
		case FATTR4_CASE_INSENSITIVE:
			/*
			 * from the openmacfuse_case mount option (switched to case-sensitive
			 * automatically when the pre-mount scan finds conflicts); case_preserving
			 * is always true
			 */
			xdr_put_bool(e, s->case_insensitive != 0);
			break;
		case FATTR4_FSID:
			xdr_put_u64(e, s->fsid_major);
			xdr_put_u64(e, s->fsid_minor);
			break;
		case FATTR4_LEASE_TIME:
			xdr_put_u32(e, s->lease_time);
			break;
		case FATTR4_RDATTR_ERROR:
			xdr_put_u32(e, rdattr_err);
			break;
		case FATTR4_ARCHIVE:
			xdr_put_bool(e, (st->flags & SF_ARCHIVED) != 0);
			break;
		case FATTR4_HIDDEN:
			xdr_put_bool(e, (st->flags & UF_HIDDEN) != 0);
			break;
		case FATTR4_SYSTEM:
			xdr_put_bool(e, 0);
			break;
		case FATTR4_FILEHANDLE: {
			uint8_t raw[OPENMACFUSE_FH_MAXLEN];
			uint32_t fl = openmacfuse_fh_encode(fh, raw);
			xdr_put_opaque(e, raw, fl);
			break;
		}
		case FATTR4_FILEID:
		case FATTR4_MOUNTED_ON_FILEID:
			xdr_put_u64(e, st->ino);
			break;
		case FATTR4_FILES_AVAIL:
			xdr_put_u64(e, sf.ffree);
			break;
		case FATTR4_FILES_FREE:
			xdr_put_u64(e, sf.ffree);
			break;
		case FATTR4_FILES_TOTAL:
			xdr_put_u64(e, sf.files);
			break;
		case FATTR4_MAXFILESIZE:
			xdr_put_u64(e, 0x7fffffffffffffffull);
			break;
		case FATTR4_MAXLINK:
			xdr_put_u32(e, 1024);
			break;
		case FATTR4_MAXNAME:
			xdr_put_u32(e, 255);
			break;
		case FATTR4_MAXREAD:
		case FATTR4_MAXWRITE:
			xdr_put_u64(e, s->rwsize);
			break;
		case FATTR4_MODE:
			xdr_put_u32(e, st->mode & 07777);
			break;
		case FATTR4_NUMLINKS:
			xdr_put_u32(e, st->nlink);
			break;
		case FATTR4_OWNER:
			put_numeric_id(e, st->uid);
			break;
		case FATTR4_OWNER_GROUP:
			put_numeric_id(e, st->gid);
			break;
		case FATTR4_RAWDEV:
			xdr_put_u32(e, (uint32_t)(st->rdev >> 24) & 0xff);
			xdr_put_u32(e, (uint32_t)st->rdev & 0xffffff);
			break;
		case FATTR4_SPACE_AVAIL:
			xdr_put_u64(e, sf.bavail * sf.bsize);
			break;
		case FATTR4_SPACE_FREE:
			xdr_put_u64(e, sf.bfree * sf.bsize);
			break;
		case FATTR4_SPACE_TOTAL:
			xdr_put_u64(e, sf.blocks * sf.bsize);
			break;
		case FATTR4_SPACE_USED:
			xdr_put_u64(e, st->used);
			break;
		case FATTR4_TIME_ACCESS:
			put_time(e, &st->atime);
			break;
		case FATTR4_TIME_BACKUP:
			put_time(e, &st->bkuptime);
			break;
		case FATTR4_TIME_CREATE:
			put_time(e, &st->crtime);
			break;
		case FATTR4_TIME_DELTA: {
			/*
			 * time granularity: reported in nanoseconds (the FUSE interface hands us
			 * a timespec)
			 */
			struct timespec d = { 0, 1 };
			put_time(e, &d);
			break;
		}
		case FATTR4_TIME_METADATA:
			put_time(e, &st->ctime);
			break;
		case FATTR4_TIME_MODIFY:
			put_time(e, &st->mtime);
			break;
		default:
			break;
		}
	}
	xdr_patch_u32(e, lenpos, (uint32_t)(e->len - start));
}

uint32_t openmacfuse_errno_to_nfs4(int neg_errno)
{
	switch (-neg_errno) {
	case 0:			return NFS4_OK;
	case OPENMACFUSE_E_NOT_SAME:	return NFS4ERR_NOT_SAME;
	case OPENMACFUSE_E_BAD_COOKIE:	return NFS4ERR_BAD_COOKIE;
	case ENOATTR:		return NFS4ERR_NOENT;
	case EPERM:		return NFS4ERR_PERM;
	case ENOENT:		return NFS4ERR_NOENT;
	case EIO:		return NFS4ERR_IO;
	case ENXIO:		return NFS4ERR_NXIO;
	case EACCES:		return NFS4ERR_ACCESS;
	case EEXIST:		return NFS4ERR_EXIST;
	case EXDEV:		return NFS4ERR_XDEV;
	case ENOTDIR:		return NFS4ERR_NOTDIR;
	case EISDIR:		return NFS4ERR_ISDIR;
	case EINVAL:		return NFS4ERR_INVAL;
	case EFBIG:		return NFS4ERR_FBIG;
	case ENOSPC:		return NFS4ERR_NOSPC;
	case EROFS:		return NFS4ERR_ROFS;
	case EMLINK:		return NFS4ERR_MLINK;
	case ENAMETOOLONG:	return NFS4ERR_NAMETOOLONG;
	case ENOTEMPTY:		return NFS4ERR_NOTEMPTY;
	case EDQUOT:		return NFS4ERR_DQUOT;
	case ESTALE:		return NFS4ERR_STALE;
	case ENOTSUP:		return NFS4ERR_NOTSUPP;
	case EAGAIN:		return NFS4ERR_DELAY;
	case ENOMEM:		return NFS4ERR_RESOURCE;
	default:		return NFS4ERR_IO;
	}
}
