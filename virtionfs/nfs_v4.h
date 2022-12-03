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

#define VNFS_BLKSIZE 8192

// Empirically verified with Linux kernel 5.11
#define NFS_ROOT_FILEID 2

// No malloc needed
struct vnfs_fh4 {
    u_int len;
    char val[NFS4_FHSIZE];
};
typedef struct vnfs_fh4 vnfs_fh4;

int nfs4_clone_fh(vnfs_fh4 *dst, nfs_fh4 *src);
int nfs4_find_op(COMPOUND4res *res, int op);
int nfs4_fill_create_attrs(struct fuse_in_header *in_hdr, uint32_t flags, fattr4 *attr);
int nfs4_op_setclientid(nfs_argop4 *op, verifier4 verifier, const char *client_name);
int nfs4_op_setclientid_confirm(struct nfs_argop4 *op, uint64_t clientid, verifier4 verifier);
int nfs4_op_getattr(nfs_argop4 *op, uint32_t *attributes, int count);
int nfs4_op_lookup(nfs_argop4 *op, const char *path);

uint64_t nfs_hton64(uint64_t val);
uint64_t nfs_ntoh64(uint64_t val);
uint64_t nfs_pntoh64(const uint32_t *buf);
int nfs_parse_attributes(struct fuse_attr *attr, const char *buf, int len);
int nfs_parse_statfs(struct fuse_kstatfs *stat, const char *buf, int len);
int nfs_parse_fileid(uint64_t *fileid, const char *buf, int len);
int32_t nfs_error_to_fuse_error(nfsstat4 status);

#endif // NFS_V4_H
