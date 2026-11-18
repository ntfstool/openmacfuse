/*
 * names.h - file name normalisation (NFC) and case folding, used to look up "equivalent names"
 *
 * Two kinds of equivalence:
 *   - Unicode normalisation: the precomposed form (NFC, usual on Windows / Linux) and the
 *     decomposed form (NFD, produced by some Mac applications) of the same name. These are
 *     treated as equivalent in every mode;
 *   - case: equivalent only on case-insensitive mounts. The fold function can be registered
 *     by the backend (NTFS, for example, uses the volume's $UpCase table); the default is
 *     Unicode simple uppercase mapping, one character at a time.
 * Comparison key = fold(NFC(name)) (NFC only when case-sensitive).
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#ifndef OPENMACFUSE_NAMES_H
#define OPENMACFUSE_NAMES_H

#include <stddef.h>
#include <stdint.h>

#define OPENMACFUSE_KEY_MAX	1024	/* comparison key buffer (names are at most 255 bytes but may grow when normalised / folded) */

typedef int (*openmacfuse_fold_fn)(const char *in, char *out, size_t outsz);

int openmacfuse_name_is_ascii(const char *s, size_t n);
/* NFC; returns the output length, or -1 on failure (the caller then uses the name as is). ASCII is copied straight through */
int openmacfuse_name_nfc(const char *in, size_t inlen, char *out, size_t outsz);
/* Default fold: simple uppercase mapping per code point (towupper in the system UTF-8 locale), with a fast path for ASCII */
int openmacfuse_name_fold_default(const char *in, char *out, size_t outsz);
/* Comparison key; returns its length (falls back to the original name on failure) */
int openmacfuse_name_key(const char *name, size_t nl, int ci, openmacfuse_fold_fn fold,
		   char *out, size_t outsz);
uint64_t openmacfuse_hash_bytes(const void *p, size_t n);
/* Call once before fork (initialises CoreFoundation's classes, see names.c) */
void openmacfuse_names_warmup(void);
/* Call before a fork without exec: every CF type the child will use (names + the device watchdog's IOKit results) is initialised in the parent first */
void openmacfuse_fork_warmup(void);

#endif /* OPENMACFUSE_NAMES_H */
