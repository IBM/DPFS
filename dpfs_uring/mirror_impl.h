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
#define IORING_METADATA_SUPPORTED
#endif

#include <liburing.h>
#include <linux/stat.h>

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
#ifdef IORING_METADATA_SUPPORTED
        struct {
            struct statx s;
            struct fuse_attr_out *out_attr;
        } getattr;
#endif
    };
    void *completion_context;
};



void fuser_mirror_assign_ops(struct fuse_ll_operations *);

#endif // VIRTIOFUSER_MIRROR_IMPL_H
