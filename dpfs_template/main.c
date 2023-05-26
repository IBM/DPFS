/*
#
# Copyright 2023- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <linux/fuse.h>
#include "dpfs/hal.h"

static int fuse_handler(void *user_data,
                        struct iovec *fuse_in_iov, int in_iovcnt,
                        struct iovec *fuse_out_iov, int out_iovcnt,
                        void *completion_context, uint16_t device_id)
{
    struct fuse_in_header *in_hdr = (struct fuse_in_header *) fuse_in_iov[0].iov_base;
    struct fuse_out_header *out_hdr = (struct fuse_out_header *) fuse_out_iov[0].iov_base;
    out_hdr->unique = in_hdr->unique;
    out_hdr->len = sizeof(struct fuse_out_header);
    out_hdr->error = -ENOSYS;

    printf("FUSE(%u) called, but not implemented\n", in_hdr->opcode);

    return 0;
}


void usage()
{
    printf("dpfs_template [-c config_path]\n");
}

int main(int argc, char **argv)
{
    const char *conf = NULL;

    int opt;
    while ((opt = getopt(argc, argv, "c:")) != -1) {
        switch (opt) {
            case 'c':
                conf = optarg;
                break;
            default: /* '?' */
                usage();
                exit(1);
        }
    }

    struct dpfs_hal_params hal_params;
    // just for safety
    memset(&hal_params, 0, sizeof(struct dpfs_hal_params));
    if (conf != NULL) {
        hal_params.conf_path = conf;
    } else {
        fprintf(stderr, "You must supply config file path with -c\n");
        usage();
        exit(1);
    }
    printf("dpfs_template starting up!\n");

    hal_params.ops.request_handler = fuse_handler;

    struct dpfs_hal *hal = dpfs_hal_new(&hal_params);
    if (hal == NULL) {
        fprintf(stderr, "Failed to initialize dpfs_hal, exiting...\n");
        return -1;
    }
    dpfs_hal_loop(hal);
    dpfs_hal_destroy(hal);

    return 0;
}

