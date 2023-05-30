/*
#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

#include <linux/fuse.h>
#include <stdio.h>
#include "dpfs/hal.h"

#ifdef __cplusplus
extern "C" {
#endif

inline void fuse_ll_debug_print_in_hdr(struct fuse_in_header *in) {
	const char *op_name;
	switch (in->opcode) {
	case 1:
	      op_name = "FUSE_LOOKUP";
	      break;
	case 2:
	      op_name = "FUSE_FORGET";
	      break;
	case 3:
	      op_name = "FUSE_GETATTR";
	      break;
	case 4:
	      op_name = "FUSE_SETATTR";
	      break;
	case 5:
	      op_name = "FUSE_READLINK";
	      break;
	case 6:
	      op_name = "FUSE_SYMLINK";
	      break;
	case 8:
	      op_name = "FUSE_MKNOD";
	      break;
	case 9:
	      op_name = "FUSE_MKDIR";
	      break;
	case 10:
	      op_name = "FUSE_UNLINK";
	      break;
	case 11:
	      op_name = "FUSE_RMDIR";
	      break;
	case 12:
	      op_name = "FUSE_RENAME";
	      break;
	case 13:
	      op_name = "FUSE_LINK";
	      break;
	case 14:
	      op_name = "FUSE_OPEN";
	      break;
	case 15:
	      op_name = "FUSE_READ";
	      break;
	case 16:
	      op_name = "FUSE_WRITE";
	      break;
	case 17:
	      op_name = "FUSE_STATFS";
	      break;
	case 18:
	      op_name = "FUSE_RELEASE";
	      break;
	case 20:
	      op_name = "FUSE_FSYNC";
	      break;
	case 21:
	      op_name = "FUSE_SETXATTR";
	      break;
	case 22:
	      op_name = "FUSE_GETXATTR";
	      break;
	case 23:
	      op_name = "FUSE_LISTXATTR";
	      break;
	case 24:
	      op_name = "FUSE_REMOVEXATTR";
	      break;
	case 25:
	      op_name = "FUSE_FLUSH";
	      break;
	case 26:
	      op_name = "FUSE_INIT";
	      break;
	case 27:
	      op_name = "FUSE_OPENDIR";
	      break;
	case 28:
	      op_name = "FUSE_READDIR";
	      break;
	case 29:
	      op_name = "FUSE_RELEASEDIR";
	      break;
	case 30:
	      op_name = "FUSE_FSYNCDIR";
	      break;
	case 31:
	      op_name = "FUSE_GETLK";
	      break;
	case 32:
	      op_name = "FUSE_SETLK";
	      break;
	case 33:
	      op_name = "FUSE_SETLKW";
	      break;
	case 34:
	      op_name = "FUSE_ACCESS";
	      break;
	case 35:
	      op_name = "FUSE_CREATE";
	      break;
	case 36:
	      op_name = "FUSE_INTERRUPT";
	      break;
	case 37:
	      op_name = "FUSE_BMAP";
	      break;
	case 38:
	      op_name = "FUSE_DESTROY";
	      break;
	case 39:
	      op_name = "FUSE_IOCTL";
	      break;
	case 40:
	      op_name = "FUSE_POLL";
	      break;
	case 41:
	      op_name = "FUSE_NOTIFY_REPLY";
	      break;
	case 42:
	      op_name = "FUSE_BATCH_FORGET";
	      break;
	case 43:
	      op_name = "FUSE_FALLOCATE";
	      break;
	case 44:
	      op_name = "FUSE_READDIRPLUS";
	      break;
	case 45:
	      op_name = "FUSE_RENAME2";
	      break;
	case 46:
	      op_name = "FUSE_LSEEK";
	      break;
	case 47:
	      op_name = "FUSE_COPY_FILE_RANGE";
	      break;
	case 48:
	      op_name = "FUSE_SETUPMAPPING";
	      break;
	case 49:
	      op_name = "FUSE_REMOVEMAPPING";
	      break;
	case 4096:
	      op_name = "CUSE_INIT";
	      break;
	default:
	      op_name = "UNKNOWN FUSE operation!";
	      break;
	}
	uint16_t thread_id = dpfs_hal_thread_id();
	printf("-- %s:%lu:%u --\n", op_name, in->unique, thread_id);
	printf("* nodeid: %lu\n", in->nodeid);
	printf("* uid, gid, pid: %u, %u, %u\n", in->uid, in->gid, in->pid);
}

#ifdef __cplusplus
}
#endif
