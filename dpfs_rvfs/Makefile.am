#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#

if HAVE_SNAP

bin_PROGRAMS = dpfs_rvfs_dpu

dpfs_rvfs_dpu_LDADD = $(srcdir)/../dpfs_hal/libdpfs_hal.la \
	$(srcdir)/../../src/libmlx_dev_emu.a $(srcdir)/../../src/libmlx_dev_emu_snap.a \
	$(srcdir)/../extern/eRPC-arm/build/liberpc.a \
	-lboost_system -lboost_thread  -lnuma\
	$(IBVERBS_LDFLAGS) $(SNAP_LDFLAGS) $(PYTHON3_LDFLAGS)

dpfs_rvfs_dpu_CPPFLAGS  = $(BASE_CPPFLAGS) \
	-I$(srcdir)/../../src $(SNAP_CFLAGS) \
	-I$(srcdir)/../dpfs_hal/include \
	-I$(srcdir)/../extern/eRPC-arm/third_party/asio/include \
	-I$(srcdir)/../extern/eRPC-arm/src \
	-I$(srcdir)/../extern/tomlcpp \
	-DERPC_INFINIBAND -Wno-address-of-packed-member # eRPC required flags for its headers

dpfs_rvfs_dpu_SOURCES = dpu.cpp ../extern/tomlcpp/toml.c ../extern/tomlcpp/tomlcpp.cpp

endif
