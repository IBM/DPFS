/*
#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

#ifndef VIRTIOFUSER_MIRROR_IMPL_H
#define VIRTIOFUSER_MIRROR_IMPL_H

#include "fuse_ll.h"

void fuser_mirror_assign_ops(struct fuse_ll_operations *);

#endif // VIRTIOFUSER_MIRROR_IMPL_H