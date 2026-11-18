/*
 * synth_ino.h - assigns stable inode numbers by path when the file system does not provide any (see synth_ino.c)
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#ifndef OPENMACFUSE_SYNTH_INO_H
#define OPENMACFUSE_SYNTH_INO_H

#include <stddef.h>
#include <stdint.h>

struct openmacfuse_synth;

struct openmacfuse_synth *openmacfuse_synth_new(void);
void openmacfuse_synth_free(struct openmacfuse_synth *s);
/* Inode number for a path, allocated if there is none yet ("/" is always 1); returns 0 on out of memory */
uint64_t openmacfuse_synth_get(struct openmacfuse_synth *s, const char *path);
/* Remove a path; if subtree is non-zero, everything below path as well */
void openmacfuse_synth_drop(struct openmacfuse_synth *s, const char *path, int subtree);
/* Rename: from (and everything below it) becomes to, keeping its numbers; whatever to had (and everything below it) is dropped first */
void openmacfuse_synth_rename(struct openmacfuse_synth *s, const char *from, const char *to);
size_t openmacfuse_synth_count(const struct openmacfuse_synth *s);

#endif /* OPENMACFUSE_SYNTH_INO_H */
