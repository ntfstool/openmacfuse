# OpenMacFUSE (libnfuse): in-process NFSv4.0 server + FUSE 2.x compatible library for macOS
#
# Copyright 2026 The OpenMacFUSE Authors
# SPDX-License-Identifier: MIT
#
#   make            build the read-only prototype server build/nfuse-m0 and the library build/libnfuse.a (native architecture)
#   make universal  same, as an arm64 + x86_64 universal build
#   make lib        build only the library (what an outer build script would call)
#   make clean
#
# Overridable: ARCHFLAGS (e.g. "-arch arm64 -arch x86_64"), MINVER (default 10.15)
# Only the system compiler and libraries are used; no third-party code.

CC       ?= cc
MINVER   ?= 10.15
ARCHFLAGS ?=
CFLAGS   ?= -O2 -g
# Mandatory compiler options are kept separate so they survive CFLAGS being overridden on the command line (as an outer build script may do)
ALL_CFLAGS = $(CFLAGS) -std=c11 -D_DARWIN_C_SOURCE -Wall -Wextra -Wno-unused-parameter \
             -mmacosx-version-min=$(MINVER) $(ARCHFLAGS) -Iinclude -Isrc
ALL_LDFLAGS = $(LDFLAGS) -mmacosx-version-min=$(MINVER) $(ARCHFLAGS) -lpthread -framework CoreFoundation

CORE_SRCS = src/xdr.c src/rpc.c src/nfs4_state.c src/nfs4_attr.c src/nfs4_ops.c src/watchdog.c \
            src/server.c src/names.c src/appledouble.c src/adfile.c
LIB_SRCS  = $(CORE_SRCS) src/backend_fuse.c src/fuse_lib.c src/fuse_opt.c src/mounter.c src/report.c src/synth_ino.c \
            src/fuse_opt_parse.c src/fuse_main.c src/devwatch.c
M0_SRCS   = $(CORE_SRCS) src/backend_mem.c src/main.c
HDRS      = $(wildcard src/*.h) $(wildcard include/*.h)

BUILD = build
OBJDIR = $(BUILD)/obj
LIB_OBJS = $(patsubst src/%.c,$(OBJDIR)/%.o,$(LIB_SRCS))

all: $(BUILD)/nfuse-m0 $(BUILD)/libnfuse.a

lib: $(BUILD)/libnfuse.a

$(OBJDIR)/%.o: src/%.c $(HDRS) Makefile
	@mkdir -p $(OBJDIR)
	$(CC) $(ALL_CFLAGS) -c -o $@ $<

$(BUILD)/libnfuse.a: $(LIB_OBJS)
	@rm -f $@
	libtool -static -o $@ $(LIB_OBJS)

$(BUILD)/nfuse-m0: $(M0_SRCS) $(HDRS) Makefile
	@mkdir -p $(BUILD)
	$(CC) $(ALL_CFLAGS) -o $@ $(M0_SRCS) $(ALL_LDFLAGS)

universal:
	$(MAKE) clean
	$(MAKE) ARCHFLAGS="-arch arm64 -arch x86_64" all

# Unit tests (run on the development machine, nothing is mounted): fuse_opt_parse, the fuse_main command line / backgrounding flow (with the mount steps faked)
# Programs that use fuse_main also need IOKit (the device watcher reads I/O statistics): -framework IOKit -framework CoreFoundation
TEST_LDFLAGS = $(ALL_LDFLAGS) -framework IOKit
UNIT_TESTS = $(BUILD)/unit_fuse_opt $(BUILD)/unit_fuse_main $(BUILD)/unit_fuse_lib $(BUILD)/unit_fork_cf

$(BUILD)/unit_%: tests/unit_%.c $(BUILD)/libnfuse.a $(HDRS)
	$(CC) $(ALL_CFLAGS) -o $@ $< $(BUILD)/libnfuse.a $(TEST_LDFLAGS)

test: $(UNIT_TESTS)
	@for t in $(UNIT_TESTS); do echo "== $$t"; $$t || exit 1; done

clean:
	rm -rf $(BUILD)

.PHONY: all lib universal clean test
