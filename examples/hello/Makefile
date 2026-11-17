# Copyright 2026 The OpenMacFUSE Authors
# SPDX-License-Identifier: MIT
#
# Build the hello example against the static library in ../../build.

TOP    = ../..
CC    ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra

hello: hello.c $(TOP)/build/libnfuse.a
	$(CC) $(CFLAGS) -I$(TOP)/include -o $@ hello.c $(TOP)/build/libnfuse.a -lpthread -framework CoreFoundation -framework IOKit

$(TOP)/build/libnfuse.a:
	$(MAKE) -C $(TOP) lib

clean:
	rm -f hello

.PHONY: clean
