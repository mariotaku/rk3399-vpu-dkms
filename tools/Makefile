# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Mariotaku <git@mariotaku.me>
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra

h264enc-test: h264enc-test.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f h264enc-test

.PHONY: clean
