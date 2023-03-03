#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#

if HAVE_SNAP

bin_PROGRAMS = dpfs_template

dpfs_template_LDADD = $(srcdir)/../dpfs_hal/libdpfs_hal.a \
                $(srcdir)/../../src/libmlx_dev_emu.a $(srcdir)/../../src/libmlx_dev_emu_snap.a \
                $(IBVERBS_LDFLAGS) $(SNAP_LDFLAGS) $(PYTHON3_LDFLAGS) -lpthread

dpfs_template_CFLAGS  = $(BASE_CFLAGS) -I$(srcdir)/../../src \
                -I$(srcdir)/../dpfs_hal \
                -I$(srcdir) $(SNAP_CFLAGS) \
                -I/usr/local/include
dpfs_template_SOURCES = main.c

endif
