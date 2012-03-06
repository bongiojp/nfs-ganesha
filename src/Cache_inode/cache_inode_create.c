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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * ---------------------------------------
 */

/**
 * \file    cache_inode_create.c
 * \author  $Author: deniel $
 * \date    $Date: 2005/11/28 17:02:26 $
 * \version $Revision: 1.29 $
 * \brief   Creation of a file through the cache layer.
 *
 * cache_inode_mkdir.c : Creation of an entry through the cache layer
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
#include "stuff_alloc.h"
#include <unistd.h>
#include <sys/types.h>
#include <sys/param.h>
#include <time.h>
#include <pthread.h>

/**
 * @brief Creates an object in a directory
 *
 * This function creates an entry in the cache and underlying
 * filesystem.  If an entry is returned, its refcount charged to the
 * call path is +1.
 *
 * @param parent [IN] Parent directory
 * @param name [IN] Name of the object to create
 * @param type [IN] Type of the object to create
 * @param policy [IN] Caching policy for this entry
 * @param mode [IN] Mode to be used at file creation
 * @param create_arg [IN] Additional argument for object creation
 * @param attr [OUT] Attributes of the new object
 * @param client [INOUT] Per-thread resource management structure
 * @param context [IN] FSAL credentials
 * @param status [OUT] Returned status
 *
 * @return Cache entry for the file created
 */

cache_entry_t *
cache_inode_create(cache_entry_t *parent,
                   fsal_name_t *name,
                   cache_inode_file_type_t type,
                   cache_inode_policy_t policy,
                   fsal_accessmode_t mode,
                   cache_inode_create_arg_t *create_arg,
                   fsal_attrib_list_t *attr,
                   cache_inode_client_t *client,
                   fsal_op_context_t *context,
                   cache_inode_status_t *status)
{
     cache_entry_t *entry = NULL;
     cache_inode_dir_entry_t *new_dir_entry = NULL;
     fsal_status_t fsal_status = {0, 0};
#ifdef _USE_MFSL
     mfsl_object_t object_handle;
#else
     fsal_handle_t object_handle;
#endif
     fsal_attrib_list_t object_attributes;
     fsal_handle_t dir_handle;
     cache_inode_fsal_data_t fsal_data;
     cache_inode_status_t status;
     cache_inode_create_arg_t zero_create_arg;
     fsal_accessflags_t access_mask = 0;

     memset(&zero_create_arg, 0, sizeof(zero_create_arg));
     memset(&fsal_data, 0, sizeof(fsal_data));
     memset(&object_handle, 0, sizeof(object_handle));

     if (pcreate_arg == NULL) {
          pcreate_arg = &zero_create_arg;
     }

     /* Set the return default to CACHE_INODE_SUCCESS */
     *status = CACHE_INODE_SUCCESS;

     ++(client->stat.nb_call_total);
     inc_func_call(client, CACHE_INODE_CREATE);

     if ((type != REGULAR_FILE) && (type != DIRECTORY) &&
         (type != SYMBOLIC_LINK) && (type != SOCKET_FILE) &&
         (type != FIFO_FILE) && (type != CHARACTER_FILE) &&
         (type != BLOCK_FILE)) {
          *status = CACHE_INODE_BAD_TYPE;

          /* stats */
          inc_func_err_unrecover(client, CACHE_INODE_CREATE);
          entry = NULL;
          goto out;
        }

     /* Check if an entry of the same name exists */
     entry = cache_inode_lookup(parent,
                                name,
                                policy,
                                attr,
                                client,
                                context,
                                status);
     if (entry != NULL) {
          *status = CACHE_INODE_ENTRY_EXISTS;
          if (entry->type != type) {
               /* Incompatible types, returns NULL */
               cache_inode_lru_unref(entry, client,
                                     LRU_FLAG_NONE);
               inc_func_err_unrecover(client, CACHE_INODE_CREATE);
               entry = NULL;
               goto out;
          } else {
               inc_func_success(client, CACHE_INODE_CREATE);
               goto out;
          }
     }

     /* The entry doesn't exist, so we can create it. */

     object_attributes.asked_attributes = client->attrmask;
     switch (type) {
     case REGULAR_FILE:
#ifdef _USE_MFSL
          fsal_status = MFSL_create(&parent->mobject,
                                    name, context,
                                    &client->mfsl_context,
                                    mode, &object_handle,
                                    &object_attributes, NULL,
                                    NULL);
#else
          fsal_status = FSAL_create(&parent->handle,
                                    name, context, mode,
                                    &object_handle, &object_attributes);
#endif
          break;

     case DIRECTORY:
#ifdef _USE_MFSL
          fsal_status = MFSL_mkdir(&parent->mobject,
                                   name, context,
                                   &client->mfsl_context,
                                   mode, &object_handle,
                                   &object_attributes,
                                   NULL,
                                   NULL);
#else
          fsal_status = FSAL_mkdir(&parent->handle,
                                   name, context, mode,
                                   &object_handle, &object_attributes);
#endif
          break;

     case SYMBOLIC_LINK:
#ifdef _USE_MFSL
          fsal_status = MFSL_symlink(&parent->mobject,
                                     name, &create_arg->link_content,
                                     context, &client->mfsl_context,
                                     mode, &object_handle,
                                     &object_attributes, NULL);
#else
          fsal_status = FSAL_symlink(&parent->handle,
                                     name, &create_arg->link_content,
                                     context, mode, &object_handle,
                                     &object_attributes);
#endif
          break;

     case SOCKET_FILE:
#ifdef _USE_MFSL
          fsal_status = MFSL_mknode(&parent->mobject, name,
                                    context, &client->mfsl_context, mode,
                                    FSAL_TYPE_SOCK, NULL,
                                    &object_handle,
                                    &object_attributes,
                                    NULL);
#else
          fsal_status = FSAL_mknode(&parent->handle, name, context,
                                    mode, FSAL_TYPE_SOCK, NULL,
                                    &object_handle, &object_attributes);
#endif
          break;

     case FIFO_FILE:
#ifdef _USE_MFSL
          fsal_status = MFSL_mknode(&parent->mobject, name,
                                    context, &client->mfsl_context,
                                    mode, FSAL_TYPE_FIFO, NULL,
                                    &object_handle,
                                    &object_attributes,
                                    NULL);
#else
          fsal_status = FSAL_mknode(&parent->handle, name, context,
                                    mode, FSAL_TYPE_FIFO, NULL,
                                    &object_handle, &object_attributes);
#endif
          break;

     case BLOCK_FILE:
#ifdef _USE_MFSL
          fsal_status = MFSL_mknode(&parent->mobject,
                                    name, context,
                                    &client->mfsl_context,
                                    mode, FSAL_TYPE_BLK,
                                    &create_arg->dev_spec,
                                    &object_handle,
                                    &object_attributes,
                                    NULL);
#else
          fsal_status = FSAL_mknode(&parent->handle,
                                    name, context,
                                    mode, FSAL_TYPE_BLK,
                                    &create_arg->dev_spec,
                                    &object_handle, &object_attributes);
#endif
             break;

     case CHARACTER_FILE:
#ifdef _USE_MFSL
          fsal_status = MFSL_mknode(&parent->mobject,
                                    name, context,
                                    &client->mfsl_context,
                                    mode, FSAL_TYPE_CHR,
                                    &create_arg->dev_spec,
                                    &object_handle,
                                    &object_attributes,
                                    NULL);
#else
          fsal_status = FSAL_mknode(&parent->handle,
                                    name, context,
                                    mode, FSAL_TYPE_CHR,
                                    &create_arg->dev_spec,
                                    &object_handle,
                                    &object_attributes);
#endif
          break;

     default:
          /* we should never go there */
          *status = CACHE_INODE_INCONSISTENT_ENTRY;
          inc_func_err_unrecover(client, CACHE_INODE_CREATE);
          entry = NULL;
          goto out;
          break;
        }

     /* Check for the result */
     if (FSAL_IS_ERROR(fsal_status)) {
          *status = cache_inode_error_convert(fsal_status);
          inc_func_err_unrecover(client, CACHE_INODE_CREATE);
          entry = NULL;
          goto out;
     }
#ifdef _USE_MFSL
     fsal_data.fh_desc.start = &object_handle.handle;
#else
     fsal_data.fh_desc.start = &object_handle;
#endif
     fsal_data.fh_desc.len = 0;
     (void) FSAL_ExpandHandle(pcontext->export_context,
                              FSAL_DIGEST_SIZEOF,
                              &fsal_data.fh_desc);

     entry = cache_inode_new_entry(&fsal_data,
                                   &object_attributes,
                                   type,
                                   policy,
                                   create_arg,
                                   client,
                                   context,
                                   CACHE_INODE_FLAG_CREATE |
                                   CACHE_INODE_FLAG_EXREF,
                                   status);
     if (entry == NULL) {
          *pstatus = CACHE_INODE_INSERT_ERROR;

          inc_func_err_unrecover(pclient, CACHE_INODE_CREATE);
          return NULL;
     }

#ifdef _USE_MFSL
     /* Copy the MFSL object to the cache */
     memcpy(&(pentry->mobject),
            &object_handle, sizeof(mfsl_object_t));
#endif

     pthread_rwlock_wrlock(&parent->content_lock);
     /* Add this entry to the directory (also takes an internal ref) */
     cache_inode_add_cached_dirent(parent,
                                   name, entry,
                                   &new_dir_entry,
                                   client,
                                   context,
                                   status);
     pthread_rwlock_unlock(&parent->content_lock);
     if (*status != CACHE_INODE_SUCCESS) {
          inc_func_err_unrecover(client, CACHE_INODE_CREATE);
          cache_inode_lru_unref(entry, client,
                                LRU_FLAG_NONE);
          entry = NULL;
          goto out;
     }

     pthread_rwlock_wrlock(&parent->attr_lock);
     /* Update the parent cached attributes */
     cache_inode_set_time_current(&parent->attributes.mtime);
     parent->attributes.ctime = parent->attributes.mtime;
     /* if the created object is a directory, it contains a link
        to its parent : '..'. Thus the numlink attr must be increased. */
     if (type == DIRECTORY) {
          ++(parent->attributes.numlinks);
     }
     pthread_rwlock_unlock(&parent->attr_lock);

     /* Copy up the child attributes */
     *attr = object_attributes;

     inc_func_success(client, CACHE_INODE_CREATE);
     *status = CACHE_INODE_SUCCESS;

out:

     return entry;
}
