#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#

if HAVE_SNAP

bin_PROGRAMS = list_emulation_managers

list_emulation_managers_LDADD = $(srcdir)/../../src/libmlx_dev_emu_snap.a \
                $(srcdir)/../../src/libmlx_dev_emu.a \
                $(IBVERBS_LDFLAGS) $(SNAP_LDFLAGS) $(PYTHON3_LDFLAGS) -lpthread

list_emulation_managers_CFLAGS  = $(BASE_CFLAGS) -I$(srcdir)/../../src $(SNAP_CFLAGS)
list_emulation_managers_SOURCES = main.c

endif
