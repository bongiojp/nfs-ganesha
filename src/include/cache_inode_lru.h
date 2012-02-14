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

#include "log_macros.h"
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

#define LRU_FLAG_NONE          0x0000

/* Flags set on LRU entries */

/* Set on pinned (state-bearing) entries. Do we need both of these? */
#define LRU_ENTRY_PINNED      0x0001
/* Set on LRU entries in the L2 (scanned and colder) queue. */
#define LRU_ENTRY_L2          0x0002

/* Flags for functions in the LRU package */

/* Take a reference on the created entry */
#define LRU_REQ_FLAG_REF       0x0004
/* The caller holds mutex on source (queue from which entry is
   removed) */
#define LRU_HAVE_LOCKED_SRC    0x0008
/* The caller holds mutex on destination (queue to which entry is
   added) */
#define LRU_HAVE_LOCKED_DST    0x0010
/* The caller holds mutex on entry */
#define LRU_HAVE_LOCKED_ENTRY  0x0020
/* The caller is fetching an initial reference */
#define LRU_REQ_INITIAL        0x0040
/* The caller is scanning the entry (READDIR) */
#define LRU_REQ_SCAN           0x0080

/* The minimum reference count for a cache entry not being recycled. */

#define LRU_SENTINEL_REFCOUNT    1

/* The number of lanes comprising a logical queue.  This must be
   prime. */

#define LRU_N_Q_LANES 7

/* LRU_WORK_PER_WAKE * LRU_N_Q_LANES is the number of entries that
   the worker thread will process and move from L1 to L2. */

#define LRU_WORK_PER_WAKE 10

#define LRU_NO_LANE ~0L

extern void cache_inode_lru_pkginit(void);
extern void cache_inode_lru_pkgshutdown(void);

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

/* convenience function to increase entry refcount, permissible
 * IFF the caller has an initial reference */
static inline void cache_inode_ref(cache_entry_t *entry, char *tag)
{
    P(entry->lru.mtx);
    ++(entry->lru.refcount);
    V(entry->lru.mtx);
}

static inline int64_t cache_inode_lru_readref(cache_entry_t *entry)
{
    int64_t cnt;

    P(entry->lru.mtx);
    cnt = entry->lru.refcount;
    V(entry->lru.mtx);

    return (cnt);
}

extern cache_entry_t *cache_inode_lru_get(cache_inode_client_t *pclient,
                                           cache_inode_status_t *pstatus,
                                           uint32_t flags);
extern cache_inode_status_t cache_inode_lru_ref(cache_entry_t *entry,
                                                cache_inode_client_t *pclient,
                                                uint32_t flags);
extern cache_inode_status_t cache_inode_lru_unref(cache_entry_t * entry,
                                                  cache_inode_client_t *pclient,
                                                  uint32_t flags);
extern void lru_wake_thread();

#endif /* _CACHE_INODE_LRU_H */
