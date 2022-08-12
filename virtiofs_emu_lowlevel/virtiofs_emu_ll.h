/*
#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

#ifndef VIRTIOFS_EMU_LL_H
#define VIRTIOFS_EMU_LL_H

#include <linux/fuse.h>
#include <unistd.h>

#include "virtio_fs_controller.h"

#define VIRTIOFS_EMU_LL_FUSE_MAX_OPCODE FUSE_REMOVEMAPPING
// The opcodes begin at FUSE_LOOKUP = 1, so need one more array index
#define VIRTIOFS_EMU_LL_FUSE_HANDLERS_LEN VIRTIOFS_EMU_LL_FUSE_MAX_OPCODE+1
#define VIRTIOFS_EMU_LL_NUM_QUEUES 64
#define VIRTIOFS_EMU_LL_QUEUE_DEPTH 64

typedef int (*virtiofs_emu_ll_handler_t) (void *user_data,
                            struct iovec *fuse_in_iov, int in_iovcnt,
                            struct iovec *fuse_out_iov, int out_iovcnt,
                            struct snap_fs_dev_io_done_ctx *cb);


struct virtiofs_emu_params {
    useconds_t polling_interval_usec; // Time between every poll
    int pf_id; // Physical function ID
    int vf_id; // Virtual function ID
    char *emu_manager; // Emulation manager
    // Amount of polling threads 0 for single threaded mode, >0 for multithreaded mode
    // Multithreaded not supported currently!
    uint32_t nthreads;
    char *tag; // Filesystem tag (i.e. the name of the virtiofs device to mount for the host)
};

struct virtiofs_emu_ll_params {
    virtiofs_emu_ll_handler_t fuse_handlers[VIRTIOFS_EMU_LL_FUSE_HANDLERS_LEN];
    void *user_data; // Pointer to user data that gets passed with every virtiofs_emu_ll_handler
    struct virtiofs_emu_params emu_params;
};
// Non-user accesible
struct virtiofs_emu_ll;

struct virtiofs_emu_ll *virtiofs_emu_ll_new(struct virtiofs_emu_ll_params *params);
void virtiofs_emu_ll_loop(struct virtiofs_emu_ll *emu);
void virtiofs_emu_ll_destroy(struct virtiofs_emu_ll *emu);

#endif // VIRTIOFS_EMU_LL_H