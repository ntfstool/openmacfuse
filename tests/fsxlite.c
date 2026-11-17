/*
 * fsxlite.c -- randomised read/write consistency test (written from scratch; same idea as the
 *              classic fsx: random operations checked byte by byte against an in-memory shadow buffer)
 *
 * Randomly performs on one file: writes (any offset / length, may go past the end of file and
 * leave a hole), reads (compared with the shadow), truncates (shorter / longer), fsync, close
 * and reopen (with 50% probability using F_NOCACHE, which bypasses the client page cache so
 * reads and writes go straight to the server), stat to check the size, whole-file checks. Any
 * single mismatching byte is reported at once and the program exits with 1.
 *
 * Usage: fsxlite <file> <seconds> [max length in bytes, default 8MB] [random seed, default from the time]
 * Output: count of each kind of operation, bytes read and written, number of mismatches (should be 0).
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static uint64_t rs;

static uint64_t rnd(void)
{
	/* xorshift64* */
	rs ^= rs >> 12;
	rs ^= rs << 25;
	rs ^= rs >> 27;
	return rs * 2685821657736338717ull;
}

static uint64_t rndn(uint64_t n)
{
	return n ? rnd() % n : 0;
}

static const char *path;
static int fd = -1, nocache;
static uint8_t *shadow, *buf;
static uint64_t size, maxlen, opn;

static void die(const char *what, uint64_t off, uint64_t len)
{
	fprintf(stderr, "fsxlite: operation %llu %s failed (off=%llu len=%llu size=%llu): %s\n",
		(unsigned long long)opn, what, (unsigned long long)off,
		(unsigned long long)len, (unsigned long long)size, strerror(errno));
	exit(2);
}

static void reopen(void)
{
	if (fd >= 0 && close(fd) < 0)
		die("close", 0, 0);
	fd = open(path, O_RDWR);
	if (fd < 0)
		die("open", 0, 0);
	nocache = (int)rndn(2);
	if (nocache)
		fcntl(fd, F_NOCACHE, 1);
}

/* Read [off, off+len) and compare with the shadow; returns the number of mismatching bytes */
static uint64_t check(uint64_t off, uint64_t len, const char *what)
{
	ssize_t got;
	uint64_t want, i;

	got = pread(fd, buf, len, (off_t)off);
	if (got < 0)
		die("pread", off, len);
	want = off >= size ? 0 : (off + len > size ? size - off : len);
	if ((uint64_t)got != want) {
		fprintf(stderr, "fsxlite: operation %llu %s: read %zd bytes, expected %llu (off=%llu len=%llu size=%llu nocache=%d)\n",
			(unsigned long long)opn, what, got, (unsigned long long)want,
			(unsigned long long)off, (unsigned long long)len,
			(unsigned long long)size, nocache);
		return 1;
	}
	for (i = 0; i < want; i++)
		if (buf[i] != shadow[off + i]) {
			fprintf(stderr, "fsxlite: operation %llu %s: content mismatch at offset %llu (read %02x, expected %02x; size=%llu nocache=%d)\n",
				(unsigned long long)opn, what,
				(unsigned long long)(off + i), buf[i], shadow[off + i],
				(unsigned long long)size, nocache);
			return 1;
		}
	return 0;
}

int main(int argc, char **argv)
{
	uint64_t nw = 0, nr = 0, nt = 0, nf = 0, nre = 0, ns = 0, nfull = 0;
	uint64_t bw = 0, br = 0, bad = 0, seed;
	time_t end;
	int secs;

	if (argc < 3) {
		fprintf(stderr, "usage: fsxlite <file> <seconds> [max length] [seed]\n");
		return 64;
	}
	path = argv[1];
	secs = atoi(argv[2]);
	maxlen = argc > 3 ? strtoull(argv[3], NULL, 10) : 8u << 20;
	seed = argc > 4 ? strtoull(argv[4], NULL, 10) : (uint64_t)time(NULL);
	rs = seed ? seed : 1;
	shadow = calloc(1, maxlen);
	buf = malloc(maxlen + 65536);
	if (!shadow || !buf)
		return 3;
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		die("open(O_CREAT)", 0, 0);
	end = time(NULL) + secs;
	while (time(NULL) < end && !bad) {
		unsigned op = (unsigned)rndn(100);
		uint64_t off, len, i;

		opn++;
		if (op < 40) {				/* write */
			off = rndn(maxlen);
			len = 1 + rndn(maxlen - off < 262144 ? maxlen - off : 262144);
			for (i = 0; i < len; i++)
				buf[i] = (uint8_t)(rnd() >> 56);
			if (pwrite(fd, buf, len, (off_t)off) != (ssize_t)len)
				die("pwrite", off, len);
			memcpy(shadow + off, buf, len);
			if (off + len > size)
				size = off + len;	/* hole in between: the shadow is already 0 there */
			nw++;
			bw += len;
		} else if (op < 70) {			/* read */
			off = rndn(size + 65536);
			len = 1 + rndn(262144);
			if (off + len > maxlen)
				len = off < maxlen ? maxlen - off : 1;
			if (off >= maxlen)
				off = maxlen - len;
			bad += check(off, len, "read");
			nr++;
			br += len;
		} else if (op < 80) {			/* truncate (shorter or longer) */
			uint64_t ns2 = rndn(maxlen + 1);

			if (ftruncate(fd, (off_t)ns2) < 0)
				die("ftruncate", ns2, 0);
			if (ns2 < size)
				memset(shadow + ns2, 0, size - ns2);
			size = ns2;
			nt++;
		} else if (op < 85) {
			if (fsync(fd) < 0)
				die("fsync", 0, 0);
			nf++;
		} else if (op < 90) {
			reopen();
			nre++;
		} else if (op < 97) {
			struct stat st;

			if (fstat(fd, &st) < 0)
				die("fstat", 0, 0);
			if ((uint64_t)st.st_size != size) {
				fprintf(stderr, "fsxlite: operation %llu stat: size %lld, expected %llu\n",
					(unsigned long long)opn, (long long)st.st_size,
					(unsigned long long)size);
				bad++;
			}
			ns++;
		} else {				/* whole-file check */
			bad += check(0, maxlen, "whole-file check");
			nfull++;
		}
	}
	if (!bad) {
		reopen();
		bad += check(0, maxlen, "final check");
	}
	close(fd);
	printf("fsxlite: %llu operations (write %llu / read %llu / truncate %llu / fsync %llu / reopen %llu / stat %llu / whole-file check %llu), "
	       "wrote %.1f MB, read %.1f MB, final size %llu, seed %llu, mismatches %llu\n",
	       (unsigned long long)opn, (unsigned long long)nw, (unsigned long long)nr,
	       (unsigned long long)nt, (unsigned long long)nf, (unsigned long long)nre,
	       (unsigned long long)ns, (unsigned long long)nfull, bw / 1048576.0,
	       br / 1048576.0, (unsigned long long)size, (unsigned long long)seed,
	       (unsigned long long)bad);
	return bad ? 1 : 0;
}
