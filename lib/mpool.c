/*
#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <ck_ring.h>

#include "mpool.h"

// Thread-safe
void *mpool_alloc(struct mpool *p) {
    void *r = NULL;
    ck_ring_dequeue_spsc(&p->ring, p->buffer, &r);
    return r;
}

// Thread-safe
void mpool_free(struct mpool *p, void *e) {
    ck_ring_enqueue_spsc(&p->ring, p->buffer, e);
}

#define MIN(x, y) x < y ? x : y

// Not thread-safe!
int mpool_init(struct mpool **p, uint64_t chunk_size, uint64_t chunks) {
    if (chunks < 4 || (chunks & (chunks - 1)) == 1) {
        fprintf(stderr, "mpool: chunks must be >= 4 and a power of 2\n");
        return -EINVAL;
    }

    *p = calloc(1, sizeof(struct mpool));
    if (!*p)
        return -ENOMEM;

    (*p)->buffer = malloc(sizeof(ck_ring_buffer_t) * chunks);
    if (!(*p)->buffer) {
        free(*p);
        return -ENOMEM;
    }
    ck_ring_init(&(*p)->ring, chunks);

    for (int i = 0; i < chunks; i++) {
        void *e = malloc(chunk_size);
        if (!e) {
            for (int j = 0; j < i; j++) {
                free(mpool_alloc(*p));
            }
            free(*p);
            return -ENOMEM;
        }
        mpool_free(*p, e);
    }

    return 0;
}

// Not thread-safe!
// If not all the chunks have been freed yet, then memory leaks will occur!
void mpool_destroy(struct mpool *p) {
    void *e = NULL;
    while (ck_ring_dequeue_spsc(&p->ring, p->buffer, &e)) {
        free(e);
    }
    free(p);
}
