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
#include "rpc.h"
#include "tomlcpp.hpp"

// Each virtio-fs uses at least 3 descriptors (aka queue entries) for each request
#define VIRTIO_FS_MIN_DESCS 3

using namespace erpc;

struct rpc_msg {
    // Back reference to dpfs_hal for the async_completion
    dpfs_hal *hal;

    MsgBuffer req;
    MsgBuffer resp;

    // Only filled if the msg is in use, if so it will point to req internally
    ReqHandle *reqh;

    // Based on the max block size of 1MiB (4k pages, so 256 descriptors) and 3 page overhead per request.
    // These will point into the req and resp buffers.
    iovec iov[256+3];
    int in_iovcnt;
    int out_iovcnt;

    rpc_msg(dpfs_hal *hal, Rpc<CTransport> &rpc) : hal(hal), iov{{0}}
    {
        this->req = rpc.alloc_msg_buffer_or_die((2 << 20) + 4096);
        this->resp = rpc.alloc_msg_buffer_or_die((2 << 20) + 4096);
    }
};

struct dpfs_hal {
    dpfs_hal_handler_t request_handler;
    void *user_data;

    // eRPC
    std::vector<rpc_msg *> avail;
    std::unique_ptr<Nexus> nexus;
    std::unique_ptr<Rpc<CTransport>> rpc;
    int session_num;
};
    
void req_handler(ReqHandle *req_handle, void *context)
{
    dpfs_hal *hal = static_cast<dpfs_hal *>(context);
    rpc_msg *msg = hal->avail.back();
    hal->avail.pop_back();

    msg->reqh = req_handle;

    uint8_t *req_buf = msg->req.buf_;
    uint8_t *resp_buf = msg->resp.buf_;

    // Load the input io vectors
    msg->in_iovcnt = *((int *) msg->req.buf_);
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
static void sm_handler(int, SmEventType, SmErrType, void *) {}

__attribute__((visibility("default")))
struct dpfs_hal *dpfs_hal_new(struct dpfs_hal_params *params) {
    dpfs_hal *hal = new dpfs_hal();
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
    auto [okc, dpu_uri] = conf->getString("dpu_uri");
    if (!okc) {
        std::cerr << "The config must contain a `dpu_uri [hostname/ip:UDP_PORT]" << std::endl;
        delete hal;
        return nullptr;
    }
    auto [okq, qd] = conf->getInt("queue_depth");
    if (!okq || qd < 1 || (qd & (qd - 1))) {
        std::cerr << "The config must contain a `queue_depth` that is >= 1 and a power of 2" << std::endl;
        delete hal;
        return nullptr;
    }
    std::cout << "dpfs_hal using RVFS starting up!" << std::endl;

    hal->nexus = std::unique_ptr<Nexus>(new Nexus(remote_uri));
    hal->rpc = std::unique_ptr<Rpc<CTransport>>(new Rpc<CTransport>(hal->nexus.get(), nullptr, 0, sm_handler));
    hal->session_num = hal->rpc->create_session(dpu_uri, 0);

    // Run till we are connected
    while (!hal->rpc->is_connected(hal->session_num)) hal->rpc->run_event_loop_once();

    // Create the message buffers that we will need for our QD
    for (size_t i = 0; i < qd/VIRTIO_FS_MIN_DESCS; i++) {
        rpc_msg *msg = new rpc_msg(hal, *hal->rpc.get());
        hal->avail.push_back(msg);
    }

    hal->request_handler = params->request_handler;
    hal->user_data = params->user_data;

    std::cout << "Connected to " << dpu_uri << std::endl;

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

    while(keep_running && hal->rpc->is_connected(hal->session_num)) {
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
int dpfs_hal_async_complete(void *completion_context, enum dpfs_hal_completion_status) {
    rpc_msg *msg = (rpc_msg *) completion_context;
    dpfs_hal *hal = msg->hal;

    struct fuse_out_header *fuse_out_header = static_cast<struct fuse_out_header *>(msg->iov[msg->in_iovcnt].iov_base);
    Rpc<CTransport>::resize_msg_buffer(&msg->resp, fuse_out_header->len);

    hal->rpc->enqueue_response(msg->reqh, &msg->resp);
    hal->avail.push_back(msg);
    return 0;
}

#endif // RVFS
