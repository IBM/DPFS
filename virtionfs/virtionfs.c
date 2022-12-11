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
#include <fcntl.h>
#include <stdatomic.h>
#include <err.h>

#include "fuse_ll.h"
#include "config.h"
#include "virtionfs.h"
#include "vnfs_connect.h"
#include "mpool2.h"
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

// How statfs_attributes maps to struct fuse_kstatfs
// blocks  = FATTR4_SPACE_TOTAL / BLOCKSIZE
// bfree   = FATTR4_SPACE_FREE / BLOCKSIE
// bavail  = FATTR4_SPACE_AVAIL / BLOCKSIZE
// files   = FATTR4_FILES_TOTAL
// ffree   = FATTR4_FILES_FREE
// bsize   = BLOCKSIZE
// namelen = FATTR4_MAXNAME
// frsize  = BLOCKSIZE

static uint32_t statfs_attributes[2] = {
        (1 << FATTR4_FILES_FREE |
         1 << FATTR4_FILES_TOTAL |
         1 << FATTR4_MAXNAME),
        (1 << (FATTR4_SPACE_AVAIL - 32) |
         1 << (FATTR4_SPACE_FREE - 32) |
         1 << (FATTR4_SPACE_TOTAL - 32))
};

// supported_attributes = standard_attributes | statfs_attributes

struct inode *vnfs4_op_putfh(struct virtionfs *vnfs, nfs_argop4 *op, uint64_t nodeid)
{
    op->argop = OP_PUTFH;
    struct inode *i = inode_table_get(vnfs->inodes, nodeid);
    if (!i) {
        return NULL;
    }
    op->nfs_argop4_u.opputfh.object.nfs_fh4_val = i->fh.val;
    op->nfs_argop4_u.opputfh.object.nfs_fh4_len = i->fh.len;
    return i;
}

struct inode *vnfs4_op_putfh_open(struct virtionfs *vnfs, nfs_argop4 *op, uint64_t nodeid)
{
    op->argop = OP_PUTFH;
    struct inode *i = inode_table_get(vnfs->inodes, nodeid);
    if (!i) {
        return NULL;
    }
    op->nfs_argop4_u.opputfh.object.nfs_fh4_val = i->fh_open.val;
    op->nfs_argop4_u.opputfh.object.nfs_fh4_len = i->fh_open.len;
    return i;
}

struct vnfs_conn* vnfs_get_conn(struct virtionfs *vnfs) {
    size_t threadid = (size_t) pthread_getspecific(virtiofs_thread_id_key);
    return &vnfs->conns[threadid];
}

int vnfs4_op_sequence(nfs_argop4 *op, struct vnfs_conn *conn, bool cachethis)
{
    op->argop = OP_SEQUENCE;
    struct SEQUENCE4args *arg = &op[0].nfs_argop4_u.opsequence;
    
    arg->sa_cachethis = cachethis;
    // sessionid
    memcpy(arg->sa_sessionid, conn->session.sessionid, sizeof(sessionid4));
    // slot stuff
    arg->sa_slotid = conn->session.target_highest_slot;
    struct vnfs_slot *slot = &conn->session.slots[arg->sa_slotid];
    arg->sa_sequenceid = ++slot->seqid;
    if (arg->sa_slotid > conn->session.highest_slot) {
        conn->session.highest_slot = arg->sa_slotid;
    }
    arg->sa_highest_slotid = conn->session.highest_slot;
    
    return 1;
}

struct create_cb_data {
    struct snap_fs_dev_io_done_ctx *cb;
    struct virtionfs *vnfs;

    struct inode *i;

    struct fuse_out_header *out_hdr;
    struct fuse_entry_out *out_entry;
    struct fuse_open_out *out_open;

    uint32_t owner_val;
};

void create_cb(struct rpc_context *rpc, int status, void *data,
                       void *private_data) {
#ifdef LATENCY_MEASURING_ENABLED
    if (vnfs->nthreads == 1) {
        ft_stop(&ft[FUSE_CREATE]);
    }
#endif
    struct create_cb_data *cb_data = (struct create_cb_data *)private_data;
    struct virtionfs *vnfs = cb_data->vnfs;
    COMPOUND4res *res = data;

    if (status != RPC_STATUS_SUCCESS) {
    	fprintf(stderr, "RPC with NFS:OPEN (with create) unsuccessful: rpc error=%d\n", status);
        cb_data->out_hdr->error = -EREMOTEIO;
        goto ret;
    }
    if (res->status != NFS4_OK) {
        cb_data->out_hdr->error = -nfs_error_to_fuse_error(res->status);
    	fprintf(stderr, "NFS:OPEN (with create) unsuccessful: nfs error=%d, fuse error=%d\n",
                res->status, cb_data->out_hdr->error);
        goto ret;
    }

    // TODO

ret:;
    struct snap_fs_dev_io_done_ctx *cb = cb_data->cb;
    mpool2_free(vnfs->p, cb_data);
    cb->cb(SNAP_FS_DEV_OP_SUCCESS, cb->user_arg);
}


int create(struct fuse_session *se, struct virtionfs *vnfs,
           struct fuse_in_header *in_hdr, struct fuse_create_in *in_create, const char *in_name,
           struct fuse_out_header *out_hdr, struct fuse_entry_out *out_entry, struct fuse_open_out *out_open,
           struct snap_fs_dev_io_done_ctx *cb)
{
    struct create_cb_data *cb_data = mpool2_alloc(vnfs->p);
    if (!cb_data) {
        out_hdr->error = -ENOMEM;
        return 0;
    }

    cb_data->cb = cb;
    cb_data->vnfs = vnfs;
    cb_data->out_hdr = out_hdr;
    cb_data->out_entry = out_entry;
    cb_data->out_open = out_open;

    COMPOUND4args args;
    nfs_argop4 op[4];
    memset(&args, 0, sizeof(args));
    args.argarray.argarray_len = sizeof(op) / sizeof(nfs_argop4);
    args.argarray.argarray_val = op;

    // PUTFH: parent
    // OPEN: create, attrs = flags, mode, usmask
    // GETATTR: out_entry
    // GETFH: out_open


    // PUTFH
    // Get the inode manually because we want the FH of the parent
    struct inode *i = inode_table_get(vnfs->inodes, in_hdr->nodeid);
    cb_data->i = i;
    if (!i) {
    	fprintf(stderr, "Invalid nodeid supplied to OPEN\n");
        mpool2_free(vnfs->p, cb_data);
        out_hdr->error = -ENOENT;
        return 0;
    }
    op[0].argop = OP_PUTFH;
    op[0].nfs_argop4_u.opputfh.object.nfs_fh4_val = i->parent->fh.val;
    op[0].nfs_argop4_u.opputfh.object.nfs_fh4_len = i->parent->fh.len;

    // OPEN
    op[1].argop = OP_OPEN;
    memset(&op[1].nfs_argop4_u.opopen, 0, sizeof(OPEN4args));
    // Windows share stuff, this means normal operation in UNIX world
    op[1].nfs_argop4_u.opopen.share_access = OPEN4_SHARE_ACCESS_BOTH;
    op[1].nfs_argop4_u.opopen.share_deny = OPEN4_SHARE_DENY_NONE;
    // Don't use this because we don't do anything special with the share, so set to zero
    op[1].nfs_argop4_u.opopen.seqid = 0;
    // Set the owner with the clientid and the unique owner number (32 bit should be safe)
    // The clientid stems from the setclientid() handshake
    op[1].nfs_argop4_u.opopen.owner.clientid = vnfs->clientid;
    // We store this so that the reference stays alive
    cb_data->owner_val = atomic_fetch_add(&vnfs->open_owner_counter, 1);
    op[1].nfs_argop4_u.opopen.owner.owner.owner_val = (char *) &cb_data->owner_val;
    op[1].nfs_argop4_u.opopen.owner.owner.owner_len = sizeof(cb_data->owner_val);
    // Tell the server we want to open the file in the current FH dir (aka the parent dir)
    // with the specified filename
    op[1].nfs_argop4_u.opopen.claim.claim = CLAIM_NULL;
    // TODO concurrency bug here if the parent gets changed underneath
    // Don't know yet how that could happen
    op[1].nfs_argop4_u.opopen.claim.open_claim4_u.file.utf8string_val = i->filename;
    op[1].nfs_argop4_u.opopen.claim.open_claim4_u.file.utf8string_len = strlen(i->filename);
    // Now we determine whether to CREATE or NOCREATE
    op[1].nfs_argop4_u.opopen.openhow.opentype = OPEN4_CREATE;
    op[1].nfs_argop4_u.opopen.openhow.openflag4_u.how.mode = UNCHECKED4;

    fattr4 *attr = &op[1].nfs_argop4_u.opopen.openhow.openflag4_u.how.createhow4_u.createattrs;
    // TODO this does a malloc, remove malloc it!
    nfs4_fill_create_attrs(in_hdr, in_create->mode, attr);

    // GETATTR
    nfs4_op_getattr(&op[2], standard_attributes, 2);
    // GETFH
    op[3].argop = OP_GETFH;

#ifdef LATENCY_MEASURING_ENABLED
    if (vnfs->nthreads == 1) {
        op_calls[FUSE_CREATE]++;
        ft_start(&ft[FUSE_CREATE]);
    }
#endif

    if (rpc_nfs4_compound_async(vnfs->rpc, create_cb, &args, cb_data) != 0) {
    	fprintf(stderr, "Failed to send NFS:OPEN (with create) request\n");
        mpool2_free(vnfs->p, cb_data);
        out_hdr->error = -EREMOTEIO;
        return 0;
    }

    return EWOULDBLOCK;
}

struct release_cb_data {
    struct snap_fs_dev_io_done_ctx *cb;
    struct virtionfs *vnfs;

    struct inode *i;

    struct fuse_out_header *out_hdr;
};

void release_cb(struct rpc_context *rpc, int status, void *data,
                       void *private_data) {
#ifdef LATENCY_MEASURING_ENABLED
    if (vnfs->nthreads == 1) {
        ft_stop(&ft[FUSE_RELEASE]);
    }
#endif
    struct release_cb_data *cb_data = (struct release_cb_data *)private_data;
    struct virtionfs *vnfs = cb_data->vnfs;
    COMPOUND4res *res = data;

    if (status != RPC_STATUS_SUCCESS) {
    	fprintf(stderr, "RPC with NFS:CLOSE unsuccessful: rpc error=%d\n", status);
        cb_data->out_hdr->error = -EREMOTEIO;
        goto ret;
    }
    if (res->status != NFS4_OK) {
        cb_data->out_hdr->error = -nfs_error_to_fuse_error(res->status);
    	fprintf(stderr, "NFS:CLOSE unsuccessful: nfs error=%d, fuse error=%d\n",
                res->status, cb_data->out_hdr->error);
        goto ret;
    }

    cb_data->i->fh_open.len = 0;

ret:;
    struct snap_fs_dev_io_done_ctx *cb = cb_data->cb;
    mpool2_free(vnfs->p, cb_data);
    cb->cb(SNAP_FS_DEV_OP_SUCCESS, cb->user_arg);
}


int release(struct fuse_session *se, struct virtionfs *vnfs,
           struct fuse_in_header *in_hdr, struct fuse_release_in *in_release,
           struct fuse_out_header *out_hdr,
           struct snap_fs_dev_io_done_ctx *cb)
{
    struct release_cb_data *cb_data = mpool2_alloc(vnfs->p);
    if (!cb_data) {
        out_hdr->error = -ENOMEM;
        return 0;
    }

    cb_data->cb = cb;
    cb_data->vnfs = vnfs;
    cb_data->out_hdr = out_hdr;

    COMPOUND4args args;
    nfs_argop4 op[2];
    memset(&args, 0, sizeof(args));
    args.argarray.argarray_len = sizeof(op) / sizeof(nfs_argop4);
    args.argarray.argarray_val = op;

    // PUTFH
    cb_data->i = vnfs4_op_putfh(vnfs, &op[0], in_hdr->nodeid);
    if (!cb_data->i) {
    	fprintf(stderr, "Invalid nodeid supplied to release\n");
        mpool2_free(vnfs->p, cb_data);
        out_hdr->error = -ENOENT;
        return 0;
    }
    // COMMIT
    op[1].argop = OP_CLOSE;
    op[1].nfs_argop4_u.opclose.seqid = 0;
    // Pass the stateid we received in the corresponding OPEN call
    op[1].nfs_argop4_u.opclose.open_stateid = cb_data->i->open_stateid;

    if (in_release->release_flags & FUSE_RELEASE_FLUSH) {
        // TODO
        fprintf(stderr, "a FUSE:release requested a flush, not implemented TODO\n");
    }

#ifdef LATENCY_MEASURING_ENABLED
    if (vnfs->nthreads == 1) {
        op_calls[FUSE_RELEASE]++;
        ft_start(&ft[FUSE_RELEASE]);
    }
#endif

    if (rpc_nfs4_compound_async(vnfs->rpc, release_cb, &args, cb_data) != 0) {
    	fprintf(stderr, "Failed to send NFS:CLOSE request\n");
        mpool2_free(vnfs->p, cb_data);
        out_hdr->error = -EREMOTEIO;
        return 0;
    }

    return EWOULDBLOCK;
}

struct fsync_cb_data {
    struct snap_fs_dev_io_done_ctx *cb;
    struct virtionfs *vnfs;

    struct fuse_out_header *out_hdr;
    struct fuse_statfs_out *stat;
};

void vfsync_cb(struct rpc_context *rpc, int status, void *data,
                       void *private_data) {
#ifdef LATENCY_MEASURING_ENABLED
    if (vnfs->nthreads == 1) {
        ft_stop(&ft[FUSE_FSYNC]);
    }
#endif
    struct fsync_cb_data *cb_data = (struct fsync_cb_data *)private_data;
    struct virtionfs *vnfs = cb_data->vnfs;
    COMPOUND4res *res = data;

    if (status != RPC_STATUS_SUCCESS) {
    	fprintf(stderr, "RPC with NFS:COMMIT unsuccessful: rpc error=%d\n", status);
        cb_data->out_hdr->error = -EREMOTEIO;
        goto ret;
    }
    if (res->status != NFS4_OK) {
        cb_data->out_hdr->error = -nfs_error_to_fuse_error(res->status);
    	fprintf(stderr, "NFS:COMMIT unsuccessful: nfs error=%d, fuse error=%d\n",
                res->status, cb_data->out_hdr->error);
        goto ret;
    }

ret:;
    struct snap_fs_dev_io_done_ctx *cb = cb_data->cb;
    mpool2_free(vnfs->p, cb_data);
    cb->cb(SNAP_FS_DEV_OP_SUCCESS, cb->user_arg);
}


// FUSE_FSYNC_FDATASYNC is not really adhered to, we always commit metadata
int vfsync(struct fuse_session *se, struct virtionfs *vnfs,
           struct fuse_in_header *in_hdr, struct fuse_fsync_in *in_fsync,
           struct fuse_out_header *out_hdr,
           struct snap_fs_dev_io_done_ctx *cb)
{
    struct fsync_cb_data *cb_data = mpool2_alloc(vnfs->p);
    if (!cb_data) {
        out_hdr->error = -ENOMEM;
        return 0;
    }

    cb_data->cb = cb;
    cb_data->vnfs = vnfs;
    cb_data->out_hdr = out_hdr;

    COMPOUND4args args;
    nfs_argop4 op[2];
    memset(&args, 0, sizeof(args));
    args.argarray.argarray_len = sizeof(op) / sizeof(nfs_argop4);
    args.argarray.argarray_val = op;

    // PUTFH
    struct inode *i = vnfs4_op_putfh(vnfs, &op[0], in_hdr->nodeid);
    if (!i) {
    	fprintf(stderr, "Invalid nodeid supplied to fsync\n");
        mpool2_free(vnfs->p, cb_data);
        out_hdr->error = -ENOENT;
        return 0;
    }
    // COMMIT
    op[1].argop = OP_COMMIT;
    // Fuse doesn't provide offset and count for us, so we commit
    // the whole file 🤷
    op[1].nfs_argop4_u.opcommit.offset = 0;
    op[1].nfs_argop4_u.opcommit.count = 0;

#ifdef LATENCY_MEASURING_ENABLED
    if (vnfs->nthreads == 1) {
        op_calls[FUSE_FSYNC]++;
        ft_start(&ft[FUSE_FSYNC]);
    }
#endif

    if (rpc_nfs4_compound_async(vnfs->rpc, vfsync_cb, &args, cb_data) != 0) {
    	fprintf(stderr, "Failed to send NFS:commit request\n");
        mpool2_free(vnfs->p, cb_data);
        out_hdr->error = -EREMOTEIO;
        return 0;
    }

    return EWOULDBLOCK;
}

struct write_cb_data {
    struct snap_fs_dev_io_done_ctx *cb;
    struct virtionfs *vnfs;

    struct fuse_write_in *in_write;
    struct iovec *in_iov;
    int *in_iovcnt;

    struct fuse_out_header *out_hdr;
    struct fuse_write_out *out_write;
};

void vwrite_cb(struct rpc_context *rpc, int status, void *data,
           void *private_data) {
#ifdef LATENCY_MEASURING_ENABLED
    if (vnfs->nthreads == 1) {
        ft_stop(&ft[FUSE_WRITE]);
    }
#endif
    struct write_cb_data *cb_data = (struct write_cb_data *) private_data;
    struct virtionfs *vnfs = cb_data->vnfs;
    COMPOUND4res *res = data;

    if (status != RPC_STATUS_SUCCESS) {
    	fprintf(stderr, "RPC with NFS:WRITE unsuccessful: rpc error=%d\n", status);
        cb_data->out_hdr->error = -EREMOTEIO;
        goto ret;
    }
    if (res->status != NFS4_OK) {
        cb_data->out_hdr->error = -nfs_error_to_fuse_error(res->status);
    	fprintf(stderr, "NFS:WRITE unsuccessful: nfs error=%d, fuse error=%d\n",
                res->status, cb_data->out_hdr->error);
        goto ret;
    }
    
    uint32_t written =  res->resarray.resarray_val[1].nfs_resop4_u.opwrite.WRITE4res_u.resok4.count;
    cb_data->out_write->size = written;

    cb_data->out_hdr->len += sizeof(*cb_data->out_write);

ret:;
    struct snap_fs_dev_io_done_ctx *cb = cb_data->cb;
    mpool2_free(vnfs->p, cb_data);
    cb->cb(SNAP_FS_DEV_OP_SUCCESS, cb->user_arg);
}

// NFS does not support I/O vectors and in the case of the host sending more than one iov
// this write implementation only send the first iov. 
// When the host receives the written len of just the first iov, it will retry with the others
// TLDR: Not efficient (but functional) with multiple I/O vectors!
int vwrite(struct fuse_session *se, struct virtionfs *vnfs,
         struct fuse_in_header *in_hdr, struct fuse_write_in *in_write,
         struct iovec *in_iov, int in_iov_cnt,
         struct fuse_out_header *out_hdr, struct fuse_write_out *out_write,
         struct snap_fs_dev_io_done_ctx *cb)
{
#ifdef DEBUG_ENABLED
    if (in_iov_cnt > 1)
        fprintf(stderr, "virtionfs:%s was called with >1 iovecs, this is not supported!\n",
                __func__);
#endif

    struct write_cb_data *cb_data = mpool2_alloc(vnfs->p);
    if (!cb_data) {
        out_hdr->error = -ENOMEM;
        return 0;
    }

    cb_data->cb = cb;
    cb_data->vnfs = vnfs;
    cb_data->in_write = in_write;
    cb_data->in_iov = in_iov;
    cb_data->out_hdr = out_hdr;
    cb_data->out_write = out_write;

    COMPOUND4args args;
    nfs_argop4 op[2];
    memset(&args, 0, sizeof(args));
    args.argarray.argarray_len = sizeof(op) / sizeof(nfs_argop4);
    args.argarray.argarray_val = op;

    // PUTFH
    struct inode *i = vnfs4_op_putfh_open(vnfs, &op[0], in_hdr->nodeid);
    if (!i) {
    	fprintf(stderr, "Invalid nodeid supplied to write\n");
        mpool2_free(vnfs->p, cb_data);
        out_hdr->error = -ENOENT;
        return 0;
    }
    // WRITE
    op[1].argop = OP_WRITE;
    op[1].nfs_argop4_u.opwrite.stateid = i->open_stateid;
    op[1].nfs_argop4_u.opwrite.offset = in_write->offset;
    op[1].nfs_argop4_u.opwrite.stable = UNSTABLE4;
    op[1].nfs_argop4_u.opwrite.data.data_val = in_iov->iov_base;
    op[1].nfs_argop4_u.opwrite.data.data_len = in_iov->iov_len;

    // libnfs by default allocates a buffer for the fully encoded NFS packet (rpc_pdu)
    // of sizeof(rpc header) + sizeof(COMPOUNF4args) + ZDR_ENCODEBUF_MINSIZE + alloc_hint
    // + rounding to 8 bytes twice
    // where ZDR_ENCODEBUF_MINSIZE is set to 4096 by libnfs
    // With alloc_hint=0 sending the RPC would fail.
    // Because this alloc_hint is really naive, the only way to safely make sure there
    // is enough buffer space, is to set the alloc_hint to our write buffer size...
    // This allocates way too much, but atleast it is safe
    uint64_t alloc_hint = in_iov->iov_len; 

#ifdef LATENCY_MEASURING_ENABLED
    if (vnfs->nthreads == 1) {
        op_calls[FUSE_WRITE]++;
        ft_start(&ft[FUSE_WRITE]);
    }
#endif
    if (rpc_nfs4_compound_async2(vnfs->rpc, vwrite_cb, &args, cb_data, alloc_hint) != 0) {
    	fprintf(stderr, "Failed to send NFS:write request\n");
        mpool2_free(vnfs->p, cb_data);
        out_hdr->error = -EREMOTEIO;
        return 0;
    }

    return EWOULDBLOCK;
}

#define MIN(x, y) x < y ? x : y

static size_t iovec_write_buf(struct iovec *iov, int iovcnt,
        void *buf, size_t size)
{
    int i = 0;
    size_t written = 0;
    while (written != size && i < iovcnt) {
        struct iovec *v = &iov[i];
        // copy all the remaining bytes or till the current end of iovec
        size_t to_cpy = MIN(size-written, v->iov_len);
        memcpy(v->iov_base, buf+written, to_cpy);

        // we hit the end of the iovec
        if (to_cpy == v->iov_len){
            // goto the next iovec
            i++;
        }
        written += to_cpy;
    }
    return written;
}

struct read_cb_data {
    struct snap_fs_dev_io_done_ctx *cb;
    struct fuse_session *se;
    struct virtionfs *vnfs;

    struct fuse_out_header *out_hdr;
    struct iovec *out_iov;
    int out_iovcnt;
};

void vread_cb(struct rpc_context *rpc, int status, void *data,
              void *private_data)
{
    struct read_cb_data *cb_data = (struct read_cb_data *)private_data;
    struct virtionfs *vnfs = cb_data->vnfs;
    COMPOUND4res *res = data;

    if (status != RPC_STATUS_SUCCESS) {
    	fprintf(stderr, "RPC with NFS:READ unsuccessful: rpc error=%d\n", status);
        cb_data->out_hdr->error = -EREMOTEIO;
        goto ret;
    }
    if (res->status != NFS4_OK) {
        cb_data->out_hdr->error = -nfs_error_to_fuse_error(res->status);
    	fprintf(stderr, "NFS:READ unsuccessful: nfs error=%d, fuse error=%d\n",
                res->status, cb_data->out_hdr->error);
        goto ret;
    }

    char *buf = res->resarray.resarray_val[1].nfs_resop4_u.opread.READ4res_u
                .resok4.data.data_val;
    uint32_t len = res->resarray.resarray_val[1].nfs_resop4_u.opread.READ4res_u
                   .resok4.data.data_len;
    // Fill the iov that we return to the host
    if (cb_data->out_iovcnt >= 1) {
        size_t written = iovec_write_buf(cb_data->out_iov, cb_data->out_iovcnt, buf, len);
        cb_data->out_hdr->len += written;
    }

ret:;
    struct snap_fs_dev_io_done_ctx *cb = cb_data->cb;
    mpool2_free(vnfs->p, cb_data);
    cb->cb(SNAP_FS_DEV_OP_SUCCESS, cb->user_arg);
}

int vread(struct fuse_session *se, struct virtionfs *vnfs,
         struct fuse_in_header *in_hdr, struct fuse_read_in *in_read,
         struct fuse_out_header *out_hdr, struct iovec *out_iov, int out_iovcnt,
         struct snap_fs_dev_io_done_ctx *cb) {
    struct read_cb_data *cb_data = mpool2_alloc(vnfs->p);
    if (!cb_data) {
        out_hdr->error = -ENOMEM;
        return 0;
    }

    cb_data->cb = cb;
    cb_data->se = se;
    cb_data->vnfs = vnfs;
    cb_data->out_hdr = out_hdr;
    cb_data->out_iov = out_iov;
    cb_data->out_iovcnt = out_iovcnt;

    COMPOUND4args args;
    nfs_argop4 op[2];
    memset(&args, 0, sizeof(args));
    args.argarray.argarray_len = sizeof(op) / sizeof(nfs_argop4);
    args.argarray.argarray_val = op;

    // PUTFH
    struct inode *i = vnfs4_op_putfh_open(vnfs, &op[0], in_hdr->nodeid);
    if (!i) {
    	fprintf(stderr, "Invalid nodeid supplied to open\n");
        mpool2_free(vnfs->p, cb_data);
        out_hdr->error = -ENOENT;
        return 0;
    }
    // OPEN
    op[1].argop = OP_READ;
    op[1].nfs_argop4_u.opread.stateid = i->open_stateid;
    op[1].nfs_argop4_u.opread.count = in_read->size;
    op[1].nfs_argop4_u.opread.offset = in_read->offset;

    if (rpc_nfs4_compound_async(vnfs->rpc, vread_cb, &args, cb_data) != 0) {
    	fprintf(stderr, "Failed to send NFS:READ request\n");
        mpool2_free(vnfs->p, cb_data);
        out_hdr->error = -EREMOTEIO;
        return 0;
    }

    return EWOULDBLOCK;
}

struct open_cb_data {
    struct snap_fs_dev_io_done_ctx *cb;
    struct virtionfs *vnfs;

    struct inode *i;

    struct fuse_out_header *out_hdr;
    struct fuse_open_out *out_open;

    uint32_t owner_val;
};

void vopen_confirm_cb(struct rpc_context *rpc, int status, void *data,
                       void *private_data)
{
#ifdef LATENCY_MEASURING_ENABLED
    if (vnfs->nthreads == 1) {
        // If the confirm was needed, then the timer wasn't stopped yet
        ft_stop(&ft[FUSE_OPEN]);
    }
#endif
    struct open_cb_data *cb_data = (struct open_cb_data *)private_data;
    struct virtionfs *vnfs = cb_data->vnfs;
    COMPOUND4res *res = data;

    if (status != RPC_STATUS_SUCCESS) {
    	fprintf(stderr, "RPC with NFS:OPEN_CONFIRM unsuccessful: rpc error=%d\n", status);
        cb_data->out_hdr->error = -EREMOTEIO;
        goto ret;
    }
    if (res->status != NFS4_OK) {
        cb_data->out_hdr->error = -nfs_error_to_fuse_error(res->status);
    	fprintf(stderr, "NFS:OPEN_CONFIRM unsuccessful: nfs error=%d, fuse error=%d\n",
                res->status, cb_data->out_hdr->error);
        goto ret;
    }
    OPEN_CONFIRM4resok *ok = &res->resarray.resarray_val[1].nfs_resop4_u.opopen_confirm
                             .OPEN_CONFIRM4res_u.resok4;
    cb_data->i->open_stateid = ok->open_stateid;

ret:;
    struct snap_fs_dev_io_done_ctx *cb = cb_data->cb;
    mpool2_free(vnfs->p, cb_data);
    cb->cb(SNAP_FS_DEV_OP_SUCCESS, cb->user_arg);
}

void vopen_cb(struct rpc_context *rpc, int status, void *data,
              void *private_data)
{
    struct open_cb_data *cb_data = (struct open_cb_data *)private_data;
    struct virtionfs *vnfs = cb_data->vnfs;
    COMPOUND4res *res = data;

    if (status != RPC_STATUS_SUCCESS) {
    	fprintf(stderr, "RPC with NFS:OPEN unsuccessful: rpc error=%d\n", status);
        cb_data->out_hdr->error = -EREMOTEIO;
        goto ret;
    }
    if (res->status != NFS4_OK) {
        cb_data->out_hdr->error = -nfs_error_to_fuse_error(res->status);
    	fprintf(stderr, "NFS:OPEN unsuccessful: nfs error=%d, fuse error=%d\n",
                res->status, cb_data->out_hdr->error);
        goto ret;
    }

    struct inode *i = cb_data->i;

    // Store the FH from OPEN as the read write FH in the inode
    // The read-write FH is not the same (can't assume) as the metadata FH
    nfs_fh4 *fh = &res->resarray.resarray_val[2].nfs_resop4_u.opgetfh.GETFH4res_u.resok4.object;
    int ret = nfs4_clone_fh(&i->fh_open, fh);
    if (ret < 0) {
        cb_data->out_hdr->error = ret;
        goto ret;
    }

    // We don't use the FUSE:fh, nor do we have any open flags at the moment
    // so set the out_open to zeros
    memset(cb_data->out_open, 0, sizeof(*cb_data->out_open));
    cb_data->out_hdr->len += sizeof(*cb_data->out_open);
    OPEN4resok *openok = &res->resarray.resarray_val[1].nfs_resop4_u.opopen.OPEN4res_u.resok4;
    if (!(openok->rflags & OPEN4_RESULT_CONFIRM)) {
        // Save the stateid we were given for the opened handle
        i->open_stateid = openok->stateid;
#ifdef LATENCY_MEASURING_ENABLED
        if (vnfs->nthreads == 1) {
            // Only now we can conclude that the OPEN is done
            ft_stop(&ft[FUSE_OPEN]);
        }
#endif
        goto ret;
    }

    // We will have to send a OPEN_CONFIRM
    COMPOUND4args args;
    nfs_argop4 op[2];
    memset(&args, 0, sizeof(args));
    args.argarray.argarray_len = sizeof(op) / sizeof(nfs_argop4);
    args.argarray.argarray_val = op;

    op[0].argop = OP_PUTFH;
    op[0].nfs_argop4_u.opputfh.object.nfs_fh4_val = i->fh_open.val;
    op[0].nfs_argop4_u.opputfh.object.nfs_fh4_len = i->fh_open.len;

    op[1].argop = OP_OPEN_CONFIRM;
    // Give back the same stateid as we received in the OPEN result
    op[1].nfs_argop4_u.opopen_confirm.open_stateid = openok->stateid;
    op[1].nfs_argop4_u.opopen_confirm.seqid = 0;
    if (rpc_nfs4_compound_async(vnfs->rpc, vopen_confirm_cb, &args, cb_data) != 0) {
    	fprintf(stderr, "Failed to send NFS:open_confirm request\n");
        cb_data->out_hdr->error = -EREMOTEIO;
        goto ret;
    }
    return;

ret:;
    struct snap_fs_dev_io_done_ctx *cb = cb_data->cb;
    mpool2_free(vnfs->p, cb_data);
    cb->cb(SNAP_FS_DEV_OP_SUCCESS, cb->user_arg);
}

int vopen(struct fuse_session *se, struct virtionfs *vnfs,
         struct fuse_in_header *in_hdr, struct fuse_open_in *in_open,
         struct fuse_out_header *out_hdr, struct fuse_open_out *out_open,
         struct snap_fs_dev_io_done_ctx *cb)
{
    struct open_cb_data *cb_data = mpool2_alloc(vnfs->p);
    if (!cb_data) {
        out_hdr->error = -ENOMEM;
        return 0;
    }

    cb_data->cb = cb;
    cb_data->vnfs = vnfs;
    cb_data->out_hdr = out_hdr;
    cb_data->out_open = out_open;

    COMPOUND4args args;
    nfs_argop4 op[3];
    memset(&args, 0, sizeof(args));
    args.argarray.argarray_len = sizeof(op) / sizeof(nfs_argop4);
    args.argarray.argarray_val = op;

    // PUTFH
    // Get the inode manually because we want the FH of the parent
    struct inode *i = inode_table_get(vnfs->inodes, in_hdr->nodeid);
    cb_data->i = i;
    if (!i) {
    	fprintf(stderr, "Invalid nodeid supplied to OPEN\n");
        mpool2_free(vnfs->p, cb_data);
        out_hdr->error = -ENOENT;
        return 0;
    }
    op[0].argop = OP_PUTFH;
    op[0].nfs_argop4_u.opputfh.object.nfs_fh4_val = i->parent->fh.val;
    op[0].nfs_argop4_u.opputfh.object.nfs_fh4_len = i->parent->fh.len;

    // OPEN
    op[1].argop = OP_OPEN;
    memset(&op[1].nfs_argop4_u.opopen, 0, sizeof(OPEN4args));
    // Windows share stuff, this means normal operation in UNIX world
    op[1].nfs_argop4_u.opopen.share_access = OPEN4_SHARE_ACCESS_BOTH;
    op[1].nfs_argop4_u.opopen.share_deny = OPEN4_SHARE_DENY_NONE;
    // Don't use this because we don't do anything special with the share, so set to zero
    op[1].nfs_argop4_u.opopen.seqid = 0;
    // Set the owner with the clientid and the unique owner number (32 bit should be safe)
    // The clientid stems from the setclientid() handshake
    op[1].nfs_argop4_u.opopen.owner.clientid = vnfs->clientid;
    // We store this so that the reference stays alive
    cb_data->owner_val = atomic_fetch_add(&vnfs->open_owner_counter, 1);
    op[1].nfs_argop4_u.opopen.owner.owner.owner_val = (char *) &cb_data->owner_val;
    op[1].nfs_argop4_u.opopen.owner.owner.owner_len = sizeof(cb_data->owner_val);
    // Tell the server we want to open the file in the current FH dir (aka the parent dir)
    // with the specified filename
    op[1].nfs_argop4_u.opopen.claim.claim = CLAIM_NULL;
    // TODO concurrency bug here if the parent gets changed underneath
    // Don't know yet how that could happen
    op[1].nfs_argop4_u.opopen.claim.open_claim4_u.file.utf8string_val = i->filename;
    op[1].nfs_argop4_u.opopen.claim.open_claim4_u.file.utf8string_len = strlen(i->filename);
    // Now we determine whether to CREATE or NOCREATE
    assert(!(in_open->flags & O_CREAT));
    op[1].nfs_argop4_u.opopen.openhow.opentype = OPEN4_NOCREATE;

    // GETFH
    op[2].argop = OP_GETFH;

#ifdef LATENCY_MEASURING_ENABLED
    if (vnfs->nthreads == 1) {
        op_calls[FUSE_OPEN]++;
        ft_start(&ft[FUSE_OPEN]);
    }
#endif
    if (rpc_nfs4_compound_async(vnfs->rpc, vopen_cb, &args, cb_data) != 0) {
    	fprintf(stderr, "Failed to send NFS:open request\n");
        mpool2_free(vnfs->p, cb_data);
        out_hdr->error = -EREMOTEIO;
        return 0;
    }

    return EWOULDBLOCK;
}

struct setattr_cb_data {
    struct snap_fs_dev_io_done_ctx *cb;
    struct virtionfs *vnfs;
    struct fuse_session *se;

    struct fuse_out_header *out_hdr;
    struct fuse_attr_out *out_attr;

    uint64_t *bitmap;
    char *attrlist;
};

void setattr_cb(struct rpc_context *rpc, int status, void *data,
                       void *private_data)
{
#ifdef LATENCY_MEASURING_ENABLED
    if (vnfs->nthreads == 1) {
        ft_stop(&ft[FUSE_SETATTR]);
    {
#endif
    struct setattr_cb_data *cb_data = (struct setattr_cb_data *)private_data;
    struct virtionfs *vnfs = cb_data->vnfs;
    COMPOUND4res *res = data;

    if (status != RPC_STATUS_SUCCESS) {
    	fprintf(stderr, "RPC with NFS:SETATTR unsuccessful: rpc error=%d\n", status);
        cb_data->out_hdr->error = -EREMOTEIO;
        goto ret;
    }
    if (res->status != NFS4_OK) {
        cb_data->out_hdr->error = -nfs_error_to_fuse_error(res->status);
    	fprintf(stderr, "NFS:SETATTR unsuccessful: nfs error=%d, fuse error=%d\n",
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
        cb_data->out_hdr->len += cb_data->se->conn.proto_minor < 9 ?
            FUSE_COMPAT_ATTR_OUT_SIZE : sizeof(*cb_data->out_attr);
    } else {
        cb_data->out_hdr->error = -EREMOTEIO;
    }

ret:;
    free(cb_data->bitmap);
    free(cb_data->attrlist);
    struct snap_fs_dev_io_done_ctx *cb = cb_data->cb;
    mpool2_free(vnfs->p, cb_data);
    cb->cb(SNAP_FS_DEV_OP_SUCCESS, cb->user_arg);
}

int setattr(struct fuse_session *se, struct virtionfs *vnfs,
            struct fuse_in_header *in_hdr, struct fuse_setattr_in *in_setattr,
            struct fuse_out_header *out_hdr, struct fuse_attr_out *out_attr,
            struct snap_fs_dev_io_done_ctx *cb)
{
    struct setattr_cb_data *cb_data = mpool2_alloc(vnfs->p);
    if (!cb_data) {
        out_hdr->error = -ENOMEM;
        return 0;
    }

    cb_data->cb = cb;
    cb_data->vnfs = vnfs;
    cb_data->se = se;
    cb_data->out_hdr = out_hdr;
    cb_data->out_attr = out_attr;

    COMPOUND4args args;
    nfs_argop4 op[4];
    memset(&args, 0, sizeof(args));
    args.argarray.argarray_len = sizeof(op) / sizeof(nfs_argop4);
    args.argarray.argarray_val = op;

    struct inode *i = vnfs4_op_putfh(vnfs, &op[0], in_hdr->nodeid);
    if (!i) {
    	fprintf(stderr, "Invalid nodeid supplied to GETATTR\n");
        mpool2_free(vnfs->p, cb_data);
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

    uint64_t *bitmap = mpool2_alloc(vnfs->p);
    bitmap4 attrsmask;
    attrsmask.bitmap4_len = sizeof(*bitmap);
    attrsmask.bitmap4_val = (uint32_t *) bitmap;

    size_t attrlist_len = 0;
    if (in_setattr->valid & FUSE_SET_ATTR_MODE) {
        *bitmap |= (1UL << FATTR4_MODE);
        attrlist_len += 4;
    }
    if (in_setattr->valid & FUSE_SET_ATTR_SIZE) {
        *bitmap |= (1UL << FATTR4_SIZE);
    }
    char *attrlist = malloc(attrlist_len);

    if (in_setattr->valid & FUSE_SET_ATTR_MODE) {
        *(uint32_t *) attrlist = htonl(in_setattr->mode);
        attrlist += 4;
    }
    if (in_setattr->valid & FUSE_SET_ATTR_SIZE) {
        *(uint32_t *) attrlist = nfs_hton64(in_setattr->size);
        attrlist += 8;
    }

    op[1].nfs_argop4_u.opsetattr.obj_attributes.attrmask = attrsmask;
    op[1].nfs_argop4_u.opsetattr.obj_attributes.attr_vals.attrlist4_val = attrlist;
    op[1].nfs_argop4_u.opsetattr.obj_attributes.attr_vals.attrlist4_len = attrlist_len;
    cb_data->bitmap = bitmap;
    cb_data->attrlist = attrlist;

    nfs4_op_getattr(&op[2], standard_attributes, 2);

#ifdef LATENCY_MEASURING_ENABLED
    if (vnfs->nthreads == 1) {
        op_calls[FUSE_SETATTR]++;
        ft_start(&ft[FUSE_SETATTR]);
    }
#endif
    if (rpc_nfs4_compound_async(vnfs->rpc, setattr_cb, &args, cb_data) != 0) {
    	fprintf(stderr, "Failed to send nfs4 SETATTR request\n");
        mpool2_free(vnfs->p, cb_data);
        out_hdr->error = -EREMOTEIO;
        return 0;
    }

    return EWOULDBLOCK;
}

struct statfs_cb_data {
    struct snap_fs_dev_io_done_ctx *cb;
    struct virtionfs *vnfs;
    struct fuse_session *se;

    struct fuse_out_header *out_hdr;
    struct fuse_statfs_out *out_statfs;
};

void statfs_cb(struct rpc_context *rpc, int status, void *data,
                       void *private_data) {
#ifdef LATENCY_MEASURING_ENABLED
    if (vnfs->nthreads == 1) {
        ft_stop(&ft[FUSE_STATFS]);
    }
#endif
    struct statfs_cb_data *cb_data = (struct statfs_cb_data *)private_data;
    struct virtionfs *vnfs = cb_data->vnfs;
    COMPOUND4res *res = data;

    if (status != RPC_STATUS_SUCCESS) {
    	fprintf(stderr, "NFS RPC for FUSE:statfs unsuccessful: rpc error=%d\n", status);
        cb_data->out_hdr->error = -EREMOTEIO;
        goto ret;
    }
    if (res->status != NFS4_OK) {
        cb_data->out_hdr->error = -nfs_error_to_fuse_error(res->status);
    	fprintf(stderr, "NFS:GETATTR for FUSE:statfs unsuccessful: nfs error=%d, fuse error=%d\n",
                res->status, cb_data->out_hdr->error);
        goto ret;
    }

    GETATTR4resok *resok = &res->resarray.resarray_val[1].nfs_resop4_u.opgetattr.GETATTR4res_u.resok4;
    char *attrs = resok->obj_attributes.attr_vals.attrlist4_val;
    u_int attrs_len = resok->obj_attributes.attr_vals.attrlist4_len;
    if (nfs_parse_statfs(&cb_data->out_statfs->st, attrs, attrs_len) != 0) {
        cb_data->out_hdr->error = -EREMOTEIO;
    }
    cb_data->out_hdr->len = cb_data->se->conn.proto_minor < 4 ?
        FUSE_COMPAT_STATFS_SIZE : sizeof(*cb_data->out_statfs);

ret:;
    struct snap_fs_dev_io_done_ctx *cb = cb_data->cb;
    mpool2_free(vnfs->p, cb_data);
    cb->cb(SNAP_FS_DEV_OP_SUCCESS, cb->user_arg);
}


int statfs(struct fuse_session *se, struct virtionfs *vnfs,
           struct fuse_in_header *in_hdr,
           struct fuse_out_header *out_hdr, struct fuse_statfs_out *stat,
           struct snap_fs_dev_io_done_ctx *cb)
{
    struct statfs_cb_data *cb_data = mpool2_alloc(vnfs->p);
    if (!cb_data) {
        out_hdr->error = -ENOMEM;
        return 0;
    }

    cb_data->cb = cb;
    cb_data->vnfs = vnfs;
    cb_data->out_hdr = out_hdr;
    cb_data->out_statfs = stat;

    COMPOUND4args args;
    nfs_argop4 op[2];

    // PUTFH the root
    op[0].argop = OP_PUTFH;
    struct inode *rooti = inode_table_get(vnfs->inodes, FUSE_ROOT_ID);
    op[0].nfs_argop4_u.opputfh.object.nfs_fh4_val = rooti->fh.val;
    op[0].nfs_argop4_u.opputfh.object.nfs_fh4_len = rooti->fh.len;
    // GETATTR statfs attributes
    nfs4_op_getattr(&op[1], statfs_attributes, 2);

    memset(&args, 0, sizeof(args));
    args.argarray.argarray_len = sizeof(op) / sizeof(nfs_argop4);
    args.argarray.argarray_val = op;

#ifdef LATENCY_MEASURING_ENABLED
    if (vnfs->nthreads == 1) {
        op_calls[FUSE_STATFS]++;
        ft_start(&ft[FUSE_STATFS]);
    }
#endif

    if (rpc_nfs4_compound_async(vnfs->rpc, statfs_cb, &args, cb_data) != 0) {
    	fprintf(stderr, "Failed to send FUSE:statfs request\n");
        mpool2_free(vnfs->p, cb_data);
        out_hdr->error = -EREMOTEIO;
        return 0;
    }

    return EWOULDBLOCK;
}

struct lookup_cb_data {
    struct snap_fs_dev_io_done_ctx *cb;
    struct virtionfs *vnfs;
    struct fuse_session *se;

    char *in_name;
    struct inode *parent_inode;

    struct fuse_out_header *out_hdr;
    struct fuse_entry_out *out_entry;
};

void lookup_cb(struct rpc_context *rpc, int status, void *data,
                       void *private_data) {
#ifdef LATENCY_MEASURING_ENABLED
    if (vnfs->nthreads == 1) {
        ft_stop(&ft[FUSE_LOOKUP]);
    }
#endif
    struct lookup_cb_data *cb_data = (struct lookup_cb_data *)private_data;
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

    struct inode *i = inode_table_getsert(vnfs->inodes, fileid, cb_data->in_name, cb_data->parent_inode);
    if (!i) {
        fprintf(stderr, "Couldn't getsert inode with fileid: %lu\n", fileid);
        cb_data->out_hdr->error = -ENOMEM;
        goto ret;
    }
    atomic_fetch_add(&i->nlookup, 1);
    cb_data->out_entry->generation = i->generation;

    if (i->fh.len == 0) {
        // Retreive the FH from the res and set it in the inode
        // it's stored in the inode for later use ex. getattr when it uses the nodeid
        int ret = nfs4_clone_fh(&i->fh, &res->resarray.resarray_val[3].nfs_resop4_u.opgetfh.GETFH4res_u.resok4.object);
        if (ret < 0) {
            fprintf(stderr, "Couldn't clone fh with fileid: %lu\n", fileid);
            cb_data->out_hdr->error = -ENOMEM;
            goto ret;
        }
    }
    cb_data->out_hdr->len += cb_data->se->conn.proto_minor < 9 ?
        FUSE_COMPAT_ENTRY_OUT_SIZE : sizeof(*cb_data->out_entry);

ret:;
    struct snap_fs_dev_io_done_ctx *cb = cb_data->cb;
    mpool2_free(vnfs->p, cb_data);
    cb->cb(SNAP_FS_DEV_OP_SUCCESS, cb->user_arg);
}

int lookup(struct fuse_session *se, struct virtionfs *vnfs,
           struct fuse_in_header *in_hdr, char *in_name,
           struct fuse_out_header *out_hdr, struct fuse_entry_out *out_entry,
           struct snap_fs_dev_io_done_ctx *cb)
{
    struct lookup_cb_data *cb_data = mpool2_alloc(vnfs->p);
    if (!cb_data) {
        out_hdr->error = -ENOMEM;
        return 0;
    }

    cb_data->cb = cb;
    cb_data->vnfs = vnfs;
    cb_data->se = se;
    cb_data->out_hdr = out_hdr;
    cb_data->out_entry = out_entry;

    COMPOUND4args args;
    nfs_argop4 op[4];

    // PUTFH
    struct inode *pi = vnfs4_op_putfh(vnfs, &op[0], in_hdr->nodeid);
    if (!pi) {
    	fprintf(stderr, "Invalid nodeid supplied to LOOKUP\n");
        mpool2_free(vnfs->p, cb_data);
        out_hdr->error = -ENOENT;
        return 0;
    }
    // Store these two for later use when creating the inode
    cb_data->parent_inode = pi;
    cb_data->in_name = in_name;
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
    if (vnfs->nthreads == 1) {
        op_calls[FUSE_LOOKUP]++;
        ft_start(&ft[FUSE_LOOKUP]);
    }
#endif

    if (rpc_nfs4_compound_async(vnfs->rpc, lookup_cb, &args, cb_data) != 0) {
    	fprintf(stderr, "Failed to send nfs4 LOOKUP request\n");
        mpool2_free(vnfs->p, cb_data);
        out_hdr->error = -EREMOTEIO;
        return 0;
    }

    return EWOULDBLOCK;
}

struct getattr_cb_data {
    struct snap_fs_dev_io_done_ctx *cb;
    struct virtionfs *vnfs;
    struct fuse_session *se;

    struct fuse_out_header *out_hdr;
    struct fuse_attr_out *out_attr;
};

void getattr_cb(struct rpc_context *rpc, int status, void *data,
                       void *private_data) {
#ifdef LATENCY_MEASURING_ENABLED
    if (vnfs->nthreads == 1) {
        ft_stop(&ft[FUSE_GETATTR]);
    }
#endif
    struct getattr_cb_data *cb_data = (struct getattr_cb_data *)private_data;
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
        cb_data->out_hdr->len += cb_data->se->conn.proto_minor < 9 ?
            FUSE_COMPAT_ATTR_OUT_SIZE : sizeof(*cb_data->out_attr);
    } else {
        cb_data->out_hdr->error = -EREMOTEIO;
    }

ret:;
    struct snap_fs_dev_io_done_ctx *cb = cb_data->cb;
    mpool2_free(vnfs->p, cb_data);
    cb->cb(SNAP_FS_DEV_OP_SUCCESS, cb->user_arg);
}

int getattr(struct fuse_session *se, struct virtionfs *vnfs,
            struct fuse_in_header *in_hdr, struct fuse_getattr_in *in_getattr,
            struct fuse_out_header *out_hdr, struct fuse_attr_out *out_attr,
            struct snap_fs_dev_io_done_ctx *cb)
{
    struct vnfs_conn *conn = vnfs_get_conn(vnfs);
    struct getattr_cb_data *cb_data = mpool2_alloc(vnfs->p);
    if (!cb_data) {
        out_hdr->error = -ENOMEM;
        return 0;
    }

    cb_data->cb = cb;
    cb_data->vnfs = vnfs;
    cb_data->se = se;
    cb_data->out_hdr = out_hdr;
    cb_data->out_attr = out_attr;

    COMPOUND4args args;
    nfs_argop4 op[3];

    vnfs4_op_sequence(&op[0], conn, false);
    struct inode *i = vnfs4_op_putfh(vnfs, &op[1], in_hdr->nodeid);
    if (!i) {
    	fprintf(stderr, "Invalid nodeid supplied to GETATTR\n");
        mpool2_free(vnfs->p, cb_data);
        out_hdr->error = -ENOENT;
        return 0;
    }
    nfs4_op_getattr(&op[2], standard_attributes, 2);
    
    memset(&args, 0, sizeof(args));
    args.argarray.argarray_len = sizeof(op) / sizeof(nfs_argop4);
    args.argarray.argarray_val = op;

#ifdef LATENCY_MEASURING_ENABLED
    if (vnfs->nthreads == 1) {
        op_calls[FUSE_GETATTR]++;
        ft_start(&ft[FUSE_GETATTR]);
    }
#endif
    if (rpc_nfs4_compound_async(vnfs->rpc, getattr_cb, &args, cb_data) != 0) {
    	fprintf(stderr, "Failed to send nfs4 GETATTR request\n");
        mpool2_free(vnfs->p, cb_data);
        out_hdr->error = -EREMOTEIO;
        return 0;
    }

    return EWOULDBLOCK;
}
int destroy(struct fuse_session *se, struct virtionfs *vnfs,
            struct fuse_in_header *in_hdr,
            struct fuse_out_header *out_hdr,
            struct snap_fs_dev_io_done_ctx *cb)
{
#ifdef LATENCY_MEASURING_ENABLED
    if (vnfs->nthreads == 1) {
        for (int i = 1; i <= FUSE_REMOVEMAPPING; i++) {
            if (ft[i].running) {
                fprintf(stderr, "OP(%d) timer is still running!?\n", i);
            }
            if (op_calls[i] > 0)
                printf("OP(%d) took %lu averaged over %lu calls\n", i, ft_get_nsec(&ft[i]) / op_calls[i], op_calls[i]);
        }
    }
#endif

    // TODO Destroy all the connections

    return 0;
}

int init(struct fuse_session *se, struct virtionfs *vnfs,
    struct fuse_in_header *in_hdr, struct fuse_init_in *in_init,
    struct fuse_conn_info *conn, struct fuse_out_header *out_hdr)
{
#ifdef LATENCY_MEASURING_ENABLED
    if (vnfs->nthreads == 1) {
        for (int i = 0; i < FUSE_REMOVEMAPPING+1; i++) {
            ft_init(&ft[i]);
            op_calls[i] = 0;
        }
    } else {
        fprintf(stderr, "Latency measuring is enabled but vnfs was told to do multithreading"
                ", this is not supported. Latency measuring disabled\n");
    }
#endif

    // TODO reenable when implementing flock()
    // if (conn->capable & FUSE_CAP_FLOCK_LOCKS)
    //     conn->want |= FUSE_CAP_FLOCK_LOCKS;

    // FUSE_CAP_SPLICE_READ is enabled in libfuse3 by default,
    // see do_init() in in fuse_lowlevel.c
    // We do not want this as splicing is not a thing with virtiofs
    conn->want &= ~FUSE_CAP_SPLICE_READ;
    conn->want &= ~FUSE_CAP_SPLICE_WRITE;

    // TODO FUSE:init always supplies uid=0 and gid=0,
    // so only setting the uid and gid once in the init is not sufficient
    // as in subsoquent operations different uid and gids can be supplied
    // however changing the uid and gid for every operations is very inefficient in libnfs
    // NOTE: the root permissions only properly work if the server has no_root_squash
    printf("%s, all NFS operations will go through uid %d and gid %d\n", __func__, vnfs->init_uid, vnfs->init_gid);
    vnfs->init_uid = in_hdr->uid;
    vnfs->init_gid = in_hdr->gid;

    vnfs_new_connection(vnfs);

    // TODO WARNING
    // By returning 0, we allow the host to imediately start sending us requests,
    // even though the lookup_true_rootfh or setclientid might not be done yet
    // or even fail!
    // This introduces a race condition, where if the rootfh is not found yet
    // or there is no clientid virtionfs will crash horribly
    return 0;
}

void virtionfs_assign_ops(struct fuse_ll_operations *ops) {
    ops->init = (typeof(ops->init)) init;
    ops->lookup = (typeof(ops->lookup)) lookup;
    ops->getattr = (typeof(ops->getattr)) getattr;
    // NFS accepts the NFS:fh (received from a NFS:lookup==FUSE:lookup) as
    // its parameter to the dir ops like readdir
    ops->opendir = NULL;
    ops->open = (typeof(ops->open)) vopen;
    ops->read = (typeof(ops->read)) vread;
    ops->write = (typeof(ops->write)) vwrite;
    ops->fsync = (typeof(ops->fsync)) vfsync;
    ops->release = (typeof(ops->release)) release;
    // NFS only does fsync(aka COMMIT) on files
    ops->fsyncdir = NULL;
    // The concept of flushing
    ops->flush = NULL;
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
    vnfs->nthreads = nthreads;

    vnfs->p = calloc(1, sizeof(struct mpool2));
    if (!vnfs->p) {
        warn("Failed to init virtionfs");
        // TODO fix return
        goto ret_b;
    }
    if (mpool2_init(vnfs->p, sizeof(struct lookup_cb_data), 256) < 0) {
        warn("Failed to init virtionfs");
        // TODO fix return
        goto ret_b;
    }

    vnfs->conns = calloc(vnfs->nthreads, sizeof(struct vnfs_conn));

    vnfs->nfs = nfs_init_context();
    if (vnfs->nfs == NULL) {
        warn("Failed to init nfs context\n");
        goto ret_a;
    }
    nfs_set_version(vnfs->nfs, NFS_V4);
    vnfs->rpc = nfs_get_rpc_context(vnfs->nfs);

    vnfs->inodes = calloc(1, sizeof(struct inode_table));
    if (!vnfs->inodes) {
        warn("Failed to init virtionfs");
        goto ret_b;
    }
    if (inode_table_init(vnfs->inodes) < 0) {
        warn("Failed to init virtionfs");
        goto ret_b;
    }

    struct fuse_ll_operations ops;
    memset(&ops, 0, sizeof(ops));
    virtionfs_assign_ops(&ops);

    virtiofs_emu_fuse_ll_main(&ops, emu_params, vnfs, debug);
    printf("nfsclient finished\n");

    inode_table_destroy(vnfs->inodes);
ret_b:
    nfs_destroy_context(vnfs->nfs);
ret_a:
    free(vnfs);
}
