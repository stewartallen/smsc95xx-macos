# SPDX-License-Identifier: GPL-2.0
CC      = clang
CFLAGS  ?= -O2 -g
override CFLAGS += -std=c11 -Wall -Wextra -Werror -I../../common
LDFLAGS := -framework IOKit -framework CoreFoundation

COMMON_SRCS := ../../common/smsc95xx_proto.c ../../common/smsc95xx_init.c

PROBE_SRCS := main.c usb_backend.c smsc95xx_ops.c $(COMMON_SRCS)
HDRS       := ../../common/smsc95xx_regs.h ../../common/smsc95xx_proto.h \
              ../../common/smsc95xx_init.h usb_backend.h smsc95xx_ops.h

# The init sequence is in common/ too, and it is callback-driven with no I/O of its
# own, so the tests can link it and drive it from a mock -- no frameworks, no hardware.
TEST_SRCS  := tests/test_proto.c $(COMMON_SRCS)

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
