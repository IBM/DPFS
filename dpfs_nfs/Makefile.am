#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#

if HAVE_SNAP

bin_PROGRAMS = virtionfs

virtionfs_LDADD = $(srcdir)/../dpfs_fuse/libdpfs_fuse.a \
              $(srcdir)/../dpfs_hal/libdpfs_hal.a \
              $(srcdir)/../../src/libmlx_dev_emu.a $(srcdir)/../../src/libmlx_dev_emu_snap.a \
              -L/usr/local/lib -lnfs -lck \
              $(IBVERBS_LDFLAGS) $(SNAP_LDFLAGS) $(PYTHON3_LDFLAGS) -lpthread

virtionfs_CFLAGS  = $(BASE_CFLAGS) -I$(srcdir)/../../src -I$(srcdir)/../dpfs_fuse \
                -I$(srcdir) \
                -I$(srcdir)/../dpfs_hal $(SNAP_CFLAGS) \
                -I/usr/local/include
virtionfs_SOURCES = main.c \
                    dpfs_nfs.c vnfs_connect.c \
                    mpool2.c nfs_v4.c inode.c ftimer.c

endif
