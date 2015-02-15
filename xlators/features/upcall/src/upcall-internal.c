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

/*
 * Create an upcall entry given a gfid
 */
upcall_entry*
add_upcall_entry (uuid_t gfid)
{
        upcall_entry *up_entry = NULL;

        up_entry = GF_CALLOC (1, sizeof(*up_entry),
                              gf_upcalls_mt_upcall_entry_t);
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
        up_client_entry->access_time = time(NULL);

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
        list_for_each_entry (up_client_entry, &up_client->client_list,
                             client_list) {
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
 * Later traverse through the client list of that upcall entry. If this client
 * is not present in the list, create one client entry with this client info.
 * Also check if there are other clients which need to be notified of this
 * op. If yes send notify calls to them.
 *
 * Since sending notifications for cache_invalidation is a best effort,
 * any errors during the process are logged and ignored.
 */
void
upcall_cache_invalidate (call_frame_t *frame, client_t *client,
                         uuid_t gfid, void *extra)
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
                        gf_log (THIS->name, GF_LOG_WARNING,
                                "Client entry (%s) not found ",
                                client->client_uid);
                        return;
                }
        }

        pthread_mutex_lock (&up_entry->upcall_client_mutex);
        list_for_each_entry (up_client_entry, &up_client->client_list,
                             client_list) {
                if (up_client_entry->client_uid) {
                                        gf_log (THIS->name, GF_LOG_INFO,
                                                 "In Cache invalidation "
                                                 " client %s",
                                                  up_client_entry->client_uid);
                       if (strcmp(client->client_uid,
                                  up_client_entry->client_uid)) {
                                /* any other client */

                                if ((t-up_client_entry->access_time) <
                                     CACHE_INVALIDATE_PERIOD) {
                                        /* Send notify call */
                                        uuid_copy(n_event_data.gfid, gfid);
                                        n_event_data.client_entry =
                                                            up_client_entry;
                                        n_event_data.event_type =
                                                             CACHE_INVALIDATION;
                                        n_event_data.extra = extra;

                                        /* Need to send inode flags */
                                        gf_log (THIS->name, GF_LOG_DEBUG,
                                                 "Cache invalidation "
                                                 "notification sent to %s",
                                                  up_client_entry->client_uid);
                                        frame->this->notify (frame->this,
                                                             GF_EVENT_UPCALL,
                                                             &n_event_data);
                                 } else {
                                        gf_log (THIS->name, GF_LOG_DEBUG,
                                                 "Cache invalidation "
                                                 "notification NOT sent to %s",
                                                  up_client_entry->client_uid);
                                }
                        }
                }
        }
        pthread_mutex_unlock (&up_entry->upcall_client_mutex);
}
