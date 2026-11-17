/*
 * hello.c - the smallest useful nfuse file system.
 *
 * One read-only file, /hello.txt. Builds against the FUSE 2.6 high-level API
 * and links statically with libnfuse.a. No kernel extension and no third-party
 * component: nfuse serves NFSv4.0 inside this process and macOS's own
 * mount_nfs mounts it.
 *
 *   make -C ../.. lib
 *   cc -I../../include -o hello hello.c ../../build/libnfuse.a -lpthread -framework CoreFoundation -framework IOKit
 *   mkdir -p /tmp/hello && ./hello /tmp/hello
 *   cat /tmp/hello/hello.txt
 *   umount /tmp/hello          # hello exits on its own after unmount
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#define FUSE_USE_VERSION 26

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <fuse.h>
#include <fuse_opt.h>

static const char *hello_path = "/hello.txt";
static const char *hello_str = "Hello from nfuse\n";

/*
 * nfuse addresses objects by inode number, so every object needs a stable,
 * unique, non-zero st_ino. (Many libfuse examples leave st_ino at 0 and rely
 * on the "use_ino" option being off; nfuse does not support that mode.)
 */
#define ROOT_INO	1
#define HELLO_INO	2

static int hello_getattr(const char *path, struct stat *st)
{
	memset(st, 0, sizeof(*st));
	st->st_uid = getuid();
	st->st_gid = getgid();
	if (strcmp(path, "/") == 0) {
		st->st_ino = ROOT_INO;
		st->st_mode = S_IFDIR | 0755;
		st->st_nlink = 2;
		return 0;
	}
	if (strcmp(path, hello_path) == 0) {
		st->st_ino = HELLO_INO;
		st->st_mode = S_IFREG | 0444;
		st->st_nlink = 1;
		st->st_size = (off_t)strlen(hello_str);
		return 0;
	}
	return -ENOENT;
}

static int hello_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			 off_t offset, struct fuse_file_info *fi)
{
	struct stat st;

	(void)offset;
	(void)fi;
	if (strcmp(path, "/") != 0)
		return -ENOENT;
	/* nfuse lists a directory once and pages from a snapshot; "." and ".."
	 * are accepted and skipped. */
	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);
	hello_getattr(hello_path, &st);
	filler(buf, hello_path + 1, &st, 0);
	return 0;
}

static int hello_open(const char *path, struct fuse_file_info *fi)
{
	if (strcmp(path, hello_path) != 0)
		return -ENOENT;
	if ((fi->flags & O_ACCMODE) != O_RDONLY)
		return -EACCES;
	return 0;
}

static int hello_read(const char *path, char *buf, size_t size, off_t offset,
		      struct fuse_file_info *fi)
{
	size_t len = strlen(hello_str);

	(void)fi;
	if (strcmp(path, hello_path) != 0)
		return -ENOENT;
	if (offset < 0 || (size_t)offset >= len)
		return 0;
	if (offset + size > len)
		size = len - (size_t)offset;
	memcpy(buf, hello_str + offset, size);
	return (int)size;
}

static const struct fuse_operations hello_ops = {
	.getattr = hello_getattr,
	.readdir = hello_readdir,
	.open    = hello_open,
	.read    = hello_read,
	/* No .write callback: nfuse mounts the volume read-only. */
};

int main(int argc, char *argv[])
{
	/* nfuse 1.0 provides the explicit FUSE 2.6 sequence; fuse_main() is
	 * not provided (see README, "Known limitations"). */
	char *fake_argv[] = { argv[0], "-o", "volname=Hello", NULL };
	struct fuse_args args = FUSE_ARGS_INIT(3, fake_argv);
	struct fuse_chan *ch;
	struct fuse *f;
	int rc;

	if (argc != 2) {
		fprintf(stderr, "usage: %s <mountpoint>\n", argv[0]);
		return 2;
	}
	ch = fuse_mount(argv[1], &args);
	if (!ch) {
		fprintf(stderr, "hello: fuse_mount failed\n");
		return 1;
	}
	f = fuse_new(ch, &args, &hello_ops, sizeof(hello_ops), NULL);
	if (!f) {
		fuse_unmount(argv[1], ch);
		return 1;
	}
	fuse_set_signal_handlers(fuse_get_session(f));
	rc = fuse_loop(f);		/* returns after umount or SIGINT/SIGTERM */
	fuse_remove_signal_handlers(fuse_get_session(f));
	fuse_unmount(argv[1], ch);
	fuse_destroy(f);
	return rc ? 1 : 0;
}
