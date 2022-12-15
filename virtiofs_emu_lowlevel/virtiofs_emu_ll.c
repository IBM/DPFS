/*
 *   Copyright © 2021 NVIDIA CORPORATION & AFFILIATES. ALL RIGHTS RESERVED.
 *   Copyriht 2022- IBM Inc. All rights reserved
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
#include <linux/fuse.h>
#include <sys/stat.h>

#include "virtio_fs_controller.h"
#include "nvme_emu_log.h"
#include "compiler.h"
#include "mlnx_snap_pci_manager.h"
#include "virtiofs_emu_ll.h"

struct virtiofs_emu_ll {
    struct virtio_fs_ctrl *snap_ctrl;
    virtiofs_emu_ll_handler_t handlers[VIRTIOFS_EMU_LL_FUSE_HANDLERS_LEN];
    void *user_data;
    useconds_t polling_interval_usec;
    uint32_t nthreads;

#ifdef DEBUG_ENABLED
    uint32_t handlers_call_cnts[VIRTIOFS_EMU_LL_FUSE_HANDLERS_LEN];
#endif
};

static volatile int keep_running = 1;
pthread_key_t virtiofs_thread_id_key;

void signal_handler(int dummy)
{
    keep_running = 0;
}

static void virtiofs_emu_ll_loop_singlethreaded(struct virtio_fs_ctrl *ctrl, useconds_t interval, int thread_id)
{
    // Only one thread, thread_id=0
    pthread_setspecific(virtiofs_thread_id_key, (void *) 0);

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

static void *virtiofs_emu_ll_loop_thread(void *arg)
{
    struct emu_ll_tdata *tdata = (struct emu_ll_tdata *)arg;

    // Store the thread_id in thread local storage so that the FUSE implementation
    // knows what thread number its in when called with a request
    pthread_setspecific(virtiofs_thread_id_key, (void *) tdata->thread_id);

    // poll as fast as we can! Someone else is doing mmio polling
    while (keep_running || !virtio_fs_ctrl_is_suspended(tdata->ctrl))
        virtio_fs_ctrl_progress_io(tdata->ctrl, tdata->thread_id);

    return NULL;
}

static void virtiofs_emu_ll_loop_multithreaded(struct virtio_fs_ctrl *ctrl,
                               int nthreads, useconds_t interval)
{
    struct emu_ll_tdata tdatas[nthreads];

    for (int i = 1; i < nthreads; i++) {
        tdatas[i].thread_id = i;
        tdatas[i].ctrl = ctrl;
        // Only the first thread does mmio polling (sometimes)
        if (pthread_create(&tdatas[i].thread, NULL, virtiofs_emu_ll_loop_thread, &tdatas[i])) {
            warn("Failed to create thread for io %d", i);
            for (int j = i - 1; j >= 0; j--) {
                pthread_cancel(tdatas[j].thread);
                pthread_join(tdatas[j].thread, NULL);
            }
        }
    }

    // The main thread also does mmio polling and signal handling
    virtiofs_emu_ll_loop_singlethreaded(ctrl, interval, 0);

    // The main thread exited, the other threads should exit soon
    // let's wait for them
    for (int i = 1; i < nthreads; i++) {
        pthread_join(tdatas[i].thread, NULL);
    }
}

void virtiofs_emu_ll_loop(struct virtiofs_emu_ll *emu)
{
    struct virtio_fs_ctrl *ctrl = emu->snap_ctrl;
    useconds_t interval = emu->polling_interval_usec;
    
    if (emu->nthreads <= 1)
        virtiofs_emu_ll_loop_singlethreaded(ctrl, interval, 0);
    else { // Multithreaded mode
        virtiofs_emu_ll_loop_multithreaded(ctrl, emu->nthreads, interval);
    }
}

static int virtiofs_emu_ll_fuse_unknown(void *user_data,
                            struct iovec *fuse_in_iov, int in_iovcnt,
                            struct iovec *fuse_out_iov, int out_iovcnt,
                            struct snap_fs_dev_io_done_ctx *cb) {
    struct fuse_in_header *in_hdr = (struct fuse_in_header *) fuse_in_iov[0].iov_base;
    struct fuse_out_header *out_hdr = (struct fuse_out_header *) fuse_out_iov[0].iov_base;
    out_hdr->unique = in_hdr->unique;
    out_hdr->len = sizeof(struct fuse_out_header);
    out_hdr->error = -ENOSYS;

    printf("virtiofs_emu_ll: fuse OP(%u) called, but not implemented\n", in_hdr->opcode);

    return 0;
}

static int virtiofs_emu_ll_handle_fuse_req(struct virtio_fs_ctrl *ctrl,
                            struct iovec *fuse_in_iov, int in_iovcnt,
                            struct iovec *fuse_out_iov, int out_iovcnt,
                            struct snap_fs_dev_io_done_ctx *done_ctx) {
    struct virtiofs_emu_ll *emu = ctrl->virtiofs_emu;

    if (in_iovcnt < 1 || in_iovcnt < 1) {
        fprintf(stderr, "virtiofs_emu_ll_handle_fuse_req: iovecs in and out don't both atleast one iovec\n");
        return -EINVAL;
    }

    struct fuse_in_header *in_hdr = (struct fuse_in_header *) fuse_in_iov[0].iov_base;

    if (in_hdr->opcode < 1 || in_hdr->opcode > VIRTIOFS_EMU_LL_FUSE_MAX_OPCODE) {
        fprintf(stderr, "Invalid FUSE opcode!\n");
        return -EINVAL;
    } else {
        virtiofs_emu_ll_handler_t h = emu->handlers[in_hdr->opcode];
        if (h == NULL) {
            h = virtiofs_emu_ll_fuse_unknown;
        }
        // Actually call the handler that was provided
        int ret = h(emu->user_data, fuse_in_iov, in_iovcnt, fuse_out_iov, out_iovcnt, done_ctx);
#ifdef DEBUG_ENABLED
        // Only log the number of calls with single threading, not thread safe
        if (emu->nthreads <= 1) {
            if (in_hdr->opcode == FUSE_INIT) {
                memset(&emu->handlers_call_cnts, 0, sizeof(emu->handlers_call_cnts));
            }
            emu->handlers_call_cnts[in_hdr->opcode]++;
        }
        if (ret == 0 && out_iovcnt > 0 &&
            fuse_out_iov[0].iov_len >= sizeof(struct fuse_out_header))
        {
            struct fuse_out_header *out_hdr = (struct fuse_out_header *) fuse_out_iov[0].iov_base;
            if (out_hdr->error != 0)
                fprintf(stderr, "FUSE OP(%d) request ERROR=%d, %s\n", in_hdr->opcode,
                    out_hdr->error, strerror(-out_hdr->error));
        }
#endif
        return ret;
    }
}

struct virtiofs_emu_ll *virtiofs_emu_ll_new(struct virtiofs_emu_ll_params *params) {
    struct virtiofs_emu_params emu_params = params->emu_params;
    if (emu_params.emu_manager == NULL) {
        fprintf(stderr, "virtiofs_emu_new: emu_manager is required!");
        fprintf(stderr, "Enable virtiofs emulation in the firmware (see docs) and"
                        "run `sudo spdk_rpc.py list_emulation_managers` to find"
                        "out what emulation manager name to supply.");
        return NULL;
    }
    if (emu_params.pf_id < 0) {
        fprintf(stderr, "virtiofs_emu_new: pf_id requires a value >=0!");
        // TODO add print that tells you how to figure out the pf_id
        return NULL;
    }
    if (emu_params.vf_id < -1) {
        fprintf(stderr, "virtiofs_emu_new: vf_id requires a value >=-1!");
        return NULL;
    }
    struct virtiofs_emu_ll *emu = calloc(sizeof(struct virtiofs_emu_ll), 1);

    emu->polling_interval_usec = emu_params.polling_interval_usec;
    emu->user_data = params->user_data;
    memcpy(emu->handlers, params->fuse_handlers, sizeof(params->fuse_handlers));
    emu->nthreads = emu_params.nthreads;

    struct virtio_fs_ctrl_init_attr param;
    param.emu_manager_name = emu_params.emu_manager;
    param.nthreads = emu_params.nthreads;
    param.tag = emu_params.tag;
    param.pf_id = emu_params.pf_id;
    param.vf_id = emu_params.vf_id;

    param.dev_type = "virtiofs_emu";
    param.num_queues = 2;
    param.queue_depth = 64;
    param.force_in_order = false;
    param.recover = false; // See snap_virtio_fs_ctrl.c:811, if enabled this controller is supposed to be recovered from the dead
    param.suspended = false;
    param.virtiofs_emu_handle_req = virtiofs_emu_ll_handle_fuse_req;
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

    printf("VirtIO-FS device %s on emulation manager %s is ready\n",
               param.tag, emu_params.emu_manager);

    return emu;

clear_pci_list:
    mlnx_snap_pci_manager_clear();
out:
    return NULL;
}

void virtiofs_emu_ll_destroy(struct virtiofs_emu_ll *emu) {
    printf("VirtIO-FS destroy controller %s\n", emu->snap_ctrl->sctx->context->device->name);

#ifdef DEBUG_ENABLED
    printf("Opcode call counts:\n");
    for (uint32_t i = 0; i < VIRTIOFS_EMU_LL_FUSE_MAX_OPCODE; i++) {
        if (emu->handlers_call_cnts[i])
            printf("OP %u : %u\n", i, emu->handlers_call_cnts[i]);
    }

#endif

    virtio_fs_ctrl_destroy(emu->snap_ctrl);
    mlnx_snap_pci_manager_clear();
    free(emu);
}

