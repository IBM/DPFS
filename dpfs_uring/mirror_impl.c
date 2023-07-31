/*
#
# Copyright 2023- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Original libfuse clause: much of the mirroring code is based on passthrough_hp.cc

  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>
  Copyright (C) 2017       Nikolaus Rath <Nikolaus@rath.org>
  Copyright (C) 2018       Valve, Inc

  This program can be distributed under the terms of the GNU GPLv2.
  See the file COPYING.
#
*/

#define _LARGEFILE64_SOURCE
#define _GNU_SOURCE
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <err.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <linux/fuse.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/statvfs.h>
#include <sys/file.h>
#include <string.h>
#include "dpfs_fuse.h"

#include "fuser.h"
#include "mirror_impl.h"
#include "aio.h"

// After this macro, cb_data and thread_id will be defined
// and mirror functions that need custom data in the cb_data, can set them.
// Is this ugly? Yeah sure, but it does help reduce the redudancy and bug proneness of this all.
#define CB_DATA(cb_func) \
    size_t thread_id = dpfs_hal_thread_id(); \
    struct fuser_cb_data *cb_data = mpool_alloc(f->cb_data_pools[thread_id]); \
    cb_data->thread_id = thread_id; \
    cb_data->cb = cb_func; \
    cb_data->f = f; \
    cb_data->se = se; \
    cb_data->completion_context = completion_context; \
    cb_data->in_hdr = in_hdr; \
    cb_data->out_hdr = out_hdr; \
    do {} while (0)

static void fuser_mirror_generic_cb(struct fuser_cb_data *cb_data, struct io_uring_cqe *cqe)
{
    if (cqe->res < 0) {
        cb_data->out_hdr->error = cqe->res;
        dpfs_hal_async_complete(cb_data->completion_context, DPFS_HAL_COMPLETION_SUCCES);
        return;
    }

    dpfs_hal_async_complete(cb_data->completion_context, DPFS_HAL_COMPLETION_SUCCES);
}


static void forget_one(struct fuser *f, fuse_ino_t ino, uint64_t n)
{
    struct inode *i = ino_to_inodeptr(f, ino);
    pthread_mutex_lock(&i->m);

    if (n > i->nlookup) {
        fprintf(stderr, "INTERNAL ERROR: Negative lookup count for inode %ld\n", i->src_ino);
        exit(-1);
    }
    i->nlookup -= n;

    if (f->debug)
        printf("DEBUG: %s: inode %ld count %ld\n", __func__, i->src_ino, i->nlookup);

    if (!i->nlookup) {
        if (f->debug)
            printf("DEBUG: forget: cleaning up inode %ld\n", i->src_ino);

        pthread_mutex_lock(&f->m);
        pthread_mutex_unlock(&i->m);
        inode_table_erase(f->inodes, i->src_ino);
        pthread_mutex_unlock(&f->m);
    } else if (f->debug)
        printf("DEBUG: forget: inode %ld lookup count now %ld\n", i->src_ino, i->nlookup);

    if (i->nlookup)
        pthread_mutex_unlock(&i->m);
}

int fuser_mirror_init(struct fuse_session *se, void *user_data,
    struct fuse_in_header *in_hdr, struct fuse_init_in *in_init,
    struct fuse_conn_info *conn, struct fuse_out_header *out_hdr,
    uint16_t device_id)
{
    struct fuser *f = user_data;

    if (conn->capable & FUSE_CAP_EXPORT_SUPPORT)
        conn->want |= FUSE_CAP_EXPORT_SUPPORT;

    if (f->timeout && conn->capable & FUSE_CAP_WRITEBACK_CACHE)
        conn->want |= FUSE_CAP_WRITEBACK_CACHE;

    if (conn->capable & FUSE_CAP_FLOCK_LOCKS)
        conn->want |= FUSE_CAP_FLOCK_LOCKS;

    // FUSE_CAP_SPLICE_READ is enabled in libfuse3 by default,
    // see do_init() in in fuse_lowlevel.c
    // We do not want this as splicing is not a thing with virtiofs
    conn->want &= ~FUSE_CAP_SPLICE_READ;
    conn->want &= ~FUSE_CAP_SPLICE_WRITE;

    int ret;
    if (in_hdr->uid != 0 && in_hdr->gid != 0) {
        ret = seteuid(in_hdr->uid);
        if (ret == -1) {
            warn("%s: Could not set uid of fuser to %d", __func__, in_hdr->uid);
            goto ret_errno;
        }
        ret = setegid(in_hdr->gid);
        if (ret == -1) {
            warn("%s: Could not set gid of fuser to %d", __func__, in_hdr->gid);
            goto ret_errno;
        }
    } else {
        printf("%s, init was not supplied with a non-zero uid and gid. "
        "Thus all operations will go through the name of uid %d and gid %d\n", __func__, getuid(), getgid());
    }

    se->init_done = true;

    return 0;
ret_errno:
    if (ret == -1)
        out_hdr->error = -errno;
    return 0;
}

int fuser_mirror_destroy (struct fuse_session *se, void *user_data,
        struct fuse_in_header *in_hdr,
        struct fuse_out_header *out_hdr,
        void *completion_context, uint16_t device_id)
{
    struct fuser *f = user_data;
    inode_table_clear(f->inodes);

    return 0;
}

#ifdef IORING_STATX_SUPPORTED
static void fuser_mirror_getattr_cb(struct fuser_cb_data *cb_data, struct io_uring_cqe *cqe)
{
    if (cqe->res < 0) {
        cb_data->out_hdr->error = cqe->res;
    }
    fuse_ll_reply_attrx(cb_data->se, cb_data->out_hdr, cb_data->getattr.out_attr, &cb_data->getattr.s, cb_data->f->timeout);

    dpfs_hal_async_complete(cb_data->completion_context, DPFS_HAL_COMPLETION_SUCCES);
}
#endif

int fuser_mirror_getattr(struct fuse_session *se, void *user_data,
    struct fuse_in_header *in_hdr, struct fuse_getattr_in *in_getattr,
    struct fuse_out_header *out_hdr, struct fuse_attr_out *out_attr,
    void *completion_context, uint16_t device_id)
{
    struct fuser *f = user_data;

    int fd;
    if (in_getattr->getattr_flags & FUSE_GETATTR_FH) {
        fd = in_getattr->fh;
    } else {
        struct inode *i = ino_to_inodeptr(f, in_hdr->nodeid);
        fd = i->fd;
    }

#ifdef IORING_STATX_SUPPORTED
    CB_DATA(fuser_mirror_getattr_cb);
    cb_data->getattr.out_attr = out_attr;

    struct io_uring_sqe *sqe = io_uring_get_sqe(&f->rings[thread_id]);
    if (!sqe) {
        fprintf(stderr, "ERROR: Not enough uring sqe elements avail.\n");
        out_hdr->error = -ENOMEM;
        return 0;
    }
    io_uring_prep_statx(sqe, fd, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT, STATX_BASIC_STATS, &cb_data->getattr.s);
    io_uring_sqe_set_data(sqe, cb_data);

    int res = io_uring_submit(&f->rings[thread_id]);
    if (res < 0) {
        out_hdr->error = res;
        return 0;
    }

    return EWOULDBLOCK; // We move async
#else
    struct stat s;
    int res = fstatat(fd, "", &s,
                       AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW);
    if (res == -1) {
        out_hdr->error = -errno;
        return 0;
    }
    
    return fuse_ll_reply_attr(se, out_hdr, out_attr, &s, f->timeout);
#endif
}

static int do_lookup(struct fuser *f, fuse_ino_t parent, const char *name,
                     struct fuse_entry_param *e) {
    if (f->debug)
        printf("DEBUG: lookup(): name=%s, parent=%ld\n", name, parent);
    memset(e, 0, sizeof(*e));
    e->attr_timeout = f->timeout;
    e->entry_timeout = f->timeout;

    int newfd = openat(ino_to_fd(f, parent), name, O_PATH | O_NOFOLLOW);
    if (newfd == -1)
        return errno;

    int res = fstatat(newfd, "", &e->attr, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW);
    if (res == -1) {
        int saveerr = errno;
        close(newfd);
        if (f->debug)
            printf("DEBUG: lookup(): fstatat failed\n");
        return saveerr;
    }

    if (e->attr.st_dev != f->src_dev) {
        printf("WARNING: Mountpoints in the source directory tree will be hidden.\n");
        return ENOTSUP;
    } else if (e->attr.st_ino == FUSE_ROOT_ID) {
        printf("ERROR: Source directory tree must not include inode %d\n", FUSE_ROOT_ID);
        return EIO;
    }

    pthread_mutex_lock(&f->m);
    struct inode *i = inode_table_getsert(f->inodes, e->attr.st_ino);
    if (i == NULL) {
        return ENOMEM;
    }
    e->ino = (fuse_ino_t) i;
    e->generation = i->generation;

    // found unlinked inode, unlinking happens in the fuse unlink opcode, duhhh sherlock
    if (i->fd == -ENOENT) {
        if (f->debug)
            printf("DEBUG: lookup(): inode %ld recycled; generation=%d\n", e->attr.st_ino, i->generation);
    /* fallthrough to new inode but keep existing inode.nlookup */
    }

    if (i->fd > 0) { // found existing inode
        pthread_mutex_unlock(&f->m);
        if (f->debug)
            printf("DEBUG: lookup(): inode %ld (userspace) already known; fd = %d\n", e->attr.st_ino, i->fd);
        pthread_mutex_lock(&i->m);

        i->nlookup++;
        if (f->debug)
            printf("DEBUG:%s:%d inode %ld count %ld\n", __func__, __LINE__, i->src_ino, i->nlookup);

        close(newfd);
        pthread_mutex_unlock(&i->m);
    } else { // no existing inode
        /* This is just here to make Helgrind happy. It violates the
           lock ordering requirement (inode.m must be acquired before
           fs.mutex), but this is of no consequence because at this
           point no other thread has access to the inode mutex */
        pthread_mutex_lock(&i->m);
        i->src_ino = e->attr.st_ino;
        i->src_dev = e->attr.st_dev;

        i->nlookup++;
        if (f->debug)
            printf("DEBUG:%s:%d inode %ld count %ld\n", __func__, __LINE__, i->src_ino, i->nlookup);

        i->fd = newfd;
        pthread_mutex_unlock(&f->m);
        pthread_mutex_unlock(&i->m);

        if (f->debug)
            printf("DEBUG: lookup(): created userspace inode %ld; fd = %d\n", e->attr.st_ino, i->fd);
    }

    return 0;
}

int fuser_mirror_lookup(struct fuse_session *se, void *user_data,
                        struct fuse_in_header *in_hdr, const char *const in_name,
                        struct fuse_out_header *out_hdr, struct fuse_entry_out *out_entry,
                        void *completion_context, uint16_t device_id)
{
    struct fuser *f = user_data;

    struct fuse_entry_param e;
    int err = do_lookup(f, in_hdr->nodeid, in_name, &e);
    if (err == ENOENT) {
        e.attr_timeout = f->timeout;
        e.entry_timeout = f->timeout;
        e.ino = e.attr.st_ino = 0;
        return fuse_ll_reply_entry(se, out_hdr, out_entry, &e);
    } else if (err) {
        if (err == ENFILE || err == EMFILE)
            fprintf(stderr, "ERROR: Reached maximum number of file descriptors.\n");
        out_hdr->error = -err;
        return 0;
    } else {
        return fuse_ll_reply_entry(se, out_hdr, out_entry, &e);
    }
}

int fuser_mirror_setattr(struct fuse_session *se, void *user_data,
                         struct fuse_in_header *in_hdr, struct stat *s, int valid, struct fuse_file_info *fi,
                         struct fuse_out_header *out_hdr, struct fuse_attr_out *out_attr,
                         void *completion_context, uint16_t device_id)
{
    struct fuser *f = user_data;

    struct inode *i = ino_to_inodeptr(f, in_hdr->nodeid);
    int ifd = i->fd;
    int res;

    if (valid & FUSE_SET_ATTR_MODE) {
        if (fi) {
            res = fchmod(fi->fh, s->st_mode);
        } else {
            char procname[64];
            sprintf(procname, "/proc/self/fd/%i", ifd);
            res = chmod(procname, s->st_mode);
        }
        if (res == -1)
            goto out_err;
    }
    if (valid & (FUSE_SET_ATTR_UID | FUSE_SET_ATTR_GID)) {
        uid_t uid = (valid & FUSE_SET_ATTR_UID) ? s->st_uid : -1;
        gid_t gid = (valid & FUSE_SET_ATTR_GID) ? s->st_gid : -1;

        res = fchownat(ifd, "", uid, gid, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW);
        if (res == -1)
            goto out_err;
    }
    if (valid & FUSE_SET_ATTR_SIZE) {
        if (fi) {
            res = ftruncate(fi->fh, s->st_size);
        } else {
            char procname[64];
            sprintf(procname, "/proc/self/fd/%i", ifd);
            res = truncate(procname, s->st_size);
        }
        if (res == -1)
            goto out_err;
    }
    if (valid & (FUSE_SET_ATTR_ATIME | FUSE_SET_ATTR_MTIME)) {
        struct timespec tv[2];

        tv[0].tv_sec = 0;
        tv[1].tv_sec = 0;
        tv[0].tv_nsec = UTIME_OMIT;
        tv[1].tv_nsec = UTIME_OMIT;

        if (valid & FUSE_SET_ATTR_ATIME_NOW)
            tv[0].tv_nsec = UTIME_NOW;
        else if (valid & FUSE_SET_ATTR_ATIME)
            tv[0] = s->st_atim;

        if (valid & FUSE_SET_ATTR_MTIME_NOW)
            tv[1].tv_nsec = UTIME_NOW;
        else if (valid & FUSE_SET_ATTR_MTIME)
            tv[1] = s->st_mtim;

        if (fi)
            res = futimens(fi->fh, tv);
        else {
            char procname[64];
            sprintf(procname, "/proc/self/fd/%i", ifd);
            res = utimensat(AT_FDCWD, procname, tv, 0);
        }
        if (res == -1)
            goto out_err;
    }

    struct stat snew;
    res = fstatat(i->fd, "", &snew,
                       AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW);
    if (res == -1) goto out_err;
    
    return fuse_ll_reply_attr(se, out_hdr, out_attr, &snew, f->timeout);

out_err:
    out_hdr->error = -errno;
    return 0;
}


int fuser_mirror_opendir(struct fuse_session *se, void *user_data,
                    struct fuse_in_header *in_hdr, struct fuse_open_in *in_open,
                    struct fuse_out_header *out_hdr, struct fuse_open_out *out_open,
                    void *completion_context, uint16_t device_id)
{
    struct fuser *f = user_data;
    struct inode *i = ino_to_inodeptr(f, in_hdr->nodeid);
    if (!i) {
        out_hdr->error = -EINVAL;
        return 0;
    }
    struct directory *d = calloc(1, sizeof(struct directory));
    if (!d) {
        out_hdr->error = -errno;
        return 0;
    }

    // Make Helgrind happy - it can't know that there's an implicit
    // synchronization due to the fact that other threads cannot
    // access d until we've called fuse_reply_*.
    //pthread_mutex_lock(&i->m);

    int fd = openat(i->fd, ".", O_RDONLY);
    if (fd == -1)
        goto out_errno;

    // On success, dir stream takes ownership of fd, so we
    // do not have to close it.
    d->dp = fdopendir(fd);
    if(!d->dp)
        goto out_errno;

    struct fuse_file_info fi;
    memset(&fi, 0, sizeof(fi));
    fi.flags = in_open->flags; // from fuse_lowlevel.c
    fi.fh = (uint64_t) d;
    if(f->timeout) {
        fi.keep_cache = 1;
        fi.cache_readdir = 1;
    }

    return fuse_ll_reply_open(se, out_hdr, out_open, &fi);

out_errno:
    out_hdr->error = -errno;
    directory_destroy(d);
    if (errno == ENFILE || errno == EMFILE)
        fprintf(stderr, "ERROR: Reached maximum number of file descriptors.");
    return 0;
}


int fuser_mirror_releasedir(struct fuse_session *se, void *user_data,
                    struct fuse_in_header *in_hdr, struct fuse_release_in *in_release,
                    struct fuse_out_header *out_hdr,
                    void *completion_context, uint16_t device_id)
{
    struct directory *d = (struct directory *) in_release->fh;
    directory_destroy(d);
    return 0;
}

static bool is_dot_or_dotdot(const char *name) {
    return name[0] == '.' &&
           (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}

int fuser_mirror_readdir(struct fuse_session *se, void *user_data,
                       struct fuse_in_header *in_hdr, struct fuse_read_in *in_read, bool plus,
                       struct fuse_out_header *out_hdr, struct iov read_iov,
                       void *completion_context, uint16_t device_id)
{
    struct fuser *f = user_data;

    const off_t off = in_read->offset;
    struct directory *d = (struct directory *) in_read->fh;
    struct inode *i = ino_to_inodeptr(f, in_hdr->nodeid);
    pthread_mutex_lock(&i->m);

    uint32_t rem = in_read->size; // remaining bytes to read from dir (user requested)
    int err = 0, count = 0; // count = dirents read

    if (f->debug)
        printf("DEBUG: readdir(): started with offset %ld\n", off);

    if (off != d->offset) {
        if (f->debug)
            printf("DEBUG: readdir(): seeking to %ld\n", off);
        seekdir(d->dp, off);
        d->offset = off;
    }

    while (1) {
        errno = 0;
        struct dirent *entry = readdir(d->dp);
        if (!entry) {
            if(errno) {
                err = errno;
                if (f->debug)
                    warn("DEBUG: readdir(): readdir failed with");
                goto error;
            }
            break; // End of stream
        }
        d->offset = entry->d_off;
        if (is_dot_or_dotdot(entry->d_name))
            continue;

        struct fuse_entry_param e;
        size_t written;
        if(plus) {
            err = do_lookup(f, in_hdr->nodeid, entry->d_name, &e);
            if (err)
                goto error;
            written = fuse_add_direntry_plus(&read_iov, entry->d_name, &e, entry->d_off);

            if (written == 0) {
                if (f->debug)
                    printf("DEBUG: readdir(): buffer full, returning data.\n");
                forget_one(f, e.ino, 1);
                break;
            }
        } else {
            e.attr.st_ino = entry->d_ino;
            e.attr.st_mode = entry->d_type << 12;
            written = fuse_add_direntry(&read_iov, entry->d_name, &e.attr, entry->d_off);

            if (written == 0) {
                if (f->debug)
                    printf("DEBUG: readdir(): buffer full, returning data.\n");
                break;
            }
        }

        rem -= written;
        count++;
        if (f->debug)
            printf("DEBUG: readdir(): added to buffer: %s, ino %ld, offset %ld\n",
                entry->d_name, e.attr.st_ino, entry->d_off);
    }
    err = 0;
error:

    pthread_mutex_unlock(&i->m);
    // If there's an error, we can only signal it if we haven't stored
    // any entries yet - otherwise we'd end up with wrong lookup
    // counts for the entries that are already in the buffer. So we
    // return what we've collected until that point.
    if (err && rem == in_read->size) {
        if (err == ENFILE || err == EMFILE)
            fprintf(stderr, "%s: ERROR: Reached maximum number of file descriptors.\n", __func__);
        out_hdr->error = -err;
        return 0;
    } else {
        if (f->debug)
            printf("DEBUG: readdir(): returning %d entries, curr offset %ld\n", count, d->offset);
        out_hdr->len += in_read->size - rem;
        return 0;
    }
}

#ifdef IORING_OPENAT_SUPPORTED
void fuser_mirror_open_cb(struct fuser_cb_data *cb_data, struct io_uring_cqe *cqe)
{
    if (cqe->res < 0) {
        cb_data->out_hdr->error = cqe->res;
        if (cqe->res == -ENFILE || cqe->res == -EMFILE)
            fprintf(stderr, "ERROR: Reached maximum number of file descriptors.");
        dpfs_hal_async_complete(cb_data->completion_context, DPFS_HAL_COMPLETION_SUCCES);
        return;
    }

    pthread_mutex_lock(&cb_data->open.i->m);
    cb_data->open.i->nopen++;
    pthread_mutex_unlock(&cb_data->open.i->m);
    cb_data->open.fi.keep_cache = (cb_data->f->timeout != 0);
    cb_data->open.fi.noflush = (cb_data->f->timeout == 0 && (cb_data->open.fi.flags & O_ACCMODE) == O_RDONLY);
    cb_data->open.fi.fh = cqe->res;

    fuse_ll_reply_open(cb_data->se, cb_data->out_hdr, cb_data->open.out_open, &cb_data->open.fi);
    dpfs_hal_async_complete(cb_data->completion_context, DPFS_HAL_COMPLETION_SUCCES);
}
#endif

int fuser_mirror_open(struct fuse_session *se, void *user_data,
                      struct fuse_in_header *in_hdr, struct fuse_open_in *in_open,
                      struct fuse_out_header *out_hdr, struct fuse_open_out *out_open,
                      void *completion_context, uint16_t device_id)
{
    struct fuser *f = user_data;

    struct fuse_file_info fi;
    fi.flags = in_open->flags;

    // See the conf.toml
    if (f->reject_directio)
        fi.flags &= ~O_DIRECT;

    struct inode *i = ino_to_inodeptr(f, in_hdr->nodeid);
    if (!i) {
        out_hdr->error = -EINVAL;
        return 0;
    }

    /* With writeback cache, kernel may send read requests even
       when userspace opened write-only */
    if (f->timeout && (fi.flags & O_ACCMODE) == O_WRONLY) {
        fi.flags &= ~O_ACCMODE;
        fi.flags |= O_RDWR;
    }

    /* With writeback cache, O_APPEND is handled by the kernel.  This
       breaks atomicity (since the file may change in the underlying
       filesystem, so that the kernel's idea of the end of the file
       isn't accurate anymore). However, no process should modify the
       file in the underlying filesystem once it has been read, so
       this is not a problem. */
    if (f->timeout && fi.flags & O_APPEND)
        fi.flags &= ~O_APPEND;

    /* Unfortunately we cannot use inode.fd, because this was opened
       with O_PATH (so it doesn't allow read/write access). */
    char buf[64];
    sprintf(buf, "/proc/self/fd/%i", i->fd);

#ifdef IORING_OPENAT_SUPPORTED
    CB_DATA(fuser_mirror_open_cb);
    cb_data->open.i = i;
    cb_data->open.fi = fi;
    cb_data->open.out_open = out_open;

    struct io_uring_sqe *sqe = io_uring_get_sqe(&f->rings[thread_id]);
    if (!sqe) {
        fprintf(stderr, "ERROR: Not enough uring sqe elements avail.\n");
        out_hdr->error = -ENOMEM;
        return 0;
    }
    io_uring_prep_openat(sqe, -1, buf, fi.flags & ~O_NOFOLLOW, -1);
    io_uring_sqe_set_data(sqe, cb_data);

    int res = io_uring_submit(&f->rings[thread_id]);
    if (res < 0) {
        out_hdr->error = res;
        return 0;
    }

    return EWOULDBLOCK;
#else
    int fd = open(buf, fi.flags & ~O_NOFOLLOW);
    if (fd == -1) {
        int err = errno;
        if (err == ENFILE || err == EMFILE)
            fprintf(stderr, "ERROR: Reached maximum number of file descriptors.");
        out_hdr->error = -err;
        return 0;
    }

    pthread_mutex_lock(&i->m);
    i->nopen++;
    pthread_mutex_unlock(&i->m);
    fi.keep_cache = (f->timeout != 0);
    fi.noflush = (f->timeout == 0 && (fi.flags & O_ACCMODE) == O_RDONLY);
    fi.fh = fd;

    return fuse_ll_reply_open(se, out_hdr, out_open, &fi);
#endif
}

int fuser_mirror_release(struct fuse_session *se, void *user_data,
                    struct fuse_in_header *in_hdr, struct fuse_release_in *in_release,
                    struct fuse_out_header *out_hdr,
                    void *completion_context, uint16_t device_id)
{
    struct fuser *f = user_data;

    struct inode *i = ino_to_inodeptr(f, in_hdr->nodeid);
    if (!i) {
        out_hdr->error = -EINVAL;
        return 0;
    }

    pthread_mutex_lock(&i->m);
    i->nopen--;
    pthread_mutex_unlock(&i->m);

#ifdef IORING_CLOSE_SUPPORTED
    CB_DATA(fuser_mirror_generic_cb);

    struct io_uring_sqe *sqe = io_uring_get_sqe(&f->rings[thread_id]);
    if (!sqe) {
        fprintf(stderr, "ERROR: Not enough uring sqe elements avail.\n");
        out_hdr->error = -ENOMEM;
        return 0;
    }
    io_uring_prep_close(sqe, in_release->fh);
    io_uring_sqe_set_data(sqe, cb_data);

    int res = io_uring_submit(&f->rings[thread_id]);
    if (res < 0) {
        out_hdr->error = res;
        return 0;
    }

    return EWOULDBLOCK; // We move async
#else
    close(in_release->fh);

    return 0;
#endif
}

static int do_fsync(struct fuse_session *se, void *user_data,
        struct fuse_in_header *in_hdr, int fd, unsigned fuse_flags,
        struct fuse_out_header *out_hdr,
        void *completion_context)
{
    struct fuser *f = user_data;

    CB_DATA(fuser_mirror_generic_cb);

    struct io_uring_sqe *sqe = io_uring_get_sqe(&f->rings[thread_id]);
    if (!sqe) {
        fprintf(stderr, "ERROR: Not enough uring sqe elements avail.\n");
        out_hdr->error = -ENOMEM;
        return 0;
    }
    unsigned flags = 0;
    // Currently these flags have the same values, compiler will figure it out for us
    if (fuse_flags & FUSE_FSYNC_FDATASYNC)
        flags |=  IORING_FSYNC_DATASYNC;
    io_uring_prep_fsync(sqe, fd, flags);
    io_uring_sqe_set_data(sqe, cb_data);

    int res = io_uring_submit(&f->rings[thread_id]);
    if (res < 0) {
        out_hdr->error = res;
        return 0;
    }

    return EWOULDBLOCK; // We move async
}

int fuser_mirror_fsync(struct fuse_session *se, void *user_data,
        struct fuse_in_header *in_hdr, struct fuse_fsync_in *in_fsync,
        struct fuse_out_header *out_hdr,
        void *completion_context, uint16_t device_id)
{
    return do_fsync(se, user_data, in_hdr, in_fsync->fh, in_fsync->fsync_flags, out_hdr, completion_context);
}

int fuser_mirror_fsyncdir(struct fuse_session *se, void *user_data,
        struct fuse_in_header *in_hdr, struct fuse_fsync_in *in_fsync,
        struct fuse_out_header *out_hdr,
        void *completion_context, uint16_t device_id)
{
    struct directory *d = (struct directory *) in_fsync->fh;
    int fd = dirfd(d->dp);

    return do_fsync(se, user_data, in_hdr, fd, in_fsync->fsync_flags, out_hdr, completion_context);
}

#ifdef IORING_OPENAT_SUPPORTED

void fuser_mirror_create_cb(struct fuser_cb_data *cb_data, struct io_uring_cqe *cqe)
{
    if (cqe->res < 0) {
        cb_data->out_hdr->error = cqe->res;
        if (cqe->res == -ENFILE || cqe->res == -EMFILE)
            fprintf(stderr, "ERROR: Reached maximum number of file descriptors.");
        dpfs_hal_async_complete(cb_data->completion_context, DPFS_HAL_COMPLETION_SUCCES);
        return;
    }

    cb_data->create.fi.fh = cqe->res;
    struct fuse_entry_param e;
    int err = do_lookup(cb_data->f, cb_data->in_hdr->nodeid, cb_data->create.in_name, &e);
    if (err) {
        if (err == ENFILE || err == EMFILE)
            fprintf(stderr, "ERROR: Reached maximum number of file descriptors.");
        cb_data->out_hdr->error = -err;
        dpfs_hal_async_complete(cb_data->completion_context, DPFS_HAL_COMPLETION_SUCCES);
        return;
    }

    struct inode *i = ino_to_inodeptr(cb_data->f, e.ino);
    pthread_mutex_lock(&i->m);
    i->nopen++;
    pthread_mutex_unlock(&i->m);

    fuse_ll_reply_create(cb_data->se, cb_data->out_hdr, cb_data->create.out_entry,
            cb_data->create.out_open, &e, &cb_data->create.fi);
    dpfs_hal_async_complete(cb_data->completion_context, DPFS_HAL_COMPLETION_SUCCES);
}

int fuser_mirror_create(struct fuse_session *se, void *user_data,
    struct fuse_in_header *in_hdr, struct fuse_create_in in_create, const char *const in_name,
    struct fuse_out_header *out_hdr, struct fuse_entry_out *out_entry, struct fuse_open_out *out_open,
    void *completion_context, uint16_t device_id)
{
    struct fuser *f = user_data;

    CB_DATA(fuser_mirror_generic_cb);

    memset(&cb_data->create.fi, 0, sizeof(cb_data->create.fi));
    cb_data->create.fi.flags = in_create.flags; // from fuse_lowlevel.c
    if (f->reject_directio)
        cb_data->create.fi.flags &= ~O_DIRECT;

    struct inode *ip = ino_to_inodeptr(f, in_hdr->nodeid);
    if (!ip) {
        out_hdr->error = -EINVAL;
        return 0;
    }

    cb_data->create.out_entry = out_entry;
    cb_data->create.out_open = out_open;
    cb_data->create.in_name = in_name;

    struct io_uring_sqe *sqe = io_uring_get_sqe(&f->rings[thread_id]);
    if (!sqe) {
        fprintf(stderr, "ERROR: Not enough uring sqe elements avail.\n");
        out_hdr->error = -ENOMEM;
        return 0;
    }
    io_uring_prep_openat(sqe, ip->fd, in_name,
                     (cb_data->create.fi.flags | O_CREAT) & ~O_NOFOLLOW, in_create.mode);
    io_uring_sqe_set_data(sqe, cb_data);

    int res = io_uring_submit(&f->rings[thread_id]);
    if (res < 0) {
        out_hdr->error = res;
        return 0;
    }

    return EWOULDBLOCK; // We move async
}

#else

int fuser_mirror_create(struct fuse_session *se, void *user_data,
    struct fuse_in_header *in_hdr, struct fuse_create_in in_create, const char *const in_name,
    struct fuse_out_header *out_hdr, struct fuse_entry_out *out_entry, struct fuse_open_out *out_open,
    void *completion_context, uint16_t device_id)
{
    struct fuser *f = user_data;

    struct fuse_file_info fi;
    memset(&fi, 0, sizeof(fi));
    fi.flags = in_create.flags; // from fuse_lowlevel.c
    struct inode *ip = ino_to_inodeptr(f, in_hdr->nodeid);
    if (!ip) {
        out_hdr->error = -EINVAL;
        return 0;
    }

    int fd = openat(ip->fd, in_name,
                     (fi.flags | O_CREAT) & ~O_NOFOLLOW, in_create.mode);
    if (fd == -1) {
        int err = errno;
        if (err == ENFILE || err == EMFILE)
            fprintf(stderr, "ERROR: Reached maximum number of file descriptors.");
        out_hdr->error = -err;
        return 0;
    }

    fi.fh = fd;
    struct fuse_entry_param e;
    int err = do_lookup(f, in_hdr->nodeid, in_name, &e);
    if (err) {
        if (err == ENFILE || err == EMFILE)
            fprintf(stderr, "ERROR: Reached maximum number of file descriptors.");
        out_hdr->error = -err;
        return 0;
    }

    struct inode *i = ino_to_inodeptr(f, e.ino);
    pthread_mutex_lock(&i->m);
    i->nopen++;
    pthread_mutex_unlock(&i->m);

    return fuse_ll_reply_create(se, out_hdr, out_entry, out_open, &e, &fi);
}

#endif

int fuser_mirror_rmdir(struct fuse_session *se, void *user_data,
                  struct fuse_in_header *in_hdr, const char *const in_name,
                  struct fuse_out_header *out_hdr,
                  void *completion_context, uint16_t device_id)
{
    struct fuser *f = user_data;

    struct inode *ip = ino_to_inodeptr(f, in_hdr->nodeid);
    if (!ip) {
        out_hdr->error = -EINVAL;
        return 0;
    }
    pthread_mutex_lock(&ip->m);
    int res = unlinkat(ip->fd, in_name, AT_REMOVEDIR);
    pthread_mutex_unlock(&ip->m);
    if (res == -1)
        out_hdr->error = -errno;
    return 0;
}



int fuser_mirror_forget(struct fuse_session *se, void *user_data,
                  struct fuse_in_header *in_hdr, struct fuse_forget_in *in_forget,
                  void *completion_context, uint16_t device_id)
{
    struct fuser *f = user_data;

    forget_one(f, in_hdr->nodeid, in_forget->nlookup);
    return 0;
}

int fuser_mirror_batch_forget(struct fuse_session *se, void *user_data,
                         struct fuse_in_header *in_hdr, struct fuse_batch_forget_in *in_batch_forget,
                         struct fuse_forget_one *in_forget_one,
                         void *completion_context, uint16_t device_id)
{
    struct fuser *f = user_data;

    for (int i = 0; i < in_batch_forget->count; i++) {
        forget_one(f, in_forget_one[i].nodeid, in_forget_one[i].nlookup);
    }
    return 0;
}


int fuser_mirror_rename(struct fuse_session *se, void *user_data,
                   struct fuse_in_header *in_hdr, const char *const in_name,
                   fuse_ino_t in_new_parentdir, const char *const in_new_name, uint32_t in_flags,
                   struct fuse_out_header *out_hdr,
                   void *completion_context, uint16_t device_id)
{
    struct fuser *f = user_data;

    struct inode *ip = ino_to_inodeptr(f, in_hdr->nodeid);
    struct inode *new_ip = ino_to_inodeptr(f, in_new_parentdir);

    if (!ip || !new_ip) {
        out_hdr->error = -EINVAL;
        return 0;
    }

#ifdef IORING_RENAMEAT_SUPPORTED
    CB_DATA(fuser_mirror_generic_cb);

    struct io_uring_sqe *sqe = io_uring_get_sqe(&f->rings[thread_id]);
    if (!sqe) {
        fprintf(stderr, "ERROR: Not enough uring sqe elements avail.\n");
        out_hdr->error = -ENOMEM;
        return 0;
    }
    io_uring_prep_renameat(sqe, ip->fd, in_name, new_ip->fd, in_new_name, 0);
    io_uring_sqe_set_data(sqe, cb_data);

    int res = io_uring_submit(&f->rings[thread_id]);
    if (res < 0) {
        out_hdr->error = res;
        return 0;
    }

    return EWOULDBLOCK; // We move async
#else
    int res = renameat(ip->fd, in_name, new_ip->fd, in_new_name);
    if (res == -1)
        out_hdr->error = -errno;

    return 0;
#endif
}

void fuser_mirror_read_cb(struct fuser_cb_data *cb_data, struct io_uring_cqe *cqe)
{
    if (cqe->res < 0) {
        cb_data->out_hdr->error = cqe->res;
        dpfs_hal_async_complete(cb_data->completion_context, DPFS_HAL_COMPLETION_SUCCES);
        return;
    }

    // Cannot use generic because of this
    cb_data->out_hdr->len += cqe->res;
    dpfs_hal_async_complete(cb_data->completion_context, DPFS_HAL_COMPLETION_SUCCES);
}

int fuser_mirror_read(struct fuse_session *se, void *user_data,
        struct fuse_in_header *in_hdr, struct fuse_read_in *in_read,
        struct fuse_out_header *out_hdr, struct iovec *out_iov, int out_iovcnt,
        void *completion_context, uint16_t device_id)
{
    struct fuser *f = user_data;

    CB_DATA(fuser_mirror_read_cb);

    struct io_uring_sqe *sqe = io_uring_get_sqe(&f->rings[thread_id]);
    if (!sqe) {
        fprintf(stderr, "ERROR: Not enough uring sqe elements avail.\n");
        out_hdr->error = -ENOMEM;
        return 0;
    }
    io_uring_prep_readv(sqe, in_read->fh, out_iov, out_iovcnt, in_read->offset);
    io_uring_sqe_set_data(sqe, cb_data);
    // IOSQE_ASYNC doesn't work on file systems

    int res = io_uring_submit(&f->rings[thread_id]);
    if (res < 0) {
        out_hdr->error = res;
        return 0;
    }

    return EWOULDBLOCK; // We move async
}

void fuser_mirror_write_cb(struct fuser_cb_data *cb_data, struct io_uring_cqe *cqe)
{
    if (cqe->res < 0) {
        cb_data->out_hdr->error = cqe->res;
        dpfs_hal_async_complete(cb_data->completion_context, DPFS_HAL_COMPLETION_SUCCES);
        return;
    }

    cb_data->write.out_write->size = cqe->res;
    cb_data->out_hdr->len += sizeof(*cb_data->write.out_write);
    dpfs_hal_async_complete(cb_data->completion_context, DPFS_HAL_COMPLETION_SUCCES);
}

int fuser_mirror_write(struct fuse_session *se, void *user_data,
        struct fuse_in_header *in_hdr, struct fuse_write_in *in_write,
        struct iovec *in_iov, int in_iovcnt,
        struct fuse_out_header *out_hdr, struct fuse_write_out *out_write,
        void *completion_context, uint16_t device_id)
{
    struct fuser *f = user_data;

    CB_DATA(fuser_mirror_write_cb);
    cb_data->write.out_write = out_write;

    struct io_uring_sqe *sqe = io_uring_get_sqe(&f->rings[thread_id]);
    if (!sqe) {
        fprintf(stderr, "ERROR: Not enough uring sqe elements avail.\n");
        out_hdr->error = -ENOMEM;
        return 0;
    }
    io_uring_prep_writev(sqe, in_write->fh, in_iov, in_iovcnt, in_write->offset);
    io_uring_sqe_set_data(sqe, cb_data);
    // IOSQE_ASYNC doesn't work on file systems

    int res = io_uring_submit(&f->rings[thread_id]);
    if (res < 0) {
        out_hdr->error = res;
        return 0;
    }

    return EWOULDBLOCK; // We move async
}

static int make_something(struct fuser *f, fuse_ino_t parent,
                              const char *name, mode_t mode, dev_t rdev,
                              const char *link, struct fuse_entry_param *out_e) {
    int res;
    struct inode *ip = ino_to_inodeptr(f, parent);

    if (S_ISDIR(mode)) {
        res = mkdirat(ip->fd, name, mode);
    } else if (S_ISLNK(mode)) {
        if (!link)
            return EINVAL;
        res = symlinkat(link, ip->fd, name);
    } else {
        res = mknodat(ip->fd, name, mode, rdev);
    }
    int saverr = errno;
    if (res == -1)
        goto out_err;

    saverr = do_lookup(f, parent, name, out_e);
    if (saverr)
        goto out_err;

    return 0;

out_err:
    if (saverr == ENFILE || saverr == EMFILE)
        fprintf(stderr, "ERROR: Reached maximum number of file descriptors.\n");
    return saverr;
}

int fuser_mirror_mknod(struct fuse_session *se, void *user_data,
                  struct fuse_in_header *in_hdr, struct fuse_mknod_in * in_mknod, const char *const in_name,
                  struct fuse_out_header *out_hdr, struct fuse_entry_out *out_entry,
                  void *completion_context, uint16_t device_id)
{
    struct fuser *f = user_data;

    struct fuse_entry_param e;
    int res = make_something(f, in_hdr->nodeid, in_name, in_mknod->mode, in_mknod->rdev, NULL, &e);
    if (res) {
        out_hdr->error = -res;
        return 0;
    }

    return fuse_ll_reply_entry(se, out_hdr, out_entry, &e);
}

int fuser_mirror_mkdir(struct fuse_session *se, void *user_data,
                  struct fuse_in_header *in_hdr, struct fuse_mkdir_in *in_mkdir, const char *const in_name,
                  struct fuse_out_header *out_hdr, struct fuse_entry_out *out_entry,
                  void *completion_context, uint16_t device_id)
{
    struct fuser *f = user_data;

    struct fuse_entry_param e;
    int res = make_something(f, in_hdr->nodeid, in_name, S_IFDIR | in_mkdir->mode, 0, NULL, &e);
    if (res) {
        out_hdr->error = -res;
        return 0;
    }

    return fuse_ll_reply_entry(se, out_hdr, out_entry, &e);
}

int fuser_mirror_symlink(struct fuse_session *se, void *user_data,
                    struct fuse_in_header *in_hdr, const char *const in_name, const char *const in_link,
                    struct fuse_out_header *out_hdr, struct fuse_entry_out *out_entry,
                    void *completion_context, uint16_t device_id)
{
    struct fuser *f = user_data;

    struct fuse_entry_param e;
    int res = make_something(f, in_hdr->nodeid, in_name, S_IFLNK, 0, in_link, &e);
    if (res) {
        out_hdr->error = -res;
        return 0;
    }

    return fuse_ll_reply_entry(se, out_hdr, out_entry, &e);
}

int fuser_mirror_statfs(struct fuse_session *se, void *user_data,
                   struct fuse_in_header *in_hdr,
                   struct fuse_out_header *out_hdr, struct fuse_statfs_out *out_statfs,
                   void *completion_context, uint16_t device_id)
{
    struct fuser *f = user_data;

    struct statvfs stbuf;
    int res = fstatvfs(ino_to_fd(f, in_hdr->nodeid), &stbuf);
    if (res == -1) {
        out_hdr->error = -errno;
        return 0;
    }

    return fuse_ll_reply_statfs(se, out_hdr, out_statfs, &stbuf);
}

int fuser_mirror_unlink(struct fuse_session *se, void *user_data,
                  struct fuse_in_header *in_hdr, const char *const in_name,
                  struct fuse_out_header *out_hdr,
                  void *completion_context, uint16_t device_id)
{
    struct fuser *f = user_data;

    struct inode *ip = ino_to_inodeptr(f, in_hdr->nodeid);
    if (!ip) {
        out_hdr->error = -EINVAL;
        return 0;
    }
    // Release inode.fd before last unlink like nfsd EXPORT_OP_CLOSE_BEFORE_UNLINK
    // to test reused inode numbers.
    // Skip this when inode has an open file and when writeback cache is enabled.
    if (!f->timeout) {
        struct fuse_entry_param e;
        int err = do_lookup(f, in_hdr->nodeid, in_name, &e);
        if (err) {
            out_hdr->error = -err;
            return 0;
        }
        if (e.attr.st_nlink == 1) {
            struct inode *i = ino_to_inodeptr(f, e.ino);
            if (!i) {
                out_hdr->error = -EINVAL;
                return 0;
            }
            pthread_mutex_lock(&i->m);
            if (i->fd > 0 && !i->nopen) {
                if (f->debug)
                    fprintf(stderr, "DEBUG: unlink: release inode %ld; fd=%d\n", e.attr.st_ino, i->fd);
                pthread_mutex_lock(&f->m);
                close(i->fd);
                i->fd = -ENOENT;
                i->generation++;
                pthread_mutex_unlock(&f->m);
            }
            pthread_mutex_unlock(&i->m);
        }

        // decrease the ref which lookup above had increased
        forget_one(f, e.ino, 1);
    }
#ifdef IORING_UNLINKAT_SUPPORTED
    CB_DATA(fuser_mirror_generic_cb);

    struct io_uring_sqe *sqe = io_uring_get_sqe(&f->rings[thread_id]);
    if (!sqe) {
        fprintf(stderr, "ERROR: Not enough uring sqe elements avail.\n");
        out_hdr->error = -ENOMEM;
        return 0;
    }
    io_uring_prep_unlinkat(sqe, ip->fd, in_name, 0);
    io_uring_sqe_set_data(sqe, cb_data);

    int res = io_uring_submit(&f->rings[thread_id]);
    if (res < 0) {
        out_hdr->error = res;
        return 0;
    }

    return EWOULDBLOCK; // We move async
#else
    int res = unlinkat(ip->fd, in_name, 0);
    if (res == -1)
        out_hdr->error = -errno;
    return 0;
#endif
}

int fuser_mirror_flush(struct fuse_session *se, void *user_data,
               struct fuse_in_header *in_hdr, struct fuse_file_info fi,
               struct fuse_out_header *out_hdr,
               void *completion_context, uint16_t device_id)
{
    return do_fsync(se, user_data, in_hdr, fi.fh, FUSE_FSYNC_FDATASYNC, out_hdr, completion_context);
}

int fuser_mirror_flock(struct fuse_session *se, void *user_data,
                struct fuse_in_header *in_hdr, struct fuse_file_info fi, int op,
                struct fuse_out_header *out_hdr,
                void *completion_context, uint16_t device_id)
{
    (void) in_hdr;

    int res = flock(fi.fh, op);

    if (res == -1)
        out_hdr->error = -errno;
    return 0;
}


int fuser_mirror_fallocate(struct fuse_session *se, void *user_data,
                        struct fuse_in_header *in_hdr, struct fuse_fallocate_in *in_fallocate,
                      struct fuse_out_header *out_hdr,
                      void *completion_context, uint16_t device_id)
{
#ifdef IORING_FALLOCATE_SUPPORTED
    struct fuser *f = user_data;

    CB_DATA(fuser_mirror_generic_cb);

    struct io_uring_sqe *sqe = io_uring_get_sqe(&f->rings[thread_id]);
    if (!sqe) {
        fprintf(stderr, "ERROR: Not enough uring sqe elements avail.\n");
        out_hdr->error = -ENOMEM;
        return 0;
    }
    io_uring_prep_fallocate(sqe, in_fallocate->fh, in_fallocate->mode, in_fallocate->offset, in_fallocate->length);
    io_uring_sqe_set_data(sqe, cb_data);

    int res = io_uring_submit(&f->rings[thread_id]);
    if (res < 0) {
        out_hdr->error = res;
        return 0;
    }

    return EWOULDBLOCK; // We move async
#else
    int res = fallocate64(in_fallocate->fh, in_fallocate->mode, in_fallocate->offset, in_fallocate->length);

    if (res == -1)
        out_hdr->error = -errno;
    return 0;
#endif
}

void fuser_mirror_assign_ops(struct fuse_ll_operations *ops) {
    memset(ops, 0, sizeof(*ops));
    ops->init = fuser_mirror_init;
    ops->destroy = fuser_mirror_destroy;
    ops->getattr = fuser_mirror_getattr;
    ops->lookup = fuser_mirror_lookup;
    ops->setattr = fuser_mirror_setattr;
    ops->opendir = fuser_mirror_opendir;
    ops->releasedir = fuser_mirror_releasedir;
    ops->readdir = fuser_mirror_readdir;
    ops->open = fuser_mirror_open;
    ops->release = fuser_mirror_release;
    ops->fsync = fuser_mirror_fsync;
    ops->fsyncdir = fuser_mirror_fsyncdir;
    ops->create = fuser_mirror_create;
    ops->rmdir = fuser_mirror_rmdir;
    ops->forget = fuser_mirror_forget;
    ops->batch_forget = fuser_mirror_batch_forget;
    ops->rename = fuser_mirror_rename;
    ops->read = fuser_mirror_read;
    ops->write = fuser_mirror_write;
    ops->mknod = fuser_mirror_mknod;
    ops->mkdir = fuser_mirror_mkdir;
    ops->symlink = fuser_mirror_symlink;
    ops->statfs = fuser_mirror_statfs;
    ops->unlink = fuser_mirror_unlink;
    ops->flock = fuser_mirror_flock;
    ops->flush = fuser_mirror_flush;
    ops->fallocate = fuser_mirror_fallocate;
}

