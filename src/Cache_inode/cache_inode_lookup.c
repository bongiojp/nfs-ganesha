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
 * \file    cache_inode_lookup.c
 * \author  $Author: deniel $
 * \date    $Date: 2005/11/28 17:02:26 $
 * \version $Revision: 1.33 $
 * \brief   Perform lookup through the cache.
 *
 * cache_inode_lookup.c : Perform lookup through the cache.
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
#include "cache_inode_avl.h"
#include "cache_inode_weakref.h"
#include "cache_inode_lru.h"
#include "stuff_alloc.h"

#include <unistd.h>
#include <sys/types.h>
#include <sys/param.h>
#include <time.h>
#include <pthread.h>

/**
 *
 * @brief Do the work of looking up a name in a directory.
 *
 * This function looks up a filename in the given directory.  It
 * implements the functionality of cache_inode_lookup and expects the
 * directory to be read-locked when it is called.  If a lookup from
 * cache fails, it will drop the read lock and acquire a write lock
 * before proceeding.  The caller is responsible for freeing the lock
 * on the directory in any case.
 *
 * If a cache entry is returned, its refcount is incremented by 1.
 *
 * @param pentry_parent [IN] Entry for the parent directory to be managed.
 * @param name [IN] Name of the entry that we are looking for in the cache.
 * @param pclient [INOUT] Ressource allocated by the client for the
 *                        NFS management.
 * @param pcontext [IN] FSAL credentials
 * @param pstatus [OUT] Returned status.
 *
 * @return CACHE_INODE_SUCCESS if operation is a success
 * @return CACHE_INODE_LRU_ERROR if allocation error occured when
 *                               validating the entry
 *
 */

cache_entry_t *
cache_inode_lookup_impl(cache_entry_t *pentry_parent,
                        fsal_name_t *pname,
                        cache_inode_policy_t policy,
                        cache_inode_client_t *pclient,
                        fsal_op_context_t *pcontext,
                        cache_inode_status_t *pstatus)
{
     cache_inode_dir_entry_t dirent_key;
     cache_inode_dir_entry_t *dirent = NULL;
     cache_inode_dir_entry_t *new_dir_entry = NULL;
     cache_entry_t *pentry = NULL;
     fsal_status_t fsal_status = {0, 0};
     fsal_handle_t object_handle;
     fsal_attrib_list_t object_attributes;
     cache_inode_create_arg_t create_arg = {
          .newly_created_dir = FALSE
     };
     cache_inode_file_type_t type = UNASSIGNED;
     cache_inode_status_t cache_status = CACHE_INODE_SUCCESS;
     cache_inode_fsal_data_t new_entry_fsdata;
     cache_inode_dir_entry_t *broken_dirent = NULL;

     memset(&dirent_key, 0, sizeof(dirent_key));
     memset(&new_entry_fsdata, 0, sizeof(new_entry_fsdata));

     /* Set the return default to CACHE_INODE_SUCCESS */
     *pstatus = CACHE_INODE_SUCCESS;
     /* stats */
     (pclient->stat.nb_call_total)++;
     (pclient->stat.func_stats.nb_call[CACHE_INODE_LOOKUP])++;

     if(pentry_parent->type != DIRECTORY) {
          *pstatus = CACHE_INODE_NOT_A_DIRECTORY;
          /* stats */
          (pclient->stat.func_stats.nb_err_unrecover[CACHE_INODE_LOOKUP])++;
          return NULL;
     }

     /* if name is ".", use the input value */
     if (!FSAL_namecmp(pname, (fsal_name_t *) & FSAL_DOT)) {
          pentry = pentry_parent;
     } else if (!FSAL_namecmp(pname, (fsal_name_t *) & FSAL_DOT_DOT)) {
          /* Directory do only have exactly one parent. This a limitation
           * in all FS, which implies that hard link are forbidden on
           * directories (so that they exists only in one dir).  Because
           * of this, the parent list is always limited to one element for
           * a dir.  Clients SHOULD never 'lookup( .. )' in something that
           * is no dir. */
          pentry =
               cache_inode_lookupp_impl(pentry_parent, pclient, pcontext,
                                        pstatus);
     } else if(CACHE_INODE_KEEP_CONTENT(policy)) {
          /* We first try avltree_lookup by name.  If that fails, we
           * dispatch to the FSAL. */

          FSAL_namecpy(&dirent_key.name, pname);
          dirent = cache_inode_avl_qp_lookup_s(pentry_parent, &dirent_key, 1);
          if (dirent) {
               /* Getting a weakref itself increases the refcount. */
               pentry = cache_inode_weakref_get(&dirent->entry,
                                                pclient,
                                                LRU_REQ_SCAN);
               if (pentry == NULL) {
                    broken_dirent = dirent;
               }
          }
          pthread_rwlock_unlock(&pentry_parent->content_lock);
          pthread_rwlock_wrlock(&pentry_parent->content_lock);
          /* Make sure nobody put the entry in the cache while we
             were waiting. */
          dirent = cache_inode_avl_qp_lookup_s(pentry_parent, &dirent_key, 1);
          if (dirent) {
               pentry = cache_inode_weakref_get(&dirent->entry,
                                                pclient,
                                                LRU_REQ_SCAN);
               if (pentry == NULL) {
                    broken_dirent = dirent;
               }
          }
     }

      if(pentry == NULL)
        {
          LogDebug(COMPONENT_CACHE_INODE, "Cache Miss detected");

          memset(&object_attributes, 0, sizeof(fsal_attrib_list_t));
          object_attributes.asked_attributes = pclient->attrmask;
          fsal_status =
               FSAL_lookup(&pentry_parent->handle,
                           pname, pcontext, &object_handle,
                           &object_attributes);
          if(FSAL_IS_ERROR(fsal_status)) {
               *pstatus = cache_inode_error_convert(fsal_status);
               (pclient->stat.func_stats.
                nb_err_unrecover[CACHE_INODE_LOOKUP])++;
               return NULL;
          }

          type = cache_inode_fsal_type_convert(object_attributes.type);

          /* If entry is a symlink, this value for be cached */
          if(type == SYMBOLIC_LINK) {
               if(CACHE_INODE_KEEP_CONTENT(policy)) {
                    fsal_status =
                         FSAL_readlink(&object_handle,
                                       pcontext,
                                       &create_arg.link_content,
                                       &object_attributes);
               } else {
                    fsal_status.major = ERR_FSAL_NO_ERROR;
                    fsal_status.minor = 0;
               }

               if(FSAL_IS_ERROR(fsal_status)) {
                    *pstatus = cache_inode_error_convert(fsal_status);
                    (pclient->stat.func_stats
                     .nb_err_unrecover[CACHE_INODE_LOOKUP])++;
                    return NULL;
               }
          }

          /* Allocation of a new entry in the cache */
          new_entry_fsdata.fh_desc.start = (caddr_t)(&object_handle);
          new_entry_fsdata.fh_desc.len = 0;
          FSAL_ExpandHandle(pcontext->export_context,
                            FSAL_DIGEST_SIZEOF,
                            &new_entry_fsdata.fh_desc);

          if((pentry
              = cache_inode_new_entry(&new_entry_fsdata,
                                      &object_attributes,
                                      type,
                                      policy,
                                      &create_arg,
                                      pclient,
                                      pcontext,
                                      CACHE_INODE_FLAG_EXREF,
                                      pstatus)) == NULL) {
               (pclient->stat.func_stats
                .nb_err_unrecover[CACHE_INODE_LOOKUP])++;
               return NULL;
          }

          if (CACHE_INODE_KEEP_CONTENT(policy)) {
               if (broken_dirent) {
                    /* Directory entry existed, but the weak reference
                       was broken.  Just update with the new one. */
                    broken_dirent->entry = pentry->weakref;
                    cache_status = CACHE_INODE_SUCCESS;
               } else {
                    /* Entry was found in the FSAL, add this entry to
                     * the parent directory */
                    cache_status
                         = cache_inode_add_cached_dirent(pentry_parent,
                                                         pname,
                                                         pentry,
                                                         &new_dir_entry,
                                                         pclient,
                                                         pcontext,
                                                         pstatus);
                    if(cache_status != CACHE_INODE_SUCCESS &&
                       cache_status != CACHE_INODE_ENTRY_EXISTS) {
                         /* stats */
                         (pclient->stat.func_stats
                          .nb_err_unrecover[CACHE_INODE_LOOKUP])++;
                         return NULL;
                    }
               }
          }
     }

     /* stat */
     if (*pstatus != CACHE_INODE_SUCCESS) {
          (pclient->stat.func_stats.nb_err_retryable[CACHE_INODE_LOOKUP])++;
     } else {
          (pclient->stat.func_stats.nb_success[CACHE_INODE_LOOKUP])++;
     }

  return pentry;
} /* cache_inode_lookup_impl */

/**
 *
 * @brief Public function for looking up a name in a directory
 *
 * Looks up for a name in a directory indicated by a cached entry. The
 * directory should have been cached before.
 *
 * If a cache entry is returned, the refcount on entry is +1.
 *
 * @param pentry_parent [IN] Entry for the parent directory to be managed.
 * @param name [IN] Name of the entry that we are looking up.
 * @param pattr [OUT] Attributes for the entry that we have found.
 * @param pclient [INOUT] Ressource allocated by the client for NFS management.
 * @param pcontext [IN] FSAL credentials.
 * @param pstatus [OUT] Returned status.
 *
 * @return CACHE_INODE_SUCCESS if operation is a success
 * @return CACHE_INODE_LRU_ERROR if allocation error occured when
 *                               validating the entry
 */

cache_entry_t *
cache_inode_lookup(cache_entry_t *pentry_parent,
                   fsal_name_t *pname,
                   cache_inode_policy_t policy,
                   fsal_attrib_list_t *pattr,
                   cache_inode_client_t *pclient,
                   fsal_op_context_t *pcontext,
                   cache_inode_status_t *pstatus)
{
     cache_entry_t *entry = NULL;
     fsal_accessflags_t access_mask
          = (FSAL_MODE_MASK_SET(FSAL_X_OK) |
             FSAL_ACE4_MASK_SET(FSAL_ACE_PERM_LIST_DIR));

     if (cache_inode_access(pentry_parent,
                            access_mask,
                            pclient,
                            pcontext,
                            pstatus) !=
         CACHE_INODE_SUCCESS) {
          return NULL;
     }

     pthread_rwlock_rdlock(&pentry_parent->content_lock);
     entry = cache_inode_lookup_impl(pentry_parent,
                                     pname,
                                     policy,
                                     pclient,
                                     pcontext,
                                     pstatus);
     pthread_rwlock_unlock(&pentry_parent->content_lock);

     return entry;
} /* cache_inode_lookup */
