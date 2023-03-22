/*
#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

#ifndef DPFS_HAL_H
#define DPFS_HAL_H

#include <pthread.h>
#include <sys/uio.h>
#include <unistd.h>
#include <stdint.h>


#define DPFS_HAL_NUM_QUEUES 64
#define DPFS_HAL_QUEUE_DEPTH 64
// The maximum number of outstanding requests the virtiofs consumer is allowed to have
#define DPFS_HAL_MAX_BACKGROUND (DPFS_HAL_NUM_QUEUES * DPFS_HAL_QUEUE_DEPTH)

// return int EWOULDBLOCK indicates that the done_ctx callback
// will be used to indicate when the request is fully handled
// return int 0 indicates that the request is fully handled and
// can be sent to the host
typedef int (*dpfs_hal_handler_t) (void *user_data,
                                   struct iovec *fuse_in_iov, int in_iovcnt,
                                   struct iovec *fuse_out_iov, int out_iovcnt,
                                   void *completion_context);

struct virtiofs_emu_params {
    useconds_t polling_interval_usec; // Time between every poll
    int pf_id; // Physical function ID
    int vf_id; // Virtual function ID
    char *emu_manager; // Emulation manager, DPU specific
    // Amount of polling threads 0 for single threaded mode, >0 for multithreaded mode
    // Multithreaded not supported currently!
    uint32_t nthreads;
    // Must be a power of 2!
    uint32_t queue_depth;
    char *tag; // Filesystem tag (i.e. the name of the virtiofs device to mount for the host)
};

struct dpfs_hal_params {
    dpfs_hal_handler_t request_handler;
    void *user_data; // Pointer to user data that gets passed with every dpfs_hal_handler
    struct virtiofs_emu_params emu_params;
};

enum dpfs_hal_completion_status {
    DPFS_HAL_COMPLETION_SUCCES = 0,
    DPFS_HAL_COMPLETION_ERROR
};

// Not user-accessible
struct dpfs_hal;

extern pthread_key_t dpfs_hal_thread_id_key;

struct dpfs_hal *dpfs_hal_new(struct dpfs_hal_params *params);
// DPFS will handle the polling for you, using the supplied interval in the params
void dpfs_hal_loop(struct dpfs_hal *hal);
// DPFS backend takes control over the polling, make sure you also call poll_mmio
// a few times a second to check for device management changes of the virtio-fs device
int dpfs_hal_poll_io(struct dpfs_hal *hal, int thread_id);
// Poll on the management IO
void dpfs_hal_poll_mmio(struct dpfs_hal *hal);
void dpfs_hal_destroy(struct dpfs_hal *hal);
int dpfs_hal_async_complete(void *completion_context, enum dpfs_hal_completion_status);

#endif // DPFS_HAL_H
