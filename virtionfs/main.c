/*
#
# Copyright 2020- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

#include <getopt.h>
#include "virtiofs_emu_ll.h"
#include "virtionfs.h"

void usage()
{
    printf("virtionfs [-p pf_id] [-v vf_id ] [-e emulation_manager_name] [-s server_ip] [-x export_path] \n");
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
                usage(1);
                exit(1);
        // TODO add dir param
        }
    }

    struct virtiofs_emu_params emu_params;
    // just for safety
    memset(&emu_params, 0, sizeof(struct virtiofs_emu_params));

    if (pf >= 0)
        emu_params.pf_id = pf;
    else {
        fprintf(stderr, "You must supply a pf with -p\n");
        usage();
        exit(1);
    }
    if (vf >= 0)
        emu_params.vf_id = vf;
    else
        emu_params.vf_id = -1;
    
    if (emu_manager != NULL) {
        emu_params.emu_manager = emu_manager;
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
    printf("virtionfs starting up!\n");
    printf("Connecting to %s:%s\n", server, export);

    emu_params.polling_interval_usec = 0;
    emu_params.nthreads = 0;
    emu_params.tag = "virtionfs";

    virtionfs_main(server, export, false, false, 0, &emu_params);

    return 0;
}