/*
#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

#ifndef NFS_V4_H
#define NFS_V4_H

#include <linux/fuse.h>
#include <nfsc/libnfs-raw-nfs4.h>

int nfs4_clone_fh(nfs_fh4 *dst, nfs_fh4 *src);
int nfs4_find_op(struct nfs_context *nfs, COMPOUND4res *res, int op);
int nfs4_op_getattr(struct nfs_context *nfs, nfs_argop4 *op,
    uint32_t *attributes, int count);
int nfs4_op_lookup(struct nfs_context *nfs, nfs_argop4 *op, const char *path);

uint64_t nfs_hton64(uint64_t val);
uint64_t nfs_ntoh64(uint64_t val);
uint64_t nfs_pntoh64(const uint32_t *buf);
int nfs_get_ugid(struct nfs_context *nfs, const char *buf, int slen, int is_user);
int nfs_parse_attributes(struct nfs_context *nfs, struct fuse_attr *attr,
    const char *buf, int len);
int32_t nfs_error_to_fuse_error(nfsstat4 status);

#endif // NFS_V4_H
