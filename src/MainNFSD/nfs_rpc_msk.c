/*
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright CEA/DAM/DIF  (2012)
 * contributeur : Philippe DENIEL   philippe.deniel@cea.fr
 *                Dominique MARTINET dominique.martinet.ocre@cea.fr
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * ---------------------------------------
 */

/**
 * \file    nfs_rpc_msk.c
 * \author  $Author: Dominique Martinet $
 * \date    $Date: 2012/08/31 12:33:05 $
 * \version $Revision: 0.1 $
 * \brief   The file that contain the 'rpc_msk_dispatcher_thread' routine for the nfsd.
 *
 * nfs_rpc_msk.c : The file that contain the 'rpc_msk_dispatcher_thread' routine for the nfsd (and all
 * the related stuff).
 *
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef _SOLARIS
#include "solaris_port.h"
#endif

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/file.h>           /* for having FNDELAY */
#include <sys/select.h>
#include <poll.h>
#include <assert.h>
#include "HashData.h"
#include "HashTable.h"
#include "log.h"
#include "ganesha_rpc.h"
#include "nfs23.h"
#include "nfs4.h"
#include "mount.h"
#include "nlm4.h"
#include "rquota.h"
#include "nfs_init.h"
#include "nfs_core.h"
#include "cache_inode.h"
#include "nfs_exports.h"
#include "nfs_creds.h"
#include "nfs_proto_functions.h"
#include "nfs_dupreq.h"
#include "nfs_file_handle.h"
#include "nfs_stat.h"
#include "SemN.h"
#include "nfs_tcb.h"
#include <mooshika.h>


void nfs_msk_callback_disconnect(msk_trans_t *trans) {

}

process_status_t dispatch_rpc_request(SVCXPRT *xprt);

struct clx {
  pthread_mutex_t lock;
  pthread_cond_t cond;
  SVCXPRT *xprt;
};

void nfs_msk_callback(void* arg) {
  struct clx *clx = arg;
  pthread_mutex_lock(&clx->lock);
  if(SVC_STAT(clx->xprt) == XPRT_IDLE)
    dispatch_rpc_request(clx->xprt);
  pthread_cond_signal(&clx->cond);
  pthread_mutex_unlock(&clx->lock);
}

void* nfs_msk_thread(void* arg) {
  msk_trans_t *trans = arg;
  SVCXPRT* xprt;
  struct clx clx;
// alloc onfsreq

  if( trans == NULL ) {
    LogMajor( COMPONENT_NFS_MSK, "NFS/RDMA: handle thread started but no child_trans" );
    return NULL;
  }

  pthread_cond_init(&clx.cond, NULL);
  pthread_mutex_init(&clx.lock, NULL);

  pthread_mutex_lock(&clx.lock);

  xprt = svc_msk_create(trans, 10, nfs_msk_callback, &clx);
  clx.xprt = xprt;

  /* It's still safe to set stuff here that will be used in dispatch_rpc_request because of the lock */
  xprt->xp_u1 = alloc_gsh_xprt_private(XPRT_PRIVATE_FLAG_REF);
  xprt->xp_fd = -1; /* fixme: put something here, but make it not work on fd operations... */

  /*while(trans->state == MSK_CONNECTED) { *//* use SVC_STAT(msk_xprt) */
  while(SVC_STAT(xprt) == XPRT_IDLE) {
    pthread_cond_wait(&clx.cond, &clx.lock);
  }
  pthread_mutex_unlock(&clx.lock);

  return NULL;
}

void* nfs_msk_dispatcher_thread(void* nullarg) {
  msk_trans_t *trans;        /* connection main trans */
  msk_trans_t *child_trans;  /* child trans */
  pthread_attr_t attr_thr;
  int rc = 0;
  msk_trans_attr_t trans_attr;
  pthread_t thrid_handle_trans;

  memset(&trans_attr, 0, sizeof(trans_attr));
  trans_attr.server = 10;
  trans_attr.rq_depth = 12;
  trans_attr.sq_depth = 10;
  trans_attr.addr.sa_in.sin_family = AF_INET;
  trans_attr.addr.sa_in.sin_port = htons(nfs_param.nfs_msk_param.nfs_msk_port);
  trans_attr.addr.sa_in.sin_addr.s_addr = nfs_param.core_param.bind_addr.sin_addr.s_addr; /* change to sa6 + .sin6_addr   = in6 */
  trans_attr.disconnect_callback = nfs_msk_callback_disconnect;

  /* Init for thread parameter (mostly for scheduling) */
  if(pthread_attr_init(&attr_thr) != 0)
    LogDebug( COMPONENT_NFS_MSK, "can't init pthread's attributes" );
                                
  if(pthread_attr_setscope(&attr_thr, PTHREAD_SCOPE_SYSTEM) != 0)
    LogDebug( COMPONENT_NFS_MSK, "can't set pthread's scope" );
                                    
  if(pthread_attr_setdetachstate(&attr_thr, PTHREAD_CREATE_JOINABLE) != 0)
    LogDebug( COMPONENT_NFS_MSK, "can't set pthread's join state" );

  if(pthread_attr_setstacksize(&attr_thr, THREAD_STACK_SIZE) != 0)
    LogDebug( COMPONENT_NFS_MSK, "can't set pthread's stack size" );

  /* Init RDMA via mooshika */
  if( msk_init( &trans, &trans_attr ) )
    LogFatal( COMPONENT_NFS_MSK, "9P/RDMA dispatcher could not start mooshika engine" );
  else
    LogEvent( COMPONENT_NFS_MSK, "Mooshika engine is started" );

  /* Bind Mooshika */
  if( msk_bind_server(trans) )
    LogFatal( COMPONENT_NFS_MSK, "9P/RDMA dispatcher could not bind mooshika engine" );
  else
    LogEvent( COMPONENT_NFS_MSK, "Mooshika engine is bound" );


  while(1) {
    if( ( child_trans = msk_accept_one(trans) ) == NULL )
      LogMajor( COMPONENT_NFS_MSK, "NFS/RDMA: dispatcher failed to accept a new client" );
    else {
      LogDebug( COMPONENT_NFS_MSK, "Got a new connection, spawning a polling thread" );
      if((rc = pthread_create(&thrid_handle_trans, &attr_thr, nfs_msk_thread, child_trans)))
        LogMajor( COMPONENT_NFS_MSK, "NFS/RDMA: dipatcher accepted a new client but could not spawn a related thread" );
      else
        LogEvent( COMPONENT_NFS_MSK, "NFS/RDMA: thread %u spawned to manage a new child_trans",
                  (unsigned int)thrid_handle_trans );
    }
  } /* while(1) */

  return NULL;
} /* nfs_msk_dispatched_thread */


int Init_nfs_msk( nfs_msk_parameter_t *pparam )
{
  return 0;
}
