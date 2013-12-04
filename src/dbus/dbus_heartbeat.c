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

#define HEARTBEAT_PATH  "/org/ganesha/nfsd/heartbeat"
#define HEARTBEAT_IFACE "org.ganesha.nfsd.heartbeat"
#define HEARTBEAT_NAME  "heartbeat"

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

void *dbus_heartbeat_thread(void *arg)
{
    SetNameFunction("dbus_heartbeat");
    char message[256];
    struct _ganesha_health healthstats;
    int err = 0;

    healthstats.old_enqueue = 0;
    healthstats.old_dequeue = 0;

    while (1) {
      LogFullDebug(COMPONENT_DBUS, "heartbeat sleeping %dms",
                   nfs_param.dbus_param.heartbeat_freq);
      usleep(nfs_param.dbus_param.heartbeat_freq*1000);

      get_ganesha_health(&healthstats);
      sprintf(message, "HEARTBEAT: \nnewly queued requests: %u\nnewly dequeued"
              " requests: %d\nisHealthy: %d",
              healthstats.enqueue_diff, healthstats.dequeue_diff,
              healthstats.ishealthy);

      /* send the heartbeat pulse */
      err = gsh_dbus_broadcast(HEARTBEAT_PATH, HEARTBEAT_IFACE, HEARTBEAT_NAME,
                               message);
      if (err) {
        LogCrit(COMPONENT_DBUS, "heartbeat broadcast failed. err:%d", err);
        break;
      }
    } /* 1 */

    LogCrit(COMPONENT_DBUS, "dbus heartbeat shutdown");
    return (NULL);
}
