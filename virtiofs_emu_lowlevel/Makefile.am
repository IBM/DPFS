#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#

if HAVE_SNAP

lib_LIBRARIES = libvirtiofs_emu_ll.a

libvirtiofs_emu_ll_adir = $(includedir)/
libvirtiofs_emu_ll_a_HEADERS = virtiofs_emu_ll.h

libvirtiofs_emu_ll_a_CFLAGS  = $(BASE_CFLAGS) -I$(srcdir)/../../src $(SNAP_CFLAGS)
libvirtiofs_emu_ll_a_SOURCES = virtiofs_emu_ll.c cpu_latency.c

endif
