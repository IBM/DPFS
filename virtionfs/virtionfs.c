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
#include "fuse_ll.h"

struct setattr_cb_data {
    struct snap_fs_dev_io_done_ctx *cb;
    struct virtionfs *vnfs;
    struct fuse_out_header *out_hdr;
    struct fuse_attr_out *out_attr;
};

void setattr_cb(struct rpc_context *rpc, int status, void *data,
                       void *private_data) {
    enum snap_fs_dev_op_status fs_status;

    struct setattr_cb_data *cb_data = (struct setattr_cb_data *)private_data;

    if (status == RPC_STATUS_SUCCESS) {
        struct SETATTR3res *res = (struct SETATTR3res *) data;
        if (res->status == NFS3_OK) {
            struct SETATTR3resok *resok = (struct SETATTR3resok *) data;
            if (resok->obj_wcc.after.attributes_follow) {
                fattr3_to_fuse_attr(&resok->obj_wcc.after.post_op_attr_u.attributes, &cb_data->out_attr->attr);
                cb_data->out_attr->attr_valid = cb_data->vnfs->timeout_sec;
                cb_data->out_attr->attr_valid_nsec = cb_data->vnfs->timeout_nsec;
            } else { // I have no clue how that could even happen
                cb_data->out_hdr->error = -ENOENT;
            }
        } else {
            cb_data->out_hdr->error = -ENOENT;
        }

        fs_status = SNAP_FS_DEV_OP_SUCCESS;
    } else {
        fs_status = SNAP_FS_DEV_OP_IO_ERROR;
    }

    cb_data->cb->cb(fs_status, cb_data->cb->user_arg);
}

int setattr(struct fuse_session *se, struct virtionfs *vnfs,
                         struct fuse_in_header *in_hdr, struct stat *s, int valid, struct fuse_file_info *fi,
                         struct fuse_out_header *out_hdr, struct fuse_attr_out *out_attr,
                         struct snap_fs_dev_io_done_ctx *cb)
{
    struct setattr_cb_data *cb_data = mpool_alloc(vnfs->p);
    if (!cb_data) {
        out_hdr->error = -ENOMEM;
        return 0;
    }

    struct SETATTR3args args;
    memset(&args, 0, sizeof(args));
    args.object.data.data_len = sizeof(in_hdr->nodeid);
    args.object.data.data_val = (char *) &in_hdr->nodeid;

    if (valid & FUSE_SET_ATTR_MODE) {
        args.new_attributes.mode.set_it = 1;
        args.new_attributes.mode.set_mode3_u.mode = s->st_mode;
    }
    if (valid & FUSE_SET_ATTR_UID) {
        args.new_attributes.uid.set_it = 1;
        args.new_attributes.uid.set_uid3_u.uid = s->st_uid;
    }
    if (valid & FUSE_SET_ATTR_GID) {
        args.new_attributes.gid.set_it = 1;
        args.new_attributes.gid.set_gid3_u.gid = s->st_gid;
    }
    if (valid & FUSE_SET_ATTR_SIZE) {
        args.new_attributes.size.set_it = 1;
        args.new_attributes.size.set_size3_u.size = s->st_size;
    }
    if (valid & FUSE_SET_ATTR_ATIME) {
        args.new_attributes.atime.set_it = 1;
        if (valid & FUSE_SET_ATTR_ATIME_NOW) {
            args.new_attributes.atime.set_atime_u.atime.nseconds = UTIME_NOW;
        } else {
            args.new_attributes.atime.set_atime_u.atime.nseconds = s->st_atim.tv_nsec;
            args.new_attributes.atime.set_atime_u.atime.seconds = s->st_atim.tv_sec;
        }
    }
    if (valid & FUSE_SET_ATTR_MTIME) {
        args.new_attributes.mtime.set_it = 1;
        if (valid & FUSE_SET_ATTR_MTIME_NOW) {
            args.new_attributes.mtime.set_mtime_u.mtime.nseconds = UTIME_NOW;
        } else {
            args.new_attributes.mtime.set_mtime_u.mtime.nseconds = s->st_mtim.tv_nsec;
            args.new_attributes.mtime.set_mtime_u.mtime.seconds = s->st_mtim.tv_sec;
        }
    }

    int res = rpc_nfs3_setattr_async(vnfs->rpc, setattr_cb, &args, cb_data);
    if (res != 0)
        out_hdr->error = -res;
    return 0;
}

struct lookup_cb_data {
    struct snap_fs_dev_io_done_ctx *cb;
    struct virtionfs *vnfs;
    struct fuse_out_header *out_hdr;
    struct fuse_entry_out *out_entry;
};

void lookup_cb(struct rpc_context *rpc, int status, void *data,
                       void *private_data) {
    enum snap_fs_dev_op_status fs_status;

    struct lookup_cb_data *cb_data = (struct lookup_cb_data *)private_data;

    if (status == RPC_STATUS_SUCCESS) {
        struct LOOKUP3res *res = (struct LOOKUP3res *) data;
        if (res->status == NFS3_OK) {
            LOOKUP3resok *resok = &res->LOOKUP3res_u.resok;
            if (resok->obj_attributes.attributes_follow) {
                fattr3_to_fuse_attr(&resok->obj_attributes.post_op_attr_u.attributes, &cb_data->out_entry->attr);
                cb_data->out_entry->generation = 0;
                cb_data->out_entry->nodeid = resok->obj_attributes.post_op_attr_u.attributes.fileid;
                cb_data->out_entry->attr_valid = cb_data->vnfs->timeout_sec;
                cb_data->out_entry->attr_valid_nsec = cb_data->vnfs->timeout_nsec;
                cb_data->out_entry->entry_valid = cb_data->vnfs->timeout_sec;
                cb_data->out_entry->entry_valid_nsec = cb_data->vnfs->timeout_nsec;
            } else { // I have no clue how that could even happen
                cb_data->out_hdr->error = -ENOENT;
            }
        } else {
            cb_data->out_hdr->error = res->status;
        }

        fs_status = SNAP_FS_DEV_OP_SUCCESS;
    } else {
        fs_status = SNAP_FS_DEV_OP_IO_ERROR;
    }

    cb_data->cb->cb(fs_status, cb_data->cb->user_arg);
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

    struct LOOKUP3args args;
    args.what.dir.data.data_len = sizeof(in_hdr->nodeid);
    args.what.dir.data.data_val = (char *) &in_hdr->nodeid;
    args.what.name = in_name;

    int res = rpc_nfs3_lookup_async(vnfs->rpc, lookup_cb, &args, cb_data);
    if (res != 0)
        out_hdr->error = -res;
    return 0;
}

struct getattr_cb_data {
    struct snap_fs_dev_io_done_ctx *cb;
    struct virtionfs *vnfs;
    struct fuse_out_header *out_hdr;
    struct fuse_attr_out *out_attr;
};

void getattr_cb(struct rpc_context *rpc, int status, void *data,
                       void *private_data) {
    enum snap_fs_dev_op_status fs_status;

    struct getattr_cb_data *cb_data = (struct getattr_cb_data *)private_data;

    if (status == RPC_STATUS_SUCCESS) {
        struct GETATTR3res *res = (struct GETATTR3res *) data;
        if (res->status == NFS3_OK) {
            cb_data->out_attr->attr.ino = res->GETATTR3res_u.resok.obj_attributes.fileid;
        } else {
            cb_data->out_hdr->error = res->status;
        }
        fs_status = SNAP_FS_DEV_OP_SUCCESS;

    } else {
        fs_status = SNAP_FS_DEV_OP_IO_ERROR;
    }

    cb_data->cb->cb(fs_status, cb_data->cb->user_arg);
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

    struct GETATTR3args args;
    args.object.data.data_len = sizeof(in_getattr->fh);
    args.object.data.data_val = (char *) &in_getattr->fh;

    int res = rpc_nfs3_getattr_async(vnfs->rpc, getattr_cb, &args, &cb_data);
    if (res != 0)
        out_hdr->error = -res;
    return EWOULDBLOCK;
}

static void getrootfh_cb(struct rpc_context *rpc, int status, void *data, void *private_data) {
    struct virtionfs *vnfs = private_data;
    COMPOUND4res *res = data;
    
    if (status != RPC_STATUS_SUCCESS || res->status != NFS4_OK) {
    	fprintf(stderr, "Failed to get root filehandle of server\n");
    	exit(10);
    }
    
    vnfs->rootfh = res->resarray.resarray_val[1].nfs_resop4_u.opgetfh.GETFH4res_u.resok4.object;
}

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
    op[1].argop = OP_GETFH;
    
    memset(&args, 0, sizeof(args));
    args.argarray.argarray_len = sizeof(op) / sizeof(nfs_argop4);
    args.argarray.argarray_val = op;
    if (rpc_nfs4_compound_async(vnfs->rpc, getrootfh_cb, &args, vnfs) != 0) {
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
    ops->lookup = (typeof(ops->lookup)) NULL;
    ops->getattr = (typeof(ops->getattr)) NULL;
    // NFS accepts the NFS:fh (received from a NFS:lookup==FUSE:lookup) as
    // its parameter to the dir ops like readdir
    ops->opendir = NULL;
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
