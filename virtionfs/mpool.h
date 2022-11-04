/*
#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

#ifndef MPOOL_H
#define MPOOL_H

#include <stdint.h>

/*
    mpool is a minimal non-threadsafe memory pool that can increase in size, but not decrease(!)
    it is explicitly non-threadsafe as each thread should have its own pool for locality
*/

struct mpool_chunk {
    struct mpool_chunk *next;
};
struct mpool {
    uint64_t chunk_size;
    uint64_t alloc_size;
    struct mpool_chunk *head;
};

void *mpool_alloc(struct mpool *p);
void mpool_free(struct mpool *p, void *e);

/*
 size = the initial amount of chunks that the pool contains
 chunk_size = the size of each chunk
 alloc_size = the size with which the pool is extended if no more elements are available
 returns error code if unsuccesful
 */
int mpool_init(struct mpool *p, uint64_t chunk_size, uint64_t alloc_size);
void mpool_destroy(struct mpool *p);

#endif // MPOOL_H
