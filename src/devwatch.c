/*
 * devwatch.c -- device watcher used by fuse_main (described in devwatch.h)
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

#include "fuse.h"
#include "devwatch.h"
#include "report.h"

#define DW_PERIOD_MS	500

static char g_dev[1024];
static char g_bsd[128];
static char g_lostfile[1024];
static uint64_t g_rdev;	/* device number (character / block device) or inode (image file) */
static int g_have_dev;
static int g_iostat_ok;
static int g_media_ok;		/* an IOMedia for this disk existed in IOKit at prepare time: from then on its presence decides */
static uint64_t g_io_last;
static _Atomic uint64_t g_progress;

static pthread_t g_th;
static int g_running;
static int g_stop;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static struct fuse *g_f;

int openmacfuse_devwatch_wanted(void)
{
	const char *d = getenv("OPENMACFUSE_WATCH_DEV"), *t = getenv("OPENMACFUSE_TEST_DEVLOST_FILE");

	return (d && *d) || (t && *t);
}

/* /dev/rdisk4s1 -> disk4s1 */
static void bsd_name(const char *dev, char *out, size_t n)
{
	const char *b = strrchr(dev, '/');

	b = b ? b + 1 : dev;
	if (b[0] == 'r' && strncmp(b + 1, "disk", 4) == 0)
		b++;
	snprintf(out, n, "%s", strncmp(b, "disk", 4) == 0 ? b : "");
}

static uint64_t dict_num(CFDictionaryRef d, const char *key)
{
	CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
	CFNumberRef v;
	int64_t x = 0;

	if (!k)
		return 0;
	v = CFDictionaryGetValue(d, k);
	CFRelease(k);
	if (v && CFGetTypeID(v) == CFNumberGetTypeID())
		CFNumberGetValue(v, kCFNumberSInt64Type, &x);
	return x > 0 ? (uint64_t)x : 0;
}

/*
 * Is the IOMedia with this BSD name still present in IOKit: 1 yes, 0 no.
 * Observed: after hdiutil detach -force while this process has the device
 * open, the /dev node does not go away (stat keeps working); only the IOMedia disappears
 * (same on power loss: the node and its ioctls keep working).
 */
static int media_present(const char *bsd)
{
	io_service_t m = IOServiceGetMatchingService(MACH_PORT_NULL, IOBSDNameMatching(MACH_PORT_NULL, 0, bsd));

	if (!m)
		return 0;
	IOObjectRelease(m);
	return 1;
}

uint64_t openmacfuse_devwatch_iostat(const char *bsd, int *ok)
{
	io_service_t media;
	CFTypeRef st;
	uint64_t sum = 0;

	*ok = 0;
	if (!bsd || !*bsd)
		return 0;
	media = IOServiceGetMatchingService(MACH_PORT_NULL, IOBSDNameMatching(MACH_PORT_NULL, 0, bsd));
	if (!media)
		return 0;
	/* The statistics hang off the IOBlockStorageDriver above (shared by the partitions and the whole disk; any progress is enough) */
	st = IORegistryEntrySearchCFProperty(media, kIOServicePlane, CFSTR("Statistics"),
					     kCFAllocatorDefault,
					     kIORegistryIterateRecursively | kIORegistryIterateParents);
	IOObjectRelease(media);
	if (!st)
		return 0;
	if (CFGetTypeID(st) == CFDictionaryGetTypeID()) {
		CFDictionaryRef d = st;

		sum = dict_num(d, "Operations (Read)") + dict_num(d, "Operations (Write)") +
		      dict_num(d, "Errors (Read)") + dict_num(d, "Errors (Write)");
		*ok = 1;
	}
	CFRelease(st);
	return sum;
}

int openmacfuse_devwatch_prepare(void)
{
	const char *d = getenv("OPENMACFUSE_WATCH_DEV"), *t = getenv("OPENMACFUSE_TEST_DEVLOST_FILE");
	struct stat st;

	g_have_dev = 0;
	g_dev[0] = g_bsd[0] = g_lostfile[0] = 0;
	if (t && *t)
		snprintf(g_lostfile, sizeof(g_lostfile), "%s", t);
	if (d && *d) {
		snprintf(g_dev, sizeof(g_dev), "%s", d);
		if (stat(g_dev, &st) != 0)
			return -1;
		g_rdev = S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode) ? (uint64_t)st.st_rdev : (uint64_t)st.st_ino;
		g_have_dev = 1;
		bsd_name(g_dev, g_bsd, sizeof(g_bsd));
		g_io_last = openmacfuse_devwatch_iostat(g_bsd, &g_iostat_ok);
		g_media_ok = g_bsd[0] && media_present(g_bsd);
	}
	return g_have_dev || g_lostfile[0] ? 0 : -1;
}

uint64_t openmacfuse_devwatch_progress(void)
{
	return atomic_load_explicit(&g_progress, memory_order_relaxed);
}

/* Device still there and still the same one: 1; gone: 0 (why gets the reason, how / err the telemetry fields) */
static int dev_present(const char **why, const char **how, int *err)
{
	struct stat st;

	if (g_lostfile[0] && access(g_lostfile, F_OK) == 0) {
		*why = "test hook OPENMACFUSE_TEST_DEVLOST_FILE";
		*how = "test_hook";
		*err = ENXIO;
		return 0;
	}
	if (!g_have_dev)
		return 1;
	if (g_media_ok && !media_present(g_bsd)) {
		*why = "device no longer present in IOKit (unplugged / power loss / image force-ejected)";
		*how = "media_gone";
		*err = ENXIO;
		return 0;
	}
	if (stat(g_dev, &st) != 0) {
		if (errno == ENOENT || errno == ENXIO) {
			*err = errno;
			*why = "device node disappeared (unplugged / power loss)";
			*how = "node_gone";
			return 0;
		}
		return 1;	/* other errors (permissions, transient) do not count */
	}
	if ((S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode) ? (uint64_t)st.st_rdev : (uint64_t)st.st_ino) != g_rdev) {
		*why = "device node now refers to a different device";
		*how = "node_changed";
		*err = ENXIO;
		return 0;
	}
	return 1;
}

static void *dw_thread(void *arg)
{
	int reported = 0;

	(void)arg;
	pthread_mutex_lock(&g_mu);
	while (!g_stop) {
		struct timeval now;
		struct timespec ts;
		const char *why = NULL, *how = "";
		int err = 0;

		gettimeofday(&now, NULL);
		ts.tv_sec = now.tv_sec + (now.tv_usec / 1000 + DW_PERIOD_MS) / 1000;
		ts.tv_nsec = (long)(((now.tv_usec / 1000 + DW_PERIOD_MS) % 1000) * 1000000L);
		pthread_cond_timedwait(&g_cv, &g_mu, &ts);
		if (g_stop)
			break;
		pthread_mutex_unlock(&g_mu);
		if (!dev_present(&why, &how, &err)) {
			if (!reported) {
				reported = 1;
				openmacfuse_report("device_lost", "\"err\":%d,\"how\":\"%s\",\"probe_ms\":%d", err, how,
					     DW_PERIOD_MS);
			}
			/*
			 * May be called repeatedly (the library only honours the first call). During the mount
			 * handshake (fuse_new has not returned, g_f is still NULL) the library uses the current
			 * instance; a call made before the instance exists has no effect and is repeated on the
			 * next round. After that fuse_loop returns and stop reaps this thread.
			 */
			openmacfuse_device_lost(g_f, why);
			pthread_mutex_lock(&g_mu);
			continue;
		}
		if (g_iostat_ok) {
			int ok;
			uint64_t v = openmacfuse_devwatch_iostat(g_bsd, &ok);

			if (ok && v != g_io_last) {
				g_io_last = v;
				atomic_fetch_add_explicit(&g_progress, 1, memory_order_relaxed);
			}
		}
		pthread_mutex_lock(&g_mu);
	}
	pthread_mutex_unlock(&g_mu);
	return NULL;
}

int openmacfuse_devwatch_start(struct fuse *f)
{
	if (g_running)
		return 0;
	g_f = f;
	g_stop = 0;
	if (pthread_create(&g_th, NULL, dw_thread, NULL) != 0)
		return -1;
	g_running = 1;
	return 0;
}

void openmacfuse_devwatch_stop(void)
{
	if (!g_running)
		return;
	pthread_mutex_lock(&g_mu);
	g_stop = 1;
	pthread_cond_signal(&g_cv);
	pthread_mutex_unlock(&g_mu);
	pthread_join(g_th, NULL);
	g_running = 0;
	g_f = NULL;
}
