/*
#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>

#include "mpool.h"

// Thread-safe
void *mpool_alloc(struct mpool *p) {
    void *r = NULL;

    pthread_spin_lock(&p->lock);
alloc:
    if (p->head) {
        r = p->head;        
        if (p->head->next)
            p->head = p->head->next;
        goto ret;
    } else {
        printf("mpool allocating new chunk\n");
        for (int i = 0; i < p->alloc_size-1; i++) {
            void *c = malloc(p->chunk_size);
            if (!c) {
                if (i > 0) { // if malloc failed we return one of the previously malloc'ed chunks
                    goto alloc;
                } else { // or completely fail
                    r = NULL;
                    goto ret;
                }
            }
            // Same as mpool_free, but without the lock, since we already have it
            ((struct mpool_chunk *)c)->next = p->head;
            p->head = c;
        }
        void *c = malloc(p->chunk_size);
        if (!c) { // if malloc failed we return one of the previously malloc'ed chunks
            goto alloc;
        } else {
            r = c;
            goto ret;
        }
    }

ret:
    pthread_spin_unlock(&p->lock);
    return r;
}

// Thread-safe
void mpool_free(struct mpool *p, void *e) {
    struct mpool_chunk *chunk = (struct mpool_chunk *) e;

    pthread_spin_lock(&p->lock);
    chunk->next = p->head;
    p->head = chunk;
    pthread_spin_unlock(&p->lock);
}

// Not thread-safe!
int mpool_init(struct mpool *p, uint64_t chunk_size, uint64_t alloc_size) {
    p->chunk_size = chunk_size;
    p->alloc_size = alloc_size;
    p->head = NULL;
    pthread_spin_init(&p->lock, 0);

    // Alloc one chunk to trigger the initial chunk allocations
    void *c = mpool_alloc(p);
    if (!c)
        return -1;
    mpool_free(p, c);

    return 0;
}

// Not thread-safe!
// If not all the chunks have been freed yet, then memory leaks will occur!
void mpool_destroy(struct mpool *p) {
    struct mpool_chunk *c = p->head;
    pthread_spin_destroy(&p->lock);
    while (c) {
        struct mpool_chunk *n = c->next;
        free(c);
        c = n;
    }
}
