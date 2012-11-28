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
 * \file    nfs4_op_setattr.c
 * \author  $Author: deniel $
 * \date    $Date: 2005/11/28 17:02:52 $
 * \version $Revision: 1.15 $
 * \brief   Routines used for managing the NFS4 COMPOUND functions.
 *
 * nfs4_op_setattr.c : Routines used for managing the NFS4 COMPOUND functions.
 *
 *
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef _SOLARIS
#include "solaris_port.h"
#endif

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/file.h>           /* for having FNDELAY */
#include "HashData.h"
#include "HashTable.h"
#include "log.h"
#include "ganesha_rpc.h"
#include "nfs23.h"
#include "nfs4.h"
#include "mount.h"
#include "nfs_core.h"
#include "cache_inode.h"
#include "nfs_exports.h"
#include "nfs_creds.h"
#include "nfs_proto_functions.h"
#include "nfs_proto_tools.h"
#include "nfs_tools.h"
#include "nfs_file_handle.h"
#include "sal_functions.h"

/**
 * nfs4_op_setattr: The NFS4_OP_SETATTR operation.
 * 
 * This functions handles the NFS4_OP_SETATTR operation in NFSv4. This function can be called only from nfs4_Compound
 *
 * @param op    [IN]    pointer to nfs4_op arguments
 * @param data  [INOUT] Pointer to the compound request's data
 * @param resp  [IN]    Pointer to nfs4_op results
 *
 * @return NFS4_OK if successfull, other values show an error.  
 * 
 */

#define arg_SETATTR4 op->nfs_argop4_u.opsetattr
#define res_SETATTR4 resp->nfs_resop4_u.opsetattr

int nfs4_op_setattr(struct nfs_argop4 *op,
                    compound_data_t * data, struct nfs_resop4 *resp)
{
  struct timeval         t;
  fsal_attrib_list_t     sattr;
  fsal_attrib_list_t     parent_attr;
  cache_inode_status_t   cache_status = CACHE_INODE_SUCCESS;
  const char           * tag = "SETATTR";
  state_t              * pstate_found = NULL;
  state_t              * pstate_open  = NULL;
  cache_entry_t        * pentry       = NULL;

  memset(&sattr, 0, sizeof(sattr));
  memset(&parent_attr, 0, sizeof(parent_attr));
  resp->resop = NFS4_OP_SETATTR;
  res_SETATTR4.status = NFS4_OK;

  /* Do basic checks on a filehandle */
  res_SETATTR4.status = nfs4_sanity_check_FH(data,0LL);
  if(res_SETATTR4.status != NFS4_OK)
    return res_SETATTR4.status;

  /* Get only attributes that are allowed to be read */
  if(!nfs4_Fattr_Check_Access(&arg_SETATTR4.obj_attributes, FATTR4_ATTR_WRITE))
    {
      res_SETATTR4.status = NFS4ERR_INVAL;
      return res_SETATTR4.status;
    }

  /* Ask only for supported attributes */
  if(!nfs4_Fattr_Supported(&arg_SETATTR4.obj_attributes))
    {
      res_SETATTR4.status = NFS4ERR_ATTRNOTSUPP;
      return res_SETATTR4.status;
    }

  /* Convert the fattr4 in the request to a FSAL attr structure */
  res_SETATTR4.status = nfs4_Fattr_To_FSAL_attr(&sattr,
                                                &(arg_SETATTR4.obj_attributes),
                                                data->export_perms.anonymous_uid,
                                                data->export_perms.anonymous_gid);
  if(res_SETATTR4.status != NFS4_OK)
    return res_SETATTR4.status;

  /*
   * trunc may change Xtime so we have to start with trunc and finish
   * by the mtime and atime 
   */
  if(FSAL_TEST_MASK(sattr.asked_attributes, FSAL_ATTR_SIZE))
    {
      /* Setting the size of a directory is prohibited */
      if(data->current_filetype == DIRECTORY)
        {
          res_SETATTR4.status = NFS4ERR_ISDIR;
          return res_SETATTR4.status;
        }
      /* Object should be a file */
      if(data->current_entry->type != REGULAR_FILE)
        {
          res_SETATTR4.status = NFS4ERR_INVAL;
          return res_SETATTR4.status;
        }

      /* vnode to manage is the current one */
      pentry = data->current_entry;

      /* Check stateid correctness and get pointer to state */
      res_SETATTR4.status = nfs4_Check_Stateid(&arg_SETATTR4.stateid,
                                               data->current_entry,
                                               &pstate_found,
                                               data,
                                               STATEID_SPECIAL_ANY,
                                               0,FALSE,                  /* do not check owner seqid */
                                               tag);
      if(res_SETATTR4.status != NFS4_OK)
        return res_SETATTR4.status;

      /* NB: After this points, if pstate_found == NULL, then the stateid is all-0 or all-1 */
      if(pstate_found != NULL)
        {
          switch(pstate_found->state_type)
            {
              case STATE_TYPE_SHARE:
                pstate_open = pstate_found;
                break;

              case STATE_TYPE_LOCK:
                pstate_open = pstate_found->state_data.lock.popenstate;
                break;

              case STATE_TYPE_DELEG:
                pstate_open = NULL;
                break;

              default:
                res_SETATTR4.status = NFS4ERR_BAD_STATEID;
                return res_SETATTR4.status;
            }

          /* This is a size operation, this means that the file MUST have been opened for writing */
          if(pstate_open != NULL &&
             (pstate_open->state_data.share.share_access & OPEN4_SHARE_ACCESS_WRITE) == 0)
            {
              /* Bad open mode, return NFS4ERR_OPENMODE */
              res_SETATTR4.status = NFS4ERR_OPENMODE;
              return res_SETATTR4.status;
            }
        }
      else
        {
          /* Special stateid, no open state, check to see if any share conflicts */
          pstate_open = NULL;

          /*
           * Special stateid, no open state, check to see if any share conflicts
           * The stateid is all-0 or all-1
           */
          res_SETATTR4.status = nfs4_check_special_stateid(pentry,
                                                           "SETATTR(size)",
                                                           FATTR4_ATTR_WRITE);
          if(res_SETATTR4.status != NFS4_OK)
            return res_SETATTR4.status;
        }
    }

  /* Now, we set the mode */
  if(FSAL_TEST_MASK(sattr.asked_attributes, FSAL_ATTR_MODE) ||
     FSAL_TEST_MASK(sattr.asked_attributes, FSAL_ATTR_OWNER) ||
     FSAL_TEST_MASK(sattr.asked_attributes, FSAL_ATTR_GROUP) ||
     FSAL_TEST_MASK(sattr.asked_attributes, FSAL_ATTR_SIZE)  ||
     FSAL_TEST_MASK(sattr.asked_attributes, FSAL_ATTR_MTIME) ||
#ifdef _USE_NFS4_ACL
     FSAL_TEST_MASK(sattr.asked_attributes, FSAL_ATTR_ATIME) ||
     FSAL_TEST_MASK(sattr.asked_attributes, FSAL_ATTR_ACL))
#else
     FSAL_TEST_MASK(sattr.asked_attributes, FSAL_ATTR_ATIME))
#endif
    {
      /* Check for NOSUID/NOSGID when using chmod */
      if(FSAL_TEST_MASK(sattr.asked_attributes, FSAL_ATTR_MODE))
        {
          if(((sattr.mode & FSAL_MODE_SUID) &&
              ((data->pexport->export_perms.options & EXPORT_OPTION_NOSUID) == EXPORT_OPTION_NOSUID))
             || ((sattr.mode & FSAL_MODE_SGID)
                 && ((data->pexport->export_perms.options & EXPORT_OPTION_NOSGID) ==
                     EXPORT_OPTION_NOSGID)))
            {
              LogInfo(COMPONENT_NFS_V4,
                      "Setattr denied because setuid or setgid bit is disabled in configuration file. setuid=%d, setgid=%d",
                      sattr.mode & FSAL_MODE_SUID ? 1 : 0,
                      sattr.mode & FSAL_MODE_SGID ? 1 : 0);
              res_SETATTR4.status = NFS4ERR_PERM;
              return res_SETATTR4.status;
            }
        }

#define S_NSECS 1000000000UL  /* nsecs in 1s */
      /* Set the atime and mtime (ctime is not setable) */

      /* get the current time */
       gettimeofday(&t, NULL);

      /** @todo : check correctness of this block... looks suspicious */
      if(FSAL_TEST_MASK(sattr.asked_attributes, FSAL_ATTR_ATIME) == SET_TO_SERVER_TIME4)
        {
          sattr.atime.seconds = t.tv_sec;
          sattr.atime.nseconds = t.tv_usec;
        }
      else
        {
          /* a carry into seconds considered invalid */
          if (sattr.atime.nseconds >= S_NSECS)
          {
            res_SETATTR4.status = NFS4ERR_INVAL;
            return res_SETATTR4.status;
          }
        }

      /* Should we use the time from the client handside or from the server handside ? */
      /** @todo : check correctness of this block... looks suspicious */
      if(FSAL_TEST_MASK(sattr.asked_attributes, FSAL_ATTR_MTIME) == SET_TO_SERVER_TIME4)
        {
          sattr.mtime.seconds = t.tv_sec;
          sattr.mtime.nseconds = t.tv_usec;
        }
      else
        {
          /* a carry into seconds considered invalid */
          if (sattr.mtime.nseconds >= S_NSECS)
          {
            res_SETATTR4.status = NFS4ERR_INVAL;
            return res_SETATTR4.status;
          }
        }

      if(cache_inode_setattr(data->current_entry,
                             &sattr,
                             data->pcontext,
                             pstate_open != NULL,
                             &cache_status) != CACHE_INODE_SUCCESS)
        {
          res_SETATTR4.status = nfs4_Errno(cache_status);
          return res_SETATTR4.status;
        }
    }

  /* Set the replyed structure */
  res_SETATTR4.attrsset.bitmap4_len = arg_SETATTR4.obj_attributes.attrmask.bitmap4_len;

  if((res_SETATTR4.attrsset.bitmap4_val =
      gsh_calloc(res_SETATTR4.attrsset.bitmap4_len, sizeof(uint32_t))) == NULL)
    {
      res_SETATTR4.status = NFS4ERR_SERVERFAULT;
      LogEvent(COMPONENT_NFS_V4,
               "FAILED to allocate bitmap");
      return res_SETATTR4.status;
    }

  memcpy(res_SETATTR4.attrsset.bitmap4_val,
         arg_SETATTR4.obj_attributes.attrmask.bitmap4_val,
         res_SETATTR4.attrsset.bitmap4_len * sizeof(u_int));

  /* Exit with no error */
  res_SETATTR4.status = NFS4_OK;

  return res_SETATTR4.status;
}                               /* nfs4_op_setattr */

/**
 * nfs4_op_setattr_Free: frees what was allocated to handle nfs4_op_setattr.
 *
 * Frees what was allocared to handle nfs4_op_setattr.
 *
 * @param resp  [INOUT]    Pointer to nfs4_op results
 *
 * @return nothing (void function )
 *
 */
void nfs4_op_setattr_Free(SETATTR4res * resp)
{
  if(resp->status == NFS4_OK)
    gsh_free(resp->attrsset.bitmap4_val);
  return;
}                               /* nfs4_op_setattr_Free */
