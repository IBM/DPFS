/*
#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

#ifndef VIRTIOFUSER_MIRROR_IMPL_H
#define VIRTIOFUSER_MIRROR_IMPL_H

#include "dpfs_fuse.h"
#include <liburing.h>
#include <linux/stat.h>

enum fuser_rw_cb_op {
    FUSER_RW_CB_WRITE = 0,
    FUSER_RW_CB_READ,
    FUSER_RW_CB_FSYNC,
};

struct fuser_cb_data;
typedef void (*fuser_uring_cb) (struct fuser_cb_data *, struct io_uring_cqe *);

struct fuser_cb_data {
    fuser_uring_cb cb;
    struct fuser *f;
    struct fuse_session *se;

    struct fuse_in_header *in_hdr;
    struct fuse_out_header *out_hdr;
    union {
        struct {
            struct fuse_write_out *out_write;
        } write;
        struct {
            struct statx s;
            struct fuse_attr_out *out_attr;
        } getattr;
    };
    void *completion_context;
};



void fuser_mirror_assign_ops(struct fuse_ll_operations *);

#endif // VIRTIOFUSER_MIRROR_IMPL_H
