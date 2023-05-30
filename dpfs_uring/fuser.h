/*
#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

#ifndef FUSER_H
#define FUSER_H

#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <dirent.h>
#include <sys/types.h>
#include <linux/fuse.h>
#include <liburing.h>

#include "dpfs_fuse.h"
#include "mpool.h"

struct inode {
    fuse_ino_t ino;

    // O_PATH | O_NOFOLLOW = just for passing it as the parent to openat
    // The fd with read and write permissions is not stored, but given and received from the user
    int fd;

    dev_t src_dev;
    ino_t src_ino;
    int generation;
    uint64_t nopen;
    uint64_t nlookup;
    pthread_mutex_t m;

    struct inode *next;
};

struct inode *inode_new(fuse_ino_t ino);
void inode_destroy(struct inode *);

struct inode_table {
    struct inode **array;
    size_t use;
    size_t size;
};

#define INODE_TABLE_SIZE 8192

int inode_table_init(struct inode_table **);
size_t inode_table_hash(struct inode_table *, fuse_ino_t);
struct inode *inode_table_get(struct inode_table *, fuse_ino_t);
struct inode *inode_table_getsert(struct inode_table *, fuse_ino_t);
void inode_table_insert(struct inode_table *, struct inode *);
struct inode *inode_table_remove(struct inode_table *, fuse_ino_t);
bool inode_table_erase(struct inode_table *, fuse_ino_t);
void inode_table_clear(struct inode_table *t);

struct directory {
    DIR *dp;
    off_t offset;
};

void directory_destroy(struct directory *);

struct fuser {
    pthread_mutex_t m;
    struct inode_table *inodes; // protected by m
    struct inode root;
    double timeout;
    bool debug;
    char *source;
    // size_t blocksize;
    dev_t src_dev; // gets set to the dev of the source
    // bool nocache;
    
    struct dpfs_fuse *fuse;

    uint16_t nrings;
    uint16_t cq_polling_nthreads;

    volatile bool io_poll_thread_stop;
    struct io_uring *rings;
    bool cq_polling;
    // if cq_polling == false, then nthreads = nrings

    struct mpool **cb_data_pools;
};

struct inode *ino_to_inodeptr(struct fuser *, fuse_ino_t);
int ino_to_fd(struct fuser *, fuse_ino_t);

int fuser_main(bool debug, char *source, double metadata_timeout,
               const char *conf_path, bool cq_polling,
               uint16_t cq_polling_nthreads, bool sq_polling);

#endif // FUSER_H
