#
# Copyright 2023- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#

if HAVE_SNAP

bin_PROGRAMS = dpfs_aio

dpfs_aio_LDADD = $(srcdir)/../dpfs_fuse/libdpfs_fuse.la \
	$(srcdir)/../dpfs_hal/libdpfs_hal.la \
	-lck -lpthread

dpfs_aio_CFLAGS = $(BASE_CFLAGS) -I$(srcdir) -I$(srcdir)/../lib \
  -I/usr/local/include \
	-I$(srcdir)/../extern/tomlcpp \
  -I$(srcdir)/../dpfs_fuse -I$(srcdir)/../dpfs_hal/include

dpfs_aio_SOURCES = fuser.c mirror_impl.c main.c \
	../lib/mpool.c \
	../extern/tomlcpp/toml.c

endif
