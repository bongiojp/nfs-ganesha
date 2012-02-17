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
#include <stdint.h>
#include "stuff_alloc.h"
#include "nlm_list.h"
#include "fsal.h"
#include "nfs_core.h"
#include "log_macros.h"
#include "cache_inode.h"
#include "generic_weakref.h"

/**
 *
 * \file generic_weakref.c
 * \author Matt Benjamin
 * \brief Generic weak reference package
 *
 * \section DESCRIPTION
 *
 * This module defines an infrastructure for enforcement of
 * reference counting guarantees, eviction safety, and access restrictions
 * using ordinary object addresses.
 *
 */

#define CACHE_LINE_SIZE 64
#define CACHE_PAD(_n) char __pad ## _n [CACHE_LINE_SIZE]

typedef struct gweakref_partition_
{
    pthread_rwlock_t lock;
    struct avltree t;
    CACHE_PAD(0);
} gweakref_partition_t;

struct gweakref_table_
{
    uint64_t genctr;
    CACHE_PAD(0);
    gweakref_partition_t *partition;
    CACHE_PAD(1);
    uint32_t npart;
};

#define gwt_partition_of_addr_k(xt, k) \
    (((xt)->partition)+(((uint64_t)k)%(xt)->npart))

typedef struct gweakref_priv_
{
    struct avltree_node node_k;
    gweakref_t k;
} gweakref_priv_t;

static inline int wk_cmpf(const struct avltree_node *lhs,
                          const struct avltree_node *rhs)
{
    gweakref_priv_t *lk, *rk;

    lk = avltree_container_of(lhs, gweakref_priv_t, node_k);
    rk = avltree_container_of(rhs, gweakref_priv_t, node_k);

    if (lk->k.ptr < rk->k.ptr)
	return (-1);

    if (lk->k.ptr == rk->k.ptr)
	return (0);

    return (1);
}

gweakref_table_t *gweakref_init(uint32_t npart)
{
    int ix;
    pthread_rwlockattr_t rwlock_attr;
    gweakref_partition_t *wp;
    gweakref_table_t *wt = NULL;

    wt = (gweakref_table_t *) Mem_Alloc(sizeof(gweakref_table_t));
    if (! wt)
        goto out;

    /* prior versions of Linux tirpc are subject to default prefer-reader
     * behavior (so have potential for writer starvation) */
    pthread_rwlockattr_init(&rwlock_attr);
#ifdef GLIBC
    pthread_rwlockattr_setkind_np(
        &rwlock_attr, 
        PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
#endif

    /* npart should be a small integer */
    wt->npart = npart;
    wt->partition = (gweakref_partition_t *)
        Mem_Alloc(npart * sizeof(gweakref_partition_t)); 
    for (ix = 0; ix < npart; ++ix) {
        wp = &wt->partition[ix];
        pthread_rwlock_init(&wp->lock, &rwlock_attr);
        avltree_init(&wp->t, wk_cmpf, 0 /* must be 0 */);
    }
    wt->genctr = 0;    

out:
    return (wt);
}

gweakref_t gweakref_insert(gweakref_table_t *wt, void *obj)
{
    gweakref_t ret;
    gweakref_priv_t *ref;
    gweakref_partition_t *wp;
    struct avltree_node *node;

    ref = (gweakref_priv_t *) Mem_Alloc(sizeof(gweakref_priv_t));

    ref->k.ptr = obj;
    ref->k.gen = __sync_add_and_fetch(&wt->genctr, 1);

    wp = (gweakref_partition_t *) gwt_partition_of_addr_k(wt, ref->k.ptr);

    pthread_rwlock_wrlock(&wp->lock);
    node = avltree_insert(&ref->node_k, &wp->t);
    if (! node) {
        /* success */
        ret = ref->k;
    } else {
        /* matching key existed */
        ret.ptr = NULL;
        ret.gen = 0;
    }
    pthread_rwlock_unlock(&wp->lock);

    return (ret);
}

void *gweakref_lookup(gweakref_table_t *wt, gweakref_t *ref)
{
    struct avltree_node *node;
    gweakref_priv_t refk, *tref;
    gweakref_partition_t *wp;
    void *ret = NULL;

    /* look up ref.ptr--return !NULL iff ref.ptr is found and
     * ref.gen == found.gen */

    refk.k = *ref;
    wp = gwt_partition_of_addr_k(wt, refk.k.ptr);
    pthread_rwlock_rdlock(&wp->lock);
    node = avltree_lookup(&refk.node_k, &wp->t);
    if (node) {
        /* found it, maybe */
        tref = avltree_container_of(node, gweakref_priv_t, node_k);
        if (tref->k.gen == ref->gen)
            ret = ref->ptr;
    }
    pthread_rwlock_unlock(&wp->lock);
    
    return (ret);
}

#define GWR_FLAG_NONE    0x0000
#define GWR_FLAG_WLOCKED  0x0001

static inline void gweakref_delete_impl(gweakref_table_t *wt, gweakref_t *ref,
                                        uint32_t flags)
{
    struct avltree_node *node;
    gweakref_priv_t refk, *tref;
    gweakref_partition_t *wp;

    /* lookup up ref.ptr, delete iff ref.ptr is found and
     * ref.gen == found.gen */

    refk.k = *ref;
    wp = gwt_partition_of_addr_k(wt, refk.k.ptr);
    if (! (flags & GWR_FLAG_WLOCKED))
        pthread_rwlock_wrlock(&wp->lock);
    node = avltree_lookup(&refk.node_k, &wp->t);
    if (node) {
        /* found it, maybe */
        tref = avltree_container_of(node, gweakref_priv_t, node_k);
        if (tref->k.gen == ref->gen)
            /* unhook it */
            avltree_remove(node, &wp->t);
            Mem_Free(tref);
    }
    if (! (flags & GWR_FLAG_WLOCKED))
        pthread_rwlock_unlock(&wp->lock);
}

void gweakref_delete(gweakref_table_t *wt, gweakref_t *ref)
{
    gweakref_delete_impl(wt, ref, GWR_FLAG_NONE);
}

void gweakref_destroy(gweakref_table_t *wt)
{
    struct avltree_node *node, *onode;
    gweakref_partition_t *wp;
    gweakref_priv_t *tref;
    int ix;

    /* quiesce the server, then... */

    for (ix = 0; ix < wt->npart; ++ix) {
        wp = &wt->partition[ix];
        onode = NULL;
        node = avltree_first(&wp->t);
        do {
            if (onode) {
                tref = avltree_container_of(onode, gweakref_priv_t, node_k);
                Mem_Free(tref);
            }
        } while ((onode = node) && (node = avltree_next(node)));
        if (onode) {
            tref = avltree_container_of(onode, gweakref_priv_t, node_k);
            Mem_Free(tref);
        }
        avltree_init(&wp->t, wk_cmpf, 0 /* must be 0 */);
    }
    Mem_Free(wt->partition);
    Mem_Free(wt);
}
