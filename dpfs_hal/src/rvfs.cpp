/*
#
# Copyright 2023- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

#include "config.h"
#if defined(DPFS_RVFS)

#include <vector>
#include <iostream>
#include <memory>
#include <linux/fuse.h>
#include "hal.h"
#include "rvfs.h"
#include "rpc.h"
#include "tomlcpp.hpp"

// Each virtio-fs uses at least 3 descriptors (aka queue entries) for each request
#define VIRTIO_FS_MIN_DESCS 3

using namespace erpc;

__attribute__((visibility("default")))
pthread_key_t dpfs_hal_thread_id_key;

struct rpc_msg {
    // Back reference to dpfs_hal for the async_completion
    dpfs_hal *hal;

    // Only filled if the msg is in use, if so it will point to req internally
    ReqHandle *reqh;

    // Based on the max block size of 1MiB (4k pages, so 256 descriptors) and 3 page overhead per request.
    // These will point into the req and resp buffers.
    iovec iov[256+3];
    int in_iovcnt;
    int out_iovcnt;

    rpc_msg(dpfs_hal *hal) : hal(hal), reqh(nullptr),
        iov{{0}}, in_iovcnt(0), out_iovcnt(0)
    {}
};

struct dpfs_hal {
    dpfs_hal_handler_t request_handler;
    void *user_data;

    // eRPC
    std::vector<rpc_msg *> avail;
    std::unique_ptr<Nexus> nexus;
    std::unique_ptr<Rpc<CTransport>> rpc;

    dpfs_hal(dpfs_hal_handler_t rh, void *ud) :
        request_handler(rh), user_data(ud), avail() {}
};
    
static void req_handler(ReqHandle *reqh, void *context)
{
    dpfs_hal *hal = static_cast<dpfs_hal *>(context);
    rpc_msg *msg = hal->avail.back();
    hal->avail.pop_back();

#ifdef DEBUG_ENABLED
    printf("DPFS_HAL_RVFS %s: received eRPC in msg %p", __func__, msg);
#endif

    msg->reqh = reqh;

    uint8_t *req_buf = reqh->get_req_msgbuf()->buf_;
    uint8_t *resp_buf = reqh->pre_resp_msgbuf_.buf_;

    // Load the input io vectors
    msg->in_iovcnt = *((int *) req_buf);
    req_buf += sizeof(msg->in_iovcnt);

    size_t i = 0;
    for (; i < msg->in_iovcnt; i++) {
        size_t iov_len = *((size_t *) req_buf);
        req_buf += sizeof(iov_len);

        // Directly map into the NIC buffer for zero copy
        msg->iov[i].iov_base = req_buf;
        msg->iov[i].iov_len = iov_len;

        req_buf += iov_len;
    }

    // Load the output io vectors
    msg->out_iovcnt = *((int *) req_buf);
    req_buf += sizeof(msg->out_iovcnt);
    
    for (; i < msg->in_iovcnt + msg->out_iovcnt; i++) {
        size_t iov_len = *((size_t *) req_buf);
        req_buf += sizeof(iov_len);

        // Directly map into the NIC buffer for zero copy
        msg->iov[i].iov_base = resp_buf;
        msg->iov[i].iov_len = iov_len;

        resp_buf += iov_len;
    }

    int ret = hal->request_handler(hal->user_data,
            msg->iov, msg->in_iovcnt,
            msg->iov+msg->in_iovcnt, msg->out_iovcnt,
            static_cast<void *>(msg));

    if (ret == 0) {
        dpfs_hal_async_complete(msg, DPFS_HAL_COMPLETION_SUCCES);
    } else if (ret == EWOULDBLOCK) {
        // Do nothing, the FS impl has to call async_completion themselves
    } else {
        dpfs_hal_async_complete(msg, DPFS_HAL_COMPLETION_ERROR);
    }
}

// The session management callback that is invoked when sessions are successfully created or destroyed.
static void sm_handler(int, SmEventType event, SmErrType err, void *) {
    std::cout << "Event: " << sm_event_type_str(e) << " Error: " << sm_err_type_str(e) << std::endl;
}

__attribute__((visibility("default")))
struct dpfs_hal *dpfs_hal_new(struct dpfs_hal_params *params) {
    dpfs_hal *hal = new dpfs_hal(params->request_handler, params->user_data);
    auto res = toml::parseFile(params->conf_path);
    if (!res.table) {
        std::cerr << "cannot parse file: " << res.errmsg << std::endl;
        delete hal;
        return nullptr;
    }
    auto conf = res.table->getTable("rvfs");
    if (!conf) {
        std::cerr << "missing [rvfs]" << std::endl;
        delete hal;
        return nullptr;
    }
    auto [ok, remote_uri] = conf->getString("remote_uri");
    if (!ok) {
        std::cerr << "The config must contain a `remote_uri` [hostname/ip:UDP_PORT]" << std::endl;
        delete hal;
        return nullptr;
    }
    auto [okq, qd] = conf->getInt("queue_depth");
    if (!okq || qd < 1 || (qd & (qd - 1))) {
        std::cerr << "The config must contain a `queue_depth` that is >= 1 and a power of 2" << std::endl;
        delete hal;
        return nullptr;
    }
    if (pthread_key_create(&dpfs_hal_thread_id_key, NULL)) {
        std::cerr << "Failed to create thread-local key for dpfs_hal threadid" << std::endl;
        delete hal;
        return nullptr;
    }
    // Only one thread, thread_id=0
    pthread_setspecific(dpfs_hal_thread_id_key, (void *) 0);

    hal->nexus = std::unique_ptr<Nexus>(new Nexus(remote_uri));
    hal->nexus->register_req_func(DPFS_RVFS_REQTYPE_FUSE, req_handler);
    
    hal->rpc = std::unique_ptr<Rpc<CTransport>>(new Rpc<CTransport>(hal->nexus.get(), hal, 0, sm_handler));
    // Same as in rvfs_dpu
    hal->rpc->set_pre_resp_msgbuf_size(DPFS_RVFS_MAX_REQRESP_SIZE);

    for (int i = 0; i < qd, i++) {
        hal->avail.push_back(new rpc_msg(hal));
    }


    std::cout << "DPFS HAL with RVFS frontend online at " << remote_uri << "!" << std::endl;

    return hal;
}

static volatile int keep_running;

static void signal_handler(int dummy)
{
    keep_running = 0;
}

__attribute__((visibility("default")))
void dpfs_hal_loop(struct dpfs_hal *hal) {
    keep_running = 1;
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = signal_handler;
    sigaction(SIGINT, &act, 0);
    sigaction(SIGPIPE, &act, 0);
    sigaction(SIGTERM, &act, 0);

    while(keep_running) {
        hal->rpc->run_event_loop_once();
    }
}

__attribute__((visibility("default")))
int dpfs_hal_poll_io(struct dpfs_hal *hal, int) {
    hal->rpc->run_event_loop_once();
    return 0;
}

// Just do nothing
__attribute__((visibility("default")))
void dpfs_hal_poll_mmio(struct dpfs_hal *) {}

__attribute__((visibility("default")))
void dpfs_hal_destroy(struct dpfs_hal *hal) {
    while (hal->avail.size()) {
        rpc_msg *msg = hal->avail.back();
        hal->avail.pop_back();
        delete msg;
    }

    delete hal;
}

__attribute__((visibility("default")))
int dpfs_hal_async_complete(void *completion_context, enum dpfs_hal_completion_status)
{
#ifdef DEBUG_ENABLED
    printf("DPFS_HAL_RVFS %s: replying to msg %p", __func__, msg);
#endif
    rpc_msg *msg = (rpc_msg *) completion_context;
    dpfs_hal *hal = msg->hal;

    struct fuse_out_header *fuse_out_header = static_cast<struct fuse_out_header *>(msg->iov[msg->in_iovcnt].iov_base);
    Rpc<CTransport>::resize_msg_buffer(&msg->reqh->pre_resp_msgbuf_, fuse_out_header->len);

    hal->rpc->enqueue_response(msg->reqh, &msg->reqh->pre_resp_msgbuf_);
    hal->avail.push_back(msg);
    return 0;
}

#endif // RVFS
