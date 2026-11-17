/*
 * xattrtool.c -- test helper: read and write large extended attribute values (resource forks
 *                etc.); the xattr command line tool is awkward for values of several MB
 *
 *   xattrtool set  <file> <attr name> <value file> [chunk size]   with a non-zero chunk size the resource fork is written in chunks by position
 *   xattrtool get  <file> <attr name> <output file>
 *   xattrtool size <file> <attr name>
 *   xattrtool list <file>
 *   xattrtool hidden <file>        prints UF_HIDDEN from st_flags (0/1)
 *
 * Exit code 0 on success; otherwise errno is printed.
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

static char *slurp(const char *p, size_t *n)
{
	FILE *f = fopen(p, "rb");
	char *b;
	long l;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	l = ftell(f);
	fseek(f, 0, SEEK_SET);
	b = malloc(l ? (size_t)l : 1);
	if (b && fread(b, 1, (size_t)l, f) != (size_t)l) {
		free(b);
		b = NULL;
	}
	fclose(f);
	*n = (size_t)l;
	return b;
}

int main(int argc, char **argv)
{
	if (argc >= 5 && strcmp(argv[1], "set") == 0) {
		size_t n, chunk = argc >= 6 ? strtoul(argv[5], NULL, 10) : 0, off = 0;
		char *v = slurp(argv[4], &n);

		if (!v) {
			perror("read value file");
			return 2;
		}
		if (!chunk || strcmp(argv[3], XATTR_RESOURCEFORK_NAME) != 0) {
			if (setxattr(argv[2], argv[3], v, n, 0, 0)) {
				printf("setxattr errno=%d %s\n", errno, strerror(errno));
				return 1;
			}
			return 0;
		}
		do {
			size_t k = n - off < chunk ? n - off : chunk;
			if (setxattr(argv[2], argv[3], v + off, k, (u_int32_t)off, 0)) {
				printf("setxattr(pos=%zu) errno=%d %s\n", off, errno, strerror(errno));
				return 1;
			}
			off += k;
		} while (off < n);
		return 0;
	}
	if (argc >= 5 && strcmp(argv[1], "get") == 0) {
		ssize_t n = getxattr(argv[2], argv[3], NULL, 0, 0, 0), m;
		char *v;
		FILE *f;

		if (n < 0) {
			printf("getxattr errno=%d %s\n", errno, strerror(errno));
			return 1;
		}
		v = malloc(n ? (size_t)n : 1);
		m = getxattr(argv[2], argv[3], v, (size_t)n, 0, 0);
		if (m < 0) {
			printf("getxattr errno=%d %s\n", errno, strerror(errno));
			return 1;
		}
		f = fopen(argv[4], "wb");
		if (!f || fwrite(v, 1, (size_t)m, f) != (size_t)m) {
			perror("write output");
			return 2;
		}
		fclose(f);
		return 0;
	}
	if (argc >= 4 && strcmp(argv[1], "size") == 0) {
		ssize_t n = getxattr(argv[2], argv[3], NULL, 0, 0, 0);
		if (n < 0) {
			printf("errno=%d\n", errno);
			return 1;
		}
		printf("%zd\n", n);
		return 0;
	}
	if (argc >= 3 && strcmp(argv[1], "list") == 0) {
		char buf[65536];
		ssize_t n = listxattr(argv[2], buf, sizeof(buf), 0), i;
		if (n < 0) {
			printf("errno=%d\n", errno);
			return 1;
		}
		for (i = 0; i < n; i += (ssize_t)strlen(buf + i) + 1)
			printf("%s\n", buf + i);
		return 0;
	}
	if (argc >= 3 && strcmp(argv[1], "hidden") == 0) {
		struct stat st;
		if (lstat(argv[2], &st)) {
			printf("errno=%d\n", errno);
			return 1;
		}
		printf("%d\n", (st.st_flags & UF_HIDDEN) ? 1 : 0);
		return 0;
	}
	fprintf(stderr, "usage: see the file header\n");
	return 2;
}
