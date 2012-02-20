/**
 *
 * \file    cache_inode_remove.c
 * \author  $Author: leibovic $
 * \date    $Date: 2006/01/31 10:18:58 $
 * \version $Revision: 1.32 $
 * \brief   Removes an entry of any type.
 */

/*
 * vim:expandtab:shiftwidth=8:tabstop=8:
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
#include "cache_content.h"
#include "stuff_alloc.h"

#include <unistd.h>
#include <sys/types.h>
#include <sys/param.h>
#include <time.h>
#include <pthread.h>
#include <assert.h>

/**
 *
 * cache_inode_is_dir_empty: checks if a directory is empty or not. No mutex management.
 *
 * Checks if a directory is empty or not. No mutex management
 *
 * @param pentry [IN] entry to be checked (should be of type DIRECTORY)
 *
 * @return CACHE_INODE_SUCCESS is directory is empty\n
 * @return CACHE_INODE_BAD_TYPE is pentry is not of type DIRECTORY\n
 * @return CACHE_INODE_DIR_NOT_EMPTY if pentry is not empty
 *
 */
cache_inode_status_t cache_inode_is_dir_empty(cache_entry_t *pentry)
{
     cache_inode_status_t status;

     /* Sanity check */
     if(pentry->type != DIRECTORY) {
          return CACHE_INODE_BAD_TYPE;
     }

     status = (pentry->object.dir.nbactive == 0) ?
          CACHE_INODE_SUCCESS :
          CACHE_INODE_DIR_NOT_EMPTY;

     return status;
}                               /* cache_inode_is_dir_empty */

/**
 *
 * cache_inode_is_dir_empty_WithLock: checks if a directory is empty or not, BUT has lock management.
 *
 * Checks if a directory is empty or not, BUT has lock management.
 *
 * @param pentry [IN] entry to be checked (should be of type DIRECTORY)
 *
 * @return CACHE_INODE_SUCCESS is directory is empty\n
 * @return CACHE_INODE_BAD_TYPE is pentry is not of type DIRECTORY\n
 * @return CACHE_INODE_DIR_NOT_EMPTY if pentry is not empty
 *
 */
cache_inode_status_t cache_inode_is_dir_empty_WithLock(cache_entry_t * pentry)
{
     cache_inode_status_t status;

     pthread_rwlock_rdlock(&pentry->object.dir.dir_lock);
     status = cache_inode_is_dir_empty(pentry);
     pthread_rwlock_unlock(&pentry->object.dir.dir_lock);

     return status;
}                               /* cache_inode_is_dir_empty_WithLock */

/**
 * cache_inode_clean_internal: remove a pentry from cache and all LRUs,
 *                             and release related resources.
 *
 * @param pentry [IN] entry to be deleted from cache
 * @param hash_table_t [IN] The cache hash table
 * @param pclient [INOUT] ressource allocated by the client for the nfs management.
 */
cache_inode_status_t cache_inode_clean_internal(cache_entry_t * to_remove_entry,
                                                cache_inode_client_t * pclient)
{
  fsal_handle_t *pfsal_handle_remove;
  cache_inode_parent_entry_t *parent_iter = NULL;
  cache_inode_parent_entry_t *parent_iter_next = NULL;
  cache_inode_fsal_data_t fsaldata;
  cache_inode_status_t status;
  hash_buffer_t key, old_key, old_value;
  int rc;

  memset( (char *)&fsaldata, 0, sizeof( fsaldata ) ) ;

  if((pfsal_handle_remove =
      cache_inode_get_fsal_handle(to_remove_entry, &status)) == NULL)
    {
      return status;
    }

  /* delete the entry from the cache */
  key.pdata = to_remove_entry->fh_desc.start;
  key.len = to_remove_entry->fh_desc.len;

  /* use the key to delete the entry */
  rc = HashTable_Del(fh_to_cache_entry_ht, &key, &old_key, &old_value);

  if(rc)
    LogCrit(COMPONENT_CACHE_INODE,
            "HashTable_Del error %d in cache_inode_clean_internal", rc);

  if((rc != HASHTABLE_SUCCESS) && (rc != HASHTABLE_ERROR_NO_SUCH_KEY))
    {
      return CACHE_INODE_INCONSISTENT_ENTRY;
    }

  /* release the key that was stored in hash table */
  if(rc != HASHTABLE_ERROR_NO_SUCH_KEY)
    {
      /* return Hashtable (sentinel) reference */
      cache_inode_lru_unref(to_remove_entry, pclient, LRU_FLAG_NONE);

      /* Sanity check: old_value.pdata is expected to be equal to pentry,
       * and is released later in this function */
      if((cache_entry_t *) old_value.pdata != to_remove_entry ||
	 ((cache_entry_t *)old_value.pdata)->fh_desc.start != (caddr_t)&(to_remove_entry->handle))
        {
          LogCrit(COMPONENT_CACHE_INODE,
                  "cache_inode_remove: unexpected pdata %p from hash table (pentry=%p)",
                  old_value.pdata, to_remove_entry);
        }
    }

  /* Free the parent list entries */

  parent_iter = to_remove_entry->parent_list;
  while(parent_iter != NULL)
    {
      parent_iter_next = parent_iter->next_parent;

      ReleaseToPool(parent_iter, &pclient->pool_parent);

      parent_iter = parent_iter_next;
    }

  return CACHE_INODE_SUCCESS;
}                               /* cache_inode_clean_internal */

/**
 *
 * cache_inode_remove_sw: removes a pentry addressed by its parent pentry and
 * its FSAL name.  Mutex management is switched.
 *
 * Removes a pentry addressed by its parent pentry and its FSAL name.  Mutex
 * management is switched.
 *
 * @param pentry  [IN]     entry for the parent directory to be managed.
 * @param name    [IN]     name of the entry that we are looking for in the cache.
 * @param pattr   [OUT]    attributes for the entry that we have found.
 * @param pclient [INOUT] ressource allocated by the client for the nfs management.
 * @param pcontext   [IN]    FSAL credentials
 * @param pstatus [OUT]   returned status.
 *
 * @return CACHE_INODE_SUCCESS if operation is a success \n
 * @return CACHE_INODE_LRU_ERROR if allocation error occured when validating the entry
 *
 */
cache_inode_status_t cache_inode_remove(cache_entry_t *pentry,
                                        fsal_name_t *pnode_name,
                                        fsal_attrib_list_t *pattr,
                                        cache_inode_client_t *pclient,
                                        fsal_op_context_t *pcontext,
                                        cache_inode_status_t *pstatus)
{
     cache_inode_status_t status;
     fsal_accessflags_t access_mask = 0;

     /* stats */
     (pclient->stat.nb_call_total)++;
     (pclient->stat.func_stats.nb_call[CACHE_INODE_REMOVE])++;

     /* Get the attribute lock and check access */
     pthread_rwlock_wrlock(&pentry->attr_lock);

     /* Check if caller is allowed to perform the operation */
     access_mask = (FSAL_MODE_MASK_SET(FSAL_W_OK) |
                    FSAL_ACE4_MASK_SET(FSAL_ACE_PERM_DELETE_CHILD));

     if((*pstatus
         = cache_inode_access_sw(pentry,
                                 access_mask,
                                 pclient,
                                 pcontext,
                                 &status,
                                 FALSE))
        != CACHE_INODE_SUCCESS) {
          goto unlock_attr;
     }

     /* Acquire the directory lock and remove the entry */

     pthread_rwlock_wrlock(&pentry->object.dir.dir_lock);

     cache_inode_remove_int(pentry,
                            pnode_name,
                            pclient,
                            pcontext,
                            pstatus,
                            TRUE, /* Keep the attribute lock so we
                                     can copy attributes back to the
                                     caller.  I plan to get rid of
                                     this later. */
                            FALSE);

     *pattr = pentry->attributes;

unlock_attr:

     pthread_rwlock_unlock(&pentry->attr_lock);

     if (*pstatus == CACHE_INODE_SUCCESS) {
          (pclient->stat.func_stats.nb_success[CACHE_INODE_REMOVE])++;
     } else {
          (pclient->stat.func_stats.nb_err_unrecover[CACHE_INODE_REMOVE])++;
     }

     return *pstatus;
}                               /* cache_inode_remove */

/**
 *
 * \brief Implement actual work of removing file
 *
 * Actually remove an entry from the directory.  Assume that the
 * directory contents and attributes are locked for writes.  The
 * attribute lock is released unless keep_md_lock is TRUE.
 *
 * @param entry   [IN]    entry for the parent directory to be managed.
 * @param name    [IN]    name of the entry that we are looking for in the cache.
 * @param client  [INOUT] ressource allocated by the client for the nfs management.
 * @param context [IN]    FSAL credentials
 * @param status  [OUT]   returned status.
 *
 * @return CACHE_INODE_SUCCESS if operation is a success \n
 * @return CACHE_INODE_LRU_ERROR if allocation error occured when validating the entry
 *
 */
cache_inode_status_t cache_inode_remove_int(cache_entry_t *entry,
                                            fsal_name_t *name,
                                            cache_inode_client_t *client,
                                            fsal_op_context_t *context,
                                            cache_inode_status_t *status,
                                            bool_t keep_attr_lock,
                                            bool_t keep_dir_lock)
{
     cache_entry_t *to_remove_entry = NULL;
     cache_content_status_t content_status = 0;
     fsal_status_t fsal_status = {0, 0};
     fsal_attrib_list_t remove_attr;

     if(entry->type != DIRECTORY) {
          *status = CACHE_INODE_BAD_TYPE;
          goto out;
     }

     /* Factor this somewhat.  In the case where the directory hasn't
        been populated, the entry may not exist in the cache and we'd
        be bringing it in just to dispose of it. */

     /* Looks up for the entry to remove */
     if ((to_remove_entry
          = cache_inode_lookup_sw(entry,
                                  name,
                                  CACHE_INODE_JOKER_POLICY,
                                  &remove_attr,
                                  client,
                                  context,
                                  status,
                                  FALSE)) == NULL) {
          goto out;
     }

     /* Lock the attributes (so we can decrement the link count) */
     pthread_rwlock_wrlock(&to_remove_entry->attr_lock);

     LogDebug(COMPONENT_CACHE_INODE,
              "---> Cache_inode_remove : %s", name->name);

     cache_inode_prep_attrs(entry, client);
#ifdef _USE_MFSL
     fsal_status = MFSL_unlink(&entry->mobject,
                               name,
                               &to_remove_entry->mobject,
                               context,
                               &client->mfsl_context,
                               &entry->attributes,
                               NULL);
#else
     fsal_status = FSAL_unlink(&entry->handle,
                               name,
                               context,
                               &entry->attributes);
#endif

     if (FSAL_IS_ERROR(fsal_status)) {
          *status = cache_inode_error_convert(fsal_status);
          goto unlock;
     }
     cache_inode_fixup_md(entry);

     if (!keep_attr_lock) {
          pthread_rwlock_unlock(&entry->attr_lock);
     }

     /* Remove the entry from parent dir_entries avl */
     cache_inode_remove_cached_dirent(entry, name, client, status);
     if (!keep_dir_lock) {
          pthread_rwlock_unlock(&entry->object.dir.dir_lock);
     }

     LogFullDebug(COMPONENT_CACHE_INODE,
                  "cache_inode_remove_cached_dirent: status=%d", *status);

     /* Update the attributes for the removed entry */

     if ((to_remove_entry->type != DIRECTORY) &&
         (to_remove_entry->attributes.numlinks > 1)) {
          if ((*status = cache_inode_refresh_attrs(to_remove_entry,
                                                   context,
                                                   client))
              != CACHE_INODE_SUCCESS) {
               goto unlock;
          }
     } else {
          /* Otherwise our count is zero, or it was an empty
             directory. */
          to_remove_entry->attributes.numlinks = 0;
     }

     /* Now, delete "to_remove_entry" from the cache inode and free
        its associated resources, but only if numlinks == 0 */
     if (to_remove_entry->attributes.numlinks == 0) {
          /* If pentry is a regular file, data cached, the related
             data cache entry should be removed as well */
          if (to_remove_entry->type == REGULAR_FILE) {
               if(to_remove_entry->object.file.pentry_content != NULL) {
                    /* Something is to be deleted, release the cache
                       data entry */
                    if (cache_content_release_entry((cache_content_entry_t *)
                                                    (to_remove_entry->object
                                                     .file.pentry_content),
                                                    (cache_content_client_t *)
                                                    client->pcontent_client,
                                                    &content_status)
                        != CACHE_CONTENT_SUCCESS) {
                         LogEvent(COMPONENT_CACHE_INODE,
                                  "pentry %p, named %s could not be "
                                  "released from data cache, status=%d",
                                  to_remove_entry, name->name,
                                  content_status);
                    }
               }
          }

          /* Destroy the entry when everyone's references to it have
             been relinquished.  Most likely now. */
          pthread_rwlock_unlock(&to_remove_entry->attr_lock);
          if ((*status =
               cache_inode_lru_unref(to_remove_entry,
                                     client,
                                     0)) != CACHE_INODE_SUCCESS) {
               goto out;
          }
          /* We call unref twice.  Once for the reference taken by
             lookup, and once for the sentinel reference. */
          if ((*status =
               cache_inode_lru_unref(to_remove_entry,
                                     client,
                                     0)) != CACHE_INODE_SUCCESS) {
               goto out;
          }
     } else {
     unlock:

          pthread_rwlock_unlock(&to_remove_entry->attr_lock);
     }

out:

     if (*status != CACHE_INODE_SUCCESS) {
          (client->stat.func_stats.nb_err_unrecover[CACHE_INODE_REMOVE])++;
     }
     return *status;
}
