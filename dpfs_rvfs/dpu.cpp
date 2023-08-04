/*
#
# Copyright 2020- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <linux/fuse.h>
#include <string>
#include <vector>
#include <memory>
#include <iostream>
#include <thread>
#include <boost/lockfree/spsc_queue.hpp>
#include "config.h"
#include "rvfs.h"
#include "dpfs/hal.h"
#include "rpc.h"
#include "tomlcpp.hpp"

// Each virtio-fs uses at least 3 descriptors (aka queue entries) for each request
#define VIRTIO_FS_MIN_DESCS 3

using namespace erpc;

struct rpc_msg {
    MsgBuffer req;
    MsgBuffer resp;
#ifdef DEBUG_ENABLED
    struct iovec *in_iov;
#endif
    // Virtio-fs req output stuff
    struct iovec *out_iov;
    int out_iovcnt;
    void *completion_context;

    rpc_msg(Rpc<CTransport> &rpc) : out_iov(nullptr), out_iovcnt(0), completion_context(nullptr)
    {
        this->req = rpc.alloc_msg_buffer_or_die(DPFS_RVFS_MAX_REQRESP_SIZE);
        this->resp = rpc.alloc_msg_buffer_or_die(DPFS_RVFS_MAX_REQRESP_SIZE);
    }
};

struct rpc_state {
    boost::lockfree::spsc_queue<struct rpc_msg *, boost::lockfree::capacity<1024>> avail;
    std::unique_ptr<Nexus> nexus;
    std::unique_ptr<Rpc<CTransport>> rpc;
    int session_num;
};

void response_func(void *context, void *tag)
{
    rpc_state *state = (rpc_state *) context;
    rpc_msg *msg = (rpc_msg *) tag;
    uint8_t *resp_buf = msg->resp.buf_;

    for (size_t i = 0; i < msg->out_iovcnt; i++) {
        memcpy(msg->out_iov[i].iov_base, (void *) resp_buf, msg->out_iov[i].iov_len);
        resp_buf += msg->out_iov[i].iov_len;
    }

#ifdef DEBUG_ENABLED
    struct fuse_in_header *in_hdr = (struct fuse_in_header *) msg->in_iov[0].iov_base;
    struct fuse_out_header *out_hdr = (struct fuse_out_header *) msg->out_iov[0].iov_base;
    printf("RECEIVE: FUSE OP(%u) request reply for id=%lu in eRPC msg %p\n", in_hdr->opcode, in_hdr->unique, tag);
    if (out_hdr->error != 0)
        fprintf(stderr, "FUSE OP(%u) request with id=%lu ERROR=%d, %s\n",
                in_hdr->opcode, in_hdr->unique, out_hdr->error, strerror(-out_hdr->error));
#endif

    dpfs_hal_async_complete(msg->completion_context, DPFS_HAL_COMPLETION_SUCCES);

    state->avail.push(msg);
}

// The session management callback that is invoked when sessions are successfully created or destroyed.
static void sm_handler(int, SmEventType event, SmErrType err, void *) {
    std::cout << "Event: " << sm_event_type_str(event) << " Error: " << sm_err_type_str(err) << std::endl;
}

// Sends the virtio-fs request via eRPC to the remote server
static int fuse_handler(void *user_data,
                        struct iovec *in_iov, int in_iovcnt,
                        struct iovec *out_iov, int out_iovcnt,
                        void *completion_context, uint16_t device_id)
{
    rpc_state *state = (rpc_state *) user_data;
    // Messages and their buffers are dynamically allocated
    // The queue_depth of the virtio-fs device is static, so this wont infinitely allocate memory
    // Just be sure to warm up the system before evaulating performance
    rpc_msg *msg;
    if (!state->avail.pop(msg)) {
        msg = new rpc_msg(*state->rpc.get());
    }
    uint8_t *req_buf = msg->req.buf_;

#ifdef DEBUG_ENABLED
    struct fuse_in_header *in_hdr = (struct fuse_in_header *) in_iov[0].iov_base;
    printf("SEND: FUSE OP(%u) request with id=%lu, %d input iovecs and %d output iovecs. Sending in eRPC msg %p\n",
            in_hdr->opcode, in_hdr->unique, in_iovcnt, out_iovcnt, msg);
#endif

    // WARNING: This code assumes that the two communicating nodes are either x86_64 or ARM64
    *((int *) req_buf) = in_iovcnt;
    req_buf += sizeof(in_iovcnt);

    for (size_t i = 0; i < in_iovcnt; i++) {
        // Set the iov_len into the request buffer
        *((size_t *) req_buf) = in_iov[i].iov_len;
        req_buf += sizeof(in_iov[i].iov_len);

        // Fill the request buffer with iov_base data
        memcpy(req_buf, in_iov[i].iov_base, in_iov[i].iov_len);
        req_buf += in_iov[i].iov_len;
    }

    *((int *) req_buf) = out_iovcnt;
    req_buf += sizeof(out_iovcnt);

    for (size_t i = 0; i < out_iovcnt; i++) {
        // Set the iov_len into the request buffer
        *((size_t *) req_buf) = out_iov[i].iov_len;
        req_buf += sizeof(out_iov[i].iov_len);
    }

    state->rpc->resize_msg_buffer(&msg->req, req_buf - msg->req.buf_);
    state->rpc->enqueue_request(state->session_num, DPFS_RVFS_REQTYPE_FUSE, &msg->req, &msg->resp, response_func, (void *) msg, kInvalidBgETid);

    msg->completion_context = completion_context;
#ifdef DEBUG_ENABLED
    msg->in_iov = in_iov;
#endif
    msg->out_iov = out_iov;
    msg->out_iovcnt = out_iovcnt;

    return EWOULDBLOCK;
}

static volatile int keep_running;

static void signal_handler(int dummy)
{
    keep_running = 0;
}

void usage()
{
    printf("dpfs_rvfs_dpu [-c config_path]\n");
}

static volatile uint16_t ndevices;

void hal_polling(struct dpfs_hal *hal, Nexus *nexus) {
    // We need to register ourself in eRPC so that we can send requests in the fuse_handler
    nexus->tls_registry_.init();

    uint32_t count = 0;
    while(keep_running) {
        for (uint16_t i = 0; i < ndevices; i++) {
            if (count++ == 10000) {
                dpfs_hal_poll_mmio(hal, i);
                count = 0;
            }

            dpfs_hal_poll_io(hal, i);
        }
    }
}

void register_dpfs_device(void *user_data, uint16_t device_id) {
    ndevices++;
}
void unregister_dpfs_device(void *user_data, uint16_t device_id) {
    ndevices--;
}

int main(int argc, char **argv)
{
    const char *config_path = NULL;

    int opt;
    while ((opt = getopt(argc, argv, "c:")) != -1) {
        switch (opt) {
            case 'c':
                config_path = optarg;
                break;
            default: /* '?' */
                usage();
                exit(1);
        }
    }

    if (!config_path) {
        std::cerr << "A config file is required!" << std::endl;
        usage();
        return -1;
    }

    auto res = toml::parseFile(config_path);
    if (!res.table) {
        std::cerr << "Cannot parse config file: " << res.errmsg << std::endl;
        return -1;
    }
    auto conf = res.table->getTable("rvfs");
    if (!conf) {
        std::cerr << "The config must contain a [rvfs] table" << std::endl;
        return -1;
    }
    
    auto [ok, remote_uri] = conf->getString("remote_uri");
    if (!ok) {
        std::cerr << "The config must contain a `remote_uri` [hostname/ip:UDP_PORT]" << std::endl;
        return -1;
    }
    auto [okc, dpu_uri] = conf->getString("dpu_uri");
    if (!okc) {
        std::cerr << "The config must contain a `dpu_uri` [hostname/ip:UDP_PORT]" << std::endl;
        return -1;
    }
    auto [okd, two_threads] = conf->getBool("two_threads");
    if (!okd) {
        std::cerr << "The config must contain a boolean `two_threads`" << std::endl;
        return -1;
    }

    std::cout << "dpfs_rvfs_dpu starting up!" << std::endl;
    std::cout << "Connecting to " << remote_uri << ". The virtio-fs device will only be up after the connection is established!" << std::endl;

    rpc_state state {};
    size_t numa_node = 0;
    // 1 background thread, which is unused but created to enable multithreading in eRPC
    size_t erpc_bg_threads = two_threads;
    state.nexus = std::unique_ptr<Nexus>(new Nexus(dpu_uri, numa_node, erpc_bg_threads));
    state.rpc = std::unique_ptr<Rpc<CTransport>>(new Rpc<CTransport>(state.nexus.get(), &state, 0, sm_handler));
    state.session_num = state.rpc->create_session(remote_uri, 0);

    // Run till we are connected
    while (!state.rpc->is_connected(state.session_num)) state.rpc->run_event_loop_once();

    struct dpfs_hal_params hal_params;
    // just for safety if a new option gets added
    memset(&hal_params, 0, sizeof(struct dpfs_hal_params));
    hal_params.conf_path = config_path;
    hal_params.ops.request_handler = fuse_handler;
    hal_params.ops.register_device = register_dpfs_device;
    hal_params.ops.unregister_device = unregister_dpfs_device;
    hal_params.user_data = &state;

    struct dpfs_hal *hal = dpfs_hal_new(&hal_params, true);
    if (hal == NULL) {
        fprintf(stderr, "Failed to initialize dpfs_hal, exiting...\n");
        return -1;
    }

    keep_running = 1;
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = signal_handler;
    sigaction(SIGINT, &act, 0);
    sigaction(SIGPIPE, &act, 0);
    sigaction(SIGTERM, &act, 0);

    std::cout << "Connected to the remote and virtio-fs device is online" << std::endl;

    if (two_threads) {
        // The eRPC connection was created on the current threads.
        // eRPC doesn't allow us to switch which thread is the "dispatch" thread.
        // So for simplicity we use the current thread as the eRPC polling thread.
        std::thread hal_thread(hal_polling, hal, state.nexus.get());
        uint32_t count = 0;
        while(keep_running && state.rpc->is_connected(state.session_num)) {
            state.rpc->run_event_loop_once();
        }
        keep_running = 0;
        hal_thread.join();
    } else {
        uint32_t count = 0;
        while(keep_running && state.rpc->is_connected(state.session_num)) {
            for (uint16_t i = 0; i < ndevices; i++) {
                if (count++ == 10000) {
                    dpfs_hal_poll_mmio(hal, i);
                    count = 0;
                }

                dpfs_hal_poll_io(hal, i);
                state.rpc->run_event_loop_once();
            }
        }
    }

    dpfs_hal_destroy(hal);

    return 0;
}

