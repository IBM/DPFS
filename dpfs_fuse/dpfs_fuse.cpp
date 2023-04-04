/*
 * This is only really useful if you do a single run, as then the number
 * is somewhat representativ
#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

/*
 * The ENOSYS returns for FUSE operations are always after fully parsing
 * the input data, this is useful because that data can then be used
 * for debug printing. It does not affect performance because FUSE
 * implementations are required to never call that operation again after
 * an ENOSYS.
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>
#include <err.h>
//#include <stdatomic.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/fcntl.h>
#include <stddef.h>
#include <linux/fuse.h>

#include "config.h"
#include "common.h"
#include "debug.h"
#include "dpfs/hal.h"
#include "dpfs_fuse.h"

struct fuse_ll;
typedef int (*fuse_handler_t) (struct fuse_ll *,
                             struct iovec *fuse_in_iov, int in_iovcnt,
                             struct iovec *fuse_out_iov, int out_iovcnt,
                             void *completion_context);

#define DPFS_FUSE_MAX_OPCODE FUSE_REMOVEMAPPING
// The opcodes begin at FUSE_LOOKUP = 1, so need one more array index
#define DPFS_FUSE_HANDLERS_LEN DPFS_FUSE_MAX_OPCODE+1

struct fuse_ll {
    void *user_data;
    struct fuse_ll_operations ops;
    struct fuse_session *se;
    fuse_handler_t fuse_handlers[DPFS_FUSE_HANDLERS_LEN];
    bool debug;
};

static void convert_stat(const struct stat *stbuf, struct fuse_attr *attr)
{
    attr->ino	= stbuf->st_ino;
    attr->mode	= stbuf->st_mode;
    attr->nlink	= stbuf->st_nlink;
    attr->uid	= stbuf->st_uid;
    attr->gid	= stbuf->st_gid;
    attr->rdev	= stbuf->st_rdev;
    attr->size	= stbuf->st_size;
    attr->blksize	= stbuf->st_blksize;
    attr->blocks	= stbuf->st_blocks;
    attr->atime	= stbuf->st_atime;
    attr->mtime	= stbuf->st_mtime;
    attr->ctime	= stbuf->st_ctime;
    attr->atimensec = 0;
    attr->mtimensec = 0;
    attr->ctimensec = 0;

    //attr->atimensec = ST_ATIM_NSEC(stbuf);
    //attr->mtimensec = ST_MTIM_NSEC(stbuf);
    //attr->ctimensec = ST_CTIM_NSEC(stbuf);
}

static void convert_attr(const struct fuse_setattr_in *attr, struct stat *stbuf)
{
    stbuf->st_mode	       = attr->mode;
    stbuf->st_uid	       = attr->uid;
    stbuf->st_gid	       = attr->gid;
    stbuf->st_size	       = attr->size;
    stbuf->st_atime	       = attr->atime;
    stbuf->st_mtime	       = attr->mtime;
    stbuf->st_ctime        = attr->ctime;
    stbuf->st_atim.tv_nsec = 0;
    stbuf->st_mtim.tv_nsec = 0;
    stbuf->st_ctim.tv_nsec = 0;

    //ST_ATIM_NSEC_SET(stbuf, attr->atimensec);
    //ST_MTIM_NSEC_SET(stbuf, attr->mtimensec);
    //ST_CTIM_NSEC_SET(stbuf, attr->ctimensec);
}

static void convert_statfs(const struct statvfs *stbuf,
               struct fuse_kstatfs *kstatfs)
{
    kstatfs->bsize	 = stbuf->f_bsize;
    kstatfs->frsize	 = stbuf->f_frsize;
    kstatfs->blocks	 = stbuf->f_blocks;
    kstatfs->bfree	 = stbuf->f_bfree;
    kstatfs->bavail	 = stbuf->f_bavail;
    kstatfs->files	 = stbuf->f_files;
    kstatfs->ffree	 = stbuf->f_ffree;
    // Maximum FILEname length
    kstatfs->namelen     = stbuf->f_namemax;
}

static void fill_entry(struct fuse_entry_out *arg,
               const struct fuse_entry_param *e)
{
    arg->nodeid = e->ino;
    arg->generation = e->generation;
    arg->entry_valid = calc_timeout_sec(e->entry_timeout);
    arg->entry_valid_nsec = calc_timeout_nsec(e->entry_timeout);
    arg->attr_valid = calc_timeout_sec(e->attr_timeout);
    arg->attr_valid_nsec = calc_timeout_nsec(e->attr_timeout);
    convert_stat(&e->attr, &arg->attr);
}

static void fill_open(struct fuse_open_out *arg,
              const struct fuse_file_info *f)
{
    arg->fh = f->fh;
    arg->open_flags = 0;
    if (f->direct_io)
        arg->open_flags |= FOPEN_DIRECT_IO;
    if (f->keep_cache)
        arg->open_flags |= FOPEN_KEEP_CACHE;
    if (f->cache_readdir)
        arg->open_flags |= FOPEN_CACHE_DIR;
    if (f->nonseekable)
        arg->open_flags |= FOPEN_NONSEEKABLE;
    // Not supported on Ubuntu 20.04
    //if (f->noflush)
    //	arg->open_flags |= FOPEN_NOFLUSH;
}


unsigned long calc_timeout_sec(double t)
{
    if (t > (double) ULONG_MAX)
        return ULONG_MAX;
    else if (t < 0.0)
        return 0;
    else
        return (unsigned long) t;
}

unsigned int calc_timeout_nsec(double t)
{
    double f = t - (double) calc_timeout_sec(t);
    if (f < 0.0)
        return 0;
    else if (f >= 0.999999999)
        return 999999999;
    else
        return (unsigned int) (f * 1.0e9);
}

int fuse_ll_reply_attr(struct fuse_session *se, struct fuse_out_header *out_hdr, struct fuse_attr_out *out_attr, struct stat *s, double attr_timeout) {
    size_t size = se->conn.proto_minor < 9 ?
        FUSE_COMPAT_ATTR_OUT_SIZE : sizeof(*out_attr);

    memset(out_attr, 0, sizeof(*out_attr));
    out_attr->attr_valid = calc_timeout_sec(attr_timeout);
    out_attr->attr_valid_nsec = calc_timeout_nsec(attr_timeout);
    convert_stat(s, &out_attr->attr);

    out_hdr->len += size;
    return 0;
}

int fuse_ll_reply_entry(struct fuse_session *se, struct fuse_out_header *out_hdr, struct fuse_entry_out *out_entry, struct fuse_entry_param *e) {
    size_t size = se->conn.proto_minor < 9 ?
        FUSE_COMPAT_ENTRY_OUT_SIZE : sizeof(*out_entry);

    /* before ABI 7.4 e->ino == 0 was invalid, only ENOENT meant
       negative entry */
    if (!e->ino && se->conn.proto_minor < 4) {
        out_hdr->error = -ENOENT;
        return 0;
    }

    memset(out_entry, 0, sizeof(*out_entry));
    fill_entry(out_entry, e);

    out_hdr->len += size;
    return 0;
}
int fuse_ll_reply_open(struct fuse_session *se, struct fuse_out_header *out_hdr,
        struct fuse_open_out *out_open, struct fuse_file_info *fi)
{
    fill_open(out_open, fi);

    out_hdr->len += sizeof(*out_open);
    return 0;
}

int fuse_ll_reply_create(struct fuse_session *se, struct fuse_out_header *out_hdr,
    struct fuse_entry_out *out_entry, struct fuse_open_out *out_open,
    const struct fuse_entry_param *e, const struct fuse_file_info *fi)
{
    size_t entrysize = se->conn.proto_minor < 9 ?
        FUSE_COMPAT_ENTRY_OUT_SIZE : sizeof(struct fuse_entry_out);

    memset(out_entry, 0, sizeof(*out_entry));
    fill_entry(out_entry, e);
    fill_open(out_open, fi);
    out_hdr->len += entrysize + sizeof(struct fuse_open_out);
    return 0;
}

int fuse_ll_reply_statfs(struct fuse_session *se, struct fuse_out_header *out_hdr,
    struct fuse_statfs_out *out_statfs, const struct statvfs *stbuf)
{
    size_t size = se->conn.proto_minor < 4 ?
        FUSE_COMPAT_STATFS_SIZE : sizeof(*out_statfs);

    convert_statfs(stbuf, &out_statfs->st);

    out_hdr->len += size;
    return 0;
}

int fuse_ll_reply_iov(struct fuse_session *se, struct fuse_out_header *out_hdr,
    struct iov *iov)
{
    out_hdr->len += iov->total_size - iov->bytes_unused;
    free(iov);
    return 0;
}

void iov_init(struct iov *iov, struct iovec *iovec, int iovcnt) {
    iov->iovec = iovec;
    iov->iovcnt = iovcnt;
    iov->iov_idx = 0;
    iov->buf_idx = 0;
    iov->total_size = 0;
    for (int i = 0; i < iov->iovcnt; i++) {
        iov->total_size += iov->iovec[i].iov_len;
    }
    iov->bytes_unused = iov->total_size;
}

size_t iov_write_buf(struct iov *iov, void *buf, size_t size) {
    // is there enough space in the iov?
    if (iov->bytes_unused < size)
        return 0;
    // because of this if check we can assume that the iov is big enough during the copy loop

    size_t rem = size;
    while (rem != 0) {
        struct iovec *v = &iov->iovec[iov->iov_idx];
        // copy all the remaining bytes or till the current end of iovec
        size_t to_cpy = MIN(rem, v->iov_len - iov->buf_idx);
        memcpy(((char *)v->iov_base) + iov->buf_idx, buf, to_cpy);

        // we hit the end of the iovec
        if (iov->buf_idx + to_cpy == v->iov_len){
            // goto the next iovec
            iov->iov_idx++;
            iov->buf_idx = 0;
        } else { // move the current iovec forward by the amount of bytes copied
            iov->buf_idx += to_cpy;
        }
        rem -= to_cpy;
    }
    iov->bytes_unused -= size;
    return size;
}

size_t fuse_add_direntry(struct iov *read_iov,
             const char *name, const struct stat *stbuf, off_t off)
{
    size_t namelen = strlen(name);
    size_t entlen = FUSE_NAME_OFFSET + namelen;
    size_t entlen_padded = FUSE_DIRENT_ALIGN(entlen);

    // iov_write_buf also checks this, but we should do it before we execute the memcpys
    if (read_iov->bytes_unused < entlen_padded) {
        return 0;
    }

    char buf[entlen_padded];

    struct fuse_dirent *dirent = (struct fuse_dirent *) buf;
    dirent->ino = stbuf->st_ino;
    dirent->off = off;
    dirent->namelen = namelen;
    dirent->type = (stbuf->st_mode & S_IFMT) >> 12;
    memcpy(dirent->name, name, namelen);
    memset(dirent->name + namelen, 0, entlen_padded - entlen);

    return iov_write_buf(read_iov, buf, entlen_padded);
}

size_t fuse_add_direntry_plus(struct iov *read_iov,
                  const char *name,
                  const struct fuse_entry_param *e, off_t off)
{
    size_t namelen = strlen(name);
    size_t entlen = FUSE_NAME_OFFSET_DIRENTPLUS + namelen;
    size_t entlen_padded = FUSE_DIRENT_ALIGN(entlen);

    // iov_write_buf also checks this, but we should do it before we execute the memcpys
    if (read_iov->bytes_unused < entlen_padded) {
        return 0;
    }

    char buf[entlen_padded];

    struct fuse_direntplus *dp = (struct fuse_direntplus *) buf;
    memset(&dp->entry_out, 0, sizeof(dp->entry_out));
    fill_entry(&dp->entry_out, e);

    struct fuse_dirent *dirent = &dp->dirent;
    dirent->ino = e->attr.st_ino;
    dirent->off = off;
    dirent->namelen = namelen;
    dirent->type = (e->attr.st_mode & S_IFMT) >> 12;
    memcpy(dirent->name, name, namelen);
    memset(dirent->name + namelen, 0, entlen_padded - entlen);

    return iov_write_buf(read_iov, buf, entlen_padded);
}

static int fuse_ll_init(struct fuse_ll *f_ll,
               struct iovec *fuse_in_iov, int in_iovcnt,
               struct iovec *fuse_out_iov, int out_iovcnt,
               void *completion_context) {
    if (in_iovcnt != 2 || out_iovcnt != 2) {
        fprintf(stderr, "%s: invalid number of iovecs!\n", __func__);
        return -EINVAL;
    }

    struct fuse_in_header *in_hdr = (struct fuse_in_header *) fuse_in_iov[0].iov_base;
    struct fuse_out_header *out_hdr = (struct fuse_out_header *) fuse_out_iov[0].iov_base;
    out_hdr->unique = in_hdr->unique;
    out_hdr->len = sizeof(*out_hdr);
    out_hdr->error = 0;
    struct fuse_init_in *inarg = (struct fuse_init_in *) fuse_in_iov[1].iov_base;
    struct fuse_init_out *outarg = (struct fuse_init_out *) fuse_out_iov[1].iov_base;

    struct fuse_session *se = f_ll->se;
    if (se->got_init == 1 && se->got_destroy == 0) {
        out_hdr->error = -EISCONN;
        return 0;
    }

    size_t bufsize = se->bufsize;
    size_t outargsize = sizeof(*outarg);
#ifdef DEBUG_ENABLED
    printf("INIT in: %u.%u\n", inarg->major, inarg->minor);
    if (inarg->major == 7 && inarg->minor >= 6) {
        printf("* flags=0x%08x\n", inarg->flags);
        printf("* max_readahead=0x%08x\n", inarg->max_readahead);
    }
#endif
    se->conn.proto_major = inarg->major;
    se->conn.proto_minor = inarg->minor;
    se->conn.capable = 0;
    se->conn.want = 0;
    se->conn.max_background = DPFS_HAL_MAX_BACKGROUND;

    memset(outarg, 0, sizeof(*outarg));
    outarg->major = FUSE_KERNEL_VERSION;
    outarg->minor = FUSE_KERNEL_MINOR_VERSION;

    if (inarg->major < 7) {
        printf("fuse: unsupported protocol version: %u.%u\n",
            inarg->major, inarg->minor);
        out_hdr->error = -EPROTO;
        return 0;
    }

    if (inarg->major > 7) {
        /* Wait for a second INIT request with a 7.X version */
        out_hdr->len+=outargsize;
        return 0;
    }

    if (inarg->minor >= 6) {
        // Settle for the smallest max_readahead size of the two parties
        if (inarg->max_readahead < se->conn.max_readahead)
            se->conn.max_readahead = inarg->max_readahead;
        else
            inarg->max_readahead = se->conn.max_readahead;
        //if (inarg->flags & FUSE_INIT_EXT)
        //	inarg->flags = inargflags | (uint64_t) arg->flags2 << 32;
        if (inarg->flags & FUSE_ASYNC_READ)
            se->conn.capable |= FUSE_CAP_ASYNC_READ;
        if (inarg->flags & FUSE_POSIX_LOCKS)
            se->conn.capable |= FUSE_CAP_POSIX_LOCKS;
        if (inarg->flags & FUSE_ATOMIC_O_TRUNC)
            se->conn.capable |= FUSE_CAP_ATOMIC_O_TRUNC;
        if (inarg->flags & FUSE_EXPORT_SUPPORT)
            se->conn.capable |= FUSE_CAP_EXPORT_SUPPORT;
        if (inarg->flags & FUSE_DONT_MASK)
            se->conn.capable |= FUSE_CAP_DONT_MASK;
        if (inarg->flags & FUSE_FLOCK_LOCKS)
            se->conn.capable |= FUSE_CAP_FLOCK_LOCKS;
        if (inarg->flags & FUSE_AUTO_INVAL_DATA)
            se->conn.capable |= FUSE_CAP_AUTO_INVAL_DATA;
        if (inarg->flags & FUSE_DO_READDIRPLUS)
            se->conn.capable |= FUSE_CAP_READDIRPLUS;
        if (inarg->flags & FUSE_READDIRPLUS_AUTO)
            se->conn.capable |= FUSE_CAP_READDIRPLUS_AUTO;
        if (inarg->flags & FUSE_ASYNC_DIO)
            se->conn.capable |= FUSE_CAP_ASYNC_DIO;
        if (inarg->flags & FUSE_WRITEBACK_CACHE)
            se->conn.capable |= FUSE_CAP_WRITEBACK_CACHE;
        if (inarg->flags & FUSE_NO_OPEN_SUPPORT)
            se->conn.capable |= FUSE_CAP_NO_OPEN_SUPPORT;
        if (inarg->flags & FUSE_PARALLEL_DIROPS)
            se->conn.capable |= FUSE_CAP_PARALLEL_DIROPS;
        if (inarg->flags & FUSE_POSIX_ACL)
            se->conn.capable |= FUSE_CAP_POSIX_ACL;
        if (inarg->flags & FUSE_HANDLE_KILLPRIV)
            se->conn.capable |= FUSE_CAP_HANDLE_KILLPRIV;
        if (inarg->flags & FUSE_CACHE_SYMLINKS)
            se->conn.capable |= FUSE_CAP_CACHE_SYMLINKS;
        if (inarg->flags & FUSE_NO_OPENDIR_SUPPORT)
            se->conn.capable |= FUSE_CAP_NO_OPENDIR_SUPPORT;
        if (inarg->flags & FUSE_EXPLICIT_INVAL_DATA)
            se->conn.capable |= FUSE_CAP_EXPLICIT_INVAL_DATA;
        if (!(inarg->flags & FUSE_MAX_PAGES)) {
            size_t max_bufsize =
                FUSE_DEFAULT_MAX_PAGES_PER_REQ * getpagesize()
                + FUSE_BUFFER_HEADER_SIZE;
            if (bufsize > max_bufsize) {
                bufsize = max_bufsize;
            }
        }
    } else {
        se->conn.max_readahead = 0;
    }

    if (se->conn.proto_minor >= 18)
        se->conn.capable |= FUSE_CAP_IOCTL_DIR;

    /* Default settings for modern filesystems.
     *
     * Most of these capabilities were disabled by default in
     * libfuse2 for backwards compatibility reasons. In libfuse3,
     * we can finally enable them by default (as long as they're
     * supported by the kernel).
     */
#define LL_SET_DEFAULT(cond, cap) \
    if ((cond) && (se->conn.capable & (cap))) \
        se->conn.want |= (cap)
    LL_SET_DEFAULT(1, FUSE_CAP_ASYNC_READ);
    LL_SET_DEFAULT(1, FUSE_CAP_PARALLEL_DIROPS);
    // We disable FUSE_CAP_AUTO_INVAL_DATA by default because it kills read performance
    // It causes every read to first send a getattr and then a read
    // To provide better consistency, properly use the timeouts in the attr
    //LL_SET_DEFAULT(1, FUSE_CAP_AUTO_INVAL_DATA);
    LL_SET_DEFAULT(1, FUSE_CAP_HANDLE_KILLPRIV);
    LL_SET_DEFAULT(1, FUSE_CAP_ASYNC_DIO);
    LL_SET_DEFAULT(1, FUSE_CAP_IOCTL_DIR);
    LL_SET_DEFAULT(1, FUSE_CAP_ATOMIC_O_TRUNC);
    //LL_SET_DEFAULT(se->op.write_buf, FUSE_CAP_SPLICE_READ);
    //LL_SET_DEFAULT(se->op.getlk && se->op.setlk,
    //	       FUSE_CAP_POSIX_LOCKS);
    //LL_SET_DEFAULT(se->op.flock, FUSE_CAP_FLOCK_LOCKS);
    //LL_SET_DEFAULT(se->op.readdirplus, FUSE_CAP_READDIRPLUS);
    //LL_SET_DEFAULT(se->op.readdirplus && se->op.readdir,
    //	       FUSE_CAP_READDIRPLUS_AUTO);
    se->conn.want &= ~FUSE_CAP_SPLICE_READ;
    se->conn.want &= ~FUSE_CAP_POSIX_LOCKS;
    LL_SET_DEFAULT(1, FUSE_CAP_FLOCK_LOCKS);
    LL_SET_DEFAULT(1, FUSE_CAP_READDIRPLUS);
    LL_SET_DEFAULT(1, FUSE_CAP_READDIRPLUS_AUTO);
    se->conn.time_gran = 1;
    
    if (bufsize < FUSE_MIN_READ_BUFFER) {
        fprintf(stderr, "fuse: warning: buffer size too small: %zu\n",
            bufsize);
        bufsize = FUSE_MIN_READ_BUFFER;
    }
    se->bufsize = bufsize;

    if (se->conn.max_write > bufsize - FUSE_BUFFER_HEADER_SIZE)
        se->conn.max_write = bufsize - FUSE_BUFFER_HEADER_SIZE;

    se->got_init = 1;
    if (f_ll->ops.init) {
        int op_res = f_ll->ops.init(se, f_ll->user_data, in_hdr, inarg, &se->conn, out_hdr);
        if (out_hdr->error != 0 || op_res != 0) {
            se->error = -EPROTO;
            se->got_destroy = 1;
            return op_res;
        }
    }

    if (se->conn.want & (~se->conn.capable)) {
        fprintf(stderr, "fuse: error: filesystem requested capabilities "
            "0x%X that are not supported by kernel, aborting.\n",
            se->conn.want & (~se->conn.capable));
        se->error = -EPROTO;
        //fuse_session_exit(se);
        se->got_destroy = 1;
        out_hdr->error = -EPROTO;
        return 0;
    }

    if (inarg->flags & FUSE_MAX_PAGES) {
        outarg->flags |= FUSE_MAX_PAGES;
        outarg->max_pages = (se->conn.max_write - 1) / getpagesize() + 1;
    }
    /* Always enable big writes, this is superseded
       by the max_write option */
    outarg->flags |= FUSE_BIG_WRITES;

    if (se->conn.want & FUSE_CAP_ASYNC_READ)
        outarg->flags |= FUSE_ASYNC_READ;
    if (se->conn.want & FUSE_CAP_POSIX_LOCKS)
        outarg->flags |= FUSE_POSIX_LOCKS;
    if (se->conn.want & FUSE_CAP_ATOMIC_O_TRUNC)
        outarg->flags |= FUSE_ATOMIC_O_TRUNC;
    if (se->conn.want & FUSE_CAP_EXPORT_SUPPORT)
        outarg->flags |= FUSE_EXPORT_SUPPORT;
    if (se->conn.want & FUSE_CAP_DONT_MASK)
        outarg->flags |= FUSE_DONT_MASK;
    if (se->conn.want & FUSE_CAP_FLOCK_LOCKS)
        outarg->flags |= FUSE_FLOCK_LOCKS;
    if (se->conn.want & FUSE_CAP_AUTO_INVAL_DATA)
        outarg->flags |= FUSE_AUTO_INVAL_DATA;
    if (se->conn.want & FUSE_CAP_READDIRPLUS)
        outarg->flags |= FUSE_DO_READDIRPLUS;
    if (se->conn.want & FUSE_CAP_READDIRPLUS_AUTO)
        outarg->flags |= FUSE_READDIRPLUS_AUTO;
    if (se->conn.want & FUSE_CAP_ASYNC_DIO)
        outarg->flags |= FUSE_ASYNC_DIO;
    if (se->conn.want & FUSE_CAP_WRITEBACK_CACHE)
        outarg->flags |= FUSE_WRITEBACK_CACHE;
    if (se->conn.want & FUSE_CAP_POSIX_ACL)
        outarg->flags |= FUSE_POSIX_ACL;
    if (se->conn.want & FUSE_CAP_CACHE_SYMLINKS)
        outarg->flags |= FUSE_CACHE_SYMLINKS;
    if (se->conn.want & FUSE_CAP_EXPLICIT_INVAL_DATA)
        outarg->flags |= FUSE_EXPLICIT_INVAL_DATA;

    //if (inarg->flags & FUSE_INIT_EXT) {
    //	outarg->flags |= FUSE_INIT_EXT;
    //	outarg->flags2 = outarg->flags >> 32;
    //}

    outarg->max_readahead = se->conn.max_readahead;
    outarg->max_write = se->conn.max_write;
    if (se->conn.proto_minor >= 13) {
        if (se->conn.max_background >= (1 << 16))
            se->conn.max_background = (1 << 16) - 1;
        if (se->conn.congestion_threshold > se->conn.max_background)
            se->conn.congestion_threshold = se->conn.max_background;
        if (!se->conn.congestion_threshold) {
            se->conn.congestion_threshold = se->conn.max_background * 3 / 4;
        }

        outarg->max_background = se->conn.max_background;
        outarg->congestion_threshold = se->conn.congestion_threshold;
    }
    if (se->conn.proto_minor >= 23)
        outarg->time_gran = se->conn.time_gran;

#ifdef DEBUG_ENABLED
    printf("INIT out: %u.%u\n", outarg->major, outarg->minor);

    printf("* flags=0x%08x\n", outarg->flags);
    printf("* max_readahead=0x%08x\n", outarg->max_readahead);
    printf("* max_write=0x%08x\n", outarg->max_write);
    printf("* max_background=%i\n", outarg->max_background);
    printf("* congestion_threshold=%i\n", outarg->congestion_threshold);
    printf("* time_gran=%u\n", outarg->time_gran);
#endif
    if (inarg->minor < 5)
        outargsize = FUSE_COMPAT_INIT_OUT_SIZE;
    else if (inarg->minor < 23)
        outargsize = FUSE_COMPAT_22_INIT_OUT_SIZE;

    out_hdr->len+=outargsize;
    return 0;
}

static int fuse_ll_destroy(struct fuse_ll *f_ll,
                  struct iovec *fuse_in_iov, int in_iovcnt,
                  struct iovec *fuse_out_iov, int out_iovcnt,
                  void *completion_context) {
    if (in_iovcnt != 1 || out_iovcnt != 1) {
        fprintf(stderr, "fuser_destroy: invalid number of iovecs!\n");
        return -EINVAL;
    }

    struct fuse_in_header *in_hdr = (struct fuse_in_header *) fuse_in_iov[0].iov_base;
    struct fuse_out_header *out_hdr = (struct fuse_out_header *) fuse_out_iov[0].iov_base;
    out_hdr->unique = in_hdr->unique;
    out_hdr->len = sizeof(*out_hdr);
    out_hdr->error = 0;

    f_ll->se->got_destroy = 1;
    if (f_ll->ops.destroy)
        return f_ll->ops.destroy(f_ll->se, f_ll->user_data, in_hdr, out_hdr, completion_context);
    else
        return 0;
}


static int fuse_ll_lookup(struct fuse_ll *f_ll,
               struct iovec *fuse_in_iov, int in_iovcnt,
               struct iovec *fuse_out_iov, int out_iovcnt,
                  void *completion_context) {
    if (in_iovcnt != 2 || out_iovcnt != 2) {
        fprintf(stderr, "%s: invalid number of iovecs!\n", __func__);
        return -EINVAL;
    }

    struct fuse_in_header *in_hdr = (struct fuse_in_header *) fuse_in_iov[0].iov_base;
    struct fuse_out_header *out_hdr = (struct fuse_out_header *) fuse_out_iov[0].iov_base;
    out_hdr->unique = in_hdr->unique;
    out_hdr->len = sizeof(*out_hdr);
    out_hdr->error = 0;
    const char *const in_name = (const char *const) fuse_in_iov[1].iov_base;
    struct fuse_entry_out *out_entry = (struct fuse_entry_out *) fuse_out_iov[1].iov_base;

#ifdef DEBUG_ENABLED
    fuse_ll_debug_print_in_hdr(in_hdr);
    printf("* in_name: %s\n", in_name);
#endif

    if (!f_ll->se->init_done) {
        out_hdr->error = -EBUSY;
        return 0;
    }
    if (f_ll->ops.lookup)
        return f_ll->ops.lookup(f_ll->se, f_ll->user_data, in_hdr, in_name, out_hdr, out_entry, completion_context);
    else {
        out_hdr->error = -ENOSYS;
        return 0;
    }
}

static int fuse_ll_setattr(struct fuse_ll *f_ll,
               struct iovec *fuse_in_iov, int in_iovcnt,
               struct iovec *fuse_out_iov, int out_iovcnt,
                  void *completion_context) {

    if (in_iovcnt != 2 || out_iovcnt != 2) {
        fprintf(stderr, "fuser_setattr: invalid number of iovecs!\n");
        return -EINVAL;
    }

    struct fuse_in_header *in_hdr = (struct fuse_in_header *) fuse_in_iov[0].iov_base;
    struct fuse_out_header *out_hdr = (struct fuse_out_header *) fuse_out_iov[0].iov_base;
    out_hdr->unique = in_hdr->unique;
    out_hdr->len = sizeof(*out_hdr);
    out_hdr->error = 0;

    struct fuse_setattr_in *in_setattr = (struct fuse_setattr_in *) fuse_in_iov[1].iov_base;
    struct fuse_attr_out *out_attr = (struct fuse_attr_out *) fuse_out_iov[1].iov_base;

#ifdef DEBUG_ENABLED
    fuse_ll_debug_print_in_hdr(in_hdr);
    printf("* fh: %lu", in_setattr->fh);
#endif

    if (!f_ll->se->init_done) {
        out_hdr->error = -EBUSY;
        return 0;
    }

    if (f_ll->ops.setattr) {
        struct fuse_file_info *fi = NULL;
        struct fuse_file_info fi_store;
        struct stat s;
        memset(&s, 0, sizeof(s));
        convert_attr(in_setattr, &s);
        if (in_setattr->valid & FATTR_FH) {
            in_setattr->valid &= ~FATTR_FH;
            memset(&fi_store, 0, sizeof(fi_store));
            fi = &fi_store;
            fi->fh = in_setattr->fh;
        }
        in_setattr->valid &=
            FUSE_SET_ATTR_MODE	|
            FUSE_SET_ATTR_UID	|
            FUSE_SET_ATTR_GID	|
            FUSE_SET_ATTR_SIZE	|
            FUSE_SET_ATTR_ATIME	|
            FUSE_SET_ATTR_MTIME	|
            FUSE_SET_ATTR_ATIME_NOW	|
            FUSE_SET_ATTR_MTIME_NOW |
            FUSE_SET_ATTR_CTIME;

        return f_ll->ops.setattr(f_ll->se, f_ll->user_data, in_hdr, &s, in_setattr->valid, fi, out_hdr, out_attr, completion_context);
    } else if (f_ll->ops.setattr_async) {
        return f_ll->ops.setattr_async(f_ll->se, f_ll->user_data, in_hdr, in_setattr, out_hdr, out_attr, completion_context);
    } else {
        out_hdr->error = -ENOSYS;
        return 0;
    }
}
    

static int fuse_ll_create(struct fuse_ll *f_ll,
               struct iovec *fuse_in_iov, int in_iovcnt,
               struct iovec *fuse_out_iov, int out_iovcnt,
               void *completion_context) {
    if (in_iovcnt != 2 || out_iovcnt != 2) {
        fprintf(stderr, "%s: invalid number of iovecs!\n", __func__);
        return -EINVAL;
    }

    struct fuse_in_header *in_hdr = (struct fuse_in_header *) fuse_in_iov[0].iov_base;
    struct fuse_out_header *out_hdr = (struct fuse_out_header *) fuse_out_iov[0].iov_base;
    out_hdr->unique = in_hdr->unique;
    out_hdr->len = sizeof(*out_hdr);
    out_hdr->error = 0;

    char *name;
    struct fuse_entry_out *out_entry = (struct fuse_entry_out *) fuse_out_iov[1].iov_base;
    size_t entrysize = f_ll->se->conn.proto_minor < 9 ?
        FUSE_COMPAT_ENTRY_OUT_SIZE : sizeof(struct fuse_entry_out);
    struct fuse_open_out *out_open = (struct fuse_open_out *) (((char *)fuse_out_iov[1].iov_base) + entrysize);

    struct fuse_create_in *in_create;
    if (f_ll->se->conn.proto_minor >= 12) {
        in_create = (struct fuse_create_in *) fuse_in_iov[1].iov_base;
        name = ((char *) fuse_in_iov[1].iov_base) + sizeof(struct fuse_create_in);
    } else {
        struct fuse_create_in in_create_struct;
        in_create = &in_create_struct;
        struct fuse_open_in *in_open = (struct fuse_open_in *) fuse_in_iov[1].iov_base;

        in_create->flags = in_open->flags;
        in_create->mode = 0;
        in_create->umask = 0;
        in_create->padding = 0;
        name = ((char *) fuse_in_iov[1].iov_base) + sizeof(struct fuse_open_in);
    }

#ifdef DEBUG_ENABLED
    fuse_ll_debug_print_in_hdr(in_hdr);
    printf("* name: %s\n", name);
#endif

    if (!f_ll->se->init_done) {
        out_hdr->error = -EBUSY;
        return 0;
    }
    if (!f_ll->ops.create) {
        out_hdr->error = -ENOSYS;
        return 0;
    }

    return f_ll->ops.create(f_ll->se, f_ll->user_data, in_hdr, *in_create, name, out_hdr, out_entry, out_open, completion_context);
}

static int fuse_ll_flush(struct fuse_ll *f_ll,
                struct iovec *fuse_in_iov, int in_iovcnt,
                struct iovec *fuse_out_iov, int out_iovcnt,
                void *completion_context) {
    if (in_iovcnt != 2 || out_iovcnt != 1) {
        fprintf(stderr, "%s: invalid number of iovecs!\n", __func__);
        return -EINVAL;
    }

    struct fuse_in_header *in_hdr = (struct fuse_in_header *) fuse_in_iov[0].iov_base;
    struct fuse_out_header *out_hdr = (struct fuse_out_header *) fuse_out_iov[0].iov_base;
    out_hdr->unique = in_hdr->unique;
    out_hdr->len = sizeof(*out_hdr);
    out_hdr->error = 0;

    struct fuse_flush_in *in_flush = (struct fuse_flush_in *) fuse_in_iov[1].iov_base;
    struct fuse_file_info fi;

#ifdef DEBUG_ENABLED
    fuse_ll_debug_print_in_hdr(in_hdr);
    printf("* fh: %lu\n", in_flush->fh);
#endif

    if (!f_ll->se->init_done) {
        out_hdr->error = -EBUSY;
        return 0;
    }
    if (!f_ll->ops.flush) {
        out_hdr->error = -ENOSYS;
        return 0;
    }

    memset(&fi, 0, sizeof(fi));
    fi.fh = in_flush->fh;
    fi.flush = 1;
    if (f_ll->se->conn.proto_minor >= 7)
        fi.lock_owner = in_flush->lock_owner;

    return f_ll->ops.flush(f_ll->se, f_ll->user_data, in_hdr, fi, out_hdr, completion_context);
}

static int fuse_ll_setlk_common(struct fuse_ll *f_ll,
               struct iovec *fuse_in_iov, int in_iovcnt,
               struct iovec *fuse_out_iov, int out_iovcnt,
               void *completion_context, bool sleep) {
    if (in_iovcnt != 2 || out_iovcnt != 1) {
        fprintf(stderr, "%s: invalid number of iovecs!\n", __func__);
        return -EINVAL;
    }

    struct fuse_in_header *in_hdr = (struct fuse_in_header *) fuse_in_iov[0].iov_base;
    struct fuse_out_header *out_hdr = (struct fuse_out_header *) fuse_out_iov[0].iov_base;
    out_hdr->unique = in_hdr->unique;
    out_hdr->len = sizeof(*out_hdr);
    out_hdr->error = 0;

    struct fuse_lk_in *in_lk = (struct fuse_lk_in *) fuse_in_iov[1].iov_base;
    struct fuse_file_info fi;
    memset(&fi, 0, sizeof(fi));

#ifdef DEBUG_ENABLED
    fuse_ll_debug_print_in_hdr(in_hdr);
    printf("* fh: %lu\n", in_lk->fh);
#endif

    if (!f_ll->se->init_done) {
        out_hdr->error = -EBUSY;
        return 0;
    }

    fi.fh = in_lk->fh;
    fi.lock_owner = in_lk->owner;

    if (in_lk->lk_flags & FUSE_LK_FLOCK) {
        int op = 0;

        switch (in_lk->lk.type) {
        case F_RDLCK:
            op = LOCK_SH;
            break;
        case F_WRLCK:
            op = LOCK_EX;
            break;
        case F_UNLCK:
            op = LOCK_UN;
            break;
        }
        if (!sleep)
            op |= LOCK_NB;

        if (f_ll->ops.flock) {
            return f_ll->ops.flock(f_ll->se, f_ll->user_data, in_hdr, fi, op, out_hdr, completion_context);
        } else {
            out_hdr->error = -ENOSYS;
            return 0;
        }
    }
    else {
        out_hdr->error = -ENOSYS;
        return 0;
    }
}

static int fuse_ll_setlkw(struct fuse_ll *f_ll,
               struct iovec *fuse_in_iov, int in_iovcnt,
               struct iovec *fuse_out_iov, int out_iovcnt,
                  void *completion_context) {
    return fuse_ll_setlk_common(f_ll, fuse_in_iov, in_iovcnt, fuse_out_iov, out_iovcnt, completion_context, true);
}

static int fuse_ll_setlk(struct fuse_ll *f_ll,
               struct iovec *fuse_in_iov, int in_iovcnt,
               struct iovec *fuse_out_iov, int out_iovcnt,
                  void *completion_context) {
    return fuse_ll_setlk_common(f_ll, fuse_in_iov, in_iovcnt, fuse_out_iov, out_iovcnt, completion_context, true);
}

static int fuse_ll_getattr(struct fuse_ll *f_ll,
               struct iovec *fuse_in_iov, int in_iovcnt,
               struct iovec *fuse_out_iov, int out_iovcnt,
                  void *completion_context) {
    if (in_iovcnt != 2 || out_iovcnt != 2) {
        fprintf(stderr, "fuser_mirror_getattr: invalid number of iovecs!\n");
        return -EINVAL;
    }

    struct fuse_in_header *in_hdr = (struct fuse_in_header *) fuse_in_iov[0].iov_base;
    struct fuse_out_header *out_hdr = (struct fuse_out_header *) fuse_out_iov[0].iov_base;
    out_hdr->unique = in_hdr->unique;
    out_hdr->len = sizeof(*out_hdr);
    out_hdr->error = 0;

    struct fuse_getattr_in *in_getattr = (struct fuse_getattr_in *) fuse_in_iov[1].iov_base;
    struct fuse_attr_out *out_attr = (struct fuse_attr_out *) fuse_out_iov[1].iov_base;

#ifdef DEBUG_ENABLED
    fuse_ll_debug_print_in_hdr(in_hdr);
    printf("* fh: %lu\n", in_getattr->fh);
#endif

    if (!f_ll->se->init_done) {
        out_hdr->error = -EBUSY;
        return 0;
    }
    if (!f_ll->ops.getattr) {
        out_hdr->error = -ENOSYS;
        return 0;
    }

    return f_ll->ops.getattr(f_ll->se, f_ll->user_data, in_hdr, in_getattr, out_hdr, out_attr, completion_context);
}

static int fuse_ll_opendir(struct fuse_ll *f_ll,
               struct iovec *fuse_in_iov, int in_iovcnt,
               struct iovec *fuse_out_iov, int out_iovcnt,
                  void *completion_context) {
    if (in_iovcnt != 2 || out_iovcnt != 2) {
        fprintf(stderr, "%s: invalid number of iovecs!\n", __func__);
        return -EINVAL;
    }

    struct fuse_in_header *in_hdr = (struct fuse_in_header *) fuse_in_iov[0].iov_base;
    struct fuse_out_header *out_hdr = (struct fuse_out_header *) fuse_out_iov[0].iov_base;
    out_hdr->unique = in_hdr->unique;
    out_hdr->len = sizeof(*out_hdr);
    out_hdr->error = 0;

    struct fuse_open_in *in_open = (struct fuse_open_in *) fuse_in_iov[1].iov_base;
    struct fuse_open_out *out_open = (struct fuse_open_out *) fuse_out_iov[1].iov_base;

#ifdef DEBUG_ENABLED
    fuse_ll_debug_print_in_hdr(in_hdr);
    printf("* flags: 0x%X\n", in_open->flags);
#endif

    if (!f_ll->se->init_done) {
        out_hdr->error = -EBUSY;
        return 0;
    }
    if (!f_ll->ops.opendir) {
        out_hdr->error = -ENOSYS;
        return 0;
    }

    return f_ll->ops.opendir(f_ll->se, f_ll->user_data, in_hdr, in_open, out_hdr, out_open, completion_context);
}

static int fuse_ll_releasedir(struct fuse_ll *f_ll,
               struct iovec *fuse_in_iov, int in_iovcnt,
               struct iovec *fuse_out_iov, int out_iovcnt,
                  void *completion_context) {
    if (in_iovcnt != 2 || out_iovcnt != 1) {
        fprintf(stderr, "%s: invalid number of iovecs!\n", __func__);
        return -EINVAL;
    }

    struct fuse_in_header *in_hdr = (struct fuse_in_header *) fuse_in_iov[0].iov_base;
    struct fuse_out_header *out_hdr = (struct fuse_out_header *) fuse_out_iov[0].iov_base;
    out_hdr->unique = in_hdr->unique;
    out_hdr->len = sizeof(*out_hdr);
    out_hdr->error = 0;

    struct fuse_release_in *in_release = (struct fuse_release_in *) fuse_in_iov[1].iov_base;

#ifdef DEBUG_ENABLED
    fuse_ll_debug_print_in_hdr(in_hdr);
    printf("* fh: %lu\n", in_release->fh);
#endif

    if (!f_ll->se->init_done) {
        out_hdr->error = -EBUSY;
        return 0;
    }
    if (!f_ll->ops.releasedir) {
        out_hdr->error = -ENOSYS;
        return 0;
    }

    return f_ll->ops.releasedir(f_ll->se, f_ll->user_data, in_hdr, in_release, out_hdr, completion_context);
}

static int fuse_ll_readdir_common(struct fuse_ll *f_ll,
               struct iovec *fuse_in_iov, int in_iovcnt,
               struct iovec *fuse_out_iov, int out_iovcnt,
               void *completion_context,
               bool plus) {
    if (!(in_iovcnt > 1 || out_iovcnt > 1)) {
        fprintf(stderr, "%s: invalid number of iovecs!\n", __func__);
        return -EINVAL;
    }

    struct fuse_in_header *in_hdr = (struct fuse_in_header *) fuse_in_iov[0].iov_base;
    struct fuse_out_header *out_hdr = (struct fuse_out_header *) fuse_out_iov[0].iov_base;
    out_hdr->unique = in_hdr->unique;
    out_hdr->len = sizeof(*out_hdr);
    out_hdr->error = 0;


    struct fuse_read_in *in_read = (struct fuse_read_in *) fuse_in_iov[1].iov_base;

#ifdef DEBUG_ENABLED
    fuse_ll_debug_print_in_hdr(in_hdr);
    printf("* fh: %lu\n", in_read->fh);
#endif

    if (!f_ll->se->init_done) {
        out_hdr->error = -EBUSY;
        return 0;
    }
    if (!f_ll->ops.readdir) {
        out_hdr->error = -ENOSYS;
        return 0;
    }

    struct iov read_iov;
    iov_init(&read_iov, &fuse_out_iov[1], out_iovcnt-1);

    return f_ll->ops.readdir(f_ll->se, f_ll->user_data, in_hdr, in_read, plus, out_hdr, read_iov, completion_context);
}

static int fuse_ll_readdir(struct fuse_ll *f_ll,
               struct iovec *fuse_in_iov, int in_iovcnt,
               struct iovec *fuse_out_iov, int out_iovcnt,
                void *completion_context) {
    return fuse_ll_readdir_common(f_ll, fuse_in_iov, in_iovcnt, fuse_out_iov, out_iovcnt, completion_context, false);
}

static int fuse_ll_readdirplus(struct fuse_ll *f_ll,
               struct iovec *fuse_in_iov, int in_iovcnt,
               struct iovec *fuse_out_iov, int out_iovcnt,
                  void *completion_context) {
    return fuse_ll_readdir_common(f_ll, fuse_in_iov, in_iovcnt, fuse_out_iov, out_iovcnt, completion_context, true);
}

static int fuse_ll_open(struct fuse_ll *f_ll,
               struct iovec *fuse_in_iov, int in_iovcnt,
               struct iovec *fuse_out_iov, int out_iovcnt,
                  void *completion_context) {
    if (in_iovcnt != 2 || out_iovcnt != 2) {
        fprintf(stderr, "%s: invalid number of iovecs!\n", __func__);
        return -EINVAL;
    }

    struct fuse_in_header *in_hdr = (struct fuse_in_header *) fuse_in_iov[0].iov_base;
    struct fuse_out_header *out_hdr = (struct fuse_out_header *) fuse_out_iov[0].iov_base;
    out_hdr->unique = in_hdr->unique;
    out_hdr->len = sizeof(*out_hdr);
    out_hdr->error = 0;

    struct fuse_open_in *in_open = (struct fuse_open_in *) fuse_in_iov[1].iov_base;
    struct fuse_open_out *out_open = (struct fuse_open_out *) fuse_out_iov[1].iov_base;

#ifdef DEBUG_ENABLED
    fuse_ll_debug_print_in_hdr(in_hdr);
    printf("* flags: 0x%X\n", in_open->flags);
#endif

    if (!f_ll->se->init_done) {
        out_hdr->error = -EBUSY;
        return 0;
    }
    if (!f_ll->ops.open) {
        out_hdr->error = -ENOSYS;
        return 0;
    }

    return f_ll->ops.open(f_ll->se, f_ll->user_data, in_hdr, in_open, out_hdr, out_open, completion_context);
}

static int fuse_ll_release(struct fuse_ll *f_ll,
               struct iovec *fuse_in_iov, int in_iovcnt,
               struct iovec *fuse_out_iov, int out_iovcnt,
                  void *completion_context) {
    if (in_iovcnt != 2 || out_iovcnt != 1) {
        fprintf(stderr, "%s: invalid number of iovecs!\n", __func__);
        return -EINVAL;
    }

    struct fuse_in_header *in_hdr = (struct fuse_in_header *) fuse_in_iov[0].iov_base;
    struct fuse_out_header *out_hdr = (struct fuse_out_header *) fuse_out_iov[0].iov_base;
    out_hdr->unique = in_hdr->unique;
    out_hdr->len = sizeof(*out_hdr);
    out_hdr->error = 0;

    struct fuse_release_in *in_release = (struct fuse_release_in *) fuse_in_iov[1].iov_base;

#ifdef DEBUG_ENABLED
    fuse_ll_debug_print_in_hdr(in_hdr);
    printf("* fh: %lu\n", in_release->fh);
    printf("* flags: 0x%X\n", in_release->flags);
    printf("* release_flags: 0x%X\n", in_release->release_flags);
    printf("* lock_owner: %lu\n", in_release->lock_owner);
#endif

    if (!f_ll->se->init_done) {
        out_hdr->error = -EBUSY;
        return 0;
    }
    if (!f_ll->ops.release) {
        out_hdr->error = -ENOSYS;
        return 0;
    }

    return f_ll->ops.release(f_ll->se, f_ll->user_data, in_hdr, in_release, out_hdr, completion_context);
}

static int fuse_ll_fsync(struct fuse_ll *f_ll,
               struct iovec *fuse_in_iov, int in_iovcnt,
               struct iovec *fuse_out_iov, int out_iovcnt,
                  void *completion_context) {
    if (in_iovcnt != 2 || out_iovcnt != 1) {
        fprintf(stderr, "%s: invalid number of iovecs!\n", __func__);
        return -EINVAL;
    }

    struct fuse_in_header *in_hdr = (struct fuse_in_header *) fuse_in_iov[0].iov_base;
    struct fuse_out_header *out_hdr = (struct fuse_out_header *) fuse_out_iov[0].iov_base;
    out_hdr->unique = in_hdr->unique;
    out_hdr->len = sizeof(*out_hdr);
    out_hdr->error = 0;

    struct fuse_fsync_in *in_fsync = (struct fuse_fsync_in *) fuse_in_iov[1].iov_base;

#ifdef DEBUG_ENABLED
    fuse_ll_debug_print_in_hdr(in_hdr);
    printf("* fh: %lu\n", in_fsync->fh);
    printf("* fsync_flags: 0x%X\n", in_fsync->fsync_flags);
#endif

    if (!f_ll->se->init_done) {
        out_hdr->error = -EBUSY;
        return 0;
    }
    if (!f_ll->ops.fsync) {
        out_hdr->error = -ENOSYS;
        return 0;
    }

    return f_ll->ops.fsync(f_ll->se, f_ll->user_data, in_hdr, in_fsync, out_hdr, completion_context);
}

static int fuse_ll_fsyncdir(struct fuse_ll *f_ll,
               struct iovec *fuse_in_iov, int in_iovcnt,
               struct iovec *fuse_out_iov, int out_iovcnt,
                  void *completion_context) {
    if (in_iovcnt != 2 || out_iovcnt != 1) {
        fprintf(stderr, "%s: invalid number of iovecs!\n", __func__);
        return -EINVAL;
    }

    struct fuse_in_header *in_hdr = (struct fuse_in_header *) fuse_in_iov[0].iov_base;
    struct fuse_out_header *out_hdr = (struct fuse_out_header *) fuse_out_iov[0].iov_base;
    out_hdr->unique = in_hdr->unique;
    out_hdr->len = sizeof(*out_hdr);
    out_hdr->error = 0;

    struct fuse_fsync_in *in_fsync = (struct fuse_fsync_in *) fuse_in_iov[1].iov_base;

#ifdef DEBUG_ENABLED
    fuse_ll_debug_print_in_hdr(in_hdr);
    printf("* fh: %lu\n", in_fsync->fh);
    printf("* fsync_flags: 0x%X\n", in_fsync->fsync_flags);
#endif

    if (!f_ll->se->init_done) {
        out_hdr->error = -EBUSY;
        return 0;
    }
    if (!f_ll->ops.fsyncdir) {
        out_hdr->error = -ENOSYS;
        return 0;
    }

    return f_ll->ops.fsyncdir(f_ll->se, f_ll->user_data, in_hdr, in_fsync, out_hdr, completion_context);
}

static int fuse_ll_rmdir(struct fuse_ll *f_ll,
               struct iovec *fuse_in_iov, int in_iovcnt,
               struct iovec *fuse_out_iov, int out_iovcnt,
                  void *completion_context) {
    if (in_iovcnt != 2 || out_iovcnt != 1) {
        fprintf(stderr, "%s: invalid number of iovecs!\n", __func__);
        return -EINVAL;
    }

    struct fuse_in_header *in_hdr = (struct fuse_in_header *) fuse_in_iov[0].iov_base;
    struct fuse_out_header *out_hdr = (struct fuse_out_header *) fuse_out_iov[0].iov_base;
    out_hdr->unique = in_hdr->unique;
    out_hdr->len = sizeof(*out_hdr);
    out_hdr->error = 0;

    const char *const in_name = (const char *) fuse_in_iov[1].iov_base;

#ifdef DEBUG_ENABLED
    fuse_ll_debug_print_in_hdr(in_hdr);
    printf("* name: %s\n", in_name);
#endif

    if (!f_ll->se->init_done) {
        out_hdr->error = -EBUSY;
        return 0;
    }
    if (!f_ll->ops.rmdir) {
        out_hdr->error = -ENOSYS;
        return 0;
    }

    return f_ll->ops.rmdir(f_ll->se, f_ll->user_data, in_hdr, in_name, out_hdr, completion_context);
}

static int fuse_ll_forget(struct fuse_ll *f_ll,
               struct iovec *fuse_in_iov, int in_iovcnt,
               struct iovec *fuse_out_iov, int out_iovcnt,
                  void *completion_context) {
    if (in_iovcnt != 1 || out_iovcnt != 0) {
        fprintf(stderr, "%s: invalid number of iovecs!\n", __func__);
        return -EINVAL;
    }

    struct fuse_in_header *in_hdr = (struct fuse_in_header *) fuse_in_iov[0].iov_base;
    struct fuse_forget_in *in_forget = (struct fuse_forget_in *) (((char *) fuse_in_iov[0].iov_base) + sizeof(struct fuse_in_header));

#ifdef DEBUG_ENABLED
    fuse_ll_debug_print_in_hdr(in_hdr);
#endif

    if (f_ll->ops.forget)
        return f_ll->ops.forget(f_ll->se, f_ll->user_data, in_hdr, in_forget, completion_context);
    else
        return 0;
}

static int fuse_ll_batch_forget(struct fuse_ll *f_ll,
               struct iovec *fuse_in_iov, int in_iovcnt,
               struct iovec *fuse_out_iov, int out_iovcnt,
                  void *completion_context) {
    if (in_iovcnt != 1 || out_iovcnt != 0) {
        fprintf(stderr, "%s: invalid number of iovecs!\n", __func__);
        return -EINVAL;
    }

    struct fuse_in_header *in_hdr = (struct fuse_in_header *) fuse_in_iov[0].iov_base;
    struct fuse_batch_forget_in *in_batch_forget = (struct fuse_batch_forget_in *) (((char *) fuse_in_iov[0].iov_base)
            + sizeof(struct fuse_in_header));
    struct fuse_forget_one *in_forget = (struct fuse_forget_one *) (((char *) fuse_in_iov[0].iov_base)
            + sizeof(struct fuse_in_header) + sizeof(struct fuse_batch_forget_in));

#ifdef DEBUG_ENABLED
    fuse_ll_debug_print_in_hdr(in_hdr);
#endif

    if (f_ll->ops.batch_forget)
        return f_ll->ops.batch_forget(f_ll->se, f_ll->user_data, in_hdr, in_batch_forget, in_forget, completion_context);
    else
        return 0;
}

static int fuse_ll_rename(struct fuse_ll *f_ll,
               struct iovec *fuse_in_iov, int in_iovcnt,
               struct iovec *fuse_out_iov, int out_iovcnt,
                  void *completion_context) {
    if (in_iovcnt != 2 || out_iovcnt != 1) {
        fprintf(stderr, "%s: invalid number of iovecs!\n", __func__);
        return -EINVAL;
    }

    struct fuse_in_header *in_hdr = (struct fuse_in_header *) fuse_in_iov[0].iov_base;
    struct fuse_out_header *out_hdr = (struct fuse_out_header *) fuse_out_iov[0].iov_base;
    out_hdr->unique = in_hdr->unique;
    out_hdr->len = sizeof(*out_hdr);
    out_hdr->error = 0;

    struct fuse_rename_in *in_rename = (struct fuse_rename_in *) fuse_in_iov[1].iov_base;
    const char *name = ((char *) fuse_in_iov[1].iov_base) + sizeof(*in_rename);
    const char *new_name = (const char *) name + strlen(name) + 1;

#ifdef DEBUG_ENABLED
    fuse_ll_debug_print_in_hdr(in_hdr);
    printf("* name: %s\n", name);
    printf("* newdir: %lu\n", in_rename->newdir);
    printf("* new_name: %s\n", new_name);
#endif

    if (!f_ll->se->init_done) {
        out_hdr->error = -EBUSY;
        return 0;
    }
    if (!f_ll->ops.rename) {
        out_hdr->error = -ENOSYS;
        return 0;
    }

    return f_ll->ops.rename(f_ll->se, f_ll->user_data, in_hdr, name, in_rename->newdir,
                    new_name, 0, out_hdr, completion_context);
}

static int fuse_ll_rename2(struct fuse_ll *f_ll,
               struct iovec *fuse_in_iov, int in_iovcnt,
               struct iovec *fuse_out_iov, int out_iovcnt,
                  void *completion_context) {
    if (in_iovcnt != 2 || out_iovcnt != 1) {
        fprintf(stderr, "%s: invalid number of iovecs!\n", __func__);
        return -EINVAL;
    }

    struct fuse_in_header *in_hdr = (struct fuse_in_header *) fuse_in_iov[0].iov_base;
    struct fuse_out_header *out_hdr = (struct fuse_out_header *) fuse_out_iov[0].iov_base;
    out_hdr->unique = in_hdr->unique;
    out_hdr->len = sizeof(*out_hdr);
    out_hdr->error = 0;

    struct fuse_rename2_in *in_rename2 = (struct fuse_rename2_in *) fuse_in_iov[1].iov_base;
    const char *name = ((char *) fuse_in_iov[1].iov_base) + sizeof(*in_rename2);
    const char *new_name = (const char *) name + strlen(name) + 1;

#ifdef DEBUG_ENABLED
    fuse_ll_debug_print_in_hdr(in_hdr);
    printf("* name: %s\n", name);
    printf("* newdir: %lu\n", in_rename2->newdir);
    printf("* flags: 0x%X\n", in_rename2->flags);
    printf("* new_name: %s\n", new_name);
#endif

    if (!f_ll->se->init_done) {
        out_hdr->error = -EBUSY;
        return 0;
    }
    if (!f_ll->ops.rename) {
        out_hdr->error = -ENOSYS;
        return 0;
    }


    return f_ll->ops.rename(f_ll->se, f_ll->user_data, in_hdr, name, in_rename2->newdir,
                    new_name, in_rename2->flags, out_hdr, completion_context);
}

static int fuse_ll_read(struct fuse_ll *f_ll,
               struct iovec *fuse_in_iov, int in_iovcnt,
               struct iovec *fuse_out_iov, int out_iovcnt,
                  void *completion_context) {
    if (in_iovcnt != 2 || out_iovcnt < 2) {
        fprintf(stderr, "%s: invalid number of iovecs!\n", __func__);
        return -EINVAL;
    }

    struct fuse_in_header *in_hdr = (struct fuse_in_header *) fuse_in_iov[0].iov_base;
    struct fuse_out_header *out_hdr = (struct fuse_out_header *) fuse_out_iov[0].iov_base;
    out_hdr->unique = in_hdr->unique;
    out_hdr->len = sizeof(*out_hdr);
    out_hdr->error = 0;

    struct fuse_read_in *in_read = (struct fuse_read_in *) fuse_in_iov[1].iov_base;

#ifdef DEBUG_ENABLED
    fuse_ll_debug_print_in_hdr(in_hdr);
    printf("* fh: %lu\n", in_read->fh);
    printf("* offset: %lu\n", in_read->offset);
    printf("* size: %u\n", in_read->size);
    printf("* read_flags: 0x%X\n", in_read->read_flags);
    printf("* lock_owner: %lu\n", in_read->lock_owner);
    printf("* flags: 0x%X\n", in_read->flags);
#endif

    if (!f_ll->se->init_done) {
        out_hdr->error = -EBUSY;
        return 0;
    }
    if (!f_ll->ops.read) {
        out_hdr->error = -ENOSYS;
        return 0;
    }

    size_t total_read_iov_size = 0;
    for (int i = 1; i < out_iovcnt; i++) {
        total_read_iov_size += fuse_out_iov[i].iov_len;
    }

    if (total_read_iov_size != in_read->size) {
        fprintf(stderr, "%s: iovecs not the same size as the amount of data requested to read!!!\n", __func__);
        return -EINVAL;
    }

    return f_ll->ops.read(f_ll->se, f_ll->user_data, in_hdr, in_read, out_hdr,
            &fuse_out_iov[1], out_iovcnt-1, completion_context);
}

static int fuse_ll_write(struct fuse_ll *f_ll,
               struct iovec *fuse_in_iov, int in_iovcnt,
               struct iovec *fuse_out_iov, int out_iovcnt,
               void *completion_context) {
    if (in_iovcnt < 2 || out_iovcnt != 2) {
        fprintf(stderr, "%s: invalid number of iovecs!\n", __func__);
        return -EINVAL;
    }

    struct fuse_in_header *in_hdr = (struct fuse_in_header *) fuse_in_iov[0].iov_base;
    struct fuse_out_header *out_hdr = (struct fuse_out_header *) fuse_out_iov[0].iov_base;
    out_hdr->unique = in_hdr->unique;
    out_hdr->len = sizeof(*out_hdr);
    out_hdr->error = 0;

    struct fuse_write_in *in_write = (struct fuse_write_in *) fuse_in_iov[1].iov_base;
    struct fuse_write_out *out_write = (struct fuse_write_out *) fuse_out_iov[1].iov_base;

#ifdef DEBUG_ENABLED
    fuse_ll_debug_print_in_hdr(in_hdr);
    printf("* fh: %lu\n", in_write->fh);
    printf("* offset: %lu\n", in_write->offset);
    printf("* size: %u\n", in_write->size);
    printf("* read_flags: 0x%X\n", in_write->write_flags);
    printf("* lock_owner: %lu\n", in_write->lock_owner);
    printf("* flags: 0x%X\n", in_write->flags);
#endif

    if (!f_ll->se->init_done) {
        out_hdr->error = -EBUSY;
        return 0;
    }
    if (!f_ll->ops.write) {
        out_hdr->error = -ENOSYS;
        return 0;
    }

    size_t total_write_iov_size = 0;
    for (int i = 2; i < in_iovcnt; i++) {
        total_write_iov_size += fuse_in_iov[i].iov_len;
    }

    if (total_write_iov_size != in_write->size) {
        fprintf(stderr, "%s: iovecs not the same size as the amount of data requested to write!!!\n", __func__);
        return -EINVAL;
    }

    return f_ll->ops.write(f_ll->se, f_ll->user_data, in_hdr, in_write,
            &fuse_in_iov[2], in_iovcnt-2, out_hdr, out_write, completion_context);
}

static int fuse_ll_mknod(struct fuse_ll *f_ll,
               struct iovec *fuse_in_iov, int in_iovcnt,
               struct iovec *fuse_out_iov, int out_iovcnt,
           void *completion_context)
{
    if (in_iovcnt != 2 || out_iovcnt != 2) {
        fprintf(stderr, "%s: invalid number of iovecs!\n", __func__);
        return -EINVAL;
    }

    struct fuse_in_header *in_hdr = (struct fuse_in_header *) fuse_in_iov[0].iov_base;
    struct fuse_out_header *out_hdr = (struct fuse_out_header *) fuse_out_iov[0].iov_base;
    out_hdr->unique = in_hdr->unique;
    out_hdr->len = sizeof(*out_hdr);
    out_hdr->error = 0;

    if (!f_ll->se->init_done) {
        out_hdr->error = -EBUSY;
        return 0;
    }
    if (!f_ll->ops.mknod) {
        out_hdr->error = -ENOSYS;
        return 0;
    }

    struct fuse_mknod_in *in_mknod = (struct fuse_mknod_in *) fuse_in_iov[1].iov_base;
    const char *in_name;
    if (f_ll->se->conn.proto_minor < 12)
        in_name = ((char *) fuse_in_iov[1].iov_base) + FUSE_COMPAT_MKNOD_IN_SIZE;
    else
        in_name = ((char *) fuse_in_iov[1].iov_base) + sizeof(struct fuse_mknod_in);

    struct fuse_entry_out *out_entry = (struct fuse_entry_out *) fuse_out_iov[1].iov_base;

#ifdef DEBUG_ENABLED
    fuse_ll_debug_print_in_hdr(in_hdr);
    printf("* name: %s\n", in_name);
    printf("* mode: %u\n", in_mknod->mode);
    printf("* rdev: %u\n", in_mknod->rdev);
    printf("* umask: 0x%X\n", in_mknod->umask);
#endif

    return f_ll->ops.mknod(f_ll->se, f_ll->user_data, in_hdr, in_mknod, in_name, out_hdr, out_entry, completion_context);
}

static int fuse_ll_mkdir(struct fuse_ll *f_ll,
               struct iovec *fuse_in_iov, int in_iovcnt,
               struct iovec *fuse_out_iov, int out_iovcnt,
           void *completion_context)
{
    if (in_iovcnt != 2 || out_iovcnt != 2) {
        fprintf(stderr, "%s: invalid number of iovecs!\n", __func__);
        return -EINVAL;
    }

    struct fuse_in_header *in_hdr = (struct fuse_in_header *) fuse_in_iov[0].iov_base;
    struct fuse_out_header *out_hdr = (struct fuse_out_header *) fuse_out_iov[0].iov_base;
    out_hdr->unique = in_hdr->unique;
    out_hdr->len = sizeof(*out_hdr);
    out_hdr->error = 0;

    struct fuse_mkdir_in *in_mkdir = (struct fuse_mkdir_in *) fuse_in_iov[1].iov_base;
    const char *in_name = ((char *) fuse_in_iov[1].iov_base) + sizeof(struct fuse_mkdir_in);
    struct fuse_entry_out *out_entry = (struct fuse_entry_out *) fuse_out_iov[1].iov_base;

#ifdef DEBUG_ENABLED
    fuse_ll_debug_print_in_hdr(in_hdr);
    printf("* name: %s\n", in_name);
    printf("* mode: %u\n", in_mkdir->mode);
    printf("* umask: 0x%X\n", in_mkdir->umask);
#endif

    if (!f_ll->se->init_done) {
        out_hdr->error = -EBUSY;
        return 0;
    }
    if (!f_ll->ops.mkdir) {
        out_hdr->error = -ENOSYS;
        return 0;
    }

    return f_ll->ops.mkdir(f_ll->se, f_ll->user_data, in_hdr, in_mkdir, in_name, out_hdr, out_entry, completion_context);
}

static int fuse_ll_symlink(struct fuse_ll *f_ll,
               struct iovec *fuse_in_iov, int in_iovcnt,
               struct iovec *fuse_out_iov, int out_iovcnt,
           void *completion_context)
{
    if (in_iovcnt != 2 || out_iovcnt != 2) {
        fprintf(stderr, "%s: invalid number of iovecs!\n", __func__);
        return -EINVAL;
    }

    struct fuse_in_header *in_hdr = (struct fuse_in_header *) fuse_in_iov[0].iov_base;
    struct fuse_out_header *out_hdr = (struct fuse_out_header *) fuse_out_iov[0].iov_base;
    out_hdr->unique = in_hdr->unique;
    out_hdr->len = sizeof(*out_hdr);
    out_hdr->error = 0;

    const char *in_name = (const char *) fuse_in_iov[1].iov_base;
    const char *in_link_name = in_name + strlen(in_name)+1;
    struct fuse_entry_out *out_entry = (struct fuse_entry_out *) fuse_out_iov[1].iov_base;

#ifdef DEBUG_ENABLED
    struct fuse_link_in *in_link = (struct fuse_link_in *) in_link_name + strlen(in_link_name) + 1;
    fuse_ll_debug_print_in_hdr(in_hdr);
    printf("* name: %s\n", in_name);
    printf("* link_name: %s\n", in_link_name);
    printf("* oldnodeid: %lu\n", in_link->oldnodeid);
#endif

    if (!f_ll->se->init_done) {
        out_hdr->error = -EBUSY;
        return 0;
    }
    if (!f_ll->ops.symlink) {
        out_hdr->error = -ENOSYS;
        return 0;
    }

    return f_ll->ops.symlink(f_ll->se, f_ll->user_data, in_hdr, in_name, in_link_name, out_hdr, out_entry, completion_context);
}

static int fuse_ll_statfs(struct fuse_ll *f_ll,
               struct iovec *fuse_in_iov, int in_iovcnt,
               struct iovec *fuse_out_iov, int out_iovcnt,
           void *completion_context)
{
    if (in_iovcnt != 1 || out_iovcnt != 2) {
        fprintf(stderr, "%s: invalid number of iovecs!\n", __func__);
        return -EINVAL;
    }

    struct fuse_in_header *in_hdr = (struct fuse_in_header *) fuse_in_iov[0].iov_base;
    struct fuse_out_header *out_hdr = (struct fuse_out_header *) fuse_out_iov[0].iov_base;
    out_hdr->unique = in_hdr->unique;
    out_hdr->len = sizeof(*out_hdr);
    out_hdr->error = 0;

    struct fuse_statfs_out *out_statfs = (struct fuse_statfs_out *) fuse_out_iov[1].iov_base;

#ifdef DEBUG_ENABLED
    fuse_ll_debug_print_in_hdr(in_hdr);
#endif

    if (!f_ll->se->init_done) {
        out_hdr->error = -EBUSY;
        return 0;
    }
    if (!f_ll->ops.statfs) {
        out_hdr->error = -ENOSYS;
        return 0;
    }

    return f_ll->ops.statfs(f_ll->se, f_ll->user_data, in_hdr, out_hdr, out_statfs, completion_context);
}

static int fuse_ll_unlink(struct fuse_ll *f_ll,
               struct iovec *fuse_in_iov, int in_iovcnt,
               struct iovec *fuse_out_iov, int out_iovcnt,
           void *completion_context)
{
    if (in_iovcnt != 2 || out_iovcnt != 1) {
        fprintf(stderr, "%s: invalid number of iovecs!\n", __func__);
        return -EINVAL;
    }

    struct fuse_in_header *in_hdr = (struct fuse_in_header *) fuse_in_iov[0].iov_base;
    struct fuse_out_header *out_hdr = (struct fuse_out_header *) fuse_out_iov[0].iov_base;
    out_hdr->unique = in_hdr->unique;
    out_hdr->len = sizeof(*out_hdr);
    out_hdr->error = 0;

    const char *in_name = (const char *) fuse_in_iov[1].iov_base;

#ifdef DEBUG_ENABLED
    fuse_ll_debug_print_in_hdr(in_hdr);
    printf("* name: %s\n", in_name);
#endif

    if (!f_ll->se->init_done) {
        out_hdr->error = -EBUSY;
        return 0;
    }
    if (!f_ll->ops.unlink) {
        out_hdr->error = -ENOSYS;
        return 0;
    }

    return f_ll->ops.unlink(f_ll->se, f_ll->user_data , in_hdr, in_name, out_hdr, completion_context);
}

static int fuse_ll_readlink(struct fuse_ll *f_ll,
               struct iovec *fuse_in_iov, int in_iovcnt,
               struct iovec *fuse_out_iov, int out_iovcnt,
           void *completion_context)
{
    // TODO this function is still WIP, printing to see how it works
    fprintf(stderr, "READLINK called with:\n");
    for (int i = 0; i < in_iovcnt; i++) {
        fprintf(stderr, " in_iov[%d].len=%ld", i, fuse_in_iov[i].iov_len);
    }
    fprintf(stderr, "\n");
    for (int i = 0; i < out_iovcnt; i++) {
        fprintf(stderr, " out_iov[%d].len=%ld", i, fuse_out_iov[i].iov_len);
    }
    fprintf(stderr, "\n");
    (void) f_ll;

    struct fuse_out_header *out_hdr = (struct fuse_out_header *) fuse_out_iov[0].iov_base;
    out_hdr->error = -ENOSYS;
    return 0;
}

static int fuse_ll_fallocate(struct fuse_ll *f_ll,
        struct iovec *fuse_in_iov, int in_iovcnt,
        struct iovec *fuse_out_iov, int out_iovcnt,
        void *completion_context)
{
    if (in_iovcnt != 2 || out_iovcnt != 1) {
        fprintf(stderr, "%s: invalid number of iovecs!\n", __func__);
        return -EINVAL;
    }

    struct fuse_in_header *in_hdr = (struct fuse_in_header *) fuse_in_iov[0].iov_base;
    struct fuse_out_header *out_hdr = (struct fuse_out_header *) fuse_out_iov[0].iov_base;
    out_hdr->unique = in_hdr->unique;
    out_hdr->len = sizeof(*out_hdr);
    out_hdr->error = 0;

    struct fuse_fallocate_in *in_fallocate = (struct fuse_fallocate_in *) fuse_in_iov[1].iov_base;

#ifdef DEBUG_ENABLED
    fuse_ll_debug_print_in_hdr(in_hdr);
    printf("* fh: %lu\n", in_fallocate->fh);
    printf("* offset: %lu\n", in_fallocate->offset);
    printf("* length: %lu\n", in_fallocate->length);
    printf("* mode: %u\n", in_fallocate->mode);
#endif

    if (!f_ll->se->init_done) {
        out_hdr->error = -EBUSY;
        return 0;
    }
    if (!f_ll->ops.fallocate) {
        out_hdr->error = -ENOSYS;
        return 0;
    }

    return f_ll->ops.fallocate(f_ll->se, f_ll->user_data, in_hdr, in_fallocate, out_hdr, completion_context);
}

static void fuse_ll_map(struct fuse_ll *fuse_ll) {
    // NULL maps to fuse_unknown
    memset(&fuse_ll->fuse_handlers, 0, sizeof(fuse_ll->fuse_handlers));

    fuse_ll->fuse_handlers[FUSE_INIT] = fuse_ll_init;
    fuse_ll->fuse_handlers[FUSE_DESTROY] = fuse_ll_destroy;
    fuse_ll->fuse_handlers[FUSE_GETATTR] = fuse_ll_getattr;
    fuse_ll->fuse_handlers[FUSE_LOOKUP] = fuse_ll_lookup;
    fuse_ll->fuse_handlers[FUSE_SETATTR] = fuse_ll_setattr;
    fuse_ll->fuse_handlers[FUSE_OPENDIR] = fuse_ll_opendir;
    fuse_ll->fuse_handlers[FUSE_RELEASEDIR] = fuse_ll_releasedir;
    fuse_ll->fuse_handlers[FUSE_READDIR] = fuse_ll_readdir;
    fuse_ll->fuse_handlers[FUSE_READDIRPLUS] = fuse_ll_readdirplus;
    fuse_ll->fuse_handlers[FUSE_OPEN] = fuse_ll_open;
    fuse_ll->fuse_handlers[FUSE_RELEASE] = fuse_ll_release;
    //// We don't impl FUSE_FLUSH. The only use would be to return write errors on close()
    //// but that's of no use with a remote file system
    fuse_ll->fuse_handlers[FUSE_FSYNC] = fuse_ll_fsync;
    fuse_ll->fuse_handlers[FUSE_FSYNCDIR] = fuse_ll_fsyncdir;
    fuse_ll->fuse_handlers[FUSE_CREATE] = fuse_ll_create;
    fuse_ll->fuse_handlers[FUSE_RMDIR] = fuse_ll_rmdir;
    fuse_ll->fuse_handlers[FUSE_FORGET] = fuse_ll_forget;
    fuse_ll->fuse_handlers[FUSE_BATCH_FORGET] = fuse_ll_batch_forget;
    fuse_ll->fuse_handlers[FUSE_RENAME] = fuse_ll_rename;
    fuse_ll->fuse_handlers[FUSE_RENAME2] = fuse_ll_rename2;
    fuse_ll->fuse_handlers[FUSE_READ] = fuse_ll_read;
    fuse_ll->fuse_handlers[FUSE_WRITE] = fuse_ll_write;
    fuse_ll->fuse_handlers[FUSE_MKNOD] = fuse_ll_mknod;
    fuse_ll->fuse_handlers[FUSE_MKDIR] = fuse_ll_mkdir;
    fuse_ll->fuse_handlers[FUSE_SYMLINK] = fuse_ll_symlink;
    fuse_ll->fuse_handlers[FUSE_STATFS] = fuse_ll_statfs;
    fuse_ll->fuse_handlers[FUSE_UNLINK] = fuse_ll_unlink;
    fuse_ll->fuse_handlers[FUSE_READLINK] = fuse_ll_readlink;
    fuse_ll->fuse_handlers[FUSE_FLUSH] = fuse_ll_flush;
    fuse_ll->fuse_handlers[FUSE_SETLKW] = fuse_ll_setlkw;
    fuse_ll->fuse_handlers[FUSE_SETLK] = fuse_ll_setlk;
    fuse_ll->fuse_handlers[FUSE_FALLOCATE] = fuse_ll_fallocate;
}

static int fuse_unknown(struct fuse_ll *fuse_ll,
                        struct iovec *fuse_in_iov, int in_iovcnt,
                        struct iovec *fuse_out_iov, int out_iovcnt,
                        void *completion_context) {
    struct fuse_in_header *in_hdr = (struct fuse_in_header *) fuse_in_iov[0].iov_base;
    struct fuse_out_header *out_hdr = (struct fuse_out_header *) fuse_out_iov[0].iov_base;
    out_hdr->unique = in_hdr->unique;
    out_hdr->len = sizeof(struct fuse_out_header);
    out_hdr->error = -ENOSYS;

    printf("dpfs_fuse: fuse OP(%u) called, but not implemented\n", in_hdr->opcode);

    return 0;
}

static int fuse_handle_req(void *u,
                           struct iovec *in_iov, int in_iovcnt,
                           struct iovec *out_iov, int out_iovcnt,
                           void *completion_context)
{
    struct fuse_ll *fuse_ll = (struct fuse_ll *) u;

    if (in_iovcnt < 1 || in_iovcnt < 1) {
        fprintf(stderr, "%s: iovecs in and out don't both atleast one iovec\n", __func__);
        return -EINVAL;
    }

    struct fuse_in_header *in_hdr = (struct fuse_in_header *) in_iov[0].iov_base;

    if (in_hdr->opcode < 1 || in_hdr->opcode > DPFS_FUSE_MAX_OPCODE) {
        fprintf(stderr, "Invalid FUSE opcode!\n");
        return -EINVAL;
    } else {
        fuse_handler_t h = fuse_ll->fuse_handlers[in_hdr->opcode];
        if (h == NULL) {
            h = fuse_unknown;
        }
        int ret = h(fuse_ll, in_iov, in_iovcnt, out_iov, out_iovcnt, completion_context);
        
#ifdef DEBUG_ENABLED
        if (ret == 0 && out_iovcnt > 0 &&
            out_iov[0].iov_len >= sizeof(struct fuse_out_header))
        {
            struct fuse_out_header *out_hdr = (struct fuse_out_header *) out_iov[0].iov_base;
            if (out_hdr->error != 0)
                fprintf(stderr, "FUSE OP(%d) request ERROR=%d, %s\n", in_hdr->opcode,
                    out_hdr->error, strerror(-out_hdr->error));
        }
#endif
        return ret;
    }
}

int dpfs_fuse_main(struct fuse_ll_operations *ops, const char *hal_conf_path,
                   void *user_data, bool debug)
{
#ifdef DEBUG_ENABLED
    printf("dpfs_fuse is running in DEBUG mode\n");
#endif

    struct fuse_ll *f_ll = (struct fuse_ll *) calloc(1, sizeof(struct fuse_ll));
    f_ll->ops = *ops;
    f_ll->debug = debug;
    f_ll->user_data = user_data;

    f_ll->se = (struct fuse_session *) calloc(1, sizeof(struct fuse_session));
    if (f_ll->se == NULL) {
        err(1, "ERROR: Could not allocate memory for fuse_session");
    }
    f_ll->se->conn.max_write = UINT_MAX;
    f_ll->se->conn.max_readahead = UINT_MAX;

    f_ll->se->bufsize = FUSE_MAX_MAX_PAGES * getpagesize() +
        FUSE_BUFFER_HEADER_SIZE;

    fuse_ll_map(f_ll);

    struct dpfs_hal_params hal_params;
    memset(&hal_params, 0, sizeof(hal_params));
    hal_params.user_data = f_ll;
    hal_params.request_handler = fuse_handle_req;
    hal_params.conf_path = hal_conf_path;

    struct dpfs_hal *emu = dpfs_hal_new(&hal_params);
    if (emu == NULL) {
        fprintf(stderr, "Failed to initialize hal, exiting...\n");
        return -1;
    }
    dpfs_hal_loop(emu);
    dpfs_hal_destroy(emu);
    
    return 0;
}
