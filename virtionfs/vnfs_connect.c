/*
#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

#include <sys/time.h>
#include <nfsc/libnfs.h>
#include <err.h>
#include <nfsc/libnfs-raw.h>
#include <nfsc/libnfs-raw-nfs4.h>
#include "vnfs_connect.h"
#include "virtionfs.h"
#include "nfs_v4.h"
#include "inode.h"

static void vnfs_conn_boot_done(struct virtionfs *vnfs) {
    printf("VNFS connection %u fully up!\n", vnfs->conn_cntr);
    vnfs->conn_cntr++;
    if (vnfs->conn_cntr < vnfs->nthreads) {
        vnfs_new_connection(vnfs);
    } else {
        printf("VNFS boot finished! All %u connections are ready to roll!\n", vnfs->conn_cntr);
    }
}

struct lookup_true_rootfh_cb_data {
    struct virtionfs *vnfs;
    struct fuse_out_header *out_hdr;
    struct snap_fs_dev_io_done_ctx *cb;
    char *export;
};

static void lookup_true_rootfh_cb(struct rpc_context *rpc, int status, void *data,
                       void *private_data) {
    struct virtionfs *vnfs = private_data;
    struct vnfs_conn *conn = &vnfs->conns[vnfs->conn_cntr];
    COMPOUND4res *res = data;

    if (status != RPC_STATUS_SUCCESS) {
    	fprintf(stderr, "RPC with NFS:LOOKUP_TRUE_ROOTFH unsuccessful: rpc error=%d\n", status);
        vnfs_destroy_connection(vnfs, conn, VNFS_CONN_STATE_ERROR);
        return;
    }
    if (res->status != NFS4_OK) {
    	fprintf(stderr, "NFS:LOOKUP_TRUE_ROOTFH unsuccessful: nfs error=%d", res->status);
        vnfs_destroy_connection(vnfs, conn, VNFS_CONN_STATE_ERROR);
        return;
    }

    int i = nfs4_find_op(res, OP_GETFH);
    assert(i >= 0);

    // Store the filehandle of the TRUE root (aka the filehandle of where our export lives)
    struct inode *rooti = inode_new(FUSE_ROOT_ID, NULL, NULL);
    nfs4_clone_fh(&rooti->fh, &res->resarray.resarray_val[i].nfs_resop4_u.opgetfh.GETFH4res_u.resok4.object); 
    inode_table_insert(vnfs->inodes, rooti);

    // T0 is done
    vnfs_conn_boot_done(vnfs);
}

static int lookup_true_rootfh(struct virtionfs *vnfs)
{
    struct vnfs_conn *conn = &vnfs->conns[vnfs->conn_cntr];

    char *export = strdup(vnfs->export);
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
    nfs_argop4 op[3+count];
    int i = 0;

    op[i].argop = OP_SEQUENCE;
    // TODO fill the sequence
    i++;
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

    if (rpc_nfs4_compound_async(vnfs->conns[vnfs->conn_cntr].rpc, lookup_true_rootfh_cb, &args, vnfs) != 0) {
    	fprintf(stderr, "Failed to send nfs4 LOOKUP request\n");
        vnfs_destroy_connection(vnfs, conn, VNFS_CONN_STATE_ERROR);
        return -1;
    }

    return 0;
}

static void create_session_cb(struct rpc_context *rpc, int status, void *data, void *private_data)
{
    struct virtionfs *vnfs = private_data;
    struct vnfs_conn *conn = &vnfs->conns[vnfs->conn_cntr];
    COMPOUND4res *res = data;
    
    if (status != RPC_STATUS_SUCCESS) {
        fprintf(stderr, "RPC with NFS:CREATE_SESSION unsuccessful: rpc error=%d\n", status);
        vnfs_destroy_connection(vnfs, conn, VNFS_CONN_STATE_ERROR);
        return;
    }
    if (res->status != NFS4_OK) {
        fprintf(stderr, "NFS:CREATE_SESSION unsuccessful: nfs error=%d\n", res->status);
        vnfs_destroy_connection(vnfs, conn, VNFS_CONN_STATE_ERROR);
        return;
    }

    CREATE_SESSION4resok *ok = &res->resarray.resarray_val[0].nfs_resop4_u.opcreatesession.CREATE_SESSION4res_u.csr_resok4;
    // TODO handle the attrs
    memcpy(vnfs->sessionid, ok->csr_sessionid, sizeof(sessionid4));
    // The sequenceid we receive in this ok is the same as we sent, so no need to do anything

    // The session and connection is now fully up, lets lookup the true root
    lookup_true_rootfh(vnfs);
}

static int create_session(struct virtionfs *vnfs, clientid4 clientid, sequenceid4 seqid)
{
    struct vnfs_conn *conn = &vnfs->conns[vnfs->conn_cntr];

    COMPOUND4args args;
    nfs_argop4 op[1];
    memset(&args, 0, sizeof(args));
    args.argarray.argarray_len = sizeof(op) / sizeof(nfs_argop4);
    args.argarray.argarray_val = op;
    memset(op, 0, sizeof(op));

    nfs4_op_createsession(&op[0], clientid, seqid);
    
    if (rpc_nfs4_compound_async(conn->rpc, create_session_cb, &args, vnfs) != 0) {
    	fprintf(stderr, "Failed to send NFS:create_session request\n");
        vnfs_destroy_connection(vnfs, conn, VNFS_CONN_STATE_ERROR);
        return -1;
    }

    return 0;
}

static void bind_conn_cb(struct rpc_context *rpc, int status, void *data, void *private_data)
{
    struct virtionfs *vnfs = private_data;
    struct vnfs_conn *conn = &vnfs->conns[vnfs->conn_cntr];
    COMPOUND4res *res = data;
    
    if (status != RPC_STATUS_SUCCESS) {
        fprintf(stderr, "RPC with NFS:bind_conn_to_session unsuccessful: rpc error=%d\n", status);
        vnfs_destroy_connection(vnfs, conn, VNFS_CONN_STATE_ERROR);
        return;
    }
    if (res->status != NFS4_OK) {
        fprintf(stderr, "NFS:bind_conn_to_session unsuccessful: nfs error=%d\n", res->status);
        vnfs_destroy_connection(vnfs, conn, VNFS_CONN_STATE_ERROR);
        return;
    }

    BIND_CONN_TO_SESSION4resok *ok = &res->resarray.resarray_val[0].nfs_resop4_u.opbindconntosession.BIND_CONN_TO_SESSION4res_u.bctsr_resok4;

    if (!(ok->bctsr_dir & CDFC4_FORE)) {
        // This connection is not a foreground channel, thus we can't send requests over it
        fprintf(stderr, "ERROR: A NFS connection was opened, bound to the session but"
                        "wasn't given a foreground channel!\n");
        vnfs_destroy_connection(vnfs, conn, VNFS_CONN_STATE_ERROR);
        return;
    }

    if (memcmp(ok->bctsr_sessid, vnfs->sessionid, sizeof(sessionid4)) == 0) {
        vnfs_conn_boot_done(vnfs);
    }
}

static int bind_conn(struct virtionfs *vnfs)
{
    struct vnfs_conn *conn = &vnfs->conns[vnfs->conn_cntr];

    COMPOUND4args args;
    nfs_argop4 op[1];
    memset(&args, 0, sizeof(args));
    args.argarray.argarray_len = sizeof(op) / sizeof(nfs_argop4);
    args.argarray.argarray_val = op;
    memset(op, 0, sizeof(op));

    // We don't support callbacks
    nfs4_op_bindconntosession(&op[0], &vnfs->sessionid, CDFC4_FORE, false);
    
    if (rpc_nfs4_compound_async(conn->rpc, bind_conn_cb, &args, vnfs) != 0) {
    	fprintf(stderr, "Failed to send NFS:setclientid request\n");
        vnfs_destroy_connection(vnfs, conn, VNFS_CONN_STATE_ERROR);
        return -1;
    }

    return 0;
}

static verifier4 default_verifier = {'0', '1', '2', '3', '4', '5', '6', '7'};

static void exchangeid_cb(struct rpc_context *rpc, int status, void *data, void *private_data)
{
    struct virtionfs *vnfs = private_data;
    struct vnfs_conn *conn = &vnfs->conns[vnfs->conn_cntr];
    COMPOUND4res *res = data;
    
    if (status != RPC_STATUS_SUCCESS) {
        fprintf(stderr, "RPC with NFS:exchange_id unsuccessful: rpc error=%d\n", status);
        vnfs_destroy_connection(vnfs, conn, VNFS_CONN_STATE_ERROR);
        return;
    }
    if (res->status != NFS4_OK) {
        fprintf(stderr, "NFS:exchange_id unsuccessful: nfs error=%d\n", res->status);
        vnfs_destroy_connection(vnfs, conn, VNFS_CONN_STATE_ERROR);
        return;
    }

    EXCHANGE_ID4resok *ok = &res->resarray.resarray_val[0].nfs_resop4_u.opexchangeid.EXCHANGE_ID4res_u.eir_resok4;

    // If we are T0
    if (vnfs->conn_cntr == 0) {
        memcpy(&vnfs->first_exchangeid, ok, sizeof(*ok));
        // Create a session (that the other connections can also use)
        create_session(vnfs, ok->eir_clientid, ok->eir_sequenceid);
    } else {
        if (nfs4_check_session_trunking_allowed(&vnfs->first_exchangeid, ok)) {
            bind_conn(vnfs);
        } else {
            // TODO handle can't trunking
            // close connection
        }
    }
}

static int exchangeid(struct virtionfs *vnfs)
{
    struct vnfs_conn *conn = &vnfs->conns[vnfs->conn_cntr];

    COMPOUND4args args;
    nfs_argop4 op[1];
    memset(&args, 0, sizeof(args));
    args.argarray.argarray_len = sizeof(op) / sizeof(nfs_argop4);
    args.argarray.argarray_val = op;
    memset(op, 0, sizeof(op));

    verifier4 v;
    memcpy(default_verifier, v, sizeof(v));
    v[0] = vnfs->conn_cntr;
    nfs4_op_exchangeid(&op[0], default_verifier, "virtionfs");
    
    if (rpc_nfs4_compound_async(conn->rpc, exchangeid_cb, &args, vnfs) != 0) {
    	fprintf(stderr, "Failed to send NFS:exchange_id request\n");
        vnfs_destroy_connection(vnfs, conn, VNFS_CONN_STATE_ERROR);
        return -1;
    }

    return 0;
}

void vnfs_destroy_connection(struct virtionfs *vnfs, struct vnfs_conn *conn, enum vnfs_conn_state state)
{
    // RPC is paired with the NFS context, so NFS_destroy destroys RPC
    conn->rpc = NULL;
    nfs_destroy_context(conn->nfs);

    nfs_mt_service_thread_stop(vnfs->nfs);
}

int vnfs_new_connection(struct virtionfs *vnfs) {
    struct vnfs_conn *conn = &vnfs->conns[vnfs->conn_cntr];
    struct nfs_context *nfs = nfs_init_context();
    if (conn->nfs == NULL) {
        warn("Failed to init libnfs context for connection %u\n", vnfs->conn_cntr);
        conn->state = VNFS_CONN_STATE_ERROR;
        return -1;
    }
    conn->nfs = nfs;
    nfs_set_version(nfs, NFS_V4);
    struct rpc_context *rpc = nfs_get_rpc_context(nfs);
    conn->rpc = rpc;

    // TODO FUSE:init always supplies uid=0 and gid=0,
    // so only setting the uid and gid once in the init is not sufficient
    // as in subsoquent operations different uid and gids can be supplied
    // however changing the uid and gid for every operations is very inefficient in libnfs
    // NOTE: the root permissions only properly work if the server has no_root_squash
    printf("%s, all NFS operations will go through uid %d and gid %d\n", __func__, vnfs->uid, vnfs->gid);
    nfs_set_uid(nfs, vnfs->uid);
    nfs_set_gid(nfs, vnfs->gid);

    if(nfs_mount(nfs, vnfs->server, vnfs->export)) {
        warn("Failed to mount nfs for connection %u\n", vnfs->conn_cntr);
        conn->state = VNFS_CONN_STATE_ERROR;
        conn->rpc = NULL;
        nfs_destroy_context(conn->nfs);
        return -1;
    }

    // We want to poll as fast as possible, MAX PERFORMANCE
    nfs_set_poll_timeout(nfs, -1);
    if (nfs_mt_service_thread_start(nfs)) {
        warn("Failed to start libnfs service thread for connection %u\n", vnfs->conn_cntr);
        conn->state = VNFS_CONN_STATE_ERROR;
        conn->rpc = NULL;
        nfs_destroy_context(conn->nfs);
        return -1;
    }

    int ret = exchangeid(vnfs);
    if (ret != 0) {
        vnfs_destroy_connection(vnfs, conn, VNFS_CONN_STATE_ERROR);
    }
    return ret;
}

