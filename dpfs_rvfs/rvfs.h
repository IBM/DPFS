#ifndef DPFS_RVFS_H
#define DPFS_RVFS_H

#define DPFS_RVFS_MAX_REQRESP_SIZE ((2 << 20) + (4096*4))

// We support max virtio queue depth of 512, and min req size is 4 elements, so 128 requests, this is really large
#define QUEUE_SIZE 512

#define DPFS_RVFS_REQTYPE_FUSE 0

#endif // DPFS_RVFS_H
