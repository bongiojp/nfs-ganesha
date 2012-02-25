/*
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright CEA/DAM/DIF  (2008)
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
 * ---------------------------------------
 */

/**
 *
 * \File    cache_inode_kill_entry.c
 * \author  $Author: deniel $
 * \date    $Date: 2006/01/05 15:14:51 $
 * \version $Revision: 1.63 $
 * \brief   Some routines for management of the cache_inode layer, shared by other calls.
 *
 * cache_inode_kill_entry.c : Some routines for management of the cache_inode layer, shared by other calls.
 *
 *
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef _SOLARIS
#include "solaris_port.h"
#endif                          /* _SOLARIS */

#include "LRU_List.h"
#include "log.h"
#include "HashData.h"
#include "HashTable.h"
#include "fsal.h"
#include "cache_inode.h"
#include "cache_inode_lru.h"
#include "cache_inode_weakref.h"
#include "cache_content.h"
#include "stuff_alloc.h"
#include "nfs4_acls.h"

#include <unistd.h>
#include <sys/types.h>
#include <sys/param.h>
#include <time.h>
#include <pthread.h>
#include <string.h>
#include <assert.h>

/**
 *
 * @brief Forcibly remove an entry from the cache
 *
 * @todo ACE: Enable killing of state-bearing files after changing
 *            states to use weak references.
 *
 * This function removes an entry from the cache immediately when it
 * has become unusable (for example, when the FSAL declares it to be
 * stale.)
 *
 * @param entry [in] The entry to be killed
 * @param client [in,out] Structure to manage per-thread resources
 * @param status [out] CACHE_INODE_SUCCESS or various error codes
 * @param flags [in] A word whose bits indicate locking status
 */

cache_inode_status_t
cache_inode_kill_entry(cache_entry_t *entry,
                       cache_inode_client_t *client,
                       cache_inode_status_t *status,
                       uint32_t flags)
{
     cache_inode_fsal_data_t fsaldata;
     hash_buffer_t key, old_key;
     hash_buffer_t old_value;
     int rc;

     if (cache_inode_file_holds_state(entry)) {
          goto out;
     }

     /* You do not request that a lock be held on an object you are
        destroying. */
     assert(!(flags & (CACHE_INODE_FLAG_ATTR_HOLD |
                       CACHE_INODE_FLAG_CONTENT_HOLD)));

     memset((char *)&fsaldata, 0, sizeof(fsaldata));

     LogInfo(COMPONENT_CACHE_INODE,
             "Using cache_inode_kill_entry for entry %p", entry);

     fsaldata.fh_desc.start = pfsal_handle;
     fsaldata.fh_desc.len = 0;
     (void) FSAL_ExpandHandle(NULL,  /* pcontext but not used... */
                              FSAL_DIGEST_SIZEOF,
                              &fsaldata.fh_desc);

     /* Use the handle to build the key */
     key.pdata = fsaldata.fh_desc.start;
     key.len = fsaldata.fh_desc.len;

     /* return HashTable (sentinel) reference */
     cache_inode_lru_unref(pentry, pclient, LRU_FLAG_NONE);

     /* Clean up the associated ressources in the FSAL */
     if(FSAL_IS_ERROR(fsal_status = FSAL_CleanObjectResources(pfsal_handle)))
     {
      LogCrit(COMPONENT_CACHE_INODE,
              "cache_inode_kill_entry: Couldn't free FSAL ressources fsal_status.major=%u",
              fsal_status.major);
    }

     /* Sanity check: old_value.pdata is expected to be equal to pentry,
      * and is released later in this function */
     if ((cache_entry_t *) old_value.pdata != entry ||
         (cache_entry_t *)old_value.pdata->fh_desc.start != &pentry->handle) {
          LogCrit(COMPONENT_CACHE_INODE,
                  "cache_inode_kill_entry: unexpected pdata %p from hash table (pentry=%p)",
                  old_value.pdata, pentry);
     }


     if ((rc = HashTable_Del(fh_to_cache_entry_ht,
                             &key,
                             &old_key,
                             &old_value)) != HASHTABLE_SUCCESS) {
          if (rc != HASHTABLE_ERROR_NO_SUCH_KEY) {
               LogCrit(COMPONENT_CACHE_INODE,
                       "cache_inode_kill_entry: entry could not be deleted, "
                       " status = %d",
                       rc);
          }
     }

     cache_inode_weakref_delete(&entry->weakref);

     /* Return the sentinel reference */
     cache_inode_lru_unref(entry, client, LRU_FLAG_NONE);

     entry = NULL;

out:

     if (entry) {
          if (flags & CACHE_INODE_FLAG_ATTR_HAVE) {
               pthread_rwlock_unlock(&entry->attr_lock);
          }
          if (flags & CACHE_INODE_FLAG_CONTENT_HAVE) {
               pthread_rwlock_unlock(&entry->content_lock);
          }
     }


     *status = CACHE_INODE_SUCCESS;
     return *status;
}                               /* cache_inode_kill_entry */
