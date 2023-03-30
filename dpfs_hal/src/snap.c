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
#include <sched.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <err.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/stat.h>

#include "virtio_fs_controller.h"
#include "nvme_emu_log.h"
#include "compiler.h"
#include "mlnx_snap_pci_manager.h"
#include "hal.h"
#include "cpu_latency.h"
#include "toml.h"

struct dpfs_hal {
    struct virtio_fs_ctrl *snap_ctrl;
    dpfs_hal_handler_t request_handler;
    void *user_data;
    useconds_t polling_interval_usec;
    uint32_t nthreads;

#ifdef DEBUG_ENABLED
    uint32_t handlers_call_cnts[DPFS_HAL_FUSE_HANDLERS_LEN];
#endif
};

static volatile int keep_running = 1;
pthread_key_t dpfs_hal_thread_id_key;

static void signal_handler(int dummy)
{
    keep_running = 0;
}


__attribute__((visibility("default")))
int dpfs_hal_poll_io(struct dpfs_hal *hal, int thread_id) {
    return virtio_fs_ctrl_progress_io(hal->snap_ctrl, thread_id);
}

__attribute__((visibility("default")))
void dpfs_hal_poll_mmio(struct dpfs_hal *hal) {
    virtio_fs_ctrl_progress(hal->snap_ctrl);
}

static void dpfs_hal_loop_singlethreaded(struct virtio_fs_ctrl *ctrl, useconds_t interval, int thread_id)
{
    // Only one thread, thread_id=0
    pthread_setspecific(dpfs_hal_thread_id_key, (void *) 0);

    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = signal_handler;
    sigaction(SIGINT, &act, 0);
    sigaction(SIGPIPE, &act, 0);
    sigaction(SIGTERM, &act, 0);

    bool suspending = false;
    uint32_t count = 0;

    while (keep_running || !virtio_fs_ctrl_is_suspended(ctrl)) {
        /*
         * don't call usleep(0) because it adds a huge overhead
         * to polling.
         */
        if (interval > 0) {
            usleep(interval);
            // actual io
            virtio_fs_ctrl_progress_io(ctrl, thread_id);
            // This is for mmio (management io)
            virtio_fs_ctrl_progress(ctrl);
        } else {
            /*
             * poll submission queues as fast as we can
             * but don't spend resources on polling mmio
             */
            virtio_fs_ctrl_progress_io(ctrl, thread_id);
            if (count++ % 10000 == 0) {
                virtio_fs_ctrl_progress(ctrl);
            }
        }

        if (unlikely(!keep_running && !suspending)) {
            virtio_fs_ctrl_suspend(ctrl);
            suspending = true;
        }
    }
}

struct emu_ll_tdata {
    size_t thread_id;
    struct virtio_fs_ctrl *ctrl;
    useconds_t interval;
    pthread_t thread;
};

static void *dpfs_hal_loop_thread(void *arg)
{
    struct emu_ll_tdata *tdata = (struct emu_ll_tdata *)arg;

    // Store the thread_id in thread local storage so that the FUSE implementation
    // knows what thread number its in when called with a request
    pthread_setspecific(dpfs_hal_thread_id_key, (void *) tdata->thread_id);

    // poll as fast as we can! Someone else is doing mmio polling
    while (keep_running || !virtio_fs_ctrl_is_suspended(tdata->ctrl))
        virtio_fs_ctrl_progress_io(tdata->ctrl, tdata->thread_id);

    return NULL;
}

static void dpfs_hal_loop_multithreaded(struct virtio_fs_ctrl *ctrl,
                               int nthreads, useconds_t interval)
{
    struct emu_ll_tdata tdatas[nthreads];

    for (int i = 1; i < nthreads; i++) {
        tdatas[i].thread_id = i;
        tdatas[i].ctrl = ctrl;
        // Only the first thread does mmio polling (sometimes)
        if (pthread_create(&tdatas[i].thread, NULL, dpfs_hal_loop_thread, &tdatas[i])) {
            warn("Failed to create thread for io %d", i);
            for (int j = i - 1; j >= 0; j--) {
                pthread_cancel(tdatas[j].thread);
                pthread_join(tdatas[j].thread, NULL);
            }
        }
    }

    // The main thread also does mmio polling and signal handling
    dpfs_hal_loop_singlethreaded(ctrl, interval, 0);

    // The main thread exited, the other threads should exit soon
    // let's wait for them
    for (int i = 1; i < nthreads; i++) {
        pthread_join(tdatas[i].thread, NULL);
    }
}

__attribute__((visibility("default")))
void dpfs_hal_loop(struct dpfs_hal *emu)
{
    struct virtio_fs_ctrl *ctrl = emu->snap_ctrl;
    useconds_t interval = emu->polling_interval_usec;

    start_low_latency();
    
    if (emu->nthreads <= 1)
        dpfs_hal_loop_singlethreaded(ctrl, interval, 0);
    else { // Multithreaded mode
        dpfs_hal_loop_multithreaded(ctrl, emu->nthreads, interval);
    }

    stop_low_latency();
}

// Currently only supports SNAP
__attribute__((visibility("default")))
int dpfs_hal_async_complete(void *completion_context, enum dpfs_hal_completion_status status)
{
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
    struct dpfs_hal *hal = ctrl->virtiofs_emu;

    return hal->request_handler(hal->user_data, in_iov, in_iovcnt, out_iov, out_iovcnt, done_ctx);
}

__attribute__((visibility("default")))
struct dpfs_hal *dpfs_hal_new(struct dpfs_hal_params *params)
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
    toml_datum_t pf_id = toml_string_in(snap_conf, "pf_id");
    if (!pf_id.ok || pf_id.u.i < 0) {
        fprintf(stderr, "%s: pf_id requires a value >=0!"
                "Hint: use list_emulation_managers to find out the physical function id.", __func__);
        return NULL;
    }
    toml_datum_t vf_id = toml_string_in(snap_conf, "pf_id");
    if (!vf_id.ok || vf_id.u.i < -1) {
        fprintf(stderr, "%s: vf_id requires a value >=-1!"
                "Hint: -1 means no virtual function and directly run on the physical function.", __func__);
        return NULL;
    }
    toml_datum_t qd = toml_string_in(snap_conf, "queue_depth");
    if (!qd.ok || qd.u.i < 1|| (qd.u.i & (qd.u.i - 1))) {
        fprintf(stderr, "%s: queue_depth must be a power of 2 and >= 1!", __func__);
        return NULL;
    }
    toml_datum_t nthreads = toml_string_in(snap_conf, "nthreads");
    if (!nthreads.ok || nthreads.u.i < 0) {
        fprintf(stderr, "%s: nthreads must be >= 0!", __func__);
        return NULL;
    }
    toml_datum_t polling_interval = toml_string_in(snap_conf, "polling_interval_usec");
    if (!polling_interval.ok || polling_interval.u.i < 0 ) {
        fprintf(stderr, "%s: polling_interval_usec must be >= 0!", __func__);
        return NULL;
    }
    toml_datum_t tag = toml_string_in(snap_conf, "tag");
    if (!tag.ok) {
        fprintf(stderr, "%s: a virtio-fs file system tag in the form of a string must be supplied!"
                "This is the name with which the host mounts the file system", __func__);
        return NULL;
    }
    struct dpfs_hal *emu = calloc(sizeof(struct dpfs_hal), 1);

    emu->polling_interval_usec = polling_interval.u.i;
    emu->user_data = params->user_data;
    emu->request_handler = params->request_handler;
    emu->nthreads = nthreads.u.i;

    struct virtio_fs_ctrl_init_attr param;
    param.emu_manager_name = emu_manager.u.s;
    param.nthreads = nthreads.u.i;
    param.tag = tag.u.s;
    param.pf_id = pf_id.u.i;
    param.vf_id = vf_id.u.i;

    param.dev_type = "virtiofs_emu";
    // A queue per thread
    param.num_queues = 1 + nthreads.u.i;
    // Must be an order of 2 or you will get err 121
    // queue slots that are left unused significantly decrease performance because of the snap poller
    param.queue_depth = qd.u.i;
    param.force_in_order = false;
    // See snap_virtio_fs_ctrl.c:811, if enabled this controller is
    // supposed to be recovered from the dead
    param.recover = false;
    param.suspended = false;
    param.virtiofs_emu_handle_req = dpfs_hal_handle_req;
    param.vf_change_cb = NULL;
    param.vf_change_cb_arg = NULL;

    param.virtiofs_emu = emu;

    // Yes I know, we don't do NVMe here
    // But snap uses this nvme logger everywhere so 💁
    if (nvme_init_logger()) {
        goto out;
    }

    if (mlnx_snap_pci_manager_init()) {
        fprintf(stderr, "Failed to init emulation managers list\n");
        goto out;
    };

    emu->snap_ctrl = virtio_fs_ctrl_init(&param);
    if (!emu->snap_ctrl) {
        fprintf(stderr, "failed to initialize VirtIO-FS controller\n");
        goto clear_pci_list;
    }

    // Initialize the thread-local key we use to tell each of the Virtio
    // polling threads, which thread id it has
    if (pthread_key_create(&dpfs_hal_thread_id_key, NULL)) {
        fprintf(stderr, "Failed to create thread-local key for dpfs_hal threadid\n");
        goto clear_pci_list;
    }

    printf("VirtIO-FS device %s on emulation manager %s is ready\n",
               param.tag, emu_manager.u.s);

    return emu;

clear_pci_list:
    mlnx_snap_pci_manager_clear();
out:
    free(param.virtiofs_emu);
    return NULL;
}

__attribute__((visibility("default")))
void dpfs_hal_destroy(struct dpfs_hal *emu)
{
    printf("VirtIO-FS destroy controller %s\n", emu->snap_ctrl->sctx->context->device->name);

    virtio_fs_ctrl_destroy(emu->snap_ctrl);
    mlnx_snap_pci_manager_clear();
    free(emu);
}

#endif // HAVE_SNAP
