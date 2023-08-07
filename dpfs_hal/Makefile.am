#
# Copyright 2023- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#

lib_LTLIBRARIES = libdpfs_hal.la

libdpfs_hal_ladir = $(includedir)/
libdpfs_hal_la_HEADERS = include/dpfs/hal.h

libdpfs_hal_la_CFLAGS = -I$(builddir)/include/dpfs \
	-fPIC -fvisibility=hidden

libdpfs_hal_la_SOURCES = $(builddir)/../lib/cpu_latency.c \
	$(builddir)/../extern/tomlcpp/toml.c

libdpfs_hal_la_LDFLAGS = $(IBVERBS_LDFLAGS)

if !DPFS_RVFS
if HAVE_SNAP

libdpfs_hal_la_LIBADD = $(srcdir)/../../src/libmlx_dev_emu.a \
	$(srcdir)/../../src/libmlx_dev_emu_snap.a \
	$(IBVERBS_LDFLAGS) $(SNAP_LDFLAGS) $(PYTHON3_LDFLAGS)

libdpfs_hal_la_LDFLAGS += --exclude-libs=libmlx_dev_emu.a \
	--exclude-libs=libmlx_dev_emu_snap.a \
	$(SNAP_LDFLAGS) $(PYTHON3_LDFLAGS) 

# Mellanox Snap BlueField virtio-fs emulation
libdpfs_hal_la_CFLAGS += $(BASE_CFLAGS) \
	$(SNAP_CFLAGS) -I$(builddir)/../../src \
	-I$(builddir)/../extern/tomlcpp \
	-I$(srcdir)/../lib

libdpfs_hal_la_SOURCES += src/snap.c

endif HAVE_SNAP
else DPFS_RVFS
# RVFS using eRPC

libdpfs_hal_la_LIBADD = $(srcdir)/../extern/eRPC-arm/build/liberpc.a \
	-lboost_system -lboost_thread -lnuma $(IBVERBS_LDFLAGS) 

# For the common RVFS definitions
libdpfs_hal_la_CPPFLAGS = -I$(builddir)/../dpfs_rvfs
libdpfs_hal_la_CPPFLAGS += -I$(builddir)/../lib
# eRPC required flags for its headers
libdpfs_hal_la_CPPFLAGS += $(BASE_CPPFLAGS) $(libdpfs_hal_la_CFLAGS) \
	-I$(builddir)/../extern/eRPC-arm/third_party/asio/include \
	-I$(builddir)/../extern/eRPC-arm/src \
	-DERPC_INFINIBAND -Wno-address-of-packed-member
# toml library
libdpfs_hal_la_CPPFLAGS += -I$(builddir)/../extern/tomlcpp

libdpfs_hal_la_SOURCES += src/rvfs.cpp \
	$(builddir)/../extern/tomlcpp/tomlcpp.cpp

endif DPFS_RVFS
