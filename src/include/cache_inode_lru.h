/*
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2010, The Linux Box Corporation
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
#define LRU_GET_FLAG_REF       0x0001
#define LRU_FLAG_Q_LRU         0x0002
#define LRU_FLAG_Q_PINNED      0x0004
#define LRU_FLAG_LOCKED        0x0008

#define SENTINEL_REFCOUNT    1

extern void cache_inode_lru_pkginit(void);
extern void cache_inode_lru_pkgshutdown(void);

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

extern cache_entry_t * cache_inode_lru_get(cache_inode_client_t *pclient,
                                           cache_inode_status_t *pstatus,
                                           uint32_t flags);
extern cache_inode_status_t cache_inode_lru_ref(cache_entry_t * entry,
                                                uint32_t flags, char *tag);
extern cache_inode_status_t cache_inode_lru_unref(cache_entry_t * entry,
                                                  cache_inode_client_t *pclient,
                                                  uint32_t flags, char *tag);
extern void *lru_thread(void *arg);
extern void wakeup_lru_thread();

#endif /* _CACHE_INODE_LRU_H */
