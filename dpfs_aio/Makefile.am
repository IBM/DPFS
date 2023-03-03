#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#

if HAVE_SNAP

bin_PROGRAMS = dpfs_aio

dpfs_aio_LDADD = $(srcdir)/../dpfs_fuse/libdpfs_fuse.a \
        $(srcdir)/../dpfs_hal/libdpfs_hal.a \
        $(srcdir)/../../src/libmlx_dev_emu.a $(srcdir)/../../src/libmlx_dev_emu_snap.a \
        $(IBVERBS_LDFLAGS) $(SNAP_LDFLAGS) $(PYTHON3_LDFLAGS) -lpthread

dpfs_aio_CFLAGS  = $(BASE_CFLAGS) -I$(srcdir)/../../src -I$(srcdir)/../dpfs_fuse -I$(srcdir)/../dpfs_hal $(SNAP_CFLAGS)
dpfs_aio_SOURCES = fuser.c mpool.c mirror_impl.c main.c

endif
