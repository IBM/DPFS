#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#

if HAVE_SNAP

noinst_LIBRARIES = libdpfs_hal.a

libdpfs_hal_adir = $(includedir)/
libdpfs_hal_a_HEADERS = dpfs_hal.h

libdpfs_hal_a_CFLAGS  = $(BASE_CFLAGS) -I$(srcdir)/../../src $(SNAP_CFLAGS)
libdpfs_hal_a_SOURCES = dpfs_hal.c cpu_latency.c

endif
