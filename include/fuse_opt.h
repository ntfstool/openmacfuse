/*
 * fuse_opt.h -- FUSE 2.x compatibility header of openmacfuse: command-line argument list
 *
 * Adding / removing arguments, plus the template parser fuse_opt_parse (used by FUSE 2.x file
 * systems that parse their own options; implemented in src/fuse_opt_parse.c as an independent
 * implementation of the semantics described in the public FUSE 2.x documentation).
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#ifndef OPENMACFUSE_FUSE_OPT_H
#define OPENMACFUSE_FUSE_OPT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Argument list: argv is NULL-terminated; non-zero allocated means argv was allocated by this library and must be freed */
struct fuse_args {
	int	argc;
	char	**argv;
	int	allocated;
};

#define FUSE_ARGS_INIT(argc, argv)	{ argc, argv, 0 }

/* Append one argument (the string is copied); 0 on success, -1 on failure */
int fuse_opt_add_arg(struct fuse_args *args, const char *arg);
/* Insert one argument at position pos */
int fuse_opt_insert_arg(struct fuse_args *args, int pos, const char *arg);
/* Free an argument list allocated by this library */
void fuse_opt_free_args(struct fuse_args *args);
/* Append one item to a comma-separated option string (created when *opts is NULL) */
int fuse_opt_add_opt(char **opts, const char *opt);
/* Same, but ',' and '\\' in opt are escaped */
int fuse_opt_add_opt_escaped(char **opts, const char *opt);

/* ---------- Template parsing ----------
 *
 * The forms a template (templ) can take; they decide what is matched and how the parameter is taken:
 *   "-x" "-foo" "--foo"     matches exactly this argument
 *   "foo"                   matches the item foo in a -o option string (-o foo, -ofoo, -o a,foo,b all count)
 *   "foo=" "--foo="         takes a parameter: matches an item / argument starting with it
 *   "foo=%s" "--foo=%lu"    same, and the parameter is parsed with the format and stored at data + offset
 *   "-x " "-foo "           the parameter is the next command-line argument (-x val) or directly attached (-xval)
 *   "-x %s"                 the previous form + a format
 * With the format "%s" a newly allocated string is stored (a non-NULL pointer already there is
 * freed first); any other format is parsed by sscanf and must consume the whole parameter,
 * otherwise it is an error.
 * When offset is FUSE_OPT_KEY no value is stored; proc is called instead, with key = value.
 * Without a format and with an offset that is not a key, a match writes value to data + offset (int).
 * One argument may match several templates; all of them are processed, in order.
 */
struct fuse_opt {
	const char	*templ;
	unsigned long	offset;
	int		value;
};

#define OPENMACFUSE_OPT_OFFSET_KEY	((unsigned long)-1)
#define FUSE_OPT_KEY(templ, key)	{ templ, OPENMACFUSE_OPT_OFFSET_KEY, key }
#define FUSE_OPT_END			{ NULL, 0, 0 }

/* Options no template matches (an item of -o, or an argument starting with -) are passed to proc with this key */
#define FUSE_OPT_KEY_OPT	-1
/* Arguments that do not start with - (mount point, device, ...) are passed to proc with this key */
#define FUSE_OPT_KEY_NONOPT	-2
/* As a template's value: a match is kept unchanged in the output argument list, proc is not called */
#define FUSE_OPT_KEY_KEEP	-3
/* As a template's value: a match is discarded, proc is not called */
#define FUSE_OPT_KEY_DISCARD	-4

/*
 * Return value of proc: -1 error (fuse_opt_parse returns -1); 0 discard this argument; 1 keep it
 * in the output argument list.
 * arg: one item of -o (without the -o), or a whole command-line argument (for the "-x val" form
 * it is the joined "-xval").
 * outargs: the output argument list under construction; proc may add to it.
 */
typedef int (*fuse_opt_proc_t)(void *data, const char *arg, int key,
			       struct fuse_args *outargs);

/*
 * Parse args according to the templates (argv[0] is kept as is); the result replaces args: the
 * -o items that were kept are merged into a single "-o a,b,c" placed right after argv[0], the
 * other kept arguments follow in their original order. Everything after "--" is a non-option.
 * opts may be NULL; with proc NULL everything unmatched is kept. 0 on success, -1 on failure
 * (args unchanged).
 */
int fuse_opt_parse(struct fuse_args *args, void *data,
		   const struct fuse_opt opts[], fuse_opt_proc_t proc);

/* Can opt (an item of -o, or an argument starting with -) match one of the templates in opts: 1 yes, 0 no */
int fuse_opt_match(const struct fuse_opt opts[], const char *opt);

#ifdef __cplusplus
}
#endif

#endif /* OPENMACFUSE_FUSE_OPT_H */
