#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#

if HAVE_SNAP

lib_LTLIBRARIES = libdpfs_fuse.la

libdpfs_fuse_adir = $(includedir)/
include_HEADERS = dpfs_fuse.h

libdpfs_fuse_la_LIBADD = $(srcdir)/../dpfs_hal/libdpfs_hal.a \
	$(srcdir)/../../src/libmlx_dev_emu.a $(srcdir)/../../src/libmlx_dev_emu_snap.a \
	$(srcdir)/../eRPC-arm/build/liberpc.a \
	$(IBVERBS_LDFLAGS) $(SNAP_LDFLAGS) $(PYTHON3_LDFLAGS) 

libdpfs_fuse_la_LDFLAGS = -lboost_system -lboost_thread \
	--exclude-libs=libmlx_dev_emu.a --exclude-libs=libmlx_dev_emu_snap.a --exclude-libs=liberpc.a -fvisibility=hidden \
	$(IBVERBS_LDFLAGS) $(SNAP_LDFLAGS) $(PYTHON3_LDFLAGS) 

libdpfs_fuse_la_CPPFLAGS  = $(BASE_CPPFLAGS) \
	-I$(srcdir)/../../src $(SNAP_CFLAGS) \
	-I$(srcdir)/../dpfs_hal \
	-I$(srcdir)/../eRPC-arm/third_party/asio/include \
	-I$(srcdir)/../eRPC-arm/src \
	-DERPC_INFINIBAND -Wno-address-of-packed-member # eRPC required flags for its headers

libdpfs_fuse_la_SOURCES = dpfs_fuse.cpp 

endif
