/*
 * fuse_common.h -- FUSE 2.x compatibility header of openmacfuse: common types
 *
 * Written independently from the public FUSE 2.x high-level interface (the fuse_operations /
 * fuse_file_info / fuse_conn_info family) so that existing FUSE 2.x file systems can link
 * against libopenmacfuse.a without changing their callback code. The struct layouts only have to be
 * consistent within this library (the file system and the library are both compiled against
 * these headers); binary compatibility with other FUSE implementations is not a goal.
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#ifndef OPENMACFUSE_FUSE_COMMON_H
#define OPENMACFUSE_FUSE_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "fuse_opt.h"

/* Interface version: 2.9 is advertised (callbacks from 2.8+ such as ioctl are available) */
#define FUSE_MAJOR_VERSION	2
#define FUSE_MINOR_VERSION	9
#define FUSE_MAKE_VERSION(maj, min)	((maj) * 10 + (min))
#define FUSE_VERSION	FUSE_MAKE_VERSION(FUSE_MAJOR_VERSION, FUSE_MINOR_VERSION)

/* Identifies this library as the in-process NFS backend (file systems and tests can test for it) */
#define OPENMACFUSE_FUSE	1

#ifdef __cplusplus
extern "C" {
#endif

/* Filled in by the file system at open time and passed back unchanged on every later call */
struct fuse_file_info {
	int		flags;		/* open(2) flags */
	unsigned long	fh_old;		/* legacy field, unused */
	int		writepage;
	unsigned int	direct_io : 1;
	unsigned int	keep_cache : 1;
	unsigned int	flush : 1;
	unsigned int	nonseekable : 1;
	unsigned int	flock_release : 1;
	unsigned int	padding : 27;
	uint64_t	fh;		/* the file system's own handle */
	uint64_t	lock_owner;
};

/* Capability bits (conn->capable / conn->want) */
#define FUSE_CAP_ASYNC_READ	(1 << 0)
#define FUSE_CAP_POSIX_LOCKS	(1 << 1)
#define FUSE_CAP_ATOMIC_O_TRUNC	(1 << 3)
#define FUSE_CAP_EXPORT_SUPPORT	(1 << 4)
#define FUSE_CAP_BIG_WRITES	(1 << 5)
#define FUSE_CAP_DONT_MASK	(1 << 6)
#define FUSE_CAP_IOCTL_DIR	(1 << 11)

/* ioctl flags */
#define FUSE_IOCTL_COMPAT	(1 << 0)
#define FUSE_IOCTL_UNRESTRICTED	(1 << 1)
#define FUSE_IOCTL_RETRY	(1 << 2)
#define FUSE_IOCTL_DIR		(1 << 4)
#define FUSE_IOCTL_MAX_IOV	256

/* Connection parameters negotiated in the init callback */
struct fuse_conn_info {
	unsigned	proto_major;
	unsigned	proto_minor;
	unsigned	async_read;
	unsigned	max_write;
	unsigned	max_readahead;
	unsigned	capable;
	unsigned	want;
	unsigned	max_background;
	unsigned	congestion_threshold;
	/* Mac extension: optional features the file system declares it will use */
	struct {
		unsigned	case_insensitive : 1;
		unsigned	setvolname : 1;
		unsigned	xtimes : 1;
	} enable;
	unsigned	reserved[22];
};

/* Mac extension: declare support for creation / backup times in init */
#define FUSE_ENABLE_XTIMES(conn)		((conn)->enable.xtimes = 1)
#define FUSE_ENABLE_SETVOLNAME(conn)		((conn)->enable.setvolname = 1)
#define FUSE_ENABLE_CASE_INSENSITIVE(conn)	((conn)->enable.case_insensitive = 1)

struct fuse_session;
struct fuse_chan;

/* Mount: parse the -o options and prepare the local socket; returns a channel (the actual mount_nfs happens in fuse_new) */
struct fuse_chan *fuse_mount(const char *mountpoint, struct fuse_args *args);
/* Unmount: unmount if still mounted, then stop the server threads and clean up the socket */
void fuse_unmount(const char *mountpoint, struct fuse_chan *ch);

int fuse_version(void);

/* ---------- For FUSE 2.x file systems that use fuse_main ---------- */

/* Only here so that sources using these callbacks / capability bits compile: OpenMacFUSE never calls poll / read_buf / write_buf, the capability bits are merely recorded */
#define FUSE_CAP_SPLICE_WRITE	(1 << 7)
#define FUSE_CAP_SPLICE_MOVE	(1 << 8)
#define FUSE_CAP_SPLICE_READ	(1 << 9)
#define FUSE_CAP_FLOCK_LOCKS	(1 << 10)

struct fuse_pollhandle;

enum fuse_buf_flags {
	FUSE_BUF_IS_FD		= (1 << 1),
	FUSE_BUF_FD_SEEK	= (1 << 2),
	FUSE_BUF_FD_RETRY	= (1 << 3),
};

struct fuse_buf {
	size_t			size;
	enum fuse_buf_flags	flags;
	void			*mem;
	int			fd;
	off_t			pos;
};

struct fuse_bufvec {
	size_t		count;
	size_t		idx;
	size_t		off;
	struct fuse_buf	buf[1];
};

/*
 * Parse the generic FUSE command line: the first non-option argument is the mount point (a
 * second one is an error), -f foreground, -s single-threaded, -d / -o debug debugging (implies
 * foreground); after -h / --help or -V / --version has printed, *mountpoint is NULL.
 * Recognised -f / -s / -h / -V are removed from args; the rest (-o ..., -d) is left for
 * fuse_mount / fuse_new. mountpoint is a newly allocated string (the caller frees it);
 * multithreaded / foreground may be NULL. 0 on success, -1 on failure.
 */
int fuse_parse_cmdline(struct fuse_args *args, char **mountpoint,
		       int *multithreaded, int *foreground);

/* Make fuse_loop return on SIGHUP / SIGINT / SIGTERM; SIGPIPE is ignored */
int fuse_set_signal_handlers(struct fuse_session *se);
void fuse_remove_signal_handlers(struct fuse_session *se);

#ifdef __cplusplus
}
#endif

#endif /* OPENMACFUSE_FUSE_COMMON_H */
