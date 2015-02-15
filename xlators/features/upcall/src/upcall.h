/*
   Copyright (c) 2006-2014 Red Hat, Inc. <http://www.redhat.com>
   This file is part of GlusterFS.

   This file is licensed to you under your choice of the GNU Lesser
   General Public License, version 3 or any later version (LGPLv3 or
   later), or the GNU General Public License, version 2 (GPLv2), in all
   cases as published by the Free Software Foundation.
*/
#ifndef __UPCALL_INFRA_H__
#define __UPCALL_INFRA_H__

#ifndef _CONFIG_H
#define _CONFIG_H
#include "config.h"
#endif

#include "compat-errno.h"
#include "stack.h"
#include "call-stub.h"
#include "upcall-mem-types.h"
#include "client_t.h"
#include "rpcsvc.h"
#include "lkowner.h"

/* The time period for which a client will be notified of cache_invalidation
 * events post its last access */
#define CACHE_INVALIDATE_PERIOD 60

/* Flags sent for cache_invalidation */
#define UP_NLINK   0x00000001   /* update nlink */
#define UP_MODE    0x00000002   /* update mode and ctime */
#define UP_OWN     0x00000004   /* update mode,uid,gid and ctime */
#define UP_SIZE    0x00000008   /* update fsize */
#define UP_TIMES   0x00000010   /* update all times */
#define UP_ATIME   0x00000020   /* update atime only */
#define UP_PERM    0x00000040   /* update fields needed for
                                   permission checking */
#define UP_RENAME  0x00000080   /* this is a rename op -
                                   delete the cache entry */


#define UPCALL_STACK_UNWIND(fop, frame, params ...) do {        \
        upcall_local_t *__local = NULL;                         \
        xlator_t *__xl          = NULL;                         \
        if (frame) {                                            \
                        __xl         = frame->this;             \
                        __local      = frame->local;            \
                        frame->local = NULL;                    \
                }                                               \
                STACK_UNWIND_STRICT (fop, frame, params);       \
                upcall_local_wipe (__xl, __local);              \
        } while (0)

#define UPCALL_STACK_DESTROY(frame) do {                   \
                upcall_local_t *__local = NULL;            \
                xlator_t    *__xl    = NULL;               \
                __xl                 = frame->this;        \
                __local              = frame->local;       \
                frame->local         = NULL;               \
                STACK_DESTROY (frame->root);               \
                upcall_local_wipe (__xl, __local);         \
        } while (0)

struct _upcalls_private_t {
	int client_id; /* Not sure if reqd */
};
typedef struct _upcalls_private_t upcalls_private_t;

enum upcall_event_type_t {
        EVENT_NULL,
        CACHE_INVALIDATION,
};
typedef enum upcall_event_type_t upcall_event_type;

struct _upcall_client_t {
        struct list_head client_list;
        /* strdup to store client_uid, strdup. Free it explicitly */
        char *client_uid;
        time_t access_time; /* time last accessed */
};
typedef struct _upcall_client_t upcall_client;

struct _upcall_entry_t {
        struct list_head list;
        uuid_t gfid;
        upcall_client client; /* list of clients */
        pthread_mutex_t upcall_client_mutex; /* mutex for clients list
                                                of this upcall entry */
};
typedef struct _upcall_entry_t upcall_entry;

/* For now lets maintain a linked list of upcall entries.
 * But for faster lookup we should be switching any of the
 * tree data structures
 */
upcall_entry upcall_global_list;
pthread_mutex_t upcall_global_list_mutex; /* mutex for upcall entries list */

struct _notify_event_data {
        uuid_t gfid;
        upcall_client *client_entry;
        upcall_event_type event_type;
        /* any extra data needed, like inode flags
         * to be invalidated incase of cache invalidation,
         * may be fd for lease recalls */
        void *extra;
};
typedef struct _notify_event_data notify_event_data;

struct upcall_local {
        upcall_client *client;
        upcall_entry *up_entry;
        uuid_t   gfid;
        fd_t *fd;
};
typedef struct upcall_local upcall_local_t;

void upcall_local_wipe (xlator_t *this, upcall_local_t *local);
upcall_local_t *upcall_local_init (call_frame_t *frame, uuid_t gfid);

upcall_entry *add_upcall_entry (uuid_t gfid);
upcall_entry *get_upcall_entry (uuid_t gfid);
upcall_client *add_upcall_client (call_frame_t *frame, uuid_t gfid,
                                  client_t *client, upcall_entry **up_entry);
upcall_client *get_upcall_client (call_frame_t *frame, uuid_t gfid,
                                  client_t *client, upcall_entry **up_entry);
void upcall_cache_invalidate (call_frame_t *frame, client_t *client,
                              uuid_t gfid, void *extra);

#endif /* __UPCALL_INFRA_H__ */
