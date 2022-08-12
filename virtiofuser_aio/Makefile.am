#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#

if HAVE_SNAP

bin_PROGRAMS = virtiofuser_aio

virtiofuser_aio_LDADD = $(srcdir)/../virtiofs_emu_fuse_lowlevel/libvirtiofs_emu_fuse_ll.a \
              $(srcdir)/../virtiofs_emu_lowlevel/libvirtiofs_emu_ll.a \
              $(srcdir)/../../src/libmlx_dev_emu.a $(srcdir)/../../src/libmlx_dev_emu_snap.a \
              $(IBVERBS_LDFLAGS) $(SNAP_LDFLAGS) $(PYTHON3_LDFLAGS) -lpthread

virtiofuser_aio_CFLAGS  = $(BASE_CFLAGS) -I$(srcdir)/../../src -I$(srcdir)/../virtiofs_emu_fuse_lowlevel -I$(srcdir)/../virtiofs_emu_lowlevel $(SNAP_CFLAGS)
virtiofuser_aio_SOURCES = fuser.c mpool.c mirror_impl.c main.c

endif
