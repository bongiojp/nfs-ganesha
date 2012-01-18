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

#include <unistd.h>
#include <sys/types.h>
#include <sys/param.h>
#include <time.h>
#include <pthread.h>
#include <assert.h>
#include "stuff_alloc.h"
#include "nlm_list.h"
#include "fsal.h"
#include "nfs_core.h"
#include "log_macros.h"
#include "cache_inode.h"
#include "cache_inode_lru.h"

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

/* Cahe inode entry weakref table */
/* static hash_table_t weakref_; */

struct lru_q_pair
{
    struct glist_head q; /* LRU is at HEAD, MRU at tail */
    uint64_t size;
};

struct lru_q_
{
    struct lru_q_pair lru;
    struct lru_q_pair lru_pinned; /* uncollectable, due to state */
};

/* Initially, we implement a single-level cache.  The algorithm here
 * generalizes to multi-level caching algorithm MQ [Zhou].
 */
#define N_LRU_Q 1
static struct lru_q_ lru_q[N_LRU_Q];

static pthread_mutex_t lru_mtx;
static pthread_cond_t lru_cv;

#define LRU_SLEEPING 0x0001
#define LRU_SHUTDOWN 0x0002

static struct lru_thread_state
{
    pthread_t thread_id;
    uint32_t flags;
} lru_thread_state;


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
void cache_inode_lru_pkginit(pthread_attr_t *attr_thr)
{
    int ix, code = 0;

    pthread_mutex_init(&lru_mtx, NULL);
    pthread_cond_init(&lru_cv, NULL);

    for (ix = 0; ix < N_LRU_Q; ++ix) {

        /* init queues at each level */
        init_glist(&lru_q[ix].lru.q);
        lru_q[ix].lru.size = 0;

        init_glist(&lru_q[ix].lru_pinned.q);
        lru_q[ix].lru_pinned.size = 0;

    }

    /* spawn LRU background thread */
    code = pthread_create(&lru_thread_state.thread_id, attr_thr, lru_thread,
                          (void *) NULL);
    assert(code == 0);
}

/*
 * Shutdown subsystem
 */

void cache_inode_lru_pkgshutdown(void)
{
    /* post and wait for shutdown of LRU background thread */
}

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

    struct lru_q_pair *qp = &lru_q[1].lru;


    P(lru_mtx);
    /* victim at LRU */
    lru = glist_first_entry(&qp->q, cache_inode_lru_t, q);
    entry = container_of(lru, cache_entry_t, lru);
    if (entry && entry->lru.refcount == SENTINEL_REFCOUNT) {
        glist_del(&entry->lru.q);
        glist_add_tail(&qp->q, &entry->lru.q); /* MRU */
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

    entry->lru.flags = LRU_FLAG_Q_LRU; /* queue partition */
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
    glist_add_tail(&qp->q, &entry->lru.q); /* MRU */
    (qp->size)++;
    V(lru_mtx);
        
    *pstatus = CACHE_INODE_SUCCESS;
out:
    return (entry);
}

cache_inode_status_t cache_inode_lru_ref(cache_entry_t * entry,
                                         uint32_t flags)
{
    struct lru_q_pair *qp;

    /* queue partition */
    if (entry->lru.flags & LRU_FLAG_Q_LRU)
        qp = &lru_q[1].lru;
    else
        qp = &lru_q[1].lru_pinned;

    P(entry->lru.mtx);
    ++(entry->lru.refcount);

    /* adjust LRU */
    P(lru_mtx);
    glist_del(&entry->lru.q);
    glist_add_tail(&qp->q, &entry->lru.q); /* MRU */

    V(lru_mtx);
    V(entry->lru.mtx);

    return (CACHE_INODE_SUCCESS);
}

cache_inode_status_t cache_inode_lru_unref(cache_entry_t * entry,
                                           cache_inode_client_t *pclient,
                                           uint32_t flags)
{
    struct lru_q_pair *qp;

    /* queue partition */
    if (entry->lru.flags & LRU_FLAG_Q_LRU)
        qp = &lru_q[1].lru;
    else
        qp = &lru_q[1].lru_pinned;

    P(lru_mtx);
    P(entry->lru.mtx);

    assert(entry->lru.refcount >= 1);

    if (--(entry->lru.refcount) == 0) {
        /* entry is UNREACHABLE -- the call path is recycling entry */
        glist_del(&entry->lru.q);
        (qp->size)--;
   
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

    return (CACHE_INODE_SUCCESS);
}

#define S_NSECS 1000000000UL	/* nsecs in 1s */
#define MS_NSECS 1000000UL	/* nsecs in 1ms */

static void
lru_thread_delay_ms(unsigned long ms)
{
    time_t now;
    struct timespec then;
    unsigned long long nsecs;

    now = time(0);
    nsecs = (S_NSECS * now) + (MS_NSECS * ms);
    then.tv_sec = nsecs / S_NSECS;
    then.tv_nsec = nsecs % S_NSECS;
    
    pthread_mutex_lock(&lru_mtx);
    pthread_cond_timedwait(&lru_cv, &lru_mtx, &then);
    pthread_mutex_unlock(&lru_mtx);
}

/* Async thread to perform long-term reorganization, compaction,
 * other operations that cannot be performed in constant time. */
void *lru_thread(void *arg)
{
    SetNameFunction("lru_thread");

    while (1) {

        if (lru_thread_state.flags & LRU_SHUTDOWN)
            break;

        LogCrit(COMPONENT_CACHE_INODE,
                "%s: top of poll loop", __func__);


        /* do stuff */

        lru_thread_delay_ms(1000 * 5);
    }

    LogCrit(COMPONENT_CACHE_INODE,
            "%s: shutdown", __func__);

    return (NULL);
}

void wakeup_lru_thread() {
    P(lru_mtx);
    if (lru_thread_state.flags & LRU_SLEEPING)
        pthread_cond_signal(&lru_cv);
    V(lru_mtx);
}
