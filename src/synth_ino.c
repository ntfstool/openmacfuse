/*
 * synth_ino.c - assigns stable inode numbers by path when the file system does not provide any
 *
 * openmacfuse's backend builds its node table and NFS file handles from inode numbers (the use_ino semantics
 * of FUSE). Most file systems supply real inode numbers; some (exFAT and disk-image drivers, for example) leave
 * st_ino unset in getattr (always 0), which used to make every LOOKUP return an I/O error and
 * directory listings come back empty.
 *
 * Approach: the backend enables this table when it finds the root directory's st_ino is 0. From then
 * on, whenever getattr yields st_ino 0, the "path as handed to the file system" is looked up in the
 * table and given a new number if absent (counting up from 2; the root is always 1). A path keeps
 * its number for as long as the object exists:
 *   - after a successful remove the path is dropped (for a directory, with every path below it); a new object of the same name gets a new number;
 *   - after a successful rename the old path (for a directory, with every path below it) becomes the new path with the same numbers, so handles already issued stay valid;
 *   - changes made behind our back (another program modifying the volume directly) are not seen; as with FUSE without use_ino, objects are identified by path.
 * Only called by the single service thread while it holds the global lock; no locking here. Memory: one string per path.
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "synth_ino.h"

struct ent {
	char		*path;		/* NULL = empty slot; TOMB = deleted */
	uint64_t	ino;
};

#define TOMB	((char *)1)

struct openmacfuse_synth {
	struct ent	*t;
	size_t		cap;		/* power of 2 */
	size_t		used;		/* includes deleted slots */
	size_t		live;
	uint64_t	next;
};

static uint64_t hash_str(const char *s)
{
	uint64_t h = 1469598103934665603ULL;

	while (*s)
		h = (h ^ (unsigned char)*s++) * 1099511628211ULL;
	return h;
}

struct openmacfuse_synth *openmacfuse_synth_new(void)
{
	struct openmacfuse_synth *s = calloc(1, sizeof(*s));

	if (!s)
		return NULL;
	s->cap = 1024;
	s->t = calloc(s->cap, sizeof(*s->t));
	if (!s->t) {
		free(s);
		return NULL;
	}
	s->next = 2;
	return s;
}

void openmacfuse_synth_free(struct openmacfuse_synth *s)
{
	size_t i;

	if (!s)
		return;
	for (i = 0; i < s->cap; i++)
		if (s->t[i].path && s->t[i].path != TOMB)
			free(s->t[i].path);
	free(s->t);
	free(s);
}

/* Find the slot for path: returns it if found; otherwise returns the first usable slot (*found = 0) */
static struct ent *slot(struct openmacfuse_synth *s, const char *path, int *found)
{
	size_t m = s->cap - 1, i = (size_t)hash_str(path) & m;
	struct ent *free_slot = NULL;

	for (;;) {
		struct ent *e = &s->t[i];

		if (!e->path) {
			*found = 0;
			return free_slot ? free_slot : e;
		}
		if (e->path == TOMB) {
			if (!free_slot)
				free_slot = e;
		} else if (strcmp(e->path, path) == 0) {
			*found = 1;
			return e;
		}
		i = (i + 1) & m;
	}
}

static int grow(struct openmacfuse_synth *s)
{
	struct openmacfuse_synth n = *s;
	size_t i;

	n.cap = s->cap * 2;
	n.t = calloc(n.cap, sizeof(*n.t));
	if (!n.t)
		return -1;
	n.used = n.live = 0;
	for (i = 0; i < s->cap; i++) {
		struct ent *e = &s->t[i];
		int f;

		if (e->path && e->path != TOMB) {
			struct ent *d = slot(&n, e->path, &f);

			*d = *e;
			n.used++;
			n.live++;
		}
	}
	free(s->t);
	*s = n;
	return 0;
}

static int put(struct openmacfuse_synth *s, char *path, uint64_t ino)
{
	struct ent *e;
	int f;

	if ((s->used + 1) * 4 >= s->cap * 3 && grow(s))
		return -1;
	e = slot(s, path, &f);
	if (f) {
		free(e->path);
		e->path = path;
		e->ino = ino;
		return 0;
	}
	if (!e->path)
		s->used++;
	e->path = path;
	e->ino = ino;
	s->live++;
	return 0;
}

uint64_t openmacfuse_synth_get(struct openmacfuse_synth *s, const char *path)
{
	struct ent *e;
	char *dup;
	uint64_t ino;
	int f;

	if (!s || !path)
		return 0;
	e = slot(s, path, &f);
	if (f)
		return e->ino;
	dup = strdup(path);
	if (!dup)
		return 0;
	ino = strcmp(path, "/") == 0 ? 1 : s->next++;
	if (put(s, dup, ino)) {
		free(dup);
		return 0;
	}
	return ino;
}

/* path itself, or a path below it (path + "/...") */
static int under(const char *p, const char *path, size_t pl)
{
	if (strncmp(p, path, pl) != 0)
		return 0;
	if (p[pl] == 0)
		return 1;
	return p[pl] == '/' || (pl == 1 && path[0] == '/');
}

void openmacfuse_synth_drop(struct openmacfuse_synth *s, const char *path, int subtree)
{
	size_t i, pl;

	if (!s || !path)
		return;
	pl = strlen(path);
	for (i = 0; i < s->cap; i++) {
		struct ent *e = &s->t[i];

		if (!e->path || e->path == TOMB)
			continue;
		if (subtree ? under(e->path, path, pl) : strcmp(e->path, path) == 0) {
			if (strcmp(e->path, "/") == 0)
				continue;	/* the root is always kept */
			free(e->path);
			e->path = TOMB;
			s->live--;
		}
	}
}

void openmacfuse_synth_rename(struct openmacfuse_synth *s, const char *from, const char *to)
{
	size_t i, fl, tl, n = 0, k;
	struct { char *path; uint64_t ino; } *mv = NULL;

	if (!s || !from || !to || strcmp(from, "/") == 0)
		return;
	fl = strlen(from);
	tl = strlen(to);
	/* Drop the target path (and everything below it) first: it has been overwritten */
	openmacfuse_synth_drop(s, to, 1);
	/* Detach everything that has to move first, then reinsert under the new paths (reinserting may grow the table, so it cannot be done while scanning) */
	for (i = 0; i < s->cap; i++) {
		struct ent *e = &s->t[i];

		if (!e->path || e->path == TOMB || !under(e->path, from, fl))
			continue;
		if (n % 64 == 0) {
			void *nm = realloc(mv, (n + 64) * sizeof(*mv));

			if (!nm)
				break;
			mv = nm;
		}
		mv[n].path = e->path;
		mv[n].ino = e->ino;
		n++;
		e->path = TOMB;
		s->live--;
	}
	for (k = 0; k < n; k++) {
		size_t rest = strlen(mv[k].path) - fl;
		char *np = malloc(tl + rest + 1);

		if (np) {
			memcpy(np, to, tl);
			memcpy(np + tl, mv[k].path + fl, rest + 1);
			if (put(s, np, mv[k].ino))
				free(np);
		}
		free(mv[k].path);
	}
	free(mv);
}

size_t openmacfuse_synth_count(const struct openmacfuse_synth *s)
{
	return s ? s->live : 0;
}
