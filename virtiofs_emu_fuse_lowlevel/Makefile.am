if HAVE_SNAP

noinst_LIBRARIES = libvirtiofs_emu_fuse_ll.a

libvirtiofs_emu_fuse_ll_adir = $(includedir)/
libvirtiofs_emu_fuse_ll_a_HEADERS = fuse_ll.h

libvirtiofs_emu_fuse_ll_a_LIBADD = $(srcdir)/../virtiofs_emu_lowlevel/libvirtiofs_emu_ll.a

libvirtiofs_emu_fuse_ll_a_CFLAGS  = $(BASE_CFLAGS) -I$(srcdir)/../../src -I$(srcdir)/../virtiofs_emu_lowlevel $(SNAP_CFLAGS)
libvirtiofs_emu_fuse_ll_a_SOURCES = fuse_ll.c

endif
