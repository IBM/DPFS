/*
#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

#ifndef LIST_H
#define LIST_H

#define list_entry(ptr, type, member)           \
    container_of(ptr, type, member)

struct list_head {
    struct list_head *next;
    struct list_head *prev;
};

static void init_list_head(struct list_head *list) {
    list->next = list;
    list->prev = list;
}

static int list_empty(const struct list_head *head) {
    return head->next == head;
}

static void list_add(struct list_head *e, struct list_head *prev,
             struct list_head *next) {
    next->prev = e;
    e->next = next;
    e->prev = prev;
    prev->next = e;
}

static inline void list_add_head(struct list_head *e, struct list_head *head) {
    list_add(e, head, head->next);
}

static inline void list_add_tail(struct list_head *e, struct list_head *head) {
    list_add(e, head->prev, head);
}

static inline void list_del(struct list_head *entry) {
    struct list_head *prev = entry->prev;
    struct list_head *next = entry->next;

    next->prev = prev;
    prev->next = next;
}
#endif // LIST_H