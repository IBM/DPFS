/*
#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

#include <sys/time.h>
#include <nfsc/libnfs.h>
#include <nfsc/libnfs-raw.h>
#include <nfsc/libnfs-raw-nfs.h>
#include <nfsc/libnfs-raw-nfs4.h>
#include <linux/fuse.h>
#include <poll.h>
#include <err.h>

#include "virtionfs.h"
#include "mpool.h"
#include "helpers.h"
#include "nfs_v4.h"
#include "fuse_ll.h"


static uint32_t supported_attrs_attributes[1] = {
    (1 << FATTR4_SUPPORTED_ATTRS)
};
static uint32_t standard_attributes[2] = {
    (1 << FATTR4_TYPE |
     1 << FATTR4_SIZE |
     1 << FATTR4_FILEID),
    (1 << (FATTR4_MODE - 32) |
     1 << (FATTR4_NUMLINKS - 32) |
     1 << (FATTR4_OWNER - 32) |
     1 << (FATTR4_OWNER_GROUP - 32) |
     1 << (FATTR4_SPACE_USED - 32) |
     1 << (FATTR4_TIME_ACCESS - 32) |
     1 << (FATTR4_TIME_METADATA - 32) |
     1 << (FATTR4_TIME_MODIFY - 32))
};

//struct lookup_cb_data {
//    struct snap_fs_dev_io_done_ctx *cb;
//    struct virtionfs *vnfs;
//    struct fuse_out_header *out_hdr;
//    struct fuse_entry_out *out_entry;
//};
//
//void setattr_cb(struct rpc_context *rpc, int status, void *data,
//                       void *private_data) {
//    struct lookup_cb_data *cb_data = (struct lookup_cb_data *)private_data;
//    struct virtionfs *vnfs = cb_data->vnfs;
//    COMPOUND4res *res = data;
//
//    if (status != RPC_STATUS_SUCCESS || res->status != NFS4_OK) {
//    	fprintf(stderr, "Failed to perform GETATTR\n");
//        cb_data->out_hdr->error = -EREMOTEIO;
//        goto ret;
//    }
//
//ret:
//    cb_data->cb->cb(SNAP_FS_DEV_OP_SUCCESS, cb_data->cb->user_arg);
//}
//
//int setattr(struct fuse_session *se, struct virtionfs *vnfs,
//            struct fuse_in_header *in_hdr, struct stat *s, int valid, struct fuse_file_info *fi,
//            struct fuse_out_header *out_hdr, struct fuse_attr_out *out_attr,
//            struct snap_fs_dev_io_done_ctx *cb)
//{
//    struct lookup_cb_data *cb_data = mpool_alloc(vnfs->p);
//    if (!cb_data) {
//        out_hdr->error = -ENOMEM;
//        return 0;
//    }
//
//    cb_data->cb = cb;
//    cb_data->vnfs = vnfs;
//    cb_data->out_hdr = out_hdr;
//    cb_data->out_entry = out_entry;
//
//    COMPOUND4args args;
//    nfs_argop4 op[3];
//    nfs4_op_lookup(vnfs->nfs, &op[0], in_name);
//    nfs4_op_getattr(vnfs->nfs, &op[1], standard_attributes, 2);
//    op[2].argop = OP_GETFH;
//
//    memset(&args, 0, sizeof(args));
//    args.argarray.argarray_len = sizeof(op) / sizeof(nfs_argop4);
//    args.argarray.argarray_val = op;
//
//    if (rpc_nfs4_compound_async(vnfs->rpc, lookup_cb, &args, cb_data) != 0) {
//    	fprintf(stderr, "Failed to send nfs4 GETATTR request\n");
//        out_hdr->error = -EREMOTEIO;
//        return 0;
//    }
//
//    return EWOULDBLOCK;
//}

struct lookup_cb_data {
    struct snap_fs_dev_io_done_ctx *cb;
    struct virtionfs *vnfs;
    struct fuse_out_header *out_hdr;
    struct fuse_entry_out *out_entry;
};

void lookup_cb(struct rpc_context *rpc, int status, void *data,
                       void *private_data) {
    struct lookup_cb_data *cb_data = (struct lookup_cb_data *)private_data;
    struct virtionfs *vnfs = cb_data->vnfs;
    COMPOUND4res *res = data;

    if (status != RPC_STATUS_SUCCESS || res->status != NFS4_OK) {
    	fprintf(stderr, "Failed to perform GETATTR\n");
        cb_data->out_hdr->error = -EREMOTEIO;
        goto ret;
    }

    char *attrs = res->resarray.resarray_val[1].nfs_resop4_u.opgetattr.GETATTR4res_u.resok4.obj_attributes.attr_vals.attrlist4_val;
    u_int attrs_len = res->resarray.resarray_val[1].nfs_resop4_u.opgetattr.GETATTR4res_u.resok4.obj_attributes.attr_vals.attrlist4_len;
    int ret = nfs_parse_attributes(vnfs->nfs, &cb_data->out_entry->attr, attrs, attrs_len);
    if (ret != 0) {
        cb_data->out_hdr->error = -EREMOTEIO;
        goto ret;
    }
    // Finish the attr
    cb_data->out_entry->attr_valid = 0;
    cb_data->out_entry->attr_valid_nsec = 0;
    cb_data->out_entry->entry_valid = 0;
    cb_data->out_entry->entry_valid_nsec = 0;

    // Get the NFS:FH and return it as the FUSE:nodeid
    assert(res->resarray.resarray_val[2].nfs_resop4_u.opgetfh.GETFH4res_u.resok4.object.nfs_fh4_len == sizeof(uint64_t));
    cb_data->out_entry->nodeid = *((uint64_t *) res->resarray.resarray_val[2].nfs_resop4_u.opgetfh.GETFH4res_u.resok4.object.nfs_fh4_val);

ret:
    cb_data->cb->cb(SNAP_FS_DEV_OP_SUCCESS, cb_data->cb->user_arg);
}

int lookup(struct fuse_session *se, struct virtionfs *vnfs,
                        struct fuse_in_header *in_hdr, char *in_name,
                        struct fuse_out_header *out_hdr, struct fuse_entry_out *out_entry,
                        struct snap_fs_dev_io_done_ctx *cb)
{
    struct lookup_cb_data *cb_data = mpool_alloc(vnfs->p);
    if (!cb_data) {
        out_hdr->error = -ENOMEM;
        return 0;
    }

    cb_data->cb = cb;
    cb_data->vnfs = vnfs;
    cb_data->out_hdr = out_hdr;
    cb_data->out_entry = out_entry;

    COMPOUND4args args;
    nfs_argop4 op[3];
    nfs4_op_lookup(vnfs->nfs, &op[0], in_name);
    nfs4_op_getattr(vnfs->nfs, &op[1], standard_attributes, 2);
    op[2].argop = OP_GETFH;

    memset(&args, 0, sizeof(args));
    args.argarray.argarray_len = sizeof(op) / sizeof(nfs_argop4);
    args.argarray.argarray_val = op;

    if (rpc_nfs4_compound_async(vnfs->rpc, lookup_cb, &args, cb_data) != 0) {
    	fprintf(stderr, "Failed to send nfs4 GETATTR request\n");
        out_hdr->error = -EREMOTEIO;
        return 0;
    }

    return EWOULDBLOCK;
}

struct getattr_cb_data {
    struct snap_fs_dev_io_done_ctx *cb;
    struct virtionfs *vnfs;
    struct fuse_out_header *out_hdr;
    struct fuse_attr_out *out_attr;
};

void getattr_cb(struct rpc_context *rpc, int status, void *data,
                       void *private_data) {
    struct getattr_cb_data *cb_data = (struct getattr_cb_data *)private_data;
    struct virtionfs *vnfs = cb_data->vnfs;
    COMPOUND4res *res = data;

    if (status != RPC_STATUS_SUCCESS || res->status != NFS4_OK) {
    	fprintf(stderr, "Failed to perform GETATTR: rpc_status=%d, nfs_status=%d\n", status, res->status);
        cb_data->out_hdr->error = -EREMOTEIO;
        goto ret;
    }

    char *attrs = res->resarray.resarray_val[1].nfs_resop4_u.opgetattr.GETATTR4res_u.resok4.obj_attributes.attr_vals.attrlist4_val;
    u_int attrs_len = res->resarray.resarray_val[1].nfs_resop4_u.opgetattr.GETATTR4res_u.resok4.obj_attributes.attr_vals.attrlist4_len;
    if (nfs_parse_attributes(vnfs->nfs, &cb_data->out_attr->attr, attrs, attrs_len) == 0) {
        // This is not filled in by the parse_attributes fn
        cb_data->out_attr->attr.rdev = 0;
        cb_data->out_attr->attr_valid = 0;
        cb_data->out_attr->attr_valid_nsec = 0;
    } else {
        cb_data->out_hdr->error = -EREMOTEIO;
    }

ret:
    cb_data->cb->cb(SNAP_FS_DEV_OP_SUCCESS, cb_data->cb->user_arg);
}

int getattr(struct fuse_session *se, struct virtionfs *vnfs,
                      struct fuse_in_header *in_hdr, struct fuse_getattr_in *in_getattr,
                      struct fuse_out_header *out_hdr, struct fuse_attr_out *out_attr,
                    struct snap_fs_dev_io_done_ctx *cb)
{
    struct getattr_cb_data *cb_data = mpool_alloc(vnfs->p);
    if (!cb_data) {
        out_hdr->error = -ENOMEM;
        return 0;
    }

    cb_data->cb = cb;
    cb_data->vnfs = vnfs;
    cb_data->out_hdr = out_hdr;
    cb_data->out_attr = out_attr;

    COMPOUND4args args;
    nfs_argop4 op[2];

    if (in_hdr->nodeid == FUSE_ROOT_ID) {
        op[0].argop = OP_PUTROOTFH;
    } else {
        op[0].argop = OP_PUTFH;
        op[0].nfs_argop4_u.opputfh.object.nfs_fh4_len = sizeof(in_hdr->nodeid);
        op[0].nfs_argop4_u.opputfh.object.nfs_fh4_val = (char *) &in_hdr->nodeid;
    }

    nfs4_op_getattr(vnfs->nfs, &op[1], standard_attributes, 2);
    
    memset(&args, 0, sizeof(args));
    args.argarray.argarray_len = sizeof(op) / sizeof(nfs_argop4);
    args.argarray.argarray_val = op;

    if (rpc_nfs4_compound_async(vnfs->rpc, getattr_cb, &args, cb_data) != 0) {
    	fprintf(stderr, "Failed to send nfs4 GETATTR request\n");
        out_hdr->error = -EREMOTEIO;
        return 0;
    }

    return EWOULDBLOCK;
}

static void get_supported_attributes(struct rpc_context *rpc, int status, void *data, void *private_data) {
    //struct virtionfs *vnfs = private_data;
    COMPOUND4res *res = data;
    
    if (status != RPC_STATUS_SUCCESS || res->status != NFS4_OK) {
    	fprintf(stderr, "Failed to get root filehandle of server\n");
    	exit(10);
    }

    //char *supported_attrs_bits = res->resarray.resarray_val[1].nfs_resop4_u.opgetattr.GETATTR4res_u.resok4.obj_attributes.attr_vals.attrlist4_val;
    u_int len = res->resarray.resarray_val[1].nfs_resop4_u.opgetattr.GETATTR4res_u.resok4.obj_attributes.attr_vals.attrlist4_len;
    printf("len=%u\n", len);
}

//static void getrootfh_cb(struct rpc_context *rpc, int status, void *data, void *private_data) {
//    struct virtionfs *vnfs = private_data;
//    COMPOUND4res *res = data;
//    
//    if (status != RPC_STATUS_SUCCESS || res->status != NFS4_OK) {
//    	fprintf(stderr, "Failed to get root filehandle of server\n");
//    	exit(10);
//    }
//    
//    vnfs->rootfh = res->resarray.resarray_val[1].nfs_resop4_u.opgetfh.GETFH4res_u.resok4.object;
//}

int init(struct fuse_session *se, struct virtionfs *vnfs,
    struct fuse_in_header *in_hdr, struct fuse_init_in *in_init,
    struct fuse_conn_info *conn, struct fuse_out_header *out_hdr)
{
    if (conn->capable & FUSE_CAP_EXPORT_SUPPORT)
        conn->want |= FUSE_CAP_EXPORT_SUPPORT;

    if ((vnfs->timeout_sec || vnfs->timeout_nsec) && conn->capable & FUSE_CAP_WRITEBACK_CACHE)
        conn->want |= FUSE_CAP_WRITEBACK_CACHE;

    if (conn->capable & FUSE_CAP_FLOCK_LOCKS)
        conn->want |= FUSE_CAP_FLOCK_LOCKS;

    // FUSE_CAP_SPLICE_READ is enabled in libfuse3 by default,
    // see do_init() in in fuse_lowlevel.c
    // We do not want this as splicing is not a thing with virtiofs
    conn->want &= ~FUSE_CAP_SPLICE_READ;
    conn->want &= ~FUSE_CAP_SPLICE_WRITE;

    int ret;
    if (in_hdr->uid != 0 && in_hdr->gid != 0) {
        ret = seteuid(in_hdr->uid);
        if (ret == -1) {
            warn("%s: Could not set uid of fuser to %d", __func__, in_hdr->uid);
            goto ret_errno;
        }
        ret = setegid(in_hdr->gid);
        if (ret == -1) {
            warn("%s: Could not set gid of fuser to %d", __func__, in_hdr->gid);
            goto ret_errno;
        }
    } else {
        printf("%s, init was not supplied with a non-zero uid and gid. "
        "Thus all operations will go through the name of uid %d and gid %d\n", __func__, getuid(), getgid());
    }

    ret = nfs_mount(vnfs->nfs, vnfs->server, vnfs->export);
    if (ret != 0) {
        printf("Failed to mount nfs\n");
        goto ret_errno;
    }
    if (nfs_mt_service_thread_start(vnfs->nfs)) {
        printf("Failed to start libnfs service thread\n");
        goto ret_errno;
    }

    COMPOUND4args args;
    nfs_argop4 op[2];
    
    memset(op, 0, sizeof(op));
    op[0].argop = OP_PUTROOTFH;
    nfs4_op_getattr(vnfs->nfs, &op[1], supported_attrs_attributes, 1);
    (void) standard_attributes;
    
    memset(&args, 0, sizeof(args));
    args.argarray.argarray_len = sizeof(op) / sizeof(nfs_argop4);
    args.argarray.argarray_val = op;
    if (rpc_nfs4_compound_async(vnfs->rpc, get_supported_attributes, &args, vnfs) != 0) {
    	fprintf(stderr, "Failed to send nfs4 GETROOTFH request\n");
    	exit(10);
    }

    return 0;
ret_errno:
    if (ret == -1)
        out_hdr->error = -errno;
    return 0;
}

void virtionfs_assign_ops(struct fuse_ll_operations *ops) {
    ops->init = (typeof(ops->init)) init;
    ops->lookup = (typeof(ops->lookup)) lookup;
    ops->getattr = (typeof(ops->getattr)) getattr;
    // NFS accepts the NFS:fh (received from a NFS:lookup==FUSE:lookup) as
    // its parameter to the dir ops like readdir
    ops->opendir = NULL;
    //ops->setattr = (typeof(ops->setattr)) setattr;
}

void virtionfs_main(char *server, char *export,
               bool debug, double timeout, uint32_t nthreads,
               struct virtiofs_emu_params *emu_params) {
    struct virtionfs *vnfs = calloc(1, sizeof(struct virtionfs));
    if (!vnfs) {
        warn("Failed to init virtionfs");
        return;
    }
    vnfs->server = server;
    vnfs->export = export;
    vnfs->debug = debug;
    vnfs->timeout_sec = calc_timeout_sec(timeout);
    vnfs->timeout_nsec = calc_timeout_nsec(timeout);

    vnfs->nfs = nfs_init_context();
    if (vnfs->nfs == NULL) {
        warn("Failed to init nfs context\n");
        goto virtionfs_main_ret_c;
    }
    nfs_set_version(vnfs->nfs, NFS_V4);
    vnfs->rpc = nfs_get_rpc_context(vnfs->nfs);

    vnfs->p = calloc(1, sizeof(struct mpool));
    if (!vnfs->p) {
        warn("Failed to init virtionfs");
        goto virtionfs_main_ret_b;
    }
    if (mpool_init(vnfs->p, 100, sizeof(struct getattr_cb_data), 10) < 0) {
        warn("Failed to init virtionfs");
        goto virtionfs_main_ret_a;
    }

    struct fuse_ll_operations ops;
    memset(&ops, 0, sizeof(ops));
    virtionfs_assign_ops(&ops);

    virtiofs_emu_fuse_ll_main(&ops, emu_params, vnfs, debug);
    printf("nfsclient finished\n");

    mpool_destroy(vnfs->p);
virtionfs_main_ret_a:
    free(vnfs->p);
virtionfs_main_ret_b:
    nfs_destroy_context(vnfs->nfs);
virtionfs_main_ret_c:
    free(vnfs);
}
