/*
#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

#ifndef MPOOL2_H
#define MPOOL2_H

#include <stdint.h>
#include <ck_ring.h>

/*
    mpool2 is a minimal thread-safe memory pool that can NOT increase in size,
    specialized for Single-Producer Single-Consumer
    it is basically a glorified concurrencykit fifo
*/

#define MPOOL2_SIZE 256
struct mpool2 {
    ck_ring_t ring;
    ck_ring_buffer_t *buffer;
};

void *mpool2_alloc(struct mpool2 *p);
void mpool2_free(struct mpool2 *p, void *e);

/*
 chunks = the total amount of chunks that the pool contains
 chunk_size = the size of each chunk
 returns error code if unsuccesful
 */
int mpool2_init(struct mpool2 *p, uint64_t chunk_size, uint64_t chunks);
void mpool2_destroy(struct mpool2 *p);

#endif // MPOOL_H
