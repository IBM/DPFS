/*
#
# Copyright (C) 2017 by Ronnie Sahlberg <ronniesahlberg@gmail.com>
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

#include "nfs_v4.h"

static uint32_t create_attributes[2] = {
    0,
    (1 << (FATTR4_MODE - 32) |
     1 << (FATTR4_OWNER - 32) |
     1 << (FATTR4_OWNER_GROUP - 32))
};

int nfs4_clone_fh(vnfs_fh4 *dst, nfs_fh4 *src) {
    if (src->nfs_fh4_len > NFS4_FHSIZE) return -ENOMEM;
    dst->len = src->nfs_fh4_len;
    memcpy(dst->val, src->nfs_fh4_val, dst->len);

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

int fuse_stat_to_nfs_attrlist(int valid) { return 0;}

#define CREATE_ATTRS_SIZE 32 + 32 + sizeof(uint32_t)
int nfs4_fill_create_attrs(struct fuse_in_header *in_hdr, uint32_t mode, fattr4 *attr) {
    attr->attr_vals.attrlist4_val = malloc(CREATE_ATTRS_SIZE);

    if (!attr->attr_vals.attrlist4_val) {
        return -ENOMEM;
    }
    attr->attr_vals.attrlist4_len = 64 + sizeof(uint32_t);
    memset(attr->attr_vals.attrlist4_val, 0, attr->attr_vals.attrlist4_len);

    attr->attrmask.bitmap4_val = create_attributes;
    attr->attrmask.bitmap4_len = 2;
    
    int i = 0;
    char *str = attr->attr_vals.attrlist4_val;

    /* Mode */
    mode = htonl(mode);
    memcpy(str, &mode, sizeof(uint32_t));
    i += sizeof(uint32_t);

    /* UID */
    int l = snprintf(&str[i + 4], CREATE_ATTRS_SIZE - 4 - i,
                     "%d", in_hdr->uid);
    if (l < 0) {
        return -1;
    }
    uint32_t len = htonl(l);
    /* UID length prefix */
    memcpy(&str[i], &len, sizeof(uint32_t));
    i += 4 + l;
    i = (i + 3) & ~0x03;

    /* GID */
    l = snprintf(&str[i + 4], CREATE_ATTRS_SIZE - 4 - i,
                 "%d", in_hdr->gid);
    if (l < 0) {
        return -1;
    }
    len = htonl(l);
    /* GID length prefix */
    memcpy(&str[i], &len, sizeof(uint32_t));
    i += 4 + l;
    i = (i + 3) & ~0x03;

    return 0;
}

bool nfs4_check_session_trunking_allowed(EXCHANGE_ID4resok *l, EXCHANGE_ID4resok *r) {
    return l->eir_clientid == r->eir_clientid 
                && l->eir_server_owner.so_major_id.so_major_id_len == r->eir_server_owner.so_major_id.so_major_id_len
                && memcmp(l->eir_server_owner.so_major_id.so_major_id_val, r->eir_server_owner
                    .so_major_id.so_major_id_val, l->eir_server_owner.so_major_id.so_major_id_len) == 0
                && l->eir_server_owner.so_minor_id == r->eir_server_owner.so_minor_id
                && l->eir_server_scope.eir_server_scope_len == r->eir_server_scope.eir_server_scope_len
                && memcmp(l->eir_server_scope.eir_server_scope_val, r->eir_server_scope
                    .eir_server_scope_val, l->eir_server_scope.eir_server_scope_len) == 0;
}

int nfs4_op_createsession(nfs_argop4 *op, clientid4 clientid)
{
   CREATE_SESSION4args *arg;
   op[0].argop = OP_BIND_CONN_TO_SESSION;
   arg = &op[0].nfs_argop4_u.opcreatesession;

   arg->csa_clientid = clientid;
   arg->csa_flags = 0;
   arg->csa_cb_program = 0;
   arg->csa_sec_parms.csa_sec_parms_val = NULL;
   arg->csa_sec_parms.csa_sec_parms_len = 0;

   // Currently no caching support
   arg->csa_fore_chan_attrs.ca_maxresponsesize_cached = 0;
   arg->csa_fore_chan_attrs.ca_maxrequests = NFS4_MAXREQUESTSIZE;
   arg->csa_fore_chan_attrs.ca_maxresponsesize = NFS4_MAXRESPONSESIZE;
   arg->csa_fore_chan_attrs.ca_maxrequestsize = NFS4_MAXREQUESTSIZE;
   // We have too little control over libnfs to properly use this
   arg->csa_fore_chan_attrs.ca_headerpadsize = 0;
   // Magic from the Linux kernel, 8 seems about right
   arg->csa_fore_chan_attrs.ca_maxoperations = NFS4_MAX_OPS;
   // No RDMA here
   arg->csa_fore_chan_attrs.ca_rdma_ird.ca_rdma_ird_val = NULL;
   arg->csa_fore_chan_attrs.ca_rdma_ird.ca_rdma_ird_len = 0;

   // Currently no caching support
   arg->csa_back_chan_attrs.ca_maxresponsesize_cached = 0;
   arg->csa_back_chan_attrs.ca_maxrequests = NFS4_MAXREQUESTSIZE;
   arg->csa_back_chan_attrs.ca_maxresponsesize = NFS4_MAXRESPONSESIZE;
   arg->csa_back_chan_attrs.ca_maxrequestsize = NFS4_MAXREQUESTSIZE;
   // We have too little control over libnfs to properly use this
   arg->csa_back_chan_attrs.ca_headerpadsize = 0;
   // Magic from the Linux kernel, 8 seems about right
   arg->csa_back_chan_attrs.ca_maxoperations = NFS4_MAX_OPS;
   // No RDMA here
   arg->csa_back_chan_attrs.ca_rdma_ird.ca_rdma_ird_val = NULL;
   arg->csa_back_chan_attrs.ca_rdma_ird.ca_rdma_ird_len = 0;

   return 1;
}


int nfs4_op_bindconntosession(nfs_argop4 *op, sessionid4 *sessionid, channel_dir_from_client4 channel, bool rdma)
{
   BIND_CONN_TO_SESSION4args *bindargs;
   op[0].argop = OP_BIND_CONN_TO_SESSION;
   bindargs = &op[0].nfs_argop4_u.opbindconntosession;

   memcpy(&bindargs->bctsa_sessid, sessionid, sizeof(sessionid4));
   bindargs->bctsa_dir = CDFC4_FORE_OR_BOTH;
   bindargs->bctsa_use_conn_in_rdma_mode = rdma;

   return 1;
}

int nfs4_op_exchangeid(nfs_argop4 *op, verifier4 verifier, const char *client_name)
{
   EXCHANGE_ID4args *exidargs;
   op[0].argop = OP_EXCHANGE_ID;
   exidargs = &op[0].nfs_argop4_u.opexchangeid;

   memcpy(exidargs->eia_clientowner.co_verifier, verifier, sizeof(verifier4));
   exidargs->eia_clientowner.co_ownerid.co_ownerid_val = (char *) client_name;
   exidargs->eia_clientowner.co_ownerid.co_ownerid_len = strlen(client_name);

   exidargs->eia_state_protect.spa_how = SP4_NONE;

   exidargs->eia_flags = 0;

   exidargs->eia_client_impl_id.eia_client_impl_id_val = NULL;
   exidargs->eia_client_impl_id.eia_client_impl_id_len = 0;

   return 1;
}

/* Functions taken and modified from libnfs/lib/nfs_v4.c
 * commit: 2678dfecd9c797991b7768490929b1478f339809 */

int nfs4_op_setclientid(nfs_argop4 *op, verifier4 verifier, const char *client_name)
{
   SETCLIENTID4args *scidargs;
   op[0].argop = OP_SETCLIENTID;
   scidargs = &op[0].nfs_argop4_u.opsetclientid;

   memcpy(scidargs->client.verifier, verifier, sizeof(verifier4));
   scidargs->client.id.id_len = strlen(client_name);
   scidargs->client.id.id_val = (char *) client_name;
   /* TODO: Decide what we should do here. As long as we only
    * expose a single FD to the application we will not be able to
    * do NFSv4 callbacks easily.
    * Just give it garbage for now until we figure out how we should
    * solve this. Until then we will just have to avoid doing things
    * that require a callback.
    * ( Clients (i.e. Linux) ignore this anyway and just call back to
    *   the originating address and program anyway. )
    */
   scidargs->callback.cb_program = 0; /* NFS4_CALLBACK */
   scidargs->callback.cb_location.r_netid = "tcp";
   scidargs->callback.cb_location.r_addr = "0.0.0.0.0.0";
   scidargs->callback_ident = 0x00000001;

   return 1;
}

int nfs4_op_setclientid_confirm(struct nfs_argop4 *op, uint64_t clientid, verifier4 verifier)
{
   SETCLIENTID_CONFIRM4args *scidcargs;

   op[0].argop = OP_SETCLIENTID_CONFIRM;
   scidcargs = &op[0].nfs_argop4_u.opsetclientid_confirm;
   scidcargs->clientid = clientid;
   memcpy(scidcargs->setclientid_confirm, verifier, NFS4_VERIFIER_SIZE);

   return 1;
}

int nfs4_find_op(COMPOUND4res *res, int op)
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

int nfs4_op_lookup(nfs_argop4 *op, const char *path)
{
    LOOKUP4args *largs;

    op->argop = OP_LOOKUP;
    largs = &op->nfs_argop4_u.oplookup;
    largs->objname.utf8string_len = strlen(path);
    largs->objname.utf8string_val = (char *) path;

    return 1;
}

int nfs4_op_getattr(nfs_argop4 *op, uint32_t *attributes, int count)
{
    GETATTR4args *gaargs;

    op->argop = OP_GETATTR;
    gaargs = &op->nfs_argop4_u.opgetattr;
    memset(gaargs, 0, sizeof(*gaargs));

    gaargs->attr_request.bitmap4_val = attributes;
    gaargs->attr_request.bitmap4_len = count;

    return 1;
}

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

static int nfs_get_ugid(const char *buf, int slen, int is_user)
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

int nfs_parse_attributes(struct fuse_attr *attr,
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
    attr->uid = nfs_get_ugid(buf, slen, 1);
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
    attr->gid = nfs_get_ugid(buf, slen, 0);
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
    
    attr->blksize = VNFS_BLKSIZE;
    attr->blocks  = (space_used + VNFS_BLKSIZE -1) / VNFS_BLKSIZE;

    // We don't have information for this field
    attr->rdev = 0;
    
    return 0;
}

int nfs_parse_statfs(struct fuse_kstatfs *stat, const char *buf, int len)
{
    uint64_t u64;
    uint32_t u32;

    stat->bsize   = VNFS_BLKSIZE;
    stat->frsize  = VNFS_BLKSIZE;

    /* Files Free */
    CHECK_GETATTR_BUF_SPACE(len, 8);
    memcpy(&u64, buf, 8);
    stat->ffree = nfs_ntoh64(u64);
    buf += 8;
    len -= 8;

    /* Files Total */
    CHECK_GETATTR_BUF_SPACE(len, 8);
    memcpy(&u64, buf, 8);
    stat->files = nfs_ntoh64(u64);
    buf += 8;
    len -= 8;

    /* Max Name */
    CHECK_GETATTR_BUF_SPACE(len, 4);
    memcpy(&u32, buf, 4);
    stat->namelen = ntohl(u32);
    buf += 4;
    len -= 4;

    /* Space Avail */
    CHECK_GETATTR_BUF_SPACE(len, 8);
    memcpy(&u64, buf, 8);
    stat->bavail = nfs_ntoh64(u64) / stat->frsize;
    buf += 8;
    len -= 8;

    /* Space Free */
    CHECK_GETATTR_BUF_SPACE(len, 8);
    memcpy(&u64, buf, 8);
    stat->bfree = nfs_ntoh64(u64) / stat->frsize;
    buf += 8;
    len -= 8;

    /* Space Total */
    CHECK_GETATTR_BUF_SPACE(len, 8);
    memcpy(&u64, buf, 8);
    stat->blocks = nfs_ntoh64(u64) / stat->frsize;
    buf += 8;
    len -= 8;

    return 0;
}

int nfs_parse_fileid(uint64_t *fileid,
    const char *buf, int len)
{
    /* Fileid aka Inode */
    CHECK_GETATTR_BUF_SPACE(len, 8);
    *fileid = nfs_pntoh64((uint32_t *)(void *)buf);

    return 0;
}

