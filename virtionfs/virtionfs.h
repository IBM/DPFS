/*
#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

#ifndef VIRTIONFS_VIRTIONFS_H
#define VIRTIONFS_VIRTIONFS_H

#include <stdbool.h>
#include <sys/time.h>
#include <nfsc/libnfs.h>
#include <nfsc/libnfs-raw.h>
#include <nfsc/libnfs-raw-nfs.h>
#include "virtiofs_emu_ll.h"

void virtionfs_main(char *server, char *export,
               bool debug, double timeout, uint32_t nthreads,
               struct virtiofs_emu_params *emu_params);

struct virtionfs {
    struct nfs_context *nfs;
    struct rpc_context *rpc;
    struct mpool *p;
    char *server;
    char *export;
    bool debug;
    uint64_t timeout_sec;
    uint32_t timeout_nsec;
};

#endif // VIRTIONFS_VIRTIONFS_H