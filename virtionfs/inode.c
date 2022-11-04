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
#include <stdlib.h>
#include <err.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <fcntl.h>
#include <nfsc/libnfs-raw-nfs4.h>

#include "inode.h"
#include "virtiofs_emu_ll.h"
#include "common.h"

static struct inode *inode_new(fattr4_fileid fileid) {
    struct inode *i = calloc(1, sizeof(struct inode));
    if (!i)
        return NULL;

    i->fileid = fileid;
    // We keep the fh at 0, aka no fh

    return i;
}

static void inode_destroy(struct inode *i) {
    free(i);
}

int inode_table_init(struct inode_table *t) {
    t->size = INODE_TABLE_SIZE;
    t->array = (struct inode **) calloc(1, sizeof(struct inode *) * t->size);
    if (t->array == NULL) {
        fprintf(stderr, "Could not allocate memory for inode_table.array!\n");
        return -1;
    }
    pthread_mutex_init(&t->m, NULL);

    return 0;
}

void inode_table_destroy(struct inode_table *t) {
    for (size_t i = 0; i < t->size; i++) {
        if (t->array[i]) {
            struct inode *next = t->array[i]->next;
            inode_destroy(t->array[i]);
            while (next) {
                struct inode *old_next = next;
                next = next->next;
                inode_destroy(old_next);
            }
        }
    }

    pthread_mutex_destroy(&t->m);
    free(t->array);
    free(t);
}

static size_t inode_table_hash(struct inode_table *t, fattr4_fileid fileid) {
    return fileid % t->size;
}

struct inode *inode_table_get(struct inode_table *t, fattr4_fileid fileid) {
    size_t hash = inode_table_hash(t, fileid);

    for (struct inode *inode = t->array[hash]; inode != NULL; inode = inode->next)
        if (inode->fileid == fileid)
            return inode;

    return NULL;
}

struct inode *inode_table_getsert(struct inode_table *t, fattr4_fileid fileid) {
    size_t hash = inode_table_hash(t, fileid);

    struct inode *i;
    pthread_mutex_lock(&t->m);
    for (i = t->array[hash]; i != NULL; i = i->next)
        if (i->fileid == fileid)
            goto ret;

    i = inode_new(fileid);
    if (!i)
        goto ret;

    if (t->array[hash] != NULL)
        i->next = t->array[hash];
    t->array[hash] = i;

ret:
    pthread_mutex_unlock(&t->m);
    return i;
}

static struct inode *inode_table_remove(struct inode_table *t, fattr4_fileid fileid) {
    size_t hash = inode_table_hash(t, fileid);

    struct inode *prev = NULL;
    for (struct inode *inode = t->array[hash]; inode != NULL; inode = inode->next) {
        if (inode->fileid == fileid) {
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

// TODO call on forget
bool inode_table_erase(struct inode_table *t, fattr4_fileid fileid) {
    pthread_mutex_lock(&t->m);

    struct inode *i = inode_table_remove(t, fileid);
    if (i)
        inode_destroy(i);

    pthread_mutex_unlock(&t->m);
    return i;
}

