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

#define RECALL_LEASE(frame, n_event_data, gfid, up_client_entry)  do {          \
        upcall_lease *lease_entry = NULL;                                       \
        uuid_copy(n_event_data.gfid, gfid);                                     \
        n_event_data.client_entry = up_client_entry;                            \
        n_event_data.event_type =  up_client_entry->lease;                      \
        list_for_each_entry (lease_entry, &up_client_entry->lease_entries.lease_list, lease_list) {  \
                n_event_data.extra = lease_entry->fd;                           \
                gf_log (THIS->name, GF_LOG_WARNING, "upcall Lease"              \
                        "recall - %s", up_client_entry->client_uid);            \
                frame->this->notify (frame->this, GF_EVENT_UPCALL,              \
                                     &n_event_data);                            \
        }                                                                       \
} while (0);
/*
 * Create an upcall entry given a gfid
 */
upcall_entry*
add_upcall_entry (uuid_t gfid)
{
        upcall_entry *up_entry = NULL;

        up_entry = GF_CALLOC (1, sizeof(*up_entry), gf_upcalls_mt_upcall_entry_t);
        if (!up_entry) {
                gf_log (THIS->name, GF_LOG_WARNING,
                         "upcall_entry Memory allocation failed");
                return NULL;
        }
        gf_log (THIS->name, GF_LOG_INFO, "In get_upcall_entry ");

        INIT_LIST_HEAD (&up_entry->list);
        uuid_copy(up_entry->gfid, gfid);
        INIT_LIST_HEAD(&up_entry->client.client_list);
        pthread_mutex_init (&up_entry->upcall_client_mutex, NULL);
        up_entry->lease_cnt = 0;

        pthread_mutex_lock (&upcall_global_list_mutex);
        list_add_tail (&up_entry->list, &upcall_global_list.list);
        pthread_mutex_unlock (&upcall_global_list_mutex);
        return up_entry;
}

/*
 * For now upcall entries are maintained in a linked list.
 * Given a gfid, traverse through that list and lookup for an entry
 * with that gfid. If none found, create an entry with the gfid given.
 */
upcall_entry*
get_upcall_entry (uuid_t gfid)
{
        upcall_entry *up_entry = NULL;

        pthread_mutex_lock (&upcall_global_list_mutex);
        list_for_each_entry (up_entry, &upcall_global_list.list, list) {
                if (memcmp(up_entry->gfid, gfid, 16) == 0) {
                        pthread_mutex_unlock (&upcall_global_list_mutex);
                        /* found entry */
                        return up_entry;
                }
        }
        pthread_mutex_unlock (&upcall_global_list_mutex);

        /* entry not found. create one */
        up_entry = add_upcall_entry(gfid);
        return up_entry;
}

/*
 * Allocate and add a new client entry to the given upcall entry
 */
upcall_client*
add_upcall_client (call_frame_t *frame, uuid_t gfid, client_t *client,
                   upcall_entry **up_entry)
{
        upcall_client *up_client_entry = NULL;

        up_client_entry = GF_CALLOC (1, sizeof(*up_client_entry),
                                     gf_upcalls_mt_upcall_entry_t);
        if (!up_client_entry) {
                gf_log (THIS->name, GF_LOG_WARNING,
                         "upcall_entry Memory allocation failed");
                return NULL;
        }
        INIT_LIST_HEAD (&up_client_entry->client_list);
        up_client_entry->client_uid = gf_strdup(client->client_uid);
        up_client_entry->rpc = (rpcsvc_t *)(client->rpc);
        up_client_entry->trans = (rpc_transport_t *)client->trans;
        up_client_entry->access_time = time(NULL);
        INIT_LIST_HEAD (&up_client_entry->lease_entries.lease_list);
        pthread_mutex_init (&up_client_entry->upcall_lease_mutex, NULL);

        pthread_mutex_lock (&(*up_entry)->upcall_client_mutex);
        list_add_tail (&up_client_entry->client_list,
                       &(*up_entry)->client.client_list);
        pthread_mutex_unlock (&(*up_entry)->upcall_client_mutex);

        gf_log (THIS->name, GF_LOG_INFO, "upcall_entry client added - %s",
                up_client_entry->client_uid);
        return up_client_entry;
}

/*
 * Given gfid and client->uid, retrieve the corresponding upcall client entry.
 * If none found, create a new entry.
 */
upcall_client*
get_upcall_client (call_frame_t *frame, uuid_t gfid, client_t *client,
                         upcall_entry **up_entry)
{
        upcall_client *up_client_entry = NULL;
        upcall_client *up_client = NULL;
        gf_boolean_t  found_client = _gf_false;

        if (!*up_entry)
                *up_entry = get_upcall_entry(gfid);
        if (!*up_entry) { /* cannot reach here */
                return NULL;
        }
        up_client = &(*up_entry)->client;

        pthread_mutex_lock (&(*up_entry)->upcall_client_mutex);
        list_for_each_entry (up_client_entry, &up_client->client_list, client_list) {
                if (up_client_entry->client_uid) {
                        if (strcmp(client->client_uid,
                                  up_client_entry->client_uid) == 0) {
                                /* found client entry. Update the access_time */
                                up_client_entry->access_time = time(NULL);
                                found_client = _gf_true;
                                gf_log (THIS->name, GF_LOG_INFO,
                                        "upcall_entry client found - %s",
                                        up_client_entry->client_uid);
                                break;
                        }
                }
        }
        pthread_mutex_unlock (&(*up_entry)->upcall_client_mutex);

        if (!found_client) { /* create one */
                up_client_entry = add_upcall_client (frame, gfid, client,
                                                           up_entry);
        }
        ((upcall_local_t *)(frame->local))->client = up_client_entry;

        return up_client_entry;
}

/*
 * Given a gfid, client, first fetch upcall_entry based on gfid.
 * Later traverse through the client list of that upcall entry and
 * verify if there is any client entry with conflictind leases set. If yes
 * send notify call, change the lease type to RECALL_LEASE_IN_PROGRESS
 * and make a note of the recall time. If the entry is not found, create one.
 * Also cache the client entry found in the frame->local.
 *
 * But if the lease found is already getting recalled, check the recall_time.
 * Incase if it hasnt exceeded the LEASE_PERIOD, bail out. Otherwise,
 * purge the lease locks and continue with the fop.
 *
 * Assuming within the same client, all the lease lock, access_check conflicts
 * are handled by the applications/glusterfs_clients.
 *
 * Note: these lease checks are done for the fops
 *      write, read, setattr, open, lock. rename, unlink, link??
 *
 * Return : 0; if no leases present. continue processing fop
 *        : 1; conflict lease found; sent recall, send EDELAY error
 *        : -1' for any other processing error.
 */
int
upcall_lease_check (call_frame_t *frame, client_t *client, uuid_t gfid,
                    gf_boolean_t is_write, upcall_entry **up_entry)
{
        upcall_client *up_client_entry = NULL;
        upcall_client *up_client       = NULL;
        int            found_client    = 0;
        gf_boolean_t   recall_sent     = _gf_false;
        notify_event_data n_event_data;
        time_t t = time(NULL);

        *up_entry = get_upcall_entry(gfid);
        if (!*up_entry) { /* cannot reach here */
                return -1;
        }

        if (!((*up_entry)->lease_cnt > 0)) { /* No leases present */
                 gf_log (THIS->name, GF_LOG_INFO, "No leases ");
                goto out;
        }

        up_client = get_upcall_client (frame, gfid, client, up_entry);
        if (!up_client) {
                goto err;
        }

        /* Assuming within the same client, all the lease lock, access_check conflicts
         * are handled by the applications/glusterfs_clients.
         *
         * TODO: For rename & unlink fops, recall the delegation even if its
         * from the same client.
         */
        if (up_client->lease_cnt > 0) {
        if (up_client->lease == READ_WRITE_LEASE) {
                GF_ASSERT ((*up_entry)->lease_cnt == 1);
                if ((frame->op == GF_FOP_RENAME) ||
                    (frame->op == GF_FOP_UNLINK)) {
                        RECALL_LEASE(frame, n_event_data, gfid,
                                     up_client);
                        up_client->lease =
                                     RECALL_READ_WRITE_LEASE_IN_PROGRESS;
                        recall_sent = _gf_true;
                }
                goto out;
        } else if (up_client->lease == READ_LEASE) {
                if ((frame->op == GF_FOP_RENAME) ||
                    (frame->op == GF_FOP_UNLINK) || is_write) {
                        RECALL_LEASE(frame, n_event_data, gfid,
                                     up_client);
                        up_client->lease =
                                     RECALL_READ_LEASE_IN_PROGRESS;
                        recall_sent = _gf_true;
                }
                if ((*up_entry)->lease_cnt == 1)
                        goto out;
        } else {
                recall_sent = _gf_true;

#ifdef PURGE_LEASE
                if (time(NULL) > (up_client->recall_time + LEASE_PERIOD)) {
                        /* Purge the expired lease */
                        purge_lease (*up_entry, up_client, client);
                }
#endif
                if ((*up_entry)->lease_cnt == 1) {
                        goto out;
                }
        }
        }

        pthread_mutex_lock (&(*up_entry)->upcall_client_mutex);
        list_for_each_entry (up_client_entry, &up_client->client_list, client_list) {
                if (up_client_entry->client_uid) {
                        if (strcmp(client->client_uid, up_client_entry->client_uid)) {
                                /* any other client */
                                if (!up_client_entry->lease_cnt)
                                        continue;
                                recall_sent = _gf_true;
                                switch (up_client_entry->lease) {
                                case READ_WRITE_LEASE:
                                        /* there can only be one READ_WRITE_LEASE */
                                        /* send notify */
                                        GF_ASSERT ((*up_entry)->lease_cnt == 1);
                                        RECALL_LEASE(frame, n_event_data, gfid, up_client_entry);
                                        up_client_entry->lease =
                                                         RECALL_READ_WRITE_LEASE_IN_PROGRESS;
                                        up_client_entry->recall_time = time(NULL);
                                        goto unlock;
                                case READ_LEASE:
                                        if (is_write) {
                                        /* send notify */
                                        RECALL_LEASE(frame, n_event_data, gfid, up_client_entry);
                                        up_client_entry->lease =
                                                         RECALL_READ_LEASE_IN_PROGRESS;
                                        up_client_entry->recall_time = time(NULL);
                                        }
                                        break;
                                case RECALL_READ_LEASE_IN_PROGRESS:
                                case RECALL_READ_WRITE_LEASE_IN_PROGRESS:
                                        /* Check if recall_time exceeded lease_time
                                         * XXX: As per rfc, server shoudnt recall
                                         * lease if CB_PATH_DOWN.
                                         */
#ifdef PURGE_LEASE
                                        if (time(NULL) >
                                                 (up_client_entry->recall_time +
                                                         LEASE_PERIOD)) {
                                                /* Remove the lease */
                                                gf_log (THIS->name, GF_LOG_WARNING,
                                                        "upcall Lease recall time"
                                                        "expired, deleting it - %s",
                                                        up_client_entry->client_uid);
                                                purge_lease (*up_entry, up_client_entry, client);
                                        }
#endif
                                         if ((*up_entry)->lease_cnt == 1) {
                                                 /* No More READ_LEASES present */
                                                goto unlock;
                                         }
                                        break;
                                case PURGE_IN_PROGRESS:
                                        break;
                                case PURGE_FAILED:
                                        /* lock xlator couldn't purge this lease */
                                        goto err;
                                default:
                                        /* shouldnt be reaching here */
                                        goto err;
                                }
                        }
                }
        }
unlock:
        pthread_mutex_unlock (&(*up_entry)->upcall_client_mutex);

out:
        return ((recall_sent == _gf_true) ?  1 : 0);

err:
        return -1;
}

static int
purge_lease_cbk (call_frame_t *frame, void *cookie,
                 xlator_t *this, int32_t op_ret,
                 int32_t op_errno, dict_t *xdata)
{
        int32_t    ret    = -1;
        fd_t      *fd     = NULL;
        client_t  *client = NULL;
        upcall_local_t     *local;
        upcall_lease       *up_lease;
        int       found   = 0;

        if (op_ret < 0) {
                goto out;
        }

        local = (upcall_local_t *)frame->local;
        fd = local->fd;
        client = frame->root->client;

        fd_unref (fd);

        gf_client_unref (client);
        UPCALL_STACK_DESTROY (frame);

        list_for_each_entry (up_lease, &local->client->lease_entries.lease_list,
                             lease_list) {
/*                if (is_same_lkowner(&lk_owner, &c_lkowner)) { */
                if (up_lease->fd == local->fd) {
                        list_del (&up_lease->lease_list);
                        found = 1;
                        break;
                }
        }
        GF_FREE (up_lease);
        local->client->lease_cnt--;
        if (!local->client->lease_cnt)
                local->client->lease = 0;
        local->up_entry->lease_cnt--;
        ret = 0;
out:
        if (!ret)
                local->client->lease = PURGE_FAILED;
        return ret;
}

int purge_lease (upcall_entry *up_entry, upcall_client *up_client, client_t *client)
{
        fd_t               *fd = NULL;
        int                 i = 0, ret = -1;
        call_frame_t       *tmp_frame = NULL;
        xlator_t           *bound_xl = NULL;
        char               *path     = NULL;
        upcall_lease       *lease = NULL;
        upcall_local_t     *local = NULL;

        bound_xl = client->bound_xl;

        pthread_mutex_lock (&up_client->upcall_lease_mutex);
        list_for_each_entry (lease, &up_client->lease_entries.lease_list, lease_list) {

                fd = lease->fd;

                if (fd != NULL) {
                        tmp_frame = create_frame (THIS, THIS->ctx->pool);
                        if (tmp_frame == NULL) {
                                goto out;
                        }

                        local = upcall_local_init(tmp_frame, up_entry->gfid);
                        if (!local) {
                                errno = ENOMEM;
                                goto out;
                        }
                        GF_ASSERT (fd->inode);

                        ret = inode_path (fd->inode, NULL, &path);

                        if (ret > 0) {
                                gf_log (THIS->name, GF_LOG_INFO,
                                        "fd cleanup on %s", path);
                                GF_FREE (path);
                        }  else {

                                gf_log (THIS->name, GF_LOG_INFO,
                                        "fd cleanup on inode with gfid %s",
                                        uuid_utoa (fd->inode->gfid));
                        }

                        local->fd = fd;
                        local->client = up_client;
                        local->up_entry = up_entry;
                        tmp_frame->root->pid = 0;
                        gf_client_ref (client);
                        tmp_frame->root->client = client;
                        memset (&tmp_frame->root->lk_owner, 0,
                                sizeof (gf_lkowner_t));

                        STACK_WIND (tmp_frame,
                                    purge_lease_cbk,
                                    bound_xl, bound_xl->fops->flush, fd, NULL);
                }
        }
        pthread_mutex_lock (&up_client->upcall_lease_mutex);

        ret = 0;

out:
        if (!ret)
                up_client->lease = PURGE_FAILED;
        else
                up_client->lease = PURGE_IN_PROGRESS;
        return ret;
}

int
remove_lease (call_frame_t *frame, client_t *client, fd_t *fd, uuid_t gfid)
{
        upcall_client *up_client_entry = NULL;
        upcall_entry  *up_entry        = NULL;
        gf_lkowner_t   c_lkowner, lkowner;
        int            found           = 0;
        upcall_lease  *up_lease        = NULL;

        c_lkowner = frame->root->lk_owner;

        if (((upcall_local_t *)frame->local)->client) {
                up_client_entry = ((upcall_local_t *)frame->local)->client;
        } else {
                up_client_entry = get_upcall_client (frame, gfid, client, &up_entry);
                if (!up_client_entry) {
                        return -1;
                }
        }

        pthread_mutex_lock (&up_client_entry->upcall_lease_mutex);
        list_for_each_entry (up_lease, &up_client_entry->lease_entries.lease_list,
                             lease_list) {
/*                if (is_same_lkowner(&lk_owner, &c_lkowner)) { */
                if (up_lease->fd == fd) {
                        list_del (&up_lease->lease_list);
                        found = 1;
                        break;
                }
        }
        pthread_mutex_unlock (&up_client_entry->upcall_lease_mutex);

        if (!found) {
                gf_log (THIS->name, GF_LOG_WARNING, "No lease lock found - %s",
                        client->client_uid);
                /* similar to Unlock, lets return success */
                /* It may so happen that application tried to release the lock
                 * and we may have purged it at the same time.
                 * XXX: Need to investigate.
                 */
                return 0;
        }

        GF_FREE (up_lease);

        /* Assuming within the same client, all the lease lock conflicts will be handled
         * by the applications/glusterfs_clients.
         */
        up_client_entry->lease_cnt--;
        if (!up_client_entry->lease_cnt)
                up_client_entry->lease = 0;
        up_client_entry->access_time = time(NULL);

        up_entry->lease_cnt--;
        gf_log (THIS->name, GF_LOG_WARNING, "Lease removed - %s",
                client->client_uid);
        return 0;
}

int
add_lease (call_frame_t *frame, client_t *client, uuid_t gfid,
           fd_t *fd, gf_boolean_t is_write)
{
        upcall_client *up_client_entry = NULL;
        upcall_entry  *up_entry        = NULL;
        gf_lkowner_t   lk_owner;
        upcall_lease  *up_lease = NULL;

        lk_owner = frame->root->lk_owner;

        if (((upcall_local_t *)frame->local)->client) {
                up_client_entry = ((upcall_local_t *)frame->local)->client;
        } else {
                up_client_entry = get_upcall_client (frame, gfid, client,
                                                           &up_entry);
                if (!up_client_entry) {
                        return -1;
                }
        }

        up_lease = GF_CALLOC (1, sizeof(*up_lease),
                              gf_upcalls_mt_upcall_entry_t);
        if (!up_lease) {
                errno = ENOMEM;
                return -1;
        }
        /* All the conflicts would have been checked before adding lease
         * Assuming within the same client, all the lease lock conflicts will be handled
         * by the applications/glusterfs_clients.
         */

        if (is_write) { /* write_lease */
                up_client_entry->lease = READ_WRITE_LEASE;
        } else {
                up_client_entry->lease = READ_LEASE;
        }

        up_client_entry->access_time = time(NULL);
        up_client_entry->lease_cnt++;
        up_lease->fd = fd;
        up_lease->lkowner = frame->root->lk_owner;
        pthread_mutex_lock (&up_client_entry->upcall_lease_mutex);
        list_add_tail(&up_lease->lease_list,
                      &up_client_entry->lease_entries.lease_list);
        pthread_mutex_unlock (&up_client_entry->upcall_lease_mutex);

        up_entry->lease_cnt++;
        gf_log (THIS->name, GF_LOG_WARNING, "Lease added - %s",
                client->client_uid);
        return 0;
}

/*
 * Given a gfid, client, first fetch upcall_entry based on gfid.
 * Later traverse through the client list of that upcall entry. If this client
 * is not present in the list, create one client entry with this client info.
 * Also check if there are other clients which need to be notified of this
 * op. If yes send notify calls to them.
 *
 * Since sending notifications for cache_invalidation is a best effort, any errors
 * during the process are logged and ignored.
 */
void
upcall_cache_invalidate (call_frame_t *frame, client_t *client, uuid_t gfid, void *extra)
{
        upcall_entry  *up_entry = NULL;
        upcall_client *up_client_entry = NULL;
        upcall_client *up_client = NULL;
        notify_event_data n_event_data;
        time_t t = time(NULL);

        if (((upcall_local_t *)frame->local)->client) {
                up_client = ((upcall_local_t *)frame->local)->client;
                up_entry = get_upcall_entry(gfid);
        } else {
                up_client = get_upcall_client (frame, gfid, client, &up_entry);
                if (!up_client) {
                        gf_log (THIS->name, GF_LOG_WARNING, "Client entry (%s) not found ",
                                client->client_uid);
                        return;
                }
        }

        pthread_mutex_lock (&up_entry->upcall_client_mutex);
        list_for_each_entry (up_client_entry, &up_client->client_list, client_list) {
                if (up_client_entry->client_uid) {
                       if (strcmp(client->client_uid, up_client_entry->client_uid)) {
                                /* any other client */

                                /* TODO: check if that client entry is still valid.
                                 * Notify will fail incase of expired client entries.
                                 * Reaper thread should be clening up such entries.
                                 */
                                if ((t-up_client_entry->access_time) < LEASE_PERIOD) {
                                        /* Send notify call */
                                        uuid_copy(n_event_data.gfid, gfid);
                                        n_event_data.client_entry = up_client_entry;
                                        n_event_data.event_type =  CACHE_INVALIDATION;
                                        n_event_data.extra = extra;
                                        /* Need to send inode flags */
                                        gf_log (THIS->name, GF_LOG_DEBUG,
                                                 "Cache invalidation notification sent to %s",
                                                  up_client_entry->client_uid);
                                        frame->this->notify (frame->this, GF_EVENT_UPCALL,
                                                             &n_event_data);
                                 } else {
                                        gf_log (THIS->name, GF_LOG_DEBUG,
                                                 "Cache invalidation notification NOT sent to %s",
                                                  up_client_entry->client_uid);
                                }
                        }
                }
        }
        pthread_mutex_unlock (&up_entry->upcall_client_mutex);
}
