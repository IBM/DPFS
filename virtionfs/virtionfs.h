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

void virtionfs_main(char *server, char *export,
               bool debug, double timeout, uint32_t nthreads,
               struct virtiofs_emu_params *emu_params);

struct virtionfs {
    struct nfs_context *nfs;
    struct rpc_context *rpc;
    struct mpool *p;
    struct inode_table *inodes;

    char *server;
    char *export;
    bool debug;
    uint64_t timeout_sec;
    uint32_t timeout_nsec;

    // Currently there are two async NFS handshake operations
    // that need to complete
    // if this int == 2, then handshake is fully finished
#define VIRTIONFS_HANDSHAKE_PROGRESS_COMPLETE 2
    atomic_uint handshake_progress;
    atomic_uint open_owner_counter;
    atomic_uint seqid;

    nfs_fh4 rootfh;
    clientid4 clientid;
    verifier4 setclientid_confirm;
};

#endif // VIRTIONFS_VIRTIONFS_H
