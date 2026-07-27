# SPDX-License-Identifier: GPL-2.0-only
#
# Makefile for the FUSE filesystem.
#

# Uncomment below line to compile with debug option
# ccflags-y += -DDEBUG

obj-m += fuse.o

fuse-y := dev.o dir.o file.o inode.o control.o xattr.o acl.o readdir.o ioctl.o
fuse-$(CONFIG_FUSE_DAX) += dax.o

.PHONY: all clean

all:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(CURDIR) modules

clean:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(CURDIR) clean
