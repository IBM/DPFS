#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#

if HAVE_SNAP

lib_LTLIBRARIES = libdpfs_fuse.la

libdpfs_fuse_adir = $(includedir)/
include_HEADERS = dpfs_fuse.h

libdpfs_fuse_la_LIBADD = $(srcdir)/../dpfs_hal/libdpfs_hal.la \
	$(srcdir)/../extern/eRPC-arm/build/liberpc.a
	 
libdpfs_fuse_la_LDFLAGS = -lboost_system -lboost_thread \
	--exclude-libs=liberpc.a -fvisibility=hidden

libdpfs_fuse_la_CPPFLAGS  = $(BASE_CPPFLAGS) \
	-I$(srcdir)/../../src $(SNAP_CFLAGS) \
	-I$(srcdir)/../dpfs_hal/include \
	-I$(srcdir)/../extern/eRPC-arm/third_party/asio/include \
	-I$(srcdir)/../extern/eRPC-arm/src \
	-DERPC_INFINIBAND -Wno-address-of-packed-member # eRPC required flags for its headers

libdpfs_fuse_la_SOURCES = dpfs_fuse.cpp 

endif
