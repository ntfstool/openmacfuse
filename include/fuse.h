/*
 * fuse.h -- FUSE 2.x compatibility header of nfuse: high-level (path-based) interface
 *
 * Written independently from the public definition of the FUSE 2.x high-level interface: the
 * names, parameters and semantics of the callbacks match the public interface (0 on success,
 * negative errno on failure), plus the getxtimes / setcrtime / setbkuptime / setchgtime
 * extensions common on the Mac and getxattr / setxattr with a position parameter.
 *
 * Differences from traditional FUSE (implemented in libnfuse.a):
 *   - no kernel FUSE module and no separate helper server process: the library runs an NFSv4.0
 *     server inside this process and mounts it with the system's own mount_nfs;
 *   - fuse_loop has the library's server thread handle the requests; callbacks run serially
 *     (like a single-threaded fuse_loop);
 *   - an unmount is recognised by the client closing the connection + the mount no longer being
 *     in the mount table, after which fuse_loop returns.
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#ifndef NFUSE_FUSE_H
#define NFUSE_FUSE_H

#ifndef FUSE_USE_VERSION
#define FUSE_USE_VERSION 26
#endif
/* FUSE_USE_VERSION 26-29 is supported (the 2.6-2.9 high-level interface has a single layout in nfuse); older compatibility interfaces and 3.x are not */
#if FUSE_USE_VERSION < 26
#error "nfuse only supports FUSE_USE_VERSION 26-29"
#endif
#if FUSE_USE_VERSION >= 30
#error "nfuse only supports the FUSE 2.x interface (FUSE_USE_VERSION 26-29), not 3.x"
#endif

#include <fcntl.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <time.h>
#include <utime.h>

#include "fuse_common.h"

#ifdef __cplusplus
extern "C" {
#endif

struct fuse;

/* Filler function of readdir: returning 1 means the buffer is full, stop; off of 0 means everything is delivered in one pass */
typedef int (*fuse_fill_dir_t)(void *buf, const char *name,
			       const struct stat *stbuf, off_t off);

/* The old getdir interface (only for structural completeness, unused) */
typedef struct fuse_dirhandle *fuse_dirh_t;
typedef int (*fuse_dirfil_t)(fuse_dirh_t h, const char *name, int type,
			     ino_t ino);

struct fuse_operations {
	int (*getattr)(const char *, struct stat *);
	int (*readlink)(const char *, char *, size_t);
	int (*getdir)(const char *, fuse_dirh_t, fuse_dirfil_t);
	int (*mknod)(const char *, mode_t, dev_t);
	int (*mkdir)(const char *, mode_t);
	int (*unlink)(const char *);
	int (*rmdir)(const char *);
	int (*symlink)(const char *, const char *);
	int (*rename)(const char *, const char *);
	int (*link)(const char *, const char *);
	int (*chmod)(const char *, mode_t);
	int (*chown)(const char *, uid_t, gid_t);
	int (*truncate)(const char *, off_t);
	int (*utime)(const char *, struct utimbuf *);
	int (*open)(const char *, struct fuse_file_info *);
	int (*read)(const char *, char *, size_t, off_t, struct fuse_file_info *);
	int (*write)(const char *, const char *, size_t, off_t,
		     struct fuse_file_info *);
	int (*statfs)(const char *, struct statvfs *);
	int (*flush)(const char *, struct fuse_file_info *);
	int (*release)(const char *, struct fuse_file_info *);
	int (*fsync)(const char *, int, struct fuse_file_info *);
#if defined(__APPLE__)
	int (*setxattr)(const char *, const char *, const char *, size_t, int,
			uint32_t position);
	int (*getxattr)(const char *, const char *, char *, size_t,
			uint32_t position);
#else
	int (*setxattr)(const char *, const char *, const char *, size_t, int);
	int (*getxattr)(const char *, const char *, char *, size_t);
#endif
	int (*listxattr)(const char *, char *, size_t);
	int (*removexattr)(const char *, const char *);
	int (*opendir)(const char *, struct fuse_file_info *);
	int (*readdir)(const char *, void *, fuse_fill_dir_t, off_t,
		       struct fuse_file_info *);
	int (*releasedir)(const char *, struct fuse_file_info *);
	int (*fsyncdir)(const char *, int, struct fuse_file_info *);
	void *(*init)(struct fuse_conn_info *conn);
	void (*destroy)(void *);
	int (*access)(const char *, int);
	int (*create)(const char *, mode_t, struct fuse_file_info *);
	int (*ftruncate)(const char *, off_t, struct fuse_file_info *);
	int (*fgetattr)(const char *, struct stat *, struct fuse_file_info *);
	int (*lock)(const char *, struct fuse_file_info *, int cmd,
		    struct flock *);
	int (*utimens)(const char *, const struct timespec tv[2]);
	int (*bmap)(const char *, size_t blocksize, uint64_t *idx);

	unsigned int flag_nullpath_ok : 1;
	unsigned int flag_nopath : 1;
	unsigned int flag_utime_omit_ok : 1;
	unsigned int flag_reserved : 29;

	int (*ioctl)(const char *, int cmd, void *arg,
		     struct fuse_file_info *, unsigned int flags, void *data);
	int (*flock)(const char *, struct fuse_file_info *, int op);
	int (*fallocate)(const char *, int, off_t, off_t,
			 struct fuse_file_info *);

	/* Mac extensions */
	int (*setvolname)(const char *);
	int (*exchange)(const char *, const char *, unsigned long);
	int (*getxtimes)(const char *, struct timespec *bkuptime,
			 struct timespec *crtime);
	int (*setbkuptime)(const char *, const struct timespec *tv);
	int (*setchgtime)(const char *, const struct timespec *tv);
	int (*setcrtime)(const char *, const struct timespec *tv);
	int (*chflags)(const char *, uint32_t);

	/*
	 * A few more callbacks from 2.8 / 2.9, present only so that file systems that set them compile
	 * (filled in by name with designated initialisers, so their position does not matter). nfuse
	 * never calls them: poll (NFS has no corresponding operation), read_buf / write_buf (only
	 * read / write are used; a file system that provides write_buf but no write is mounted
	 * read-only). lock / flock / ioctl / bmap / fallocate are not called either.
	 */
	int (*poll)(const char *, struct fuse_file_info *,
		    struct fuse_pollhandle *ph, unsigned *reventsp);
	int (*write_buf)(const char *, struct fuse_bufvec *buf, off_t off,
			 struct fuse_file_info *);
	int (*read_buf)(const char *, struct fuse_bufvec **bufp, size_t size,
			off_t off, struct fuse_file_info *);
};

/* Caller information while a callback runs (from the AUTH_SYS credential of the NFS request; the pid is not available and is always 0) */
struct fuse_context {
	struct fuse	*fuse;
	uid_t		uid;
	gid_t		gid;
	pid_t		pid;
	void		*private_data;	/* return value of init */
	mode_t		umask;
};

/*
 * Create a file system instance: copy the callback table, call init, then start the server
 * thread and run mount_nfs to complete the mount (the mount handshake needs the callbacks,
 * which is why this happens here and not in fuse_mount).
 * Before returning, the server thread is parked at a request boundary: the caller may
 * daemon()/fork next, threads do not survive a fork, and fuse_loop starts it again.
 * Returns NULL on failure.
 */
struct fuse *fuse_new(struct fuse_chan *ch, struct fuse_args *args,
		      const struct fuse_operations *op, size_t op_size,
		      void *user_data);
void fuse_destroy(struct fuse *f);
/* Handle requests until unmounted or an exit signal arrives; returns 0 normally */
int fuse_loop(struct fuse *f);
void fuse_exit(struct fuse *f);
struct fuse_context *fuse_get_context(void);
struct fuse_session *fuse_get_session(struct fuse *f);

/*
 * nfuse extension (not part of FUSE 2.x): called by the file system when it finds that the
 * underlying device is gone (unplugged, power loss); may be called from any thread, and more
 * than once. From then on every NFS request is answered with NFS4ERR_IO at once (the client
 * hands the application EIO and does not retry) and no longer reaches the file system callbacks
 * (except operations such as PUTFH and LOCKU that only touch server state; CLOSE also returns
 * IO so that close() reports the error); fuse_loop returns shortly after and fuse_unmount goes
 * straight to a forced unmount. reason is written to the log.
 * Use #ifdef NFUSE_HAS_DEVICE_LOST to test for availability (other FUSE implementations do not have it).
 */
#define NFUSE_HAS_DEVICE_LOST 1
void nfuse_device_lost(struct fuse *f, const char *reason);

/*
 * ---------- All-in-one fuse_main (src/fuse_main.c) ----------
 *
 * fuse_main(argc, argv, op, user_data): parse the command line (fuse_parse_cmdline) -> mount ->
 * go to the background -> handle requests until unmounted -> unmount -> return. Without -f / -d
 * it goes to the background, with the same semantics as FUSE 2.x: the init callback is called
 * in the background process.
 * nfuse completes the mount inside fuse_new, so the order is: fork first, the child mounts; the
 * parent waits for the child's report -- once mounted the parent calls _exit(0) directly (it
 * does not return to the caller, so the file system's clean-up code does not run a second time
 * in the parent); if the mount failed it waits for the child to exit and exits with the child's
 * exit code (the error is returned by fuse_main in the child for the file system to handle and
 * print itself, exactly once).
 * Only after mounting does the child setsid, chdir("/") and redirect stdio to /dev/null, so a
 * caller reading the output pipe sees end-of-file after the mount is up; a caller that kills
 * the process group during the mount kills the child as well; if the parent is already gone by
 * the time the mount is up (the caller timed out and gave up), the child unmounts and exits.
 *
 * Return value (matches what common FUSE file systems check for): 0 success (including -h / -V);
 * 1 bad arguments; 2 no mount point given; 3 fuse_mount failed (mount point unusable etc.);
 * 4 mount failed (fuse_new: mount_nfs failed, the file system's getattr("/") failed);
 * 5 going to the background failed / the caller gave up; 6 installing the signal handlers
 * failed; 7 error while handling requests.
 *
 * Default options fuse_main adds on its own (each can be overridden by the same option on the
 * command line / in the environment variable; the last one wins):
 *   nfuse_case=sensitive    generic FUSE file systems are exported case-sensitive (a file system
 *                           that calls fuse_new directly still gets the library default, insensitive)
 *   nfuse_xattr=appledouble the client writes `._X` files itself. adfile requires the file
 *                           system's setxattr to accept attribute names without a namespace
 *                           prefix such as com.apple.*; some ext drivers reject those (which
 *                           makes Finder copies fail)
 *   volname=<last component of the mount point> when no volname was given (some drivers swallow
 *                           the caller's -o volname)
 * Environment variables (only honoured by fuse_main):
 *   NFUSE_OPTS=<a,b=c>      appended to the end of the -o options (for programs that swallow -o)
 *   NFUSE_WATCH_DEV=<device>  watch the underlying device: node gone / device replaced ->
 *                           nfuse_device_lost (requests return IO, forced unmount, exit); the
 *                           I/O counts of that device (IOKit statistics) also serve as the
 *                           watchdog's "backend progress"
 *   NFUSE_TEST_DEVLOST_FILE=<file>  test hook: this file appearing = the device is gone
 *   NFUSE_REPORT=<jsonl file> structured telemetry (start / nfuse_mount / mount_fail /
 *                           device_lost / watchdog / nfuse_guard / exit; format in src/report.h)
 * The options nosuid / nodev / noexec are passed on to mount_nfs by the library (fuse_lib.c, so
 * they also apply to file systems that do not use fuse_main).
 * Handshake stuck / mount_nfs timeout: the library takes over the connection and answers IO so
 * that mount_nfs fails on its own (it is not killed and nothing is force-unmounted); the process
 * exits with 24.
 * When the mount fails after init has been called, the library calls destroy to clean up.
 */
int fuse_main_real(int argc, char *argv[], const struct fuse_operations *op,
		   size_t op_size, void *user_data);
#define fuse_main(argc, argv, op, user_data) \
	fuse_main_real(argc, argv, op, sizeof(*(op)), user_data)

/* nfuse callbacks run serially anyway: the multi-threaded variant is the same as fuse_loop */
int fuse_loop_mt(struct fuse *f);

#ifdef __cplusplus
}
#endif

#endif /* NFUSE_FUSE_H */
