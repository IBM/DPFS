/*
#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

#include <errno.h>
#include <stdlib.h>

#include "mpool.h"

void *mpool_alloc(struct mpool *p) {
    if (p->head) {
        void *e = p->head;        
        if (p->head->next)
            p->head = p->head->next;
        return e;
    } else {
        for (int i = 0; i < p->alloc_size-1; i++) {
            void *c = malloc(p->chunk_size);
            if (!c) {
                if (i > 0) // if malloc failed we return one of the previously malloc'ed chunks
                    return mpool_alloc(p);
                else // or completely fail
                    return NULL;
            }
            mpool_free(p, c);
        }
        void *c = malloc(p->chunk_size);
        if (!c) // if malloc failed we return one of the previously malloc'ed chunks
            return mpool_alloc(p);
        else
            return c;
    }

}

void mpool_free(struct mpool *p, void *e) {
    struct mpool_chunk *chunk = (struct mpool_chunk *) e;
    chunk->next = p->head;
    p->head = e;
}

int mpool_init(struct mpool *p, uint64_t size, uint64_t chunk_size, uint64_t alloc_size) {
    p->chunk_size = chunk_size;
    p->alloc_size = alloc_size;
    p->head = NULL;

    // Alloc one chunk to trigger the initial chunk allocations
    void *c = mpool_alloc(p);
    if (!c)
        return -1;
    mpool_free(p, c);

    return 0;
}

// If not all the chunks have been freed yet, then memory leaks will occur!
void mpool_destroy(struct mpool *p) {
    struct mpool_chunk *c = p->head;
    while (c) {
        struct mpool_chunk *n = c->next;
        free(c);
        c = n;
    }
}
