/*
#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# GPL-2.0 WITH Linux-syscall-note
# from https://elixir.bootlin.com/linux/v6.0.12/source/include/uapi/linux/nfs4.h
#
*/

#ifndef NFS_V4_H
#define NFS_V4_H

#include <stdbool.h>
#include <linux/fuse.h>
#include <nfsc/libnfs-raw-nfs4.h>

#define VNFS_BLKSIZE 8192

// Empirically verified with Linux kernel 5.11
#define NFS_ROOT_FILEID 2

// 1MB is max read/write size in Linux, + some overhead
#define NFS4_MAXRESPONSESIZE (1 << 20)
#define NFS4_MAXREQUESTSIZE (1 << 20)

#define NFS4_MAX_OUTSTANDING_REQUESTS 64

#define NFS4DOT1_MINOR 1

/*
 *  linux/include/linux/nfs4.h
 *
 *  NFSv4 protocol definitions.
 *
 *  Copyright (c) 2002 The Regents of the University of Michigan.
 *  All rights reserved.
 *
 *  Kendrick Smith <kmsmith@umich.edu>
 *  Andy Adamson   <andros@umich.edu>
 */

/* An NFS4 sessions server must support at least NFS4_MAX_OPS operations.
 * If a compound requires more operations, adjust NFS4_MAX_OPS accordingly.
 */
#define NFS4_MAX_OPS   8

/* Our NFS4 client back channel server only wants the cb_sequene and the
 * actual operation per compound
 */
#define NFS4_MAX_BACK_CHANNEL_OPS 2

/*
 * End of Linux header
 */

// No malloc needed
struct vnfs_fh4 {
    u_int len;
    char val[NFS4_FHSIZE];
};
typedef struct vnfs_fh4 vnfs_fh4;

int nfs4_clone_fh(vnfs_fh4 *dst, nfs_fh4 *src);
int nfs4_find_op(COMPOUND4res *res, int op);
int nfs4_fill_create_attrs(struct fuse_in_header *in_hdr, uint32_t flags, fattr4 *attr);
bool nfs4_check_session_trunking_allowed(EXCHANGE_ID4resok *l, EXCHANGE_ID4resok *r);
bool nfs4_check_clientid_trunking_allowed(EXCHANGE_ID4resok *l, EXCHANGE_ID4resok *r);
// Supply the clientid received from EXCHANGE_ID
int nfs4_op_createsession(nfs_argop4 *op, clientid4 clientid, sequenceid4 seqid);
int nfs4_op_bindconntosession(nfs_argop4 *op, sessionid4 *sessionid, channel_dir_from_client4 channel, bool rdma);
int nfs4_op_exchangeid(nfs_argop4 *op, verifier4 verifier, const char *client_name);
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
