#
# Copyright 2023- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#

if HAVE_SNAP

bin_PROGRAMS = dpfs_kv

dpfs_kv_LDADD = $(srcdir)/../dpfs_fuse/libdpfs_fuse.la \
	$(srcdir)/../dpfs_hal/libdpfs_hal.la \
                -L$(srcdir)/../extern/RAMCloud/obj.c-api -lramcloud -lpthread

dpfs_kv_CPPFLAGS = $(BASE_CPPFLAGS) \
                -I$(srcdir)/../dpfs_fuse \
                -I$(srcdir)/../dpfs_hal/include \
                -I$(srcdir)/../extern/tomlcpp \
                -I/usr/local/include \
                -I$(srcdir)/../extern/RAMCloud/obj.c-api \
                -I$(srcdir)/../extern/RAMCloud/src

dpfs_kv_SOURCES = main.cpp

endif
