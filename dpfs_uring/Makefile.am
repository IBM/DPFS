#
# Copyright 2023- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#

if HAVE_SNAP

bin_PROGRAMS = dpfs_uring

dpfs_uring_LDADD = $(srcdir)/../dpfs_fuse/libdpfs_fuse.la \
	$(srcdir)/../dpfs_hal/libdpfs_hal.la \
	-lck -lpthread -luring

dpfs_uring_CFLAGS = $(BASE_CFLAGS) -I$(srcdir) -I$(srcdir)/../lib \
  -I/usr/local/include \
	-I$(srcdir)/../extern/tomlcpp \
  -I$(srcdir)/../dpfs_fuse -I$(srcdir)/../dpfs_hal/include

dpfs_uring_SOURCES = fuser.c mirror_impl.c main.c \
	../lib/mpool.c \
	../extern/tomlcpp/toml.c

endif
