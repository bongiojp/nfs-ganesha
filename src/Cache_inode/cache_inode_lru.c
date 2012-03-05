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
 * \file cache_inode_lru.c
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

static void *lru_thread(void *arg);

struct lru_q_base
{
     struct glist_head q; /* LRU is at HEAD, MRU at tail */
     pthread_mutex_t mtx;
     uint64_t size;
};

/* Cache-line padding macro from MCAS */

#define CACHE_LINE_SIZE 64 /* XXX arch-specific define */
#define CACHE_PAD(_n) char __pad ## _n [CACHE_LINE_SIZE]
#define ALIGNED_ALLOC(_s)                                       \
     ((void *)(((unsigned long)malloc((_s)+CACHE_LINE_SIZE*2) + \
                CACHE_LINE_SIZE - 1) & ~(CACHE_LINE_SIZE-1)))

struct lru_q_
{
     struct lru_q_base lru;
     struct lru_q_base lru_pinned; /* uncollectable, due to state */
     CACHE_PAD(0);
};

/* A multi-level LRU algorithm inspired by MQ [Zhou].  Transition from
   L1 to L2 implies various checks (open files, etc) have been
   performed, so ensures they are performed only once.  A
   correspondence to the "scan resistance" property of 2Q and MQ is
   accomplished by recycling/clean loads onto the LRU of L1.  Async
   processing onto L2 constrains oscillation in this algorithm. */

static struct lru_q_ LRU_1[LRU_N_Q_LANES];
static struct lru_q_ LRU_2[LRU_N_Q_LANES];

/* This is a global counter of files opened by cache_inode.  This is
   preliminary expected to go away.  Problems with this method are
   that it overcounts file descriptors for FSALs that don't use them
   for open files, and, under the Lieb Rearchitecture, FSALs will be
   responsible for caching their own file descriptors, with
   interfaces for Cache_Inode to interrogate them as to usage or
   instruct them to close them. */

size_t open_fd_count = 0;

/* The refcount mechanism distinguishes 3 key object states:

   1. unreferenced (unreachable)
   2. unincremented, but reachable
   3. incremented

   It seems most convenient to make unreferenced correspond to refcount==0.
   Then refcount==1 is a SENTINEL_REFCOUNT in which the only reference to
   the entry is the set of functions which can grant new references.  An
   object with refcount > 1 has been referenced by some thread, which must
   release its reference at some point.

   Currently, I propose to distinguish between objects with positive refcount
   and objects with state.  The latter could be evicted, in the normal case,
   only with loss of protocol correctness, but may have only the sentinel
   refcount.  To preserve constant time operation, they are stored in an
   independent partition of the LRU queue. */

static pthread_mutex_t lru_mtx;
static pthread_cond_t lru_cv;

static const uint32_t LRU_STATE_NONE = 0x00;
static const uint32_t LRU_STATE_RECLAIMING = 0x01;

static struct lru_state
{
     uint64_t entries_hiwat;
     uint64_t entries_lowat;
     uint32_t fds_hiwat;
     uint32_t flags;
     uint64_t last_count;
} lru_state;

static const uint32_t LRU_SLEEPING = 0x00000001;
static const uint32_t LRU_SHUTDOWN = 0x00000002;

static struct lru_thread_state
{
     pthread_t thread_id;
     uint32_t flags;
} lru_thread_state;

extern cache_inode_gc_policy_t cache_inode_gc_policy;

static inline void
lru_init_queue(struct lru_q_base *q)
{
     init_glist(&q->q);
     pthread_mutex_init(&q->mtx, NULL);
     q->size = 0;
}

/* Given a lane and a set of flags, return a pointer to the
   appropriate queue. */

static inline struct lru_q_base *
lru_select_queue(uint32_t flags, uint32_t lane)
{
     if (flags & LRU_ENTRY_PINNED) {
          if (flags & LRU_ENTRY_L2) {
               return &LRU_2[lane].lru_pinned;
          } else {
               return &LRU_1[lane].lru_pinned;
          }
     } else {
          if (flags & LRU_ENTRY_L2) {
               return &LRU_2[lane].lru;
          } else {
               return &LRU_1[lane].lru;
          }
     }
}

/* Insert the entry in the queue specified by the flags and lane. */

static inline void
lru_insert_entry(cache_inode_lru_t *lru, uint32_t flags, uint32_t lane)
{
     /* Destination LRU */
     struct lru_q_base *d = NULL;
     if (!(flags & LRU_HAVE_LOCKED_ENTRY)) {
          pthread_mutex_lock(&lru->mtx);
     }
     d = lru_select_queue(flags, lane);
     if (!(flags & LRU_HAVE_LOCKED_DST)) {
          pthread_mutex_lock(&d->mtx);
     }
     glist_add(&d->q, &lru->q);
     ++(d->size);
     if (!(flags & LRU_HAVE_LOCKED_DST)) {
          pthread_mutex_unlock(&d->mtx);
     }
     lru->flags &= ~(LRU_ENTRY_L2 | LRU_ENTRY_PINNED);
     lru->flags |= (flags & (LRU_ENTRY_L2 | LRU_ENTRY_PINNED));
     lru->lane = lane;
     if (!(flags & LRU_HAVE_LOCKED_ENTRY)) {
          pthread_mutex_unlock(&lru->mtx);
     }
}

/* Remove the entry from its queue. */

static inline void
lru_remove_entry(cache_inode_lru_t *lru,
                 uint32_t flags,
                 uint32_t lane __attribute__((unused)))
{
     /* Source LRU */
     struct lru_q_base *s = NULL;
     if (!(flags & LRU_HAVE_LOCKED_ENTRY)) {
          pthread_mutex_lock(&lru->mtx);
     }
     s = lru_select_queue(lru->flags, lru->lane);
     if (!(flags & LRU_HAVE_LOCKED_SRC)) {
          pthread_mutex_lock(&s->mtx);
     }

     glist_del(&lru->q);
     --(s->size);
     if (!(flags & LRU_HAVE_LOCKED_SRC)) {
          pthread_mutex_unlock(&s->mtx);
     }
     lru->flags &= ~(LRU_ENTRY_L2 | LRU_ENTRY_PINNED);
     lru->lane = LRU_NO_LANE;

     if (!(flags & LRU_HAVE_LOCKED_ENTRY)) {
          pthread_mutex_unlock(&lru->mtx);
     }
}

/* Move an entry from one queue to another.  The destination queue is
   specified in the flags argument. */

static inline void
lru_move_entry(cache_inode_lru_t *lru, uint32_t flags, uint32_t lane)
{
     /* Source LRU */
     struct lru_q_base *s = NULL;
     /* Destination LRU */
     struct lru_q_base *d = NULL;
     if (!(flags & LRU_HAVE_LOCKED_ENTRY)) {
          pthread_mutex_lock(&lru->mtx);
     }
     assert(lru->lane != LRU_NO_LANE);
     s = lru_select_queue(lru->flags, lru->lane);
     if (!(flags & LRU_HAVE_LOCKED_SRC)) {
          pthread_mutex_lock(&s->mtx);
     }
     d = lru_select_queue(flags, lane);
     if ((s != d) && !(flags & LRU_HAVE_LOCKED_DST)) {
          pthread_mutex_lock(&d->mtx);
     }
     glist_del(&lru->q);
     --(s->size);
     if (!(flags & LRU_HAVE_LOCKED_SRC)) {
          pthread_mutex_unlock(&s->mtx);
     }
     /* When moving from L2 to L1, add to the LRU, otherwise add to
        the MRU.  (In general we don't want to promote things except
        on initial reference, but promoting things on move makes more
        sense than demoting them.) */
     if ((lru->flags & LRU_ENTRY_L2) &&
         !(flags & LRU_ENTRY_L2)) {
          glist_add_tail(&d->q, &lru->q);
     } else {
          glist_add(&d->q, &lru->q);
     }
     ++(d->size);
     if ((s != d) && !(flags & LRU_HAVE_LOCKED_DST)) {
          pthread_mutex_unlock(&d->mtx);
     }
     lru->flags &= ~(LRU_ENTRY_L2 | LRU_ENTRY_PINNED);
     lru->flags |= (LRU_ENTRY_L2 | LRU_ENTRY_PINNED);
     lru->lane = lane;
     if (!(flags & LRU_HAVE_LOCKED_ENTRY)) {
          pthread_mutex_unlock(&lru->mtx);
     }
}

/*
 * Initialize subsystem
 */
void cache_inode_lru_pkginit(void)
{
     pthread_attr_t attr_thr;
     int ix = 0, code = 0;

     open_fd_count = 0;

     /* Repurpose some GC policy */
     lru_state.flags = LRU_STATE_NONE;

     /* Set high and low watermarks */
     lru_state.entries_hiwat
          = (75 * cache_inode_gc_policy.hwmark_nb_entries) / 100;
     lru_state.entries_lowat
          = (50 * cache_inode_gc_policy.hwmark_nb_entries) / 100;
     lru_state.fds_hiwat
          = (75 * cache_inode_gc_policy.max_fd) / 100;
     lru_state.last_count = 0;

     pthread_mutex_init(&lru_mtx, NULL);
     pthread_cond_init(&lru_cv, NULL);

     for (ix = 0; ix < LRU_N_Q_LANES; ++ix) {
          /* L1, unpinned */
          lru_init_queue(&LRU_1[ix].lru);
          /* L1, pinned */
          lru_init_queue(&LRU_1[ix].lru_pinned);
          /* L2, unpinned */
          lru_init_queue(&LRU_2[ix].lru);
          /* L2, pinned */
          lru_init_queue(&LRU_2[ix].lru_pinned);
     }

     if (pthread_attr_init(&attr_thr) != 0) {
          LogCrit(COMPONENT_CACHE_INODE_LRU,
                  "can't init pthread's attributes");
     }

     if (pthread_attr_setscope(&attr_thr, PTHREAD_SCOPE_SYSTEM)
         != 0) {
          LogCrit(COMPONENT_CACHE_INODE_LRU, "can't set pthread's scope");
     }

     if (pthread_attr_setdetachstate(&attr_thr, PTHREAD_CREATE_JOINABLE)
         != 0) {
          LogCrit(COMPONENT_CACHE_INODE_LRU, "can't set pthread's join state");
     }

     if (pthread_attr_setstacksize(&attr_thr, THREAD_STACK_SIZE)
         != 0) {
          LogCrit(COMPONENT_CACHE_INODE_LRU, "can't set pthread's stack size");
     }

     /* spawn LRU background thread */
     code = pthread_create(&lru_thread_state.thread_id, &attr_thr, lru_thread,
                          (void *) NULL);
     assert(code == 0);
}

/*
 * Shutdown subsystem
 */

void cache_inode_lru_pkgshutdown(void)
{
     /* Post and wait for shutdown of LRU background thread */
     pthread_mutex_lock(&lru_mtx);
     lru_thread_state.flags |= LRU_SHUTDOWN;
     lru_wake_thread(LRU_FLAG_NONE);
     pthread_mutex_unlock(&lru_mtx);
}

static inline void cache_inode_lru_clean(cache_entry_t *entry,
                                         cache_inode_client_t *client)
{
     fsal_status_t fsal_status = {0, 0};
     /* Clean an LRU entry re-use.  */
     assert((entry->lru.refcount == LRU_SENTINEL_REFCOUNT) ||
            (entry->lru.refcount == (LRU_SENTINEL_REFCOUNT - 1)));

     /* Clean up the associated ressources in the FSAL */
     if (FSAL_IS_ERROR(fsal_status
                       = FSAL_CleanObjectResources(&entry->handle))) {
          LogCrit(COMPONENT_CACHE_INODE,
                  "cache_inode_lru_clean: Couldn't free FSAL ressources "
                  "fsal_status.major=%u", fsal_status.major);
     }

     cache_inode_clean_internal(entry, client);
     entry->lru.refcount = 0;
     cache_inode_clean_entry(entry);
}

/* If the queue specified in q is non-empty return its least recently
   used entry.  Nota Bene: This function locks q, retaining the lock
   if an entry is found and unlocking it otherwise. */

static inline cache_inode_lru_t *
try_reap_entry(struct lru_q_base *q)
{
     cache_inode_lru_t *lru = NULL;

     pthread_mutex_lock(&q->mtx);
     lru = glist_first_entry(&q->q, cache_inode_lru_t, q);
     if (!lru) {
          pthread_mutex_unlock(&q->mtx);
     }
     return NULL;
}

/*
 * cache_inode_lru_get aka EvictBlock()
 *
 * repurpose a resident entry in the LRU system, if the system is
 * above low-water mark, from pool otherwise.
 */
cache_entry_t *
cache_inode_lru_get(cache_inode_client_t *client,
                    cache_inode_status_t *status,
                    uint32_t flags)
{
     /* The lane from which we harvest (or into which we store) the
        new entry.  Usually the lane assigned to this thread. */
     uint32_t lane = 0;
     /* For searching through lanes */
     uint32_t index = 0;
     /* The LRU entry */
     cache_inode_lru_t *lru = NULL;
     /* The Cache entry being created */
     cache_entry_t *entry = NULL;

     /* If we are in reclaim state, try to find an entry to recycle. */
     if (lru_state.flags & LRU_STATE_RECLAIMING) {
          /* Search through logical L2 entry. */

          for (lane = (client->thread_id + (index = 0)) % LRU_N_Q_LANES;
               (index < LRU_N_Q_LANES) && !lru;
               lane = (client->thread_id + (++index)) % LRU_N_Q_LANES) {
               lru = try_reap_entry(&LRU_2[lane].lru);
          }

          /* Search through logical L1 if nothing was found in L2
             (fall through, otherwise.) */

          for (lane = (client->thread_id + (index = 0)) % LRU_N_Q_LANES;
               (index < LRU_N_Q_LANES) && !lru;
               lane = (client->thread_id + (++index)) % LRU_N_Q_LANES) {
               lru = try_reap_entry(&LRU_1[lane].lru);
          }

          /* If we found an entry, use it */
          if (lru) {
               entry = container_of(lru, cache_entry_t, lru);

               if (entry)
                    LogFullDebug(COMPONENT_CACHE_INODE_LRU,
                                 "first entry %p refcount %"PRIu64" flags %d",
                                 entry,
                                 entry->lru.refcount,
                                 entry->lru.flags);

               /* Take the mutex here, before checking state and
                  refcount. */
               pthread_mutex_lock(&entry->lru.mtx);
               /* I /THINK/ we get a special dispensation in here
                  regarding the state lock because we're holding the
                  ref mutex.  Nobody should be able to get the entry
                  to put state on it we have the mutex, and if the
                  we're at the sentinel refcount, nobody should have
                  a reference they can use to put state on the
                  object. */
               /* if we can recycle this entry, do so. */
               if ((entry->lru.refcount == LRU_SENTINEL_REFCOUNT) &&
                   !cache_inode_file_holds_state(entry)) {
                    /* If this entry is in L2, move to the LRU of L1.
                       Otherwise leave it where it is.  Since this is
                       effectively a new file, being in L1 already
                       shouldn't count. */

                    if (entry->lru.flags & LRU_ENTRY_L2) {
                         lru_move_entry(&entry->lru,
                                        LRU_HAVE_LOCKED_ENTRY |
                                        LRU_HAVE_LOCKED_SRC,
                                        client->lru_lane);
                         pthread_mutex_unlock(&LRU_2[lane].lru.mtx);
                    } else {
                         pthread_mutex_unlock(&LRU_1[lane].lru.mtx);
                    }

                    /* Since we locked the entry before checking state
                       and refcount, nothing should have been able to
                       put state on it.  However, someone might be
                       waiting on it now.  Maybe the best way to deal
                       with that is to require someone to check the
                       handle after acquiring the mutex. */

                    LogFullDebug(COMPONENT_CACHE_INODE_LRU,
                                 "VICTIM entry %p refcount %"PRIu64" flags %d",
                                 entry,
                                 entry->lru.refcount,
                                 entry->lru.flags);

                    cache_inode_lru_clean(entry, client);

                    if (flags & LRU_REQ_FLAG_REF)
                         ++(entry->lru.refcount);

                    *status = CACHE_INODE_SUCCESS;
                    goto out;
               } else {
                    pthread_mutex_unlock(&entry->lru.mtx);
                    if (entry->lru.flags & LRU_ENTRY_L2) {
                         pthread_mutex_unlock(&LRU_2[lane].lru.mtx);
                    } else {
                         pthread_mutex_unlock(&LRU_1[lane].lru.mtx);
                    }
                    lru = NULL;
               }
          }
     }

     if (!lru) {
          lane = client->lru_lane;
          GetFromPool(entry, &client->pool_entry, cache_entry_t);
          if(entry == NULL) {
               LogCrit(COMPONENT_CACHE_INODE_LRU,
                       "can't allocate a new entry from cache pool");
               *status = CACHE_INODE_MALLOC_ERROR;
               goto out;
          }
          entry->lru.flags = 0;
          entry->lru.refcount = 0;
          if (pthread_mutex_init(&entry->lru.mtx, NULL) != 0) {
               ReleaseToPool(entry, &client->pool_entry);
               LogCrit(COMPONENT_CACHE_INODE_LRU,
                       "pthread_mutex_init of lru.mtx returned %d (%s)",
                       errno,
                       strerror(errno));
               *status = CACHE_INODE_INIT_ENTRY_FAILED;
               goto out;
          }

          if (flags & LRU_REQ_FLAG_REF)
               ++(entry->lru.refcount);

          lru_insert_entry(&entry->lru, LRU_HAVE_LOCKED_ENTRY, lane);
     }

     *status = CACHE_INODE_SUCCESS;
out:
     return (entry);
}

static inline cache_inode_status_t
cache_inode_lru_pin(cache_entry_t * entry,
                    uint32_t flags,
                    uint32_t lane)
{
     if (!(flags & LRU_HAVE_LOCKED_ENTRY)) {
          pthread_mutex_lock(&entry->lru.mtx);
     }

     /* Already pinned? */
     if (entry->lru.flags & LRU_ENTRY_PINNED)
          goto unlock;

     /* We make no effort to keep things in L2 when pinning them,
        because almost anything that you could do to pin something
        would invalidate the L2 guarantee of having been checked. */

     lru_move_entry(&entry->lru, flags | LRU_ENTRY_PINNED |
                    LRU_HAVE_LOCKED_ENTRY, lane);

unlock:
     if (!(flags & LRU_HAVE_LOCKED_ENTRY)) {
          pthread_mutex_unlock(&entry->lru.mtx);
     }

     return (CACHE_INODE_SUCCESS);
}

static inline cache_inode_status_t
cache_inode_lru_unpin(cache_entry_t *entry,
                      uint32_t flags,
                      uint32_t lane)
{
     if (!(flags & LRU_HAVE_LOCKED_ENTRY)) {
          pthread_mutex_lock(&entry->lru.mtx);
     }

     /* Already pinned? */
     if (!(entry->lru.flags & LRU_ENTRY_PINNED))
          goto unlock;

     lru_move_entry(&entry->lru, (flags & ~LRU_ENTRY_PINNED) |
                    LRU_HAVE_LOCKED_ENTRY, lane);

unlock:
     if (!(flags & LRU_HAVE_LOCKED_ENTRY)) {
          pthread_mutex_unlock(&entry->lru.mtx);
     }

     return (CACHE_INODE_SUCCESS);
}

cache_inode_status_t
cache_inode_lru_ref(cache_entry_t *entry,
                    cache_inode_client_t *client,
                    uint32_t flags)
{
     /* Refuse to grant a reference if we're below the sentinel value */
     if (entry->lru.refcount == 0) {
          return CACHE_INODE_DEAD_ENTRY;
     }

     /* These shouldn't ever be set */
     flags &= ~(LRU_ENTRY_PINNED | LRU_ENTRY_L2);

     /* Initial and Scan are mutually exclusive. */

     assert(!((flags & LRU_REQ_INITIAL) &&
              (flags & LRU_REQ_SCAN)));

     if (!(flags & LRU_HAVE_LOCKED_ENTRY)) {
          pthread_mutex_lock(&entry->lru.mtx);
     }

     /**
      * @todo ACE: I think it might be better to redo this so that the
      *       state layer calls the LRU to pin and unpin entries,
      *       rather than having the LRU interrogate the entry about
      *       its state every time it looks at it.
      */

     pthread_rwlock_rdlock(&entry->state_lock);
     if (cache_inode_file_holds_state(entry)) {
          flags |= LRU_ENTRY_PINNED;
     }

     ++(entry->lru.refcount);

     /* Move an entry forward if this is an initial reference.  In the
        case of a scan, move to the MRU of L2.  (lru_move_entry from
        L2 to L2 will promote to the MRU.) */

     if ((flags & LRU_REQ_INITIAL) ||
         ((flags & LRU_REQ_SCAN) &&
          (entry->lru.flags & LRU_ENTRY_L2))) {
          lru_move_entry(&entry->lru, flags | LRU_HAVE_LOCKED_ENTRY,
                         client->lru_lane);
     }

     pthread_rwlock_unlock(&entry->state_lock);

     if (!(flags & LRU_HAVE_LOCKED_ENTRY)) {
          pthread_mutex_unlock(&entry->lru.mtx);
     }

     return (CACHE_INODE_SUCCESS);
}

cache_inode_status_t
cache_inode_lru_unref(cache_entry_t *entry,
                      cache_inode_client_t *client,
                      uint32_t flags)
{
     if (!(flags & LRU_HAVE_LOCKED_ENTRY)) {
          pthread_mutex_lock(&entry->lru.mtx);
     }

     assert(entry->lru.refcount >= 1);

     if (--(entry->lru.refcount) == 0) {
          /* entry is UNREACHABLE -- the call path is recycling entry */
          cache_inode_lru_clean(entry, client);
          lru_remove_entry(&entry->lru, flags, client->lru_lane);

          pthread_mutex_unlock(&entry->lru.mtx);
          ReleaseToPool(entry, &client->pool_entry);
          goto unlock;
     }

     /* Is this actually needed?  We should be set if we have the SAL
        call cache_inode_lru_pin and cache_inode_lru_unpin */
     pthread_rwlock_rdlock(&entry->state_lock);
     if (cache_inode_file_holds_state(entry) &&
         !(entry->lru.flags & LRU_ENTRY_PINNED)) {
          cache_inode_lru_pin(entry,
                              flags | LRU_HAVE_LOCKED_ENTRY,
                              client->lru_lane);
     } else if (!cache_inode_file_holds_state(entry) &&
                (entry->lru.flags & LRU_ENTRY_PINNED)) {
          cache_inode_lru_unpin(entry,
                                flags | LRU_HAVE_LOCKED_ENTRY,
                                client->lru_lane);
     }
     pthread_rwlock_unlock(&entry->state_lock);

unlock:

     if (!(flags & LRU_HAVE_LOCKED_ENTRY)) {
          pthread_mutex_unlock(&entry->lru.mtx);
     }

    return (CACHE_INODE_SUCCESS);
}

#define S_NSECS 1000000000UL    /* nsecs in 1s */
#define MS_NSECS 1000000UL      /* nsecs in 1ms */

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
     lru_thread_state.flags |= LRU_SLEEPING;
     pthread_cond_timedwait(&lru_cv, &lru_mtx, &then);
     lru_thread_state.flags &= ~LRU_SLEEPING;
     pthread_mutex_unlock(&lru_mtx);
}

/* Async thread to perform long-term reorganization, compaction,
 * other operations that cannot be performed in constant time. */
static void *lru_thread(void *arg)
{
     /* Index */
     size_t i = 0;
    /* Temporary holder for flags */
     uint32_t tmpflags = lru_state.flags;

     SetNameFunction("lru_thread");

     while (1) {
          if (lru_thread_state.flags & LRU_SHUTDOWN)
               break;

          LogFullDebug(COMPONENT_CACHE_INODE_LRU,
                       "top of poll loop");

          pthread_mutex_lock(&lru_mtx);
          lru_state.last_count = 0;

          /* First, sum the queue counts.  This lets us know where we
             are relative to our watermarks. */

          for (i = 0; i < LRU_N_Q_LANES; ++i) {
               pthread_mutex_lock(&LRU_1[i].lru.mtx);
               lru_state.last_count += LRU_1[i].lru.size;
               pthread_mutex_unlock(&LRU_1[i].lru.mtx);

               pthread_mutex_lock(&LRU_1[i].lru_pinned.mtx);
               lru_state.last_count += LRU_1[i].lru_pinned.size;
               pthread_mutex_unlock(&LRU_1[i].lru_pinned.mtx);

               pthread_mutex_lock(&LRU_2[i].lru.mtx);
               lru_state.last_count += LRU_2[i].lru.size;
               pthread_mutex_unlock(&LRU_2[i].lru.mtx);

               pthread_mutex_lock(&LRU_2[i].lru_pinned.mtx);
               lru_state.last_count += LRU_2[i].lru_pinned.size;
               pthread_mutex_unlock(&LRU_2[i].lru_pinned.mtx);
          }

          if (tmpflags & LRU_STATE_RECLAIMING) {
               if (lru_state.last_count < lru_state.entries_lowat)
                    tmpflags &= ~LRU_STATE_RECLAIMING;
          } else {
               if (lru_state.last_count > lru_state.entries_hiwat) {
                    tmpflags |= LRU_STATE_RECLAIMING;
               }
          }

          /* I think it should be safe to read this unprotected, since
             the libc manual says almost all platforms have atomic
             integer load/store. */

          lru_state.flags = tmpflags;

          pthread_mutex_unlock(&lru_mtx);

          /* Do a constant amount of work on every queue.  In this
             case, if the number of file descriptors we have open is
             greater than the high water mark, start closing them.
             So that we don't re-examine the same file multiple
             times, we examine each file, and then move every
             examined file to L2 whether it has a file descriptor we
             can close or not.  This is a preliminary example of the
             L2 functionality rather than soemthing we expect to be
             permanent.  (It will have to adapt heavily to the new
             FSAL API, for example.) */

          if (open_fd_count > lru_state.fds_hiwat) {
               for (i = 0; i < LRU_N_Q_LANES; ++i) {
                    LogDebug(COMPONENT_CACHE_INODE_LRU,
                             "FD count is %zd and high water mark is "
                             "%d: reaping.",
                             open_fd_count,
                             lru_state.fds_hiwat);
                    int workdone = 0;
                    cache_inode_lru_t *lru = NULL;
                    pthread_mutex_lock(&LRU_1[i].lru.mtx);
                    while ((workdone < LRU_WORK_PER_WAKE) &&
                           (lru = glist_first_entry(&LRU_1[i].lru.q,
                                                    cache_inode_lru_t,
                                                    q))) {
                         cache_entry_t *entry
                              = container_of(lru, cache_entry_t, lru);
                         pthread_mutex_lock(&lru->mtx);
                         pthread_rwlock_rdlock(&entry->state_lock);
                         if (!cache_inode_file_holds_state(entry)) {
                              /* We would want the state lock for
                                 this anyway, so someone doesn't get
                                 an open while we're disposing of the
                                 descriptor. */
                              pthread_rwlock_wrlock(&entry->content_lock);
                              if (entry->object.file.open_fd.openflags
                                  != FSAL_O_CLOSED) {
                                   fsal_status_t fsal_status =
                                        FSAL_close(&(entry->object
                                                     .file.open_fd.fd));
                                   if (FSAL_IS_ERROR(fsal_status)) {
                                        LogCrit(COMPONENT_CACHE_INODE_LRU,
                                                "Error closing file in "
                                                "LRU thread.");
                                   }
                                   entry->object.file.open_fd.openflags
                                        = FSAL_O_CLOSED;
                                   --(open_fd_count);
                              }
                              /* Move the entry to L2 whatever the
                                 result of examining it.*/
                              lru_move_entry(lru,
                                             LRU_HAVE_LOCKED_SRC |
                                             LRU_HAVE_LOCKED_ENTRY |
                                             LRU_ENTRY_L2,
                                             i);
                              pthread_rwlock_unlock(&entry->content_lock);
                         } else {
                              /* Highly unlikely, this will probably
                                 go away if we make the SAL
                                 responsible for pinning/unpinning. */
                              lru_move_entry(lru,
                                             LRU_HAVE_LOCKED_SRC |
                                             LRU_HAVE_LOCKED_ENTRY |
                                             LRU_ENTRY_L2 |
                                             LRU_ENTRY_PINNED,
                                             i);
                         }
                         pthread_rwlock_unlock(&entry->state_lock);
                         pthread_mutex_unlock(&lru->mtx);
                    }
                    pthread_mutex_unlock(&LRU_1[i].lru.mtx);
               }
          } else {
               LogDebug(COMPONENT_CACHE_INODE_LRU,
                        "FD count is %zd.  Would need to be above %d to reap.",
                        open_fd_count,
                        lru_state.fds_hiwat);
          }

          lru_thread_delay_ms(1000 * 5 * 60);
     }

     LogCrit(COMPONENT_CACHE_INODE_LRU,
             "shutdown");

     return (NULL);
}

void lru_wake_thread(uint32_t flags)
{
     if (lru_thread_state.flags & LRU_SLEEPING)
          pthread_cond_signal(&lru_cv);
}
