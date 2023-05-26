/*
#
# Copyright 2023- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

#include <getopt.h>
#include <linux/fuse.h>
#include <sys/statvfs.h>
#include <errno.h>
#include <inttypes.h>

#include <memory>
#include <atomic>
#include <boost/lockfree/queue.hpp>

#include "RamCloud.h"
#include "ClientException.h"
#include "TableEnumerator.h"

#include "dpfs_fuse.h"
#include "dpfs/hal.h"

struct RamCloudUserData {
    RAMCloud::RamCloud *ramcloud;
    uint64_t dataTableId;
    uint64_t inodeTableId;
    volatile bool stopPoller;
};

constexpr size_t MAX_FILE_NAME = 128;

struct RamCloudInode {
    struct stat attr;
    char name[MAX_FILE_NAME];
};

constexpr uint64_t FNV_BASIS = 14695981039346656037ull;
constexpr uint64_t FNV_PRIME = 1099511628211ull;

/*
 * FNV-1a 64 bit hash of null terminated buffer (tail-recursion)
 * cf. https://en.wikipedia.org/wiki/Fowler%E2%80%93Noll%E2%80%93Vo_hash_function
 */
uint64_t fnv1a_hash(const char* str, uint64_t hash = FNV_BASIS)
{
    return *str ? fnv1a_hash(str + 1, (hash ^ *str) * FNV_PRIME) : hash;
}

class BufferHolder {
protected:
    RAMCloud::Buffer value;
};

class AsyncLookupOp : public BufferHolder, public RAMCloud::ReadRpc {
private:
    struct fuse_session *se;
    struct fuse_out_header *out_hdr;
    struct fuse_entry_out *out_entry;
    void *completion_context;
public:
    AsyncLookupOp(RamCloudUserData& userData, uint64_t inodeId,
                  struct fuse_session *se, struct fuse_out_header *out_hdr,
                  struct fuse_entry_out *out_entry, void *completion_context)
                  : ReadRpc{userData.ramcloud, userData.inodeTableId, &inodeId, sizeof(inodeId),
                            &value, NULL},
                    se{se}, out_hdr{out_hdr}, out_entry{out_entry},
                    completion_context{completion_context}
    {
        while (getState() == RETRY) {
            isReady();
        }
    }

    void completed() override
    {
        ReadRpc::completed();

        struct fuse_entry_param e;
        e.attr_timeout = 1;
        e.entry_timeout = 1;

        isReady();
        const RAMCloud::WireFormat::Read::Response* respHdr(getResponseHeader<RAMCloud::WireFormat::Read>());
        if (respHdr->common.status == RAMCloud::STATUS_OBJECT_DOESNT_EXIST) {
            // printf("Lookup: entry does not exist\n");
            e.ino = e.attr.st_ino = 0;
            fuse_ll_reply_entry(se, out_hdr, out_entry, &e);
        } else if (respHdr->common.status != RAMCloud::STATUS_OK) {
            fprintf(stderr, "[ERROR] Lookup: rpc failed with status = %d\n", respHdr->common.status);
            out_hdr->error = -EIO;
        } else if (value.size() - sizeof(*respHdr) != sizeof(RamCloudInode)) {
            fprintf(stderr, "[ERROR] Lookup: invalid size\n");
            out_hdr->error = -EIO;
        } else {
            RamCloudInode inode;
            value.copy(sizeof(*respHdr), sizeof(inode), &inode); //TODO: do we need to copy?
            struct fuse_entry_param e;
            e.ino = inode.attr.st_ino;
            e.attr = inode.attr;
            fuse_ll_reply_entry(se, out_hdr, out_entry, &e);
        }
        dpfs_hal_async_complete(completion_context, DPFS_HAL_COMPLETION_SUCCES);
        delete this;
    }
};

class AsyncGetAttrOp : public BufferHolder, public RAMCloud::ReadRpc {
private:
    struct fuse_session *se;
    struct fuse_out_header *out_hdr;
    struct fuse_attr_out *out_attr;
    void *completion_context;
public:
    AsyncGetAttrOp(RamCloudUserData& userData, uint64_t inodeId,
                   struct fuse_session *se, struct fuse_out_header *out_hdr,
                   struct fuse_attr_out *out_attr, void *completion_context)
                   : ReadRpc{userData.ramcloud, userData.inodeTableId, &inodeId, sizeof(inodeId),
                             &value, NULL},
                     se{se}, out_hdr{out_hdr}, out_attr{out_attr},
                     completion_context{completion_context}
    {
        while (getState() == RETRY) {
            isReady();
        }
    }

    void completed() override
    {
        ReadRpc::completed();

        struct stat attr;

        isReady();
        const RAMCloud::WireFormat::Read::Response* respHdr(getResponseHeader<RAMCloud::WireFormat::Read>());
        if (respHdr->common.status == RAMCloud::STATUS_OBJECT_DOESNT_EXIST) {
            printf("GetAttr: entry does not exist\n");
            attr.st_ino = 0; // is this correct? Or should we return -EIO?
            fuse_ll_reply_attr(se, out_hdr, out_attr, &attr, 1);
        } else if (respHdr->common.status != RAMCloud::STATUS_OK) {
            fprintf(stderr, "[ERROR] GetAttr: rpc failed with status = %d\n", respHdr->common.status);
            out_hdr->error = -EIO;
        } else if (value.size() - sizeof(*respHdr) != sizeof(RamCloudInode)) {
            fprintf(stderr, "[ERROR] GetAttr: invalid size\n");
            out_hdr->error = -EIO;
        } else {
            RamCloudInode inode;
            value.copy(sizeof(*respHdr), sizeof(inode), &inode); //TODO: do we need to copy?
            attr = inode.attr;
            fuse_ll_reply_attr(se, out_hdr, out_attr, &attr, 1);
        }
        dpfs_hal_async_complete(completion_context, DPFS_HAL_COMPLETION_SUCCES);
        delete this;
    }
};

class AsyncWriteOp;

class AsyncGetAttr : public BufferHolder, public RAMCloud::ReadRpc {
private:
    AsyncWriteOp& writeOp;
public:
    AsyncGetAttr(RamCloudUserData& userData, uint64_t inodeId, AsyncWriteOp& writeOp)
                   : ReadRpc{userData.ramcloud, userData.inodeTableId, &inodeId, sizeof(inodeId),
                             &value, NULL},
                     writeOp{writeOp}
    {
        while (getState() == RETRY) {
            isReady();
        }
    }

    void completed() override;
};

class IovecToBuf {
public:
    bool dealloc;
    char *buf;
    uint32_t size;

    IovecToBuf(struct iovec *iov, int iovcnt)
    {
        if (iovcnt == 1) {
            buf = reinterpret_cast<char *>(iov->iov_base);
            size = (uint32_t)iov->iov_len;
            dealloc = false;
        } else {
            size = 0;
            for (int i = 0; i < iovcnt; i++) {
                size += (uint32_t)iov[i].iov_len;
            }
            buf = new char[size];
            size_t offset = 0;
            for (int i = 0; i < iovcnt; i++) {
                memcpy(buf + offset, iov[i].iov_base, iov[i].iov_len);
            }
            dealloc = true;
        }
    }

    ~IovecToBuf()
    {
        if (dealloc) {
            delete buf;
        }
    }
};

class AsyncWriteOp : public IovecToBuf, public RAMCloud::WriteRpc {
private:
    struct fuse_session *se;
    struct fuse_out_header *out_hdr;
    struct fuse_write_out *out_write;
    void *completion_context;
    bool firstDone;
    RamCloudUserData& userData;
    AsyncGetAttr asyncGetAttr;
public:
    AsyncWriteOp(RamCloudUserData& userData, uint64_t inodeId, struct iovec* in_iov,
                 int iovcnt, struct fuse_session *se, struct fuse_out_header *out_hdr,
                 struct fuse_write_out *out_write, void *completion_context)
                 : IovecToBuf{in_iov, iovcnt},
                   WriteRpc{userData.ramcloud, userData.dataTableId, &inodeId, sizeof(inodeId),
                            buf, size},
                   se{se}, out_hdr{out_hdr}, out_write{out_write},
                   completion_context{completion_context}, firstDone{false}, userData{userData},
                   asyncGetAttr{userData, inodeId, *this}
    {
        while (getState() == RETRY) {
            isReady();
        }
    }

    void finish()
    {
        if (out_hdr->error == 0) {
            try {
                if (inode.attr.st_size != size) {
                    /* update size */
                    inode.attr.st_size = size;
                    userData.ramcloud->write(userData.inodeTableId, &inode.attr.st_ino,
                                             sizeof(inode.attr.st_ino), &inode, sizeof(inode));
                }
                out_write->size = size;
                out_hdr->len += sizeof(*out_write);
            } catch (RAMCloud::ClientException& e) {
                fprintf(stderr, "[ERROR] setattr write: %d\n", e.status);
                out_hdr->error = -EIO;
            }
        }
        dpfs_hal_async_complete(completion_context, DPFS_HAL_COMPLETION_SUCCES);
        delete this;
    }

    void completed() override
    {
        WriteRpc::completed();

        isReady();
        const RAMCloud::WireFormat::Write::Response* respHdr(getResponseHeader<RAMCloud::WireFormat::Write>());
        if (respHdr->common.status != RAMCloud::STATUS_OK) {
            fprintf(stderr, "[ERROR] Write: rpc failed with status = %d\n", respHdr->common.status);
            done(-EIO);
        } else {
            done(0);
        }
    }

    void done(int error)
    {
        if (error != 0) {
            out_hdr->error = error;
        }

        if (firstDone) {
            finish();
        } else {
            firstDone = true;
        }
    }

    RamCloudInode inode;
};

inline void AsyncGetAttr::completed()
{
    ReadRpc::completed();

    isReady();
    const RAMCloud::WireFormat::Read::Response* respHdr(getResponseHeader<RAMCloud::WireFormat::Read>());
    if (respHdr->common.status == RAMCloud::STATUS_OBJECT_DOESNT_EXIST) {
        fprintf(stderr, "[ERROR] Write getattr: entry does not exist\n");
        writeOp.done(-EIO);
    } else if (respHdr->common.status != RAMCloud::STATUS_OK) {
        fprintf(stderr, "[ERROR] Write getattr: rpc failed with status = %d\n", respHdr->common.status);
        writeOp.done(-EIO);
    } else if (value.size() - sizeof(*respHdr) != sizeof(RamCloudInode)) {
        fprintf(stderr, "[ERROR] Write getattr: invalid size\n");
        writeOp.done(-EIO);
    } else {
        value.copy(sizeof(*respHdr), sizeof(writeOp.inode), &writeOp.inode);
        writeOp.done(0);
    }
}

class AsyncReadOp : public BufferHolder, public RAMCloud::ReadRpc {
private:
    uint64_t offset;
    struct iovec *in_iov;
    int iovcnt;
    struct fuse_session *se;
    struct fuse_out_header *out_hdr;
    void *completion_context;
public:
    AsyncReadOp(RamCloudUserData& userData, uint64_t inodeId, uint64_t offset,
                struct iovec *in_iov, int iovcnt, struct fuse_session *se,
                struct fuse_out_header *out_hdr, void *completion_context) :
                ReadRpc{userData.ramcloud, userData.dataTableId, &inodeId, sizeof(inodeId),
                        &value, NULL},
                offset{offset}, in_iov{in_iov}, iovcnt{iovcnt}, se{se}, out_hdr{out_hdr},
                completion_context{completion_context}
    {
        while (getState() == RETRY) {
            isReady();
        }
    }

    void completed() override
    {
        ReadRpc::completed();

        isReady();
        const RAMCloud::WireFormat::Read::Response* respHdr(getResponseHeader<RAMCloud::WireFormat::Read>());
        if (respHdr->common.status == RAMCloud::STATUS_OBJECT_DOESNT_EXIST) {
            printf("Read: entry does not exist\n");
            out_hdr->error = -ENOENT;
        } else if (respHdr->common.status != RAMCloud::STATUS_OK) {
            fprintf(stderr, "[ERROR] Read: rpc failed with status = %d\n", respHdr->common.status);
            out_hdr->error = -EIO;
        } else {
            uint32_t read = 0;
            for (int i = 0; i < iovcnt; i++) {
                uint32_t len = value.copy(sizeof(*respHdr) + offset + read,
                                          (uint32_t)in_iov[i].iov_len, in_iov[i].iov_base);
                if (len == 0) {
                    // fprintf(stderr, "Read: EOF\n");
                    out_hdr->error = read == 0 ? -EOF : 0;
                    break;
                }
                read += len;
            }
            out_hdr->len += read;
        }
        dpfs_hal_async_complete(completion_context, DPFS_HAL_COMPLETION_SUCCES);
        delete this;
    }
};

static int fuse_init(struct fuse_session *se, RamCloudUserData *userData,
                     struct fuse_in_header *in_hdr, struct fuse_init_in *in_init,
                     struct fuse_conn_info *conn, struct fuse_out_header *out_hdr, uint16_t device_id)
{

    printf("FUSE init\n");

    se->init_done = true;

    /* We do not do anything here for now */

    return 0;
}

static int fuse_lookup(struct fuse_session *se, RamCloudUserData *userData,
                       struct fuse_in_header *in_hdr, const char *const in_name,
                       struct fuse_out_header *out_hdr, struct fuse_entry_out *out_entry,
                       void *completion_context, uint16_t device_id)
{
    new AsyncLookupOp{*userData, fnv1a_hash(in_name), se, out_hdr, out_entry, completion_context};

    return EWOULDBLOCK;
}

static int fuse_getattr(struct fuse_session *se, RamCloudUserData *userData,
                        struct fuse_in_header *in_hdr, struct fuse_getattr_in *in_getattr,
                        struct fuse_out_header *out_hdr, struct fuse_attr_out *out_attr,
                        void *completion_context, uint16_t device_id)
{
    struct stat st;

    /* root directory */
    if (in_hdr->nodeid == 1) {
        st.st_dev = 0;         /* ID of device containing file */
        st.st_ino = 1;         /* Inode number */
        st.st_mode = S_IFDIR;  /* File type and mode */
        st.st_nlink = 0;       /* Number of hard links */
        st.st_uid = 0;         /* User ID of owner */
        st.st_gid = 0;         /* Group ID of owner */
        st.st_rdev = 0;        /* Device ID (if special file) */
        st.st_size = 128;      /* Total size, in bytes */
        st.st_blksize = 1;     /* Block size for filesystem I/O */
        st.st_blocks = 1;      /* Number of 512B blocks allocated */

        // st.st_atim;  /* Time of last access */
        // st.st_mtim;  /* Time of last modification */
        // st.st_ctim;  /* Time of last status change */
        return fuse_ll_reply_attr(se, out_hdr, out_attr, &st, 1);
    } else {
        new AsyncGetAttrOp{*userData, in_hdr->nodeid, se, out_hdr, out_attr, completion_context};
        return EWOULDBLOCK;
    }
}

static int fuse_statfs(struct fuse_session *se, RamCloudUserData *userData,
                       struct fuse_in_header *in_hdr,
                       struct fuse_out_header *out_hdr, struct fuse_statfs_out *out_statfs,
                       void *completion_context, uint16_t device_id)
{
    struct statvfs stbuf;
    stbuf.f_bsize = 1;    /* Filesystem block size */
    stbuf.f_frsize = 4096;   /* Fragment size */
    stbuf.f_blocks = 1024*1024;   /* Size of fs in f_frsize units */
    stbuf.f_bfree = 1024;    /* Number of free blocks */
    stbuf.f_bavail = 1024;   /* Number of free blocks for unprivileged users */
    stbuf.f_files = 1024;    /* Number of inodes */
    stbuf.f_ffree = 1024;    /* Number of free inodes */
    stbuf.f_favail = 1024;   /* Number of free inodes for unprivileged users */
    stbuf.f_fsid = 1;     /* Filesystem ID */
    stbuf.f_flag = 0;     /* Mount flags */
    stbuf.f_namemax = MAX_FILE_NAME;  /* Maximum filename length */

    return fuse_ll_reply_statfs(se, out_hdr, out_statfs, &stbuf);
}

static int fuse_setattr(struct fuse_session *se, RamCloudUserData *userData,
                        struct fuse_in_header *in_hdr, struct stat *s, int valid, struct fuse_file_info *fi,
                        struct fuse_out_header *out_hdr, struct fuse_attr_out *out_attr,
                        void *completion_context, uint16_t device_id)
{
    RamCloudInode inode;
    try {
        RAMCloud::Buffer value;
        userData->ramcloud->read(userData->inodeTableId, &in_hdr->nodeid, sizeof(in_hdr->nodeid),
                            &value);
        value.copy(0, sizeof(inode), &inode);
    } catch (RAMCloud::ClientException& e) {
        fprintf(stderr, "[ERROR] setattr read: %d\n", e.status);
        if (e.status == RAMCloud::STATUS_OBJECT_DOESNT_EXIST) {
            out_hdr->error = -ENOENT;
        } else {
            out_hdr->error = -EIO;
        }
        return 0;
    }

    /* sync for now */
    if (valid & FUSE_SET_ATTR_MODE) {
        inode.attr.st_mode = s->st_mode;
    }

    if (valid & FUSE_SET_ATTR_UID) {
        inode.attr.st_uid = s->st_uid;
    }

    if (valid & FUSE_SET_ATTR_GID) {
        inode.attr.st_gid = s->st_gid;
    }

    if (valid & FUSE_SET_ATTR_SIZE) {
        inode.attr.st_size = s->st_size;
        /* Number of 512B blocks allocated */
        inode.attr.st_blocks =  s->st_size / 512 + (s->st_size % 512 == 0 ? 0 : 1);
    }

    try {
        userData->ramcloud->write(userData->inodeTableId, &in_hdr->nodeid, sizeof(in_hdr->nodeid),
                                 &inode, sizeof(inode));
    } catch (RAMCloud::ClientException& e) {
        fprintf(stderr, "[ERROR] setattr write: %d\n", e.status);
        out_hdr->error = -EIO;
        return 0;
    }

    return fuse_ll_reply_attr(se, out_hdr, out_attr, &inode.attr, 1);
}

static int fuse_mknod(struct fuse_session *se, RamCloudUserData *userData,
                      struct fuse_in_header *in_hdr, struct fuse_mknod_in *in_mknod,
                      const char *const in_name,
                      struct fuse_out_header *out_hdr, struct fuse_entry_out *out_entry,
                      void *completion_context, uint16_t device_id)
{
    if (in_hdr->nodeid != 1) {
        /* we don't support directories so anything other than root nodeid should not happen!? */
        fprintf(stderr, "[ERROR] mknod: invalid inode id\n");
        out_hdr->error = -EIO;
        return 0;
    }

    if (!S_ISREG(in_mknod->mode)) {
        /* We don't allow anything but files */
        fprintf(stderr, "[ERROR] mknod: invalid mode\n");
        out_hdr->error = -EINVAL;
        return 0;
    }

    RamCloudInode inode;
    strcpy(inode.name, in_name);
    inode.attr.st_dev = 0;         /* ID of device containing file */
    inode.attr.st_ino = fnv1a_hash(in_name);         /* Inode number */
    inode.attr.st_mode = S_IFREG;  /* File type and mode */
    inode.attr.st_nlink = 0;       /* Number of hard links */
    inode.attr.st_uid = 0;         /* User ID of owner */
    inode.attr.st_gid = 0;         /* Group ID of owner */
    inode.attr.st_rdev = 0;        /* Device ID (if special file) */
    inode.attr.st_size = 0;      /* Total size, in bytes */
    inode.attr.st_blksize = 1;     /* Block size for filesystem I/O */
    inode.attr.st_blocks = 1;      /* Number of 512B blocks allocated */

    // inode.attr.st_atim;  /* Time of last access */
    // inode.attr.st_mtim;  /* Time of last modification */
    // inode.attr.st_ctim;  /* Time of last status change */

    try {
        userData->ramcloud->write(userData->inodeTableId, &inode.attr.st_ino,
                                  sizeof(inode.attr.st_ino), &inode, sizeof(inode));
    } catch (RAMCloud::ClientException& e) {
        fprintf(stderr, "[ERROR] mknod write: %d\n", e.status);
        out_hdr->error = -EIO;
        return 0;
    }

    struct fuse_entry_param e;
    e.ino = inode.attr.st_ino;
    e.generation = 0;
    e.attr_timeout = 1;
    e.entry_timeout = 1;
    e.attr = inode.attr;
    return fuse_ll_reply_entry(se, out_hdr, out_entry, &e);
}

static int fuse_write(struct fuse_session *se, RamCloudUserData *userData,
                      struct fuse_in_header *in_hdr, struct fuse_write_in *in_write,
                      struct iovec *in_iov, int iovcnt,
                      struct fuse_out_header *out_hdr, struct fuse_write_out *out_write,
                      void *completion_context, uint16_t device_id)
{
    // The ramcloud backend only allows for complete file writes
    if (in_write->offset != 0) {
        fprintf(stderr, "[ERROR] write: offset != 0\n");
        out_hdr->error = -EINVAL;
        return 0;
    }

    new AsyncWriteOp{*userData, in_hdr->nodeid, in_iov, iovcnt, se, out_hdr, out_write,
                     completion_context};

    return EWOULDBLOCK;
}

static int fuse_read(struct fuse_session *se, RamCloudUserData *userData,
                 struct fuse_in_header *in_hdr, struct fuse_read_in *in_read,
                 struct fuse_out_header *out_hdr, struct iovec *in_iov, int iovcnt,
                 void *completion_context, uint16_t device_id)
{

    new AsyncReadOp{*userData, in_hdr->nodeid, in_read->offset, in_iov, iovcnt, se, out_hdr,
                    completion_context};

    return EWOULDBLOCK;
}

static int fuse_readdir(struct fuse_session *se, RamCloudUserData *userData,
                        struct fuse_in_header *in_hdr, struct fuse_read_in *in_read, bool plus,
                        struct fuse_out_header *out_hdr, struct iov iov,
                        void *completion_context, uint16_t device_id)
{
    RAMCloud::TableEnumerator *tableEnum =
        reinterpret_cast<RAMCloud::TableEnumerator *>(in_read->fh);

    size_t offset = in_read->offset + 1;
    size_t totalWritten = 0;
    while (tableEnum->hasNext()) {
        const void* key;
        uint32_t keyLength;
        const void* value;
        uint32_t valueLength;
        tableEnum->nextKeyAndData(&keyLength, &key, &valueLength, &value);
        if (keyLength != sizeof(key)) {
            fprintf(stderr, "[ERROR] readdir: invalid key size\n");
            break;
        }
        if (valueLength != sizeof(RamCloudInode)) {
            fprintf(stderr, "[ERROR] readdir: invalid value size\n");
            break;
        }
        const RamCloudInode *inode = reinterpret_cast<const RamCloudInode *>(value);
        size_t written = 0;
        if (plus) {
            struct fuse_entry_param e;
            e.ino = inode->attr.st_ino;
            e.generation = 0;
            e.attr = inode->attr;
            e.attr_timeout = 1;
            e.entry_timeout = 1;
            written = fuse_add_direntry_plus(&iov, inode->name, &e, offset);
        } else {
            written = fuse_add_direntry(&iov, inode->name, &inode->attr, offset);
        }
        if (written == 0) {
            /* buffer full */
            break;
        }
        totalWritten += written;
        offset++;
    }

    out_hdr->len += totalWritten;
    return 0;
}

static int fuse_opendir(struct fuse_session *se, RamCloudUserData *userData,
                        struct fuse_in_header *in_hdr, struct fuse_open_in *in_open,
                        struct fuse_out_header *out_hdr, struct fuse_open_out *out_open,
                        void *completion_context, uint16_t device_id)
{
    struct fuse_file_info fi = {};
    fi.fh = (uint64_t) new RAMCloud::TableEnumerator{*userData->ramcloud, userData->inodeTableId,
                                                     false};
    fi.flags = in_open->flags;
    return fuse_ll_reply_open(se, out_hdr, out_open, &fi);
}

static int fuse_releasedir(struct fuse_session *se, RamCloudUserData *userData,
                           struct fuse_in_header *in_hdr, struct fuse_release_in *in_release,
                           struct fuse_out_header *out_hdr,
                           void *completion_context, uint16_t device_id)
{
    RAMCloud::TableEnumerator *tableEnum =
        reinterpret_cast<RAMCloud::TableEnumerator *>(in_release->fh);
    delete tableEnum;
    return 0;
}

static int fuse_unlink(struct fuse_session *se, RamCloudUserData *userData,
                       struct fuse_in_header *in_hdr, const char *const in_name,
                       struct fuse_out_header *out_hdr,
                       void *completion_context, uint16_t device_id)
{
    /* TODO: use multiRemove */
    uint64_t inodeId = fnv1a_hash(in_name);
    RAMCloud::RemoveRpc removeInode{userData->ramcloud, userData->inodeTableId, &inodeId,
                                    sizeof(inodeId)};
    RAMCloud::RemoveRpc removeData{userData->ramcloud, userData->dataTableId, &inodeId,
                                    sizeof(inodeId)};
    while (!removeData.isReady() || !removeInode.isReady());
    try {
        removeInode.wait();
        removeData.wait();
    } catch (RAMCloud::ClientException& e) {
        fprintf(stderr, "[ERROR] unlink: %d\n", e.status);
        out_hdr->error = -EIO;
    }
    return 0;
}

static void *ramcloud_poll(void *arg)
{
    RamCloudUserData *userData = reinterpret_cast<RamCloudUserData *>(arg);

    printf("ramcloud_poll: start\n");

    while (!userData->stopPoller) {
        userData->ramcloud->poll();
    }

    printf("ramcloud_poll: stop\n");

    return NULL;
}

void usage()
{
    printf("dpfs_kv [-c config_path]\n");
}

int main(int argc, char **argv)
{
    char *coordinator = NULL;

    int opt;
    while ((opt = getopt(argc, argv, "c:")) != -1) {
        switch (opt) {
            case 'c':
                config_path = optarg;
                break;
            default: /* '?' */
                usage();
                exit(1);
        }
    }

    auto res = toml::parseFile(conf_path);
    if (!res.table) {
        std::cerr << "cannot parse file: " << res.errmsg << std::endl;
        return -1;
    }
    auto conf = res.table->getTable("kv");
    if (!conf) {
        std::cerr << "missing [kv]" << std::endl;
        return -1;
    }
    auto [ok, coordinator] = conf->getString("ramcloud_coordinator");
    if (!ok) {
        std::cerr << "The config must contain  [kv]:`ramcloud_coordinator`" << std::endl;
        return -1;
    }

    printf("dpfs_kv starting up!\n");
    printf("Connecting to RAMCloud coordinator %s\n", coordinator);

    RAMCloud::RamCloud ramcloud(coordinator);
    RamCloudUserData user_data;
    user_data.stopPoller = false;
    user_data.ramcloud = &ramcloud;

    user_data.dataTableId = ramcloud.createTable("data");
    user_data.inodeTableId = ramcloud.createTable("inode");

    printf("Start poll thread for RAMCloud %s\n", coordinator);
    // poll ramcloud here
    pthread_t ramcloud_poll_thread;
    if (pthread_create(&ramcloud_poll_thread, NULL, ramcloud_poll, &user_data)) {
        fprintf(stderr, "Failed to create RAMCloud poll thread, exiting...\n");
        return -1;
    }

    struct fuse_ll_operations ops;
    memset(&ops, 0, sizeof(ops));
    ops.init = (typeof(ops.init)) fuse_init;
    ops.lookup = (typeof(ops.lookup)) fuse_lookup;
    ops.getattr = (typeof(ops.getattr)) fuse_getattr;
    ops.setattr = (typeof(ops.setattr)) fuse_setattr;
    ops.opendir = (typeof(ops.opendir)) fuse_opendir;
    ops.releasedir = (typeof(ops.releasedir)) fuse_releasedir;
    ops.readdir = (typeof(ops.readdir)) fuse_readdir;
    ops.open = NULL;
    ops.release = NULL;
    ops.create = NULL;
    ops.forget = NULL;
    ops.read =  (typeof(ops.read)) fuse_read;
    ops.write = (typeof(ops.write)) fuse_write;
    ops.statfs = (typeof(ops.statfs)) fuse_statfs;
    ops.flush = NULL;
    ops.mknod = (typeof(ops.mknod)) fuse_mknod; /* not sure if this is needed at all */
    ops.unlink = (typeof(ops.unlink)) fuse_unlink;

    dpfs_fuse_main(&ops, conf_path, &user_data, NULL, NULL);

    user_data.stopPoller = true;
    pthread_join(ramcloud_poll_thread, NULL);

    return 0;
}

