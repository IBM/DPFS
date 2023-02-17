/*
#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

#include <err.h>
#include <stdlib.h>
#include <stdio.h>
#include <infiniband/verbs.h>

#include "nvme_emu_log.h"
#include "mlnx_snap_pci_manager.h"

void print_pfs_vfs(struct snap_pfs_ctx *ctx) {
    for (int i = 0; i < ctx->max_pfs; i++) {
        printf("    * PF %d with pci_number %s has %d VFs\n", i, ctx->pfs[i].pci_number, ctx->pfs[i].num_vfs);
    }
}

int main(void) {
    if (nvme_init_logger())
        err(1, "Failed to open logger. Does /var/log/mlnx_snap exist?");

    if (mlnx_snap_pci_manager_init())
        err(1, "Failed to init SNAP pci manager, this can mean two things:\n"
                "* No emulation capability is turned on in the firmware. (virtio-fs, virtio-blk, virtio-net or nvme)"
                "* The BF needs to be rebooted (full host external power cycle), because the new firmware configuration is not yet applied or the emulation firmware is in a crashed state (happens if a SNAP application was not correctly shut down)\n");

    int ibv_count;
    struct ibv_device **ibv_list = ibv_get_device_list(&ibv_count);
    if (!ibv_list) {
        fprintf(stderr, "Failed to open IB device list.\n");
        goto err_pci;
    }

    for (int i = 0; i < ibv_count; i++) {
        const char *rdma_device = ibv_get_device_name(ibv_list[i]);

        struct snap_context *sctx = mlnx_snap_get_snap_context(rdma_device);
        if (!sctx)
            continue;

        printf("The reported number of VFs is incorrect\n");
        printf("Emulation manager (aka RDMA device) \"%s\" supports:\n", rdma_device);

        if (sctx->emulation_caps & SNAP_VIRTIO_FS) {
            printf("* virtio_fs\n");
            print_pfs_vfs(&sctx->virtio_fs_pfs);
            printf("    * Maximum number of emulated virtqueues = %u\n",
                    sctx->virtio_fs_caps.max_emulated_virtqs);
        }
        if (sctx->emulation_caps & SNAP_VIRTIO_BLK) {
            printf("* virtio_blk\n");
            print_pfs_vfs(&sctx->virtio_blk_pfs);
            printf("    * Maximum number of emulated virtqueues = %u\n",
                    sctx->virtio_blk_caps.max_emulated_virtqs);
        }
        if (sctx->emulation_caps & SNAP_VIRTIO_NET) {
            printf("* virtio_net\n");
            print_pfs_vfs(&sctx->virtio_net_pfs);
            printf("    * Maximum number of emulated virtqueues = %u\n",
                    sctx->virtio_net_caps.max_emulated_virtqs);
        }
        if (sctx->emulation_caps & SNAP_NVME) {
            printf("* nvme\n");
            print_pfs_vfs(&sctx->nvme_pfs);
            printf("    * Maximum number of emulated completion queues = %u\n",
                    sctx->nvme_caps.max_emulated_nvme_cqs);
            printf("    * Maximum number of emulated submission queues = %u\n",
                    sctx->nvme_caps.max_emulated_nvme_cqs);
        }
    } 

    free(ibv_list);
err_pci:
    mlnx_snap_pci_manager_clear();

    return 0;
}
