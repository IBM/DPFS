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
#include "dpfs_hal.h"
#include "rpc.h"

#define QD 64
// Each virtio-fs uses at least 3 descriptors (aka queue entries) for each request
#define VIRTIO_FS_MIN_DESCS 3

using namespace erpc;

struct rpc_msg {
    MsgBuffer req;
    MsgBuffer resp;
    // Virtio-fs req output stuff
    struct iovec *out_iov;
    int out_iovcnt;
    void *completion_context;

    rpc_msg(Rpc<CTransport> &rpc) : out_iov(nullptr), out_iovcnt(0), completion_context(nullptr)
    {
        this->req = rpc.alloc_msg_buffer_or_die((2 << 20) + 4096);
        this->resp = rpc.alloc_msg_buffer_or_die((2 << 20) + 4096);
    }
};

struct rpc_state {
    std::vector<struct rpc_msg *> avail;
    std::unique_ptr<Nexus> nexus;
    std::unique_ptr<Rpc<CTransport>> rpc;
    int session_num;
};

void response_func(void *context, void *tag) {
    rpc_state *state = (rpc_state *) context;
    rpc_msg *msg = (rpc_msg *) tag;
    uint8_t *req_buf = msg->req.buf_;

    int out_iovcnt = *((int *) msg->req.buf_);
    req_buf += sizeof(out_iovcnt);

    for (size_t i = 0; i < out_iovcnt; i++) {
        size_t iov_len = *((size_t *) req_buf);
        req_buf += sizeof(iov_len);

        memcpy(msg->out_iov[i].iov_base, (void *) req_buf, iov_len);
        req_buf += iov_len;
    }

    dpfs_hal_async_complete(msg->completion_context, DPFS_HAL_COMPLETION_SUCCES);

    state->avail.push_back(msg);
}

// The session management callback that is invoked when sessions are successfully created or destroyed.
void sm_handler(int, SmEventType, SmErrType, void *) {}

static int fuse_handler(void *user_data,
                        struct iovec *in_iov, int in_iovcnt,
                        struct iovec *out_iov, int out_iovcnt,
                        void *completion_context)
{
    rpc_state *state = (rpc_state *) user_data;
    // Send it via eRPC to the remote side
    rpc_msg *msg = state->avail.back();
    uint8_t *req_buf = msg->req.buf_;
    state->avail.pop_back();

    *((int *) req_buf) = in_iovcnt;
    req_buf += sizeof(in_iovcnt);

    for (size_t i = 0; i < in_iovcnt; i++) {
        // Set the iov_len into the request buffer
        *(size_t *) req_buf = in_iov[i].iov_len;
        req_buf += sizeof(in_iov[i].iov_len);

        // Fill the request buffer with iov_base data
        memcpy(req_buf, in_iov[i].iov_base, in_iov[i].iov_len);
        req_buf += in_iov[i].iov_len;
    }

    state->rpc->enqueue_request(state->session_num, 0, &msg->req, &msg->resp, response_func, (void *) state);

    return 0;
}

static volatile int keep_running;

void signal_handler(int dummy)
{
    keep_running = 0;
}

void usage()
{
    printf("dpfs_erpc_dpu [-p pf_id] [-v vf_id ] [-e emulation_manager_name]  [-l local_uri] [-s server_uri] [-x server_export_path]\n");
}

int main(int argc, char **argv)
{
    int pf = -1;
    int vf = -1;
    const char *emu_manager = NULL; // the rdma device name which supports being an emulation manager and virtio_fs emu
    const char *local = NULL;
    const char *server = NULL;
    const char *e = NULL;

    int opt;
    while ((opt = getopt(argc, argv, "p:v:e:l:s:x:")) != -1) {
        switch (opt) {
            case 'p':
                pf = atoi(optarg);
                break;
            case 'v':
                vf = atoi(optarg);
                break;
            case 'e':
                emu_manager = optarg;
                break;
            case 'l':
                local = optarg;
                break;
            case 's':
                server = optarg;
                break;
            case 'x':
                e = optarg;
                break;
            default: /* '?' */
                usage();
                exit(1);
        }
    }

    struct dpfs_hal_params hal_params;
    // just for safety
    memset(&hal_params, 0, sizeof(struct dpfs_hal_params));
    struct virtiofs_emu_params *emu_params = &hal_params.emu_params;

    if (pf >= 0)
        emu_params->pf_id = pf;
    else {
        fprintf(stderr, "You must supply a pf with -p\n");
        usage();
        exit(1);
    }
    if (vf >= 0)
        emu_params->vf_id = vf;
    else
        emu_params->vf_id = -1;
    
    if (emu_manager != NULL) {
        emu_params->emu_manager = emu_manager;
    } else {
        fprintf(stderr, "You must supply an emu manager name with -e\n");
        usage();
        exit(1);
    }
    if (local == NULL) {
        fprintf(stderr, "You must supply a local uri with -l [hostname/ip:UDP_port]\n");
        usage();
        exit(1);
    }
    std::string client_uri = local;
    if (server == NULL) {
        fprintf(stderr, "You must supply a server uri with -s [hostname/ip:UDP_port]\n");
        usage();
        exit(1);
    }
    std::string server_uri = server;
    if (e == NULL) {
        fprintf(stderr, "You must supply a server file export path with -e\n");
        usage();
        exit(1);
    }
    printf("dpfs_rpc_dpu starting up!\n");
    printf("Connecting to %s:%s\n", server, e);

    emu_params->queue_depth = QD;
    emu_params->polling_interval_usec = 0;
    emu_params->nthreads = 0;
    emu_params->tag = "dpfs_template";
    hal_params.request_handler = fuse_handler;

    rpc_state state;
    state.nexus = std::unique_ptr<Nexus>(new Nexus(client_uri));
    state.rpc = std::unique_ptr<Rpc<CTransport>>(new Rpc<CTransport>(state.nexus.get(), nullptr, 0, sm_handler));
    state.session_num = state.rpc->create_session(server_uri, 0);

    // Run till we are connected
    while (!state.rpc->is_connected(state.session_num)) state.rpc->run_event_loop_once();

    // Create the message buffers that we will need for our QD
    for (size_t i = 0; i < QD/VIRTIO_FS_MIN_DESCS; i++) {
        rpc_msg *msg = new rpc_msg(*state.rpc.get());
        state.avail.push_back(msg);
    }

    struct dpfs_hal *hal = dpfs_hal_new(&hal_params);
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

    uint32_t count = 0;
    while(keep_running && state.rpc->is_connected(state.session_num)) {
        if (count++ % 10000 == 0)
            dpfs_hal_poll_mmio(hal);

        dpfs_hal_poll_io(hal, 0);
        state.rpc->run_event_loop_once();
    }
    dpfs_hal_destroy(hal);

    return 0;
}

