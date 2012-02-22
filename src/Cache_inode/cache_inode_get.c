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
 * \file    cache_inode_get.c
 * \author  $Author: deniel $
 * \date    $Date: 2005/11/28 17:02:26 $
 * \version $Revision: 1.26 $
 * \brief   Get and eventually cache an entry.
 *
 * cache_inode_get.c : Get and eventually cache an entry.
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
#include "stuff_alloc.h"

#include <unistd.h>
#include <sys/types.h>
#include <sys/param.h>
#include <time.h>
#include <pthread.h>

/**
 *
 * @brief Gets an entry by using its fsdata as a key and caches it if needed.
 *
 * Gets an entry by using its fsdata as a key and caches it if needed.
 *
 * If a cache entry is returned, its refcount is incremented by one.
 *
 * cache_inode_get_located is no longer needed with the split between
 * directory ad attribute locks.
 *
 * @param fsdata [IN] File system data
 * @param pattr [OUT] Pointer to the attributes for the result
 * @param pclient [INOUT] Pointer to resource management structure
 * @param pcontext [IN] FSAL credentials
 * @param pstatus [OUT] Returned status
 *
 * @return If successful, the pointer to the entry; NULL otherwise
 *
 */
cache_entry_t *cache_inode_get(cache_inode_fsal_data_t * pfsdata,
                               cache_inode_policy_t policy,
                               fsal_attrib_list_t * pattr,
                               cache_inode_client_t * pclient,
                               fsal_op_context_t * pcontext,
                               cache_inode_status_t * pstatus )
{
  hash_buffer_t key, value;
  cache_entry_t *pentry = NULL;
  fsal_status_t fsal_status;
  cache_inode_create_arg_t create_arg;
  cache_inode_file_type_t type;
  int hrc = 0;
  fsal_attrib_list_t fsal_attributes;
  fsal_handle_t *pfile_handle;
  cache_inode_fsal_data_t *ppoolfsdata = NULL;
  void *htoken;

  memset(&create_arg, 0, sizeof(create_arg));

  /* Set the return default to CACHE_INODE_SUCCESS */
  *pstatus = CACHE_INODE_SUCCESS;

  /* stats */
  /* cache_invalidate calls this with no context or client */
  if (pclient) {
    pclient->stat.nb_call_total += 1;
    pclient->stat.func_stats.nb_call[CACHE_INODE_GET] += 1;
  }

  /* Turn the input to a hash key on our own.
   */
  key.pdata = pfsdata->fh_desc.start;
  key.len = pfsdata->fh_desc.len;

  switch (hrc = HashTable_GetEx(fh_to_cache_entry_ht, &key, &value, &htoken))
    {
    case HASHTABLE_SUCCESS:
      /* Entry exists in the cache and was found */
      pentry = (cache_entry_t *) value.pdata;

      /* take an extra reference within the critical section */
      cache_inode_lru_ref(pentry, pclient, LRU_REQ_INITIAL);

      HashTable_Release(fh_to_cache_entry_ht, htoken);

      /* return attributes additionally */
      *pattr = pentry->attributes;

      if ( !pclient ) {
        /* invalidate. Just return it to mark it stale and go on. */
        return( pentry );
      }

      break;

    case HASHTABLE_ERROR_NO_SUCH_KEY:
      if ( !pclient ) {
        /* invalidate. Just return */
        return( NULL );
      }
      /* Cache miss, allocate a new entry */

      pfile_handle = (fsal_handle_t *) pfsdata->fh_desc.start;

      /* First, call FSAL to know what the object is */
      fsal_attributes.asked_attributes = pclient->attrmask;
      fsal_status = FSAL_getattrs(pfile_handle, pcontext, &fsal_attributes);
      if(FSAL_IS_ERROR(fsal_status))
        {
          *pstatus = cache_inode_error_convert(fsal_status);

          LogDebug(COMPONENT_CACHE_INODE,
                   "cache_inode_get: cache_inode_status=%u fsal_status=%u,%u ",
                   *pstatus, fsal_status.major, fsal_status.minor);

          if(fsal_status.major == ERR_FSAL_STALE)
            {
              char handle_str[256];

              snprintHandle(handle_str, 256, pfile_handle);
              LogEvent(COMPONENT_CACHE_INODE,
                       "cache_inode_get: Stale FSAL File Handle %s, fsal_status=(%u,%u)",
                       handle_str, fsal_status.major, fsal_status.minor);

              *pstatus = CACHE_INODE_FSAL_ESTALE;
            }

          /* stats */
          pclient->stat.func_stats.nb_err_unrecover[CACHE_INODE_GET] += 1;

          return NULL;
        }

      /* The type has to be set in the attributes */
      if(!FSAL_TEST_MASK(fsal_attributes.supported_attributes, FSAL_ATTR_TYPE))
        {
          *pstatus = CACHE_INODE_FSAL_ERROR;

          /* stats */
          pclient->stat.func_stats.nb_err_unrecover[CACHE_INODE_GET] += 1;

          return NULL;
        }

      /* Get the cache_inode file type */
      type = cache_inode_fsal_type_convert(fsal_attributes.type);

      if(type == SYMBOLIC_LINK)
        {
          if( CACHE_INODE_KEEP_CONTENT( policy ) )
           {
             FSAL_CLEAR_MASK(fsal_attributes.asked_attributes);
             FSAL_SET_MASK(fsal_attributes.asked_attributes, pclient->attrmask);
             fsal_status =
                FSAL_readlink(pfile_handle, pcontext, &create_arg.link_content,
                              &fsal_attributes);
            }
          else
            {
               fsal_status.major = ERR_FSAL_NO_ERROR ;
               fsal_status.minor = 0 ;
            }

          if(FSAL_IS_ERROR(fsal_status))
            {
              *pstatus = cache_inode_error_convert(fsal_status);

              /* stats */
              pclient->stat.func_stats.nb_err_unrecover[CACHE_INODE_GET] += 1;

              if(fsal_status.major == ERR_FSAL_STALE)
                {
                  cache_inode_status_t kill_status;

                  LogEvent(COMPONENT_CACHE_INODE,
                           "cache_inode_get: Stale FSAL File Handle detected for pentry = %p, fsal_status=(%u,%u)",
                           pentry, fsal_status.major, fsal_status.minor);

                  /* return reference we just took */
                  cache_inode_lru_unref(pentry, pclient, LRU_FLAG_NONE);

                  /* hashtable refcount will likely drop to zero now */
                  if(cache_inode_kill_entry(pentry, NO_LOCK, pclient, &kill_status) !=
                     CACHE_INODE_SUCCESS)
                    LogCrit(COMPONENT_CACHE_INODE,
                            "cache_inode_get: Could not kill entry %p, "
                            "status = %u",
                            pentry,
                            kill_status);

                  *pstatus = CACHE_INODE_FSAL_ESTALE;

                }

              return NULL;
            }
        }

      /* Add the entry to the cache */
      if ( type == 1)
        LogCrit(COMPONENT_CACHE_INODE,"inode get");

      if((pentry = cache_inode_new_entry( pfsdata,
                                          &fsal_attributes,
                                          type,
                                          policy,
                                          &create_arg,
                                          NULL,    /* never used to add a new DIR_CONTINUE within this function */
                                          pclient,
                                          pcontext,
                                          CACHE_INODE_FLAG_EXREF, /* This is a population, not a creation */
                                          pstatus ) ) == NULL )
        {
          /* stats */
          pclient->stat.func_stats.nb_err_unrecover[CACHE_INODE_GET] += 1;

          return NULL;
        }

      /* Set the returned attributes */
      *pattr = fsal_attributes;

      /* Now, exit the switch/case and returns */
      break;

    default:
      /* This should not happened */
      *pstatus = CACHE_INODE_INVALID_ARGUMENT;
      LogCrit(COMPONENT_CACHE_INODE,
              "cache_inode_get returning CACHE_INODE_INVALID_ARGUMENT - this "
              "should not have happened");

      if ( !pclient ) {
        /* invalidate. Just return */
        return( NULL );
      }

      /* stats */
      pclient->stat.func_stats.nb_err_unrecover[CACHE_INODE_GET] += 1;

      return NULL;
      break;
    }  /* end switch */

  /* Want to ASSERT pclient at this point */
  *pstatus = CACHE_INODE_SUCCESS;

  /* valid the found entry, if this is not feasible, returns nothing to the
   * client */

  /* it appears likely that if cache_inode_valid fails, the entry is
   * still in HashTable. Who is responsible for cleaning it up in this
   * case? (Matt) */

  /* stats */
  pclient->stat.func_stats.nb_success[CACHE_INODE_GET] += 1;

  /* Free this key */
  cache_inode_release_fsaldata_key(&key, pclient);

  return ( pentry );
}  /* cache_inode_get */

/**
 *
 * cache_inode_put:  release logical reference to a cache entry conferred by
 * a previous call to cache_inode_get (cache_inode_get_located).
 *
 * The result is typically to decrement the reference count on entry, but
 * additional side effects include LRU adjustment, movement to/from the
 * protected LRU partition, or recyling if the caller has raced an operation
 * which made entry unreachable (and this current caller has the last
 * reference).  Caller MUST NOT make further accesses to the memory pointed
 * to by entry.
 *
 * @param entry [IN] cache entry being returned
 * @param pclient [INOUT] ressource allocated by the client for the nfs management.
 *
 * @return status.
 *
 */
cache_inode_status_t cache_inode_put(cache_entry_t *entry,
                                     cache_inode_client_t *pclient)
{
  return (cache_inode_lru_unref(entry, pclient, LRU_FLAG_NONE));
}
