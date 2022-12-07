/*
#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <ck_fifo.h>

#include "mpool2.h"

// Thread-safe
void *mpool2_alloc(struct mpool2 *p) {
    void *r = NULL;
    ck_fifo_spsc_dequeue(&p->fifo, r);
    return r;
}

// Thread-safe
void mpool2_free(struct mpool2 *p, void *e) {
    ck_fifo_spsc_enqueue(&p->fifo, e, e);
}

#define MIN(x, y) x < y ? x : y

// Not thread-safe!
int mpool2_init(struct mpool2 *p, uint64_t chunk_size, uint64_t chunks) {
    chunk_size = MIN(sizeof(struct ck_fifo_spsc_entry), chunk_size);

    for (int i = 0; i < chunks; i++) {
        void *c = malloc(chunk_size);
        if (!c) {
            // TODO
        }
        ck_fifo_spsc_enqueue(&p->fifo, c, c);
    }

    return 0;
}

// Not thread-safe!
// If not all the chunks have been freed yet, then memory leaks will occur!
void mpool2_destroy(struct mpool2 *p) {
    void *c = NULL;
    while (ck_fifo_spsc_dequeue(&p->fifo, c)) {
        free(c);
    }
}
