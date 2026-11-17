/*
 * report.h - structured telemetry for fuse_main (environment variable NFUSE_REPORT=<jsonl file>)
 *
 * For file systems that only call fuse_main (fuse2fs, exfat-fuse and the like), emit JSON lines
 * in the same format as the telemetry of a host application's own file system engine, so a log
 * collector written for that format can consume them unchanged:
 *   {"ts":<milliseconds>,"pid":<pid>,"role":"nfuse","seq":<n>,"type":"<type>","tool":"<program
 *   name>",...}
 * Types and fields:
 *   start        tool version os arch uid euid foreground watch_dev cmd
 *   nfuse_mount  case case_downgraded case_conflicts case_scan_complete xattr transport mount_s browse ro rwsize
 *                fsync flush_ms watchdog_s watchdog_max_s guard guard_s tcp_guard + watch_dev
 *                iostat
 *   mount_fail   stage (fuse_mount / fuse_new) ret ms
 *   device_lost  err how probe_ms (how = media_gone / node_gone /
 *                node_changed / test_hook)
 *   watchdog     why op secs io_done (why includes mount_stuck / mount_timeout = handshake stuck,
 *                the process then exits with 24)
 *   nfuse_guard  written by the guard process (watchdog.c, role is also nfuse)
 *   exit         code (outcome code: 0 normal, 23 device gone, anything else = fuse_main's return
 *                value) ret reason lasted_s
 * Writing: O_APPEND, one write per line (lines never interleave even with multiple threads and
 * the guard process writing at the same time).
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#ifndef NFUSE_REPORT_H
#define NFUSE_REPORT_H

/*
 * Read NFUSE_REPORT and open the file; tool is the program name written into every line. Returns
 * 0 if enabled, -1 if not.
 */
int nfuse_report_open(const char *tool);
/* File descriptor (for the guard process); -1 if not enabled */
int nfuse_report_fd(void);
/*
 * Write one line; fields is an already-formatted list of JSON fields (without the surrounding
 * commas) and may be NULL.
 */
void nfuse_report(const char *type, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* JSON string escaping (out is at least n bytes) */
char *nfuse_report_esc(const char *s, char *out, size_t n);
/* Close (for tests) */
void nfuse_report_close(void);

#endif /* NFUSE_REPORT_H */
