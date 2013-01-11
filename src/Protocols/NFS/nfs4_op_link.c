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
 * \file    nfs4_op_link.c
 * \author  $Author: deniel $
 * \date    $Date: 2005/11/28 17:02:50 $
 * \version $Revision: 1.11 $
 * \brief   Routines used for managing the NFS4 COMPOUND functions.
 *
 * nfs4_op_link.c : Routines used for managing the NFS4 COMPOUND functions.
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

/**
 * nfs4_op_link: The NFS4_OP_LINK operation.
 * 
 * This functions handles the NFS4_OP_LINK operation in NFSv4. This function can be called only from nfs4_Compound.
 *
 * @param op    [IN]    pointer to nfs4_op arguments
 * @param data  [INOUT] Pointer to the compound request's data
 * @param resp  [IN]    Pointer to nfs4_op results
 *
 * @return NFS4_OK if successfull, other values show an error.  
 * 
 */

#define arg_LINK4 op->nfs_argop4_u.oplink
#define res_LINK4 resp->nfs_resop4_u.oplink

int nfs4_op_link(struct nfs_argop4 *op, compound_data_t * data, struct nfs_resop4 *resp)
{
  char __attribute__ ((__unused__)) funcname[] = "nfs4_op_link";

  cache_entry_t        * dir_pentry = NULL;
  cache_entry_t        * file_pentry = NULL;
  cache_inode_status_t   cache_status;
  fsal_attrib_list_t     attr;
  fsal_name_t            newname;

  resp->resop = NFS4_OP_LINK;
  res_LINK4.status = NFS4_OK;

  /* Do basic checks on a filehandle */
  res_LINK4.status = nfs4_sanity_check_FH(data, 0LL);
  if(res_LINK4.status != NFS4_OK)
    return res_LINK4.status;

  /* Do basic checks on saved filehandle */

  /* If there is no FH */
  if(nfs4_Is_Fh_Empty(&(data->savedFH)))
    {
      LogDebug(COMPONENT_NFS_V4,
               "No saved file handle");
      res_LINK4.status = NFS4ERR_NOFILEHANDLE;
      return res_LINK4.status;
    }

  /* If the filehandle is invalid */
  if(nfs4_Is_Fh_Invalid(&(data->savedFH)))
    {
      res_LINK4.status = NFS4ERR_BADHANDLE;
      return res_LINK4.status;
    }

  /* Tests if the Filehandle is expired (for volatile filehandle) */
  if(nfs4_Is_Fh_Expired(&(data->savedFH)))
    {
      res_LINK4.status = NFS4ERR_FHEXPIRED;
      return res_LINK4.status;
    }

  /* Pseudo Fs is explictely a Read-Only File system */
  if(nfs4_Is_Fh_Pseudo(&(data->savedFH)))
    {
      res_LINK4.status = NFS4ERR_ROFS;
      return res_LINK4.status;
    }

  /*
   * This operation creates a hard link, for the file represented by the saved FH, in directory represented by currentFH under the 
   * name arg_LINK4.target 
   */

  /* Crossing device is not allowed */
  if(((file_handle_v4_t *) (data->currentFH.nfs_fh4_val))->exportid !=
     ((file_handle_v4_t *) (data->savedFH.nfs_fh4_val))->exportid)
    {
      res_LINK4.status = NFS4ERR_XDEV;
      return res_LINK4.status;
    }

  /* Convert the UFT8 objname to a regular string */
  cache_status = utf8_to_name(&arg_LINK4.newname, &newname);

  if(cache_status != CACHE_INODE_SUCCESS)
    {
      res_LINK4.status = nfs4_Errno(cache_status);
      return res_LINK4.status;
    }

  /* Sanity check: never create a link named '.' or '..' */
  if(!FSAL_namecmp(&newname, (fsal_name_t *) & FSAL_DOT)
     || !FSAL_namecmp(&newname, (fsal_name_t *) & FSAL_DOT_DOT))
    {
      res_LINK4.status = NFS4ERR_BADNAME;
      return res_LINK4.status;
    }

  /* get info from compound data */
  dir_pentry = data->current_entry;

  /* Destination FH (the currentFH) must be a directory */
  if(data->current_filetype != DIRECTORY)
    {
      res_LINK4.status = NFS4ERR_NOTDIR;
      return res_LINK4.status;
    }

  /* Target object (the savedFH) must not be a directory */
  if(data->saved_filetype == DIRECTORY)
    {
      res_LINK4.status = NFS4ERR_ISDIR;
      return res_LINK4.status;
    }

  /* We have to keep track of the 'change' file attribute for reply structure */
  if((cache_status = cache_inode_getattr(dir_pentry,
                                         &attr,
                                         data->pcontext,
                                         &cache_status)) != CACHE_INODE_SUCCESS)
    {
      res_LINK4.status = nfs4_Errno(cache_status);
      return res_LINK4.status;
    }
  res_LINK4.LINK4res_u.resok4.cinfo.before
       = cache_inode_get_changeid4(dir_pentry);

  /* Convert savedFH into a vnode */
  file_pentry = data->saved_entry;

  /* make the link */
  if(cache_inode_link(file_pentry,
                      dir_pentry,
                      &newname,
                      &attr,
                      data->pcontext, &cache_status) != CACHE_INODE_SUCCESS)
    {
      res_LINK4.status = nfs4_Errno(cache_status);
      return res_LINK4.status;
    }

  res_LINK4.LINK4res_u.resok4.cinfo.after
       = cache_inode_get_changeid4(dir_pentry);
  res_LINK4.LINK4res_u.resok4.cinfo.atomic = FALSE;

  res_LINK4.status = NFS4_OK;
  return NFS4_OK;
}                               /* nfs4_op_link */

/**
 * nfs4_op_link_Free: frees what was allocared to handle nfs4_op_link.
 * 
 * Frees what was allocared to handle nfs4_op_link.
 *
 * @param resp  [INOUT]    Pointer to nfs4_op results
 *
 * @return nothing (void function )
 *
 */
void nfs4_op_link_Free(LINK4res * resp)
{
  return;
}                               /* nfs4_op_link_Free */
