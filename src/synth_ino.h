/*
 * synth_ino.h - assigns stable inode numbers by path when the file system does not provide any (see synth_ino.c)
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#ifndef NFUSE_SYNTH_INO_H
#define NFUSE_SYNTH_INO_H

#include <stddef.h>
#include <stdint.h>

struct nfuse_synth;

struct nfuse_synth *nfuse_synth_new(void);
void nfuse_synth_free(struct nfuse_synth *s);
/* Inode number for a path, allocated if there is none yet ("/" is always 1); returns 0 on out of memory */
uint64_t nfuse_synth_get(struct nfuse_synth *s, const char *path);
/* Remove a path; if subtree is non-zero, everything below path as well */
void nfuse_synth_drop(struct nfuse_synth *s, const char *path, int subtree);
/* Rename: from (and everything below it) becomes to, keeping its numbers; whatever to had (and everything below it) is dropped first */
void nfuse_synth_rename(struct nfuse_synth *s, const char *from, const char *to);
size_t nfuse_synth_count(const struct nfuse_synth *s);

#endif /* NFUSE_SYNTH_INO_H */
