/*
 * fuse_opt.c -- FUSE 2.x argument-list functions (compatibility layer; only the few a driver needs)
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>

#include "fuse_opt.h"

/* Before the first modification of a caller-supplied argv, take a copy that this library owns */
static int own_args(struct fuse_args *args)
{
	char **nv;
	int i;

	if (args->allocated)
		return 0;
	nv = calloc((size_t)args->argc + 1, sizeof(char *));
	if (!nv)
		return -1;
	for (i = 0; i < args->argc; i++) {
		nv[i] = strdup(args->argv[i]);
		if (!nv[i]) {
			while (i--)
				free(nv[i]);
			free(nv);
			return -1;
		}
	}
	args->argv = nv;
	args->allocated = 1;
	return 0;
}

int fuse_opt_insert_arg(struct fuse_args *args, int pos, const char *arg)
{
	char **nv, *dup;

	if (pos < 0 || pos > args->argc || own_args(args))
		return -1;
	dup = strdup(arg);
	if (!dup)
		return -1;
	nv = realloc(args->argv, ((size_t)args->argc + 2) * sizeof(char *));
	if (!nv) {
		free(dup);
		return -1;
	}
	memmove(nv + pos + 1, nv + pos, (size_t)(args->argc - pos) * sizeof(char *));
	nv[pos] = dup;
	args->argc++;
	nv[args->argc] = NULL;
	args->argv = nv;
	return 0;
}

int fuse_opt_add_arg(struct fuse_args *args, const char *arg)
{
	return fuse_opt_insert_arg(args, args->argc, arg);
}

void fuse_opt_free_args(struct fuse_args *args)
{
	int i;

	if (args && args->allocated) {
		for (i = 0; i < args->argc; i++)
			free(args->argv[i]);
		free(args->argv);
	}
	if (args) {
		args->argv = NULL;
		args->argc = 0;
		args->allocated = 0;
	}
}

static int add_opt_common(char **opts, const char *opt, int esc)
{
	size_t ol = *opts ? strlen(*opts) : 0, add = 0, i;
	char *n, *p;

	for (i = 0; opt[i]; i++)
		add += (esc && (opt[i] == ',' || opt[i] == '\\')) ? 2 : 1;
	n = realloc(*opts, ol + 1 + add + 1);
	if (!n)
		return -1;
	p = n + ol;
	if (ol)
		*p++ = ',';
	for (i = 0; opt[i]; i++) {
		if (esc && (opt[i] == ',' || opt[i] == '\\'))
			*p++ = '\\';
		*p++ = opt[i];
	}
	*p = 0;
	*opts = n;
	return 0;
}

int fuse_opt_add_opt(char **opts, const char *opt)
{
	return add_opt_common(opts, opt, 0);
}

int fuse_opt_add_opt_escaped(char **opts, const char *opt)
{
	return add_opt_common(opts, opt, 1);
}
