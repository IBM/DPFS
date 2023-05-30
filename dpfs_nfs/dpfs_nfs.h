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

#include "config.h"
#include "dpfs_fuse.h"
#include "mpool.h"
#ifdef LATENCY_MEASURING_ENABLED
#include "ftimer.h"
#endif

void dpfs_nfs_main(char *server, char *export,
               double timeout, bool cq_polling,
               const char *conf_path);

enum vnfs_conn_state {
    VNFS_CONN_STATE_UNINIT = 0,
    VNFS_CONN_STATE_ESTABLISHED,
    VNFS_CONN_STATE_CLOSED,
    VNFS_CONN_STATE_SHOULD_CLOSE
};

struct vnfs_slot {
    // Starts at 1
    sequenceid4 seqid;
    bool in_use;
};

struct vnfs_session {
    sessionid4 sessionid;
    // The settings we negotiated with the server
    channel_attrs4 attrs;
    // Index is slotid4
    struct vnfs_slot *slots;
    uint32_t nslots;
    // The highest slot ID for which the client has a request outstanding
    slotid4 highest_slot;
};

struct vnfs_conn {
    uint32_t vnfs_conn_id;
    enum vnfs_conn_state state;
    struct nfs_context *nfs;
    struct rpc_context *rpc;
    // The session under which this connection is operating
    struct vnfs_session session;
#ifdef LATENCY_MEASURING_ENABLED
    struct ftimer ft[FUSE_REMOVEMAPPING+1];
    uint64_t op_calls[FUSE_REMOVEMAPPING+1];
#endif
};

struct virtionfs {
    uint16_t nthreads;
    bool cq_polling;

    // We open connections on the main thread and when running
    // each thread gets its own connection
    struct vnfs_conn *conns;
    uint32_t conn_cntr;

    struct inode_table *inodes;
    struct mpool **p;

    char *server;
    char *export;
    bool debug;
    uint64_t timeout_sec;
    uint32_t timeout_nsec;
    // TODO change uid and gid on a per-request basis
    int init_uid;
    int init_gid;

    atomic_uint open_owner_counter;

    struct EXCHANGE_ID4resok first_exchangeid;

    clientid4 clientid;
    verifier4 setclientid_confirm;
};

struct inode *vnfs4_op_putfh(struct virtionfs *vnfs, nfs_argop4 *op, uint64_t nodeid);

int vnfs4_op_sequence(nfs_argop4 *op, struct vnfs_conn *conn, bool cachethis);
int vnfs4_handle_sequence(COMPOUND4res *res, struct vnfs_conn *conn);

#define vnfs_error(fmt, ...) fprintf(stderr, "vnfs error %s:%d - " fmt, __FILE__, __LINE__, ##__VA_ARGS__)

#endif // VIRTIONFS_VIRTIONFS_H
