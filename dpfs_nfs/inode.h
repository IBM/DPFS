/*
#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

#ifndef VIRTIONFS_INODE_H
#define VIRTIONFS_INODE_H

#include <stdatomic.h>
#include <sys/time.h>
#include <nfsc/libnfs.h>
#include <nfsc/libnfs-raw-nfs4.h>

#include "config.h"
#include "dpfs_fuse.h"
#include "nfs_v4.h"

struct inode {
    // We return the fileid as fuse_ino_t
    // This is only possible under the assumption that FUSE_ROOT_ID (=1)
    // is not in the possible range of fileids.
    // This is needed because the host will always use FUSE_ROOT_ID,
    // the value of which we can't change.
    // This assumption is made because empirically we found that the
    // NFS root (aka PUTROOTFH) has fileid=2 and others only
    // above that.
    // AKA this assumption is NOT battle-tested
    fattr4_fileid fileid;
    // A fh.nfs_fh4_len of 0 means that there is no FH
    vnfs_fh4 fh;
    // TODO protect this fh with a lock
    vnfs_fh4 fh_open;
    stateid4 open_stateid;

    atomic_size_t generation;
    atomic_size_t nlookup;
    atomic_size_t nopen;

    struct inode *next;

#ifdef VNFS_NULLDEV
    bool cached;
    struct fuse_attr cached_attr;
#endif
};

struct inode_table {
    struct inode **array;
    size_t size;
    pthread_mutex_t m;
};

#define INODE_TABLE_SIZE 8192

struct inode *inode_new(fattr4_fileid fileid);
void inode_destroy(struct inode *i);

int inode_table_init(struct inode_table **t);
void inode_table_destroy(struct inode_table *t);
struct inode *inode_table_get(struct inode_table *, fattr4_fileid);
struct inode *inode_table_insert(struct inode_table *t, struct inode *i);
struct inode *inode_table_getsert(struct inode_table *t, fattr4_fileid fileid);
struct inode *inode_table_remove(struct inode_table *t, fattr4_fileid fileid);
bool inode_table_erase(struct inode_table *, fattr4_fileid);

#endif // VIRTIONFS_INODE_H
