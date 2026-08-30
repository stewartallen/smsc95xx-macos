# SPDX-License-Identifier: GPL-2.0
CC      = clang
CFLAGS  ?= -O2 -g
override CFLAGS += -std=c11 -Wall -Wextra -Werror
LDFLAGS := -framework IOKit -framework CoreFoundation

PROBE_SRCS := main.c usb_backend.c smsc95xx_ops.c smsc95xx_proto.c
HDRS       := smsc95xx_regs.h smsc95xx_proto.h usb_backend.h smsc95xx_ops.h

TEST_SRCS  := tests/test_proto.c smsc95xx_proto.c

.PHONY: all test clean

all: smsc95xx-probe

smsc95xx-probe: $(PROBE_SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(PROBE_SRCS) $(LDFLAGS)

# Tests link only the pure layer, so they need no frameworks and no hardware.
test-proto: $(TEST_SRCS) $(HDRS) tests/test_harness.h
	$(CC) $(CFLAGS) -o $@ $(TEST_SRCS)

test: test-proto
	./test-proto

clean:
	rm -f smsc95xx-probe test-proto
