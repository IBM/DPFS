/*
#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

#ifndef MPOOL_H
#define MPOOL_H

#include <stdint.h>
#include <ck_ring.h>

/*
    mpool is a minimal thread-safe memory pool that can NOT increase in size,
    specialized for Single-Producer Single-Consumer. It is basically a glorified concurrencykit fifo
    Because it cannot grow in size, the number of chunks shouuld be carefully selected based
    on the queue depth of your file system implementation.
*/

#define MPOOL_SIZE 256
struct mpool {
    ck_ring_t ring;
    ck_ring_buffer_t *buffer;
};

void *mpool_alloc(struct mpool *p);
void mpool_free(struct mpool *p, void *e);

/*
 chunks = the total amount of chunks that the pool contains
 chunk_size = the size of each chunk
 returns error code if unsuccesful
 */
int mpool_init(struct mpool **p, uint64_t chunk_size, uint64_t chunks);
void mpool_destroy(struct mpool *p);

#endif // MPOOL_H
