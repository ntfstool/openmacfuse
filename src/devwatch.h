/*
 * devwatch.h -- device watcher used by fuse_main
 *
 * Generic FUSE file systems (ext, exFAT, BitLocker and similar third-party drivers) do not call
 * openmacfuse_device_lost themselves and do not report progress to the watchdog. The caller names the
 * underlying device to fuse_main with the environment variable OPENMACFUSE_WATCH_DEV=<device>; every
 * 0.5 seconds this module then:
 *   - stats the device node: gone / replaced by another device (st_rdev changed) -> openmacfuse_device_lost;
 *   - reads the I/O statistics of the device from IOKit (read/write counts + error counts) and
 *     records one "backend progress" tick whenever they change, which lets the watchdog tell a
 *     slow disk from a hang (the progress callback in openmacfuse.h). When no statistics are available
 *     (image file, permissions) no progress is reported and the watchdog decides by time alone.
 * Test hook OPENMACFUSE_TEST_DEVLOST_FILE=<file>: the file appearing = the device is gone.
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#ifndef OPENMACFUSE_DEVWATCH_H
#define OPENMACFUSE_DEVWATCH_H

#include <stdint.h>

struct fuse;

/* Read the environment variable; returns 1 when watching was requested */
int openmacfuse_devwatch_wanted(void);
/* Record the initial state of the device (call before fuse_new, in this process); -1 if watching was not requested or the device does not exist */
int openmacfuse_devwatch_prepare(void);
/*
 * Start the watcher thread: fuse_main starts it in the final background process, before fuse_new
 * (so progress is reported and unplugging is detected during the mount handshake too);
 * fuse_main passes NULL: openmacfuse_device_lost then uses the library's current instance (registered
 * at the very start of fuse_new; a call made before the instance exists has no effect and is
 * repeated on the next round).
 * When the device is gone one device_lost telemetry line is written (OPENMACFUSE_REPORT).
 */
int openmacfuse_devwatch_start(struct fuse *f);
/* Stop the thread and wait for it to exit (after fuse_loop returns / after fuse_new fails, before unmounting) */
void openmacfuse_devwatch_stop(void);
/* Progress counter for the watchdog (lock-free) */
uint64_t openmacfuse_devwatch_progress(void);
/* For unit tests: sum of the four counters taken from the IOKit statistics dictionary; bsd is a name like "disk4s1"; returns 0 and sets *ok = 0 when unavailable */
uint64_t openmacfuse_devwatch_iostat(const char *bsd, int *ok);

#endif /* OPENMACFUSE_DEVWATCH_H */
