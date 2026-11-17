/*
 * unit_fuse_opt.c -- unit tests for fuse_opt_parse / fuse_opt_match (run on the development machine, nothing is mounted)
 *
 *   make -C nfuse test
 *
 * The template tables follow the way real FUSE file systems (ext, exFAT drivers) use the API
 * (values stored at an offset, %s / %lu / %i / %u / %llu, FUSE_OPT_KEY, KEEP / DISCARD,
 * separated "-x " parameters, "--", escapes, several templates matching at once, proc adding
 * arguments to outargs, arguments unchanged on error).
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fuse_opt.h"

static int g_fail, g_pass;

#define CHECK(cond, ...) do { \
	if (cond) { g_pass++; } else { g_fail++; fprintf(stderr, "  [FAIL] %s:%d: ", __FILE__, __LINE__); \
		fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

/* Join the argument list into one line (arguments separated by '|') for easy comparison */
static const char *joined(const struct fuse_args *a)
{
	static char buf[4096];
	size_t n = 0;
	int i;

	buf[0] = 0;
	for (i = 0; i < a->argc; i++)
		n += (size_t)snprintf(buf + n, sizeof(buf) - n, "%s%s", i ? "|" : "", a->argv[i]);
	return buf;
}

struct d {
	int		ro;
	int		fakeroot;
	unsigned long	offset;
	char		*lockfile;
	int		ival;
	unsigned	uval;
	unsigned long long llval;
	char		*dev;
	char		*cache;
	int		help, version, xflag;
	char		*xarg;
	int		ro_key;
	int		nkeep_opt;
	char		seen[1024];
};

enum { K_IGN, K_VER, K_HELP, K_CACHE, K_X, K_RO };

#define O(t, f, v) { t, offsetof(struct d, f), v }

static const struct fuse_opt tbl[] = {
	O("ro", ro, 1),
	O("rw", ro, 0),
	O("fakeroot", fakeroot, 1),
	O("offset=%lu", offset, 0),
	O("lockfile=%s", lockfile, 0),
	O("ival=%i", ival, 0),
	O("uval=%u", uval, 0),
	O("llval=%llu", llval, 0),
	FUSE_OPT_KEY("ro", K_RO),			/* matches together with the "ro" above */
	FUSE_OPT_KEY("user_xattr", K_IGN),
	FUSE_OPT_KEY("cache_size=%s", K_CACHE),
	FUSE_OPT_KEY("keepme", FUSE_OPT_KEY_KEEP),
	FUSE_OPT_KEY("dropme", FUSE_OPT_KEY_DISCARD),
	FUSE_OPT_KEY("-V", K_VER),
	FUSE_OPT_KEY("--version", K_VER),
	FUSE_OPT_KEY("-h", K_HELP),
	FUSE_OPT_KEY("-x ", K_X),
	FUSE_OPT_KEY("--keep-dash", FUSE_OPT_KEY_KEEP),
	FUSE_OPT_END
};

static int proc(void *data, const char *arg, int key, struct fuse_args *out)
{
	struct d *d = data;
	size_t n = strlen(d->seen);

	snprintf(d->seen + n, sizeof(d->seen) - n, "%s%d:%s", n ? " " : "", key, arg);
	switch (key) {
	case FUSE_OPT_KEY_NONOPT:
		if (!d->dev) {
			d->dev = strdup(arg);
			return 0;
		}
		return 1;
	case FUSE_OPT_KEY_OPT:
		d->nkeep_opt++;
		return strcmp(arg, "bad") == 0 ? -1 : 1;
	case K_IGN:
		return 0;
	case K_CACHE:
		d->cache = strdup(arg + 11);
		return 0;
	case K_VER:
		d->version = 1;
		fuse_opt_add_arg(out, "--version");	/* as real file systems do: proc adds an argument to the output list */
		return 0;
	case K_HELP:
		d->help = 1;
		return 0;
	case K_X:
		d->xflag++;
		free(d->xarg);
		d->xarg = strdup(arg);
		return 1;
	case K_RO:
		d->ro_key = 1;
		return 0;
	}
	return 1;
}

static void dfree(struct d *d)
{
	free(d->lockfile);
	free(d->dev);
	free(d->cache);
	free(d->xarg);
	memset(d, 0, sizeof(*d));
}

static void test_fuse2fs_like(void)
{
	char *argv[] = { "fuse2fs", "/dev/disk4s1", "/Volumes/LNX", "-o",
			 "volname=LNX,local,allow_other,no_default_opts,fakeroot,nosuid,nodev,nobrowse,ro,user_xattr,cache_size=64M,offset=4096,lockfile=/tmp/lk,dropme,keepme",
			 NULL };
	struct fuse_args a = FUSE_ARGS_INIT(5, argv);
	struct d d;

	memset(&d, 0, sizeof(d));
	CHECK(fuse_opt_parse(&a, &d, tbl, proc) == 0, "parse of an ext-driver-style command line succeeds");
	CHECK(d.dev && strcmp(d.dev, "/dev/disk4s1") == 0, "the first non-option is the device: %s", d.dev ? d.dev : "(null)");
	CHECK(d.ro == 1 && d.ro_key == 1, "ro matches both the offset template and the key template (ro=%d key=%d)", d.ro, d.ro_key);
	CHECK(d.fakeroot == 1, "fakeroot");
	CHECK(d.offset == 4096, "offset=%%lu -> %lu", d.offset);
	CHECK(d.lockfile && strcmp(d.lockfile, "/tmp/lk") == 0, "lockfile=%%s");
	CHECK(d.cache && strcmp(d.cache, "64M") == 0, "cache_size goes through the key, arg is the whole item");
	CHECK(strcmp(joined(&a),
		     "fuse2fs|-o|volname=LNX,local,allow_other,no_default_opts,nosuid,nodev,nobrowse,keepme|/Volumes/LNX") == 0,
	      "output argument list: %s", joined(&a));
	CHECK(a.allocated == 1, "the output argument list is owned by the library");
	fuse_opt_free_args(&a);
	dfree(&d);
}

static void test_formats(void)
{
	char *argv[] = { "p", "-oival=0x10,uval=42,llval=18446744073709551615", NULL };
	struct fuse_args a = FUSE_ARGS_INIT(2, argv);
	struct d d;

	memset(&d, 0, sizeof(d));
	CHECK(fuse_opt_parse(&a, &d, tbl, proc) == 0, "format parsing succeeds");
	CHECK(d.ival == 16, "%%i understands 0x: %d", d.ival);
	CHECK(d.uval == 42, "%%u: %u", d.uval);
	CHECK(d.llval == 18446744073709551615ULL, "%%llu maximum value");
	CHECK(strcmp(joined(&a), "p") == 0, "everything consumed by templates: %s", joined(&a));
	fuse_opt_free_args(&a);
	dfree(&d);
}

static void test_bad_format_keeps_args(void)
{
	char *argv[] = { "p", "-o", "offset=12abc", "/mnt", NULL };
	struct fuse_args a = FUSE_ARGS_INIT(4, argv);
	struct d d;

	memset(&d, 0, sizeof(d));
	CHECK(fuse_opt_parse(&a, &d, tbl, proc) == -1, "offset=12abc is an error");
	CHECK(a.argv == argv && a.argc == 4 && a.allocated == 0, "the argument list is left untouched on error");
	dfree(&d);
}

static void test_string_replaces_old(void)
{
	char *argv[] = { "p", "-olockfile=/a", "-olockfile=/b", "-olockfile=", NULL };
	struct fuse_args a = FUSE_ARGS_INIT(4, argv);
	struct d d;

	memset(&d, 0, sizeof(d));
	CHECK(fuse_opt_parse(&a, &d, tbl, proc) == 0, "%%s several times");
	CHECK(d.lockfile && d.lockfile[0] == 0, "the last one (empty string) wins, the earlier ones were freed");
	fuse_opt_free_args(&a);
	dfree(&d);
}

static void test_escape(void)
{
	char *argv[] = { "p", "-o", "volname=a\\,b,c\\\\d,ro", NULL };
	struct fuse_args a = FUSE_ARGS_INIT(3, argv);
	struct d d;

	memset(&d, 0, sizeof(d));
	CHECK(fuse_opt_parse(&a, &d, tbl, proc) == 0, "escape parsing");
	CHECK(strstr(d.seen, "-1:volname=a,b") != NULL, "proc receives the unescaped volname=a,b: %s", d.seen);
	CHECK(strstr(d.seen, "-1:c\\d") != NULL, "proc receives c\\d: %s", d.seen);
	CHECK(strcmp(joined(&a), "p|-o|volname=a\\,b,c\\\\d") == 0, "kept items are escaped again: %s", joined(&a));
	fuse_opt_free_args(&a);
	dfree(&d);
}

static void test_sep_param(void)
{
	char *argv[] = { "p", "-x", "val", "-xval2", "/m", NULL };
	struct fuse_args a = FUSE_ARGS_INIT(5, argv);
	struct d d;

	memset(&d, 0, sizeof(d));
	CHECK(fuse_opt_parse(&a, &d, tbl, proc) == 0, "-x parameter");
	CHECK(d.xflag == 2, "both spellings match: %d", d.xflag);
	CHECK(strstr(d.seen, ":-xval ") != NULL || strstr(d.seen, ":-xval\0") != NULL,
	      "when written separately proc receives the joined -xval: %s", d.seen);
	CHECK(strcmp(joined(&a), "p|-x|val|-xval2") == 0, "kept arguments that were separate stay separate: %s", joined(&a));
	fuse_opt_free_args(&a);
	dfree(&d);
	{
		char *argv2[] = { "p", "-x", NULL };
		struct fuse_args b = FUSE_ARGS_INIT(2, argv2);

		CHECK(fuse_opt_parse(&b, &d, tbl, proc) == -1, "-x without a following parameter is an error");
		dfree(&d);
	}
}

static void test_dashdash(void)
{
	char *argv[] = { "p", "/dev/x", "--", "-f", "-oro", NULL };
	struct fuse_args a = FUSE_ARGS_INIT(5, argv);
	struct d d;

	memset(&d, 0, sizeof(d));
	CHECK(fuse_opt_parse(&a, &d, tbl, proc) == 0, "-- parsing");
	CHECK(d.ro == 0, "-oro after -- is not treated as an option");
	CHECK(strcmp(joined(&a), "p|--|-f|-oro") == 0, "-- is kept in front: %s", joined(&a));
	fuse_opt_free_args(&a);
	dfree(&d);
	{
		char *argv2[] = { "p", "/dev/x", "--", NULL };
		struct fuse_args b = FUSE_ARGS_INIT(3, argv2);

		CHECK(fuse_opt_parse(&b, &d, tbl, proc) == 0 && strcmp(joined(&b), "p") == 0,
		      "-- is dropped when nothing was kept after it: %s", joined(&b));
		fuse_opt_free_args(&b);
		dfree(&d);
	}
}

static void test_keep_discard_and_dash_keep(void)
{
	char *argv[] = { "p", "--keep-dash", "-odropme,keepme", "--version", NULL };
	struct fuse_args a = FUSE_ARGS_INIT(4, argv);
	struct d d;

	memset(&d, 0, sizeof(d));
	CHECK(fuse_opt_parse(&a, &d, tbl, proc) == 0, "KEEP / DISCARD");
	CHECK(d.version == 1, "--version goes through the key");
	CHECK(strcmp(joined(&a), "p|-o|keepme|--keep-dash|--version") == 0,
	      "-o merged and placed first; the --version added in proc comes after: %s", joined(&a));
	CHECK(strstr(d.seen, "dropme") == NULL && strstr(d.seen, "keepme") == NULL, "KEEP / DISCARD do not call proc");
	fuse_opt_free_args(&a);
	dfree(&d);
}

static void test_errors(void)
{
	struct d d;

	memset(&d, 0, sizeof(d));
	{
		char *argv[] = { "p", "-o", NULL };
		struct fuse_args a = FUSE_ARGS_INIT(2, argv);

		CHECK(fuse_opt_parse(&a, &d, tbl, proc) == -1, "-o without a parameter is an error");
	}
	{
		char *argv[] = { "p", "-o", "ro,bad", NULL };
		struct fuse_args a = FUSE_ARGS_INIT(3, argv);

		CHECK(fuse_opt_parse(&a, &d, tbl, proc) == -1, "proc returning -1 is an error");
		CHECK(a.argv == argv, "the argument list is untouched");
	}
	dfree(&d);
}

static void test_null_proc_and_table(void)
{
	char *argv[] = { "p", "/mnt", "-f", "-ofoo,bar=1", "-d", NULL };
	struct fuse_args a = FUSE_ARGS_INIT(5, argv);

	CHECK(fuse_opt_parse(&a, NULL, NULL, NULL) == 0, "no templates and no proc");
	CHECK(strcmp(joined(&a), "p|-o|foo,bar=1|/mnt|-f|-d") == 0, "everything is kept: %s", joined(&a));
	fuse_opt_free_args(&a);
	{
		struct fuse_args e = FUSE_ARGS_INIT(0, NULL);

		CHECK(fuse_opt_parse(&e, NULL, NULL, NULL) == 0, "empty argument list");
	}
}

static void test_allocated_input(void)
{
	struct fuse_args a = FUSE_ARGS_INIT(0, NULL);
	struct d d;

	memset(&d, 0, sizeof(d));
	fuse_opt_add_arg(&a, "p");
	fuse_opt_add_arg(&a, "-oro,zzz");
	fuse_opt_add_arg(&a, "/dev/a");
	fuse_opt_add_arg(&a, "/mnt");
	CHECK(fuse_opt_parse(&a, &d, tbl, proc) == 0, "input list allocated by the library");
	CHECK(strcmp(joined(&a), "p|-o|zzz|/mnt") == 0, "result: %s", joined(&a));
	/* Parse the result once more (fuse_main does this): the merged -o form is understood too */
	CHECK(fuse_opt_parse(&a, &d, tbl, proc) == 0 && strcmp(joined(&a), "p|-o|zzz|/mnt") == 0,
	      "parsing again leaves the result unchanged: %s", joined(&a));
	fuse_opt_free_args(&a);
	dfree(&d);
}

static void test_match(void)
{
	CHECK(fuse_opt_match(tbl, "ro") == 1, "match ro");
	CHECK(fuse_opt_match(tbl, "offset=1") == 1, "match offset=");
	CHECK(fuse_opt_match(tbl, "offset") == 0, "offset without = does not match");
	CHECK(fuse_opt_match(tbl, "-V") == 1, "match -V");
	CHECK(fuse_opt_match(tbl, "-xabc") == 1, "match -x with attached parameter");
	CHECK(fuse_opt_match(tbl, "-ro") == 0, "-ro does not match the ro of the -o space");
	CHECK(fuse_opt_match(tbl, "V") == 0, "V does not match -V");
	CHECK(fuse_opt_match(NULL, "ro") == 0, "empty table");
}

static void test_add_opt(void)
{
	char *o = NULL;

	CHECK(fuse_opt_add_opt(&o, "a") == 0 && fuse_opt_add_opt_escaped(&o, "b,c\\d") == 0, "add_opt");
	CHECK(o && strcmp(o, "a,b\\,c\\\\d") == 0, "escaping: %s", o ? o : "");
	free(o);
}

int main(void)
{
	test_fuse2fs_like();
	test_formats();
	test_bad_format_keeps_args();
	test_string_replaces_old();
	test_escape();
	test_sep_param();
	test_dashdash();
	test_keep_discard_and_dash_keep();
	test_errors();
	test_null_proc_and_table();
	test_allocated_input();
	test_match();
	test_add_opt();
	printf("unit_fuse_opt: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
