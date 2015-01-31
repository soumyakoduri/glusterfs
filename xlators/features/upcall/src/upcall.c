/*
   Copyright (c) 2006-2014 Red Hat, Inc. <http://www.redhat.com>
   This file is part of GlusterFS.

   This file is licensed to you under your choice of the GNU Lesser
   General Public License, version 3 or any later version (LGPLv3 or
   later), or the GNU General Public License, version 2 (GPLv2), in all
   cases as published by the Free Software Foundation.
*/
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>

#ifndef _CONFIG_H
#define _CONFIG_H
#include "config.h"
#endif

#include "glusterfs.h"
#include "compat.h"
#include "xlator.h"
#include "inode.h"
#include "logging.h"
#include "common-utils.h"

#include "statedump.h"
#include "syncop.h"

#include "upcall.h"
#include "upcall-mem-types.h"
#include "glusterfs3-xdr.h"
#include "protocol-common.h"
#include "defaults.h"

#ifndef LLONG_MAX
#define LLONG_MAX LONG_LONG_MAX /* compat with old gcc */
#endif /* LLONG_MAX */

/* TODO: Below two macros have to be replaced with xlator options */
#define ON_CACHE_INVALIDATE 1
#define ON_LEASE_LK         1

#define WIND_IF_UPCALL_OFF(label) do {                     \
        if (!(ON_LEASE_LK | ON_CACHE_INVALIDATE))          \
                goto label;                                \
} while (0);

#define CHECK_LEASE(frame, client, gfid, is_write, up_entry) do {\
 if (ON_LEASE_LK) {                                             \
        ret = upcall_lease_check (frame, client,                \
                                  gfid, is_write, up_entry);    \
        if (ret > 0) {                                          \
                /* conflict lease found and recall has been sent. \
 *               * Send ERR_DELAY */                            \
                gf_log (this->name, GF_LOG_INFO,                \
                        "Delegation conflict.sending EDELAY ");                 \
                op_errno = EAGAIN; /* ideally should have been EDELAY */ \
                goto err;                                       \
        } else if (ret == 0) {                                  \
                /* No conflict lease found. Go ahead with the fop */ \
                gf_log (this->name, GF_LOG_INFO,                \
                        "No Delegation conflict. continuing with fop ");        \
        } else { /* erro */                                     \
                op_errno = EINVAL;                              \
                goto err;                                       \
        }                                                       \
 } else {                                                       \
        ret = 0;                                                \
 }                                                              \
} while (0);

#define CACHE_INVALIDATE(frame, client, gfid, p_flags) do {             \
 if (ON_CACHE_INVALIDATE) {                                             \
        (void)upcall_cache_invalidate (frame, client, gfid, p_flags);   \
 }                                                                      \
} while (0);

rpcsvc_cbk_program_t upcall_cbk_prog = {
        .progname  = "Gluster Callback",
        .prognum   = GLUSTER_CBK_PROGRAM,
        .progver   = GLUSTER_CBK_VERSION,
};

int
up_open_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
             int32_t op_ret, int32_t op_errno, fd_t *fd, dict_t *xdata)
{
        client_t         *client        = NULL;
        uint32_t         flags          = 0;
        upcall_local_t   *local         = NULL;

        WIND_IF_UPCALL_OFF(out);

        client = frame->root->client;
        local = frame->local;

        if (op_ret < 0) {
                goto out;
        }
        gf_log (this->name, GF_LOG_INFO, "In open_cbk ");
        flags = (UP_ATIME) ;
        CACHE_INVALIDATE (frame, client, local->gfid, &flags);

out:
        UPCALL_STACK_UNWIND (open, frame, op_ret, op_errno, fd, xdata);
        return 0;
}


int
up_open (call_frame_t *frame, xlator_t *this, loc_t *loc, int32_t flags,
         fd_t *fd, dict_t *xdata)
{
        int              ret             = -1;
        client_t         *client         = NULL;
        upcall_entry     *up_entry       = NULL;
        int32_t          op_errno        = ENOMEM;
        gf_boolean_t     is_write        = _gf_false;
        upcall_local_t   *local          = NULL;

        WIND_IF_UPCALL_OFF(out);

        local = upcall_local_init(frame, fd->inode->gfid);
        if (!local) {
                op_errno = ENOMEM;
                goto err;
        }

        client = frame->root->client;

        gf_log (this->name, GF_LOG_INFO, "In open ");

        if (flags & (O_WRONLY | O_RDWR)) {
                is_write = _gf_true;
        }
        CHECK_LEASE (frame, client, local->gfid,
                       is_write, &up_entry);

out:
        STACK_WIND (frame, up_open_cbk,
                    FIRST_CHILD(this), FIRST_CHILD(this)->fops->open,
                    loc, flags, fd, xdata);

        return 0;

err:
        UPCALL_STACK_UNWIND (open, frame, -1, op_errno, NULL, NULL);
        return 0;
}


int
up_writev_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                int op_ret, int op_errno, struct iatt *prebuf,
                struct iatt *postbuf, dict_t *xdata)
{
        client_t         *client        = NULL;
        uint32_t         flags          = 0;
        upcall_local_t   *local         = NULL;

        client = frame->root->client;
        local = frame->local;

        if (op_ret < 0) {
                goto out;
        }
        gf_log (this->name, GF_LOG_INFO, "In writev_cbk ");
        flags = (UP_SIZE | UP_TIMES) ;
        CACHE_INVALIDATE (frame, client, local->gfid, &flags);
out:
        UPCALL_STACK_UNWIND (writev, frame, op_ret, op_errno, prebuf, postbuf, xdata);
        return 0;
}


int
up_writev (call_frame_t *frame, xlator_t *this, fd_t *fd,
            struct iovec *vector, int count, off_t off, uint32_t flags,
            struct iobref *iobref, dict_t *xdata)
{
        int              ret             = -1;
        client_t         *client         = NULL;
        upcall_entry     *up_entry       = NULL;
        int32_t          op_errno        = ENOMEM;
        gf_boolean_t     is_write        = _gf_false;
        upcall_local_t   *local          = NULL;

        WIND_IF_UPCALL_OFF(out);

        local = upcall_local_init(frame, fd->inode->gfid);
        if (!local) {
                op_errno = ENOMEM;
                goto err;
        }

        client = frame->root->client;

        gf_log (this->name, GF_LOG_INFO, "In writev ");
        CHECK_LEASE (frame, client, local->gfid,
                       _gf_true, &up_entry);
out:
        STACK_WIND (frame, up_writev_cbk,
                    FIRST_CHILD(this), FIRST_CHILD(this)->fops->writev,
                    fd, vector, count, off, flags, iobref, xdata);

        return 0;

err:
        UPCALL_STACK_UNWIND (writev, frame, -1, op_errno, NULL, NULL, NULL);
/*        up_writev_cbk (frame, NULL, frame->this, -1, op_errno, NULL, NULL, NULL); */
        return 0;
}


int
up_readv_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
               int op_ret, int op_errno,
               struct iovec *vector, int count, struct iatt *stbuf,
               struct iobref *iobref, dict_t *xdata)
{
        client_t         *client        = NULL;
        uint32_t         flags          = 0;
        upcall_local_t   *local         = NULL;

        WIND_IF_UPCALL_OFF(out);

        client = frame->root->client;
        local = frame->local;

        if (op_ret < 0) {
                goto out;
        }
        gf_log (this->name, GF_LOG_INFO, "In readv_cbk ");
        flags = (UP_ATIME) ;
        CACHE_INVALIDATE (frame, client, local->gfid, &flags);

out:
        UPCALL_STACK_UNWIND (readv, frame, op_ret, op_errno, vector, count, stbuf,
                          iobref, xdata);

        return 0;
}

int
up_readv (call_frame_t *frame, xlator_t *this,
             fd_t *fd, size_t size, off_t offset, uint32_t flags, dict_t *xdata)
{
        int              ret             = -1;
        client_t         *client         = NULL;
        upcall_entry     *up_entry       = NULL;
        int32_t          op_errno        = ENOMEM;
        gf_boolean_t     is_write        = _gf_false;
        upcall_local_t   *local          = NULL;

        WIND_IF_UPCALL_OFF(out);

        local = upcall_local_init(frame, fd->inode->gfid);
        if (!local) {
                op_errno = ENOMEM;
                goto err;
        }

        client = frame->root->client;

        gf_log (this->name, GF_LOG_INFO, "In readv ");
        CHECK_LEASE (frame, client, local->gfid,
                       _gf_false, &up_entry);

out:
        STACK_WIND (frame, up_readv_cbk,
                    FIRST_CHILD(this), FIRST_CHILD(this)->fops->readv,
                    fd, size, offset, flags, xdata);

        return 0;

err:
        UPCALL_STACK_UNWIND (readv, frame, -1, op_errno, NULL, 0, NULL, NULL, NULL);

        return 0;
}

int32_t
up_lk_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                int32_t op_ret, int32_t op_errno, struct gf_flock *lock,
                dict_t *xdata)
{
        client_t         *client        = NULL;
        uint32_t         flags          = 0;
        upcall_local_t   *local         = NULL;
        gf_boolean_t     is_write_lease = _gf_false;
        int              ret            = -1;

        WIND_IF_UPCALL_OFF(out);

        client = frame->root->client;
        local = frame->local;

        if (op_ret < 0) {
                gf_log (this->name, GF_LOG_INFO, "In lk_cbk op_ret = %d", op_ret);
                goto out;
        }
        gf_log (this->name, GF_LOG_INFO, "In lk_cbk ");
        if (local->is_lease_enabled) {
                gf_log (this->name, GF_LOG_INFO, "In up_lk_cbk, in"
                        " is_lease_enabled block");
                /* Add/Remove the lease entry
                 */
                switch (lock->l_type) {
                case GF_LK_F_RDLCK:
                        is_write_lease = _gf_false;
                        ret = add_lease (frame, client, local->gfid, local->fd,
                                         is_write_lease);
                        if (ret < 0) {
                                goto err;
                        }
                        break;
                case GF_LK_F_WRLCK:
                        is_write_lease = _gf_true;
                        ret = add_lease (frame, client, local->gfid, local->fd, is_write_lease);
                        if (ret < 0) {
                                goto err;
                        }
                        break;
                case GF_LK_F_UNLCK:
                        ret = remove_lease (frame, client, local->fd, local->gfid);
                        if (ret < 0) {
                                goto err;
                        }
                        break;
                }
        }
        flags = (UP_ATIME) ;
        CACHE_INVALIDATE (frame, client, local->gfid, &flags);
        goto out;

err:
        op_ret = ret;
        op_errno = EINVAL;
out:
        gf_log (this->name, GF_LOG_INFO, "In lk_cbk ");
        UPCALL_STACK_UNWIND (lk, frame, op_ret, op_errno, lock, xdata);
        return 0;
}

int
up_lk (call_frame_t *frame, xlator_t *this,
       fd_t *fd, int32_t cmd, struct gf_flock *flock, dict_t *xdata)
{
        int              ret             = -1;
        client_t         *client         = NULL;
        upcall_entry     *up_entry       = NULL;
        int32_t          op_errno        = ENOMEM;
        gf_boolean_t     is_write        = _gf_false;
        upcall_local_t   *local          = NULL;
        dict_t           *dict           = NULL;
        int32_t          is_lease_enabled = 0;
        char             key[1024]       = {0};
        gf_boolean_t     is_write_lease  = _gf_false;

        WIND_IF_UPCALL_OFF(out);

        local = upcall_local_init(frame, fd->inode->gfid);
        if (!local) {
                op_errno = ENOMEM;
                goto err;
        }
        local->fd = fd;

        gf_log (this->name, GF_LOG_INFO, "In up_lk ");
        client = frame->root->client;
        snprintf (key, sizeof (key), "set_lease");
        ret = dict_get_int32 (xdata, key, (void *)&is_lease_enabled);
        if (ret) {
                op_errno = EINVAL;
                goto err;
        }
        gf_log (this->name, GF_LOG_INFO, "In up_lk, is_lease_enabled = %d ",
                is_lease_enabled);
        dict_del (xdata, key);

        /* Either lock/lease request, first check if there
         * are any conflicting lease present.
         * 'LOCK' request is considered as write_access as a new state is added.
         * Skip the check if its unlock request. Reason being
         *      if its LEASE_RETURN request we need not send recalls
         *      if its unlock request, the reason there is a lock present means that
         *      there are no leases on that file taken from other client;
         */
        if (flock->l_type != GF_LK_F_UNLCK) {
                CHECK_LEASE (frame, client, local->gfid,
                               _gf_true, &up_entry);
        }

        if (is_lease_enabled) {
                if (!IA_ISREG(fd->inode->ia_type)) {
                        op_errno = EINVAL;
                        goto err;
                }
                gf_log (this->name, GF_LOG_INFO, "In up_lk, in"
                        " is_lease_enabled block");
                local->is_lease_enabled = _gf_true;
        }

out:
        STACK_WIND (frame, up_lk_cbk, FIRST_CHILD(this), FIRST_CHILD(this)->fops->lk,
                    fd, cmd, flock, xdata);
        return 0;

err:
        gf_log (this->name, GF_LOG_INFO, "In up_lk err section, ret = %d, op_errno=%d", ret, op_errno);
        UPCALL_STACK_UNWIND (lk, frame, -1, op_errno, NULL, NULL);
/*        up_lk_cbk (frame, NULL, frame->this, -1, op_errno, NULL, NULL);*/
        return 0;
}

int
up_truncate_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                  int op_ret, int op_errno, struct iatt *prebuf,
                  struct iatt *postbuf, dict_t *xdata)
{
        client_t         *client        = NULL;
        uint32_t         flags          = 0;
        upcall_local_t   *local         = NULL;

        WIND_IF_UPCALL_OFF(out);

        client = frame->root->client;
        local = frame->local;

        if (op_ret < 0) {
                goto out;
        }
        gf_log (this->name, GF_LOG_INFO, "In truncate_cbk ");
        flags = (UP_SIZE | UP_TIMES) ;
        CACHE_INVALIDATE (frame, client, local->gfid, &flags);

out:
        UPCALL_STACK_UNWIND (truncate, frame, op_ret, op_errno,
                             prebuf, postbuf, xdata);
        return 0;
}

int
up_truncate (call_frame_t *frame, xlator_t *this, loc_t *loc, off_t offset,
              dict_t *xdata)
{
        int              ret             = -1;
        client_t         *client         = NULL;
        upcall_entry     *up_entry       = NULL;
        int32_t          op_errno        = ENOMEM;
        gf_boolean_t     is_write        = _gf_false;
        upcall_local_t   *local          = NULL;

        WIND_IF_UPCALL_OFF(out);

        local = upcall_local_init(frame, loc->gfid);
        if (!local) {
                op_errno = ENOMEM;
                goto err;
        }

        client = frame->root->client;

        gf_log (this->name, GF_LOG_INFO, "In truncate ");
        /* do we need to use loc->inode->gfid ?? */
        CHECK_LEASE (frame, client, local->gfid,
                       _gf_true, &up_entry);

out:
        STACK_WIND (frame, up_truncate_cbk,
                    FIRST_CHILD(this), FIRST_CHILD(this)->fops->truncate,
                    loc, offset, xdata);

        return 0;

err:
        /*op_errno = (op_errno == -1) ? errno : op_errno;*/
        UPCALL_STACK_UNWIND (truncate, frame, -1, op_errno, NULL, NULL, NULL);

        return 0;
}

int
up_setattr_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                 int op_ret, int op_errno, struct iatt *statpre,
                 struct iatt *statpost, dict_t *xdata)
{
        client_t         *client        = NULL;
        uint32_t         flags          = 0;
        upcall_local_t   *local         = NULL;

        WIND_IF_UPCALL_OFF(out);

        client = frame->root->client;
        local = frame->local;

        if (op_ret < 0) {
                goto out;
        }
        gf_log (this->name, GF_LOG_INFO, "In truncate_cbk ");
        /* XXX: setattr -> UP_SIZE or UP_OWN or UP_MODE or UP_TIMES
         * or INODE_UPDATE (or UP_PERM esp incase of ACLs -> INODE_INVALIDATE)
         * Need to check what attr is changed and accordingly pass UP_FLAGS.
         */
        flags = (UP_SIZE | UP_TIMES | UP_OWN | UP_MODE | UP_PERM) ;
        CACHE_INVALIDATE (frame, client, local->gfid, &flags);

out:
        UPCALL_STACK_UNWIND (setattr, frame, op_ret, op_errno,
                             statpre, statpost, xdata);
        return 0;
}

int
up_setattr (call_frame_t *frame, xlator_t *this, loc_t *loc,
             struct iatt *stbuf, int32_t valid, dict_t *xdata)
{
        int              ret             = -1;
        client_t         *client         = NULL;
        upcall_entry     *up_entry       = NULL;
        int32_t          op_errno        = ENOMEM;
        gf_boolean_t     is_write        = _gf_false;
        upcall_local_t   *local          = NULL;

        WIND_IF_UPCALL_OFF(out);

        local = upcall_local_init(frame, loc->gfid);
        if (!local) {
                op_errno = ENOMEM;
                goto err;
        }

        client = frame->root->client;

        gf_log (this->name, GF_LOG_INFO, "In setattr ");
        /* do we need to use loc->inode->gfid ?? */
        CHECK_LEASE (frame, client, local->gfid,
                       _gf_true, &up_entry);

out:
        STACK_WIND (frame, up_setattr_cbk,
                           FIRST_CHILD(this),
                           FIRST_CHILD(this)->fops->setattr,
                            loc, stbuf, valid, xdata);
        return 0;

err:
       /* op_errno = (op_errno == -1) ? errno : op_errno; */
        UPCALL_STACK_UNWIND (setattr, frame, -1, op_errno, NULL, NULL, NULL);

        return 0;
}

int
up_rename_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                   int32_t op_ret, int32_t op_errno, struct iatt *stbuf,
                   struct iatt *preoldparent, struct iatt *postoldparent,
                   struct iatt *prenewparent, struct iatt *postnewparent,
                   dict_t *xdata)
{
        client_t         *client        = NULL;
        uint32_t         flags          = 0;
        upcall_local_t   *local         = NULL;

        WIND_IF_UPCALL_OFF(out);

        client = frame->root->client;
        local = frame->local;

        if (op_ret < 0) {
                goto out;
        }
        gf_log (this->name, GF_LOG_INFO, "In rename_cbk ");
        flags = (UP_RENAME);
        CACHE_INVALIDATE (frame, client, local->gfid, &flags);

        /* Need to invalidate old and new parent entries as well */
        flags = (UP_TIMES);
        CACHE_INVALIDATE (frame, client, preoldparent->ia_gfid, &flags);
        if (uuid_compare (preoldparent->ia_gfid, prenewparent->ia_gfid))
                CACHE_INVALIDATE (frame, client, prenewparent->ia_gfid, &flags);

out:
        UPCALL_STACK_UNWIND (rename, frame, op_ret, op_errno,
                             stbuf, preoldparent, postoldparent,
                             prenewparent, postnewparent, xdata);
        return 0;
}

int
up_rename (call_frame_t *frame, xlator_t *this,
          loc_t *oldloc, loc_t *newloc, dict_t *xdata)
{
        int              ret             = -1;
        client_t         *client         = NULL;
        upcall_entry     *up_entry       = NULL;
        int32_t          op_errno        = ENOMEM;
        gf_boolean_t     is_write        = _gf_false;
        upcall_local_t   *local          = NULL;

        WIND_IF_UPCALL_OFF(out);

        local = upcall_local_init(frame, oldloc->gfid);
        if (!local) {
                op_errno = ENOMEM;
                goto err;
        }

        client = frame->root->client;

        gf_log (this->name, GF_LOG_INFO, "In rename ");
        /* do we need to use loc->inode->gfid ?? */
        CHECK_LEASE (frame, client, local->gfid,
                       _gf_true, &up_entry);

out:
        STACK_WIND (frame, up_rename_cbk,
                    FIRST_CHILD(this), FIRST_CHILD(this)->fops->rename,
                    oldloc, newloc, xdata);

        return 0;

err:
        /*op_errno = (op_errno == -1) ? errno : op_errno;*/
        UPCALL_STACK_UNWIND (rename, frame, -1, op_errno, NULL, NULL, NULL, NULL,
                             NULL, NULL);

        return 0;
}

int
up_unlink_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                int op_ret, int op_errno, struct iatt *preparent,
                struct iatt *postparent, dict_t *xdata)
{
        client_t         *client        = NULL;
        uint32_t         flags          = 0;
        upcall_local_t   *local         = NULL;

        WIND_IF_UPCALL_OFF(out);

        client = frame->root->client;
        local = frame->local;

        if (op_ret < 0) {
                goto out;
        }
        gf_log (this->name, GF_LOG_INFO, "In unlink_cbk ");
        flags = (UP_NLINK | UP_TIMES) ;
        CACHE_INVALIDATE (frame, client, local->gfid, &flags);

        flags = (UP_TIMES) ;
        /* invalidate parent's entry too */
        CACHE_INVALIDATE (frame, client, postparent->ia_gfid, &flags);

out:
        UPCALL_STACK_UNWIND (unlink, frame, op_ret, op_errno,
                             preparent, postparent, xdata);
        return 0;
}

int
up_unlink (call_frame_t *frame, xlator_t *this, loc_t *loc, int xflag,
              dict_t *xdata)
{
        int              ret             = -1;
        client_t         *client         = NULL;
        upcall_entry     *up_entry       = NULL;
        int32_t          op_errno        = ENOMEM;
        gf_boolean_t     is_write        = _gf_false;
        upcall_local_t   *local          = NULL;

        WIND_IF_UPCALL_OFF(out);

        local = upcall_local_init(frame, loc->gfid);
        if (!local) {
                op_errno = ENOMEM;
                goto err;
        }

        client = frame->root->client;

        gf_log (this->name, GF_LOG_INFO, "In unlink ");
        CHECK_LEASE (frame, client, local->gfid,
                       _gf_true, &up_entry);

out:
        STACK_WIND (frame, up_unlink_cbk,
                    FIRST_CHILD(this), FIRST_CHILD(this)->fops->unlink,
                    loc, xflag, xdata);

        return 0;

err:
        UPCALL_STACK_UNWIND (unlink, frame, -1, op_errno, NULL, NULL, NULL);

        return 0;
}

int
up_link_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                int op_ret, int op_errno, inode_t *inode, struct iatt *stbuf,
                struct iatt *preparent, struct iatt *postparent, dict_t *xdata)
{
        client_t         *client        = NULL;
        uint32_t         flags          = 0;
        upcall_local_t   *local         = NULL;

        WIND_IF_UPCALL_OFF(out);

        client = frame->root->client;
        local = frame->local;

        if (op_ret < 0) {
                goto out;
        }
        gf_log (this->name, GF_LOG_INFO, "In link_cbk ");
        flags = (UP_NLINK | UP_TIMES) ;
        CACHE_INVALIDATE (frame, client, local->gfid, &flags);

        /* do we need to update parent as well?? */
out:
        UPCALL_STACK_UNWIND (link, frame, op_ret, op_errno,
                             inode, stbuf, preparent, postparent, xdata);
        return 0;
}

int
up_link (call_frame_t *frame, xlator_t *this, loc_t *oldloc,
         loc_t *newloc, dict_t *xdata)
{
        int              ret             = -1;
        client_t         *client         = NULL;
        upcall_entry     *up_entry       = NULL;
        int32_t          op_errno        = ENOMEM;
        gf_boolean_t     is_write        = _gf_false;
        upcall_local_t   *local          = NULL;

        WIND_IF_UPCALL_OFF(out);

        local = upcall_local_init(frame, oldloc->gfid);
        if (!local) {
                op_errno = ENOMEM;
                goto err;
        }

        client = frame->root->client;

        gf_log (this->name, GF_LOG_INFO, "In link ");
        CHECK_LEASE (frame, client, local->gfid,
                       _gf_true, &up_entry);

out:
        STACK_WIND (frame, up_link_cbk,
                    FIRST_CHILD(this), FIRST_CHILD(this)->fops->link,
                    oldloc, newloc, xdata);

        return 0;

err:
        UPCALL_STACK_UNWIND (link, frame, -1, op_errno, NULL, NULL, NULL, NULL, NULL);

        return 0;
}

int
up_rmdir_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                int op_ret, int op_errno, struct iatt *preparent,
                struct iatt *postparent, dict_t *xdata)
{
        client_t         *client        = NULL;
        uint32_t         flags          = 0;
        upcall_local_t   *local         = NULL;

        WIND_IF_UPCALL_OFF(out);

        client = frame->root->client;
        local = frame->local;

        if (op_ret < 0) {
                goto out;
        }
        gf_log (this->name, GF_LOG_INFO, "In rmdir_cbk ");
        flags = (UP_NLINK | UP_TIMES) ;
        CACHE_INVALIDATE (frame, client, local->gfid, &flags);
        flags = (UP_TIMES) ;
        /* invalidate parent's entry too */
        CACHE_INVALIDATE (frame, client, postparent->ia_gfid, &flags);

out:
        UPCALL_STACK_UNWIND (rmdir, frame, op_ret, op_errno,
                             preparent, postparent, xdata);
        return 0;
}

int
up_rmdir (call_frame_t *frame, xlator_t *this, loc_t *loc, int flags,
              dict_t *xdata)
{
        int              ret             = -1;
        client_t         *client         = NULL;
        upcall_entry     *up_entry       = NULL;
        int32_t          op_errno        = ENOMEM;
        gf_boolean_t     is_write        = _gf_false;
        upcall_local_t   *local          = NULL;

        WIND_IF_UPCALL_OFF(out);

        local = upcall_local_init(frame, loc->gfid);
        if (!local) {
                op_errno = ENOMEM;
                goto err;
        }

        client = frame->root->client;

        gf_log (this->name, GF_LOG_INFO, "In rmdir ");
        /*
         * at present, directory delegations are not supported
         */
        /* CHECK_LEASE (frame, client, local->gfid,
                       _gf_true, &up_entry);*/

out:
        STACK_WIND (frame, up_rmdir_cbk,
                    FIRST_CHILD(this), FIRST_CHILD(this)->fops->rmdir,
                    loc, flags, xdata);

        return 0;

err:
        UPCALL_STACK_UNWIND (rmdir, frame, -1, op_errno, NULL, NULL, NULL);

        return 0;
}

int
up_mkdir_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                int op_ret, int op_errno, inode_t *inode,
                struct iatt *stbuf, struct iatt *preparent,
                struct iatt *postparent, dict_t *xdata)
{
        client_t         *client        = NULL;
        uint32_t         flags          = 0;
        upcall_local_t   *local         = NULL;

        WIND_IF_UPCALL_OFF(out);

        client = frame->root->client;
        local = frame->local;

        if (op_ret < 0) {
                goto out;
        }
        gf_log (this->name, GF_LOG_INFO, "In mkdir_cbk ");
        flags = (UP_NLINK | UP_TIMES) ;
        CACHE_INVALIDATE (frame, client, local->gfid, &flags);
        flags = (UP_TIMES) ;
        /* invalidate parent's entry too */
        CACHE_INVALIDATE (frame, client, postparent->ia_gfid, &flags);

out:
        UPCALL_STACK_UNWIND (mkdir, frame, op_ret, op_errno,
                             inode, stbuf, preparent, postparent, xdata);
        return 0;
}

int
up_mkdir (call_frame_t *frame, xlator_t *this,
          loc_t *loc, mode_t mode, mode_t umask, dict_t *params)
{
        int              ret             = -1;
        client_t         *client         = NULL;
        upcall_entry     *up_entry       = NULL;
        int32_t          op_errno        = ENOMEM;
        gf_boolean_t     is_write        = _gf_false;
        upcall_local_t   *local          = NULL;

        WIND_IF_UPCALL_OFF(out);

        local = upcall_local_init(frame, loc->gfid);
        if (!local) {
                op_errno = ENOMEM;
                goto err;
        }

        client = frame->root->client;

        gf_log (this->name, GF_LOG_INFO, "In mkdir ");

        /* no need of lease recall as we do not support
         * directory leases
        CHECK_LEASE (frame, client, local->gfid,
                       _gf_true, &up_entry); */

out:
        STACK_WIND (frame, up_mkdir_cbk,
                    FIRST_CHILD(this), FIRST_CHILD(this)->fops->mkdir,
                    loc, mode, umask, params);

        return 0;

err:
        UPCALL_STACK_UNWIND (mkdir, frame, -1, op_errno, NULL, NULL, NULL, NULL, NULL);

        return 0;
}

int
up_create_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                int op_ret, int op_errno, fd_t *fd, inode_t *inode,
                struct iatt *stbuf, struct iatt *preparent,
                struct iatt *postparent, dict_t *xdata)
{
        client_t         *client        = NULL;
        uint32_t         flags          = 0;
        upcall_local_t   *local         = NULL;

        WIND_IF_UPCALL_OFF(out);

        client = frame->root->client;
        local = frame->local;

        if (op_ret < 0) {
                goto out;
        }
        gf_log (this->name, GF_LOG_INFO, "In create_cbk ");

        /* As its a new file create, no need of sending notification */
        /* flags = (UP_NLINK | UP_TIMES) ;
        CACHE_INVALIDATE (frame, client, local->gfid, &flags); */

        /* However invalidate parent's entry */
        flags = (UP_TIMES) ;
        CACHE_INVALIDATE (frame, client, postparent->ia_gfid, &flags);

out:
        UPCALL_STACK_UNWIND (create, frame, op_ret, op_errno, fd,
                             inode, stbuf, preparent, postparent, xdata);
        return 0;
}

int
up_create (call_frame_t *frame, xlator_t *this,
          loc_t *loc, int32_t flags, mode_t mode,
          mode_t umask, fd_t *fd, dict_t *params)
{
        int              ret             = -1;
        client_t         *client         = NULL;
        upcall_entry     *up_entry       = NULL;
        int32_t          op_errno        = ENOMEM;
        gf_boolean_t     is_write        = _gf_false;
        upcall_local_t   *local          = NULL;

        WIND_IF_UPCALL_OFF(out);

        local = upcall_local_init(frame, loc->gfid);
        if (!local) {
                op_errno = ENOMEM;
                goto err;
        }

        client = frame->root->client;

        gf_log (this->name, GF_LOG_INFO, "In create ");

        /* no need of lease recall as we do not support
         * directory leases
        CHECK_LEASE (frame, client, local->gfid,
                       _gf_true, &up_entry); */

out:
        STACK_WIND (frame, up_create_cbk,
                    FIRST_CHILD(this), FIRST_CHILD(this)->fops->create,
                    loc, flags, mode, umask, fd, params);

        return 0;

err:
        UPCALL_STACK_UNWIND (create, frame, -1, op_errno, NULL, NULL, NULL, NULL, NULL, NULL);
        return 0;
}

int32_t
mem_acct_init (xlator_t *this)
{
        int     ret = -1;

        if (!this)
                return ret;

        ret = xlator_mem_acct_init (this, gf_upcalls_mt_end + 1);

        if (ret != 0) {
                gf_log (this->name, GF_LOG_WARNING, "Memory accounting"
                        " init failed");
                return ret;
        }

        return ret;
}

void
upcall_local_wipe (xlator_t *this, upcall_local_t *local)
{
        if (local)
                mem_put (local);
}

upcall_local_t *
upcall_local_init (call_frame_t *frame, uuid_t gfid)
{
        upcall_local_t *local = NULL;
        local = mem_get0 (THIS->local_pool);
        if (!local)
                goto out;
        uuid_copy (local->gfid, gfid);
        local->is_lease_enabled = _gf_false;
        frame->local = local;
out:
        return local;
}

int
init (xlator_t *this)
{
	int			ret	= -1;
        upcalls_private_t	*priv	= NULL;

        priv = GF_CALLOC (1, sizeof (*priv),
                          gf_upcalls_mt_private_t);
	if (!priv) {
		ret = -1;
		gf_log (this->name, GF_LOG_ERROR,
			"Error allocating private struct in xlator init");
		goto out;
	}

	this->private = priv;
	ret = 0;

        INIT_LIST_HEAD (&upcall_global_list.list);
        INIT_LIST_HEAD (&upcall_global_list.client.client_list);
        pthread_mutex_init (&upcall_global_list_mutex, NULL);
        upcall_global_list.lease_cnt = 0;
        this->local_pool = mem_pool_new (upcall_local_t, 512);
out:
	if (ret) {
		GF_FREE (priv);
	}

	return ret;
}

int
fini (xlator_t *this)
{
	upcalls_private_t *priv = NULL;

	priv = this->private;
	if (!priv) {
		return 0;
	}
	this->private = NULL;
	GF_FREE (priv);

	return 0;
}

int
notify (xlator_t *this, int32_t event, void *data, ...)
{
        int           ret        = -1;
        int32_t       val        = 0;
        notify_event_data *notify_event = NULL;
        gfs3_upcall_req up_req;
        upcall_client *up_client_entry = NULL;
        dict_t        *dict      = NULL;

        switch (event) {
        case GF_EVENT_UPCALL:
        {
                gf_log (this->name, GF_LOG_INFO, "Upcall Notify event = %d",
                        event);
                if (data) {
                        dict = dict_new();

                        if (!dict) {
                                gf_log (this->name, GF_LOG_INFO,
                                        "dict allocation failed");
                                goto out;
                        }
                        gf_log (this->name, GF_LOG_INFO, "Upcall - received data");
                        notify_event = (notify_event_data *)data;
                        up_client_entry = notify_event->client_entry;

                        if (!up_client_entry) {
                                goto out;
                        }

                        memcpy(up_req.gfid, notify_event->gfid, 16);
                        gf_log (this->name, GF_LOG_INFO,
                                "Sending notify to the client- %s, gfid - %s",
                                up_client_entry->client_uid, up_req.gfid);

                        switch (notify_event->event_type) {
                        case CACHE_INVALIDATION:
                                GF_ASSERT (notify_event->extra);
                                up_req.flags = *(uint32_t *)(notify_event->extra);
                                break;
                        case RECALL_READ_LEASE:
                        case RECALL_READ_WRITE_LEASE:
                                up_req.flags = 0;
                                break;
                        default:
                                goto out;
                        }
                        up_req.event_type = notify_event->event_type;

                        ret = dict_set_str (dict, "client-uid",
                                           up_client_entry->client_uid);
                        if (ret) {
                                goto out;
                        }

                        ret = dict_set_static_ptr (dict, "upcall-request",
                                           &up_req);
                        if (ret) {
                                goto out;
                        }
                        default_notify (this, event, dict);
                }
        }
        break;
        default:
                default_notify (this, event, data);
        break;
        }
        ret = 0;
out:
        if (dict)
                dict_unref (dict);
        return ret;
}

struct xlator_fops fops = {
        .open        = up_open,
        .readv       = up_readv,
        .writev      = up_writev,
        .truncate    = up_truncate,
        .lk          = up_lk,
        .setattr     = up_setattr,
        .rename      = up_rename,
        .unlink      = up_unlink, /* invalidate both file and parent dir */
        .rmdir       = up_rmdir, /* same as above */
        .link        = up_link, /* invalidate both file and parent dir */
        .create      = up_create, /* update only direntry */
        .mkdir       = up_mkdir, /* update only dirent */
#ifdef WIP
        .ftruncate   = up_ftruncate, /* reqd? */
        .getattr     = up_getattr, /* ?? */
        .getxattr    = up_getxattr, /* ?? */
        .access      = up_access,
        .lookup      = up_lookup,
        .symlink     = up_symlink, /* invalidate both file and parent dir maybe */
        .readlink    = up_readlink, /* Needed? readlink same as read? */
        .readdirp    = up_readdirp,
        .readdir     = up_readdir,
/*  other fops to be considered -
 *   lookup, stat, opendir, readdir, readdirp, readlink, mknod, statfs, flush,
 *   fsync
 */
#endif
};

struct xlator_cbks cbks = {
};

struct volume_options options[] = {
        { .key = {NULL} },
};
