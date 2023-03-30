/*
#
# Copyright 2023- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

#include <getopt.h>
#include <stdlib.h>
#include "dpfs_fuse.h"
#include "dpfs_nfs.h"

void usage()
{
    printf("virtionfs [-c config_path]\n");
}

int main(int argc, char **argv)
{
    char *conf = NULL;

    int opt;
    while ((opt = getopt(argc, argv, "c:")) != -1) {
        switch (opt) {
            case 'c':
                config = optarg;
                break;
            default: /* '?' */
                usage();
                exit(1);
        }
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
    printf("dpfs_nfs starting up!\n");
    printf("Connecting to %s:%s\n", server, export);

    emu_params.polling_interval_usec = 0;
    emu_params.nthreads = nthreads;
    emu_params.queue_depth = queue_depth;
    emu_params.tag = "virtionfs";

    dpfs_nfs_main(server, export, false, false, nthreads, conf);

    return 0;
}
