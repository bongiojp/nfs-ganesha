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
 * @file    nfs4_op_savefh.c
 * @brief   Routines used for managing the NFS4_OP_SAVEFH operation.
 *
 * Routines used for managing the NFS4_OP_SAVEFH operation.
 */

#include "config.h"
#include "log.h"
#include "ganesha_rpc.h"
#include "nfs4.h"
#include "nfs_core.h"
#include "cache_inode.h"
#include "cache_inode_lru.h"
#include "nfs_exports.h"
#include "nfs_proto_functions.h"
#include "nfs_proto_tools.h"
#include "nfs_tools.h"
#include "nfs_file_handle.h"
#include "export_mgr.h"

/**
 *
 * @brief the NFS4_OP_SAVEFH operation
 *
 * This functions handles the NFS4_OP_SAVEFH operation in NFSv4. This
 * function can be called only from nfs4_Compound.  The operation set
 * the savedFH with the value of the currentFH.
 *
 * @param[in]     op   Arguments for nfs4_op
 * @param[in,out] data Compound request's data
 * @param[out]    resp Results for nfs4_op
 *
 * @return per RFC5661, p. 373
 *
 * @see nfs4_Compound
 *
 */

#define arg_SAVEFH op->nfs_argop4_u.opsavefh
#define res_SAVEFH resp->nfs_resop4_u.opsavefh

int nfs4_op_savefh(struct nfs_argop4 *op,
                   compound_data_t *data,
                   struct nfs_resop4 *resp)
{
  /* First of all, set the reply to zero to make sure it contains no
     parasite information */
  memset(resp, 0, sizeof(struct nfs_resop4));
  resp->resop = NFS4_OP_SAVEFH;
  res_SAVEFH.status = NFS4_OK;

  /* Do basic checks on a filehandle */
  res_SAVEFH.status = nfs4_sanity_check_FH(data, NO_FILE_TYPE, true);
  if(res_SAVEFH.status != NFS4_OK)
    return res_SAVEFH.status;

  if(data->current_entry == NULL) {
    LogFullDebug(COMPONENT_NFS_V4, "ccccccccccccccccccccccc");
  } else {
    LogFullDebug(COMPONENT_NFS_V4, "ddddddddddddddddddddddd");
  }

  /* If the savefh is not allocated, do it now */
  if(data->savedFH.nfs_fh4_len == 0)
    {
      res_SAVEFH.status = nfs4_AllocateFH(&(data->savedFH));
      if(res_SAVEFH.status != NFS4_OK)
        return res_SAVEFH.status;
    }
  if(data->current_entry == NULL) {LogFullDebug(COMPONENT_NFS_V4, "eeeeeeeeeee");}
  /* Copy the data from current FH to saved FH */
  memcpy(data->savedFH.nfs_fh4_val,
         data->currentFH.nfs_fh4_val,
         data->currentFH.nfs_fh4_len);
  data->savedFH.nfs_fh4_len = data->currentFH.nfs_fh4_len;
  if(data->current_entry == NULL) {LogFullDebug(COMPONENT_NFS_V4, "fffffffffffffff");}
  if(data->saved_export != NULL) {
      put_gsh_export(data->saved_export);
  }
  /* Save the export information by taking a reference since
   * currentFH is still active.  Assert this just to be sure...
   */
  if (data->req_ctx->export != NULL)
    data->saved_export = get_gsh_export(data->req_ctx->export->export.id, true);

  assert((data->saved_export != NULL) || nfs4_Is_Fh_Pseudo(&data->currentFH) );
  data->saved_export_perms = data->export_perms;
  if(data->current_entry == NULL) {LogFullDebug(COMPONENT_NFS_V4, "ggggggggggggggggggggg");}
  /* If saved and current entry are equal, skip the following. */

  if (data->saved_entry == data->current_entry) {
      goto out;
  }
  if(data->current_entry == NULL) {LogFullDebug(COMPONENT_NFS_V4, "hhhhhhhhhhhhhhhhhhhhhh");}
  if (data->saved_entry) {
      cache_inode_put(data->saved_entry);
      data->saved_entry = NULL;
  }
  if(data->current_entry == NULL) {LogFullDebug(COMPONENT_NFS_V4, "iiiiiiiiiiiiiiii");}
  if (data->saved_ds) {
      data->saved_ds->ops->put(data->saved_ds);
      data->saved_ds = NULL;
  }
  if(data->current_entry == NULL) {LogFullDebug(COMPONENT_NFS_V4, "jjjjjjjjjjjjjjjj");}
  data->saved_entry = data->current_entry;
  data->saved_filetype = data->current_filetype;

  /* Take another reference.  As of now the filehandle is both saved
     and current and both must be counted.  Guard this, in case we
     have a pseudofs handle. */

  if (data->saved_entry)
      cache_inode_lru_ref(data->saved_entry, LRU_FLAG_NONE);
  if(data->current_entry == NULL) {LogFullDebug(COMPONENT_NFS_V4, "kkkkkkkkkkkkkkkkkkkk");}
 out:

  if(isFullDebug(COMPONENT_NFS_V4))
    {
      char str[LEN_FH_STR];
      sprint_fhandle4(str, &data->savedFH);
      LogFullDebug(COMPONENT_NFS_V4, "SAVE FH: Saved FH %s", str);
    }
  if(data->current_entry == NULL) {LogFullDebug(COMPONENT_NFS_V4, "lllllllllllllllll");}
  return NFS4_OK;
} /* nfs4_op_savefh */

/**
 * @brief Free memory allocated for SAVEFH result
 *
 * This function frees any memory allocated for the result of the
 * NFS4_OP_SAVEFH function.
 *
 * @param[in,out] resp nfs4_op results
 */
void nfs4_op_savefh_Free(nfs_resop4 *resp)
{
  /* Nothing to be done */
  return;
} /* nfs4_op_savefh_Free */
