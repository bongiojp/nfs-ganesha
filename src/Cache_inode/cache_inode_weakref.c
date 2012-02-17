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
#include "cache_inode_weakref.h"
#include "generic_weakref.h"

/**
 *
 * \file cache_inode_weakref.c
 * \author Matt Benjamin
 * \brief Cache inode weak reference package
 *
 * \section DESCRIPTION
 *
 * Manage weak references to cache inode objects (e.g., references from
 * directory entries).
 *
 */

#define WEAKREF_PARTITIONS 17

static gweakref_table_t *cache_inode_wt = NULL;

/*
 * cache_inode_weakref_init
 * cache_inode_weakref_insert -- install entry in table
 * cache_inode_weakref_get -- get initial reference
 * cache_inode_weakref_delete -- delete entry from table
 * cache_inode_weakref_shutdown
 */

/*
 * Init package.
 */
void cache_inode_weakref_init()
{
    cache_inode_wt = gweakref_init(17);
}

/*
 * Install entry in the weakref table.  Caller must hold a reference
 * to entry.
 */
gweakref_t cache_inode_weakref_insert(cache_entry_t *entry)
{
    return (gweakref_insert(cache_inode_wt, entry));
}

/*
 * Get an initial reference to a cache entry object, based on (the
 * address and generation number stored in) ref, or NULL if the reference
 * is no longer valid.
 */
cache_entry_t *cache_inode_weakref_get(gweakref_t *ref)
{
    cache_entry_t *entry =
        (cache_entry_t *) gweakref_lookup(cache_inode_wt, ref);

        if (entry) {
            /* XXXX Adam--don't forget to add refcount and LRU management :) */
            abort();
        }

    return (entry);
}

/*
 * Delete a reference from the table.  The caller must hold an initial
 * reference (and probably must be code internal to the cache_inode_lru
 * package to ensure atomicity).
 */
int32_t cache_inode_weakref_delete(gweakref_t *ref)
{
    (void) gweakref_delete(cache_inode_wt, ref);
}

/*
 * Clean up, when no further calls will be made on the interface.
 */
void cache_inode_weakref_shutdown()
{
    gweakref_destroy(cache_inode_wt);
}
