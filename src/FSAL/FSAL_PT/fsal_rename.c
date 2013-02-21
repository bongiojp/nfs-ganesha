// ----------------------------------------------------------------------------
// Copyright IBM Corp. 2012, 2012
// All Rights Reserved
// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------
// Filename:    fsal_rename.c
// Description: Common FSI IPC Client and Server definitions
// Author:      FSI IPC dev team
// ----------------------------------------------------------------------------

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
 * -------------
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "fsal.h"
#include "fsal_internal.h"
#include "fsal_convert.h"
#include "pt_ganesha.h"

/**
 * FSAL_rename:
 * Change name and/or parent dir of a filesystem object.
 *
 * \param old_parentdir_handle (input):
 *        Source parent directory of the object is to be moved/renamed.
 * \param p_old_name (input):
 *        Pointer to the current name of the object to be moved/renamed.
 * \param new_parentdir_handle (input):
 *        Target parent directory for the object.
 * \param p_new_name (input):
 *        Pointer to the new name for the object.
 * \param cred (input):
 *        Authentication context for the operation (user,...).
 * \param src_dir_attributes (optionnal input/output):
 *        Post operation attributes for the source directory.
 *        As input, it defines the attributes that the caller
 *        wants to retrieve (by positioning flags into this structure)
 *        and the output is built considering this input
 *        (it fills the structure according to the flags it contains).
 *        May be NULL.
 * \param tgt_dir_attributes (optionnal input/output):
 *        Post operation attributes for the target directory.
 *        As input, it defines the attributes that the caller
 *        wants to retrieve (by positioning flags into this structure)
 *        and the output is built considering this input
 *        (it fills the structure according to the flags it contains).
 *        May be NULL.
 *
 * \return Major error codes :
 *        - ERR_FSAL_NO_ERROR     (no error)
 *        - Another error code if an error occured.
 */

fsal_status_t PTFSAL_rename(fsal_handle_t * p_old_parentdir_handle,       /* IN */
                          fsal_name_t * p_old_name,     /* IN */
                          fsal_handle_t * p_new_parentdir_handle,       /* IN */
                          fsal_name_t * p_new_name,     /* IN */
                          fsal_op_context_t * p_context,        /* IN */
                          fsal_attrib_list_t * p_src_dir_attributes,    /* [ IN/OUT ] */
                          fsal_attrib_list_t * p_tgt_dir_attributes     /* [ IN/OUT ] */)
{

  int rc, errsv;
  fsal_status_t status;
  fsi_stat_struct old_bufstat, new_bufstat;
  int src_equal_tgt = FALSE;
  fsal_accessflags_t access_mask = 0;
  fsal_attrib_list_t src_dir_attrs, tgt_dir_attrs;
  int stat_rc;

  FSI_TRACE(FSI_DEBUG, "FSI Rename--------------\n");

  /* sanity checks.
   * note : src/tgt_dir_attributes are optional.
   */
  if(!p_old_parentdir_handle || !p_new_parentdir_handle
     || !p_old_name || !p_new_name || !p_context)
    Return(ERR_FSAL_FAULT, 0, INDEX_FSAL_rename);

  /* Get directory access path by fid */

  // TakeTokenFSCall();
  // status = fsal_internal_handle2fd(p_context, p_old_parentdir_handle,
  //                                 &old_parent_fd,
  //                                 O_RDONLY | O_DIRECTORY);
  // FSI_TRACE(FSI_DEBUG, "older parent dir fd = %d", old_parent_fd);
  // ReleaseTokenFSCall();

  // if(FSAL_IS_ERROR(status))
  //  ReturnStatus(status, INDEX_FSAL_rename);

  /* retrieve directory metadata for checking access rights */

  src_dir_attrs.asked_attributes = PTFS_SUPPORTED_ATTRIBUTES;
  status = PTFSAL_getattrs(p_old_parentdir_handle, p_context, &src_dir_attrs);
  if(FSAL_IS_ERROR(status))
    ReturnStatus(status, INDEX_FSAL_rename);

  /* optimisation : don't do the job twice if source dir = dest dir  */
  if(!FSAL_handlecmp(p_old_parentdir_handle, p_new_parentdir_handle, &status))
    {
      // new_parent_fd = old_parent_fd;
      src_equal_tgt = TRUE;
      tgt_dir_attrs = src_dir_attrs;
    }
  else
    {
      // TakeTokenFSCall();
      // status = fsal_internal_handle2fd(p_context, p_new_parentdir_handle,
      //                                 &new_parent_fd,
      //                                 O_RDONLY | O_DIRECTORY);
      // ReleaseTokenFSCall();

      // if(FSAL_IS_ERROR(status))
      //  {
      //    ReturnStatus(status, INDEX_FSAL_rename);
      //  }

      /* retrieve destination attrs */
      tgt_dir_attrs.asked_attributes = PTFS_SUPPORTED_ATTRIBUTES;
      status = PTFSAL_getattrs(p_new_parentdir_handle, p_context, &tgt_dir_attrs);
      if(FSAL_IS_ERROR(status))
        ReturnStatus(status, INDEX_FSAL_rename);
    }

  /* check access rights */

  /* Set both mode and ace4 mask */
  access_mask = FSAL_MODE_MASK_SET(FSAL_W_OK | FSAL_X_OK) |
                FSAL_ACE4_MASK_SET(FSAL_ACE_PERM_DELETE_CHILD);

  if(!p_context->export_context->fe_static_fs_info->accesscheck_support)
    status = fsal_internal_testAccess(p_context, access_mask, NULL, &src_dir_attrs);
  else
    status = fsal_internal_access(p_context, p_old_parentdir_handle, access_mask,
                                  &src_dir_attrs);
  if(FSAL_IS_ERROR(status)) {
    ReturnStatus(status, INDEX_FSAL_rename);
  }

  if(!src_equal_tgt)
    {
      access_mask = FSAL_MODE_MASK_SET(FSAL_W_OK | FSAL_X_OK) |
                FSAL_ACE4_MASK_SET(FSAL_ACE_PERM_ADD_FILE |
                                   FSAL_ACE_PERM_ADD_SUBDIRECTORY);

	  if(!p_context->export_context->fe_static_fs_info->accesscheck_support)
            status = fsal_internal_testAccess(p_context, access_mask, NULL, &tgt_dir_attrs);
	  else
	    status = fsal_internal_access(p_context, p_new_parentdir_handle, access_mask,
	                                  &tgt_dir_attrs);
      if(FSAL_IS_ERROR(status)) {
        ReturnStatus(status, INDEX_FSAL_rename);
      }
    }

  /* build file paths */
  TakeTokenFSCall();
  stat_rc = ptfsal_stat_by_parent_name(p_context, p_old_parentdir_handle, p_old_name->name, &old_bufstat); 
  errsv = errno;
  ReleaseTokenFSCall();
  if(stat_rc) {
    Return(posix2fsal_error(errsv), errsv, INDEX_FSAL_rename);
  }

  /* Check sticky bits */

  /* Sticky bit on the source directory => the user who wants to delete the file must own it or its parent dir */
  if((fsal2unix_mode(src_dir_attrs.mode) & S_ISVTX) &&
    src_dir_attrs.owner != p_context->credential.user &&
    old_bufstat.st_uid != p_context->credential.user && p_context->credential.user != 0) {
    Return(ERR_FSAL_ACCESS, 0, INDEX_FSAL_rename);
  }

  /* Sticky bit on the target directory => the user who wants to create the file must own it or its parent dir */
  if(fsal2unix_mode(tgt_dir_attrs.mode) & S_ISVTX)
    {
      TakeTokenFSCall();
      stat_rc = ptfsal_stat_by_parent_name(p_context, p_new_parentdir_handle, p_new_name->name, &new_bufstat);
      errsv = errno;
      ReleaseTokenFSCall();

      if(stat_rc < 0)
        {
          if(errsv != ENOENT)
            {
              Return(posix2fsal_error(errsv), errsv, INDEX_FSAL_rename);
            }
        }
      else
        {

          if(tgt_dir_attrs.owner != p_context->credential.user
             && new_bufstat.st_uid != p_context->credential.user
             && p_context->credential.user != 0)
            {
              Return(ERR_FSAL_ACCESS, 0, INDEX_FSAL_rename);
            }
        }
    }

  /*************************************
   * Rename the file on the filesystem *
   *************************************/
  TakeTokenFSCall();
  rc = ptfsal_rename(p_context, p_old_parentdir_handle, p_old_name->name, p_new_parentdir_handle, p_new_name->name);  
  errsv = errno;
  ReleaseTokenFSCall();

  if(rc)
    Return(posix2fsal_error(errsv), errsv, INDEX_FSAL_rename);

  /***********************
   * Fill the attributes *
   ***********************/

  if(p_src_dir_attributes)
    {

      status = PTFSAL_getattrs(p_old_parentdir_handle, p_context, p_src_dir_attributes);

      if(FSAL_IS_ERROR(status))
        {
          FSAL_CLEAR_MASK(p_src_dir_attributes->asked_attributes);
          FSAL_SET_MASK(p_src_dir_attributes->asked_attributes, FSAL_ATTR_RDATTR_ERR);
        }

    }

  if(p_tgt_dir_attributes)
    {

      status = PTFSAL_getattrs(p_new_parentdir_handle, p_context, p_tgt_dir_attributes);

      if(FSAL_IS_ERROR(status))
        {
          FSAL_CLEAR_MASK(p_tgt_dir_attributes->asked_attributes);
          FSAL_SET_MASK(p_tgt_dir_attributes->asked_attributes, FSAL_ATTR_RDATTR_ERR);
        }

    }

  /* OK */
  Return(ERR_FSAL_NO_ERROR, 0, INDEX_FSAL_rename);

}
