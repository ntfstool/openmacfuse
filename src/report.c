/*
 * report.c - structured telemetry for fuse_main (described in report.h)
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#include <fcntl.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "report.h"

static int g_fd = -1;
static char g_tool[64];
static _Atomic unsigned long long g_seq;

char *nfuse_report_esc(const char *s, char *out, size_t n)
{
	size_t o = 0;

	if (!n)
		return out;
	if (!s)
		s = "";
	for (; *s && o + 7 < n; s++) {
		unsigned char c = (unsigned char)*s;

		if (c == '"' || c == '\\') {
			out[o++] = '\\';
			out[o++] = (char)c;
		} else if (c == '\n') {
			out[o++] = '\\';
			out[o++] = 'n';
		} else if (c < 0x20) {
			o += (size_t)snprintf(out + o, n - o, "\\u%04x", c);
		} else {
			out[o++] = (char)c;
		}
	}
	out[o] = 0;
	return out;
}

int nfuse_report_open(const char *tool)
{
	const char *p = getenv("NFUSE_REPORT");
	const char *b = tool ? strrchr(tool, '/') : NULL;
	char esc[sizeof(g_tool)];

	if (g_fd >= 0)
		return 0;
	if (!p || !*p)
		return -1;
	/* the guard process inherits it across fork; mount_nfs does not need it */
	g_fd = open(p, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
	if (g_fd < 0)
		return -1;
	/*
	 * The program name is the real executable name of this process (getprogname), not the
	 * argv[0] the caller passed to fuse_main, which cannot be trusted: exfat-fuse always
	 * passes "exfat" (so its telemetry used to report tool as exfat). argv[0] is used only
	 * when the real name is unavailable.
	 */
	{
		const char *pn = getprogname();

		if (pn && *pn)
			b = NULL, tool = pn;
	}
	nfuse_report_esc(b ? b + 1 : tool ? tool : "?", esc, sizeof(esc));
	snprintf(g_tool, sizeof(g_tool), "%s", esc);
	return 0;
}

int nfuse_report_fd(void)
{
	return g_fd;
}

void nfuse_report_close(void)
{
	if (g_fd >= 0)
		close(g_fd);
	g_fd = -1;
}

void nfuse_report(const char *type, const char *fmt, ...)
{
	char f[2048], line[2400];
	struct timeval tv;
	va_list ap;
	int n;

	if (g_fd < 0)
		return;
	f[0] = 0;
	if (fmt) {
		va_start(ap, fmt);
		vsnprintf(f, sizeof(f), fmt, ap);
		va_end(ap);
	}
	gettimeofday(&tv, NULL);
	n = snprintf(line, sizeof(line),
		     "{\"ts\":%llu,\"pid\":%d,\"role\":\"nfuse\",\"seq\":%llu,\"type\":\"%s\",\"tool\":\"%s\"%s%s}\n",
		     (unsigned long long)tv.tv_sec * 1000 + (unsigned long long)tv.tv_usec / 1000, (int)getpid(),
		     (unsigned long long)atomic_fetch_add(&g_seq, 1) + 1, type, g_tool, f[0] ? "," : "", f);
	if (n <= 0)
		return;
	if ((size_t)n >= sizeof(line)) {
		n = (int)sizeof(line) - 1;
		line[n - 2] = '}';
		line[n - 1] = '\n';
	}
	(void)!write(g_fd, line, (size_t)n);
}
