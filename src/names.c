/*
 * names.c - NFC normalisation, case folding and comparison keys for file names
 *
 * NFC uses the system's CoreFoundation (CFStringNormalize); case folding defaults to towupper_l
 * in the system UTF-8 locale (simple uppercase mapping per code point). Both are only called
 * for names containing non-ASCII characters; pure ASCII names take a fast path.
 *
 * Copyright 2026 The OpenMacFUSE Authors
 * SPDX-License-Identifier: MIT
 */
#include <locale.h>
#include <pthread.h>
#include <string.h>
#include <wctype.h>
#include <xlocale.h>

#include <CoreFoundation/CoreFoundation.h>

#include "names.h"

int openmacfuse_name_is_ascii(const char *s, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		if ((unsigned char)s[i] >= 0x80)
			return 0;
	return 1;
}

int openmacfuse_name_nfc(const char *in, size_t inlen, char *out, size_t outsz)
{
	CFStringRef s;
	CFMutableStringRef m;
	CFIndex used = 0, len;
	int ret = -1;

	if (openmacfuse_name_is_ascii(in, inlen)) {
		if (inlen + 1 > outsz)
			return -1;
		memcpy(out, in, inlen);
		out[inlen] = 0;
		return (int)inlen;
	}
	s = CFStringCreateWithBytes(kCFAllocatorDefault, (const UInt8 *)in,
				    (CFIndex)inlen, kCFStringEncodingUTF8, false);
	if (!s)
		return -1;		/* not valid UTF-8: use as is */
	m = CFStringCreateMutableCopy(kCFAllocatorDefault, 0, s);
	CFRelease(s);
	if (!m)
		return -1;
	CFStringNormalize(m, kCFStringNormalizationFormC);
	len = CFStringGetLength(m);
	if (CFStringGetBytes(m, CFRangeMake(0, len), kCFStringEncodingUTF8, 0, false,
			     (UInt8 *)out, (CFIndex)outsz - 1, &used) == len &&
	    (size_t)used < outsz) {
		out[used] = 0;
		ret = (int)used;
	}
	CFRelease(m);
	return ret;
}

static locale_t g_utf8;
static pthread_once_t g_utf8_once = PTHREAD_ONCE_INIT;

static void utf8_locale_init(void)
{
	g_utf8 = newlocale(LC_CTYPE_MASK, "UTF-8", NULL);
	if (!g_utf8)
		g_utf8 = newlocale(LC_CTYPE_MASK, "en_US.UTF-8", NULL);
}

/* Decode one UTF-8 code point; returns -1 for an invalid sequence (the caller treats it as a single byte) */
static int utf8_dec(const unsigned char *p, size_t n, uint32_t *cp)
{
	if (p[0] < 0x80) {
		*cp = p[0];
		return 1;
	}
	if ((p[0] & 0xe0) == 0xc0 && n >= 2 && (p[1] & 0xc0) == 0x80) {
		*cp = ((uint32_t)(p[0] & 0x1f) << 6) | (p[1] & 0x3f);
		return *cp >= 0x80 ? 2 : -1;
	}
	if ((p[0] & 0xf0) == 0xe0 && n >= 3 && (p[1] & 0xc0) == 0x80 &&
	    (p[2] & 0xc0) == 0x80) {
		*cp = ((uint32_t)(p[0] & 0x0f) << 12) | ((uint32_t)(p[1] & 0x3f) << 6) |
		      (p[2] & 0x3f);
		return *cp >= 0x800 ? 3 : -1;
	}
	if ((p[0] & 0xf8) == 0xf0 && n >= 4 && (p[1] & 0xc0) == 0x80 &&
	    (p[2] & 0xc0) == 0x80 && (p[3] & 0xc0) == 0x80) {
		*cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3f) << 12) |
		      ((uint32_t)(p[2] & 0x3f) << 6) | (p[3] & 0x3f);
		return (*cp >= 0x10000 && *cp <= 0x10ffff) ? 4 : -1;
	}
	return -1;
}

static int utf8_enc(uint32_t cp, char *o)
{
	if (cp < 0x80) {
		o[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		o[0] = (char)(0xc0 | (cp >> 6));
		o[1] = (char)(0x80 | (cp & 0x3f));
		return 2;
	}
	if (cp < 0x10000) {
		o[0] = (char)(0xe0 | (cp >> 12));
		o[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
		o[2] = (char)(0x80 | (cp & 0x3f));
		return 3;
	}
	o[0] = (char)(0xf0 | (cp >> 18));
	o[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
	o[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
	o[3] = (char)(0x80 | (cp & 0x3f));
	return 4;
}

int openmacfuse_name_fold_default(const char *in, char *out, size_t outsz)
{
	const unsigned char *p = (const unsigned char *)in;
	size_t n = strlen(in), i = 0, o = 0;

	pthread_once(&g_utf8_once, utf8_locale_init);
	while (i < n) {
		uint32_t cp;
		int k = utf8_dec(p + i, n - i, &cp);

		if (o + 5 > outsz)
			return -1;
		if (k < 0) {
			out[o++] = (char)p[i++];
			continue;
		}
		i += (size_t)k;
		if (cp < 0x80)
			cp = (cp >= 'a' && cp <= 'z') ? cp - 32 : cp;
		else if (g_utf8)
			cp = (uint32_t)towupper_l((wint_t)cp, g_utf8);
		o += (size_t)utf8_enc(cp, out + o);
	}
	out[o] = 0;
	return (int)o;
}

int openmacfuse_name_key(const char *name, size_t nl, int ci, openmacfuse_fold_fn fold,
		   char *out, size_t outsz)
{
	char tmp[OPENMACFUSE_KEY_MAX];
	int n;

	n = openmacfuse_name_nfc(name, nl, tmp, sizeof(tmp));
	if (n < 0) {
		if (nl + 1 > sizeof(tmp))
			nl = sizeof(tmp) - 1;
		memcpy(tmp, name, nl);
		tmp[nl] = 0;
		n = (int)nl;
	}
	if (!ci) {
		if ((size_t)n + 1 > outsz)
			return -1;
		memcpy(out, tmp, (size_t)n + 1);
		return n;
	}
	if (openmacfuse_name_is_ascii(tmp, (size_t)n)) {
		int i;

		if ((size_t)n + 1 > outsz)
			return -1;
		for (i = 0; i < n; i++)
			out[i] = (tmp[i] >= 'a' && tmp[i] <= 'z') ? tmp[i] - 32 : tmp[i];
		out[n] = 0;
		return n;
	}
	n = (fold ? fold : openmacfuse_name_fold_default)(tmp, out, outsz);
	if (n < 0)
		n = openmacfuse_name_fold_default(tmp, out, outsz);
	return n;
}

/*
 * Initialise CoreFoundation's string classes before forking: FUSE file systems often call
 * daemon() (fork without exec) after fuse_new, and the objc runtime aborts outright when a
 * class is initialised for the first time in the child (observed: SIGABRT the first time the
 * driver did NFC in the child, with performForkChildInitialize on the stack).
 */
void openmacfuse_names_warmup(void)
{
	char out[64];

	(void)openmacfuse_name_nfc("e\xcc\x81\xe1\x84\x80\xe1\x85\xa1", 9, out, sizeof(out));
	(void)openmacfuse_name_fold_default("\xc3\xa9", out, sizeof(out));
}

/*
 * Call before a fork without exec: create one of each CoreFoundation type the child will use after
 * the fork, in the parent, so that the objc classes behind them finish +initialize in the parent.
 *
 * Why (observed on macOS 27, does not happen on macOS 12): once a process has ever started a thread
 * (even one already joined; pthread_is_threaded_np stays true), the objc runtime aborts outright the
 * first time a class with +initialize is initialised in a forked child
 * ("+[NSMutableString initialize] may have been in progress in another thread when fork() was
 * called"). fuse2fs reads the bitmaps with multiple threads before calling fuse_main (on ext2 / ext3
 * without flex_bg and with more than one block group); fuse_main then forks and does its first NFC
 * in the child -> the child crashes and the mount fails.
 * What the child uses: name normalisation / folding (CFString, CFMutableString) and the device
 * watchdog's IOKit queries (IOBSDNameMatching builds a mutable dictionary; registry properties come
 * back as dictionary / number / string / data / array / boolean).
 * Only plain CF interfaces are used here: no IOKit calls (no touching mach ports before fork) and no threads.
 */
void openmacfuse_fork_warmup(void)
{
	static const UInt8 bytes[4] = { 1, 2, 3, 4 };
	long long n = 1;
	CFStringRef k, v;
	CFMutableStringRef ms;
	CFNumberRef num;
	CFDataRef data;
	CFArrayRef arr;
	CFMutableArrayRef marr;
	CFDictionaryRef dict;
	CFMutableDictionaryRef mdict;
	CFSetRef set;
	const void *keys[1], *vals[1];

	openmacfuse_names_warmup();
	k = CFStringCreateWithCString(kCFAllocatorDefault, "BSD Name", kCFStringEncodingUTF8);
	v = CFStringCreateWithBytes(kCFAllocatorDefault, (const UInt8 *)"disk", 4,
				    kCFStringEncodingUTF8, false);
	num = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &n);
	data = CFDataCreate(kCFAllocatorDefault, bytes, sizeof(bytes));
	if (k && v && num && data) {
		keys[0] = k;
		vals[0] = num;
		dict = CFDictionaryCreate(kCFAllocatorDefault, keys, vals, 1,
					  &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		mdict = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
						  &kCFTypeDictionaryValueCallBacks);
		if (mdict) {
			CFDictionarySetValue(mdict, k, v);
			CFDictionarySetValue(mdict, v, kCFBooleanTrue);
			CFDictionarySetValue(mdict, num, data);
			(void)CFDictionaryGetValue(mdict, k);
			CFRelease(mdict);
		}
		if (dict)
			CFRelease(dict);
		vals[0] = v;
		arr = CFArrayCreate(kCFAllocatorDefault, vals, 1, &kCFTypeArrayCallBacks);
		if (arr)
			CFRelease(arr);
		marr = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
		if (marr) {
			CFArrayAppendValue(marr, num);
			CFRelease(marr);
		}
		set = CFSetCreate(kCFAllocatorDefault, vals, 1, &kCFTypeSetCallBacks);
		if (set)
			CFRelease(set);
		ms = CFStringCreateMutable(kCFAllocatorDefault, 0);
		if (ms) {
			CFStringAppend(ms, v);
			CFRelease(ms);
		}
		(void)CFNumberGetValue(num, kCFNumberSInt64Type, &n);
		/*
		 * Compile-time constant strings (CFSTR; all IOKit key names are) are objc objects: CFRetain or
		 * putting one into a dictionary sends it a message, and the first message has to initialise
		 * its class (observed: the first IOBSDNameMatching in the child crashed in
		 * +[__NSCFConstantString initialize])
		 */
		{
			CFStringRef cs = CFSTR("Statistics");
			char buf[16];

			CFRetain(cs);
			(void)CFStringGetLength(cs);
			(void)CFHash(cs);
			(void)CFEqual(cs, k);
			(void)CFStringGetCString(cs, buf, sizeof(buf), kCFStringEncodingUTF8);
			mdict = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
							  &kCFTypeDictionaryValueCallBacks);
			if (mdict) {
				CFDictionarySetValue(mdict, cs, CFSTR("IOMedia"));
				(void)CFDictionaryGetValue(mdict, CFSTR("Statistics"));
				CFRelease(mdict);
			}
			CFRelease(cs);
		}
	}
	if (k)
		CFRelease(k);
	if (v)
		CFRelease(v);
	if (num)
		CFRelease(num);
	if (data)
		CFRelease(data);
}

uint64_t openmacfuse_hash_bytes(const void *p, size_t n)
{
	const unsigned char *s = p;
	uint64_t h = 0xcbf29ce484222325ull;
	size_t i;

	for (i = 0; i < n; i++) {
		h ^= s[i];
		h *= 0x100000001b3ull;
	}
	h ^= h >> 33;
	h *= 0xff51afd7ed558ccdull;
	h ^= h >> 33;
	return h ? h : 1;
}
