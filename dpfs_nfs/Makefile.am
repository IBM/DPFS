#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#

if HAVE_SNAP

bin_PROGRAMS = dpfs_nfs

dpfs_nfs_LDADD = $(srcdir)/../dpfs_fuse/libdpfs_fuse.la \
	$(srcdir)/../dpfs_hal/libdpfs_hal.la \
	-L/usr/local/lib -lnfs -lck -lpthread

dpfs_nfs_CFLAGS = $(BASE_CFLAGS) -I$(srcdir) -I$(srcdir)/../lib \
                   -I/usr/local/include \
	-I$(srcdir)/../extern/tomlcpp \
                   -I$(srcdir)/../dpfs_fuse -I$(srcdir)/../dpfs_hal/include

dpfs_nfs_SOURCES = main.c \
                   dpfs_nfs.c vnfs_connect.c \
                   nfs_v4.c inode.c \
                   ../lib/mpool.c ../lib/ftimer.c \
	../extern/tomlcpp/toml.c

endif
