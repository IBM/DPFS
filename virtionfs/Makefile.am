#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#

if HAVE_SNAP

bin_PROGRAMS = virtionfs

virtionfs_LDADD = $(srcdir)/../virtiofs_emu_fuse_lowlevel/libvirtiofs_emu_fuse_ll.a \
              $(srcdir)/../virtiofs_emu_lowlevel/libvirtiofs_emu_ll.a \
              $(srcdir)/../../src/libmlx_dev_emu.a $(srcdir)/../../src/libmlx_dev_emu_snap.a \
              -L/usr/local/lib -lnfs \
              $(IBVERBS_LDFLAGS) $(SNAP_LDFLAGS) $(PYTHON3_LDFLAGS) -lpthread

virtionfs_CFLAGS  = $(BASE_CFLAGS) -I$(srcdir)/../../src -I$(srcdir)/../virtiofs_emu_fuse_lowlevel \
                -I$(srcdir)/../virtiofs_emu_lowlevel $(SNAP_CFLAGS) \
                -I/usr/local/include
virtionfs_SOURCES = virtionfs.c helpers.c mpool.c nfs_v4.c main.c

endif
