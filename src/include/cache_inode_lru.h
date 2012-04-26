/*
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2012, The Linux Box Corporation
 * Contributor : Matt Benjamin <matt@linuxbox.com>
 *
 * Some portions Copyright CEA/DAM/DIF  (2008)
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * -------------
 */

#ifndef _CACHE_INODE_LRU_H
#define _CACHE_INODE_LRU_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif                          /* HAVE_CONFIG_H */

#ifdef _SOLARIS
#include "solaris_port.h"
#endif                          /* _SOLARIS */

#include "log.h"
#include "cache_inode.h"

/**
 *
 * \file cache_inode_lru.h
 * \author Matt Benjamin
 * \brief Constant-time cache inode cache management implementation
 *
 * \section DESCRIPTION
 *
 * This module implements a constant-time cache management strategy
 * based on LRU.  Some ideas are taken from 2Q [Johnson and Shasha 1994]
 * and MQ [Zhou, Chen, Li 2004].  In this system, cache management does
 * interact with cache entry lifecycle.  Also, the cache size high- and
 * low- water mark management is maintained, but executes asynchronously
 * to avoid inline request delay.  Cache management operations execute in
 * constant time, as expected with LRU (and MQ).
 *
 * Cache entries in use by a currently-active protocol request (or other
 * operation) have a positive refcount, and threfore should not be present
 * at the cold end of an lru queue if the cache is well-sized.
 *
 * Cache entries with lock and open state are not eligible for collection
 * under ordinary circumstances, so are kept on a separate lru_pinned
 * list to retain constant time.
 *
 */

struct lru_state
{
     uint64_t entries_hiwat;
     uint64_t entries_lowat;
     uint32_t fds_system_imposed;
     uint32_t fds_hard_limit;
     uint32_t fds_hiwat;
     uint32_t fds_lowat;
     /* This is the actual counter of 'futile' attempts at reaping
        made  in a given time period.  When it reaches the futility
        count, we turn off caching of file descriptors. */
     uint32_t futility;
     uint32_t per_lane_work;
     uint32_t biggest_window;
     uint32_t flags;
     uint64_t last_count;
     uint64_t threadwait;
     bool_t caching_fds;
};

extern struct lru_state lru_state;

/* Externally valid flags for functions in the LRU package */

/**
 * No flag at all.
 */

static const uint32_t LRU_FLAG_NONE = 0x0000;

/**
 * The caller is fetching an initial reference
 */
static const uint32_t LRU_REQ_INITIAL = 0x0040;
/**
 * The caller is scanning the entry (READDIR)
 */
static const uint32_t LRU_REQ_SCAN = 0x0080;

/* The minimum reference count for a cache entry not being recycled. */

static const int32_t LRU_SENTINEL_REFCOUNT = 1;

/* The number of lanes comprising a logical queue.  This must be
   prime. */

#define LRU_N_Q_LANES 7

static const uint32_t LRU_NO_LANE = ~0;

extern void cache_inode_lru_pkginit(void);
extern void cache_inode_lru_pkgshutdown(void);

extern size_t open_fd_count;

/* Return an integral id associated with the thread.  Currently we
   just return the index, since it is known to be integral and unique
   within ganesha.  We could return worker_thrid[index] if we wanted. */

static inline uint64_t cache_inode_lru_thread_id(int index)
{
     return (uint64_t) index;
}

/* For a given thread, this function returns a lane within the
   logical LRU queue upon which this thread should operate. */

static inline uint32_t cache_inode_lru_thread_lane(int index)
{
    return (uint32_t) (cache_inode_lru_thread_id(index) %
                       LRU_N_Q_LANES);
}

extern struct cache_entry_t *cache_inode_lru_get(cache_inode_client_t *pclient,
                                                 cache_inode_status_t *pstatus,
                                                 uint32_t flags);
extern cache_inode_status_t cache_inode_lru_ref(
     cache_entry_t *entry,
     cache_inode_client_t *pclient,
     uint32_t flags) __attribute__((warn_unused_result));
extern cache_inode_status_t cache_inode_lru_unref(
     cache_entry_t * entry,
     cache_inode_client_t *pclient,
     uint32_t flags);
extern void lru_wake_thread();
cache_inode_status_t cache_inode_inc_pin_ref(cache_entry_t *entry);
cache_inode_status_t cache_inode_dec_pin_ref(cache_entry_t *entry);

/**
 * Return TRUE if there are FDs available to serve open requests,
 * FALSE otherwise.  This function also wakes the LRU thread if the
 * current FD count is above the high water mark.
 */

static inline bool_t
cache_inode_lru_fds_available(void)
{
     if (open_fd_count >= lru_state.fds_hard_limit) {
          LogCrit(COMPONENT_CACHE_INODE_LRU,
                  "FD Hard Limit Exceeded.  Disabling FD Cache and waking"
                  " LRU thread.");
          lru_state.caching_fds = FALSE;
          lru_wake_thread();
          return FALSE;
     }
     if (open_fd_count >= lru_state.fds_hiwat) {
          LogInfo(COMPONENT_CACHE_INODE_LRU,
                  "FDs above high water mark, waking LRU thread.");
          lru_wake_thread();
     }

     return TRUE;
}

/**
 * Return true if we are currently caching file descriptors.
 */

static inline bool_t
cache_inode_lru_caching_fds(void)
{
     return lru_state.caching_fds;
}
#endif /* _CACHE_INODE_LRU_H */
