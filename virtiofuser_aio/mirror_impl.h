/*
#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

#ifndef VIRTIOFUSER_MIRROR_IMPL_H
#define VIRTIOFUSER_MIRROR_IMPL_H

#include "fuse_ll.h"

enum fuser_rw_cb_op {
    FUSER_RW_CB_WRITE = 0,
    FUSER_RW_CB_READ = 1
};

struct fuser_rw_cb_data {
    enum fuser_rw_cb_op op;
    struct snap_fs_dev_io_done_ctx *cb;
    struct fuse_in_header *in_hdr;
    struct fuse_out_header *out_hdr;
    union {
        struct {
            struct fuse_write_in *in_write;
            struct iov *in_iov;
            struct fuse_write_out *out_write;
        } write;
        struct {
            struct fuse_read_in *in_read;
            struct iov *out_iov;
        } read;
    } rw;
};



void fuser_mirror_assign_ops(struct fuse_ll_operations *);

#endif // VIRTIOFUSER_MIRROR_IMPL_H
