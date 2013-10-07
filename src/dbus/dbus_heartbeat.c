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
  int frequency_ms;
  uint32_t flags;
};

static struct _dbus_heartbeat_thread_state heartbeat_thread_state;

void dbus_heartbeat_init(void)
{
  gsh_dbus_register_path(HEARTBEAT_PATH, heartbeat_interfaces);
  heartbeat_thread_state.frequency_ms = 1000; /* milliseconds */
  heartbeat_thread_state.flags = DBUS_HEARTBEAT_PULSING;  
}

void *dbus_heartbeat_thread(void *arg)
{
    SetNameFunction("dbus_heartbeat");
    char *message = "HEARTBEAT PULSE";
    int err = 0;

    while (1) {
      LogFullDebug(COMPONENT_DBUS, "heartbeat sleeping");
      usleep(heartbeat_thread_state.frequency_ms*1000);
      if (heartbeat_thread_state.flags & DBUS_HEARTBEAT_SHUTDOWN)
	break;

      /* send a heartbeat pulse */
      err = gsh_dbus_broadcast(HEARTBEAT_PATH,
			       HEARTBEAT_IFACE,
			       HEARTBEAT_NAME,
			       message);
      if (err) {
	LogCrit(COMPONENT_DBUS,
		"heartbeat broadcast failed. err:%d", err);
	break;
      }
    } /* 1 */

    LogCrit(COMPONENT_DBUS, "dbus heartbeat shutdown");
    return (NULL);
}
