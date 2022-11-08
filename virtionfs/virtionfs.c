/*
#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

#include <sys/time.h>
#include <nfsc/libnfs.h>
#include <nfsc/libnfs-raw.h>
#include <nfsc/libnfs-raw-nfs4.h>
#include <linux/fuse.h>
#include <arpa/inet.h>
#include <poll.h>
#include <stdatomic.h>
#include <err.h>

#include "fuse_ll.h"
#include "config.h"
#include "virtionfs.h"
#include "mpool.h"
#ifdef LATENCY_MEASURING_ENABLED
#include "ftimer.h"
#endif
#include "nfs_v4.h"
#include "inode.h"

#ifdef LATENCY_MEASURING_ENABLED
static struct ftimer ft[FUSE_REMOVEMAPPING+1];
static uint64_t op_calls[FUSE_REMOVEMAPPING+1];
#endif

// static uint32_t supported_attrs_attributes[1] = {
//     (1 << FATTR4_SUPPORTED_ATTRS)
// };
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

// uint64_t blocks = FATTR4_SPACE_TOTAL / BLOCKSIZE
// uint64_t bfree; = FATTR4_SPACE_FREE / BLOCKSIE
// uint64_t bavail; = FATTR4_SPACE_AVAIL / BLOCKSIZE
// uint64_t files; = FATTR4_FILES_TOTAL
// uint64_t ffree; = FATTR4_FILES_FREE
// uint32_t bsize; = BLOCKSIZE
// uint32_t namelen; = FATTR4_MAXNAME
// uint32_t frsize; = BLOCKSIZE

static uint32_t statfs_attributes[2] = {
        (1 << FATTR4_FILES_FREE |
         1 << FATTR4_FILES_TOTAL |
         1 << FATTR4_MAXNAME),
        (1 << (FATTR4_SPACE_AVAIL - 32) |
         1 << (FATTR4_SPACE_FREE - 32) |
         1 << (FATTR4_SPACE_TOTAL - 32))
};

int nfs4_op_putfh(struct virtionfs *vnfs, nfs_argop4 *op, uint64_t nodeid)
{
    op->argop = OP_PUTFH;
    if (nodeid == FUSE_ROOT_ID) {
        op->nfs_argop4_u.opputfh.object.nfs_fh4_val = vnfs->rootfh.nfs_fh4_val;
        op->nfs_argop4_u.opputfh.object.nfs_fh4_len = vnfs->rootfh.nfs_fh4_len;
    } else {
        struct inode *i = inode_table_get(vnfs->inodes, nodeid);
        if (!i) {
            return 0;
        }
        op->nfs_argop4_u.opputfh.object.nfs_fh4_val = i->fh.nfs_fh4_val;
        op->nfs_argop4_u.opputfh.object.nfs_fh4_len = i->fh.nfs_fh4_len;
    }
    return 1;
}

struct setattr_cb_data {
    struct snap_fs_dev_io_done_ctx *cb;
    struct virtionfs *vnfs;
    struct fuse_out_header *out_hdr;
    struct fuse_attr_out *out_attr;

    uint64_t *bitmap;
    char *attrlist;
};

void setattr_cb(struct rpc_context *rpc, int status, void *data,
                       void *private_data) {
    struct setattr_cb_data *cb_data = (struct setattr_cb_data *)private_data;
    struct virtionfs *vnfs = cb_data->vnfs;
    COMPOUND4res *res = data;

    if (status != RPC_STATUS_SUCCESS) {
    	fprintf(stderr, "RPC with NFS:LOOKUP unsuccessful: rpc error=%d\n", status);
        cb_data->out_hdr->error = -EREMOTEIO;
        goto ret;
    }
    if (res->status != NFS4_OK) {
        cb_data->out_hdr->error = -nfs_error_to_fuse_error(res->status);
    	fprintf(stderr, "NFS:LOOKUP unsuccessful: nfs error=%d, fuse error=%d\n",
                res->status, cb_data->out_hdr->error);
        goto ret;
    }

    GETATTR4resok *resok = &res->resarray.resarray_val[1].nfs_resop4_u.opgetattr.GETATTR4res_u.resok4;
    char *attrs = resok->obj_attributes.attr_vals.attrlist4_val;
    u_int attrs_len = resok->obj_attributes.attr_vals.attrlist4_len;
    if (nfs_parse_attributes(&cb_data->out_attr->attr, attrs, attrs_len) == 0) {
        // This is not filled in by the parse_attributes fn
        cb_data->out_attr->attr.rdev = 0;
        cb_data->out_attr->attr_valid = 0;
        cb_data->out_attr->attr_valid_nsec = 0;
    } else {
        cb_data->out_hdr->error = -EREMOTEIO;
    }

ret:;
    free(cb_data->bitmap);
    free(cb_data->attrlist);
    struct snap_fs_dev_io_done_ctx *cb = cb_data->cb;
    mpool_free(vnfs->p, cb_data);
    cb->cb(SNAP_FS_DEV_OP_SUCCESS, cb->user_arg);
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

    cb_data->cb = cb;
    cb_data->vnfs = vnfs;
    cb_data->out_hdr = out_hdr;
    cb_data->out_attr = out_attr;

    COMPOUND4args args;
    nfs_argop4 op[4];

    int fh_found = nfs4_op_putfh(vnfs, &op[0], in_hdr->nodeid);
    if (!fh_found) {
    	fprintf(stderr, "Invalid nodeid supplied to GETATTR\n");
        mpool_free(vnfs->p, cb_data);
        out_hdr->error = -ENOENT;
        return 0;
    }

    /* TODO if locking is supported, put the stateid in here
     * zeroed stateid means anonymous aka
     * which can be used in READ, WRITE, and SETATTR requests to indicate the absence of any
     * open state associated with the request. (from 9.1.4.3. NFS 4 rfc)
     */
    op[1].argop = OP_SETATTR;
    memset(&op[1].nfs_argop4_u.opsetattr.stateid, 0, sizeof(stateid4));

    uint64_t *bitmap = malloc(sizeof(*bitmap));
    bitmap4 attrsmask;
    attrsmask.bitmap4_len = sizeof(*bitmap);
    attrsmask.bitmap4_val = (uint32_t *) bitmap;

    size_t attrlist_len = 0;
    if (valid & FUSE_SET_ATTR_MODE) {
        *bitmap |= (1UL << FATTR4_MODE);
        attrlist_len += 4;
    }
    if (valid & FUSE_SET_ATTR_SIZE) {
        *bitmap |= (1UL << FATTR4_SIZE);
    }
    char *attrlist = malloc(attrlist_len);

    if (valid & FUSE_SET_ATTR_MODE) {
        *(uint32_t *) attrlist = htonl(s->st_mode);
        attrlist += 4;
    }
    if (valid & FUSE_SET_ATTR_SIZE) {
        *(uint32_t *) attrlist = nfs_hton64(s->st_size);
        attrlist += 8;
    }

    op[1].nfs_argop4_u.opsetattr.obj_attributes.attrmask = attrsmask;
    op[1].nfs_argop4_u.opsetattr.obj_attributes.attr_vals.attrlist4_val = attrlist;
    op[1].nfs_argop4_u.opsetattr.obj_attributes.attr_vals.attrlist4_len = attrlist_len;
    cb_data->bitmap = bitmap;
    cb_data->attrlist = attrlist;

    nfs4_op_getattr(&op[2], standard_attributes, 2);

    memset(&args, 0, sizeof(args));
    args.argarray.argarray_len = sizeof(op) / sizeof(nfs_argop4);
    args.argarray.argarray_val = op;

    if (rpc_nfs4_compound_async(vnfs->rpc, setattr_cb, &args, cb_data) != 0) {
    	fprintf(stderr, "Failed to send nfs4 SETATTR request\n");
        mpool_free(vnfs->p, cb_data);
        out_hdr->error = -EREMOTEIO;
        return 0;
    }

    return EWOULDBLOCK;
}

struct statfs_cb_data {
    struct snap_fs_dev_io_done_ctx *cb;
    struct virtionfs *vnfs;

    struct fuse_out_header *out_hdr;
    struct fuse_statfs_out *stat;
};

void statfs_cb(struct rpc_context *rpc, int status, void *data,
                       void *private_data) {
    struct statfs_cb_data *cb_data = (struct statfs_cb_data *)private_data;
#ifdef LATENCY_MEASURING_ENABLED
    ft_stop(&ft[FUSE_STATFS]);
#endif
    struct virtionfs *vnfs = cb_data->vnfs;
    COMPOUND4res *res = data;

    if (status != RPC_STATUS_SUCCESS) {
    	fprintf(stderr, "RPC with NFS:LOOKUP unsuccessful: rpc error=%d\n", status);
        cb_data->out_hdr->error = -EREMOTEIO;
        goto ret;
    }
    if (res->status != NFS4_OK) {
        cb_data->out_hdr->error = -nfs_error_to_fuse_error(res->status);
    	fprintf(stderr, "NFS:LOOKUP unsuccessful: nfs error=%d, fuse error=%d\n",
                res->status, cb_data->out_hdr->error);
        goto ret;
    }

    GETATTR4resok *resok = &res->resarray.resarray_val[1].nfs_resop4_u.opgetattr.GETATTR4res_u.resok4;
    char *attrs = resok->obj_attributes.attr_vals.attrlist4_val;
    u_int attrs_len = resok->obj_attributes.attr_vals.attrlist4_len;
    if (nfs_parse_statfs(&cb_data->stat->st, attrs, attrs_len) != 0) {
        cb_data->out_hdr->error = -EREMOTEIO;
    }

ret:;
    struct snap_fs_dev_io_done_ctx *cb = cb_data->cb;
    mpool_free(vnfs->p, cb_data);
    cb->cb(SNAP_FS_DEV_OP_SUCCESS, cb->user_arg);
}


int statfs (struct fuse_session *se, struct virtionfs *vnfs,
            struct fuse_in_header *in_hdr,
            struct fuse_out_header *out_hdr, struct fuse_statfs_out *stat,
            struct snap_fs_dev_io_done_ctx *cb)
{
    struct statfs_cb_data *cb_data = mpool_alloc(vnfs->p);
    if (!cb_data) {
        out_hdr->error = -ENOMEM;
        return 0;
    }

    cb_data->cb = cb;
    cb_data->vnfs = vnfs;
    cb_data->out_hdr = out_hdr;
    cb_data->stat = stat;

    COMPOUND4args args;
    nfs_argop4 op[2];

    // PUTFH
    nfs4_op_putfh(vnfs, &op[0], FUSE_ROOT_ID);
    // GETATTR
    nfs4_op_getattr(&op[1], statfs_attributes, 2);

    memset(&args, 0, sizeof(args));
    args.argarray.argarray_len = sizeof(op) / sizeof(nfs_argop4);
    args.argarray.argarray_val = op;

#ifdef LATENCY_MEASURING_ENABLED
    op_calls[FUSE_STATFS]++;
    ft_start(&ft[FUSE_STATFS]);
#endif

    if (rpc_nfs4_compound_async(vnfs->rpc, statfs_cb, &args, cb_data) != 0) {
    	fprintf(stderr, "Failed to send FUSE:statfs request\n");
        mpool_free(vnfs->p, cb_data);
        out_hdr->error = -EREMOTEIO;
        return 0;
    }

    return EWOULDBLOCK;
}

struct lookup_cb_data {
    struct snap_fs_dev_io_done_ctx *cb;
    struct virtionfs *vnfs;

    struct fuse_out_header *out_hdr;
    struct fuse_entry_out *out_entry;
};

void lookup_cb(struct rpc_context *rpc, int status, void *data,
                       void *private_data) {
    struct lookup_cb_data *cb_data = (struct lookup_cb_data *)private_data;
#ifdef LATENCY_MEASURING_ENABLED
    ft_stop(&ft[FUSE_GETATTR]);
#endif
    struct virtionfs *vnfs = cb_data->vnfs;
    COMPOUND4res *res = data;


    if (status != RPC_STATUS_SUCCESS) {
    	fprintf(stderr, "RPC with NFS:LOOKUP unsuccessful: rpc error=%d\n", status);
        cb_data->out_hdr->error = -EREMOTEIO;
        goto ret;
    }
    if (res->status != NFS4_OK) {
        cb_data->out_hdr->error = -nfs_error_to_fuse_error(res->status);
    	fprintf(stderr, "NFS:LOOKUP unsuccessful: nfs error=%d, fuse error=%d\n",
                res->status, cb_data->out_hdr->error);
        goto ret;
    }

    char *attrs = res->resarray.resarray_val[2].nfs_resop4_u.opgetattr.GETATTR4res_u.resok4.obj_attributes.attr_vals.attrlist4_val;
    u_int attrs_len = res->resarray.resarray_val[2].nfs_resop4_u.opgetattr.GETATTR4res_u.resok4.obj_attributes.attr_vals.attrlist4_len;
    int ret = nfs_parse_attributes(&cb_data->out_entry->attr, attrs, attrs_len);
    if (ret != 0) {
        cb_data->out_hdr->error = -EREMOTEIO;
        goto ret;
    }
    fattr4_fileid fileid = cb_data->out_entry->attr.ino;
    // Finish the attr
    cb_data->out_entry->attr_valid = 0;
    cb_data->out_entry->attr_valid_nsec = 0;
    cb_data->out_entry->entry_valid = 0;
    cb_data->out_entry->entry_valid_nsec = 0;
    // Taken from the nfs_parse_attributes
    cb_data->out_entry->nodeid = fileid;
    cb_data->out_entry->generation = 0;

    struct inode *i = inode_table_getsert(vnfs->inodes, fileid);
    if (!i) {
        fprintf(stderr, "Couldn't getsert inode with fileid: %lu\n", fileid);
        cb_data->out_hdr->error = -ENOMEM;
        goto ret;
    }
    atomic_fetch_add(&i->nlookup, 1);
    cb_data->out_entry->generation = i->generation;

    if (i->fh.nfs_fh4_len == 0) {
        // Retreive the FH from the res and set it in the inode
        // it's stored in the inode for later use ex. getattr when it uses the nodeid
        int ret = nfs4_clone_fh(&i->fh, &res->resarray.resarray_val[3].nfs_resop4_u.opgetfh.GETFH4res_u.resok4.object);
        if (ret < 0) {
            fprintf(stderr, "Couldn't clone fh with fileid: %lu\n", fileid);
            cb_data->out_hdr->error = -ENOMEM;
            goto ret;
        }
    }

ret:;
    struct snap_fs_dev_io_done_ctx *cb = cb_data->cb;
    mpool_free(vnfs->p, cb_data);
    cb->cb(SNAP_FS_DEV_OP_SUCCESS, cb->user_arg);
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
    nfs_argop4 op[4];

    // PUTFH
    int fh_found = nfs4_op_putfh(vnfs, &op[0], in_hdr->nodeid);
    if (!fh_found) {
    	fprintf(stderr, "Invalid nodeid supplied to LOOKUP\n");
        mpool_free(vnfs->p, cb_data);
        out_hdr->error = -ENOENT;
        return 0;
    }
    // LOOKUP
    nfs4_op_lookup(&op[1], in_name);
    // FH now replaced with in_name's FH
    // GETATTR
    nfs4_op_getattr(&op[2], standard_attributes, 2);
    // GETFH
    op[3].argop = OP_GETFH;

    memset(&args, 0, sizeof(args));
    args.argarray.argarray_len = sizeof(op) / sizeof(nfs_argop4);
    args.argarray.argarray_val = op;

#ifdef LATENCY_MEASURING_ENABLED
    op_calls[FUSE_LOOKUP]++;
    ft_start(&ft[FUSE_LOOKUP]);
#endif

    if (rpc_nfs4_compound_async(vnfs->rpc, lookup_cb, &args, cb_data) != 0) {
    	fprintf(stderr, "Failed to send nfs4 LOOKUP request\n");
        mpool_free(vnfs->p, cb_data);
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
#ifdef LATENCY_MEASURING_ENABLED
    ft_stop(&ft[FUSE_GETATTR]);
#endif
    struct virtionfs *vnfs = cb_data->vnfs;
    COMPOUND4res *res = data;

    if (status != RPC_STATUS_SUCCESS) {
    	fprintf(stderr, "RPC with NFS:LOOKUP unsuccessful: rpc error=%d\n", status);
        cb_data->out_hdr->error = -EREMOTEIO;
        goto ret;
    }
    if (res->status != NFS4_OK) {
        cb_data->out_hdr->error = -nfs_error_to_fuse_error(res->status);
    	fprintf(stderr, "NFS:LOOKUP unsuccessful: nfs error=%d, fuse error=%d\n",
                res->status, cb_data->out_hdr->error);
        goto ret;
    }

    GETATTR4resok *resok = &res->resarray.resarray_val[1].nfs_resop4_u.opgetattr.GETATTR4res_u.resok4;
    char *attrs = resok->obj_attributes.attr_vals.attrlist4_val;
    u_int attrs_len = resok->obj_attributes.attr_vals.attrlist4_len;
    if (nfs_parse_attributes(&cb_data->out_attr->attr, attrs, attrs_len) == 0) {
        // This is not filled in by the parse_attributes fn
        cb_data->out_attr->attr.rdev = 0;
        cb_data->out_attr->attr_valid = 0;
        cb_data->out_attr->attr_valid_nsec = 0;
    } else {
        cb_data->out_hdr->error = -EREMOTEIO;
    }

ret:;
    struct snap_fs_dev_io_done_ctx *cb = cb_data->cb;
    mpool_free(vnfs->p, cb_data);
    cb->cb(SNAP_FS_DEV_OP_SUCCESS, cb->user_arg);
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

    int fh_found = nfs4_op_putfh(vnfs, &op[0], in_hdr->nodeid);
    if (!fh_found) {
    	fprintf(stderr, "Invalid nodeid supplied to GETATTR\n");
        mpool_free(vnfs->p, cb_data);
        out_hdr->error = -ENOENT;
        return 0;
    }
    nfs4_op_getattr(&op[1], standard_attributes, 2);
    
    memset(&args, 0, sizeof(args));
    args.argarray.argarray_len = sizeof(op) / sizeof(nfs_argop4);
    args.argarray.argarray_val = op;

#ifdef LATENCY_MEASURING_ENABLED
    op_calls[FUSE_GETATTR]++;
    ft_start(&ft[FUSE_GETATTR]);
#endif
    if (rpc_nfs4_compound_async(vnfs->rpc, getattr_cb, &args, cb_data) != 0) {
    	fprintf(stderr, "Failed to send nfs4 GETATTR request\n");
        mpool_free(vnfs->p, cb_data);
        out_hdr->error = -EREMOTEIO;
        return 0;
    }

    return EWOULDBLOCK;
}

struct lookup_true_rootfh_cb_data {
    struct virtionfs *vnfs;
    struct fuse_out_header *out_hdr;
    struct snap_fs_dev_io_done_ctx *cb;
    char *export;
};

static void lookup_true_rootfh_cb(struct rpc_context *rpc, int status, void *data,
                       void *private_data) {
    struct lookup_true_rootfh_cb_data *cb_data = private_data;
    struct virtionfs *vnfs = cb_data->vnfs;
    COMPOUND4res *res = data;

    if (status != RPC_STATUS_SUCCESS) {
    	fprintf(stderr, "RPC with NFS:LOOKUP_TRUE_ROOTFH unsuccessful: rpc error=%d\n", status);
        goto ret;
    }
    if (res->status != NFS4_OK) {
        cb_data->out_hdr->error = -nfs_error_to_fuse_error(res->status);
    	fprintf(stderr, "NFS:LOOKUP_TRUE_ROOTFH unsuccessful: nfs error=%d, fuse error=%d\n",
                res->status, cb_data->out_hdr->error);
        goto ret;
    }

    int i = nfs4_find_op(res, OP_GETFH);
    assert(i >= 0);

    // Store the filehandle of the TRUE root (aka the filehandle of where our export lives)
    nfs4_clone_fh(&vnfs->rootfh, &res->resarray.resarray_val[i].nfs_resop4_u.opgetfh.GETFH4res_u.resok4.object); 

ret:
    free(cb_data->export);
    struct snap_fs_dev_io_done_ctx *cb = cb_data->cb;
    mpool_free(vnfs->p, cb_data);
    cb->cb(SNAP_FS_DEV_OP_SUCCESS, cb->user_arg);
}

static int lookup_true_rootfh(struct virtionfs *vnfs, struct fuse_out_header *out_hdr,
    struct snap_fs_dev_io_done_ctx *cb)
{
    struct lookup_true_rootfh_cb_data *cb_data = mpool_alloc(vnfs->p);
    if (!cb_data) {
        out_hdr->error = -ENOMEM;
        return 0;
    }

    cb_data->cb = cb;
    cb_data->vnfs = vnfs;
    cb_data->out_hdr = out_hdr;

    char *export = strdup(vnfs->export);
    cb_data->export = export;
    int export_len = strlen(export);
    // Chop off the last slash, this is to count the correct number
    // of path elements
    if (export[export_len-1] == '/') {
        export[export_len-1] = '\0';
    }
    // Count the slashes
    size_t count = 0;
    while(*vnfs->export) if (*vnfs->export++ == '/') ++count;

    COMPOUND4args args;
    nfs_argop4 op[2+count];
    int i = 0;

    // PUTFH
    op[i++].argop = OP_PUTROOTFH;
    // LOOKUP
    char *token = strtok(export, "/");
    while (i < count+1) {
        nfs4_op_lookup(&op[i++], token);
        token = strtok(NULL, "/");
    }
    // GETFH
    op[i].argop = OP_GETFH;

    memset(&args, 0, sizeof(args));
    args.argarray.argarray_len = sizeof(op) / sizeof(nfs_argop4);
    args.argarray.argarray_val = op;

    if (rpc_nfs4_compound_async(vnfs->rpc, lookup_true_rootfh_cb, &args, cb_data) != 0) {
    	fprintf(stderr, "Failed to send nfs4 LOOKUP request\n");
        mpool_free(vnfs->p, cb_data);
        return -1;
    }

    return 0;
}

int destroy(struct fuse_session *se, struct virtionfs *vnfs,
            struct fuse_in_header *in_hdr,
            struct fuse_out_header *out_hdr,
            struct snap_fs_dev_io_done_ctx *cb)
{
#ifdef LATENCY_MEASURING_ENABLED
    for (int i = 1; i < FUSE_REMOVEMAPPING+1; i++) {
        printf("OP(%d) took %lu averaged over %lu calls\n", i, ft_get_nsec(&ft[i]) / op_calls[i], op_calls[i]);
    }
#endif
    return 0;
}

int init(struct fuse_session *se, struct virtionfs *vnfs,
    struct fuse_in_header *in_hdr, struct fuse_init_in *in_init,
    struct fuse_conn_info *conn, struct fuse_out_header *out_hdr,
    struct snap_fs_dev_io_done_ctx *cb)
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

#ifdef LATENCY_MEASURING_ENABLED
    for (int i = 0; i < FUSE_REMOVEMAPPING+1; i++) {
        ft_init(&ft[i]);
        op_calls[i] = 0;
    }
#endif

    if (lookup_true_rootfh(vnfs, out_hdr, cb)) {
        printf("Failed to retreive root filehandle for the given export\n");
        out_hdr->error = -ENOENT;
        return 0;
    }

    // TODO WARNING
    // By returning 0, we allow the host to imediately start sending us requests,
    // even though the lookup_true_rootfh might not be done yet
    // This introduces a race condition, where if the rootfh is not found yet
    // virtionfs will crash horribly
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
    ops->statfs = (typeof(ops->statfs)) statfs;
    ops->destroy = (typeof(ops->destroy)) destroy;
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
    if (export[0] != '/') {
        fprintf(stderr, "export must start with a '/'\n");
        goto ret_a;
    }
    vnfs->export = export;
    vnfs->debug = debug;
    vnfs->timeout_sec = calc_timeout_sec(timeout);
    vnfs->timeout_nsec = calc_timeout_nsec(timeout);

    vnfs->nfs = nfs_init_context();
    if (vnfs->nfs == NULL) {
        warn("Failed to init nfs context\n");
        goto ret_a;
    }
    nfs_set_version(vnfs->nfs, NFS_V4);
    vnfs->rpc = nfs_get_rpc_context(vnfs->nfs);

    vnfs->p = calloc(1, sizeof(struct mpool));
    if (!vnfs->p) {
        warn("Failed to init virtionfs");
        goto ret_b;
    }
    if (mpool_init(vnfs->p, sizeof(struct getattr_cb_data), 10) < 0) {
        warn("Failed to init virtionfs");
        goto ret_c;
    }

    vnfs->inodes = calloc(1, sizeof(struct inode_table));
    if (!vnfs->inodes) {
        warn("Failed to init virtionfs");
        goto ret_d;
    }
    if (inode_table_init(vnfs->inodes) < 0) {
        warn("Failed to init virtionfs");
        goto ret_e;
    }

    struct fuse_ll_operations ops;
    memset(&ops, 0, sizeof(ops));
    virtionfs_assign_ops(&ops);

    virtiofs_emu_fuse_ll_main(&ops, emu_params, vnfs, debug);
    printf("nfsclient finished\n");

    inode_table_destroy(vnfs->inodes);
ret_e:
    free(vnfs->inodes);
ret_d:
    mpool_destroy(vnfs->p);
ret_c:
    free(vnfs->p);
ret_b:
    nfs_destroy_context(vnfs->nfs);
ret_a:
    free(vnfs);
}
