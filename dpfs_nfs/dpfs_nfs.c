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
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "dpfs_fuse.h"
#include "dpfs_nfs.h"
#include "vnfs_connect.h"
#include "mpool.h"
#include "nfs_v4.h"
#include "inode.h"

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

#ifndef VNFS_NULLDEV
static uint32_t statfs_attributes[2] = {
        (1 << FATTR4_FILES_FREE |
         1 << FATTR4_FILES_TOTAL |
         1 << FATTR4_MAXNAME),
        (1 << (FATTR4_SPACE_AVAIL - 32) |
         1 << (FATTR4_SPACE_FREE - 32) |
         1 << (FATTR4_SPACE_TOTAL - 32))
};
#endif

#ifdef VNFS_NULLDEV
// supported_attributes = standard_attributes | statfs_attributes
#else
// supported_attributes = standard_attributes
#endif

#ifdef LATENCY_MEASURING_ENABLED
#define LATENCY_MEASURING_START(op) \
    do { \
        cb_data->conn->op_calls[FUSE_##op]++; \
        ft_init(&cb_data->ft); \
        ft_start(&cb_data->ft); \
    } while (0)

#define LATENCY_MEASURING_STOP(op) \
    do { \
        ft_stop(&cb_data->ft); \
        ft_add(&cb_data->conn->ft[FUSE_##op], &cb_data->ft); \
    } while (0)
#else 
#define LATENCY_MEASURING_START(op) do {} while(0)
#define LATENCY_MEASURING_STOP(op) do {} while(0)
#endif

// All the cb_data structs, nice and cozy together
struct getattr_cb_data {
    uint16_t thread_id;
    void *completion_context;
    struct virtionfs *vnfs;
    struct vnfs_conn *conn;
    uint32_t slotid;

#ifdef LATENCY_MEASURING_ENABLED
    struct ftimer ft;
#endif
#ifdef VNFS_NULLDEV
    struct inode *i;
#endif

    struct fuse_session *se;
    struct fuse_out_header *out_hdr;
    struct fuse_attr_out *out_attr;
};
struct lookup_cb_data {
    uint16_t thread_id;
    void *completion_context;
    struct virtionfs *vnfs;
    struct vnfs_conn *conn;
    uint32_t slotid;

#ifdef LATENCY_MEASURING_ENABLED
    struct ftimer ft;
#endif

    struct fuse_session *se;
    struct fuse_out_header *out_hdr;
    struct fuse_entry_out *out_entry;
};
struct statfs_cb_data {
    uint16_t thread_id;
    void *completion_context;
    struct virtionfs *vnfs;
    struct vnfs_conn *conn;
    uint32_t slotid;

#ifdef LATENCY_MEASURING_ENABLED
    struct ftimer ft;
#endif

    struct fuse_session *se;
    struct fuse_out_header *out_hdr;
    struct fuse_statfs_out *out_statfs;
};
struct setattr_cb_data {
    uint16_t thread_id;
    void *completion_context;
    struct virtionfs *vnfs;
    struct vnfs_conn *conn;
    uint32_t slotid;

#ifdef LATENCY_MEASURING_ENABLED
    struct ftimer ft;
#endif

    struct fuse_session *se;
    struct fuse_out_header *out_hdr;
    struct fuse_attr_out *out_attr;

    uint64_t *bitmap;
    char *attrlist;
};
struct open_cb_data {
    uint16_t thread_id;
    void *completion_context;
    struct virtionfs *vnfs;
    struct vnfs_conn *conn;
    uint32_t slotid;

#ifdef LATENCY_MEASURING_ENABLED
    struct ftimer ft;
#endif

    struct inode *i;

    struct fuse_out_header *out_hdr;
    struct fuse_open_out *out_open;

    uint32_t owner_val;
};
struct read_cb_data {
    uint16_t thread_id;
    void *completion_context;
    struct virtionfs *vnfs;
    struct vnfs_conn *conn;
    uint32_t slotid;

#ifdef LATENCY_MEASURING_ENABLED
    struct ftimer ft;
#endif

    struct fuse_out_header *out_hdr;
    struct iovec *out_iov;
    int out_iovcnt;
};
struct write_cb_data {
    uint16_t thread_id;
    void *completion_context;
    struct virtionfs *vnfs;
    struct vnfs_conn *conn;
    uint32_t slotid;

#ifdef LATENCY_MEASURING_ENABLED
    struct ftimer ft;
#endif

    struct fuse_write_in *in_write;
    struct iovec *in_iov;
    int *in_iovcnt;

    struct fuse_out_header *out_hdr;
    struct fuse_write_out *out_write;
};
struct fsync_cb_data {
    uint16_t thread_id;
    void *completion_context;
    struct virtionfs *vnfs;
    struct vnfs_conn *conn;
    uint32_t slotid;

#ifdef LATENCY_MEASURING_ENABLED
    struct ftimer ft;
#endif

    struct fuse_out_header *out_hdr;
    struct fuse_statfs_out *stat;
};
struct release_cb_data {
    uint16_t thread_id;
    void *completion_context;
    struct virtionfs *vnfs;
    struct vnfs_conn *conn;
    uint32_t slotid;

#ifdef LATENCY_MEASURING_ENABLED
    struct ftimer ft;
#endif

    struct inode *i;

    struct fuse_out_header *out_hdr;
};
struct create_cb_data {
    uint16_t thread_id;
    void *completion_context;
    struct virtionfs *vnfs;
    struct vnfs_conn *conn;
    uint32_t slotid;

#ifdef LATENCY_MEASURING_ENABLED
    struct ftimer ft;
#endif

    struct inode *i;

    struct fuse_out_header *out_hdr;
    struct fuse_entry_out *out_entry;
    struct fuse_open_out *out_open;

    uint32_t owner_val;
};

// This struct exists to get the size of the biggest cb_data for
// the memory pool chunk size 🤷.
// So if a new operation is added, its cb_data must be added here!
struct cb_data {
    union {
        struct getattr_cb_data getattr;
        struct lookup_cb_data lookup;
        struct statfs_cb_data statfs;
        struct setattr_cb_data setattr;
        struct open_cb_data open;
        struct read_cb_data read;
        struct write_cb_data write;
        struct fsync_cb_data fsync;
        struct release_cb_data release;
        struct create_cb_data create;
    };
};

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
    uint16_t thread_id = dpfs_hal_thread_id();
    return &vnfs->conns[thread_id];
}

// Only called from VirtioQ poller thread
int vnfs4_op_sequence(nfs_argop4 *op, struct vnfs_conn *conn, bool cachethis)
{
    op->argop = OP_SEQUENCE;
    struct SEQUENCE4args *arg = &op[0].nfs_argop4_u.opsequence;
    
    arg->sa_cachethis = cachethis;
    // sessionid
    memcpy(arg->sa_sessionid, conn->session.sessionid, sizeof(sessionid4));
    while (true) {
        // Determine and claim which slot we will use for this request
        for (uint32_t i = 0; i < conn->session.nslots; i++) {
            if (!conn->session.slots[i].in_use) {
                // Since only one thread (the Virtq thread) per session, whom can put the in_use to true
                // this is safe.
                arg->sa_slotid = i;
                conn->session.slots[i].in_use = true;
                goto slot_found;
            }
        }
        // All slots are in use, wait for a bit
        vnfs_error("All slots for connection %u are in use, suspending the Virtio poller"
                   "thread for a bit.\n Please performance tune (the max_background operations in FUSE"
                   "and the Virtio queue_depth so that this never happens!\n", conn->vnfs_conn_id);
        usleep(10);
    }
slot_found:
    // Determine the highest in_use slot
    for (int64_t i = conn->session.nslots; i >= 0; i--) {
        if (conn->session.slots[i].in_use) {
            arg->sa_highest_slotid = i;
        }
    }
    struct vnfs_slot *slot = &conn->session.slots[arg->sa_slotid];
    arg->sa_sequenceid = ++slot->seqid;
    
    return arg->sa_slotid;
}

// Only called from NFS poller thread
int vnfs4_handle_sequence(COMPOUND4res *res, struct vnfs_conn *conn)
{
    SEQUENCE4resok *seqok = &res->resarray.resarray_val[0].nfs_resop4_u.opsequence.SEQUENCE4res_u.sr_resok4;
    struct vnfs_slot *slot = &conn->session.slots[seqok->sr_slotid];
    slot->in_use = false;

    return 0;
}

void create_cb(struct rpc_context *rpc, int status, void *data,
                       void *private_data)
{
    struct create_cb_data *cb_data = (struct create_cb_data *)private_data;
    struct virtionfs *vnfs = cb_data->vnfs;

    LATENCY_MEASURING_STOP(CREATE);

    cb_data->conn->session.slots[cb_data->slotid].in_use = false;
    if (status != RPC_STATUS_SUCCESS) {
        vnfs_error("FUSE_CREATE:%lu - RPC error=%d, %s\n", cb_data->out_hdr->unique, status, (char *) data);
        cb_data->out_hdr->error = -EREMOTEIO;
        goto ret;
    }
    COMPOUND4res *res = data;
    if (res->status != NFS4_OK) {
        cb_data->out_hdr->error = -nfs_error_to_fuse_error(res->status);
        vnfs_error("FUSE_CREATE:%lu - NFS error=%d, FUSE error=%d\n",
                cb_data->out_hdr->unique, res->status, cb_data->out_hdr->error);
        goto ret;
    }

    // TODO

ret:;
    void *completion_context = cb_data->completion_context;
    mpool_free(vnfs->p[cb_data->thread_id], cb_data);
    dpfs_hal_async_complete(completion_context, DPFS_HAL_COMPLETION_SUCCES);
}


int create(struct fuse_session *se, void *user_data,
           struct fuse_in_header *in_hdr, struct fuse_create_in *in_create, const char *in_name,
           struct fuse_out_header *out_hdr, struct fuse_entry_out *out_entry, struct fuse_open_out *out_open,
           void *completion_context, uint16_t device_id)
{
    struct virtionfs *vnfs = user_data;
    struct vnfs_conn *conn = vnfs_get_conn(vnfs);
    uint16_t thread_id = dpfs_hal_thread_id();
    struct create_cb_data *cb_data = mpool_alloc(vnfs->p[thread_id]);
    if (!cb_data) {
        out_hdr->error = -ENOMEM;
        return 0;
    }

    cb_data->thread_id = thread_id;
    cb_data->completion_context = completion_context;
    cb_data->vnfs = vnfs;
    cb_data->conn = conn;
    cb_data->out_hdr = out_hdr;
    cb_data->out_entry = out_entry;
    cb_data->out_open = out_open;

    COMPOUND4args args;
    nfs_argop4 op[5];
    memset(&args.tag, 0, sizeof(args.tag));
    args.minorversion = NFS4DOT1_MINOR;
    args.argarray.argarray_len = sizeof(op) / sizeof(nfs_argop4);
    args.argarray.argarray_val = op;

    // SEQUENCE
    // PUTFH: parent
    // OPEN: create, attrs = flags, mode, usmask
    // GETATTR: out_entry
    // GETFH: out_open

    cb_data->slotid = vnfs4_op_sequence(&op[0], conn, false);
    // PUTFH
    // Get the inode manually because we want the FH of the parent
    struct inode *i = inode_table_get(vnfs->inodes, in_hdr->nodeid);
    cb_data->i = i;
    if (!i) {
    	vnfs_error("Invalid nodeid supplied\n");
        mpool_free(vnfs->p[thread_id], cb_data);
        out_hdr->error = -ENOENT;
        return 0;
    }
    op[1].argop = OP_PUTFH;
    op[1].nfs_argop4_u.opputfh.object.nfs_fh4_val = i->fh.val;
    op[1].nfs_argop4_u.opputfh.object.nfs_fh4_len = i->fh.len;

    // OPEN
    op[2].argop = OP_OPEN;
    memset(&op[2].nfs_argop4_u.opopen, 0, sizeof(OPEN4args));
    // Windows share stuff, this means normal operation in UNIX world
    op[2].nfs_argop4_u.opopen.share_access = OPEN4_SHARE_ACCESS_BOTH;
    op[2].nfs_argop4_u.opopen.share_deny = OPEN4_SHARE_DENY_NONE;
    // Don't use this because we don't do anything special with the share, so set to zero
    op[2].nfs_argop4_u.opopen.seqid = 0;
    // Set the owner with the clientid and the unique owner number (32 bit should be safe)
    // The clientid stems from the setclientid() handshake
    op[2].nfs_argop4_u.opopen.owner.clientid = vnfs->clientid;
    // We store this so that the reference stays alive
    cb_data->owner_val = atomic_fetch_add(&vnfs->open_owner_counter, 1);
    op[2].nfs_argop4_u.opopen.owner.owner.owner_val = (char *) &cb_data->owner_val;
    op[2].nfs_argop4_u.opopen.owner.owner.owner_len = sizeof(cb_data->owner_val);
    // Tell the server we want to open the file in the current FH dir (aka the parent dir)
    // with the specified filename
    op[2].nfs_argop4_u.opopen.claim.claim = CLAIM_FH;
    // Now we determine whether to CREATE or NOCREATE
    op[2].nfs_argop4_u.opopen.openhow.opentype = OPEN4_CREATE;
    op[2].nfs_argop4_u.opopen.openhow.openflag4_u.how.mode = UNCHECKED4;

    fattr4 *attr = &op[2].nfs_argop4_u.opopen.openhow.openflag4_u.how.createhow4_u.createattrs;
    // TODO this does a malloc, remove malloc it!
    nfs4_fill_create_attrs(in_hdr, in_create->mode, attr);

    // GETATTR
    nfs4_op_getattr(&op[3], standard_attributes, 2);
    // GETFH
    op[4].argop = OP_GETFH;

    LATENCY_MEASURING_START(CREATE);
    if (rpc_nfs4_compound_async(conn->rpc, create_cb, &args, cb_data) != 0) {
    	vnfs_error("Failed to send NFS:OPEN (with create) request\n");
        mpool_free(vnfs->p[thread_id], cb_data);
        out_hdr->error = -EREMOTEIO;
        return 0;
    }

    return EWOULDBLOCK;
}

void release_cb(struct rpc_context *rpc, int status, void *data,
                       void *private_data) {
    struct release_cb_data *cb_data = (struct release_cb_data *)private_data;
    struct virtionfs *vnfs = cb_data->vnfs;

    LATENCY_MEASURING_STOP(RELEASE);

    cb_data->conn->session.slots[cb_data->slotid].in_use = false;
    if (status != RPC_STATUS_SUCCESS) {
        vnfs_error("FUSE_RELEASE:%lu - RPC error=%d, %s\n", cb_data->out_hdr->unique, status, (char *) data);
        cb_data->out_hdr->error = -EREMOTEIO;
        goto ret;
    }
    COMPOUND4res *res = data;
    if (res->status != NFS4_OK) {
        cb_data->out_hdr->error = -nfs_error_to_fuse_error(res->status);
        vnfs_error("FUSE_RELEASE:%lu - NFS error=%d, FUSE error=%d\n",
                cb_data->out_hdr->unique, res->status, cb_data->out_hdr->error);
        goto ret;
    }

    cb_data->i->fh_open.len = 0;

ret:;
    void *completion_context = cb_data->completion_context;
    mpool_free(vnfs->p[cb_data->thread_id], cb_data);
    dpfs_hal_async_complete(completion_context, DPFS_HAL_COMPLETION_SUCCES);
}

int release(struct fuse_session *se, void *user_data,
           struct fuse_in_header *in_hdr, struct fuse_release_in *in_release,
           struct fuse_out_header *out_hdr,
           void *completion_context, uint16_t device_id)
{
    struct virtionfs *vnfs = user_data;
    struct inode *i = inode_table_get(vnfs->inodes, in_hdr->nodeid);
    if (!i) {
    	vnfs_error("Invalid nodeid supplied\n");
        out_hdr->error = -ENOENT;
        return 0;
    }
    uint32_t old_nopen = atomic_fetch_sub(&i->nopen, 1);
    // If there are still opens out there
    if (old_nopen > 1) {
        // then we don't actually release the inode
        return 0;
    }

    struct vnfs_conn *conn = vnfs_get_conn(vnfs);

    uint16_t thread_id = dpfs_hal_thread_id();
    struct release_cb_data *cb_data = mpool_alloc(vnfs->p[thread_id]);
    if (!cb_data) {
        out_hdr->error = -ENOMEM;
        return 0;
    }

    cb_data->thread_id = thread_id;
    cb_data->completion_context = completion_context;
    cb_data->vnfs = vnfs;
    cb_data->conn = conn;
    cb_data->out_hdr = out_hdr;

    COMPOUND4args args;
    nfs_argop4 op[3];
    memset(&args.tag, 0, sizeof(args.tag));
    args.minorversion = NFS4DOT1_MINOR;
    args.argarray.argarray_len = sizeof(op) / sizeof(nfs_argop4);
    args.argarray.argarray_val = op;

    cb_data->slotid = vnfs4_op_sequence(&op[0], conn, false);
    // PUTFH
    op[1].argop = OP_PUTFH;
    op[1].nfs_argop4_u.opputfh.object.nfs_fh4_val = i->fh_open.val;
    op[1].nfs_argop4_u.opputfh.object.nfs_fh4_len = i->fh_open.len;
    cb_data->i = i;
    // CLOSE
    op[2].argop = OP_CLOSE;
    op[2].nfs_argop4_u.opclose.seqid = 0;
    // Pass the stateid we received in the corresponding OPEN call
    op[2].nfs_argop4_u.opclose.open_stateid = cb_data->i->open_stateid;

    if (in_release->release_flags & FUSE_RELEASE_FLUSH) {
        // TODO Don't send the CLOSE in the callback of the COMMIT
        // because we don't support sending stuff in the callback
        vnfs_error("a FUSE:release requested a flush, not implemented TODO\n");
    }

    LATENCY_MEASURING_START(RELEASE);
    if (rpc_nfs4_compound_async(conn->rpc, release_cb, &args, cb_data) != 0) {
    	vnfs_error("Failed to send NFS:CLOSE request\n");
        mpool_free(vnfs->p[thread_id], cb_data);
        out_hdr->error = -EREMOTEIO;
        return 0;
    }

    return EWOULDBLOCK;
}

void vfsync_cb(struct rpc_context *rpc, int status, void *data,
               void *private_data) {
    struct fsync_cb_data *cb_data = (struct fsync_cb_data *)private_data;
    struct virtionfs *vnfs = cb_data->vnfs;

    LATENCY_MEASURING_STOP(FSYNC);

    cb_data->conn->session.slots[cb_data->slotid].in_use = false;
    if (status != RPC_STATUS_SUCCESS) {
        vnfs_error("FUSE_FSYNC:%lu - RPC error=%d, %s\n", cb_data->out_hdr->unique, status, (char *) data);
        cb_data->out_hdr->error = -EREMOTEIO;
        goto ret;
    }
    COMPOUND4res *res = data;
    if (res->status != NFS4_OK) {
        cb_data->out_hdr->error = -nfs_error_to_fuse_error(res->status);
        vnfs_error("FUSE_FSYNC:%lu - NFS error=%d, FUSE error=%d\n",
                cb_data->out_hdr->unique, res->status, cb_data->out_hdr->error);
        goto ret;
    }

ret:;
    void *completion_context = cb_data->completion_context;
    mpool_free(vnfs->p[cb_data->thread_id], cb_data);
    dpfs_hal_async_complete(completion_context, DPFS_HAL_COMPLETION_SUCCES);
}

// FUSE_FSYNC_FDATASYNC is not really adhered to, we always commit metadata
int vfsync(struct fuse_session *se, void *user_data,
           struct fuse_in_header *in_hdr, struct fuse_fsync_in *in_fsync,
           struct fuse_out_header *out_hdr,
           void *completion_context, uint16_t device_id)
{
    struct virtionfs *vnfs = user_data;
    struct vnfs_conn *conn = vnfs_get_conn(vnfs);
    uint16_t thread_id = dpfs_hal_thread_id();
    struct fsync_cb_data *cb_data = mpool_alloc(vnfs->p[thread_id]);
    if (!cb_data) {
        out_hdr->error = -ENOMEM;
        return 0;
    }

    cb_data->thread_id = thread_id;
    cb_data->completion_context = completion_context;
    cb_data->vnfs = vnfs;
    cb_data->conn = conn;
    cb_data->out_hdr = out_hdr;

    COMPOUND4args args;
    nfs_argop4 op[3];
    memset(&args.tag, 0, sizeof(args.tag));
    args.minorversion = NFS4DOT1_MINOR;
    args.argarray.argarray_len = sizeof(op) / sizeof(nfs_argop4);
    args.argarray.argarray_val = op;

    cb_data->slotid = vnfs4_op_sequence(&op[0], conn, false);
    // PUTFH
    struct inode *i = vnfs4_op_putfh(vnfs, &op[1], in_hdr->nodeid);
    if (!i) {
    	vnfs_error("Invalid nodeid supplied\n");
        mpool_free(vnfs->p[thread_id], cb_data);
        out_hdr->error = -ENOENT;
        return 0;
    }
    // COMMIT
    op[2].argop = OP_COMMIT;
    // Fuse doesn't provide offset and count for us, so we commit
    // the whole file 🤷
    op[2].nfs_argop4_u.opcommit.offset = 0;
    op[2].nfs_argop4_u.opcommit.count = 0;

    LATENCY_MEASURING_START(FSYNC);
    if (rpc_nfs4_compound_async(conn->rpc, vfsync_cb, &args, cb_data) != 0) {
    	vnfs_error("Failed to send NFS:commit request\n");
        mpool_free(vnfs->p[thread_id], cb_data);
        out_hdr->error = -EREMOTEIO;
        return 0;
    }

    return EWOULDBLOCK;
}

void vwrite_cb(struct rpc_context *rpc, int status, void *data,
           void *private_data)
{
    struct write_cb_data *cb_data = (struct write_cb_data *) private_data;
    struct virtionfs *vnfs = cb_data->vnfs;

    LATENCY_MEASURING_STOP(WRITE);

    cb_data->conn->session.slots[cb_data->slotid].in_use = false;
    if (status != RPC_STATUS_SUCCESS) {
        vnfs_error("FUSE_WRITE:%lu - RPC error=%d, %s\n", cb_data->out_hdr->unique, status, (char *) data);
        cb_data->out_hdr->error = -EREMOTEIO;
        goto ret;
    }
    COMPOUND4res *res = data;
    if (res->status != NFS4_OK) {
        cb_data->out_hdr->error = -nfs_error_to_fuse_error(res->status);
#ifdef DEBUG_ENABLED
        // Let's not do this error printing on the fast path
        vnfs_error("FUSE_WRITE:%lu - NFS error=%d, FUSE error=%d\n",
                cb_data->out_hdr->unique, res->status, cb_data->out_hdr->error);
#endif
        goto ret;
    }
    
    uint32_t written = 0;
    for (int i = 0; i < res->resarray.resarray_len-2; i++) {
        written += res->resarray.resarray_val[2+i].nfs_resop4_u.opwrite.WRITE4res_u.resok4.count;
    }
    cb_data->out_write->size = written;

    cb_data->out_hdr->len += sizeof(*cb_data->out_write);

ret:;
    void *completion_context = cb_data->completion_context;
    mpool_free(vnfs->p[cb_data->thread_id], cb_data);
    dpfs_hal_async_complete(completion_context, DPFS_HAL_COMPLETION_SUCCES);
}

// NFS does not support I/O vectors and in the case of the host sending more than one iov
// this write implementation only send the first iov. 
// When the host receives the written len of just the first iov, it will retry with the others
// TLDR: Not efficient (but functional) with multiple I/O vectors!
int vwrite(struct fuse_session *se, void *user_data,
         struct fuse_in_header *in_hdr, struct fuse_write_in *in_write,
         struct iovec *in_iov, int in_iov_cnt,
         struct fuse_out_header *out_hdr, struct fuse_write_out *out_write,
         void *completion_context, uint16_t device_id)
{
#ifdef VNFS_NULLDEV
    out_write->size = in_write->size;
    out_hdr->len += sizeof(*out_write);
    return 0;
#else

    struct virtionfs *vnfs = user_data;
    struct vnfs_conn *conn = vnfs_get_conn(vnfs);

    uint16_t thread_id = dpfs_hal_thread_id();
    struct write_cb_data *cb_data = mpool_alloc(vnfs->p[thread_id]);
    if (!cb_data) {
        out_hdr->error = -ENOMEM;
        return 0;
    }

    cb_data->thread_id = thread_id;
    cb_data->completion_context = completion_context;
    cb_data->vnfs = vnfs;
    cb_data->conn = conn;
    cb_data->in_write = in_write;
    cb_data->in_iov = in_iov;
    cb_data->out_hdr = out_hdr;
    cb_data->out_write = out_write;

    COMPOUND4args args;
    nfs_argop4 op[2+in_iov_cnt];
    memset(&args.tag, 0, sizeof(args.tag));
    args.minorversion = NFS4DOT1_MINOR;
    args.argarray.argarray_len = sizeof(op) / sizeof(nfs_argop4);
    args.argarray.argarray_val = op;

    cb_data->slotid = vnfs4_op_sequence(&op[0], conn, false);
    // PUTFH
    struct inode *i = vnfs4_op_putfh_open(vnfs, &op[1], in_hdr->nodeid);
    if (!i) {
    	vnfs_error("Invalid nodeid supplied\n");
        mpool_free(vnfs->p[thread_id], cb_data);
        out_hdr->error = -ENOENT;
        return 0;
    }
    // WRITE
    // Spread the iovectors over seperate WRITE4 ops in the same compound
    // so that it doesn't require any fancy shenanigans of iov writing to the rpc pdu.
    // It does however make the complete write much slower
    // in Linux nfsd (as of 6.2) each write in a single compound is individually sent to the VFS
    // TODO modify libnfs to support iovs with write or copy to a single buffer (that
    // neds a performance sanity check)
    uint64_t offset = 0;
    // We play it safe and assume that the other stuff in the request is 4k in size
    count4 maxwritesize = conn->session.attrs.ca_maxrequestsize - 4096;
    for (int j = 0; j < in_iov_cnt && offset + in_iov[j].iov_len < maxwritesize &&
           2+j < NFS4_MAX_OPS; j++) {
        op[2+j].argop = OP_WRITE;
        op[2+j].nfs_argop4_u.opwrite.stateid = i->open_stateid;
        op[2+j].nfs_argop4_u.opwrite.offset = in_write->offset + offset;
        op[2+j].nfs_argop4_u.opwrite.stable = UNSTABLE4;
        op[2+j].nfs_argop4_u.opwrite.data.data_val = in_iov[j].iov_base;
        op[2+j].nfs_argop4_u.opwrite.data.data_len = in_iov[j].iov_len;
        offset += in_iov[j].iov_len;
    }

    // libnfs by default allocates a buffer for the fully encoded NFS packet (rpc_pdu)
    // of sizeof(rpc header) + sizeof(COMPOUNF4args) + ZDR_ENCODEBUF_MINSIZE + alloc_hint
    // + rounding to 8 bytes twice
    // where ZDR_ENCODEBUF_MINSIZE is set to 4096 by libnfs
    // With alloc_hint=0 sending the RPC would fail.
    // Because this alloc_hint is really naive, the only way to safely make sure there
    // is enough buffer space, is to set the alloc_hint to our write buffer size...
    // This allocates way too much, but atleast it is safe
    uint64_t alloc_hint = offset; 

    LATENCY_MEASURING_START(WRITE);
    if (rpc_nfs4_compound_async2(conn->rpc, vwrite_cb, &args, cb_data, alloc_hint) != 0) {
    	vnfs_error("Failed to send NFS:write request\n");
        mpool_free(vnfs->p[thread_id], cb_data);
        out_hdr->error = -EREMOTEIO;
        return 0;
    }

    return EWOULDBLOCK;
#endif
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

void vread_cb(struct rpc_context *rpc, int status, void *data,
              void *private_data)
{
    struct read_cb_data *cb_data = (struct read_cb_data *)private_data;
    struct virtionfs *vnfs = cb_data->vnfs;

    LATENCY_MEASURING_STOP(READ);

    cb_data->conn->session.slots[cb_data->slotid].in_use = false;
    if (status != RPC_STATUS_SUCCESS) {
        vnfs_error("FUSE_READ:%lu - RPC error=%d, %s\n", cb_data->out_hdr->unique, status, (char *) data);
        cb_data->out_hdr->error = -EREMOTEIO;
        goto ret;
    }
    COMPOUND4res *res = data;
    if (res->status != NFS4_OK) {
        cb_data->out_hdr->error = -nfs_error_to_fuse_error(res->status);
#ifdef DEBUG_ENABLED
        // Let's not do this error printing on the fast path
        vnfs_error("FUSE_READ:%lu - NFS error=%d, FUSE error=%d\n",
                cb_data->out_hdr->unique, res->status, cb_data->out_hdr->error);
#endif
        goto ret;
    }

    char *buf = res->resarray.resarray_val[2].nfs_resop4_u.opread.READ4res_u
                .resok4.data.data_val;
    uint32_t len = res->resarray.resarray_val[2].nfs_resop4_u.opread.READ4res_u
                   .resok4.data.data_len;
    // Fill the iov that we return to the host
    if (cb_data->out_iovcnt >= 1) {
        size_t written = iovec_write_buf(cb_data->out_iov, cb_data->out_iovcnt, buf, len);
        cb_data->out_hdr->len += written;
    }

ret:;
    void *completion_context = cb_data->completion_context;
    mpool_free(vnfs->p[cb_data->thread_id], cb_data);
    dpfs_hal_async_complete(completion_context, DPFS_HAL_COMPLETION_SUCCES);
}

int vread(struct fuse_session *se, void *user_data,
          struct fuse_in_header *in_hdr, struct fuse_read_in *in_read,
          struct fuse_out_header *out_hdr, struct iovec *out_iov, int out_iovcnt,
          void *completion_context, uint16_t device_id)
{
#ifdef VNFS_NULLDEV
    out_hdr->len += in_read->size;
    return 0;
#else

    struct virtionfs *vnfs = user_data;
    struct vnfs_conn *conn = vnfs_get_conn(vnfs);
    uint16_t thread_id = dpfs_hal_thread_id();
    struct read_cb_data *cb_data = mpool_alloc(vnfs->p[thread_id]);
    if (!cb_data) {
        out_hdr->error = -ENOMEM;
        return 0;
    }

    cb_data->thread_id = thread_id;
    cb_data->completion_context = completion_context;
    cb_data->vnfs = vnfs;
    cb_data->conn = conn;
    cb_data->out_hdr = out_hdr;
    cb_data->out_iov = out_iov;
    cb_data->out_iovcnt = out_iovcnt;

    COMPOUND4args args;
    nfs_argop4 op[3];
    memset(&args.tag, 0, sizeof(args.tag));
    args.minorversion = NFS4DOT1_MINOR;
    args.argarray.argarray_len = sizeof(op) / sizeof(nfs_argop4);
    args.argarray.argarray_val = op;

    cb_data->slotid = vnfs4_op_sequence(&op[0], conn, false);
    // PUTFH
    struct inode *i = vnfs4_op_putfh_open(vnfs, &op[1], in_hdr->nodeid);
    if (!i) {
    	vnfs_error("Invalid nodeid supplied\n");
        mpool_free(vnfs->p[thread_id], cb_data);
        out_hdr->error = -ENOENT;
        return 0;
    }
    // OPEN
    op[2].argop = OP_READ;
    op[2].nfs_argop4_u.opread.stateid = i->open_stateid;
    op[2].nfs_argop4_u.opread.count = in_read->size;
    op[2].nfs_argop4_u.opread.offset = in_read->offset;

    LATENCY_MEASURING_START(READ);
    if (rpc_nfs4_compound_async(conn->rpc, vread_cb, &args, cb_data) != 0) {
    	vnfs_error("Failed to send NFS:READ request\n");
        mpool_free(vnfs->p[thread_id], cb_data);
        out_hdr->error = -EREMOTEIO;
        return 0;
    }

    return EWOULDBLOCK;
#endif
}

void vopen_cb(struct rpc_context *rpc, int status, void *data,
              void *private_data)
{
    struct open_cb_data *cb_data = (struct open_cb_data *)private_data;
    struct virtionfs *vnfs = cb_data->vnfs;

    LATENCY_MEASURING_STOP(OPEN);

    cb_data->conn->session.slots[cb_data->slotid].in_use = false;
    if (status != RPC_STATUS_SUCCESS) {
        vnfs_error("FUSE_OPEN:%lu - RPC error=%d, %s\n", cb_data->out_hdr->unique, status, (char *) data);
        cb_data->out_hdr->error = -EREMOTEIO;
        goto ret;
    }
    COMPOUND4res *res = data;
    if (res->status != NFS4_OK) {
        cb_data->out_hdr->error = -nfs_error_to_fuse_error(res->status);
        vnfs_error("FUSE_OPEN:%lu - NFS error=%d, FUSE error=%d\n",
                cb_data->out_hdr->unique, res->status, cb_data->out_hdr->error);
        goto ret;
    }

    struct inode *i = cb_data->i;

    atomic_fetch_add(&i->nopen, 1);
    // Store the FH from OPEN as the read write FH in the inode
    // The read-write FH is not the same (can't assume) as the metadata FH
    nfs_fh4 *fh = &res->resarray.resarray_val[3].nfs_resop4_u.opgetfh.GETFH4res_u.resok4.object;
    int ret = nfs4_clone_fh(&i->fh_open, fh);
    if (ret < 0) {
        cb_data->out_hdr->error = ret;
        goto ret;
    }

    // We don't use the FUSE:fh, nor do we have any open flags at the moment
    // so set the out_open to zeros
    memset(cb_data->out_open, 0, sizeof(*cb_data->out_open));
    cb_data->out_hdr->len += sizeof(*cb_data->out_open);
    OPEN4resok *openok = &res->resarray.resarray_val[2].nfs_resop4_u.opopen.OPEN4res_u.resok4;
    if (openok->rflags & OPEN4_RESULT_CONFIRM) {
        vnfs_error("virtionfs is using NFS4.1 but the server requested us"
                "to perform a OPEN_CONFIRM.\nThat should not happen per the spec!\n");
        cb_data->out_hdr->error = -EREMOTEIO;
        goto ret;
    }
    // Save the stateid we were given for the opened handle
    i->open_stateid = openok->stateid;

ret:;
    void *completion_context = cb_data->completion_context;
    mpool_free(vnfs->p[cb_data->thread_id], cb_data);
    dpfs_hal_async_complete(completion_context, DPFS_HAL_COMPLETION_SUCCES);
}

int vopen(struct fuse_session *se, void *user_data,
         struct fuse_in_header *in_hdr, struct fuse_open_in *in_open,
         struct fuse_out_header *out_hdr, struct fuse_open_out *out_open,
         void *completion_context, uint16_t device_id)
{
    struct virtionfs *vnfs = user_data;
    // Get the inode manually because we want the FH of the parent later
    struct inode *i = inode_table_get(vnfs->inodes, in_hdr->nodeid);
    if (!i) {
    	vnfs_error("Invalid nodeid supplied\n");
        out_hdr->error = -ENOENT;
        return 0;
    }
    // If the file is already opened, then just return
    if (i->nopen > 0) {
        atomic_fetch_add(&i->nopen, 1);
        // We don't use the FUSE:fh, nor do we have any open flags at the moment
        // so set the out_open to zeros
        memset(out_open, 0, sizeof(*out_open));
        out_hdr->len += sizeof(*out_open);
    }

    struct vnfs_conn *conn = vnfs_get_conn(vnfs);
    uint16_t thread_id = dpfs_hal_thread_id();
    struct open_cb_data *cb_data = mpool_alloc(vnfs->p[thread_id]);
    if (!cb_data) {
        out_hdr->error = -ENOMEM;
        return 0;
    }

    cb_data->thread_id = thread_id;
    cb_data->completion_context = completion_context;
    cb_data->vnfs = vnfs;
    cb_data->conn = conn;
    cb_data->out_hdr = out_hdr;
    cb_data->out_open = out_open;

    COMPOUND4args args;
    nfs_argop4 op[4];
    memset(&args.tag, 0, sizeof(args.tag));
    args.minorversion = NFS4DOT1_MINOR;
    args.argarray.argarray_len = sizeof(op) / sizeof(nfs_argop4);
    args.argarray.argarray_val = op;

    cb_data->slotid = vnfs4_op_sequence(&op[0], conn, false);
    // PUTFH
    cb_data->i = i;
    op[1].argop = OP_PUTFH;
    op[1].nfs_argop4_u.opputfh.object.nfs_fh4_val = i->fh.val;
    op[1].nfs_argop4_u.opputfh.object.nfs_fh4_len = i->fh.len;

    // OPEN
    op[2].argop = OP_OPEN;
    memset(&op[2].nfs_argop4_u.opopen, 0, sizeof(OPEN4args));
    // Windows share stuff, this means normal operation in UNIX world
    op[2].nfs_argop4_u.opopen.share_access = OPEN4_SHARE_ACCESS_BOTH;
    op[2].nfs_argop4_u.opopen.share_deny = OPEN4_SHARE_DENY_NONE;
    // Don't use this because we don't do anything special with the share, so set to zero
    op[2].nfs_argop4_u.opopen.seqid = 0;
    // Set the owner with the clientid and the unique owner number (32 bit should be safe)
    // The clientid stems from the setclientid() handshake
    op[2].nfs_argop4_u.opopen.owner.clientid = vnfs->clientid;
    // We store this so that the reference stays alive
    cb_data->owner_val = atomic_fetch_add(&vnfs->open_owner_counter, 1);
    op[2].nfs_argop4_u.opopen.owner.owner.owner_val = (char *) &cb_data->owner_val;
    op[2].nfs_argop4_u.opopen.owner.owner.owner_len = sizeof(cb_data->owner_val);
    // Tell the server we want to open the file in the current FH dir (aka the parent dir)
    // with the specified filename
    op[2].nfs_argop4_u.opopen.claim.claim = CLAIM_FH;
    // FUSE:OPEN cannot create a file
    op[2].nfs_argop4_u.opopen.openhow.opentype = OPEN4_NOCREATE;

    // GETFH
    op[3].argop = OP_GETFH;

    LATENCY_MEASURING_START(OPEN);
    if (rpc_nfs4_compound_async(conn->rpc, vopen_cb, &args, cb_data) != 0) {
    	vnfs_error("Failed to send NFS:open request\n");
        mpool_free(vnfs->p[thread_id], cb_data);
        out_hdr->error = -EREMOTEIO;
        return 0;
    }

    return EWOULDBLOCK;
}

void setattr_cb(struct rpc_context *rpc, int status, void *data,
                       void *private_data)
{
    struct setattr_cb_data *cb_data = (struct setattr_cb_data *)private_data;
    struct virtionfs *vnfs = cb_data->vnfs;

    LATENCY_MEASURING_STOP(OPEN);

    cb_data->conn->session.slots[cb_data->slotid].in_use = false;
    if (status != RPC_STATUS_SUCCESS) {
        vnfs_error("FUSE_SETATTR:%lu - RPC error=%d, %s\n", cb_data->out_hdr->unique, status, (char *) data);
        cb_data->out_hdr->error = -EREMOTEIO;
        goto ret;
    }
    COMPOUND4res *res = data;
    if (res->status != NFS4_OK) {
        cb_data->out_hdr->error = -nfs_error_to_fuse_error(res->status);
        vnfs_error("FUSE_SETATTR:%lu - NFS error=%d, FUSE error=%d\n",
                cb_data->out_hdr->unique, res->status, cb_data->out_hdr->error);
        goto ret;
    }

    GETATTR4resok *resok = &res->resarray.resarray_val[2].nfs_resop4_u.opgetattr.GETATTR4res_u.resok4;
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
    void *completion_context = cb_data->completion_context;
    mpool_free(vnfs->p[cb_data->thread_id], cb_data);
    dpfs_hal_async_complete(completion_context, DPFS_HAL_COMPLETION_SUCCES);
}

int setattr(struct fuse_session *se, void *user_data,
            struct fuse_in_header *in_hdr, struct fuse_setattr_in *in_setattr,
            struct fuse_out_header *out_hdr, struct fuse_attr_out *out_attr,
            void *completion_context, uint16_t device_id)
{
    struct virtionfs *vnfs = user_data;
    struct vnfs_conn *conn = vnfs_get_conn(vnfs);

    uint16_t thread_id = dpfs_hal_thread_id();
    struct setattr_cb_data *cb_data = mpool_alloc(vnfs->p[thread_id]);
    if (!cb_data) {
        out_hdr->error = -ENOMEM;
        return 0;
    }

    cb_data->thread_id = thread_id;
    cb_data->completion_context = completion_context;
    cb_data->vnfs = vnfs;
    cb_data->conn = conn;
    cb_data->se = se;
    cb_data->out_hdr = out_hdr;
    cb_data->out_attr = out_attr;

    COMPOUND4args args;
    nfs_argop4 op[4];
    memset(&args.tag, 0, sizeof(args.tag));
    args.minorversion = NFS4DOT1_MINOR;
    args.argarray.argarray_len = sizeof(op) / sizeof(nfs_argop4);
    args.argarray.argarray_val = op;

    cb_data->slotid = vnfs4_op_sequence(&op[0], conn, false);
    struct inode *i = vnfs4_op_putfh(vnfs, &op[1], in_hdr->nodeid);
    if (!i) {
    	vnfs_error("Invalid nodeid supplied\n");
        mpool_free(vnfs->p[thread_id], cb_data);
        out_hdr->error = -ENOENT;
        return 0;
    }

    /* TODO if locking is supported, put the stateid in here
     * zeroed stateid means anonymous aka
     * which can be used in READ, WRITE, and SETATTR requests to indicate the absence of any
     * open state associated with the request. (from 9.1.4.3. NFS 4 rfc)
     */
    op[2].argop = OP_SETATTR;
    memset(&op[2].nfs_argop4_u.opsetattr.stateid, 0, sizeof(stateid4));

    uint64_t *bitmap = mpool_alloc(vnfs->p[thread_id]);
    bitmap4 attrsmask;
    attrsmask.bitmap4_len = sizeof(*bitmap);
    attrsmask.bitmap4_val = (uint32_t *) bitmap;

    size_t attrlist_len = 0;
    if (in_setattr->valid & FUSE_SET_ATTR_MODE) {
        *bitmap |= (1UL << FATTR4_MODE);
        attrlist_len += sizeof(mode4);
    }
    if (in_setattr->valid & FUSE_SET_ATTR_SIZE) {
        *bitmap |= (1UL << FATTR4_SIZE);
        attrlist_len += sizeof(uint64_t);
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

    op[2].nfs_argop4_u.opsetattr.obj_attributes.attrmask = attrsmask;
    op[2].nfs_argop4_u.opsetattr.obj_attributes.attr_vals.attrlist4_val = attrlist;
    op[2].nfs_argop4_u.opsetattr.obj_attributes.attr_vals.attrlist4_len = attrlist_len;
    cb_data->bitmap = bitmap;
    cb_data->attrlist = attrlist;

    nfs4_op_getattr(&op[3], standard_attributes, 2);

    LATENCY_MEASURING_START(SETATTR);
    if (rpc_nfs4_compound_async(conn->rpc, setattr_cb, &args, cb_data) != 0) {
    	vnfs_error("Failed to send nfs4 SETATTR request\n");
        mpool_free(vnfs->p[thread_id], cb_data);
        out_hdr->error = -EREMOTEIO;
        return 0;
    }

    return EWOULDBLOCK;
}

void statfs_cb(struct rpc_context *rpc, int status, void *data,
               void *private_data)
{
    struct statfs_cb_data *cb_data = (struct statfs_cb_data *)private_data;
    struct virtionfs *vnfs = cb_data->vnfs;

    LATENCY_MEASURING_STOP(STATFS);

    cb_data->conn->session.slots[cb_data->slotid].in_use = false;
    if (status != RPC_STATUS_SUCCESS) {
        vnfs_error("FUSE_STATFS:%lu - RPC error=%d, %s\n", cb_data->out_hdr->unique, status, (char *) data);
        cb_data->out_hdr->error = -EREMOTEIO;
        goto ret;
    }
    COMPOUND4res *res = data;
    if (res->status != NFS4_OK) {
        cb_data->out_hdr->error = -nfs_error_to_fuse_error(res->status);
        vnfs_error("FUSE_STATFS:%lu - NFS error=%d, FUSE error=%d\n",
                cb_data->out_hdr->unique, res->status, cb_data->out_hdr->error);
        goto ret;
    }

    GETATTR4resok *resok = &res->resarray.resarray_val[2].nfs_resop4_u.opgetattr.GETATTR4res_u.resok4;
    char *attrs = resok->obj_attributes.attr_vals.attrlist4_val;
    u_int attrs_len = resok->obj_attributes.attr_vals.attrlist4_len;
    if (nfs_parse_statfs(&cb_data->out_statfs->st, attrs, attrs_len) != 0) {
        cb_data->out_hdr->error = -EREMOTEIO;
    }
    cb_data->out_hdr->len = cb_data->se->conn.proto_minor < 4 ?
        FUSE_COMPAT_STATFS_SIZE : sizeof(*cb_data->out_statfs);

ret:;
    void *completion_context = cb_data->completion_context;
    mpool_free(vnfs->p[cb_data->thread_id], cb_data);
    dpfs_hal_async_complete(completion_context, DPFS_HAL_COMPLETION_SUCCES);
}


int statfs(struct fuse_session *se, void *user_data,
           struct fuse_in_header *in_hdr,
           struct fuse_out_header *out_hdr, struct fuse_statfs_out *stat,
           void *completion_context, uint16_t device_id)
{
#ifdef VNFS_NULLDEV
    // We need to provide the host with plausibly real data
    memset(stat, 0, sizeof(*stat));
    out_hdr->len = se->conn.proto_minor < 4 ?
        FUSE_COMPAT_STATFS_SIZE : sizeof(*statfs);
    return 0;
#else
    struct virtionfs *vnfs = user_data;

    struct vnfs_conn *conn = vnfs_get_conn(vnfs);
    uint16_t thread_id = dpfs_hal_thread_id();
    struct statfs_cb_data *cb_data = mpool_alloc(vnfs->p[thread_id]);
    if (!cb_data) {
        out_hdr->error = -ENOMEM;
        return 0;
    }

    cb_data->thread_id = thread_id;
    cb_data->completion_context = completion_context;
    cb_data->vnfs = vnfs;
    cb_data->conn = conn;
    cb_data->se = se;
    cb_data->out_hdr = out_hdr;
    cb_data->out_statfs = stat;

    COMPOUND4args args;
    nfs_argop4 op[3];
    memset(&args.tag, 0, sizeof(args.tag));
    args.minorversion = NFS4DOT1_MINOR;
    args.argarray.argarray_len = sizeof(op) / sizeof(nfs_argop4);
    args.argarray.argarray_val = op;


    cb_data->slotid = vnfs4_op_sequence(&op[0], conn, false);
    // PUTFH the root
    op[1].argop = OP_PUTFH;
    struct inode *rooti = inode_table_get(vnfs->inodes, FUSE_ROOT_ID);
    op[1].nfs_argop4_u.opputfh.object.nfs_fh4_val = rooti->fh.val;
    op[1].nfs_argop4_u.opputfh.object.nfs_fh4_len = rooti->fh.len;
    // GETATTR statfs attributes
    nfs4_op_getattr(&op[2], statfs_attributes, 2);

    LATENCY_MEASURING_START(STATFS);
    if (rpc_nfs4_compound_async(conn->rpc, statfs_cb, &args, cb_data) != 0) {
    	vnfs_error("Failed to send FUSE:statfs request\n");
        mpool_free(vnfs->p[thread_id], cb_data);
        out_hdr->error = -EREMOTEIO;
        return 0;
    }

    return EWOULDBLOCK;
#endif
}

void lookup_cb(struct rpc_context *rpc, int status, void *data,
               void *private_data)
{
    struct lookup_cb_data *cb_data = (struct lookup_cb_data *)private_data;
    struct virtionfs *vnfs = cb_data->vnfs;

    LATENCY_MEASURING_STOP(LOOKUP);

    cb_data->conn->session.slots[cb_data->slotid].in_use = false;
    if (status != RPC_STATUS_SUCCESS) {
        vnfs_error("FUSE_LOOKUP:%lu - RPC error=%d, %s\n", cb_data->out_hdr->unique, status, (char *) data);
        cb_data->out_hdr->error = -EREMOTEIO;
        goto ret;
    }
    COMPOUND4res *res = data;
    if (res->status != NFS4_OK) {
        cb_data->out_hdr->error = -nfs_error_to_fuse_error(res->status);
        vnfs_error("FUSE_LOOKUP:%lu - NFS error=%d, FUSE error=%d\n",
                cb_data->out_hdr->unique, res->status, cb_data->out_hdr->error);
        goto ret;
    }

    char *attrs = res->resarray.resarray_val[3].nfs_resop4_u.opgetattr.GETATTR4res_u.resok4.obj_attributes.attr_vals.attrlist4_val;
    u_int attrs_len = res->resarray.resarray_val[3].nfs_resop4_u.opgetattr.GETATTR4res_u.resok4.obj_attributes.attr_vals.attrlist4_len;
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
        vnfs_error("Couldn't getsert inode with fileid: %lu\n", fileid);
        cb_data->out_hdr->error = -ENOMEM;
        goto ret;
    }
    atomic_fetch_add(&i->nlookup, 1);
    cb_data->out_entry->generation = i->generation;

    if (i->fh.len == 0) {
        // Retreive the FH from the res and set it in the inode
        // it's stored in the inode for later use ex. getattr when it uses the nodeid
        int ret = nfs4_clone_fh(&i->fh, &res->resarray.resarray_val[4].nfs_resop4_u.opgetfh.GETFH4res_u.resok4.object);
        if (ret < 0) {
            vnfs_error("Couldn't clone fh with fileid: %lu\n", fileid);
            cb_data->out_hdr->error = -ENOMEM;
            goto ret;
        }
    }
    cb_data->out_hdr->len += cb_data->se->conn.proto_minor < 9 ?
        FUSE_COMPAT_ENTRY_OUT_SIZE : sizeof(*cb_data->out_entry);

ret:;
    void *completion_context = cb_data->completion_context;
    mpool_free(vnfs->p[cb_data->thread_id], cb_data);
    dpfs_hal_async_complete(completion_context, DPFS_HAL_COMPLETION_SUCCES);
}

int lookup(struct fuse_session *se, void *user_data,
           struct fuse_in_header *in_hdr, const char *const in_name,
           struct fuse_out_header *out_hdr, struct fuse_entry_out *out_entry,
           void *completion_context, uint16_t device_id)
{
    struct virtionfs *vnfs = user_data;
    struct vnfs_conn *conn = vnfs_get_conn(vnfs);
    uint16_t thread_id = dpfs_hal_thread_id();
    struct lookup_cb_data *cb_data = mpool_alloc(vnfs->p[thread_id]);
    if (!cb_data) {
        out_hdr->error = -ENOMEM;
        return 0;
    }

    cb_data->thread_id = thread_id;
    cb_data->completion_context = completion_context;
    cb_data->vnfs = vnfs;
    cb_data->conn = conn;
    cb_data->se = se;
    cb_data->out_hdr = out_hdr;
    cb_data->out_entry = out_entry;

    COMPOUND4args args;
    nfs_argop4 op[5];
    memset(&args.tag, 0, sizeof(args.tag));
    args.minorversion = NFS4DOT1_MINOR;
    args.argarray.argarray_len = sizeof(op) / sizeof(nfs_argop4);
    args.argarray.argarray_val = op;

    cb_data->slotid = vnfs4_op_sequence(&op[0], conn, false);
    // PUTFH
    struct inode *pi = vnfs4_op_putfh(vnfs, &op[1], in_hdr->nodeid);
    if (!pi) {
    	vnfs_error("Invalid nodeid supplied\n");
        mpool_free(vnfs->p[thread_id], cb_data);
        out_hdr->error = -ENOENT;
        return 0;
    }
    // LOOKUP
    nfs4_op_lookup(&op[2], in_name);
    // FH now replaced with in_name's FH
    // GETATTR
    nfs4_op_getattr(&op[3], standard_attributes, 2);
    // GETFH
    op[4].argop = OP_GETFH;

    LATENCY_MEASURING_START(LOOKUP);
    if (rpc_nfs4_compound_async(conn->rpc, lookup_cb, &args, cb_data) != 0) {
    	vnfs_error("Failed to send nfs4 LOOKUP request\n");
        mpool_free(vnfs->p[thread_id], cb_data);
        out_hdr->error = -EREMOTEIO;
        return 0;
    }

    return EWOULDBLOCK;
}

void getattr_cb(struct rpc_context *rpc, int status, void *data,
                       void *private_data)
{
    struct getattr_cb_data *cb_data = (struct getattr_cb_data *)private_data;
    struct virtionfs *vnfs = cb_data->vnfs;

    LATENCY_MEASURING_STOP(GETATTR);

    cb_data->conn->session.slots[cb_data->slotid].in_use = false;
    if (status != RPC_STATUS_SUCCESS) {
        vnfs_error("FUSE_GETATTR:%lu - RPC error=%d, %s\n", cb_data->out_hdr->unique, status, (char *) data);
        cb_data->out_hdr->error = -EREMOTEIO;
        goto ret;
    }
    COMPOUND4res *res = data;
    if (res->status != NFS4_OK) {
        cb_data->out_hdr->error = -nfs_error_to_fuse_error(res->status);
        vnfs_error("FUSE_GETATTR:%lu - NFS error=%d, FUSE error=%d\n",
                cb_data->out_hdr->unique, res->status, cb_data->out_hdr->error);
        goto ret;
    }

    GETATTR4resok *resok = &res->resarray.resarray_val[2].nfs_resop4_u.opgetattr.GETATTR4res_u.resok4;
    char *attrs = resok->obj_attributes.attr_vals.attrlist4_val;
    u_int attrs_len = resok->obj_attributes.attr_vals.attrlist4_len;
    if (nfs_parse_attributes(&cb_data->out_attr->attr, attrs, attrs_len) == 0) {
        // This is not filled in by the parse_attributes fn
        cb_data->out_attr->attr.rdev = 0;
        cb_data->out_attr->attr_valid = 0;
        cb_data->out_attr->attr_valid_nsec = 0;
        cb_data->out_hdr->len += cb_data->se->conn.proto_minor < 9 ?
            FUSE_COMPAT_ATTR_OUT_SIZE : sizeof(*cb_data->out_attr);
#ifdef VNFS_NULLDEV
        cb_data->i->cached = true;
        cb_data->i->cached_attr = cb_data->out_attr->attr;
#endif
    } else {
        cb_data->out_hdr->error = -EREMOTEIO;
    }

ret:;
    void *completion_context = cb_data->completion_context;
    mpool_free(vnfs->p[cb_data->thread_id], cb_data);
    dpfs_hal_async_complete(completion_context, DPFS_HAL_COMPLETION_SUCCES);
}

int getattr(struct fuse_session *se, void *user_data,
            struct fuse_in_header *in_hdr, struct fuse_getattr_in *in_getattr,
            struct fuse_out_header *out_hdr, struct fuse_attr_out *out_attr,
            void *completion_context, uint16_t device_id)
{
    struct virtionfs *vnfs = user_data;

#ifdef VNFS_NULLDEV
    struct inode *i = inode_table_get(vnfs->inodes, in_hdr->nodeid);
    if (!i) {
    	vnfs_error("Invalid nodeid supplied\n");
        out_hdr->error = -ENOENT;
        return 0;
    }
    if (i->cached) {
        out_attr->attr_valid = 0;
        out_attr->attr_valid_nsec = 0;
        out_attr->dummy = 0;
        out_attr->attr = i->cached_attr;
        out_hdr->len += se->conn.proto_minor < 9 ?
            FUSE_COMPAT_ATTR_OUT_SIZE : sizeof(*out_attr);
        return 0;
    }
    // else we should first get a copy of the attr, for later getattrs
#endif

    struct vnfs_conn *conn = vnfs_get_conn(vnfs);
    uint16_t thread_id = dpfs_hal_thread_id();
    struct getattr_cb_data *cb_data = mpool_alloc(vnfs->p[thread_id]);
    if (!cb_data) {
        out_hdr->error = -ENOMEM;
        return 0;
    }

    cb_data->thread_id = thread_id;
    cb_data->completion_context = completion_context;
    cb_data->vnfs = vnfs;
    cb_data->conn = conn;
    cb_data->se = se;
    cb_data->out_hdr = out_hdr;
    cb_data->out_attr = out_attr;

    COMPOUND4args args;
    nfs_argop4 op[3];
    memset(&args.tag, 0, sizeof(args.tag));
    args.minorversion = NFS4DOT1_MINOR;
    args.argarray.argarray_len = sizeof(op) / sizeof(nfs_argop4);
    args.argarray.argarray_val = op;

    cb_data->slotid = vnfs4_op_sequence(&op[0], conn, false);
#ifdef VNFS_NULLDEV // we already have the inode
    vnfs4_op_putfh(vnfs, &op[1], in_hdr->nodeid);
    cb_data->i = i;
#else
    struct inode *i = vnfs4_op_putfh(vnfs, &op[1], in_hdr->nodeid);
    if (!i) {
    	vnfs_error("Invalid nodeid supplied\n");
        mpool_free(vnfs->p[thread_id], cb_data);
        out_hdr->error = -ENOENT;
        return 0;
    }
#endif
    nfs4_op_getattr(&op[2], standard_attributes, 2);
    
    LATENCY_MEASURING_START(GETATTR);
    if (rpc_nfs4_compound_async(conn->rpc, getattr_cb, &args, cb_data) != 0) {
    	vnfs_error("Failed to send nfs4 GETATTR request\n");
        mpool_free(vnfs->p[thread_id], cb_data);
        out_hdr->error = -EREMOTEIO;
        return 0;
    }

    return EWOULDBLOCK;
}
int destroy(struct fuse_session *se, void *user_data,
            struct fuse_in_header *in_hdr,
            struct fuse_out_header *out_hdr,
            void *completion_context, uint16_t device_id)
{
#ifdef LATENCY_MEASURING_ENABLED
    struct virtionfs *vnfs = user_data;
    for (int i = 0; i < FUSE_REMOVEMAPPING+1; i++) {
        for (int j = 1; j < vnfs->conn_cntr; j++) {
            struct vnfs_conn *conn = &vnfs->conns[j];
            if (conn->ft[i].running) {
                vnfs_error("OP(%d) timer of connection %d is still running!?\n", i, j);
                continue;
            }
            ft_add(&vnfs->conns[0].ft[i], &conn->ft[i]);
            vnfs->conns[0].op_calls[i] += conn->op_calls[i];
        }
        if (vnfs->conns[0].op_calls[i] > 0)
            printf("OP(%d) took %lu averaged over %lu calls\n", i, ft_get_nsec(&vnfs->conns[0].ft[i]) / vnfs->conns[0].op_calls[i], vnfs->conns[0].op_calls[i]);
    }
#endif

    // TODO Destroy all the connections

    return 0;
}

int init(struct fuse_session *se, void *user_data,
    struct fuse_in_header *in_hdr, struct fuse_init_in *in_init,
    struct fuse_conn_info *conn, struct fuse_out_header *out_hdr,
    uint16_t device_id)
{
    // TODO reenable when implementing flock()
    // if (conn->capable & FUSE_CAP_FLOCK_LOCKS)
    //     conn->want |= FUSE_CAP_FLOCK_LOCKS;

    // FUSE_CAP_SPLICE_READ is enabled in libfuse3 by default,
    // see do_init() in in fuse_lowlevel.c
    // We do not want this as splicing is not a thing with virtiofs
    conn->want &= ~FUSE_CAP_SPLICE_READ;
    conn->want &= ~FUSE_CAP_SPLICE_WRITE;

    se->init_done = true;

    // TODO WARNING
    // By returning 0, we allow the host to imediately start sending us requests,
    // even though the lookup_true_rootfh or setclientid might not be done yet
    // or even fail!
    // This introduces a race condition, where if the rootfh is not found yet
    // or there is no clientid virtionfs will crash horribly
    return 0;
}

void dpfs_nfs_assign_ops(struct fuse_ll_operations *ops) {
    ops->init = init;
    ops->lookup = lookup;
    ops->getattr = getattr;
    // NFS accepts the NFS:fh (received from a NFS:lookup==FUSE:lookup) as
    // its parameter to the dir ops like readdir
    ops->opendir = NULL;
    ops->open = vopen;
    ops->read = vread;
    ops->write = vwrite;
    ops->fsync = vfsync;
    ops->release = release;
    // NFS only does fsync(aka COMMIT) on files
    ops->fsyncdir = NULL;
    // The concept of flushing
    ops->flush = NULL;
    //ops->setattr = (typeof(ops->setattr)) setattr;
    ops->statfs = statfs;
    ops->destroy = destroy;
}

void dpfs_nfs_main(char *server, char *export,
               double timeout, bool cq_polling,
               const char *conf_path)
{
    struct virtionfs *vnfs = calloc(1, sizeof(struct virtionfs));
    if (!vnfs) {
        warn("Failed to dpfs_nfs");
        return;
    }
    vnfs->server = server;
    if (export[0] != '/') {
        vnfs_error("export must start with a '/'\n");
        goto ret_a;
    }
    vnfs->export = export;
    vnfs->timeout_sec = calc_timeout_sec(timeout);
    vnfs->timeout_nsec = calc_timeout_nsec(timeout);
    vnfs->cq_polling = cq_polling;

    int ret = inode_table_init(&vnfs->inodes);
    if (ret < 0) {
        vnfs_error("Failed to inode table - err=%d", ret);
        goto ret_c;
    }

    struct fuse_ll_operations ops;
    memset(&ops, 0, sizeof(ops));
    dpfs_nfs_assign_ops(&ops);

    struct dpfs_fuse *fuse = dpfs_fuse_new(&ops, conf_path, vnfs, NULL, NULL);
    if (!fuse)
        goto ret_a;
    vnfs->nthreads = dpfs_fuse_nthreads(fuse);

    vnfs->p = calloc(vnfs->nthreads, sizeof(*vnfs->p));
    for (uint16_t i = 0; i < vnfs->nthreads; i++) {
        int ret = mpool_init(&vnfs->p[i], sizeof(struct cb_data), 256);
        if (ret < 0) {
            vnfs_error("Failed to init mpool - err=%d", ret);
            goto ret_a;
        }
    }

    vnfs->conns = calloc(vnfs->nthreads, sizeof(struct vnfs_conn));
    if (!vnfs->conns) {
        warn("Failed to init NFS connections");
        goto ret_b;
    }
    vnfs_init_connections(vnfs);

    dpfs_fuse_loop(fuse);
    dpfs_fuse_destroy(fuse);

    inode_table_destroy(vnfs->inodes);
ret_c:
    free(vnfs->conns);
ret_b:
    for (uint16_t i = 0; i < vnfs->nthreads; i++) {
        mpool_destroy(vnfs->p[i]);
    }
ret_a:
    free(vnfs);
    printf("dpfs_nfs exited\n");
}
