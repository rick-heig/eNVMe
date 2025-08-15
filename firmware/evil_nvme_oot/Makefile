# SPDX-License-Identifier: GPL-2.0
#
# Makefile for PCI Endpoint Functions
#

obj-m	+= nvmet-pci-epf.o
nvmet-pci-epf-y	+= pci-epf.o

# Remove the -Werror=date-time flag for this module.
# We want to use __DATE__ and __TIME__ to show when it was built.
# These are not allowed in the Linux kernel normal build because they might
# prevent reproducible builds.
KBUILD_CFLAGS := $(filter-out -Werror=date-time,$(KBUILD_CFLAGS))

# Check architecture to determine if on host or embedded platform
# This expects the host to be different than aarch64...
ifeq ($(shell uname -m),aarch64)
	KDIR ?= /lib/modules/$(shell uname -r)/build
else
	KDIR ?= ../../work/buildroot/output/build/linux-custom
endif

default:
ifeq ($(shell uname -m),aarch64)
	$(MAKE) -C $(KDIR) M=$$PWD
else
	$(MAKE) -C $(KDIR) M=$$PWD ARCH=arm64 CROSS_COMPILE=$(shell realpath ../../work/buildroot/output/host/bin/aarch64-none-linux-gnu-)
endif

install:
ifeq ($(shell uname -m),aarch64)
	cp nvmet-pci-epf.ko /lib/modules/$(shell uname -r)/kernel/drivers/nvme/target/
else
	@echo "Install is only for embedded platform"
endif

clean:
	$(MAKE) -C $(KDIR) M=$$PWD clean