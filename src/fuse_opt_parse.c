/*
 * fuse_opt_parse.c -- FUSE 2.x command-line template parsing (fuse_opt_parse / fuse_opt_match)
 *
 * Independent implementation of the semantics described in the comments of the public FUSE 2.x
 * headers (template syntax: see include/fuse_opt.h).
 * Kept in its own file: only programs that use it (a FUSE file system calling fuse_opt_parse,
 * fuse_main) pull it in from libnfuse.a at link time.
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fuse_opt.h"

struct optctx {
	void			*data;
	const struct fuse_opt	*opts;
	fuse_opt_proc_t		proc;
	struct fuse_args	out;
	char			*optstr;	/* -o items that were kept (comma separated, already escaped) */
	int			nonopt;		/* "--" has been seen */
};

/* Shape of a template */
enum tkind {
	T_EXACT,	/* "-x" / "foo": the whole string must be equal */
	T_EQ,		/* "foo=" / "foo=%s": prefix up to and including '=' */
	T_SEP,		/* "-x " / "-x %s": prefix up to the space; the parameter follows, or is the next argument */
};

struct tinfo {
	enum tkind	kind;
	size_t		klen;		/* length of the prefix to compare (includes '=' for T_EQ) */
	const char	*fmt;		/* format ("%..."), NULL if none */
};

static void templ_info(const char *t, struct tinfo *ti)
{
	const char *sp = strchr(t, ' '), *eq = strchr(t, '=');

	ti->fmt = NULL;
	if (sp) {
		ti->kind = T_SEP;
		ti->klen = (size_t)(sp - t);
		if (sp[1] == '%')
			ti->fmt = sp + 1;
	} else if (eq) {
		ti->kind = T_EQ;
		ti->klen = (size_t)(eq - t) + 1;
		if (eq[1] == '%')
			ti->fmt = eq + 1;
	} else {
		ti->kind = T_EXACT;
		ti->klen = strlen(t);
	}
}

/*
 * Does arg match template t? is_dash: arg is a command-line argument starting with '-'
 * (otherwise it is one item of a -o list).
 * On a match *param is set (start of the parameter inside arg; NULL for T_SEP when the
 * parameter is in the next command-line argument).
 */
static int templ_match(const char *t, const char *arg, int is_dash, const char **param,
		       struct tinfo *ti)
{
	if (!t)
		return 0;
	if ((t[0] == '-') != (is_dash != 0))
		return 0;
	templ_info(t, ti);
	*param = NULL;
	switch (ti->kind) {
	case T_EXACT:
		return strcmp(t, arg) == 0;
	case T_EQ:
		if (strncmp(t, arg, ti->klen) != 0)
			return 0;
		*param = arg + ti->klen;
		return 1;
	case T_SEP:
		if (strncmp(t, arg, ti->klen) != 0)
			return 0;
		if (arg[ti->klen] == 0)
			return 1;		/* the parameter is in the next command-line argument */
		*param = arg + ti->klen;
		return 1;
	}
	return 0;
}

int fuse_opt_match(const struct fuse_opt opts[], const char *opt)
{
	const struct fuse_opt *o;
	const char *param;
	struct tinfo ti;
	int dash;

	if (!opts || !opt)
		return 0;
	dash = opt[0] == '-';
	for (o = opts; o->templ; o++)
		if (templ_match(o->templ, opt, dash, &param, &ti))
			return 1;
	return 0;
}

/* Store param at data + offset according to the format */
static int store_value(void *data, const struct fuse_opt *o, const struct tinfo *ti,
		       const char *param, const char *arg)
{
	void *var = (char *)data + o->offset;

	if (!ti->fmt) {
		*(int *)var = o->value;
		return 0;
	}
	if (strcmp(ti->fmt, "%s") == 0) {
		char **sp = var, *dup = strdup(param ? param : "");

		if (!dup) {
			fprintf(stderr, "fuse: out of memory\n");
			return -1;
		}
		free(*sp);
		*sp = dup;
		return 0;
	}
	{
		/* Every other format goes to sscanf; the whole parameter must be consumed ("12abc" is an error) */
		char fmt[64];
		int pos = -1;

		if (strlen(ti->fmt) > sizeof(fmt) - 3) {
			fprintf(stderr, "fuse: template format too long: %s\n", o->templ);
			return -1;
		}
		snprintf(fmt, sizeof(fmt), "%s%%n", ti->fmt);
		if (!param || sscanf(param, fmt, var, &pos) != 1 || pos < 0 ||
		    param[pos] != 0) {
			fprintf(stderr, "fuse: invalid parameter: %s\n", arg);
			return -1;
		}
	}
	return 0;
}

static int keep_arg(struct optctx *c, const char *arg, int is_opt)
{
	if (is_opt)
		return fuse_opt_add_opt_escaped(&c->optstr, arg);
	return fuse_opt_add_arg(&c->out, arg);
}

static int call_proc(struct optctx *c, const char *arg, int key, int is_opt)
{
	int r;

	if (key == FUSE_OPT_KEY_DISCARD)
		return 0;
	if (key == FUSE_OPT_KEY_KEEP || !c->proc)
		return keep_arg(c, arg, is_opt);
	r = c->proc(c->data, arg, key, &c->out);
	if (r == -1)
		return -1;
	return r ? keep_arg(c, arg, is_opt) : 0;
}

/*
 * Process one option. is_opt: one item of a -o list; otherwise it is a command-line argument
 * starting with '-' and next is the following command-line argument (*used is set to 1 when a
 * T_SEP template consumes it).
 */
static int process_opt(struct optctx *c, const char *arg, int is_opt, const char *next,
		       int *used)
{
	const struct fuse_opt *o;
	int matched = 0, keep = 0, r;
	char *joined = NULL;

	for (o = c->opts; o && o->templ; o++) {
		const char *param;
		const char *a = arg;
		struct tinfo ti;

		if (!templ_match(o->templ, arg, !is_opt, &param, &ti))
			continue;
		matched = 1;
		if (ti.kind == T_SEP && !param) {
			/* "-x val": the parameter is the next command-line argument; proc gets them joined as "-xval" */
			if (!next) {
				fprintf(stderr, "fuse: missing argument to option %s\n", arg);
				free(joined);
				return -1;
			}
			*used = 1;
			param = next;
			if (!joined) {
				size_t n = strlen(arg) + strlen(next) + 1;

				joined = malloc(n);
				if (!joined)
					return -1;
				snprintf(joined, n, "%s%s", arg, next);
			}
			a = joined;
		}
		if (o->offset == NFUSE_OPT_OFFSET_KEY) {
			if (o->value == FUSE_OPT_KEY_DISCARD)
				continue;
			if (o->value == FUSE_OPT_KEY_KEEP || !c->proc) {
				keep = 1;
				continue;
			}
			r = c->proc(c->data, a, o->value, &c->out);
			if (r == -1) {
				free(joined);
				return -1;
			}
			if (r)
				keep = 1;
		} else if (store_value(c->data, o, &ti, param, a)) {
			free(joined);
			return -1;
		}
	}
	free(joined);
	if (!matched) {
		/* No template knows it: hand it to proc (FUSE_OPT_KEY_OPT) */
		r = call_proc(c, arg, FUSE_OPT_KEY_OPT, is_opt);
		return r < 0 ? -1 : 0;
	}
	if (!keep)
		return 0;
	/* Keep: kept as is (the two arguments of "-x val" are kept separately, not joined) */
	if (keep_arg(c, arg, is_opt))
		return -1;
	if (*used && !is_opt && fuse_opt_add_arg(&c->out, next))
		return -1;
	return 0;
}

/* Split the option string after -o (comma separated, "\," and "\\" escapes) and process each item */
static int process_optlist(struct optctx *c, const char *s)
{
	size_t n = strlen(s), k = 0;
	char *item = malloc(n + 1);
	int r = 0;

	if (!item)
		return -1;
	for (;; s++) {
		if (*s == '\\' && s[1]) {
			item[k++] = *++s;
			continue;
		}
		if (*s == ',' || *s == 0) {
			item[k] = 0;
			if (k) {
				int used = 0;

				r = process_opt(c, item, 1, NULL, &used);
				if (r)
					break;
			}
			k = 0;
			if (*s == 0)
				break;
			continue;
		}
		item[k++] = *s;
	}
	free(item);
	return r;
}

static int process_nonopt(struct optctx *c, const char *arg)
{
	return call_proc(c, arg, FUSE_OPT_KEY_NONOPT, 0) < 0 ? -1 : 0;
}

int fuse_opt_parse(struct fuse_args *args, void *data,
		   const struct fuse_opt opts[], fuse_opt_proc_t proc)
{
	struct optctx c;
	int i, dashdash = -1;

	if (!args || !args->argv || args->argc <= 0)
		return 0;
	memset(&c, 0, sizeof(c));
	c.data = data;
	c.opts = opts;
	c.proc = proc;
	if (fuse_opt_add_arg(&c.out, args->argv[0]))
		goto fail;
	for (i = 1; i < args->argc; i++) {
		const char *a = args->argv[i];

		if (!a)
			continue;
		if (c.nonopt || a[0] != '-' || a[1] == 0) {
			if (process_nonopt(&c, a))
				goto fail;
			continue;
		}
		if (strcmp(a, "--") == 0) {
			c.nonopt = 1;
			/* Keep the "--": non-option arguments kept after it must not be parsed as options by a later pass */
			dashdash = c.out.argc;
			if (fuse_opt_add_arg(&c.out, "--"))
				goto fail;
			continue;
		}
		if (strncmp(a, "-o", 2) == 0) {
			const char *list = a + 2;

			if (!*list) {
				if (i + 1 >= args->argc) {
					fprintf(stderr, "fuse: missing argument to -o\n");
					goto fail;
				}
				list = args->argv[++i];
			}
			if (process_optlist(&c, list))
				goto fail;
			continue;
		}
		{
			int used = 0;

			if (process_opt(&c, a, 0, i + 1 < args->argc ? args->argv[i + 1] : NULL, &used))
				goto fail;
			if (used)
				i++;
		}
	}
	/* Nothing was kept after the "--": drop it */
	if (dashdash >= 0 && dashdash == c.out.argc - 1) {
		free(c.out.argv[dashdash]);
		c.out.argv[dashdash] = NULL;
		c.out.argc--;
	}
	if (c.optstr) {
		if (fuse_opt_insert_arg(&c.out, 1, "-o") ||
		    fuse_opt_insert_arg(&c.out, 2, c.optstr))
			goto fail;
		free(c.optstr);
		c.optstr = NULL;
	}
	fuse_opt_free_args(args);
	*args = c.out;
	return 0;
fail:
	free(c.optstr);
	fuse_opt_free_args(&c.out);
	return -1;
}
