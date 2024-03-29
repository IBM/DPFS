/*
#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

#ifndef VIRTIONFS_VNFS_CONNECT_H
#define VIRTIONFS_VNFS_CONNECT_H

#include "dpfs_nfs.h"

// Will keep trying to connections in the background until vnfs->nthreads is reached
int vnfs_init_connections(struct virtionfs *vnfs);
void vnfs_destroy_connection(struct vnfs_conn *conn, enum vnfs_conn_state);

#endif // VIRTIONFS_VNFS_CONNECT_H
