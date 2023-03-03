#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#

if HAVE_SNAP

bin_PROGRAMS = virtiofs_ramcloud

virtiofs_ramcloud_LDADD = $(srcdir)/../dpfs_fuse/libdpfs_fuse.a \
                $(srcdir)/../dpfs_hal/libdpfs_hal.a \
                $(srcdir)/../../src/libmlx_dev_emu.a $(srcdir)/../../src/libmlx_dev_emu_snap.a \
                $(IBVERBS_LDFLAGS) $(SNAP_LDFLAGS) $(PYTHON3_LDFLAGS) -lpthread \
                -L/home/ubuntu/RAMCloud/obj.c-api -lramcloud

virtiofs_ramcloud_CPPFLAGS  = $(BASE_CFLAGS) -I$(srcdir)/../../src \
                -I$(srcdir)/../dpfs_fuse \
                -I$(srcdir)/../dpfs_hal \
                -I/usr/local/include \ 
	        -I/home/ubuntu/RAMCloud/obj.c-api \
		-I/home/ubuntu/RAMCloud/src

virtiofs_ramcloud_SOURCES = main.cpp

endif
