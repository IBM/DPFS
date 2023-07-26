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
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DPFS_HAL_NUM_QUEUES 64
#define DPFS_HAL_QUEUE_DEPTH 64
// The maximum number of outstanding requests the virtiofs consumer is allowed to have
#define DPFS_HAL_MAX_BACKGROUND (DPFS_HAL_NUM_QUEUES * DPFS_HAL_QUEUE_DEPTH)

// return int EWOULDBLOCK indicates that the done_ctx callback
// will be used to indicate when the request is fully handled
// return int 0 indicates that the request is fully handled and
// can be sent to the host
// the error field in the struct fuse_out_header should be 0 or a negative error value
typedef int (*dpfs_hal_handler_t) (void *user_data,
                                   struct iovec *fuse_in_iov, int in_iovcnt,
                                   struct iovec *fuse_out_iov, int out_iovcnt,
                                   void *completion_context, uint16_t device_id);
typedef void (*dpfs_hal_register_device_t) (void *user_data, uint16_t device_id);
typedef void (*dpfs_hal_unregister_device_t) (void *user_data, uint16_t device_id);

struct dpfs_hal_ops {
    dpfs_hal_handler_t request_handler;    
    // These two callbacks are called during dpfs_hal_new
    dpfs_hal_register_device_t register_device;    
    dpfs_hal_unregister_device_t unregister_device;    
};

struct dpfs_hal_params {
    struct dpfs_hal_ops ops;
    void *user_data; // Pointer to user data that gets passed with every dpfs_hal_handler
    const char *conf_path; // See the example toml files in the root of dpfs_hal
};

enum dpfs_hal_completion_status {
    DPFS_HAL_COMPLETION_SUCCES = 0,
    DPFS_HAL_COMPLETION_ERROR
};

// Not user-accessible
struct dpfs_hal;

// Returns the current thread id
// This should only be called from within the request handler context!!
uint16_t dpfs_hal_thread_id(void);
// Returns the total number of DPFS threads for request handling
uint16_t dpfs_hal_nthreads(struct dpfs_hal *);

// Optionally starts a background thread that handles the mock virtio-fs devices,
// which only get polled once a second. This should be set to true when not using
// `dpfs_hal_loop`!
struct dpfs_hal *dpfs_hal_new(struct dpfs_hal_params *params, bool start_mock_thread);
// DPFS will handle the polling for you (including mock devices), using the supplied interval in the params
void dpfs_hal_loop(struct dpfs_hal *hal);
// DPFS backend takes control over the polling, make sure you also call poll_mmio
// a few times a second to check for device management changes of the virtio-fs device
int dpfs_hal_poll_io(struct dpfs_hal *hal, uint16_t device);
// Poll on the management IO
void dpfs_hal_poll_mmio(struct dpfs_hal *hal, uint16_t device);
void dpfs_hal_destroy(struct dpfs_hal *hal);
int dpfs_hal_async_complete(void *completion_context, enum dpfs_hal_completion_status);

#ifdef __cplusplus
}
#endif

#endif // DPFS_HAL_H
