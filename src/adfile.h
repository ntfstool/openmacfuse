/*
 * adfile.h - virtual `._X`: the client keeps its default nonamedattr (extended attributes stored in
 * AppleDouble files named `._X`), and the server, instead of letting them become real files on disk,
 * synthesises / parses them on the fly from X's extended attributes in the backend:
 *
 *   - LOOKUP `._X`: X exists and has extended attributes (or is being written) -> a virtual file (handle type ADFILE, ino = X);
 *     X has no attributes at all -> does not exist (if X does not exist it is handled as an ordinary file: real orphan `._` files work as before);
 *   - READ / GETATTR: synthesise the AppleDouble from X's attributes (same layout the client writes itself); the resource
 *     fork part is read from the backend by offset, never loaded whole;
 *   - CREATE / WRITE / SETATTR(size): keep the complete contents of this one file in memory (a session); on COMMIT / CLOSE /
 *     a stable write, parse the AppleDouble and write the differences back to X's backend attributes (unchanged values are not written, missing ones are removed);
 *   - REMOVE `._X`: remove all of X's attributes; RENAME `._A` -> `._B`: move the attributes from A to B;
 *   - READDIR never lists a virtual `._X`; real `._X` files on existing volumes are merged into X's attribute view by the backend (see backend_fuse.c).
 * Nothing here depends on a particular file system: where attributes are stored is up to the backend's xattr callbacks.
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#ifndef OPENMACFUSE_ADFILE_H
#define OPENMACFUSE_ADFILE_H

#include <stdint.h>

#include "backend.h"

struct openmacfuse_server;

/* If the name is `._X` (X non-empty) return X, otherwise NULL */
const char *openmacfuse_adf_target(const char *name);
/*
 * name (of the form `._X`) under dir: if X exists, returns 0 with *x set to X's inode and *exists telling
 * whether the virtual `._X` currently "exists"; if the name is not `._X` or X does not exist, returns
 * -ENOENT (the caller then handles it as an ordinary file).
 */
int  openmacfuse_adf_lookup(struct openmacfuse_server *s, openmacfuse_ino_t dir, const char *name,
		      openmacfuse_ino_t *x, int *exists);
/* Attributes of the virtual file: st comes in holding X's attributes and goes out holding those of `._X` (fileid, type, size, change) */
int  openmacfuse_adf_attr(struct openmacfuse_server *s, openmacfuse_ino_t x, struct openmacfuse_attr *st);
int  openmacfuse_adf_read(struct openmacfuse_server *s, openmacfuse_ino_t x, void *buf, uint32_t count,
		    uint64_t off);
int  openmacfuse_adf_write(struct openmacfuse_server *s, openmacfuse_ino_t x, const void *buf, uint32_t len,
		     uint64_t off);
int  openmacfuse_adf_truncate(struct openmacfuse_server *s, openmacfuse_ino_t x, uint64_t size);
/* The client creates `._X`: open a session (contents cleared if trunc is non-zero) */
int  openmacfuse_adf_create(struct openmacfuse_server *s, openmacfuse_ino_t x, int trunc);
/* Parse the session's contents and write them back to the backend (does nothing if unmodified) */
int  openmacfuse_adf_apply(struct openmacfuse_server *s, openmacfuse_ino_t x);
/* Remove all of X's extended attributes (REMOVE `._X`) */
int  openmacfuse_adf_remove(struct openmacfuse_server *s, openmacfuse_ino_t x);
/* Move all of A's attributes to B (RENAME `._A` -> `._B`), leaving A with none */
int  openmacfuse_adf_move(struct openmacfuse_server *s, openmacfuse_ino_t a, openmacfuse_ino_t b);
/*
 * Rename alias (observed on macOS 27, absent on macOS 12): right after renaming A to B, the client creates
 * `._A` under A's old name, writes an AppleDouble carrying com.apple.provenance into it, then renames
 * `._A` to `._B`. By then A is gone, so treating it as an "orphan `._`" would create a real file on disk
 * (one test run left 209 such `._` files on the volume this way).
 * So after an ordinary name is renamed successfully we record that "A under dir is an alias of B for a few
 * seconds": while A does not exist, `._A` is handled as B's virtual `._B` (the same handle), and the
 * rename `._A` -> `._B` is then the same file onto itself, a no-op; nothing is created on disk.
 * The alias only applies while A does not exist and B is still where it was; it expires after a few seconds and is dropped once A is created again.
 */
void openmacfuse_adf_renamed(struct openmacfuse_server *s, openmacfuse_ino_t odir, const char *oname,
		       openmacfuse_ino_t ndir, const char *nname, openmacfuse_ino_t y);
/* Is name (of the form `._A`) under dir a rename alias: returns 1 if so, with *y set to the current file */
int  openmacfuse_adf_alias(struct openmacfuse_server *s, openmacfuse_ino_t dir, const char *name, openmacfuse_ino_t *y);
void openmacfuse_adf_alias_drop(struct openmacfuse_server *s, openmacfuse_ino_t dir, const char *name);
/* Copy A's attributes to B (A is unchanged) */
int  openmacfuse_adf_copy(struct openmacfuse_server *s, openmacfuse_ino_t a, openmacfuse_ino_t b);
/* X itself was removed: discard the session */
void openmacfuse_adf_forget(struct openmacfuse_server *s, openmacfuse_ino_t x);
/* Before unmount: write back every session */
void openmacfuse_adf_flush_all(struct openmacfuse_server *s);
void openmacfuse_adf_free_all(struct openmacfuse_server *s);
/* fileid of a virtual `._X` (kept apart from real inode numbers) */
#define OPENMACFUSE_ADF_FILEID(x)	(0x5000000000000000ull | ((x) & 0x0fffffffffffffffull))

#endif /* OPENMACFUSE_ADFILE_H */
