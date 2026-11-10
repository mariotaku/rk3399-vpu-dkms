# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Mariotaku <git@mariotaku.me>
#
# Builds neatvnc 0.9.1's V4L2 M2M backend with v4l2m2m-fixes.patch into a
# small harness. NEATVNC_SRC points at an unpacked neatvnc-0.9.1 source
# tree (apt-get source neatvnc) with the patch applied.

NEATVNC_SRC ?= neatvnc-0.9.1
CC ?= gcc
CFLAGS ?= -O2 -Wall
PKGS = gbm libdrm

neatvnc-harness: harness.c $(NEATVNC_SRC)/src/enc/h264/v4l2m2m-impl.c
	$(CC) $(CFLAGS) -Istub $(shell pkg-config --cflags $(PKGS)) -o $@ $^ \
		$(shell pkg-config --libs $(PKGS))

clean:
	rm -f neatvnc-harness

.PHONY: clean
