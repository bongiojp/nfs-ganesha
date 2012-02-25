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
 * @brief Set the attributes for an entry located in the cache by its address.
 *
 * Sets the attributes for an entry located in the cache by its
 * address. Attributes are provided with compliance to the underlying
 * FSAL semantics. Attributes that are set are returned in "*pattr".
 *
 * @param pentry [in] Entry whose attributes are to be set
 * @param pattr [in,out] Attributes to set/result of set
 * @param pclient [in,out] Structure for per-thread resource
 *                         management
 * @param pcontext [in] FSAL credentials
 * @param pstatus [out] returned status
 *
 * @return CACHE_INODE_SUCCESS if operation is a success \n
 * @return CACHE_INODE_LRU_ERROR if allocation error occured when
 *         validating the entry
 */

cache_inode_status_t
cache_inode_setattr(cache_entry_t *pentry,
                    fsal_attrib_list_t *pattr,
                    cache_inode_client_t *pclient,
                    fsal_op_context_t *pcontext,
                    cache_inode_status_t *pstatus)
{
  fsal_status_t fsal_status = {0, 0};

  /* Set the return default to CACHE_INODE_SUCCESS */
  *pstatus = CACHE_INODE_SUCCESS;

  /* stat */
  pclient->stat.nb_call_total += 1;
  pclient->stat.func_stats.nb_call[CACHE_INODE_SETATTR] += 1;

  if ((pentry->type == UNASSIGNED) ||
      (pentry->type == RECYCLED))
    {
      LogCrit(COMPONENT_CACHE_INODE,
              "WARNING: unknown source pentry type: type=%d, "
              "line %d in file %s", pentry->type, __LINE__, __FILE__);
      *pstatus = CACHE_INODE_BAD_TYPE;
      return *pstatus;
    }

  if ((pattr->asked_attributes & FSAL_ATTR_SIZE) &&
      (pentry->type != REGULAR_FILE)) {
    LogMajor(COMPONENT_CACHE_INODE,
             "Attempt to truncate non-regular file: type=%d",
             pentry->type);
    *pstatus = CACHE_INODE_BAD_TYPE;
  }

  pthread_rwlock_wrlock(&pentry->attr_lock);
  if(pattr->asked_attributes & FSAL_ATTR_SIZE)
    {
      fsal_status = FSAL_truncate(&pentry->handle,
                                  pcontext, pattr->filesize,
                                  NULL, NULL);
      if(FSAL_IS_ERROR(fsal_status))
        {
          *pstatus = cache_inode_error_convert(fsal_status);
          pthread_rwlock_wrlock(&pentry->attr_lock);

          /* stat */
          pclient->stat.func_stats.nb_err_unrecover[CACHE_INODE_SETATTR] += 1;
          return *pstatus;
        }
    }

  cache_inode_prep_attrs(pentry, pclient);
#ifdef _USE_MFSL
  fsal_status =
      MFSL_setattrs(&pentry->mobject, pcontext, &pclient->mfsl_context, pattr,
                    &pentry->attributes, NULL);
#else
  fsal_status = FSAL_setattrs(&pentry->handle, pcontext, pattr,
                              &pentry->attributes);
#endif
  if(FSAL_IS_ERROR(fsal_status))
    {
      *pstatus = cache_inode_error_convert(fsal_status);
      pthread_rwlock_unlock(&pentry->attr_lock);
      /* stat */
      pclient->stat.func_stats.nb_err_unrecover[CACHE_INODE_SETATTR] += 1;
      return *pstatus;
    }
  cache_inode_fixup_md(pentry);
  /* Return the attributes as set */
  *pattr = pentry->attributes;
  pthread_rwlock_unlock(&pentry->attr_lock);

  /* stat */
  if(*pstatus != CACHE_INODE_SUCCESS)
    pclient->stat.func_stats.nb_err_retryable[CACHE_INODE_SETATTR] += 1;
  else
    pclient->stat.func_stats.nb_success[CACHE_INODE_SETATTR] += 1;

  return *pstatus;
}                               /* cache_inode_setattr */
