/*
#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

#include "helpers.h"

void fattr3_to_fuse_attr(fattr3 *attr, struct fuse_attr *fuse_attr) {
    fuse_attr->ino = attr->fileid;
    fuse_attr->size = attr->size;
    fuse_attr->blocks = 0;
    fuse_attr->atime = attr->atime.seconds;
    fuse_attr->mtime = attr->mtime.seconds;
    fuse_attr->ctime = attr->ctime.seconds;
    fuse_attr->atimensec = attr->atime.nseconds;
    fuse_attr->mtimensec = attr->mtime.nseconds;
    fuse_attr->ctimensec = attr->ctime.nseconds;
    fuse_attr->mode = attr->mode;
    fuse_attr->nlink = attr->nlink;
    fuse_attr->uid = attr->uid;
    fuse_attr->gid = attr->gid;
    fuse_attr->rdev = 0; // see specdata3 of nfs3 rfc
    fuse_attr->blksize = 0;
}