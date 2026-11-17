/*
 * nfuse.h -- nfuse-specific extension interface (what the traditional FUSE 2.x high-level interface lacks)
 *
 * A file system may register these callbacks with nfuse_set_ext_ops before fuse_new; all of
 * them are optional, and the library falls back to a generic approach for any that is not
 * registered (see each item). Only available when linking libnfuse; file system code that is
 * also built against traditional FUSE should wrap the calls in #ifdef.
 *
 * Mount options (-o, parsed by the library itself; the file system need not know them):
 *   nfuse_xattr=adfile|appledouble|streams
 *                                    where extended attributes go.
 *                                    adfile (default with fuse_mount / fuse_new): the client keeps
 *                                    writing `._X` AppleDouble files, and the library translates
 *                                    them into the file system's setxattr etc. callbacks, so no
 *                                    `._` files are stored on the volume; `._X` files already on
 *                                    an older volume are hidden and merged into X's attributes.
 *                                    appledouble (default with fuse_main): the client's `._`
 *                                    files are stored as ordinary files.
 *                                    streams (experimental): the client mounts with namedattr.
 *                                    Off by default: the macOS NFS client crashed the kernel on
 *                                    the named-attribute path during development.
 *   nfuse_adclean=1|0                when an extended attribute of a file is written, its `._X`
 *                                    is first moved into the file system; 1 (default) then
 *                                    deletes `._X`, 0 keeps `._X` (and records a hidden
 *                                    "migrated" marker attribute so it is not read again)
 *   nfuse_case=insensitive|sensitive default insensitive: lookups are case-insensitive
 *                                    (case-preserving) and case_insensitive is declared to the
 *                                    client; if a scan before mounting finds names on the volume
 *                                    that differ only in case, the whole volume is automatically
 *                                    mounted sensitive
 *   nfuse_case_scan_ms=<ms>           time limit for that scan (default 300, 0 = no scan)
 *   nfuse_nfc=1|0                    convert names to Unicode NFC on create / rename (default 1);
 *                                    lookups always treat the NFC and NFD spellings as equal
 *
 * Automatic recovery (environment variables; the defaults are the production behaviour):
 *   NFUSE_WATCHDOG_S=<s>       watchdog: when a request takes longer than this with no backend
 *                              progress at all in the meantime (see the progress callback), or a
 *                              server thread has been stalled this long, it is declared hung:
 *                              from then on all requests return IO, the mount is force-unmounted
 *                              and the process exits (exit code NFUSE_EXIT_WATCHDOG). Default 30, 0 = off
 *   NFUSE_WATCHDOG_MAX_S=<s>   absolute limit for a single request (hung even with progress), default 600, 0 = none
 *   NFUSE_GUARD=0              do not start the guard process (started by default: it force-unmounts
 *                              on behalf of a driver that was killed / crashed / froze as a whole)
 *   NFUSE_GUARD_S=<s>          heartbeat silence after which the guard process treats the driver
 *                              as frozen, default 20, 0 = only handle "driver exited"
 *   NFUSE_MOUNT_TIMEOUT_S=<s>  mount_nfs timeout, default 30
 *
 * Mount option nobrowse: passed to mount_nfs only when given (hidden from Finder), otherwise not passed.
 * Mount options nosuid / nodev / noexec: passed to mount_nfs when given (handled centrally in
 * the library; suid / dev / exec cancel them).
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#ifndef NFUSE_EXT_H
#define NFUSE_EXT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NFUSE_EXT_VERSION	2

/* Default process exit code after the watchdog declared a hang and force-unmounted (the file system's watchdog callback can substitute its own) */
#define NFUSE_EXIT_WATCHDOG	24
/* Outcome code after the device was lost and the mount force-unmounted (used by the exit telemetry line of fuse_main) */
#define NFUSE_EXIT_DEVICE_LOST	23

struct nfuse_ext_ops {
	/*
	 * Case folding: fold a UTF-8 name (already converted to NFC by the library) character by
	 * character into a comparable form written to out (NUL-terminated); returns the length, or a
	 * negative number on failure. Two names with the same folded result "differ only in case".
	 * NULL: the library uses the simple per-code-point Unicode uppercase mapping.
	 */
	int (*casefold)(const char *in, char *out, size_t outsz);
	/*
	 * Named attributes (extended attributes) read and written by offset, for large values such
	 * as resource forks of several MB:
	 *   xattr_read     read [off, off+size); returns the number of bytes read (up to the end), -ENOATTR if the attribute does not exist;
	 *   xattr_write    write at off (creates the attribute if missing; with size 0 it is only created), no truncation; returns the number of bytes written;
	 *   xattr_truncate truncate the attribute value to size bytes (-ENOATTR if it does not exist).
	 * NULL: the library reads, modifies and writes the whole value with getxattr / setxattr (slow for large values).
	 */
	int (*xattr_read)(const char *path, const char *name, char *buf, size_t size,
			  uint64_t off);
	int (*xattr_write)(const char *path, const char *name, const char *buf,
			   size_t size, uint64_t off);
	int (*xattr_truncate)(const char *path, const char *name, uint64_t size);

	/* ---- Automatic recovery (since version 2) ---- */
	/*
	 * Backend progress counter: monotonically increasing, incremented for every completed device
	 * I/O (successful or not). The watchdog uses it to tell "slow but moving" (spinning disks,
	 * huge directories, slow flushes) from "hung": a request is only hung when it exceeds the
	 * threshold and the counter has not changed once during that time. Called by the watchdog
	 * thread at any time; must be lock-free and non-blocking. NULL: decide by time alone.
	 */
	uint64_t (*progress)(void);
	/*
	 * Called once when the watchdog declares a hang (on the watchdog thread, while other threads
	 * may be stuck inside the file system: do not take file system locks, do not touch the
	 * device), for recording telemetry. why: stuck (a request is stuck without progress) /
	 * too_long (absolute limit exceeded) / conn_stall / accept_stall / loop_stall (a server
	 * thread stalled); op: name of the stuck NFS operation; secs: how long it has been stuck.
	 * Returns the process exit code (<= 0 means NFUSE_EXIT_WATCHDOG). The library then
	 * force-unmounts and calls _exit.
	 */
	int (*watchdog)(const char *why, const char *op, double secs);
	/*
	 * Descriptor the guard process writes telemetry to (a JSON-lines file opened with O_APPEND);
	 * -1 or NULL = none. After the driver died / froze, the guard process force-unmounts on its
	 * behalf and writes one line {"type":"nfuse_guard","what":"driver_gone|driver_stalled",...}.
	 */
	int (*report_fd)(void);
};

/* Register the extension callbacks (pass sizeof(struct nfuse_ext_ops) as ops_size so fields can be added later). Returns 0 */
int nfuse_set_ext_ops(const struct nfuse_ext_ops *ops, size_t ops_size);

/*
 * Mount result (for the file system's telemetry; available any time after fuse_new succeeded).
 * Pass sizeof(struct nfuse_mount_info) as size.
 * Returns 0; -1 when f is NULL or nothing is mounted yet.
 */
struct fuse;
struct nfuse_mount_info {
	char		case_mode[16];		/* insensitive / sensitive (what is actually in effect) */
	int		case_downgraded;	/* 1 = case-insensitive by default, but automatically downgraded to sensitive because the volume has names differing only in case */
	uint32_t	case_conflicts;		/* number of name pairs differing only in case found by the pre-mount scan */
	int		case_scan_complete;	/* the scan covered the whole volume within the time limit; -1 = no scan */
	char		xattr_mode[16];		/* adfile / appledouble / streams */
	char		transport[8];		/* unix / tcp */
	double		mount_s;		/* duration of the mount_nfs handshake (seconds) */
	int		browse;			/* 0 = mounted with nobrowse (hidden from Finder) */
	int		readonly;
	uint32_t	rwsize;
	double		watchdog_s;		/* 0 = off */
	double		watchdog_max_s;
	int		guard_on;
	double		guard_s;
	int		tcp_guard;		/* local-access protection (only meaningful with tcp) */
};
#define NFUSE_HAS_MOUNT_INFO 1
int nfuse_get_mount_info(struct fuse *f, struct nfuse_mount_info *mi, size_t size);

/*
 * Why fuse_loop returned: unmounted (the client unmounted) / device_lost (nfuse_device_lost) /
 * exit_requested (SIGHUP / SIGINT / SIGTERM or fuse_exit); an empty string while it has not returned.
 */
#define NFUSE_HAS_END_REASON 1
const char *nfuse_end_reason(struct fuse *f);

#ifdef __cplusplus
}
#endif

#endif /* NFUSE_EXT_H */
