# SPDX-License-Identifier: GPL-2.0
CFLAGS  := -O2 -g -std=c11 -Wall -Wextra -Werror
LDLIBS  := -framework IOKit -framework CoreFoundation

usb-reenumerate: usb_reenumerate.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

.PHONY: clean
clean:
	rm -f usb-reenumerate
