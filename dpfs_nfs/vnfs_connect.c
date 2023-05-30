/*
#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

#include <sys/time.h>
#include <nfsc/libnfs.h>
#include <err.h>
#include <stdlib.h>
#include <nfsc/libnfs-raw.h>
#include <nfsc/libnfs-raw-nfs4.h>
#include <string.h>
#include "vnfs_connect.h"
#include "nfs_v4.h"
#include "inode.h"

static void vnfs_conn_up(struct virtionfs *vnfs)
{
    struct vnfs_conn *conn = &vnfs->conns[vnfs->conn_cntr];
    conn->state = VNFS_CONN_STATE_ESTABLISHED;
    printf("VNFS connection %u fully up!\n", conn->vnfs_conn_id);

    vnfs->conn_cntr++;
    if (vnfs->conn_cntr < vnfs->nthreads)
        vnfs_init_connections(vnfs);
    else
        printf("VNFS boot finished! All %u connections are ready to roll!\n", vnfs->conn_cntr);
}

static void reclaim_complete_cb(struct rpc_context *rpc, int status, void *data,
                                  void *private_data)
{
    struct virtionfs *vnfs = private_data;
    struct vnfs_conn *conn = &vnfs->conns[vnfs->conn_cntr];
    COMPOUND4res *res = data;

    if (status != RPC_STATUS_SUCCESS) {
    	fprintf(stderr, "RPC with NFS:RECLAIM_COMPLETE unsuccessful: rpc error=%d\n", status);
        vnfs_destroy_connection(conn, VNFS_CONN_STATE_SHOULD_CLOSE);
        return;
    }
    if (res->status != NFS4_OK && res->status != NFS4ERR_COMPLETE_ALREADY) {
    	fprintf(stderr, "NFS:RECLAIM_COMPLETE unsuccessful: nfs error=%d", res->status);
        vnfs_destroy_connection(conn, VNFS_CONN_STATE_SHOULD_CLOSE);
        return;
    }
    if (res->status == NFS4ERR_COMPLETE_ALREADY) {
        printf("The NFS server told us that an old connection lingered around"
               "(NFS4_ERR_COMPLETE_ALREADY), only a partial handshake was necessary.\n");
    }

    vnfs4_handle_sequence(res, conn);

    vnfs_conn_up(vnfs);
}

static void reclaim_complete(struct virtionfs *vnfs)
{
    struct vnfs_conn *conn = &vnfs->conns[vnfs->conn_cntr];

    COMPOUND4args args;
    nfs_argop4 op[2];
    args.minorversion = NFS4DOT1_MINOR;
    memset(&args.tag, 0, sizeof(args.tag));
    args.argarray.argarray_len = sizeof(op) / sizeof(nfs_argop4);
    args.argarray.argarray_val = op;
    memset(op, 0, sizeof(op));

    vnfs4_op_sequence(&op[0], conn, false);

    op[1].argop = OP_RECLAIM_COMPLETE;
    op[1].nfs_argop4_u.opreclaimcomplete.rca_one_fs = false;

    if (rpc_nfs4_compound_async(conn->rpc, reclaim_complete_cb, &args, vnfs) != 0) {
    	fprintf(stderr, "%s: Failed to send nfs4 RECLAIM_COMPLETE request\n", __func__);
        vnfs_destroy_connection(conn, VNFS_CONN_STATE_SHOULD_CLOSE);
    }
}

static void lookup_true_rootfh_cb(struct rpc_context *rpc, int status, void *data,
                       void *private_data)
{
    struct virtionfs *vnfs = private_data;
    struct vnfs_conn *conn = &vnfs->conns[vnfs->conn_cntr];
    COMPOUND4res *res = data;

    if (status != RPC_STATUS_SUCCESS) {
    	fprintf(stderr, "RPC with NFS:LOOKUP_TRUE_ROOTFH unsuccessful: rpc error=%d\n", status);
        vnfs_destroy_connection(conn, VNFS_CONN_STATE_SHOULD_CLOSE);
        return;
    }
    if (res->status != NFS4_OK) {
    	fprintf(stderr, "NFS:LOOKUP_TRUE_ROOTFH unsuccessful: nfs error=%d", res->status);
        vnfs_destroy_connection(conn, VNFS_CONN_STATE_SHOULD_CLOSE);
        return;
    }
    vnfs4_handle_sequence(res, conn);
    int i = nfs4_find_op(res, OP_GETFH);
    assert(i >= 0);

    // Store the filehandle of the TRUE root (aka the filehandle of where our export lives)
    struct inode *rooti = inode_new(FUSE_ROOT_ID);
    nfs4_clone_fh(&rooti->fh, &res->resarray.resarray_val[i].nfs_resop4_u.opgetfh
            .GETFH4res_u.resok4.object); 
    inode_table_insert(vnfs->inodes, rooti);

    reclaim_complete(vnfs);
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
    char *export_traverse = export;
    while(*export_traverse) if (*export_traverse++ == '/') ++count;

    COMPOUND4args args;
    nfs_argop4 op[3+count];
    memset(&args.tag, 0, sizeof(args.tag));
    args.minorversion = NFS4DOT1_MINOR;
    args.argarray.argarray_len = sizeof(op) / sizeof(nfs_argop4);
    args.argarray.argarray_val = op;
    int i = 0;

    vnfs4_op_sequence(&op[i++], conn, false);
    // PUTFH
    op[i++].argop = OP_PUTROOTFH;
    // LOOKUP
    char *token = strtok(export, "/");
    while (i < count+2) {
        nfs4_op_lookup(&op[i++], token);
        token = strtok(NULL, "/");
    }
    // GETFH
    op[i].argop = OP_GETFH;

    if (rpc_nfs4_compound_async(conn->rpc, lookup_true_rootfh_cb, &args, vnfs) != 0) {
    	fprintf(stderr, "%s: Failed to send nfs4 LOOKUP request\n", __func__);
        vnfs_destroy_connection(conn, VNFS_CONN_STATE_SHOULD_CLOSE);
        free(export);
        return -1;
    }

    free(export);
    return 0;
}

static void create_session_cb(struct rpc_context *rpc, int status, void *data,
        void *private_data)
{
    struct virtionfs *vnfs = private_data;
    struct vnfs_conn *conn = &vnfs->conns[vnfs->conn_cntr];
    COMPOUND4res *res = data;
    
    if (status != RPC_STATUS_SUCCESS) {
        fprintf(stderr, "RPC with NFS:CREATE_SESSION unsuccessful: rpc error=%d\n", status);
        vnfs_destroy_connection(conn, VNFS_CONN_STATE_SHOULD_CLOSE);
        return;
    }
    if (res->status != NFS4_OK) {
        fprintf(stderr, "NFS:CREATE_SESSION unsuccessful: nfs error=%d\n", res->status);
        vnfs_destroy_connection(conn, VNFS_CONN_STATE_SHOULD_CLOSE);
        return;
    }

    CREATE_SESSION4resok *ok = &res->resarray.resarray_val[0].nfs_resop4_u.
        opcreatesession.CREATE_SESSION4res_u.csr_resok4;
    memcpy(conn->session.sessionid, ok->csr_sessionid, sizeof(sessionid4));
    memcpy(&conn->session.attrs, &ok->csr_fore_chan_attrs, sizeof(channel_attrs4));
    // The sequenceid we receive in this ok is the same as we sent, so no need to do anything
    // We set no flags, so no need to do anything
    //
    if (conn->session.attrs.ca_maxoperations < NFS4_MAX_OPS) {
        fprintf(stderr, "WARNING: Your NFS server might be running an older version of the Linux kernel."
                        "It only supports %u maxoperations per request, we hoped for %u."
                        "This will negatively impact write performance for large block sizes.\n",
                        conn->session.attrs.ca_maxoperations, NFS4_MAX_OPS);
    }

    conn->session.nslots = ok->csr_fore_chan_attrs.ca_maxrequests;
    conn->session.slots = calloc(conn->session.nslots, sizeof(struct vnfs_slot));

    // The session and connection is now fully up
    // We might be the first connection and need to lookup the true rootfh
    if (vnfs->conn_cntr == 0)
        lookup_true_rootfh(vnfs);
    else // We only need to RECLAIM_COMPLETE once
        vnfs_conn_up(vnfs);
}

static int create_session(struct virtionfs *vnfs, struct vnfs_conn *conn,
        clientid4 clientid, sequenceid4 seqid)
{
    COMPOUND4args args;
    nfs_argop4 op[1];
    args.minorversion = NFS4DOT1_MINOR;
    memset(&args.tag, 0, sizeof(args.tag));
    args.argarray.argarray_len = sizeof(op) / sizeof(nfs_argop4);
    args.argarray.argarray_val = op;
    memset(op, 0, sizeof(op));

    nfs4_op_createsession(&op[0], clientid, seqid);
    
    if (rpc_nfs4_compound_async(conn->rpc, create_session_cb, &args, vnfs) != 0) {
    	fprintf(stderr, "Failed to send NFS:create_session request\n");
        vnfs_destroy_connection(conn, VNFS_CONN_STATE_SHOULD_CLOSE);
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
        vnfs_destroy_connection(conn, VNFS_CONN_STATE_SHOULD_CLOSE);
        return;
    }
    if (res->status != NFS4_OK) {
        fprintf(stderr, "NFS:exchange_id unsuccessful: nfs error=%d\n", res->status);
        vnfs_destroy_connection(conn, VNFS_CONN_STATE_SHOULD_CLOSE);
        return;
    }

    EXCHANGE_ID4resok *ok = &res->resarray.resarray_val[0].nfs_resop4_u.opexchangeid
            .EXCHANGE_ID4res_u.eir_resok4;

    // If we are T0
    if (vnfs->conn_cntr == 0) {
        memcpy(&vnfs->first_exchangeid, ok, sizeof(*ok));
        // The owner major string and server scope string must be copied over in new buffers
        vnfs->first_exchangeid.eir_server_owner.so_major_id.so_major_id_val =
            malloc(vnfs->first_exchangeid.eir_server_owner.so_major_id.so_major_id_len);
        memcpy(vnfs->first_exchangeid.eir_server_owner.so_major_id.so_major_id_val,
               ok->eir_server_owner.so_major_id.so_major_id_val,
               vnfs->first_exchangeid.eir_server_owner.so_major_id.so_major_id_len);
        vnfs->first_exchangeid.eir_server_scope.eir_server_scope_val =
            malloc(vnfs->first_exchangeid.eir_server_scope.eir_server_scope_len);
        memcpy(vnfs->first_exchangeid.eir_server_scope.eir_server_scope_val,
               ok->eir_server_scope.eir_server_scope_val,
               vnfs->first_exchangeid.eir_server_scope.eir_server_scope_len);

        create_session(vnfs, conn, ok->eir_clientid, ok->eir_sequenceid);
    } else {
        if (nfs4_check_clientid_trunking_allowed(&vnfs->first_exchangeid, ok)) {
            create_session(vnfs, conn, ok->eir_clientid, ok->eir_sequenceid);
        } else {
            fprintf(stderr, "VNFS connection %u was not allowed to start trunking\n",
                    vnfs->conn_cntr);
            vnfs_destroy_connection(conn, VNFS_CONN_STATE_SHOULD_CLOSE);
        }
    }
}

static int exchangeid(struct virtionfs *vnfs, struct vnfs_conn *conn)
{
    COMPOUND4args args;
    nfs_argop4 op[1];
    args.minorversion = NFS4DOT1_MINOR;
    memset(&args.tag, 0, sizeof(args.tag));
    args.argarray.argarray_len = sizeof(op) / sizeof(nfs_argop4);
    args.argarray.argarray_val = op;
    memset(op, 0, sizeof(op));

    nfs4_op_exchangeid(&op[0], default_verifier, "virtionfs");
    
    if (rpc_nfs4_compound_async(conn->rpc, exchangeid_cb, &args, vnfs) != 0) {
    	fprintf(stderr, "Failed to send NFS:exchange_id request\n");
        vnfs_destroy_connection(conn, VNFS_CONN_STATE_SHOULD_CLOSE);
        return -1;
    }

    return 0;
}

void vnfs_destroy_connection(struct vnfs_conn *conn, enum vnfs_conn_state state)
{
    // This function might be called from inside of the NFS service thread,
    // which means that we cannot destroy the NFS context nor stop the NFS service thread
    // Two solutions:
    // 1. Destroy from a VirtioQ thread
    // 2. Patch libnfs to support this usecase. (Seems like a lot of work)

    // RPC is paired with the NFS context, so NFS_destroy destroys RPC
}

int vnfs_init_connections(struct virtionfs *vnfs)
{
    struct vnfs_conn *conn = &vnfs->conns[vnfs->conn_cntr];
    // This might be the second FUSE_INIT call
    memset(conn, 0, sizeof(struct vnfs_conn));
    conn->vnfs_conn_id = vnfs->conn_cntr;

#ifdef LATENCY_MEASURING_ENABLED
    for (int i = 0; i < FUSE_REMOVEMAPPING+1; i++) {
        ft_init(&conn->ft[i]);
    }
#endif

    struct nfs_context *nfs = nfs_init_context();
    if (nfs == NULL) {
        warn("Failed to init libnfs context for connection %u\n", vnfs->conn_cntr);
        conn->state = VNFS_CONN_STATE_SHOULD_CLOSE;
        return -1;
    }
    conn->nfs = nfs;
    nfs_set_version(nfs, NFS_V4);
    struct rpc_context *rpc = nfs_get_rpc_context(nfs);
    conn->rpc = rpc;

    // TODO investigate if this can be efficiently set on a per FS operation basis
    nfs_set_uid(nfs, 0);
    nfs_set_gid(nfs, 0);

    if(nfs_mount(nfs, vnfs->server, vnfs->export)) {
        warn("Failed to mount nfs for connection %u\n", vnfs->conn_cntr);
        conn->state = VNFS_CONN_STATE_SHOULD_CLOSE;
        conn->rpc = NULL;
        nfs_destroy_context(conn->nfs);
        return -1;
    }

    // We want to poll as fast as possible, MAX PERFORMANCE
    if (vnfs->cq_polling)
        nfs_set_poll_timeout(nfs, -1);
    else
        nfs_set_poll_timeout(nfs, 100);

    if (nfs_mt_service_thread_start(nfs)) {
        warn("Failed to start libnfs service thread for connection %u\n", conn->vnfs_conn_id);
        conn->state = VNFS_CONN_STATE_SHOULD_CLOSE;
        conn->rpc = NULL;
        nfs_destroy_context(conn->nfs);
        return -1;
    }

    int ret = exchangeid(vnfs, conn);
    if (ret != 0) {
        vnfs_destroy_connection(conn, VNFS_CONN_STATE_SHOULD_CLOSE);
    }
    return ret;
}

