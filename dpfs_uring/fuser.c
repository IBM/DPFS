/*
#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

#include <sys/resource.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <err.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <limits.h>
#include <pthread.h>
#include <fcntl.h>
#include <liburing.h>

#include "fuser.h"
#include "mirror_impl.h"

struct inode *inode_new(fuse_ino_t ino) {
    struct inode *i = calloc(1, sizeof(struct inode));
    if (!i)
        return NULL;

    i->ino = ino;
    i->fd = -1;
    pthread_mutex_init(&i->m, NULL);

    return i;
}

void inode_destroy(struct inode *i) {
    if (i->fd > 0)
        close(i->fd);
    pthread_mutex_destroy(&i->m);
    free(i);
}

void inode_table_clear(struct inode_table *t) {
    // Everything should be cleared already
    for (size_t i = 0; i < t->size; i++) {
        struct inode *next = t->array[i];
        while (next) {
            struct inode *i = next;
            next = i->next;
            fprintf(stderr, "ERROR: a inode was not released by the host before destroying the file system");

            if (i->fd != -1) {
                fprintf(stderr, ", fd was not closed");
                close(i->fd);
            }
            fprintf(stderr, "\n");
            free(i);
        }
        t->array[i] = NULL;
    }
}

int inode_table_init(struct inode_table **ret_t) {
    struct inode_table *t = calloc(1, sizeof(struct inode_table));
    if (t == NULL) {
        fprintf(stderr, "Could not allocate memory for inode_table!\n");
        return -1;
    }
    t->size = INODE_TABLE_SIZE;
    t->array = (struct inode **) calloc(1, sizeof(struct inode *) * t->size);
    if (t->array == NULL) {
        fprintf(stderr, "Could not allocate memory for inode_table.array!\n");
        return -1;
    }
    t->use = 0;

    *ret_t = t;
    return 0;
}

size_t inode_table_hash(struct inode_table *t, fuse_ino_t ino) {
    return ino % t->size;
}

// TODO not thread-safe
struct inode *inode_table_get(struct inode_table *t, fuse_ino_t ino) {
    size_t hash = inode_table_hash(t, ino);

    for (struct inode *inode = t->array[hash]; inode != NULL; inode = inode->next)
        if (inode->ino == ino)
            return inode;

    return NULL;
}

struct inode *inode_table_getsert(struct inode_table *t, fuse_ino_t ino) {
    size_t hash = inode_table_hash(t, ino);

    for (struct inode *inode = t->array[hash]; inode != NULL; inode = inode->next)
        if (inode->ino == ino)
            return inode;

    struct inode *i = inode_new(ino);
    if (!i)
        return NULL;

    inode_table_insert(t, i);

    return i;
}

void inode_table_insert(struct inode_table *t, struct inode *i) {
    size_t hash = inode_table_hash(t, i->ino);

    if (t->array[hash] != NULL)
        i->next = t->array[hash];
    t->array[hash] = i;
}

struct inode *inode_table_remove(struct inode_table *t, fuse_ino_t ino) {
    size_t hash = inode_table_hash(t, ino);
    struct inode *prev = NULL;
    for (struct inode *inode = t->array[hash]; inode != NULL; inode = inode->next) {
        if (inode->ino == ino) {
            if (!prev) { // we set the first inode to the next
                t->array[hash] = inode->next;
            } else { // link prev and next
                prev->next = inode->next;
            }
            return inode;
        }
        prev = inode;
    }
    return NULL;
}

bool inode_table_erase(struct inode_table *t, fuse_ino_t ino) {
    struct inode *i = inode_table_remove(t, ino);
    if (i)
        inode_destroy(i);
    return i;
}

struct inode *ino_to_inodeptr(struct fuser *f, fuse_ino_t ino) {
    if (ino == FUSE_ROOT_ID)
        return &f->root;

    struct inode* inode = (struct inode*) (ino);
    if(inode->fd == -1) {
        fprintf(stderr, "Unknown inode %ld!\n", ino);
        return NULL;
    }
    return inode;
}
int ino_to_fd(struct fuser *f, fuse_ino_t ino) {
    struct inode *inode = ino_to_inodeptr(f, ino);
    if (!inode)
        return -1;
    else
        return inode->fd;
}

void directory_destroy(struct directory *d) {
    if (d->dp)
        closedir(d->dp);
    free(d);
}

static void maximize_fd_limit() {
    struct rlimit lim;
    int res = getrlimit(RLIMIT_NOFILE, &lim);
    if (res != 0) {
        warn("WARNING: getrlimit() failed with");
        return;
    }
    lim.rlim_cur = lim.rlim_max;
    res = setrlimit(RLIMIT_NOFILE, &lim);
    if (res != 0)
        warn("WARNING: setrlimit() failed with");
}

static void *fuser_io_poll_thread(struct fuser *f) {
    while(!f->io_poll_thread_stop){
        struct io_uring_cqe *cqe;
        int ret;
        if (f->cq_polling)
             ret = io_uring_peek_cqe(&f->ring, &cqe);
        else
             ret = io_uring_wait_cqe(&f->ring, &cqe);

        if(ret == -EAGAIN || ret == -EINTR){
            continue; // No event to process
        } else if(ret != 0) {
            fprintf(stderr, "ERROR: uring cqe peek/wait ret = %d\n", ret);
            fprintf(stderr, "ERROR: stopping uring cqe polling\n");
            return NULL;
        } // else process the event

        struct fuser_cb_data *cb_data = io_uring_cqe_get_data(cqe);

        cb_data->cb(cb_data, cqe);

        io_uring_cqe_seen(&f->ring, cqe);
        mpool_free(f->cb_data_pool, cb_data);
    }
    return NULL;
}

// todo proper error handling
int fuser_main(bool debug, char *source, double metadata_timeout, const char *conf_path,
        bool cq_polling, bool sq_polling) {
    struct fuser *f = calloc(1, sizeof(struct fuser));
    if (f == NULL)
        err(1, "ERROR: Could not allocate memory for struct fuser");

    f->debug = debug;
    f->source = strdup(source);
    f->timeout = metadata_timeout;

    struct stat s;
    int ret = lstat(f->source, &s);
    if (ret == -1)
        err(1, "ERROR: failed to stat source (\"%s\")", source);
    if (!S_ISDIR(s.st_mode))
        errx(1, "ERROR: source is not a directory");
    f->src_dev = s.st_dev;

    // Open source(i.e. root dir)
    f->root.fd = open(f->source, O_PATH);
    if (f->root.fd == -1)
        err(1, "ERROR: open(\"%s\", O_PATH)", f->source);
    f->root.nlookup = 9999;
    f->root.ino = FUSE_ROOT_ID;

    // Don't apply umask, use modes exactly as specified
    umask(0);

    // We need an fd for every dentry in our the filesystem that the
    // kernel knows about. This is way more than most processes need,
    // so try to get rid of any resource softlimit.
    maximize_fd_limit();

    ret = inode_table_init(&f->inodes);
    if (ret == -1)
        err(1, "ERROR: Failed to init inode_table f->inodes");

    struct io_uring_params params;

    memset(&params, 0, sizeof(params));
    f->cq_polling = cq_polling;
    if (f->cq_polling) {
        //params.flags |= IORING_SETUP_IOPOLL;
    }
    if (sq_polling) {
        params.flags |= IORING_SETUP_SQPOLL;
        params.sq_thread_idle = 2000;
    }

    ret = io_uring_queue_init_params(512, &f->ring, &params);
    if (ret) {
        fprintf(stderr, "ERROR: Unable to setup io_uring: %s\n", strerror(-ret));
        return -1;
    }

    struct fuse_ll_operations ops;
    fuser_mirror_assign_ops(&ops);

    mpool_init(&f->cb_data_pool, sizeof(struct fuser_cb_data), 256);
    pthread_t poll_thread;
    pthread_create(&poll_thread, NULL, (void *(*)(void *))fuser_io_poll_thread, f);

    dpfs_fuse_main(&ops, conf_path, f, NULL, NULL);

    f->io_poll_thread_stop = true;
    // TODO drain the queue first
    io_uring_queue_exit(&f->ring);
    if (!f->cq_polling)
        pthread_cancel(poll_thread);
    else
        pthread_join(poll_thread, NULL);
    
    mpool_destroy(f->cb_data_pool);
    // destroy inode table
    free(f);

    return 0;
}
