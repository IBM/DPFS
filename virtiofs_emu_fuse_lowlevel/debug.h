#include <linux/fuse.h>
#include <stdio.h>

static inline void fuse_ll_debug_print_in_hdr(struct fuse_in_header *in) {
	printf("FUSE OP(%d):\n", in->opcode);
	printf("* nodeid: %lu\n", in->nodeid);
	printf("* uid, gid, pid: %u, %u, %u\n", in->uid, in->gid, in->pid);
}
