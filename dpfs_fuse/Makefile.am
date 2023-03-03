if HAVE_SNAP

noinst_LIBRARIES = libdpfs_fuse.a

libdpfs_fuse_adir = $(includedir)/
libdpfs_fuse_a_HEADERS = dpfs_fuse.h

libdpfs_fuse_a_LIBADD = $(srcdir)/../dpfs_hal/libdpfs_hal.a

libdpfs_fuse_a_CFLAGS  = $(BASE_CFLAGS) -I$(srcdir)/../../src -I$(srcdir)/../dpfs_hal $(SNAP_CFLAGS)
libdpfs_fuse_a_SOURCES = dpfs_fuse.c

endif
