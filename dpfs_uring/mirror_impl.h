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
#ifndef IORING_DISABLE_METADATA
        struct {
            struct statx s;
            struct fuse_attr_out *out_attr;
        } getattr;
        struct {
            struct inode *i;
            struct fuse_file_info fi;
            struct fuse_open_out *out_open;
        } open;
        struct {
            struct fuse_file_info fi;
            const char *in_name;
            struct fuse_entry_out *out_entry;
            struct fuse_open_out *out_open;
        } create;
        struct {
            const char *in_name;
            struct fuse_entry_out *out_entry;
        } mk;
#endif
    };
    void *completion_context;
};



void fuser_mirror_assign_ops(struct fuse_ll_operations *);

#endif // VIRTIOFUSER_MIRROR_IMPL_H
