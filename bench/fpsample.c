/*
 * fpsample.c -- sample the real memory of processes at a fixed interval (physical footprint, the
 *               same figure as "Memory" in Activity Monitor and the footprint command); with
 *               several processes the total is printed as well
 *
 * Uses proc_pid_rusage(RUSAGE_INFO_V4).ri_phys_footprint; one sample takes only a few
 * microseconds, so even a 0.2 s interval is not distorted (one run of the footprint command
 * takes tens to hundreds of milliseconds). Only this user's processes can be sampled.
 *
 * Usage: fpsample <interval ms> <pid> [pid ...]
 *   Each output line: relative-ms total-MB per-process-MB...; on SIGINT / SIGTERM or once all
 *   processes have exited, the last line is "# peak X MB, N samples".
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#include <libproc.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop;

static void on_sig(int s)
{
	(void)s;
	stop = 1;
}

static double now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

int main(int argc, char **argv)
{
	int ms, n, i;
	pid_t *pids;
	double t0, peak = 0;
	long samples = 0;

	if (argc < 3) {
		fprintf(stderr, "usage: fpsample <interval ms> <pid> [pid ...]\n");
		return 64;
	}
	ms = atoi(argv[1]);
	n = argc - 2;
	pids = calloc((size_t)n, sizeof(*pids));
	for (i = 0; i < n; i++)
		pids[i] = (pid_t)atoi(argv[i + 2]);
	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	setvbuf(stdout, NULL, _IOLBF, 0);
	t0 = now_ms();
	while (!stop) {
		double tot = 0, v[64];
		int alive = 0;

		for (i = 0; i < n && i < 64; i++) {
			struct rusage_info_v4 ri;

			v[i] = 0;
			if (proc_pid_rusage(pids[i], RUSAGE_INFO_V4, (rusage_info_t *)&ri) == 0) {
				v[i] = ri.ri_phys_footprint / 1048576.0;
				alive++;
			}
			tot += v[i];
		}
		if (!alive)
			break;
		printf("%.0f %.1f", now_ms() - t0, tot);
		for (i = 0; i < n && i < 64; i++)
			printf(" %.1f", v[i]);
		printf("\n");
		if (tot > peak)
			peak = tot;
		samples++;
		usleep((useconds_t)ms * 1000);
	}
	printf("# peak %.1f MB, %ld samples\n", peak, samples);
	return 0;
}
