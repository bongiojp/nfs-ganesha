/*
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright CEA/DAM/DIF  (2008)
 * contributeur : Philippe DENIEL   philippe.deniel@cea.fr
 *                Thomas LEIBOVICI  thomas.leibovici@cea.fr
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
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
 * \file    cache_inode_setattr.c
 * \author  $Author: leibovic $
 * \date    $Date: 2006/02/14 11:47:40 $
 * \version $Revision: 1.19 $
 * \brief   Sets the attributes for an entry.
 *
 * cache_inode_setattr.c : Sets the attributes for an entry.
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
#include "stuff_alloc.h"
#include "nfs4_acls.h"

#include <unistd.h>
#include <sys/types.h>
#include <sys/param.h>
#include <time.h>
#include <pthread.h>
#include <assert.h>

/**
 * @brief Set the attributes for a file.
 *
 * This function sets the attributes of a file, both in the cache and
 * in the underlying filesystem.
 *
 * @param entry [in] Entry whose attributes are to be set
 * @param attr [in,out] Attributes to set/result of set
 * @param client [in,out] Structure for per-thread resource
 *                         management
 * @param context [in] FSAL credentials
 * @param status [out] returned status
 *
 * @retval CACHE_INODE_SUCCESS if operation is a success
 * @retval CACHE_INODE_LRU_ERROR if allocation error occured when
 *         validating the entry
 */

cache_inode_status_t
cache_inode_setattr(cache_entry_t *entry,
                    fsal_attrib_list_t *attr,
                    cache_inode_client_t *client,
                    fsal_op_context_t *context,
                    cache_inode_status_t *status)
{
     fsal_status_t fsal_status = {0, 0};

     ++(client->stat.nb_call_total);
     ++(client->stat.func_stats.nb_call[CACHE_INODE_SETATTR]);

     if ((entry->type == UNASSIGNED) ||
         (entry->type == RECYCLED)) {
          LogCrit(COMPONENT_CACHE_INODE,
                  "WARNING: unknown source entry type: type=%d, "
                  "line %d in file %s", entry->type, __LINE__, __FILE__);
          *status = CACHE_INODE_BAD_TYPE;
          goto out;
     }

     if ((attr->asked_attributes & FSAL_ATTR_SIZE) &&
         (entry->type != REGULAR_FILE)) {
          LogMajor(COMPONENT_CACHE_INODE,
                   "Attempt to truncate non-regular file: type=%d",
                   entry->type);
          *status = CACHE_INODE_BAD_TYPE;
     }

     pthread_rwlock_wrlock(&entry->attr_lock);
     if (attr->asked_attributes & FSAL_ATTR_SIZE) {
          fsal_status = FSAL_truncate(&entry->handle,
                                      context, attr->filesize,
                                      NULL, NULL);
          if (FSAL_IS_ERROR(fsal_status)) {
               *status = cache_inode_error_convert(fsal_status);
               ++(client->stat.func_stats
                  .nb_err_unrecover[CACHE_INODE_SETATTR]);
               goto unlock;
          }
     }

     cache_inode_prep_attrs(entry, client);
#ifdef _USE_MFSL
     fsal_status =
          MFSL_setattrs(&entry->mobject, context, &client->mfsl_context, attr,
                        &entry->attributes, NULL);
#else
     fsal_status = FSAL_setattrs(&entry->handle, context, attr,
                                 &entry->attributes);
#endif
     if (FSAL_IS_ERROR(fsal_status)) {
          *status = cache_inode_error_convert(fsal_status);
          ++(client->stat.func_stats.nb_err_unrecover[CACHE_INODE_SETATTR]);
          goto unlock;
     }
     cache_inode_fixup_md(entry);

     /* Copy the complete set of new attributes out. */

     *attr = entry->attributes;

     *status = CACHE_INODE_SUCCESS;
     ++(client->stat.func_stats.nb_success[CACHE_INODE_SETATTR]);

unlock:
     pthread_rwlock_unlock(&entry->attr_lock);

out:

     return *status;
} /* cache_inode_setattr */
