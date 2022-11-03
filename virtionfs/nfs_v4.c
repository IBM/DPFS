/*
#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

#include <nfsc/libnfs-raw-nfs4.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <linux/fuse.h>
#include <arpa/inet.h>

int nfs4_clone_fh(nfs_fh4 *dst, nfs_fh4 *src) {
    dst->nfs_fh4_len = src->nfs_fh4_len;
    dst->nfs_fh4_val = malloc(dst->nfs_fh4_len);
    if (!dst->nfs_fh4_val) {
        return -ENOMEM;
    }
    memcpy(dst->nfs_fh4_val, src->nfs_fh4_val, dst->nfs_fh4_len);

    return 0;
}

int nfs4_find_op(struct nfs_context *nfs, COMPOUND4res *res, int op)
{
        int i;

        for (i = 0; i < (int)res->resarray.resarray_len; i++) {
                if (res->resarray.resarray_val[i].resop == op) {
                        break;
                }
        }
        if (i == res->resarray.resarray_len) {
                return -1;
        }

        return i;
}

int nfs4_op_lookup(struct nfs_context *nfs, nfs_argop4 *op, const char *path)
{
    LOOKUP4args *largs;

    op->argop = OP_LOOKUP;
    largs = &op->nfs_argop4_u.oplookup;
    largs->objname.utf8string_len = strlen(path);
    largs->objname.utf8string_val = (char *) path;

    return 1;
}

int nfs4_op_getattr(struct nfs_context *nfs, nfs_argop4 *op,
    uint32_t *attributes, int count)
{
    GETATTR4args *gaargs;

    op->argop = OP_GETATTR;
    gaargs = &op->nfs_argop4_u.opgetattr;
    memset(gaargs, 0, sizeof(*gaargs));

    gaargs->attr_request.bitmap4_val = attributes;
    gaargs->attr_request.bitmap4_len = count;

    return 1;
}

/* Taken from libnfs/lib/nfs_v4.c
 * commit: 2678dfecd9c797991b7768490929b1478f339809 */
__attribute__((unused)) uint64_t nfs_hton64(uint64_t val)
{
    int i;
    uint64_t res;
    unsigned char *ptr = (void *)&res;

    for (i = 0; i < 8; i++) {
        ptr[7 - i] = val & 0xff;
        val >>= 8;
    }
    return res;
}

__attribute__((unused)) uint64_t nfs_ntoh64(uint64_t val)
{
    int i;
    uint64_t res;
    unsigned char *ptr = (void *)&res;

    for (i = 0; i < 8; i++) {
        ptr[7 - i] = val & 0xff;
        val >>= 8;
    }
    return res;
}

uint64_t nfs_pntoh64(const uint32_t *buf)
{
    uint64_t val;

    val = ntohl(*(uint32_t *)(void *)buf);
    buf++;
    val <<= 32;
    val |= ntohl(*(uint32_t *)(void *)buf);

    return val;
}

int nfs_get_ugid(struct nfs_context *nfs, const char *buf, int slen, int is_user)
{
    int ugid = 0;

    while (slen) {
        if (isdigit(*buf)) {
            ugid *= 10;
            ugid += *buf - '0';
        } else {
            
            return 65534;
        }
        buf++;
        slen--;
    }
    return ugid;
}

#define CHECK_GETATTR_BUF_SPACE(len, size)                              \
    if (len < size) {                                                   \
        fprintf(stderr, "Not enough data in fattr4\n");                 \
        return -1;                                                      \
    }

int nfs_parse_attributes(struct nfs_context *nfs, struct fuse_attr *attr,
    const char *buf, int len)
{
    int type, slen, pad;
    
    /* Type */
    CHECK_GETATTR_BUF_SPACE(len, 4);
    type = ntohl(*(uint32_t *)(void *)buf);
    buf += 4;
    len -= 4;
    /* Size */
    CHECK_GETATTR_BUF_SPACE(len, 8);
    attr->size = nfs_pntoh64((uint32_t *)(void *)buf);
    buf += 8;
    len -= 8;
    /* Fileid aka Inode */
    CHECK_GETATTR_BUF_SPACE(len, 8);
    attr->ino = nfs_pntoh64((uint32_t *)(void *)buf);
    buf += 8;
    len -= 8;
    /* Mode */
    CHECK_GETATTR_BUF_SPACE(len, 4);
    attr->mode = ntohl(*(uint32_t *)(void *)buf);
    buf += 4;
    len -= 4;
    switch (type) {
    case NF4REG:
            attr->mode |= S_IFREG;
            break;
    case NF4DIR:
            attr->mode |= S_IFDIR;
            break;
    case NF4BLK:
            attr->mode |= S_IFBLK;
            break;
    case NF4CHR:
            attr->mode |= S_IFCHR;
            break;
    case NF4LNK:
            attr->mode |= S_IFLNK;
            break;
    case NF4SOCK:
            attr->mode |= S_IFSOCK;
            break;
    case NF4FIFO:
            attr->mode |= S_IFIFO;
            break;
    default:
            break;
    }
    /* Num Links */
    CHECK_GETATTR_BUF_SPACE(len, 4);
    attr->nlink = ntohl(*(uint32_t *)(void *)buf);
    buf += 4;
    len -= 4;
    /* Owner */
    CHECK_GETATTR_BUF_SPACE(len, 4);
    slen = ntohl(*(uint32_t *)(void *)buf);
    buf += 4;
    len -= 4;
    pad = (4 - (slen & 0x03)) & 0x03;
    CHECK_GETATTR_BUF_SPACE(len, slen);
    attr->uid = nfs_get_ugid(nfs, buf, slen, 1);
    buf += slen;
    CHECK_GETATTR_BUF_SPACE(len, pad);
    buf += pad;
    len -= pad;
    /* Group */
    CHECK_GETATTR_BUF_SPACE(len, 4);
    slen = ntohl(*(uint32_t *)(void *)buf);
    buf += 4;
    len -= 4;
    pad = (4 - (slen & 0x03)) & 0x03;
    CHECK_GETATTR_BUF_SPACE(len, slen);
    attr->gid = nfs_get_ugid(nfs, buf, slen, 0);
    buf += slen;
    CHECK_GETATTR_BUF_SPACE(len, pad);
    buf += pad;
    len -= pad;
    /* Space Used */
    CHECK_GETATTR_BUF_SPACE(len, 8);
    uint64_t space_used = nfs_pntoh64((uint32_t *)(void *)buf);
    buf += 8;
    len -= 8;
    /* ATime */
    CHECK_GETATTR_BUF_SPACE(len, 12);
    attr->atime = nfs_pntoh64((uint32_t *)(void *)buf);
    buf += 8;
    len -= 8;
    attr->atimensec = ntohl(*(uint32_t *)(void *)buf);
    buf += 4;
    len -= 4;
    /* CTime */
    CHECK_GETATTR_BUF_SPACE(len, 12);
    attr->ctime = nfs_pntoh64((uint32_t *)(void *)buf);
    buf += 8;
    len -= 8;
    attr->ctimensec = ntohl(*(uint32_t *)(void *)buf);
    buf += 4;
    len -= 4;
    /* MTime */
    CHECK_GETATTR_BUF_SPACE(len, 12);
    attr->mtime = nfs_pntoh64((uint32_t *)(void *)buf);
    buf += 8;
    len -= 8;
    attr->mtimensec = ntohl(*(uint32_t *)(void *)buf);
    buf += 4;
    len -= 4;
    
    attr->blksize = NFS_BLKSIZE;
    attr->blocks  = (space_used + NFS_BLKSIZE -1) / NFS_BLKSIZE;

    // We don't have information for this field
    attr->rdev = 0;
    
    return 0;
}

int32_t nfs_error_to_fuse_error(nfsstat4 status) {
    if (status <= NFS4ERR_MLINK) {
        return status;
    } else {
        fprintf(stderr, "Unknown NFS status code in nfs_error_to_fuse_error!\n");
        return -ENOSYS;
    }
}

