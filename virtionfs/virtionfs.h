/*
#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

#ifndef VIRTIONFS_VIRTIONFS_H
#define VIRTIONFS_VIRTIONFS_H

#include <stdbool.h>
#include <stdatomic.h>
#include <sys/time.h>
#include <nfsc/libnfs.h>
#include <nfsc/libnfs-raw-nfs4.h>
#include "virtiofs_emu_ll.h"
#include "mpool2.h"

void virtionfs_main(char *server, char *export,
               bool debug, double timeout, uint32_t nthreads,
               struct virtiofs_emu_params *emu_params);

enum vnfs_conn_state {
    VNFS_CONN_STATE_UNINIT = 0,
    VNFS_CONN_STATE_ESTABLISHED,
    VNFS_CONN_STATE_CLOSED,
    VNFS_CONN_STATE_ERROR
};

struct nfs_slot {
    // Starts at 1
    sequenceid4 seqid;
};

struct vnfs_conn {
    struct nfs_context *nfs;
    struct rpc_context *rpc;

    enum vnfs_conn_state state;
    // The settings we negotiated with the server
    channel_attrs4 attrs;
    // Index is slotid4
    struct nfs_slot *slots;
    slotid4 cl_highest_slot;
    slotid4 sr_highest_slot;
    slotid4 sr_target_highest_slot;
};

struct virtionfs {
    struct nfs_context *nfs;
    struct rpc_context *rpc;
    // We open connections on the main thread
    struct vnfs_conn *conns;
    uint32_t conn_cntr;
    sessionid4 sessionid;
    // We receive the first sequenceid from EXCHANGE_ID
    atomic_uint seqid;

    struct inode_table *inodes;
    struct mpool2 *p;

    char *server;
    char *export;
    bool debug;
    uint64_t timeout_sec;
    uint32_t timeout_nsec;
    uint32_t nthreads;
    // TODO change uid and gid on a per-request basis
    uint32_t uid;
    uint32_t gid;

    // Currently there are two async NFS handshake operations
    // that need to complete
    // if this int == 2, then handshake is fully finished
    // and the init_done_ctx can be called to send the init result
    // to the host
#define VIRTIONFS_HANDSHAKE_PROGRESS_COMPLETE 2
    atomic_uint handshake_progress;

    atomic_uint open_owner_counter;

    struct EXCHANGE_ID4resok first_exchangeid;

    clientid4 clientid;
    verifier4 setclientid_confirm;
};

#endif // VIRTIONFS_VIRTIONFS_H
