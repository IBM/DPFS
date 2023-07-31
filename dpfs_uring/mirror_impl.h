/*
#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

#ifndef VIRTIOFUSER_MIRROR_IMPL_H
#define VIRTIOFUSER_MIRROR_IMPL_H

#include "dpfs_fuse.h"
#include <linux/io_uring.h>

// This must be checked before liburing include because
// liburing defines all the ops even if they aren't supported
// by the local kernel
#ifdef IORING_OP_STATX
#define IORING_STATX_SUPPORTED
#endif
#ifdef IORING_OP_OPENAT
#define IORING_OPENAT_SUPPORTED
#endif
#ifdef IORING_OP_FALLOCATE
#define IORING_FALLOCATE_SUPPORTED
#endif
#ifdef IORING_OP_RENAMEAT
#define IORING_RENAMEAT_SUPPORTED
#endif
#ifdef IORING_OP_CLOSE
#define IORING_CLOSE_SUPPORTED
#endif
#ifdef IORING_OP_UNLINKAT
#define IORING_UNLINKAT_SUPPORTED
#endif

#include <liburing.h>
#include <linux/stat.h>

struct fuser_cb_data;
typedef void (*fuser_uring_cb) (struct fuser_cb_data *, struct io_uring_cqe *);

struct fuser_cb_data {
    uint16_t thread_id;
    fuser_uring_cb cb;
    struct fuser *f;
    struct fuse_session *se;

    struct fuse_in_header *in_hdr;
    struct fuse_out_header *out_hdr;
    union {
        struct {
            struct fuse_write_out *out_write;
        } write;
#ifdef IORING_STATX_SUPPORTED
        struct {
            struct statx s;
            struct fuse_attr_out *out_attr;
        } getattr;
#endif
#ifdef IORING_OPENAT_SUPPORTED
        struct {
            struct inode *i;
            struct fuse_file_info fi;
            struct fuse_open_out *out_open;
        } open;
        struct {
            struct fuse_file_info fi;
            struct fuse_entry_out *out_entry;
            struct fuse_open_out *out_open;
            const char *in_name;
        } create;
#endif
    };
    void *completion_context;
};



void fuser_mirror_assign_ops(struct fuse_ll_operations *);

#endif // VIRTIOFUSER_MIRROR_IMPL_H
