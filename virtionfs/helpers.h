/*
#
# Copyright 2020- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

#ifndef VIRTIONFS_HELPERS
#define VIRTIONFS_HELPERS
#include <sys/time.h>
#include <nfsc/libnfs.h>
#include <nfsc/libnfs-raw.h>
#include <nfsc/libnfs-raw-nfs.h>
#include <linux/fuse.h>

void fattr3_to_fuse_attr(fattr3 *attr, struct fuse_attr *fuse_attr);
#endif // VIRTIONFS_HELPERS