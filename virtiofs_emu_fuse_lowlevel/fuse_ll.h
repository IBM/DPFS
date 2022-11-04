/*
#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

#ifndef VIRTIOFS_EMU_FUSE_LOWLEVEL_FUSE_LL_H
#define VIRTIOFS_EMU_FUSE_LOWLEVEL_FUSE_LL_H

#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/uio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include "virtiofs_emu_ll.h"

// Beginning of libfuse/include/fuse_lowlevel.h selective copy

#define FUSE_USE_VERSION 30
#include "fuse_common.h"

/** The node ID of the root inode */
#define FUSE_ROOT_ID 1

/** Inode number type */
typedef uint64_t fuse_ino_t;

// There is currently a race condition here where the init and destroy could be called concurrently and break
struct fuse_session {
    //char *mountpoint;
    //volatile int exited;
    //int fd;
    //struct mount_opts *mo;
    //int debug; // already present in fuser
    //int deny_others;
    int got_init;
    //void *userdata;
    // uid_t owner; doesn't make sense with virtiofs?
    struct fuse_conn_info conn;
    // pthread_mutex_t lock; // Only used for multithreaded req handling in libfuse
    int got_destroy;
    //pthread_key_t pipe_key;
    //int broken_splice_nonblock;
    size_t bufsize;
    int error;
};

#define FUSE_MAX_MAX_PAGES 256
#define FUSE_DEFAULT_MAX_PAGES_PER_REQ 32

/* room needed in buffer to accommodate header */
#define FUSE_BUFFER_HEADER_SIZE 0x1000

/** Directory entry parameters supplied to fuse_reply_entry() */
struct fuse_entry_param {
    /** Unique inode number
     *
     * In lookup, zero means negative entry (from version 2.5)
     * Returning ENOENT also means negative entry, but by setting zero
     * ino the kernel may cache negative entries for entry_timeout
     * seconds.
     */
    fuse_ino_t ino;

    /** Generation number for this entry.
     *
     * If the file system will be exported over NFS, the
     * ino/generation pairs need to be unique over the file
     * system's lifetime (rather than just the mount time). So if
     * the file system reuses an inode after it has been deleted,
     * it must assign a new, previously unused generation number
     * to the inode at the same time.
     *
     */
    uint64_t generation;

    /** Inode attributes.
     *
     * Even if attr_timeout == 0, attr must be correct. For example,
     * for open(), FUSE uses attr.st_size from lookup() to determine
     * how many bytes to request. If this value is not correct,
     * incorrect data will be returned.
     */
    struct stat attr;

    /** Validity timeout (in seconds) for inode attributes. If
        attributes only change as a result of requests that come
        through the kernel, this should be set to a very large
        value. */
    double attr_timeout;

    /** Validity timeout (in seconds) for the name. If directory
        entries are changed/deleted only as a result of requests
        that come through the kernel, this should be set to a very
        large value. */
    double entry_timeout;
};

/* 'to_set' flags in setattr */
#define FUSE_SET_ATTR_MODE	(1 << 0)
#define FUSE_SET_ATTR_UID	(1 << 1)
#define FUSE_SET_ATTR_GID	(1 << 2)
#define FUSE_SET_ATTR_SIZE	(1 << 3)
#define FUSE_SET_ATTR_ATIME	(1 << 4)
#define FUSE_SET_ATTR_MTIME	(1 << 5)
#define FUSE_SET_ATTR_ATIME_NOW	(1 << 7)
#define FUSE_SET_ATTR_MTIME_NOW	(1 << 8)
#define FUSE_SET_ATTR_CTIME	(1 << 10)

// End of libfuse/include/fuse_lowlevel.h selective copy

struct iov {
    struct iovec *iovec;
    int iovcnt;
    int iov_idx;
    int buf_idx;
    size_t bytes_unused;
};
void iov_init(struct iov *, struct iovec *, int);
size_t iov_write_buf(struct iov*, void *, size_t);

unsigned int calc_timeout_nsec(double);
unsigned long calc_timeout_sec(double);

int fuse_ll_reply_attr(struct fuse_session *se, struct fuse_out_header *, struct fuse_attr_out *, struct stat *, double attr_timeout);
int fuse_ll_reply_entry(struct fuse_session *se, struct fuse_out_header *, struct fuse_entry_out *, struct fuse_entry_param *);
int fuse_ll_reply_open(struct fuse_session *se, struct fuse_out_header *out_hdr, struct fuse_open_out *out_open, struct fuse_file_info *fi);
int fuse_ll_reply_create(struct fuse_session *se, struct fuse_out_header *out_hdr,
    struct fuse_entry_out *out_entry, struct fuse_open_out *out_open,
    const struct fuse_entry_param *e, const struct fuse_file_info *fi);

int fuse_ll_reply_statfs(struct fuse_session *se, struct fuse_out_header *out_hdr,
    struct fuse_statfs_out *out_statfs, const struct statvfs *stbuf);

size_t fuse_add_direntry(struct iov *read_iov, const char *name,
                  const struct stat *stbuf, off_t off);
size_t fuse_add_direntry_plus(struct iov *read_iov, const char *name,
                  const struct fuse_entry_param *e, off_t off);

struct fuse_ll_operations {
    int (*init) (struct fuse_session *, void *user_data,
                 struct fuse_in_header *, struct fuse_init_in *,
                 struct fuse_conn_info *, struct fuse_out_header *,
                 struct snap_fs_dev_io_done_ctx *cb);
    int (*destroy) (struct fuse_session *, void *user_data,
                    struct fuse_in_header *,
                    struct fuse_out_header *,
                    struct snap_fs_dev_io_done_ctx *cb);
    // Reply with fuse_ll_reply_entry()
    int (*lookup) (struct fuse_session *, void *user_data,
                   struct fuse_in_header *, const char *const in_name,
                   struct fuse_out_header *, struct fuse_entry_out *out_entry,
                   struct snap_fs_dev_io_done_ctx *cb);

    int (*setattr) (struct fuse_session *, void *user_data,
                    struct fuse_in_header *in_hdr, struct stat *s, int valid, struct fuse_file_info *fi,
                    struct fuse_out_header *out_hdr, struct fuse_attr_out *out_attr,
                    struct snap_fs_dev_io_done_ctx *cb);
    int (*create) (struct fuse_session *, void *user_data,
                   struct fuse_in_header *in_hdr, struct fuse_create_in *in_create, const char *in_name,
                   struct fuse_out_header *out_hdr, struct fuse_entry_out *out_entry, struct fuse_open_out *out_open,
                   struct snap_fs_dev_io_done_ctx *cb);
    int (*flush) (struct fuse_session *, void *user_data,
                   struct fuse_in_header *, struct fuse_file_info *fi,
                   struct fuse_out_header *,
                   struct snap_fs_dev_io_done_ctx *cb);
    int (*flock) (struct fuse_session *, void *user_data,
                  struct fuse_in_header *, struct fuse_file_info *fi, int op,
                  struct fuse_out_header *,
                  struct snap_fs_dev_io_done_ctx *cb);
    // Reply with fuse_ll_reply_attr()
    int (*getattr) (struct fuse_session *, void *user_data,
                      struct fuse_in_header *, struct fuse_getattr_in *,
                      struct fuse_out_header *, struct fuse_attr_out *,
                    struct snap_fs_dev_io_done_ctx *cb);
    // Reply with fuse_ll_reply_open()
    int (*opendir) (struct fuse_session *, void *user_data,
                    struct fuse_in_header *, struct fuse_open_in *,
                    struct fuse_out_header *, struct fuse_open_out *,
                    struct snap_fs_dev_io_done_ctx *cb);
    int (*releasedir) (struct fuse_session *, void *user_data,
                       struct fuse_in_header *, struct fuse_release_in *,
                       struct fuse_out_header *,
                       struct snap_fs_dev_io_done_ctx *cb);
    int (*readdir) (struct fuse_session *, void *user_data,
                    struct fuse_in_header *, struct fuse_read_in *, bool plus,
                    struct fuse_out_header *, struct iov *,
                    struct snap_fs_dev_io_done_ctx *cb);
    // Reply with fuse_ll_reply_open()
    int (*open) (struct fuse_session *, void *user_data,
                 struct fuse_in_header *, struct fuse_open_in *,
                 struct fuse_out_header *, struct fuse_open_out *,
                 struct snap_fs_dev_io_done_ctx *cb);
    int (*release) (struct fuse_session *, void *user_data,
                    struct fuse_in_header *, struct fuse_release_in *,
                    struct fuse_out_header *,
                    struct snap_fs_dev_io_done_ctx *cb);
    int (*fsync) (struct fuse_session *, void *user_data,
                  struct fuse_in_header *, struct fuse_fsync_in *,
                  struct fuse_out_header *,
                  struct snap_fs_dev_io_done_ctx *cb);
    int (*fsyncdir) (struct fuse_session *, void *user_data,
                     struct fuse_in_header *, struct fuse_fsync_in *,
                     struct fuse_out_header *,
                     struct snap_fs_dev_io_done_ctx *cb);
    int (*rmdir) (struct fuse_session *, void *user_data,
                  struct fuse_in_header *, const char *const in_name,
                  struct fuse_out_header *,
                  struct snap_fs_dev_io_done_ctx *cb);
    int (*forget) (struct fuse_session *, void *user_data,
                   struct fuse_in_header *, struct fuse_forget_in *in_forget,
                   struct snap_fs_dev_io_done_ctx *cb);
    int (*batch_forget) (struct fuse_session *, void *user_data,
                         struct fuse_in_header *, struct fuse_batch_forget_in *in_batch_forget,
                         struct fuse_forget_one *in_forget_one,
                   struct snap_fs_dev_io_done_ctx *cb);
    int (*rename) (struct fuse_session *, void *user_data,
                   struct fuse_in_header *, const char *const in_name,
                   fuse_ino_t in_new_parentdir, const char *const in_new_name, uint32_t in_flags,
                   struct fuse_out_header *,
                   struct snap_fs_dev_io_done_ctx *cb);
    int (*read) (struct fuse_session *, void *user_data,
                 struct fuse_in_header *, struct fuse_read_in *,
                 struct fuse_out_header *, struct iov *,
                 struct snap_fs_dev_io_done_ctx *cb);
    int (*write) (struct fuse_session *, void *user_data,
                  struct fuse_in_header *, struct fuse_write_in *, struct iov *,
                  struct fuse_out_header *, struct fuse_write_out *,
                  struct snap_fs_dev_io_done_ctx *cb);
    int (*mknod) (struct fuse_session *, void *user_data,
                  struct fuse_in_header *, struct fuse_mknod_in *, const char *const,
                  struct fuse_out_header *, struct fuse_entry_out *,
                  struct snap_fs_dev_io_done_ctx *cb);
    int (*mkdir) (struct fuse_session *, void *user_data,
                  struct fuse_in_header *, struct fuse_mkdir_in *, const char *const,
                  struct fuse_out_header *, struct fuse_entry_out *,
                  struct snap_fs_dev_io_done_ctx *cb);
    int (*symlink) (struct fuse_session *, void *user_data,
                    struct fuse_in_header *, const char *const in_name,
                    const char *const in_link_name,
                    struct fuse_out_header *, struct fuse_entry_out *,
                    struct snap_fs_dev_io_done_ctx *cb);
    int (*statfs) (struct fuse_session *, void *user_data,
                   struct fuse_in_header *,
                   struct fuse_out_header *, struct fuse_statfs_out *,
                   struct snap_fs_dev_io_done_ctx *cb);
    int (*unlink) (struct fuse_session *, void *user_data,
                   struct fuse_in_header *, const char *const,
                   struct fuse_out_header *,
                   struct snap_fs_dev_io_done_ctx *cb);
    int (*fallocate) (struct fuse_session *, void *user_data,
                        struct fuse_in_header *, struct fuse_fallocate_in *,
                      struct fuse_out_header *,
                      struct snap_fs_dev_io_done_ctx *cb);
};

struct fuse_ll {
    void *user_data;
    struct fuse_ll_operations ops;
    struct fuse_session *se;
    bool debug;
};

int virtiofs_emu_fuse_ll_main(struct fuse_ll_operations *ops, struct virtiofs_emu_params *emu_params,
                              void *user_data, bool debug);

#endif // VIRTIOFS_EMU_FUSE_LOWLEVEL_FUSE_LL_H
