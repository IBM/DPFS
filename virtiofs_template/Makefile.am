#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#

if HAVE_SNAP

bin_PROGRAMS = virtiofs_template

virtiofs_template_LDADD = $(srcdir)/../virtiofs_emu_lowlevel/libvirtiofs_emu_ll.a \
                $(srcdir)/../../src/libmlx_dev_emu.a $(srcdir)/../../src/libmlx_dev_emu_snap.a \
                $(IBVERBS_LDFLAGS) $(SNAP_LDFLAGS) $(PYTHON3_LDFLAGS) -lpthread

virtiofs_template_CFLAGS  = $(BASE_CFLAGS) -I$(srcdir)/../../src \
                -I$(srcdir)/../virtiofs_emu_lowlevel \
                -I$(srcdir) $(SNAP_CFLAGS) \
                -I/usr/local/include
virtiofs_template_SOURCES = main.c

endif
