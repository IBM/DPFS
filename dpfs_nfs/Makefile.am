#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#

if HAVE_SNAP

bin_PROGRAMS = dpfs_nfs

dpfs_nfs_LDADD = $(srcdir)/../dpfs_fuse/libdpfs_fuse.la \
	-L/usr/local/lib -lnfs -lck -lpthread

dpfs_nfs_CFLAGS = $(BASE_CFLAGS) -I$(srcdir) \
                   -I/usr/local/include \
                   -I$(srcdir)/../dpfs_fuse -I$(srcdir)/../dpfs_hal

dpfs_nfs_SOURCES = main.c \
                   dpfs_nfs.c vnfs_connect.c \
                   mpool2.c nfs_v4.c inode.c ftimer.c

endif
