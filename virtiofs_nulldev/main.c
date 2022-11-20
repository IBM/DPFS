/*
#
# Copyright 2020- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

#include <getopt.h>
#include "virtiofs_emu_ll.h"

static int fuse_init(void *user_data,
                     struct iovec *fuse_in_iov, int in_iovcnt,
                     struct iovec *fuse_out_iov, int out_iovcnt,
                     struct snap_fs_dev_io_done_ctx *cb) {
    struct fuse_in_header *in_hdr = (struct fuse_in_header *) fuse_in_iov[0].iov_base;
    struct fuse_out_header *out_hdr = (struct fuse_out_header *) fuse_out_iov[0].iov_base;
    out_hdr->unique = in_hdr->unique;
    out_hdr->len = sizeof(struct fuse_out_header);
    out_hdr->error = -ENOSYS;

    printf("init called, but not implemented\n");

    return 0;
}


void usage()
{
    printf("virtiofs_nulldev [-p pf_id] [-v vf_id ] [-e emulation_manager_name]\n");
}

int main(int argc, char **argv)
{
    int pf = -1;
    int vf = -1;
    char *emu_manager = NULL; // the rdma device name which supports being an emulation manager and virtio_fs emu
    char *server = NULL;
    char *export = NULL;

    int opt;
    while ((opt = getopt(argc, argv, "p:v:e:s:x:")) != -1) {
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
            case 's':
                server = optarg;
                break;
            case 'x':
                export = optarg;
                break;
            default: /* '?' */
                usage();
                exit(1);
        }
    }

    struct virtiofs_emu_ll_params emu_ll_params;
    // just for safety
    memset(&emu_ll_params, 0, sizeof(struct virtiofs_emu_params));
    struct virtiofs_emu_params *emu_params = &emu_ll_params.emu_params;

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
    if (server == NULL) {
        fprintf(stderr, "You must supply a server IP with -s\n");
        usage();
        exit(1);
    }
    if (export == NULL) {
        fprintf(stderr, "You must supply a export path with -p\n");
        usage();
        exit(1);
    }
    printf("virtiofs_nulldev starting up!\n");
    printf("Connecting to %s:%s\n", server, export);

    emu_params->polling_interval_usec = 0;
    emu_params->nthreads = 0;
    emu_params->tag = "virtiofs_nulldev";

    emu_ll_params.fuse_handlers[FUSE_INIT] = fuse_init;
    // Only implement for nodeid = 0, return garbage
    emu_ll_params.fuse_handlers[FUSE_GETATTR] = NULL;
    emu_ll_params.fuse_handlers[FUSE_STATFS] = NULL;

    struct virtiofs_emu_ll *emu = virtiofs_emu_ll_new(&emu_ll_params);
    if (emu == NULL) {
        fprintf(stderr, "Failed to initialize emu_ll, exiting...\n");
        return -1;
    }
    virtiofs_emu_ll_loop(emu);
    virtiofs_emu_ll_destroy(emu);

    return 0;
}

