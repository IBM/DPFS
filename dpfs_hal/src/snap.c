/*
 *   Copyright © 2021 NVIDIA CORPORATION & AFFILIATES. ALL RIGHTS RESERVED.
 *   Copyriht 2023- IBM Inc. All rights reserved
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#if defined(HAVE_SNAP) && !defined(DPFS_RVFS)

#define _GNU_SOURCE
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <sched.h>
#include <stdint.h>
#include <stdbool.h>
#include <err.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/stat.h>
#include <sys/queue.h>

#include "virtio_fs_controller.h"
#include "nvme_emu_log.h"
#include "compiler.h"
#include "mlnx_snap_pci_manager.h"
#include "hal.h"
#include "cpu_latency.h"
#include "toml.h"

struct dpfs_hal_device {
    struct virtio_fs_ctrl *snap_ctrl;
    uint16_t device_id;
    uint16_t pf_id;
    char *tag;

    uint16_t poll_counter;
    bool suspending;

    struct dpfs_hal *hal;
};

struct dpfs_hal {
    int ndevices;
    struct dpfs_hal_device *devices;
    int nmock_devices;
    struct dpfs_hal_device *mock_devices;
    pthread_t mock_thread;
    bool mock_thread_running;

    struct dpfs_hal_ops ops;
    void *user_data;
    useconds_t polling_interval_usec;
    uint16_t nthreads;
};

static volatile int keep_running = 1;

pthread_key_t dpfs_hal_thread_id_key;
__attribute__((visibility("default")))
uint16_t dpfs_hal_thread_id(void) {
    return (uint16_t) (size_t) pthread_getspecific(dpfs_hal_thread_id_key);
}
__attribute__((visibility("default")))
uint16_t dpfs_hal_nthreads(struct dpfs_hal *hal)
{
    return hal->nthreads;
}

static void signal_handler(int dummy)
{
    keep_running = 0;
    printf("DPFS-HAL SNAP: the HAL will exit when the host has suspended all the virtio-fs devices");
}

__attribute__((visibility("default")))
int dpfs_hal_poll_io(struct dpfs_hal *hal, uint16_t device_id)
{
    if (device_id < hal->ndevices)
        return virtio_fs_ctrl_progress_all_io(hal->devices[device_id].snap_ctrl);
    else
        return -ENODEV;
}

__attribute__((visibility("default")))
void dpfs_hal_poll_mmio(struct dpfs_hal *hal, uint16_t device_id)
{
    if (device_id < hal->ndevices)
        virtio_fs_ctrl_progress(hal->devices[device_id].snap_ctrl);
}

static void dpfs_hal_poll_device(struct dpfs_hal_device *dev)
{
    struct dpfs_hal *hal = dev->hal;

    /*
     * don't call usleep(0) because it adds a huge overhead
     * to polling.
     */
    if (hal->polling_interval_usec > 0) {
        usleep(hal->polling_interval_usec);
        // actual io
        virtio_fs_ctrl_progress_all_io(dev->snap_ctrl);
        // This is for mmio (management io)
        virtio_fs_ctrl_progress(dev->snap_ctrl);
    } else {
        /*
         * poll submission queues as fast as we can
         * but don't spend resources on polling mmio
         */
        virtio_fs_ctrl_progress_all_io(dev->snap_ctrl);
        if (dev->poll_counter++ == 10000) {
            virtio_fs_ctrl_progress(dev->snap_ctrl);
            dev->poll_counter = 0;
        }
    }

    if (unlikely(!keep_running && !dev->suspending)) {
        virtio_fs_ctrl_suspend(dev->snap_ctrl);
        dev->suspending = true;
    }
}

static bool all_devices_suspended(struct dpfs_hal *hal)
{
    uint16_t n = 0;
    for (uint16_t i = 0; i < hal->ndevices; i++) {
        if (virtio_fs_ctrl_is_suspended(hal->devices[i].snap_ctrl))
            n++;
    }
    return n == hal->ndevices;
}

// Checks for (management) I/O every second on all mock devices
static void *dpfs_hal_mock_thread(void *arg)
{
    struct dpfs_hal *hal = arg;

    while (keep_running || !all_devices_suspended(hal)) {
        for (size_t i = 0; i < hal->nmock_devices; i++) {
            struct dpfs_hal_device *dev = &hal->mock_devices[i];
            // actual io
            virtio_fs_ctrl_progress_all_io(dev->snap_ctrl);
            // This is for mmio (management io)
            virtio_fs_ctrl_progress(dev->snap_ctrl);

            if (unlikely(!keep_running && !dev->suspending)) {
                virtio_fs_ctrl_suspend(dev->snap_ctrl);
                dev->suspending = true;
            }
        }
        sleep(1);
    }

    return NULL;
}

struct dpfs_hal_loop_thread {
    pthread_t thread;
    size_t thread_id;
    struct dpfs_hal *hal;
};

static void *dpfs_hal_loop_static_thread(void *arg)
{
    struct dpfs_hal_loop_thread *ht = arg;
    struct dpfs_hal *hal = ht->hal;

    // Store the thread_id in thread local storage so that the FUSE implementation
    // knows what thread number its in when called with a request
    pthread_setspecific(dpfs_hal_thread_id_key, (void *) ht->thread_id);

    long num_cpus = sysconf(_SC_NPROCESSORS_CONF);
    cpu_set_t loop_cpu;
    CPU_ZERO(&loop_cpu);
    // Calculate our polling CPU
    // with two threads and 8 total cores:
    // thread 0 will occupy core 7
    // thread 1 will occupy core 6
    CPU_SET(num_cpus - 1 - ht->thread_id, &loop_cpu);
    int ret = sched_setaffinity(gettid(), sizeof(loop_cpu), &loop_cpu);
    if (ret == -1) {
        warn("Could not set the CPU affinity of polling thread %lu. DPFS thread %lu will continue not pinned.", ht->thread_id, ht->thread_id);
    }

    size_t ndevices = hal->ndevices / hal->nthreads;
    size_t remainder = hal->ndevices % hal->nthreads;
    size_t devices_start = ndevices * ht->thread_id;
    size_t devices_end = devices_start + ndevices;
    if (ht->thread_id == 0 && remainder != 0) {
        // Extend our window to the right with the remainder devices
        devices_end += remainder;
        ndevices += remainder;
    } else if (remainder != 0) {
        // Move our window to the right by the number of devices T0 has
        devices_start += remainder;
        devices_end += remainder;
    }

    while (keep_running || !all_devices_suspended(hal)) {
        for (size_t i = devices_start; i < devices_end; i++) {
            dpfs_hal_poll_device(&hal->devices[i]);
        }
    }

    return NULL;
}

static void dpfs_hal_loop_static(struct dpfs_hal *hal)
{
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = signal_handler;
    sigaction(SIGINT, &act, 0);
    sigaction(SIGPIPE, &act, 0);
    sigaction(SIGTERM, &act, 0);

    // Make sure the mock thread is not already running because of invalid usage of the HAL API 
    if (!hal->mock_thread_running && hal->nmock_devices > 0) {
        if (pthread_create(&hal->mock_thread, NULL, dpfs_hal_mock_thread, hal)) {
            warn("Failed to create thread for mock devices mmio");
            return;
        }
        hal->mock_thread_running = true;
    }

    struct dpfs_hal_loop_thread tdatas[hal->nthreads];
    for (int i = 0; i < hal->nthreads; i++) {
        tdatas[i].thread_id = i;
        tdatas[i].hal = hal;
        // Only the first thread does mmio polling (sometimes)
        if (pthread_create(&tdatas[i].thread, NULL, dpfs_hal_loop_static_thread, &tdatas[i])) {
            warn("Failed to create thread for io %d", i);
            for (int j = 0; j < i; i++) {
                pthread_cancel(tdatas[j].thread);
            }
            return;
        }
    }



    printf("DPFS-HAL SNAP: All device pollers are up and running.\n");

    // Wait for all the polling threads to stop
    for (int i = 0; i < hal->nthreads; i++) {
        pthread_join(tdatas[i].thread, NULL);
    }
    if (hal->nmock_devices > 0) {
        pthread_join(hal->mock_thread, NULL);
        hal->mock_thread_running = false;
    }
}

__attribute__((visibility("default")))
void dpfs_hal_loop(struct dpfs_hal *hal)
{
    start_low_latency();

    // Currently only static scheduling is supported
    dpfs_hal_loop_static(hal);

    stop_low_latency();
}

/*
struct dpfs_hal_completion_ctx {
    struct snap_fs_dev_io_done_ctx *snap;
    struct dpfs_hal_ctrl *ctrl;
}; */

// Currently only supports SNAP
__attribute__((visibility("default")))
int dpfs_hal_async_complete(void *completion_context, enum dpfs_hal_completion_status status)
{
    // Increment IO counter
    // If timer is > 1 sec, calc the IOPS and reset the counter
    struct snap_fs_dev_io_done_ctx *cb = completion_context;
    enum snap_fs_dev_op_status snap_status = SNAP_FS_DEV_OP_IO_ERROR;
    switch (status) {
        case DPFS_HAL_COMPLETION_SUCCES:
            snap_status = SNAP_FS_DEV_OP_SUCCESS;
            break;
        case DPFS_HAL_COMPLETION_ERROR:
            snap_status = SNAP_FS_DEV_OP_IO_ERROR;
            break;
    }
    cb->cb(snap_status, cb->user_arg);
    return 0;
}

static int dpfs_hal_handle_req(struct virtio_fs_ctrl *ctrl,
                            struct iovec *in_iov, int in_iovcnt,
                            struct iovec *out_iov, int out_iovcnt,
                            struct snap_fs_dev_io_done_ctx *done_ctx)
{
    struct dpfs_hal_device *dev = ctrl->virtiofs_emu;
    struct dpfs_hal *hal = dev->hal;

    return hal->ops.request_handler(hal->user_data, in_iov, in_iovcnt, out_iov, out_iovcnt, done_ctx, dev->device_id);
}

static int dpfs_hal_init_dev(struct dpfs_hal *hal, struct dpfs_hal_device *dev, uint16_t device_id,
        char *emu_manager, int pf_id, char *tag, int qd)
{
    char *full_tag;
    int ret = asprintf(&full_tag, "%s-%u", tag, device_id);
    if (ret == -1 || !full_tag) {
        fprintf(stderr, "%s: couldn't allocate memory for virtio-fs tag", __func__);
        return -1;
    }

    struct virtio_fs_ctrl_init_attr param;
    param.emu_manager_name = emu_manager;
    // A single device could be shared amongst multiple polling threads, but because of locking
    // in SNAP, there is no point in doing this. So a device is owned by a single thread
    param.nthreads = 1;
    param.tag = full_tag;
    param.pf_id = pf_id;
    param.vf_id = -1;

    param.dev_type = "virtiofs_emu";
    // one for HiPrio and one for Requests
    param.num_queues = 2;
    // Must be an order of 2 or you will get err 121
    // queue slots that are left unused significantly decrease performance because of the SNAP poller
    param.queue_depth = qd;
    param.force_in_order = false;
    // See snap_virtio_fs_ctrl.c:811, if enabled this controller is
    // supposed to be recovered from the dead
    param.recover = false;
    param.suspended = false;
    param.virtiofs_emu_handle_req = dpfs_hal_handle_req;
    param.vf_change_cb = NULL;
    param.vf_change_cb_arg = NULL;
    param.virtiofs_emu = dev;

    struct virtio_fs_ctrl *snap_ctrl = virtio_fs_ctrl_init(&param);
    if (!snap_ctrl) {
        fprintf(stderr, "failed to initialize virtio-fs device using SNAP on PF %u\n", param.pf_id);
        free(full_tag);
        return -1;
    }

    dev->snap_ctrl = snap_ctrl;
    dev->device_id = device_id;
    dev->pf_id = pf_id;
    dev->hal = hal;
    dev->tag = full_tag;

    if (hal->ops.register_device)
        hal->ops.register_device(hal->user_data, device_id);

    return 0;
}

static void dpfs_hal_destroy_dev(struct dpfs_hal_device *dev)
{
    struct dpfs_hal *hal = dev->hal;

    if (hal->ops.unregister_device)
        hal->ops.unregister_device(hal->user_data, dev->device_id);

    virtio_fs_ctrl_destroy(dev->snap_ctrl);
    free(dev->tag);
}

__attribute__((visibility("default")))
struct dpfs_hal *dpfs_hal_new(struct dpfs_hal_params *params, bool start_mock_thread)
{
    FILE *fp;
    char errbuf[200];
    
    // 1. Read and parse toml file
    fp = fopen(params->conf_path, "r");
    if (!fp) {
        fprintf(stderr, "%s: cannot open %s - %s", __func__,
                params->conf_path, strerror(errno));
        return NULL;
    }
    
    toml_table_t *conf = toml_parse_file(fp, errbuf, sizeof(errbuf));
    fclose(fp);
    
    if (!conf) {
        fprintf(stderr, "%s: cannot parse - %s", __func__, errbuf);
        return NULL;
    }
    
    // 2. Traverse to a table.
    toml_table_t *snap_conf = toml_table_in(conf, "snap_hal");
    if (!snap_conf) {
        fprintf(stderr, "%s: missing [snap_hal] in hal config", __func__);
        return NULL;
    }
    
    toml_datum_t emu_manager = toml_string_in(snap_conf, "emu_manager");
    if (!emu_manager.ok) {
        fprintf(stderr, "%s: emu_manager is required!\n", __func__);
        fprintf(stderr, "Hint: Enable virtiofs emulation in the firmware (see docs) and"
                        "run `list_emulation_managers` to find"
                        "out what emulation manager name to supply.");
        return NULL;
    }
    toml_array_t *pf_ids = toml_array_in(snap_conf, "pf_ids");
    if (!pf_ids || toml_array_nelem(pf_ids) < 1 || toml_array_nelem(pf_ids) > UINT16_MAX || toml_array_kind(pf_ids) != 'v') {
        fprintf(stderr, "%s: pf_ids is required and must be an array of integer values!"
                " Hint: use list_emulation_managers to find out the physical function id.\n", __func__);
        return NULL;
    }
    for (int i = 0; i < toml_array_nelem(pf_ids); i++) {
        toml_datum_t pf = toml_int_at(pf_ids, i);
        if (!pf.ok || pf.u.i < 0) {
            fprintf(stderr, "%s: All physical function ids must be >= 0 integers!"
                "Hint: use list_emulation_managers to find out the physical function id.\n", __func__);
            return NULL;
        }
    }
    toml_datum_t qd = toml_int_in(snap_conf, "queue_depth");
    if (!qd.ok || qd.u.i < 1|| (qd.u.i & (qd.u.i - 1))) {
        fprintf(stderr, "%s: queue_depth must be a power of 2 and >= 1\n!", __func__);
        return NULL;
    }
    toml_datum_t nthreads = toml_int_in(snap_conf, "nthreads");
    if (!nthreads.ok || nthreads.u.i < 1) {
        fprintf(stderr, "%s: nthreads must be >= 1!", __func__);
        return NULL;
    }
    if (nthreads.u.i > toml_array_nelem(pf_ids)) {
        fprintf(stderr, "%s: nthreads value invalid! there cannot be more threads than virtio-fs devices\n", __func__);
        return NULL;
    }
    toml_datum_t polling_interval = toml_int_in(snap_conf, "polling_interval_usec");
    if (!polling_interval.ok || polling_interval.u.i < 0 ) {
        fprintf(stderr, "%s: polling_interval_usec must be >= 0\n!", __func__);
        return NULL;
    }
    toml_datum_t tag = toml_string_in(snap_conf, "tag");
    if (!tag.ok) {
        fprintf(stderr, "%s: a virtio-fs file system tag in the form of a string must be supplied!"
                "This is the name with which the host mounts the file system\n", __func__);
        return NULL;
    }
    toml_array_t *mock_pf_ids = toml_array_in(snap_conf, "mock_pf_ids"); // optional
    for (int i = 0; i < toml_array_nelem(mock_pf_ids); i++) {
        toml_datum_t mock_pf = toml_int_at(mock_pf_ids, i);
        if (!mock_pf.ok || mock_pf.u.i < 0) {
            fprintf(stderr, "%s: All mock physical function ids must be >= 0 integers!"
                " Hint: use list_emulation_managers to find out the physical function id.\n", __func__);
            return NULL;
        }
        bool found = false;
        for (int j = 0; j < toml_array_nelem(pf_ids); j++) {
            if (mock_pf.u.i == toml_int_at(pf_ids, j).u.i) {
                found = true;
                break;
            }
        }
        if (found) {
            fprintf(stderr, "%s: All mock physical function ids not also be present in `pf_ids`!", __func__);
            return NULL;
        }
    }
    if (!mock_pf_ids || toml_array_nelem(mock_pf_ids) == 0)
        mock_pf_ids = NULL;

    struct dpfs_hal *hal = calloc(1, sizeof(struct dpfs_hal));
    hal->polling_interval_usec = polling_interval.u.i;
    hal->user_data = params->user_data;
    hal->ops = params->ops;
    hal->nthreads = nthreads.u.i;
    hal->ndevices = toml_array_nelem(pf_ids);
    hal->devices = calloc(hal->ndevices, sizeof(*hal->devices));
    if (mock_pf_ids) {
        hal->nmock_devices = toml_array_nelem(mock_pf_ids);
        hal->mock_devices = calloc(hal->nmock_devices, sizeof(*hal->mock_devices));
    }

    // Initialize the thread-local key we use to tell each of the Virtio
    // polling threads, which thread id it has
    if (pthread_key_create(&dpfs_hal_thread_id_key, NULL)) {
        fprintf(stderr, "Failed to create thread-local key for dpfs_hal threadid\n");
        goto out;
    }

    // Yes I know, we don't do NVMe here
    // But SNAP uses this nvme logger everywhere so 💁
    if (nvme_init_logger()) {
        goto out;
    }

    if (mlnx_snap_pci_manager_init()) {
        fprintf(stderr, "Failed to init emulation managers list\n");
        goto out;
    };

    uint16_t device_id = 0;
    for (uint16_t i = 0; i < hal->ndevices; i++) {
        toml_datum_t pf = toml_int_at(pf_ids, i);

        struct dpfs_hal_device *dev = &hal->devices[i];
        int ret = dpfs_hal_init_dev(hal, dev, device_id, emu_manager.u.s, pf.u.i, tag.u.s, qd.u.i);
        if (ret) {
            for (uint16_t j = 0; j < i; j++) {
                dpfs_hal_destroy_dev(&hal->devices[j]);
            }
            goto clear_pci_list;
        }
        device_id++;
    }

    for (uint16_t i = 0; i < hal->nmock_devices; i++) {
        toml_datum_t pf = toml_int_at(mock_pf_ids, i);

        struct dpfs_hal_device *dev = &hal->mock_devices[i];
        int ret = dpfs_hal_init_dev(hal, dev, device_id, emu_manager.u.s, pf.u.i, tag.u.s, qd.u.i);
        if (ret) {
            for (uint16_t j = 0; j < i; j++) {
                dpfs_hal_destroy_dev(&hal->mock_devices[j]);
            }
            for (uint16_t j = 0; j < hal->ndevices; j++) {
                dpfs_hal_destroy_dev(&hal->devices[j]);
            }
            goto clear_pci_list;
        }
        device_id++;
    }

    if (start_mock_thread && hal->nmock_devices > 0) {
        if (pthread_create(&hal->mock_thread, NULL, dpfs_hal_mock_thread, hal)) {
            warn("Failed to create thread for mock devices\n");
            for (uint16_t i = 0; i < hal->nmock_devices; i++) {
                dpfs_hal_destroy_dev(&hal->mock_devices[i]);
            }
            for (uint16_t i = 0; i < hal->ndevices; i++) {
                dpfs_hal_destroy_dev(&hal->devices[i]);
            }
            goto clear_pci_list;
        }
        hal->mock_thread_running = true;
    }

    printf("DPFS HAL with SNAP frontend online!\n");
    printf("The virtio-fs device with tag \"%s\" is running on emulation manager \"%s\" (",
        tag.u.s, emu_manager.u.s);
    printf("PF%u", hal->devices[0].pf_id);
    for (uint16_t i = 1; i < hal->ndevices; i++)
            printf(", PF%u", hal->devices[i].pf_id);
    printf(") and ready to be consumed by the host\n");

    return hal;

clear_pci_list:
    mlnx_snap_pci_manager_clear();
out:
    free(tag.u.s);
    free(emu_manager.u.s);
    free(hal->devices);
    if (hal->mock_devices)
        free(hal->mock_devices);
    free(hal);
    toml_free(conf);
    return NULL;
}

__attribute__((visibility("default")))
void dpfs_hal_destroy(struct dpfs_hal *hal)
{
    printf("DPFS HAL destroying %d virtio-fs devices on SNAP RDMA device %s\n", hal->ndevices + hal->nmock_devices,
            hal->devices[0].snap_ctrl->sctx->context->device->name);

    if (hal->mock_thread_running) {
        keep_running = false;
        pthread_join(hal->mock_thread, NULL);
    }

    for (uint16_t i = 0; i < hal->ndevices; i++) {
        dpfs_hal_destroy_dev(&hal->devices[i]);
    }
    for (uint16_t i = 0; i < hal->nmock_devices; i++) {
        dpfs_hal_destroy_dev(&hal->mock_devices[i]);
    }
    mlnx_snap_pci_manager_clear();

    free(hal->devices);
    if (hal->mock_devices)
        free(hal->mock_devices);
    free(hal);
}

#endif // HAVE_SNAP
