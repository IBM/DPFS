/*
#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

#include <getopt.h>
#include "fuser.h"
#include "virtiofs_emu_ll.h"

void usage()
{
    printf("virtiofuser [-p pf_id] [-v vf_id ] [-e emulation_manager_name] [-d dir_mirror_path]\n");
}

int main(int argc, char **argv)
{
    int pf = -1;
    int vf = -1;
    char *emu_manager = NULL; // the rdma device name which supports being an emulation manager and virtio_fs emu
    char *dir = NULL; // the directory that will be mirrored

    int opt;
    while ((opt = getopt(argc, argv, "p:v:e:d:")) != -1) {
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
            case 'd':
                dir = optarg;
                break;
            default: /* '?' */
                usage();
                exit(1);
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
    if (dir == NULL) {
        fprintf(stderr, "You must supply an emu manager name with -e\n");
        usage();
        exit(1);
    }
    char *rp = realpath(dir, NULL);
    if (rp == NULL) {
        fprintf(stderr, "Could not parse dir %s, errno=%d\n", dir, errno);
        exit(errno);
    }
    dir = rp;
    printf("virtiofuser starting up!\n");
    printf("Mirroring %s\n", dir);

    emu_params.polling_interval_usec = 0;
    emu_params.nthreads = 0;
    emu_params.tag = "virtiofuser";

    fuser_main(false, dir, false, &emu_params);

    return 0;
}
