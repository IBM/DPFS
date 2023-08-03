/*
#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

#define _GNU_SOURCE
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <err.h>
#include <sched.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <limits.h>
#include <pthread.h>
#include <fcntl.h>

#include "config.h"
#include "mirror_impl.h"
#include "fuser.h"

struct inode *inode_new(fuse_ino_t ino) {
    struct inode *i = calloc(1, sizeof(struct inode));
    if (!i)
        return NULL;

    i->ino = ino;
    i->fd = -1;
    pthread_mutex_init(&i->m, NULL);

    return i;
}

static void inode_destroy(struct inode *i) {
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
#ifdef DEBUG_ENABLED
            fprintf(stderr, "WARNING: a inode was not released by the host before destroying the file system");

            if (i->fd != -1) {
                fprintf(stderr, ", fd was not closed");
                close(i->fd);
            }
            fprintf(stderr, "\n");
#endif
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

struct tdata {
    pthread_t t;
    uint16_t thread_id;
    struct fuser *f;
};

// Blocks on a single ring
static void *fuser_io_blocking_thread(void *arg) {
    struct tdata *td = arg;
    struct fuser *f = td->f;

    while(!f->io_poll_thread_stop){
            struct io_uring_cqe *cqe;
            int ret = io_uring_wait_cqe(&f->rings[td->thread_id], &cqe);

            if(ret == -EAGAIN || ret == -EINTR){
                continue; // No event to process
            } else if(ret != 0) {
                fprintf(stderr, "ERROR: uring cqe wait ret = %d\n", ret);
                fprintf(stderr, "ERROR: stopping uring cqe waiting\n");
                return NULL;
            } // else process the event

            struct fuser_cb_data *cb_data = io_uring_cqe_get_data(cqe);
#ifdef DEBUG_ENABLED
            printf("Uring: got cqe for FUSE OP(%u) with id=%u\n", cb_data->in_hdr->opcode, cb_data->in_hdr->unique);
#endif

            cb_data->cb(cb_data, cqe);

            io_uring_cqe_seen(&f->rings[td->thread_id], cqe);
            mpool_free(f->cb_data_pools[cb_data->thread_id], cb_data);
    }
    return NULL;
}

// Polls on a range of rings depending on the number of polling threads and rings
static void *fuser_io_poll_thread(void *arg) {
    struct tdata *td = arg;
    struct fuser *f = td->f;

    long num_cpus = sysconf(_SC_NPROCESSORS_CONF);
    if (dpfs_fuse_nthreads(f->fuse) + f->cq_polling_nthreads <= num_cpus) {
        cpu_set_t loop_cpu;
        CPU_ZERO(&loop_cpu);
        // Calculate our polling CPU
        // with two DPFS threads, two CQ threads and 8 total cores:
        // DPFS thread 0 will occupy core 7
        // DPFS thread 1 will occupy core 6
        // cq polling thread 0 will occupy core 5
        // cq polling thread 1 will occupy core 4
        CPU_SET(num_cpus-1 - dpfs_fuse_nthreads(f->fuse) - td->thread_id, &loop_cpu);
        int ret = sched_setaffinity(gettid(), sizeof(loop_cpu), &loop_cpu);
        if (ret == -1) {
            warn("Could not set the CPU affinity of polling thread %u. uring polling thread %u will continue not pinned.", td->thread_id, td->thread_id);
        }
    }

    // Determine the window of rings we need to poll
    size_t n = f->nrings / f->cq_polling_nthreads;
    size_t remainder = f->nrings % f->cq_polling_nthreads;
    size_t start = n * td->thread_id;
    size_t end = start + n;
    if (td->thread_id == 0 && remainder != 0) {
        // Extend our window to the right with the remainder devices
        end += remainder;
    } else if (remainder != 0) {
        // Move our window to the right by the number of devices T0 has
        start += remainder;
        end += remainder;
    }

    while(!td->f->io_poll_thread_stop){
        for (uint16_t i = start; i < end; i++) {
            struct io_uring_cqe *cqe;
            int ret = io_uring_peek_cqe(&td->f->rings[i], &cqe);

            if(ret == -EAGAIN || ret == -EINTR){
                continue; // No event to process
            } else if(ret != 0) {
                fprintf(stderr, "ERROR: uring cqe peek ret = %d\n", ret);
                fprintf(stderr, "ERROR: stopping uring cqe polling\n");
                return NULL;
            } // else process the event

            struct fuser_cb_data *cb_data = io_uring_cqe_get_data(cqe);
#ifdef DEBUG_ENABLED
            printf("Uring: got cqe for FUSE OP(%u) with id=%u\n", cb_data->in_hdr->opcode, cb_data->in_hdr->unique);
#endif

            cb_data->cb(cb_data, cqe);

            io_uring_cqe_seen(&td->f->rings[i], cqe);
            mpool_free(td->f->cb_data_pools[cb_data->thread_id], cb_data);
        }
    }
    return NULL;
}

// TODO proper error handling
int fuser_main(char *source, double metadata_timeout, enum fuser_directio_mode directio_mode,
        const char *conf_path, bool cq_polling, uint16_t cq_polling_nthreads, bool sq_polling) {
    struct fuser *f = calloc(1, sizeof(struct fuser));
    if (f == NULL)
        err(1, "ERROR: Could not allocate memory for struct fuser");

    f->source = strdup(source);
    f->timeout = metadata_timeout;
    f->directio_mode = directio_mode;

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

    struct fuse_ll_operations ops;
    fuser_mirror_assign_ops(&ops);

    struct dpfs_fuse *fuse = dpfs_fuse_new(&ops, conf_path, f, NULL, NULL);
    f->fuse = fuse;
    f->nrings = dpfs_fuse_nthreads(fuse);

    struct io_uring_params params;

    memset(&params, 0, sizeof(params));
    f->cq_polling = cq_polling;
    f->cq_polling_nthreads = cq_polling_nthreads;
    if (f->cq_polling) {
        // The io_uring docs say this flag needs to be supplied if peek_cqe is used
        // but if this flag is supplied, all I/O errors 🤷, without it works anyway
        //params.flags |= IORING_SETUP_IOPOLL;
    }
    if (sq_polling) {
        // Kernel-side polling can only be enabled with fixed files
        // Which we don't implement because that's a big mess with SNAP in the HAL
        //params.flags |= IORING_SETUP_SQPOLL;
        //params.sq_thread_idle = 2000;
    }

    f->rings = calloc(f->nrings, sizeof(*f->rings));
    for (uint16_t i = 0; i < f->nrings; i++) {
        ret = io_uring_queue_init_params(512, &f->rings[i], &params);
        if (ret) {
            fprintf(stderr, "ERROR: Unable to setup io_uring: %s\n", strerror(-ret));
            return -1;
        }
    }

    f->cb_data_pools = calloc(f->nrings, sizeof(*f->cb_data_pools));
    for (uint16_t i = 0; i < f->nrings; i++) {
        mpool_init(&f->cb_data_pools[i], sizeof(struct fuser_cb_data), 256);
    }

    uint16_t nthreads;
    if (f->cq_polling) {
        nthreads = cq_polling_nthreads; // user-defined

        if (nthreads > f->nrings) {
            fprintf(stderr, "ERROR: There cannot be more cq polling threads than DPFS threads for request handling!\n");
            return -1;
        }

        long num_cpus = sysconf(_SC_NPROCESSORS_CONF);
        if (dpfs_fuse_nthreads(fuse) + nthreads >= num_cpus) {
            warn("DPFS is configured with as many or more threads than there are cores on the DPU!"
                    "Core pinning is therefore disabled in dpfs_uring!\n");
        }
    } else {
        nthreads = f->nrings; // a blocking thread per ring
    }
    struct tdata td[nthreads];

    for (uint16_t i = 0; i < nthreads; i++) {
        td[i].thread_id = i;
        td[i].f = f;
        if (f->cq_polling)
            pthread_create(&td[i].t, NULL, fuser_io_poll_thread, &td[i]);
        else
            pthread_create(&td[i].t, NULL, fuser_io_blocking_thread, &td[i]);
    }

    printf("The following operations are asynchrounous through io_uring: read, write, fsync");
#ifndef IORING_METADATA_DISABLED
    printf(", statx, open, create, fallocate, rename, close, unlink, mkdir, symlink");
#endif
    printf("\n");

    dpfs_fuse_loop(fuse);
    dpfs_fuse_destroy(fuse);

    f->io_poll_thread_stop = true;
    for (uint16_t i = 0; i < f->nrings; i++) {
        // TODO drain the queue first
        io_uring_queue_exit(&f->rings[i]);
    }
    if (f->cq_polling) {
        for (uint16_t i = 0; i < nthreads; i++) {
            pthread_join(td[i].t, NULL);
        }
    } else {
        for (uint16_t i = 0; i < nthreads; i++) {
            pthread_cancel(td[i].t);
        }
    }
    
    for (uint16_t i = 0; i < f->nrings; i++) {
        mpool_destroy(f->cb_data_pools[i]);
    }
    // destroy inode table
    free(f);

    return 0;
}
