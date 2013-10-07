/*
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) IBM Inc., 2013
 * Author: Jeremy Bongio <jbongio@us.ibm.com>
 *
 * contributeur : Philippe DENIEL   philippe.deniel@cea.fr
 *                Thomas LEIBOVICI  thomas.leibovici@cea.fr
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 * -------------
 */

/**
 * @defgroup DBUS Heartbeat
 * @{
 */

/**
 * @file dbus_heartbeat.c
 * @author Jeremy Bongio <jbongio@us.ibm.com>
 * @brief DBUS Heartbeat
 */

#include "config.h"

#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/param.h>
#include <pthread.h>
#include <assert.h>
#include <arpa/inet.h>
#include "nlm_list.h"
#include "fsal.h"
#include "nfs_core.h"
#include "log.h"
#include "avltree.h"
#include "ganesha_types.h"
#ifdef USE_DBUS_STATS
#include "ganesha_dbus.h"
#endif
#include "abstract_atomic.h"
#include "gsh_intrinsic.h"

static struct gsh_dbus_interface *heartbeat_interfaces[] = {
  NULL
};

#define HEARTBEAT_PATH  "/org/ganesha/nfsd/heartbeat"
#define HEARTBEAT_IFACE "org.ganesha.nfsd.heartbeat"
#define HEARTBEAT_NAME  "heartbeat"

#define DBUS_HEARTBEAT_NONE     0x0000
#define DBUS_HEARTBEAT_SHUTDOWN 0x0001
#define DBUS_HEARTBEAT_PULSING  0x0002

struct _dbus_heartbeat_thread_state
{
  DBusConnection* dbus_conn;
  DBusError dbus_err;
  int frequency_ms;
  uint32_t flags;
  bool enabled;
};

static struct _dbus_heartbeat_thread_state hb_thread_state;

void dbus_heartbeat_init(struct dbus_param pparam)
{
  char regbuf[128];
  int code = 0;
  
  if (pparam.heartbeat == false) {
    hb_thread_state.dbus_conn = NULL;
    hb_thread_state.enabled = false;
    return;
  }

  hb_thread_state.enabled = true;
  gsh_dbus_register_path(HEARTBEAT_PATH, heartbeat_interfaces);
  hb_thread_state.frequency_ms = pparam.heartbeat_freq; /* milliseconds */
  hb_thread_state.flags = DBUS_HEARTBEAT_PULSING;  

  dbus_error_init(&hb_thread_state.dbus_err);
  hb_thread_state.dbus_conn = dbus_bus_get_private(DBUS_BUS_SYSTEM,
                                                   &hb_thread_state.dbus_err);
  if (dbus_error_is_set(&hb_thread_state.dbus_err)) {
    LogCrit(COMPONENT_DBUS,
            "dbus_bus_get failed (%s)", hb_thread_state.dbus_err.message);
    dbus_error_free(&hb_thread_state.dbus_err);
    return;
  }

  snprintf(regbuf, 128, HEARTBEAT_IFACE);
  code = dbus_bus_request_name(hb_thread_state.dbus_conn, regbuf,
                               DBUS_NAME_FLAG_REPLACE_EXISTING ,
                               &hb_thread_state.dbus_err);
  if (dbus_error_is_set(&hb_thread_state.dbus_err)) { 
    LogCrit(COMPONENT_DBUS, "server bus reg failed (%s, %s)", regbuf,
            hb_thread_state.dbus_err.message); 
    dbus_error_free(&hb_thread_state.dbus_err);
    if (! code)
      code = EINVAL;
    return;
  }  

  if (code != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
    LogCrit(COMPONENT_DBUS, "server failed becoming primary bus owner "
            "(%s, %d)", regbuf, code);
    return;
  }
}

struct _ganesha_health
{
  int old_enqueue;
  int old_dequeue;
  int enqueue_diff;
  int dequeue_diff;
  int ishealthy;
};

void get_ganesha_health(struct _ganesha_health *healthstats)
{
  int newenq, newdeq;

  newenq = get_enqueue_count();
  newdeq = get_dequeue_count();
  healthstats->enqueue_diff = newenq - healthstats->old_enqueue;
  healthstats->dequeue_diff = newdeq - healthstats->old_dequeue;
  healthstats->old_enqueue = newenq;
  healthstats->old_dequeue = newdeq;

  /* health state indicates if we are making progress draining the
   * request queue. */
  healthstats->ishealthy =
    ((healthstats->enqueue_diff > 0 && healthstats->dequeue_diff > 0)
     || (healthstats->enqueue_diff == 0 && healthstats->dequeue_diff == 0));
}

int gsh_dbus_broadcast(DBusConnection *conn, char *obj_name, char *int_name,
                       char *sig_name, char *message) {
  static dbus_uint32_t serial = 0;
  DBusMessage *msg;
  DBusMessageIter sig_iter;

  msg = dbus_message_new_signal(obj_name, int_name, sig_name);
  if(msg == NULL)
    return EINVAL;

  dbus_message_iter_init_append(msg, &sig_iter);
  if (!dbus_message_iter_append_basic(&sig_iter, DBUS_TYPE_STRING, &message))
    return ENOMEM;

  if (!dbus_connection_send(conn, msg, &serial))
    return ENOMEM;

  /* flush to send it now. */
  dbus_connection_flush(conn);
  dbus_message_unref(msg);

  return 0;
}

void *dbus_heartbeat_thread(void *arg)
{
    SetNameFunction("dbus_heartbeat");
    char message[256];
    struct _ganesha_health healthstats;
    int err = 0;

    if (! hb_thread_state.enabled)
      return (NULL);

    if (! hb_thread_state.dbus_conn) {
      LogCrit(COMPONENT_DBUS, "DBUS not initialized, DBUS heartbeat thread "
              "exiting");
      return (NULL);
    }

    healthstats.old_enqueue = 0;
    healthstats.old_dequeue = 0;

    while (1) {
      LogFullDebug(COMPONENT_DBUS, "heartbeat sleeping");
      usleep(hb_thread_state.frequency_ms*1000);
      if (hb_thread_state.flags & DBUS_HEARTBEAT_SHUTDOWN)
        break;

      get_ganesha_health(&healthstats);
      sprintf(message, "HEARTBEAT: \nnewly queued requests: %u\nnewly dequeued"
              " requests: %d\nisHealthy: %d",
              healthstats.enqueue_diff, healthstats.dequeue_diff,
              healthstats.ishealthy);

      /* send the heartbeat pulse */
      err = gsh_dbus_broadcast(hb_thread_state.dbus_conn, HEARTBEAT_PATH,
                               HEARTBEAT_IFACE, HEARTBEAT_NAME, message);
      if (err) {
        LogCrit(COMPONENT_DBUS, "heartbeat broadcast failed. err:%d", err);
        break;
      }
    } /* 1 */

    LogCrit(COMPONENT_DBUS, "dbus heartbeat shutdown");
    return (NULL);
}
