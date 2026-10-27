# SPDX-License-Identifier: GPL-2.0
#
# Out-of-tree wrapper; the kbuild rules are in Kbuild. DKMS calls this
# with KERNELRELEASE and KDIR set; a manual build defaults to the
# running kernel.

KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build

all: modules

modules clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) $@

.PHONY: all modules clean
