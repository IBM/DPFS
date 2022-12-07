/*
#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

#ifndef MPOOL2_H
#define MPOOL2_H

#include <stdint.h>
#include <ck_fifo.h>

/*
    mpool is a minimal thread-safe memory pool that can NOT increase in size,
    specialized for Single-Producer Single-Consumer
    it is basically a glorified concurrencykit fifo
*/

struct mpool2 {
    ck_fifo_spsc_t fifo;
    ck_fifo_spsc_entry_t fifo_head;
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
