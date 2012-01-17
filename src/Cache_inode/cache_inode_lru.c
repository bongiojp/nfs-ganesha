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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef _SOLARIS
#include "solaris_port.h"
#endif                          /* _SOLARIS */

#include "log_macros.h"
#include "stuff_alloc.h"
#include "nlm_list.h"
#include "fsal.h"
#include "cache_inode.h"
#include "cache_inode_lru.h"
#include <unistd.h>
#include <sys/types.h>
#include <sys/param.h>
#include <time.h>
#include <pthread.h>
#include <assert.h>

/**
 *
 * \file cache_inode_lifecycle.c
 * \author Matt Benjamin
 * \brief Constant-time cache inode cache management implementation
 *
 * \section DESCRIPTION
 *
 * This module implements a constant-time cache management strategy
 * based on LRU.  Some ideas are taken from 2Q [Johnson and Shasha 1994]
 * and MQ [Zhou, Chen, Li 2004].  In this system, cache management does
 * interact with cache entry lifecycle, but the lru queue is not a garbage
 * collector. Most imporantly, cache management operations execute in constant
 * time, as expected with LRU (and MQ).
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

/* Cache inode entry lookup table */
static hash_table_t ht_;
static pthread_mutex_t lru_mtx;

/* Cahe inode entry weakref table */
/* static hash_table_t weakref_; */

struct lru_q_
{
    struct glist_head lru; /* LRU is at HEAD, MRU at tail */
    struct glist_head lru_pinned; /* uncollectable, due to state */
};

/* Initially, we implement a single-level cache.  The algorithm here
 * generalizes to multi-level caching algorithm MQ [Zhou].
 */
#define N_LRU_Q 1
static struct lru_q_ lru_q[N_LRU_Q];

/* The refcount mechanism distinguishes 3 key object states:
 *
 * 1. unreferenced (unreachable)
 * 2. unincremented, but reachable
 * 3. incremented
 *
 * It seems most convenient to make unreferenced correspond to refcount==0.
 * Then refcount==1 is a SENTINEL_REFCOUNT in which the only reference to
 * the entry is the set of functions which can grant new references.  An
 * object with refcount > 1 has been referenced by some thread, which must
 * release its reference at some point.
 *
 * Currently, I propose to distinguish between objects with positive refcount
 * and objects with state.  The latter could be evicted, in the normal case,
 * only with loss of protocol correctness, but may have only the sentinel
 * refcount.  To preserve constant time operation, they are stored in an
 * independent partition of the LRU queue.
 */

/* functions:
 * cache_inode_lru_pkginit
 * cache_inode_lru_pkgshutdown
 * cache_inode_ref -- increase refcount
 * cache_inode_unref -- decrement refcount, cond recycle
 * cache_inode_lru_get -- get initial reference, aka, EvictBlock()
 * cache_inode_lru_ref -- increment refcount and adjust LRU
 * cache_inode_lru_unref -- decrement refcount, cond recycle
 *
 * other ideas:
 * weakrefs -- planned for use in dirent
 *
 */

/*
 * Initialize subsystem
 */
void cache_inode_lru_pkginit(void)
{
    int ix;

    pthread_mutex_init(&lru_mtx, NULL);

    for (ix = 0; ix < N_LRU_Q; ++ix) {
        /* init queues at each level */
        init_glist(&lru_q[ix].lru);
        init_glist(&lru_q[ix].lru_pinned);
    }

    /* spawn LRU background thread */
    /* TODO: finish */
}

/*
 * Shutdown subsystem
 */

void cache_inode_lru_pkgshutdown(void)
{
    /* post and wait for shutdown of LRU background thread */
}

#define LRU_GET_FLAG_NONE    0x0000
#define LRU_GET_FLAG_REF     0x0001

/* convenience function to decrease entry refcount, permissible
 * IFF the caller has an initial reference */
void cache_inode_unref(cache_entry_t *entry,
                       cache_inode_client_t *pclient,
                       uint32_t flags)
{
    P(entry->lru.mtx);
    if (--(entry->lru.refcount) == 0) {
        V(entry->lru.mtx);
        P(lru_mtx);
        P(entry->lru.mtx);
        if (entry->lru.refcount > 0) {
            /* won't happen */
            V(entry->lru.mtx);
            V(lru_mtx);
            goto out;
        }
        /* entry is UNREACHABLE -- the call path is recycling entry */
        glist_del(&entry->lru.q);
        V(entry->lru.mtx);
        if (entry->internal_md.type == SYMBOLIC_LINK)
            cache_inode_release_symlink(entry, &pclient->pool_entry_symlink);
        ReleaseToPool(entry, &pclient->pool_entry);
        V(lru_mtx);
        goto out;
    }

    V(entry->lru.mtx);
out:
    return;
}

/*
 * cache_inode_lru_get aka EvictBlock()
 *
 * repurpose a resident entry in the LRU system, if the system is
 * above low-water mark, from pool otherwise.
 */
cache_entry_t * cache_inode_lru_get(cache_inode_client_t *pclient,
                                    cache_inode_status_t *pstatus,
                                    uint32_t flags)
{
    cache_inode_lru_t *lru;
    cache_entry_t *entry = NULL;

    P(lru_mtx);
    /* victim at LRU */
    lru = glist_first_entry(&lru_q[1].lru, cache_inode_lru_t, q);
    entry = container_of(lru, cache_entry_t, lru);
    if (entry && entry->lru.refcount == SENTINEL_REFCOUNT) {
        glist_del(&entry->lru.q);
        glist_add_tail(&lru_q[1].lru, &entry->lru.q); /* MRU */
        V(lru_mtx);

        P(entry->lru.mtx);
        cache_inode_clean_entry(entry);
        if (flags & LRU_GET_FLAG_REF)
            ++(entry->lru.refcount);
    
        V(entry->lru.mtx);

        *pstatus = CACHE_INODE_SUCCESS;
        goto out;
    }

    V(lru_mtx);

    /* if reclaim failed, we may:
     * a. if ! lru_q highwat, get from pool
     * b. if lru_q highwat, we either get from the pool anyway with a warning,
     * or fail the allocation
     */

    GetFromPool(entry, &pclient->pool_entry, cache_entry_t);
    if(entry == NULL) {
        LogCrit(COMPONENT_CACHE_INODE,
                "cache_inode_lru_get: "
                "Can't allocate a new entry from cache pool");
        *pstatus = CACHE_INODE_MALLOC_ERROR;
        goto out;
    }

    entry->lru.refcount = 0;
    if (pthread_mutex_init(&entry->lru.mtx, NULL) != 0) {
        ReleaseToPool(entry, &pclient->pool_entry);
          LogCrit(COMPONENT_CACHE_INODE,
                  "cache_inode_lru_get: pthread_mutex_init of "
                  "lru.mtx returned %d (%s)",
                  errno,
                  strerror(errno));
          *pstatus = CACHE_INODE_INIT_ENTRY_FAILED;
          goto out;
    }

    if (flags & LRU_GET_FLAG_REF)
        ++(entry->lru.refcount);

    P(lru_mtx);
    glist_add_tail(&lru_q[1].lru, &entry->lru.q); /* MRU */
    V(lru_mtx);
        
    *pstatus = CACHE_INODE_SUCCESS;
out:
    return (entry);
}

cache_inode_status_t cache_inode_lru_ref(cache_entry_t * entry,
                                         uint32_t flags)
{
    P(entry->lru.mtx);
    ++(entry->lru.refcount);

    /* adjust LRU */
    P(lru_mtx);
    glist_del(&entry->lru.q);
    glist_add_tail(&lru_q[1].lru, &entry->lru.q); /* MRU */

    V(lru_mtx);
    V(entry->lru.mtx);
}

cache_inode_status_t cache_inode_lru_unref(cache_entry_t * entry,
                                           cache_inode_client_t *pclient,
                                           uint32_t flags)
{
    P(lru_mtx);
    P(entry->lru.mtx);

    assert(entry->lru.refcount >= 1);

    if (--(entry->lru.refcount) == 0) {
        /* entry is UNREACHABLE -- the call path is recycling entry */
        glist_del(&entry->lru.q);
        V(entry->lru.mtx);
        if (entry->internal_md.type == SYMBOLIC_LINK)
            cache_inode_release_symlink(entry, &pclient->pool_entry_symlink);
        ReleaseToPool(entry, &pclient->pool_entry);
        goto unlock;
    }

    /* no LRU adjustment */
    V(entry->lru.mtx);
 
unlock:   
    V(lru_mtx);
}


